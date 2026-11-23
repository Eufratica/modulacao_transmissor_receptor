#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Persistência Local de Sessões, Logs e Exportação (CSV / JSON)
=============================================================
Armazena experimentos completos em arquivos JSON locais dentro de
C:\Modulador_ESP\platform\sessions\, mantendo distinção explícita entre:
  - parâmetros configurados nos firmwares;
  - medições reais adquiridas dos ESP32;
  - métricas / referências teóricas calculadas.
"""

import csv
import datetime
import io
import json
import os
import re
from typing import Any, Dict, List, Optional

SESSIONS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "sessions"))


def _safe_id(raw: str) -> str:
    return re.sub(r"[^A-Za-z0-9_\-]", "_", raw)[:64]


def save_session(session_payload: Dict[str, Any], name: Optional[str] = None) -> Dict[str, Any]:
    os.makedirs(SESSIONS_DIR, exist_ok=True)
    now = datetime.datetime.now()
    ts_str = now.strftime("%Y%m%d_%H%M%S")
    label = _safe_id(name or session_payload.get("name") or f"sessao_{ts_str}")
    session_id = f"{ts_str}_{label}"

    record = {
        "session_id": session_id,
        "created_at": now.isoformat(),
        "name": name or f"Experimento {now.strftime('%d/%m/%Y %H:%M:%S')}",
        "metadata": {
            "protocol_version": "4QAM-TLM v1",
            "data_source": session_payload.get("data_source", "unknown"),
            "is_offline_test": bool(session_payload.get("is_offline_test", False)),
        },
        "config_tx": session_payload.get("config_tx"),
        "config_rx": session_payload.get("config_rx"),
        "channel_notes": session_payload.get("channel_notes", "GPIO18 -> RC2 (1k/33nF + 10k/3.3nF) -> GPIO34"),
        "sync_events": session_payload.get("sync_events", []),
        "tx_frames": session_payload.get("tx_frames", []),
        "rx_frames": session_payload.get("rx_frames", []),
        "ground_truth_history": session_payload.get("ground_truth_history", []),
        "adc_snapshot": session_payload.get("adc_snapshot"),
        "dsp_symbols": session_payload.get("dsp_symbols", [])[-512:],
    }

    path = os.path.join(SESSIONS_DIR, f"{session_id}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(record, f, ensure_ascii=False, indent=2)

    return {"ok": True, "session_id": session_id, "path": path, "session": record}


def list_sessions() -> List[Dict[str, Any]]:
    if not os.path.isdir(SESSIONS_DIR):
        return []
    items: List[Dict[str, Any]] = []
    for fname in sorted(os.listdir(SESSIONS_DIR), reverse=True):
        if not fname.endswith(".json"):
            continue
        path = os.path.join(SESSIONS_DIR, fname)
        try:
            with open(path, encoding="utf-8") as f:
                doc = json.load(f)
            items.append({
                "session_id": doc.get("session_id", fname[:-5]),
                "created_at": doc.get("created_at"),
                "name": doc.get("name"),
                "is_offline_test": doc.get("metadata", {}).get("is_offline_test", False),
                "tx_count": len(doc.get("tx_frames", [])),
                "rx_count": len(doc.get("rx_frames", [])),
                "gt_count": len(doc.get("ground_truth_history", [])),
                "adc_samples": len((doc.get("adc_snapshot") or {}).get("samples_mv", [])),
            })
        except Exception:
            continue
    return items


def load_session(session_id: str) -> Optional[Dict[str, Any]]:
    sid = _safe_id(session_id)
    path = os.path.join(SESSIONS_DIR, f"{sid}.json")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def export_csv_bytes(state: Dict[str, Any], kind: str = "ground_truth") -> bytes:
    """Exporta métricas reais / amostras ADC para CSV UTF-8."""
    buf = io.StringIO()
    w = csv.writer(buf)

    if kind == "adc":
        snap = state.get("adc_snapshot") or {}
        samples = snap.get("samples_mv", [])
        fs = snap.get("sample_rate_hz", 20000) or 20000
        decim = snap.get("decim_factor", 1) or 1
        eff_fs = fs / float(decim)
        w.writerow(["# fonte", "ESP32 ADC1" if not snap.get("is_offline_test") else "TESTE OFFLINE"])
        w.writerow(["sample_idx", "time_ms", "voltage_mv", "sample_rate_hz", "decim_factor"])
        for i, v in enumerate(samples):
            w.writerow([i, f"{(i / eff_fs) * 1000.0:.4f}", v, fs, decim])
    elif kind == "dsp_symbols":
        syms = state.get("dsp_symbols", [])
        w.writerow(["sym_idx", "i_hat", "q_hat", "i_mid", "q_mid", "costas_ped",
                    "df_hz", "nco_phase_rad", "gardner_err", "sym_rate_corr_ppm",
                    "snr_db", "sym_phase", "decision", "rot_deg", "state"])
        for s in syms:
            w.writerow([
                s.get("sym_idx"), s.get("i_hat"), s.get("q_hat"),
                s.get("i_mid"), s.get("q_mid"), s.get("costas_ped"),
                s.get("df_hz"), s.get("nco_phase_rad"), s.get("gardner_err"),
                s.get("sym_rate_corr_ppm"), s.get("snr_db"), s.get("sym_phase"),
                s.get("decision"), s.get("rot_deg"), s.get("deframer_state"),
            ])
    else:
        gt = state.get("last_ground_truth") or {}
        rows = gt.get("rows", [])
        w.writerow(["# calculable", gt.get("calculable"), "BER", gt.get("ber"), "SER", gt.get("ser"), "EVM_PCT", gt.get("evm_rms_pct")])
        w.writerow(["sym_idx", "byte_idx", "tx_byte_hex", "rx_byte_hex", "tx_bits", "rx_bits",
                    "tx_sym", "rx_sym", "i_exp", "q_exp", "i_rx_rot", "q_rx_rot", "ok"])
        for r in rows:
            w.writerow([
                r["sym_idx"], r["byte_idx"], f"0x{r['tx_byte']:02X}", f"0x{r['rx_byte']:02X}",
                r["tx_bits"], r["rx_bits"], r["tx_sym"], r["rx_sym"],
                r["i_exp"], r["q_exp"], r.get("i_rx"), r.get("q_rx"), r["ok"],
            ])

    return buf.getvalue().encode("utf-8")
