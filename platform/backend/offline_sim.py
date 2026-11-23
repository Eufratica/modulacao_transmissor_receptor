#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Simulador de Protocolo Offline (Apenas para Testes Automatizados / Offline)
===========================================================================
Gera fluxos binários IDÊNTICOS ao protocolo real 4QAM-TLM v1 emitido pelos
ESP32, usando o canal físico simulado de tools/validar_sistema_e2e.py.

NUNCA se passa por hardware real: todo evento é marcado com
`is_offline_test = True` e separado estritamente de sessões reais de COM.
"""

import math
import os
import struct
import sys
from typing import Any, Dict, List

TOOLS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import gerar_vetores_teste as ref  # noqa: E402
import validar_sistema_e2e as sim  # noqa: E402

from serial_protocol import (  # noqa: E402
    MSG_HELLO_STATUS,
    MSG_RX_ADC_CHUNK,
    MSG_RX_BURST_EVENT,
    MSG_RX_FRAME_EVENT,
    MSG_RX_SYMBOL_BATCH,
    MSG_TX_FRAME_EVENT,
    MSG_TX_PWM_CHUNK,
    build_packet,
)


def _clamp_i16(v: float) -> int:
    return max(-32768, min(32767, int(round(v))))


def generate_offline_protocol_exchange(text: str = "4QAM-OK!",
                                       map_name: str = "natural",
                                       q_minus: bool = False,
                                       bit_order: str = "msb",
                                       fc_tx: float = 1000.40,
                                       rs_tx: float = 100.15,
                                       phase0_deg: float = 73.0) -> Dict[str, List[bytes]]:
    """
    Executa simulação TX PWM 80kHz -> Filtro RC 2ª Ordem -> ADC 20kHz -> RX DSP
    e codifica os bytes binários exatos do protocolo 4QAM-TLM v1 para validar
    o parser serial, reconstrução, Ground Truth, FFT e constelação em teste offline.
    """
    tx_packets: List[bytes] = []
    rx_packets: List[bytes] = []
    tx_seq = 0
    rx_seq = 0

    map_u = 1 if map_name == "gray" else 0
    ord_u = {"msb": 0, "lsb_bit": 1, "lsb_dibit": 2}.get(bit_order, 0)
    payload_bytes = text.encode("ascii")
    frame = ref.build_mode2_frame(payload_bytes, baud=100, bit_order=bit_order)
    syms = frame["symbols"]

    # 1. TX HELLO
    p_tx_hello = struct.pack(
        "<BBBBBBBBBBBBIIIIHHHHIIII",
        1, 1, 2, map_u, ord_u, int(q_minus), 0, 0, 0, 1, 1, 18,
        1000, 100, 40000, 80000, 1300, 900, 1000, 0, 1, len(syms), 0, 0
    )
    tx_packets.append(build_packet(MSG_HELLO_STATUS, p_tx_hello, seq=tx_seq, ts_us=100000, flags=0))
    tx_seq += 1

    # 2. TX FRAME EVENT (queued + done)
    hdr_syms = len(syms) - len(payload_bytes) * 4 - 8
    for st in (0, 2):
        p_tx_fr = struct.pack(
            "<IBBBBIIHHHHHBB",
            1, 2, map_u, ord_u, int(q_minus), 1000, 100, 1300, 900, hdr_syms, frame["crc_val"], len(payload_bytes), st, 0
        ) + payload_bytes
        tx_packets.append(build_packet(MSG_TX_FRAME_EVENT, p_tx_fr, seq=tx_seq, ts_us=102000 + st * 500000, flags=0))
        tx_seq += 1

    # 3. TX PWM CHUNK (512 amostras programadas @ 40 kHz)
    tx_mod = sim.TxModulatorC(map_name=map_name, q_sign_minus=q_minus)
    pwm_ticks: List[int] = []
    for k in range(512):
        s_val = syms[min(len(syms) - 1, k // 400)]
        mv, _ = tx_mod.next_symbol_mv(s_val)
        pwm_ticks.append(tx_mod.mv_to_cmp(mv))
    for off in range(0, 512, 128):
        cnt = min(128, 512 - off)
        p_pwm = struct.pack("<IIIH", 1, off, 40000, 1000) + struct.pack(f"<{cnt}H", *pwm_ticks[off:off + cnt])
        tx_packets.append(build_packet(MSG_TX_PWM_CHUNK, p_pwm, seq=tx_seq, ts_us=120000 + off * 25, flags=0x02))
        tx_seq += 1

    # 4. Simulação física PWM 80 kHz -> RC2 -> ADC 20 kHz -> RX DSP
    step_rc = sim.exact_rc2_stepper(1000.0, 33e-9, 10000.0, 3.3e-9)
    rx = sim.RxDemodAndDeframerC(fc_hz=1000, baud=100, fs_hz=20000, map_name=map_name, q_sign_minus=q_minus, mode=2)
    rx_sym_logs: List[Dict[str, Any]] = []
    orig_push = rx._push_symbol

    def hook_push(ih: float, qh: float) -> None:
        st_before = rx.state
        dec = rx._decide_symbol(ih, qh, rx.locked_rot)
        orig_push(ih, qh)
        st_code = {"SYNC_SEARCH": 0, "HEADER": 1, "PAYLOAD": 2, "CRC": 3, "RAW_STREAM": 4}.get(st_before, 0)
        rot_st = (rx.locked_rot & 3) | ((st_code & 7) << 2) | (0x20 if st_before != "SYNC_SEARCH" else 0)
        rx_sym_logs.append({
            "sym_idx": len(rx_sym_logs),
            "ih": _clamp_i16(ih * 4096.0),
            "qh": _clamp_i16(qh * 4096.0),
            "im": _clamp_i16(rx.mid_i_curr * 4096.0),
            "qm": _clamp_i16(rx.mid_q_curr * 4096.0),
            "ped": _clamp_i16(rx.last_ped * 4096.0),
            "df_c": _clamp_i16(rx.last_df_hz * 100.0),
            "nco_mrad": max(0, min(65535, int(round(rx.nco_phase * 1000.0)))),
            "gard": _clamp_i16(rx.last_gardner_err * 4096.0),
            "ppm": _clamp_i16((rx.sym_rate_corr / rx.sym_inc_nom) * 1e6),
            "snr_c": _clamp_i16(rx.last_snr_db * 100.0),
            "sp": _clamp_i16(rx.sym_phase * 4096.0),
            "dec": dec,
            "rot_st": rot_st,
        })

    rx._push_symbol = hook_push  # type: ignore[method-assign]

    fs_pwm = 80000.0
    t_pwm = 1.0 / fs_pwm
    sil = int(0.030 * fs_pwm)
    frm = int((len(syms) / rs_tx) * fs_pwm)
    tot = sil + frm + int(0.015 * fs_pwm)
    v1 = v2 = 1300.0
    a_mv = 900.0 / math.sqrt(2.0)
    table = ref.CONSTELLATIONS[map_name]
    ph0_rad = math.radians(phase0_deg)
    adc_snap: List[int] = []
    cmp_t = 394
    rng = 0x12345678

    for k in range(tot):
        if k % 2 == 0:
            t = (k - sil) * t_pwm + 0.37 / 20000.0
            if k < sil or k >= sil + frm:
                vid = 1300.0
            else:
                si = min(len(syms) - 1, max(0, int(t * rs_tx)))
                _, iv, qv, _ = table[syms[si]]
                th = 2.0 * math.pi * fc_tx * t + ph0_rad
                qt = -(qv * math.sin(th)) if q_minus else +(qv * math.sin(th))
                vid = 1300.0 + a_mv * (iv * math.cos(th) + qt)
            cmp_t = max(1, min(999, int(vid / 3300.0 * 1000.0 + 0.5)))

        d = cmp_t / 1000.0
        thg = d * t_pwm
        tlw = 0.5 * (t_pwm - thg)
        v1, v2 = step_rc(v1, v2, 0.0, tlw)
        v1, v2 = step_rc(v1, v2, 3300.0, thg)
        v1, v2 = step_rc(v1, v2, 0.0, tlw)

        if k % 4 == 0:
            rng = (1103515245 * rng + 12345) & 0x7FFFFFFF
            nv = max(0.0, min(2600.0, v2 + (((rng >> 16) & 0xFF) - 128) * 0.12))
            ac = max(0, min(4095, int(nv / 2500.0 * 4095.0 + 0.5)))
            mv = ac / 4095.0 * 2500.0
            if k >= sil and len(adc_snap) < 2048:
                adc_snap.append(int(round(mv)))
            rx.process_sample(mv)

    # 5. Empacota RX HELLO + BURST + ADC_SNAP + SYMBOLS + FRAME
    p_rx_hello = struct.pack(
        "<BBBBBBBBBBBBBBHIIIHHHhIIIIII",
        2, 1, 2, map_u, ord_u, int(q_minus), 0, 0, 0, 1, 0, 34, 6, 1, 10,
        1000, 100, 20000, 600, 13000, 8560, _clamp_i16(rx.last_df_hz * 100.0),
        1, 0, 0, len(rx_sym_logs), 0, 0
    )
    rx_packets.append(build_packet(MSG_HELLO_STATUS, p_rx_hello, seq=rx_seq, ts_us=100000, flags=1))
    rx_seq += 1

    rx_packets.append(build_packet(MSG_RX_BURST_EVENT, struct.pack("<BBIii", 0, 0, 1, 13000, 8560), seq=rx_seq, ts_us=130000, flags=1))
    rx_seq += 1

    for off in range(0, len(adc_snap), 128):
        cnt = min(128, len(adc_snap) - off)
        p_adc = struct.pack("<IHIIH", 1, 1, off, 20000, 1) + struct.pack(f"<{cnt}h", *adc_snap[off:off + cnt])
        rx_packets.append(build_packet(MSG_RX_ADC_CHUNK, p_adc, seq=rx_seq, ts_us=135000 + off * 50, flags=0x07))
        rx_seq += 1

    for off in range(0, len(rx_sym_logs), 5):
        batch = rx_sym_logs[off:off + 5]
        p_sym = struct.pack("<IBB", 1, len(batch), 0)
        for s in batch:
            p_sym += struct.pack(
                "<HhhhhhhHhhhhBB",
                s["sym_idx"], s["ih"], s["qh"], s["im"], s["qm"], s["ped"],
                s["df_c"], s["nco_mrad"], s["gard"], s["ppm"], s["snr_c"],
                s["sp"], s["dec"], s["rot_st"]
            )
        rx_packets.append(build_packet(MSG_RX_SYMBOL_BATCH, p_sym, seq=rx_seq, ts_us=200000 + off * 10000, flags=1))
        rx_seq += 1

    pay_rx, crc_ok, rot_deg, df_hz, snr_db = rx.recovered_frames[0]
    p_rx_fr = struct.pack(
        "<IBBBBHHHHhHHh",
        1, 2, int(crc_ok), rot_deg // 90, ord_u, 100, frame["crc_val"], frame["crc_val"],
        len(pay_rx), _clamp_i16(df_hz * 100.0), 13000, 8560, _clamp_i16(snr_db * 100.0)
    ) + pay_rx
    rx_packets.append(build_packet(MSG_RX_FRAME_EVENT, p_rx_fr, seq=rx_seq, ts_us=950000, flags=1))

    return {"tx_stream": tx_packets, "rx_stream": rx_packets}
