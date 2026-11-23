#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Parser e Codificador do Protocolo Híbrido 4QAM-TLM v1
====================================================
Decodifica simultaneamente:
  - Pacotes binários versionados (0xAA 0x55 + cabeçalho + payload + CRC-16/CCITT-FALSE)
  - Linhas ASCII da CLI dos firmwares ESP32 (4QAM-TX> / 4QAM-RX> / ESP_LOG)

Detecta e contabiliza:
  - Erros de CRC-16
  - Divergências de versão do protocolo
  - Pacotes perdidos (lacunas de sequência)
  - Pacotes duplicados (mesmo seq consecutivo)
  - Taxa real de bytes/s e pacotes/s recebidos da porta serial
"""

from dataclasses import dataclass, field
import struct
import time
from typing import Any, Callable, Dict, List, Optional

PROTO_SYNC = b"\xAA\x55"
PROTO_VERSION = 0x01

MSG_HELLO_STATUS = 0x01
MSG_TX_FRAME_EVENT = 0x02
MSG_TX_PWM_CHUNK = 0x03
MSG_RX_BURST_EVENT = 0x10
MSG_RX_SYMBOL_BATCH = 0x11
MSG_RX_FRAME_EVENT = 0x12
MSG_RX_ADC_CHUNK = 0x13


def crc16_ccitt_false(data: bytes) -> int:
    """Mesmo CRC-16/CCITT-FALSE (0x1021, init 0xFFFF) usado nos firmwares C."""
    crc = 0xFFFF
    for b in data:
        crc ^= (b & 0xFF) << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_packet(msg_type: int, payload: bytes, seq: int = 0, ts_us: int = 0, flags: int = 0) -> bytes:
    """Empacota um quadro 4QAM-TLM v1 válido (usado por testes unitários e simulador offline)."""
    hdr = struct.pack("<BBBHIB",
                      PROTO_VERSION, msg_type & 0xFF, flags & 0xFF,
                      seq & 0xFFFF, ts_us & 0xFFFFFFFF, 0)[:9]
    # Header completo após 0xAA 0x55: ver(1) type(1) flags(1) seq(2) ts_us(4) len(2) = 11 bytes
    hdr = struct.pack("<BBBHIH", PROTO_VERSION, msg_type & 0xFF, flags & 0xFF,
                      seq & 0xFFFF, ts_us & 0xFFFFFFFF, len(payload))
    body = hdr + payload
    crc = crc16_ccitt_false(body)
    return PROTO_SYNC + body + struct.pack("<H", crc)


@dataclass
class StreamStats:
    packets_ok: int = 0
    packets_lost: int = 0
    packets_dup: int = 0
    crc_errors: int = 0
    version_errors: int = 0
    malformed_packets: int = 0
    bytes_rx_total: int = 0
    cli_lines_rx: int = 0
    last_seq: Optional[int] = None
    wire_bps: float = 0.0
    _window_bytes: int = 0
    _window_t0: float = field(default_factory=time.monotonic)

    def record_bytes(self, n: int) -> None:
        self.bytes_rx_total += n
        self._window_bytes += n
        now = time.monotonic()
        dt = now - self._window_t0
        if dt >= 0.5:
            self.wire_bps = self._window_bytes / dt
            self._window_bytes = 0
            self._window_t0 = now

    def to_dict(self) -> Dict[str, Any]:
        return {
            "packets_ok": self.packets_ok,
            "packets_lost": self.packets_lost,
            "packets_dup": self.packets_dup,
            "crc_errors": self.crc_errors,
            "version_errors": self.version_errors,
            "malformed_packets": self.malformed_packets,
            "bytes_rx_total": self.bytes_rx_total,
            "cli_lines_rx": self.cli_lines_rx,
            "last_seq": self.last_seq,
            "wire_bps": round(self.wire_bps, 1),
        }


def decode_payload(msg_type: int, flags: int, seq: int, ts_us: int, payload: bytes) -> Dict[str, Any]:
    """Decodifica payloads específicos dos firmwares TX e RX mantendo rastreabilidade."""
    base: Dict[str, Any] = {
        "kind": "telemetry",
        "msg_type": msg_type,
        "flags": flags,
        "role": "rx" if (flags & 0x01) else "tx",
        "seq": seq,
        "ts_us": ts_us,
        "protocol_version": PROTO_VERSION,
    }

    if msg_type == MSG_HELLO_STATUS:
        if len(payload) >= 52 and payload[0] == 1:
            (role, pver, tx_mode, map_p, bit_ord, q_minus, inv_pol, pulse, idle_m,
             tlm_en, is_tx, gpio, fc_hz, baud, fs_hz, pwm_hz,
             dc_mv, amp_mv, pwm_period, pre1_flags, frames_sent, syms_sent,
             err_cnt, tlm_drop) = struct.unpack("<BBBBBBBBBBBBIIIIHHHHIIII", payload[:52])
            base.update({
                "event": "tx_hello",
                "role": "tx",
                "firmware_proto": pver,
                "tx_mode": tx_mode,
                "map_preset": "gray" if map_p == 1 else "natural",
                "bit_order": ["msb", "lsb_bit", "lsb_dibit"][bit_ord] if bit_ord <= 2 else "msb",
                "q_sign_minus": bool(q_minus),
                "invert_polarity": bool(inv_pol),
                "pulse_shape": "taper" if pulse == 1 else "rect",
                "idle_mode": "pilot" if idle_m == 1 else "dc",
                "telem_enabled": bool(tlm_en),
                "transmitting": bool(is_tx),
                "pwm_gpio": gpio,
                "carrier_freq_hz": fc_hz,
                "symbol_rate_baud": baud,
                "sample_rate_hz": fs_hz,
                "pwm_freq_hz": pwm_hz,
                "dc_offset_mv": dc_mv,
                "amplitude_peak_mv": amp_mv,
                "pwm_period_ticks": pwm_period,
                "mode1_use_preamble": bool(pre1_flags & 1),
                "mode1_use_sync_word": bool(pre1_flags & 2),
                "frames_sent": frames_sent,
                "symbols_sent": syms_sent,
                "error_count": err_cnt,
                "telem_dropped": tlm_drop,
            })
            return base

        if len(payload) >= 60 and payload[0] == 2:
            (role, pver, rx_mode, map_p, bit_ord, q_minus, inv_pol, rot_quad, sync1,
             tlm_en, sig_act, gpio, adc_ch, efuse_cal, adc_decim,
             fc_hz, baud, fs_hz, sq_dmv, dc_dmv, amp_dmv, df_chz,
             frames_ok, frames_crc_err, bytes_raw, syms_demod,
             adc_err, tlm_drop) = struct.unpack("<BBBBBBBBBBBBBBHIIIHHHhIIIIII", payload[:60])
            base.update({
                "event": "rx_hello",
                "role": "rx",
                "firmware_proto": pver,
                "rx_mode": rx_mode,
                "map_preset": "gray" if map_p == 1 else "natural",
                "bit_order": ["msb", "lsb_bit", "lsb_dibit"][bit_ord] if bit_ord <= 2 else "msb",
                "q_sign_minus": bool(q_minus),
                "invert_polarity": bool(inv_pol),
                "fixed_rotation_deg": (rot_quad & 3) * 90,
                "mode1_use_sync_word": bool(sync1),
                "telem_enabled": bool(tlm_en),
                "signal_active": bool(sig_act),
                "adc_gpio": gpio,
                "adc_channel": adc_ch,
                "adc_resolution_bits": 12,
                "efuse_cal_active": bool(efuse_cal),
                "adc_stream_decim": adc_decim,
                "carrier_freq_hz": fc_hz,
                "symbol_rate_baud": baud,
                "sample_rate_hz": fs_hz,
                "squelch_rms_mv": sq_dmv / 10.0,
                "dc_est_mv": dc_dmv / 10.0,
                "amp_pk_est_mv": amp_dmv / 10.0,
                "freq_offset_hz": df_chz / 100.0,
                "frames_ok": frames_ok,
                "frames_crc_err": frames_crc_err,
                "bytes_raw_rx": bytes_raw,
                "symbols_demod": syms_demod,
                "adc_errors": adc_err,
                "telem_dropped": tlm_drop,
            })
            return base

    if msg_type == MSG_TX_FRAME_EVENT and len(payload) >= 28:
        (frame_id, tx_mode, map_p, bit_ord, q_minus, fc_hz, baud,
         dc_mv, amp_mv, hdr_syms, crc16, pay_len, status_code, _) = struct.unpack("<IBBBBIIHHHHHBB", payload[:28])
        raw_pay = payload[28:28 + pay_len]
        status_names = {0: "queued", 1: "started", 2: "done", 3: "dropped"}
        base.update({
            "event": "tx_frame",
            "frame_id": frame_id,
            "tx_mode": tx_mode,
            "map_preset": "gray" if map_p == 1 else "natural",
            "bit_order": ["msb", "lsb_bit", "lsb_dibit"][bit_ord] if bit_ord <= 2 else "msb",
            "q_sign_minus": bool(q_minus),
            "carrier_freq_hz": fc_hz,
            "symbol_rate_baud": baud,
            "dc_offset_mv": dc_mv,
            "amplitude_peak_mv": amp_mv,
            "header_symbol_count": hdr_syms,
            "crc16": crc16,
            "payload_len": pay_len,
            "status": status_names.get(status_code, "unknown"),
            "payload_bytes": list(raw_pay),
            "payload_text": raw_pay.decode("utf-8", errors="replace"),
        })
        return base

    if msg_type == MSG_TX_PWM_CHUNK and len(payload) >= 14:
        frame_id, start_idx, fs_hz, period_ticks = struct.unpack("<IIIH", payload[:14])
        count = (len(payload) - 14) // 2
        ticks = list(struct.unpack(f"<{count}H", payload[14:14 + count * 2])) if count > 0 else []
        base.update({
            "event": "tx_pwm_chunk",
            "frame_id": frame_id,
            "start_idx": start_idx,
            "sample_rate_hz": fs_hz,
            "pwm_period_ticks": period_ticks,
            "cmp_ticks": ticks,
            "is_programmed_reference": True,
        })
        return base

    if msg_type == MSG_RX_BURST_EVENT and len(payload) >= 14:
        kind_u, _, burst_id, dc_dmv, amp_dmv = struct.unpack("<BBIii", payload[:14])
        base.update({
            "event": "rx_burst",
            "burst_kind": "start" if kind_u == 0 else "end",
            "burst_id": burst_id,
            "dc_est_mv": dc_dmv / 10.0,
            "amp_pk_est_mv": amp_dmv / 10.0,
        })
        return base

    if msg_type == MSG_RX_SYMBOL_BATCH and len(payload) >= 6:
        burst_id, cnt, _ = struct.unpack("<IBB", payload[:6])
        syms: List[Dict[str, Any]] = []
        off = 6
        rec_size = 26
        state_names = {0: "SYNC_SEARCH", 1: "HEADER", 2: "PAYLOAD", 3: "CRC", 4: "RAW_STREAM"}
        for _ in range(cnt):
            if off + rec_size > len(payload):
                break
            (sym_idx, ih, qh, im, qm, ped, df_c, nco_mrad, gard, ppm,
             snr_c, sp_q12, dec_sym, rot_st) = struct.unpack("<HhhhhhhHhhhhBB", payload[off:off + rec_size])
            off += rec_size
            rot_quad = rot_st & 0x03
            st_code = (rot_st >> 2) & 0x07
            locked = bool(rot_st & 0x20)
            syms.append({
                "sym_idx": sym_idx,
                "i_hat": ih / 4096.0,
                "q_hat": qh / 4096.0,
                "i_mid": im / 4096.0,
                "q_mid": qm / 4096.0,
                "costas_ped": ped / 4096.0,
                "df_hz": df_c / 100.0,
                "nco_phase_rad": nco_mrad / 1000.0,
                "gardner_err": gard / 4096.0,
                "sym_rate_corr_ppm": ppm,
                "snr_db": snr_c / 100.0,
                "sym_phase": sp_q12 / 4096.0,
                "decision": dec_sym,
                "rot_quad": rot_quad,
                "rot_deg": rot_quad * 90,
                "deframer_state": state_names.get(st_code, "UNKNOWN"),
                "sync_locked": locked,
            })
        base.update({
            "event": "rx_symbols",
            "burst_id": burst_id,
            "symbols": syms,
        })
        return base

    if msg_type == MSG_RX_FRAME_EVENT and len(payload) >= 24:
        (burst_id, rx_mode, crc_ok, locked_rot, pay_ord, tx_baud,
         rx_crc, calc_crc, pay_len, df_c, dc_dmv_u, amp_dmv_u, snr_c) = struct.unpack("<IBBBBHHHHhHHh", payload[:24])
        raw_pay = payload[24:24 + pay_len]
        base.update({
            "event": "rx_frame",
            "burst_id": burst_id,
            "rx_mode": rx_mode,
            "crc_ok": bool(crc_ok),
            "locked_rot": locked_rot,
            "locked_rot_deg": (locked_rot & 3) * 90,
            "payload_order": ["msb", "lsb_bit", "lsb_dibit"][pay_ord] if pay_ord <= 2 else "msb",
            "tx_baud": tx_baud,
            "rx_crc": rx_crc,
            "calc_crc": calc_crc,
            "payload_len": pay_len,
            "df_est_hz": df_c / 100.0,
            "dc_est_mv": dc_dmv_u / 10.0,
            "amp_pk_est_mv": amp_dmv_u / 10.0,
            "snr_db": snr_c / 100.0,
            "payload_bytes": list(raw_pay),
            "payload_text": raw_pay.decode("utf-8", errors="replace"),
        })
        return base

    if msg_type == MSG_RX_ADC_CHUNK and len(payload) >= 16:
        burst_id, snap_id, start_idx, fs_hz, decim = struct.unpack("<IHIIH", payload[:16])
        count = (len(payload) - 16) // 2
        samples = list(struct.unpack(f"<{count}h", payload[16:16 + count * 2])) if count > 0 else []
        base.update({
            "event": "rx_adc_chunk",
            "burst_id": burst_id,
            "snapshot_id": snap_id,
            "start_sample_idx": start_idx,
            "sample_rate_hz": fs_hz,
            "decim_factor": decim,
            "is_full_rate_snapshot": bool(flags & 0x02),
            "efuse_cal_active": bool(flags & 0x04),
            "samples_mv": samples,
        })
        return base

    base["event"] = "unknown"
    base["raw_len"] = len(payload)
    return base


class HybridSerialParser:
    """
    Separa de forma determinística pacotes binários 4QAM-TLM v1 (0xAA 0x55 ... CRC16)
    de texto ASCII da CLI no mesmo fluxo serial.
    """

    def __init__(self,
                 on_packet: Optional[Callable[[Dict[str, Any]], None]] = None,
                 on_cli_line: Optional[Callable[[str], None]] = None):
        self.buf = bytearray()
        self.ascii_line = bytearray()
        self.stats = StreamStats()
        self.on_packet = on_packet
        self.on_cli_line = on_cli_line

    def _emit_cli_byte(self, b: int) -> None:
        if b == 0x0A:  # LF
            line = self.ascii_line.decode("utf-8", errors="replace").strip("\r")
            self.ascii_line.clear()
            if line:
                self.stats.cli_lines_rx += 1
                if self.on_cli_line:
                    self.on_cli_line(line)
        elif b != 0x0D:
            if len(self.ascii_line) < 1024:
                self.ascii_line.append(b)

    def feed(self, data: bytes) -> List[Dict[str, Any]]:
        events: List[Dict[str, Any]] = []
        if not data:
            return events

        self.stats.record_bytes(len(data))
        self.buf.extend(data)

        while len(self.buf) > 0:
            sync_idx = self.buf.find(PROTO_SYNC)
            if sync_idx == -1:
                # Mantém último byte caso seja 0xAA esperando 0x55 no próximo chunk
                safe_end = len(self.buf) - (1 if self.buf[-1] == 0xAA else 0)
                for b in self.buf[:safe_end]:
                    self._emit_cli_byte(b)
                del self.buf[:safe_end]
                break

            if sync_idx > 0:
                for b in self.buf[:sync_idx]:
                    self._emit_cli_byte(b)
                del self.buf[:sync_idx]

            # Agora self.buf começa em 0xAA 0x55; precisamos do cabeçalho (13 bytes)
            if len(self.buf) < 13:
                break

            ver, msg_type, flags, seq, ts_us, pay_len = struct.unpack("<BBBHIH", self.buf[2:13])

            if ver != PROTO_VERSION:
                self.stats.version_errors += 1
                self._emit_cli_byte(self.buf[0])
                del self.buf[0:1]
                continue

            if pay_len > 512:
                self.stats.malformed_packets += 1
                self._emit_cli_byte(self.buf[0])
                del self.buf[0:1]
                continue

            total_len = 15 + pay_len
            if len(self.buf) < total_len:
                break  # Aguarda restante do pacote chegar pela serial

            body = bytes(self.buf[2:13 + pay_len])
            crc_rx = struct.unpack("<H", self.buf[13 + pay_len:total_len])[0]
            crc_calc = crc16_ccitt_false(body)

            if crc_rx != crc_calc:
                self.stats.crc_errors += 1
                self._emit_cli_byte(self.buf[0])
                del self.buf[0:1]
                continue

            # Pacote válido! Checa integridade da sequência
            if self.stats.last_seq is not None:
                expected_seq = (self.stats.last_seq + 1) & 0xFFFF
                if seq == self.stats.last_seq:
                    self.stats.packets_dup += 1
                elif seq != expected_seq:
                    delta = (seq - expected_seq) & 0xFFFF
                    if delta < 0x8000:
                        self.stats.packets_lost += delta
            self.stats.last_seq = seq
            self.stats.packets_ok += 1

            payload = bytes(self.buf[13:13 + pay_len])
            del self.buf[:total_len]

            pkt = decode_payload(msg_type, flags, seq, ts_us, payload)
            events.append(pkt)
            if self.on_packet:
                self.on_packet(pkt)

        return events
