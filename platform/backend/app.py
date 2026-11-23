#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Backend Local da Plataforma de Instrumentação 4-QAM
===================================================
Servidor FastAPI + WebSocket + pyserial:
  - Comunica simultaneamente com ESP32 TX e RX via portas COM exclusivas.
  - Processa amostras ADC em buffer circular, calcula FFT real e Ground Truth.
  - Gerencia sessões locais em JSON/CSV.
  - Serve a interface web estática em C:\\Modulador_ESP\\platform\\frontend.
"""

import asyncio
import json
import os
import threading
import time
from typing import Any, Dict, List, Optional, Set

from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

import dsp_analysis as dsp
import offline_sim
import serial_manager as sm
import serial_protocol as proto
import session_store as store

FRONTEND_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "frontend"))

MAX_ADC_STREAM_BUFFER = 8192
MAX_DSP_SYMBOL_HISTORY = 2000


class TelemetryHub:
    """Estado central reativo da sessão de bancada (nunca inventa dados)."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.acquisition_enabled: bool = True
        self.data_source: str = "awaiting_hardware"  # awaiting_hardware | live_hardware | offline_test
        self.is_offline_test: bool = False

        self.tx_hello: Optional[Dict[str, Any]] = None
        self.rx_hello: Optional[Dict[str, Any]] = None

        self.tx_frames: List[Dict[str, Any]] = []
        self.rx_frames: List[Dict[str, Any]] = []
        self.sync_events: List[Dict[str, Any]] = []
        self.cli_logs: List[Dict[str, Any]] = []
        self.ground_truth_history: List[Dict[str, Any]] = []
        self.last_ground_truth: Optional[Dict[str, Any]] = None

        # Buffers de formas de onda:
        self.tx_pwm_ref: Optional[Dict[str, Any]] = None
        self.tx_theoretical_wave: Optional[Dict[str, Any]] = None
        self.adc_stream_mv: List[int] = []
        self.adc_stream_decim: int = 10
        self.adc_stream_fs_hz: int = 20000
        self.adc_snapshot: Optional[Dict[str, Any]] = None

        # Buffer temporário por snapshot recebido em chunks
        self._snap_acc: Dict[int, Dict[str, Any]] = {}
        self._pwm_acc: Dict[int, Dict[str, Any]] = {}

        # Histórico DSP por símbolo
        self.dsp_symbols: List[Dict[str, Any]] = []
        self.current_burst_symbols: List[Dict[str, Any]] = []

        self.dirty_version: int = 0
        self.serial_mgr = sm.DualSerialManager(
            on_event=self.on_serial_packet,
            on_cli=self.on_cli_line,
            on_state_change=self.on_conn_change,
        )

    def reset_live_plots(self, clear_offline_flag: bool = True) -> None:
        with self._lock:
            self.tx_frames.clear()
            self.rx_frames.clear()
            self.sync_events.clear()
            self.ground_truth_history.clear()
            self.last_ground_truth = None
            self.tx_pwm_ref = None
            self.tx_theoretical_wave = None
            self.adc_stream_mv.clear()
            self.adc_snapshot = None
            self._snap_acc.clear()
            self._pwm_acc.clear()
            self.dsp_symbols.clear()
            self.current_burst_symbols.clear()
            if clear_offline_flag:
                self.is_offline_test = False
                st = self.serial_mgr.status()
                if st["tx"]["status"] != "connected":
                    self.tx_hello = None
                if st["rx"]["status"] != "connected":
                    self.rx_hello = None
                if st["tx"]["status"] == "connected" or st["rx"]["status"] == "connected":
                    self.data_source = "live_hardware"
                else:
                    self.data_source = "awaiting_hardware"
            self.dirty_version += 1

    def on_conn_change(self, role: str, status_dict: Dict[str, Any]) -> None:
        with self._lock:
            if status_dict.get("status") == "connected":
                if not self.is_offline_test:
                    self.data_source = "live_hardware"
            else:
                if role == "tx":
                    self.tx_hello = None
                else:
                    self.rx_hello = None
                other = self.serial_mgr.rx if role == "tx" else self.serial_mgr.tx
                if other.status != "connected" and not self.is_offline_test:
                    self.data_source = "awaiting_hardware"
            self.dirty_version += 1

    def on_cli_line(self, role: str, line: str) -> None:
        with self._lock:
            self.cli_logs.append({
                "ts": time.time(),
                "role": role,
                "line": line,
            })
            if len(self.cli_logs) > 250:
                self.cli_logs.pop(0)
            self.dirty_version += 1

    def on_serial_packet(self, role: str, pkt: Dict[str, Any], offline: bool = False) -> None:
        if not self.acquisition_enabled and not offline:
            return

        with self._lock:
            if offline:
                self.is_offline_test = True
                self.data_source = "offline_test"
            elif self.is_offline_test:
                # Hardware real assumiu: limpa dados simulados offline antes de registrar dados reais!
                self.is_offline_test = False
                self.data_source = "live_hardware"
                self.tx_frames.clear()
                self.rx_frames.clear()
                self.sync_events.clear()
                self.ground_truth_history.clear()
                self.last_ground_truth = None
                self.tx_pwm_ref = None
                self.tx_theoretical_wave = None
                self.adc_stream_mv.clear()
                self.adc_snapshot = None
                self._snap_acc.clear()
                self._pwm_acc.clear()
                self.dsp_symbols.clear()
                self.current_burst_symbols.clear()

            ev = pkt.get("event")
            if ev == "tx_hello":
                self.tx_hello = pkt
            elif ev == "rx_hello":
                self.rx_hello = pkt
            elif ev == "tx_frame":
                self._ingest_tx_frame(pkt, offline=offline)
            elif ev == "tx_pwm_chunk":
                self._ingest_tx_pwm(pkt)
            elif ev == "rx_burst":
                if pkt.get("burst_kind") == "start":
                    self.current_burst_symbols = []
                self.sync_events.append({
                    "ts": time.time(),
                    "is_offline_test": offline,
                    **pkt,
                })
                if len(self.sync_events) > 200:
                    self.sync_events.pop(0)
            elif ev == "rx_symbols":
                syms = pkt.get("symbols", [])
                for s in syms:
                    s["burst_id"] = pkt.get("burst_id")
                    self.dsp_symbols.append(s)
                    self.current_burst_symbols.append(s)
                if len(self.dsp_symbols) > MAX_DSP_SYMBOL_HISTORY:
                    self.dsp_symbols = self.dsp_symbols[-MAX_DSP_SYMBOL_HISTORY:]
            elif ev == "rx_frame":
                self._ingest_rx_frame(pkt, offline)
            elif ev == "rx_adc_chunk":
                self._ingest_rx_adc(pkt, offline)

            self.dirty_version += 1

    def _ingest_tx_frame(self, pkt: Dict[str, Any], offline: bool = False) -> None:
        fid = pkt["frame_id"]
        # Atualiza quadro existente apenas se pertencer à mesma origem/carga e não tiver sido concluído em ciclo anterior
        existing = next(
            (
                f for f in reversed(self.tx_frames)
                if f["frame_id"] == fid
                and bool(f.get("is_offline_test", False)) == offline
                and f.get("payload_bytes") == pkt.get("payload_bytes")
                and not (f.get("status") == "done" and pkt.get("status") != "done")
            ),
            None,
        )
        if existing:
            existing["status"] = pkt["status"]
            existing["ts_us"] = pkt["ts_us"]
            return

        chain = dsp.build_expected_tx_chain(
            payload_bytes=pkt["payload_bytes"],
            tx_mode=pkt["tx_mode"],
            map_preset=pkt["map_preset"],
            bit_order=pkt["bit_order"],
            q_sign_minus=pkt["q_sign_minus"],
            baud=pkt["symbol_rate_baud"],
        )
        rec = {**pkt, "chain": chain, "ts_host": time.time(), "is_offline_test": offline}
        self.tx_frames.append(rec)
        if len(self.tx_frames) > 50:
            self.tx_frames.pop(0)

        # Atualiza referência teórica calculada deste quadro efetivamente transmitido
        self.tx_theoretical_wave = dsp.synthesize_theoretical_reference_wave(
            all_symbols=chain["all_symbols"],
            fc_hz=pkt["carrier_freq_hz"],
            baud=pkt["symbol_rate_baud"],
            fs_hz=(self.tx_hello or {}).get("sample_rate_hz", 40000),
            dc_mv=pkt["dc_offset_mv"],
            amp_pk_mv=pkt["amplitude_peak_mv"],
            q_sign_minus=pkt["q_sign_minus"],
        )
        self.tx_theoretical_wave["frame_id"] = fid

    def _ingest_tx_pwm(self, pkt: Dict[str, Any]) -> None:
        fid = pkt["frame_id"]
        if pkt["start_idx"] == 0 or fid not in self._pwm_acc:
            self._pwm_acc[fid] = {
                "frame_id": fid,
                "sample_rate_hz": pkt["sample_rate_hz"],
                "pwm_period_ticks": pkt["pwm_period_ticks"],
                "chunks": {},
            }
        acc = self._pwm_acc[fid]
        acc["sample_rate_hz"] = pkt["sample_rate_hz"]
        acc["pwm_period_ticks"] = pkt["pwm_period_ticks"]
        acc["chunks"][pkt["start_idx"]] = pkt["cmp_ticks"]
        ticks: List[int] = []
        for s_idx in sorted(acc["chunks"].keys()):
            ticks.extend(acc["chunks"][s_idx])
        self.tx_pwm_ref = {
            "is_programmed_reference": True,
            "frame_id": fid,
            "sample_rate_hz": pkt["sample_rate_hz"],
            "pwm_period_ticks": pkt["pwm_period_ticks"],
            "cmp_ticks": ticks,
        }

    def _ingest_rx_frame(self, pkt: Dict[str, Any], offline: bool) -> None:
        rec = {**pkt, "ts_host": time.time(), "is_offline_test": offline}
        self.rx_frames.append(rec)
        if len(self.rx_frames) > 50:
            self.rx_frames.pop(0)

        self.sync_events.append({
            "ts": time.time(),
            "event": "sync_frame",
            "burst_id": pkt.get("burst_id"),
            "locked_rot_deg": pkt.get("locked_rot_deg"),
            "df_est_hz": pkt.get("df_est_hz"),
            "tx_baud": pkt.get("tx_baud"),
            "crc_ok": pkt.get("crc_ok"),
            "rx_crc": pkt.get("rx_crc"),
            "calc_crc": pkt.get("calc_crc"),
            "is_offline_test": offline,
        })

        # Associação Ground Truth: encontra último frame TX ainda não pareado (ou último compatível)
        matched_tx: Optional[Dict[str, Any]] = None
        unmatched = [f for f in self.tx_frames if not f.get("_matched")]
        if unmatched:
            # Preferência pelo mais antigo não pareado (FIFO no canal), ou mesmo modo
            matched_tx = unmatched[0]
            matched_tx["_matched"] = True
        elif self.tx_frames:
            matched_tx = self.tx_frames[-1]

        gt = dsp.compute_ground_truth(matched_tx, rec, self.current_burst_symbols)
        gt["ts"] = time.time()
        gt["is_offline_test"] = offline
        self.last_ground_truth = gt
        self.ground_truth_history.append(gt)
        if len(self.ground_truth_history) > 50:
            self.ground_truth_history.pop(0)

    def _ingest_rx_adc(self, pkt: Dict[str, Any], offline: bool) -> None:
        if pkt.get("is_full_rate_snapshot"):
            sid = pkt["snapshot_id"]
            if pkt["start_sample_idx"] == 0 or sid not in self._snap_acc:
                self._snap_acc[sid] = {
                    "snapshot_id": sid,
                    "burst_id": pkt["burst_id"],
                    "sample_rate_hz": pkt["sample_rate_hz"],
                    "decim_factor": 1,
                    "efuse_cal_active": pkt["efuse_cal_active"],
                    "is_offline_test": offline,
                    "chunks": {},
                }
            acc = self._snap_acc[sid]
            acc["chunks"][pkt["start_sample_idx"]] = pkt["samples_mv"]
            flat: List[int] = []
            for idx in sorted(acc["chunks"].keys()):
                flat.extend(acc["chunks"][idx])
            self.adc_snapshot = {
                "snapshot_id": sid,
                "burst_id": pkt["burst_id"],
                "sample_rate_hz": pkt["sample_rate_hz"],
                "decim_factor": 1,
                "efuse_cal_active": pkt["efuse_cal_active"],
                "is_offline_test": offline,
                "samples_mv": flat,
            }
            if len(self._snap_acc) > 4:
                oldest = min(self._snap_acc.keys())
                self._snap_acc.pop(oldest, None)
        else:
            self.adc_stream_decim = pkt["decim_factor"]
            self.adc_stream_fs_hz = pkt["sample_rate_hz"]
            self.adc_stream_mv.extend(pkt["samples_mv"])
            if len(self.adc_stream_mv) > MAX_ADC_STREAM_BUFFER:
                self.adc_stream_mv = self.adc_stream_mv[-MAX_ADC_STREAM_BUFFER:]

    @staticmethod
    def _minmax_downsample(samples: List[int], target_points: int = 1200) -> Dict[str, Any]:
        n = len(samples)
        if n <= target_points or target_points < 4:
            return {"samples_mv": samples, "decimated_for_view": False, "original_count": n}
        buckets = target_points // 2
        step = n / float(buckets)
        out: List[int] = []
        for b in range(buckets):
            i0 = int(b * step)
            i1 = max(i0 + 1, min(n, int((b + 1) * step)))
            seg = samples[i0:i1]
            out.append(min(seg))
            out.append(max(seg))
        return {"samples_mv": out, "decimated_for_view": True, "original_count": n}

    def build_state_snapshot(self, include_waveforms: bool = True) -> Dict[str, Any]:
        with self._lock:
            conn = self.serial_mgr.status()
            snap_src = self.adc_snapshot
            scope_view: Dict[str, Any] = {
                "available": False,
                "samples_mv": [],
                "sample_rate_hz": 20000,
                "decim_factor": 1,
                "decimated_for_view": False,
                "is_full_rate_snapshot": False,
                "is_offline_test": self.is_offline_test,
                "stats": None,
            }

            raw_src: List[int] = []
            eff_fs = 20000.0
            if snap_src and snap_src.get("samples_mv"):
                raw_src = snap_src["samples_mv"]
                eff_fs = float(snap_src["sample_rate_hz"])
                scope_view["is_full_rate_snapshot"] = True
                scope_view["decim_factor"] = 1
                scope_view["efuse_cal_active"] = snap_src.get("efuse_cal_active", False)
            elif self.adc_stream_mv:
                raw_src = self.adc_stream_mv
                dec = max(1, self.adc_stream_decim)
                eff_fs = float(self.adc_stream_fs_hz) / dec
                scope_view["is_full_rate_snapshot"] = False
                scope_view["decim_factor"] = dec

            if raw_src:
                vmin = min(raw_src)
                vmax = max(raw_src)
                vmean = sum(raw_src) / float(len(raw_src))
                scope_view["available"] = True
                scope_view["sample_rate_hz"] = eff_fs
                scope_view["stats"] = {
                    "min_mv": vmin,
                    "max_mv": vmax,
                    "mean_mv": round(vmean, 2),
                    "vpp_mv": vmax - vmin,
                    "count": len(raw_src),
                }
                if include_waveforms:
                    ds = self._minmax_downsample(raw_src, 1400)
                    scope_view["samples_mv"] = ds["samples_mv"]
                    scope_view["decimated_for_view"] = ds["decimated_for_view"]
                    scope_view["original_count"] = ds["original_count"]

            # Verifica divergência operacional explícita entre modos TX e RX
            mode_mismatch = False
            if self.tx_hello and self.rx_hello:
                txm = self.tx_hello.get("tx_mode")
                rxm = self.rx_hello.get("rx_mode")
                if txm in (1, 2) and rxm in (1, 2) and txm != rxm:
                    mode_mismatch = True

            return {
                "data_source": self.data_source,
                "is_offline_test": self.is_offline_test,
                "acquisition_enabled": self.acquisition_enabled,
                "mode_mismatch_warning": mode_mismatch,
                "connections": conn,
                "tx_status": self.tx_hello,
                "rx_status": self.rx_hello,
                "last_tx_frame": self.tx_frames[-1] if self.tx_frames else None,
                "last_rx_frame": self.rx_frames[-1] if self.rx_frames else None,
                "tx_frames": self.tx_frames[-10:],
                "rx_frames": self.rx_frames[-10:],
                "last_ground_truth": self.last_ground_truth,
                "sync_events": self.sync_events[-40:],
                "dsp_symbols": self.dsp_symbols[-400:] if include_waveforms else [],
                "scope": scope_view,
                "tx_theoretical_wave": self.tx_theoretical_wave if include_waveforms else None,
                "tx_pwm_ref": self.tx_pwm_ref if include_waveforms else None,
                "cli_logs": self.cli_logs[-80:],
                "dirty_version": self.dirty_version,
            }


hub = TelemetryHub()
app = FastAPI(title="Central de Instrumentação 4-QAM ESP32")


class ConnectReq(BaseModel):
    role: str
    port: str
    baudrate: int = 115200


class RoleReq(BaseModel):
    role: str


class CommandReq(BaseModel):
    role: str
    command: str


class AcqReq(BaseModel):
    enabled: bool


class OfflineSimReq(BaseModel):
    text: str = "4QAM-OK!"
    map_preset: str = "natural"
    q_sign_minus: bool = False
    bit_order: str = "msb"
    fc_tx: float = 1000.40
    rs_tx: float = 100.15
    phase0_deg: float = 73.0


class SessionSaveReq(BaseModel):
    name: Optional[str] = None
    channel_notes: Optional[str] = None


@app.get("/api/ports")
def api_ports() -> Dict[str, Any]:
    return {"ports": sm.list_com_ports()}


@app.get("/api/state")
def api_state() -> Dict[str, Any]:
    return hub.build_state_snapshot(include_waveforms=True)


@app.post("/api/connect")
def api_connect(req: ConnectReq) -> Dict[str, Any]:
    return hub.serial_mgr.connect(req.role, req.port, req.baudrate)


@app.post("/api/disconnect")
def api_disconnect(req: RoleReq) -> Dict[str, Any]:
    return hub.serial_mgr.disconnect(req.role)


@app.post("/api/command")
def api_command(req: CommandReq) -> Dict[str, Any]:
    res = hub.serial_mgr.send_command(req.role, req.command)
    if not res.get("ok"):
        return res
    return res


@app.post("/api/acquisition")
def api_acquisition(req: AcqReq) -> Dict[str, Any]:
    hub.acquisition_enabled = req.enabled
    hub.dirty_version += 1
    return {"ok": True, "acquisition_enabled": hub.acquisition_enabled}


@app.post("/api/clear_plots")
def api_clear_plots() -> Dict[str, Any]:
    hub.reset_live_plots(clear_offline_flag=True)
    return {"ok": True}


@app.get("/api/fft")
def api_fft(window: str = Query("hann")) -> Dict[str, Any]:
    with hub._lock:
        if hub.adc_snapshot and hub.adc_snapshot.get("samples_mv"):
            samples = [float(x) for x in hub.adc_snapshot["samples_mv"]]
            fs_hz = float(hub.adc_snapshot["sample_rate_hz"])
            src = "full_rate_adc_snapshot"
        elif hub.adc_stream_mv:
            samples = [float(x) for x in hub.adc_stream_mv]
            fs_hz = float(hub.adc_stream_fs_hz) / max(1, hub.adc_stream_decim)
            src = f"decimated_adc_stream_{hub.adc_stream_decim}x"
        else:
            return {"valid": False, "reason": "Aguardando amostras reais do ADC1 do receptor.", "freqs_hz": [], "mag_dbv": [], "mag_mv": []}
    res = dsp.compute_fft_spectrum(samples, fs_hz, window_type=window)
    res["source"] = src
    res["is_offline_test"] = hub.is_offline_test
    return res


@app.get("/api/rc_model")
def api_rc_model(
    r1: float = Query(1000.0, gt=0.0),
    c1: float = Query(33.0, gt=0.0),
    r2: float = Query(10000.0, gt=0.0),
    c2: float = Query(3.3, gt=0.0),
    fpwm: float = Query(80000.0, gt=0.0),
    fc: float = Query(1000.0, gt=0.0),
) -> Dict[str, Any]:
    return dsp.compute_rc2_bode(r1_ohm=r1, c1_nf=c1, r2_ohm=r2, c2_nf=c2, f_pwm_hz=fpwm, fc_hz=fc)


@app.post("/api/offline_test")
def api_offline_test(req: OfflineSimReq) -> Dict[str, Any]:
    """
    Executa simulação de protocolo offline e alimenta o parser serial real.
    Marcado explicitamente como DADO OFFLINE DE TESTE (nunca hardware real).
    """
    hub.reset_live_plots(clear_offline_flag=False)
    streams = offline_sim.generate_offline_protocol_exchange(
        text=req.text,
        map_name=req.map_preset,
        q_minus=req.q_sign_minus,
        bit_order=req.bit_order,
        fc_tx=req.fc_tx,
        rs_tx=req.rs_tx,
        phase0_deg=req.phase0_deg,
    )
    tx_parser = proto.HybridSerialParser(on_packet=lambda p: hub.on_serial_packet("tx", p, offline=True))
    rx_parser = proto.HybridSerialParser(on_packet=lambda p: hub.on_serial_packet("rx", p, offline=True))
    for chunk in streams["tx_stream"]:
        tx_parser.feed(chunk)
    for chunk in streams["rx_stream"]:
        rx_parser.feed(chunk)
    return {"ok": True, "is_offline_test": True, "state": hub.build_state_snapshot(True)}


@app.get("/api/sessions")
def api_sessions_list() -> Dict[str, Any]:
    return {"sessions": store.list_sessions()}


@app.post("/api/sessions/save")
def api_sessions_save(req: SessionSaveReq) -> Dict[str, Any]:
    with hub._lock:
        payload = {
            "name": req.name,
            "channel_notes": req.channel_notes or "GPIO18 -> RC2 (1k/33nF + 10k/3.3nF) -> GPIO34",
            "data_source": hub.data_source,
            "is_offline_test": hub.is_offline_test,
            "config_tx": hub.tx_hello,
            "config_rx": hub.rx_hello,
            "sync_events": hub.sync_events,
            "tx_frames": hub.tx_frames,
            "rx_frames": hub.rx_frames,
            "ground_truth_history": hub.ground_truth_history,
            "adc_snapshot": hub.adc_snapshot,
            "dsp_symbols": hub.dsp_symbols,
        }
    return store.save_session(payload, name=req.name)


@app.get("/api/sessions/{session_id}")
def api_session_get(session_id: str) -> Dict[str, Any]:
    doc = store.load_session(session_id)
    if not doc:
        raise HTTPException(status_code=404, detail="Sessão não encontrada.")
    return doc


@app.get("/api/export/json")
def api_export_json() -> Response:
    snap = hub.build_state_snapshot(include_waveforms=False)
    with hub._lock:
        snap["adc_snapshot_full"] = hub.adc_snapshot
        snap["dsp_symbols_full"] = hub.dsp_symbols
        snap["ground_truth_history"] = hub.ground_truth_history
    data = json.dumps(snap, ensure_ascii=False, indent=2).encode("utf-8")
    return Response(
        content=data,
        media_type="application/json",
        headers={"Content-Disposition": "attachment; filename=sessao_4qam.json"},
    )


@app.get("/api/export/csv")
def api_export_csv(kind: str = Query("ground_truth")) -> Response:
    with hub._lock:
        state_copy = {
            "adc_snapshot": hub.adc_snapshot,
            "dsp_symbols": hub.dsp_symbols,
            "last_ground_truth": hub.last_ground_truth,
        }
    raw = store.export_csv_bytes(state_copy, kind=kind)
    return Response(
        content=raw,
        media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": f"attachment; filename=4qam_{kind}.csv"},
    )


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    last_ver = -1
    try:
        while True:
            cur_ver = hub.dirty_version
            if cur_ver != last_ver:
                last_ver = cur_ver
                payload = hub.build_state_snapshot(include_waveforms=True)
                await ws.send_text(json.dumps(payload, ensure_ascii=False))
            await asyncio.sleep(0.10)  # Limite ~10 Hz sem saturar navegador
    except WebSocketDisconnect:
        pass
    except Exception:
        pass


if os.path.isdir(FRONTEND_DIR):
    app.mount("/static", StaticFiles(directory=FRONTEND_DIR), name="static")


@app.get("/")
def root_landing() -> FileResponse:
    return FileResponse(os.path.join(FRONTEND_DIR, "landing.html"))


@app.get("/lab")
def lab_dashboard() -> FileResponse:
    return FileResponse(os.path.join(FRONTEND_DIR, "index.html"))


@app.get("/dashboard")
def lab_dashboard_alias() -> FileResponse:
    return FileResponse(os.path.join(FRONTEND_DIR, "index.html"))


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8420, log_level="info")
