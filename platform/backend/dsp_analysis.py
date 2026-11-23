#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Motor Matemático, Análise Espectral FFT, Filtro RC 2ª Ordem e Ground Truth
===========================================================================
Regra fundamental de engenharia:
  - Separa explicitamente 'referência teórica calculada' de 'medição real ADC'.
  - Nunca calcula BER, SER ou EVM sem associação/alinhamento válido entre
    quadro transmitido e recebido; caso contrário retorna calculable=False.
"""

import math
from typing import Any, Dict, List, Optional, Tuple
import numpy as np

from serial_protocol import crc16_ccitt_false

CONSTELLATIONS: Dict[str, List[Tuple[str, int, int, int]]] = {
    "natural": [
        ("00", -1, -1, 225),
        ("01", -1, +1, 135),
        ("10", +1, -1, 315),
        ("11", +1, +1, 45),
    ],
    "gray": [
        ("00", +1, +1, 45),
        ("01", -1, +1, 135),
        ("10", +1, -1, 315),
        ("11", -1, -1, 225),
    ],
}


def byte_to_symbols(byte_val: int, bit_order: str) -> List[int]:
    """Mesma lógica de qam_mapper_byte_to_symbols()."""
    b = byte_val & 0xFF
    if bit_order == "lsb_bit":
        out = []
        for i in range(4):
            bf = (b >> (2 * i)) & 1
            bs = (b >> (2 * i + 1)) & 1
            out.append((bf << 1) | bs)
        return out
    if bit_order == "lsb_dibit":
        return [b & 3, (b >> 2) & 3, (b >> 4) & 3, (b >> 6) & 3]
    return [(b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3]


def symbols_to_byte(syms: List[int], bit_order: str) -> int:
    b = 0
    if bit_order == "lsb_bit":
        for i in range(4):
            s = syms[i] & 3
            b |= ((s >> 1) & 1) << (2 * i)
            b |= (s & 1) << (2 * i + 1)
        return b
    if bit_order == "lsb_dibit":
        return (syms[0] & 3) | ((syms[1] & 3) << 2) | ((syms[2] & 3) << 4) | ((syms[3] & 3) << 6)
    return ((syms[0] & 3) << 6) | ((syms[1] & 3) << 4) | ((syms[2] & 3) << 2) | (syms[3] & 3)


def build_expected_tx_chain(payload_bytes: List[int],
                              tx_mode: int = 1,
                              map_preset: str = "natural",
                              bit_order: str = "msb",
                              q_sign_minus: bool = False,
                              baud: int = 100,
                              preamble_bytes: int = 4,
                              sync_word: List[int] = [0xD3, 0x91],
                              mode1_use_preamble: bool = False,
                              mode1_use_sync: bool = False) -> Dict[str, Any]:
    """
    Constrói toda a cadeia digital esperada exatamente conforme qam_framer.c e qam_mapper.c,
    para exibição byte-a-byte, bit-a-bit, símbolo-a-símbolo e associação Ground Truth.
    """
    table = CONSTELLATIONS.get(map_preset, CONSTELLATIONS["natural"])
    raw_bytes = [b & 0xFF for b in payload_bytes]
    ord_code = {"msb": 0, "lsb_bit": 1, "lsb_dibit": 2}.get(bit_order, 0)

    segments: List[Tuple[str, List[int], str]] = []
    if tx_mode == 2:
        pre_len = max(2, preamble_bytes)
        half = pre_len // 2
        pre = [0x00 if i < half else 0x33 for i in range(pre_len)]
        hdr = [
            (0x1 << 4) | (ord_code & 0x03),
            (baud >> 8) & 0xFF,
            baud & 0xFF,
            len(raw_bytes) & 0xFF,
        ]
        crc_val = crc16_ccitt_false(bytes(hdr + raw_bytes))
        crc_bytes = [(crc_val >> 8) & 0xFF, crc_val & 0xFF]
        segments = [
            ("preamble", pre, "msb"),
            ("sync", list(sync_word), "msb"),
            ("header", hdr, "msb"),
            ("payload", raw_bytes, bit_order),
            ("crc16", crc_bytes, "msb"),
        ]
    else:
        crc_val = 0
        if mode1_use_preamble:
            half = preamble_bytes // 2
            segments.append(("preamble", [0x00 if i < half else 0x33 for i in range(preamble_bytes)], "msb"))
        if mode1_use_sync:
            segments.append(("sync", list(sync_word), "msb"))
        segments.append(("payload", raw_bytes, bit_order))

    all_symbols: List[Dict[str, Any]] = []
    payload_symbols: List[Dict[str, Any]] = []
    payload_rows: List[Dict[str, Any]] = []

    sym_idx = 0
    for seg_name, seg_bytes, seg_order in segments:
        for b_idx, bval in enumerate(seg_bytes):
            syms = byte_to_symbols(bval, seg_order)
            bits_str = f"{bval:08b}"
            byte_sym_objs: List[Dict[str, Any]] = []
            for k, s in enumerate(syms):
                bits_lbl, iv, qv, iq_deg = table[s]
                phi_wave = math.degrees(math.atan2(+qv if q_sign_minus else -qv, iv)) % 360.0
                obj = {
                    "global_sym_idx": sym_idx,
                    "segment": seg_name,
                    "byte_idx": b_idx,
                    "byte_val": bval,
                    "byte_char": chr(bval) if 32 <= bval <= 126 else ".",
                    "sub_idx": k,
                    "sym_val": s,
                    "bits": bits_lbl,
                    "i_exp": iv,
                    "q_exp": qv,
                    "iq_angle_deg": iq_deg,
                    "wave_phase_deg": round(phi_wave, 1),
                }
                all_symbols.append(obj)
                if seg_name == "payload":
                    payload_symbols.append(obj)
                    byte_sym_objs.append(obj)
                sym_idx += 1
            if seg_name == "payload":
                payload_rows.append({
                    "byte_idx": b_idx,
                    "byte_val": bval,
                    "hex": f"0x{bval:02X}",
                    "bits": bits_str,
                    "char": chr(bval) if 32 <= bval <= 126 else ".",
                    "symbols": byte_sym_objs,
                })

    return {
        "tx_mode": tx_mode,
        "map_preset": map_preset,
        "bit_order": bit_order,
        "q_sign_minus": q_sign_minus,
        "crc16_expected": crc_val,
        "header_symbol_count": len(all_symbols) - len(payload_symbols) - (8 if tx_mode == 2 else 0),
        "total_symbol_count": len(all_symbols),
        "payload_symbol_count": len(payload_symbols),
        "payload_rows": payload_rows,
        "all_symbols": all_symbols,
        "payload_symbols": payload_symbols,
    }


def synthesize_theoretical_reference_wave(all_symbols: List[Dict[str, Any]],
                                        fc_hz: float,
                                        baud: float,
                                        fs_hz: float,
                                        dc_mv: float,
                                        amp_pk_mv: float,
                                        q_sign_minus: bool,
                                        max_points: int = 2400) -> Dict[str, Any]:
    """
    Gera a forma de onda de REFERÊNCIA TEÓRICA CALCULADA (nunca rotulada como medição física)
    a partir dos símbolos realmente transmitidos pelo firmware TX.
    """
    if not all_symbols or baud <= 0 or fs_hz <= 0:
        return {"is_theoretical_reference": True, "t_ms": [], "v_mv": [], "sample_rate_hz": fs_hz}

    total_sec = len(all_symbols) / float(baud)
    total_samples = int(round(total_sec * fs_hz))
    stride = max(1, (total_samples + max_points - 1) // max_points)
    a_eq = float(amp_pk_mv) / math.sqrt(2.0)

    t_ms: List[float] = []
    v_mv: List[float] = []
    n_syms = len(all_symbols)

    for n in range(0, total_samples, stride):
        t = n / float(fs_hz)
        s_idx = min(n_syms - 1, int(t * baud))
        pt = all_symbols[s_idx]
        th = 2.0 * math.pi * fc_hz * t
        q_part = -(pt["q_exp"] * math.sin(th)) if q_sign_minus else +(pt["q_exp"] * math.sin(th))
        v = dc_mv + a_eq * (pt["i_exp"] * math.cos(th) + q_part)
        t_ms.append(round(t * 1000.0, 4))
        v_mv.append(round(v, 3))

    return {
        "is_theoretical_reference": True,
        "sample_rate_hz": fs_hz,
        "stride": stride,
        "t_ms": t_ms,
        "v_mv": v_mv,
    }


def rotate_iq(i_val: float, q_val: float, rot_quad: int) -> Tuple[float, float]:
    """Inverso exato de rx_mapper_rotate_iq() no C (rotação por rot_quad * 90°)."""
    r = rot_quad & 3
    if r == 1:
        return -q_val, i_val
    if r == 2:
        return -i_val, -q_val
    if r == 3:
        return q_val, -i_val
    return i_val, q_val


def compute_ground_truth(tx_record: Optional[Dict[str, Any]],
                         rx_frame: Optional[Dict[str, Any]],
                         rx_symbols: List[Dict[str, Any]]) -> Dict[str, Any]:
    """
    Compara estritamente o frame transmitido (Ground Truth) com o quadro e símbolos
    efetivamente decodificados pelo receptor ESP32.
    Nunca inventa BER/SER/EVM se não houver associação e alinhamento válidos!
    """
    if not tx_record or not rx_frame:
        return {
            "calculable": False,
            "reason": "Aguardando par TX/RX associado para comparação.",
            "ber": None,
            "ser": None,
            "evm_rms_pct": None,
        }

    tx_bytes = tx_record["payload_bytes"]
    rx_bytes = rx_frame.get("payload_bytes", [])
    chain = tx_record["chain"]
    map_preset = tx_record["map_preset"]
    table = CONSTELLATIONS.get(map_preset, CONSTELLATIONS["natural"])

    # Extração por byte comparável
    n_bytes_min = min(len(tx_bytes), len(rx_bytes))
    n_bytes_max = max(len(tx_bytes), len(rx_bytes))
    bytes_equal = sum(1 for i in range(n_bytes_min) if tx_bytes[i] == rx_bytes[i])
    byte_match_pct = (100.0 * bytes_equal / n_bytes_max) if n_bytes_max > 0 else 100.0

    # Alinhamento de bits / símbolos apenas quando os tamanhos de payload coincidem
    comparable = (len(tx_bytes) == len(rx_bytes)) and len(tx_bytes) > 0
    if not comparable:
        return {
            "calculable": False,
            "reason": f"Comprimento incompatível para alinhamento bit-a-bit (TX={len(tx_bytes)} B vs RX={len(rx_bytes)} B).",
            "tx_frame_id": tx_record["frame_id"],
            "tx_text": tx_record["payload_text"],
            "rx_text": rx_frame.get("payload_text", ""),
            "bytes_equal": bytes_equal,
            "bytes_total": n_bytes_max,
            "byte_match_pct": round(byte_match_pct, 2),
            "crc_ok": rx_frame.get("crc_ok"),
            "crc_expected": tx_record["crc16"],
            "crc_rx": rx_frame.get("rx_crc"),
            "crc_calc": rx_frame.get("calc_crc"),
            "ber": None,
            "ser": None,
            "evm_rms_pct": None,
        }

    bit_order_rx = rx_frame.get("payload_order", tx_record["bit_order"])
    tx_pay_syms = chain["payload_symbols"]

    bit_errors = 0
    total_bits = len(tx_bytes) * 8
    sym_errors = 0
    total_syms = len(tx_pay_syms)
    error_positions: List[Dict[str, Any]] = []
    comparison_rows: List[Dict[str, Any]] = []

    # Se recebemos os símbolos DSP da rajada do Modo 2, podemos alinhar I/Q exato!
    # Em Modo 2: quadro tem [Preâmbulo (16 símb)] + [Sync (8 símb)] + [Header (16 símb)] + [Payload (4N)] + [CRC (8 símb)]
    # Encontrar o fim da janela onde state transitou de HEADER para PAYLOAD, ou contar a partir do final (total - 8 CRC - 4N):
    rot_used = rx_frame.get("locked_rot", 0)
    aligned_dsp_syms: Optional[List[Dict[str, Any]]] = None
    if rx_symbols:
        pay_dsp = [s for s in rx_symbols if s.get("deframer_state") == "PAYLOAD"]
        if len(pay_dsp) == total_syms:
            aligned_dsp_syms = pay_dsp
        elif rx_frame.get("rx_mode") == 2 and len(rx_symbols) >= (total_syms + 8):
            # Fallback determinístico pelos últimos 4N símbolos antes dos 8 símbolos finais de CRC
            cand = rx_symbols[-(total_syms + 8):-8]
            if len(cand) == total_syms:
                aligned_dsp_syms = cand

    sum_err2 = 0.0
    sum_ref2 = 0.0
    ideal_radius = 1.0 / math.sqrt(2.0)

    rx_sym_list: List[int] = []
    for b in rx_bytes:
        rx_sym_list.extend(byte_to_symbols(b, bit_order_rx))

    for idx in range(total_syms):
        exp_obj = tx_pay_syms[idx]
        s_exp = exp_obj["sym_val"]
        s_rx = rx_sym_list[idx]
        b_exp = exp_obj["bits"]
        b_rx = table[s_rx][0]

        sym_ok = (s_exp == s_rx)
        if not sym_ok:
            sym_errors += 1
        b_diff = (0 if b_exp[0] == b_rx[0] else 1) + (0 if b_exp[1] == b_rx[1] else 1)
        bit_errors += b_diff

        i_rx_rot: Optional[float] = None
        q_rx_rot: Optional[float] = None
        if aligned_dsp_syms is not None:
            dsp = aligned_dsp_syms[idx]
            i_rot, q_rot = rotate_iq(dsp["i_hat"], dsp["q_hat"], rot_used)
            i_rx_rot = round(i_rot, 4)
            q_rx_rot = round(q_rot, 4)
            s_i = exp_obj["i_exp"] * ideal_radius
            s_q = exp_obj["q_exp"] * ideal_radius
            sum_err2 += (i_rot - s_i) ** 2 + (q_rot - s_q) ** 2
            sum_ref2 += (s_i ** 2 + s_q ** 2)

        if not sym_ok:
            error_positions.append({
                "sym_idx": idx,
                "byte_idx": idx // 4,
                "expected_sym": s_exp,
                "received_sym": s_rx,
                "expected_bits": b_exp,
                "received_bits": b_rx,
            })

        comparison_rows.append({
            "sym_idx": idx,
            "byte_idx": exp_obj["byte_idx"],
            "tx_byte": exp_obj["byte_val"],
            "rx_byte": rx_bytes[exp_obj["byte_idx"]],
            "tx_bits": b_exp,
            "rx_bits": b_rx,
            "tx_sym": s_exp,
            "rx_sym": s_rx,
            "i_exp": exp_obj["i_exp"],
            "q_exp": exp_obj["q_exp"],
            "i_rx": i_rx_rot,
            "q_rx": q_rx_rot,
            "ok": sym_ok,
        })

    ber = bit_errors / float(total_bits)
    ser = sym_errors / float(total_syms)
    evm_pct: Optional[float] = None
    evm_note = "EVM não calculável sem amostras I/Q alinhadas do DSP para cada símbolo do payload."
    if aligned_dsp_syms is not None and sum_ref2 > 0:
        evm_pct = math.sqrt(sum_err2 / sum_ref2) * 100.0
        evm_note = (
            f"Normalizado por AGC do receptor (raio ideal 1/sqrt(2)) após Costas Loop "
            f"e compensação de rotação {rot_used * 90}°."
        )

    return {
        "calculable": True,
        "tx_frame_id": tx_record["frame_id"],
        "tx_text": tx_record["payload_text"],
        "rx_text": rx_frame.get("payload_text", ""),
        "bytes_equal": bytes_equal,
        "bytes_total": n_bytes_max,
        "byte_match_pct": round(byte_match_pct, 2),
        "total_bits": total_bits,
        "bit_errors": bit_errors,
        "ber": ber,
        "total_symbols": total_syms,
        "symbol_errors": sym_errors,
        "ser": ser,
        "evm_rms_pct": round(evm_pct, 3) if evm_pct is not None else None,
        "evm_note": evm_note,
        "crc_ok": rx_frame.get("crc_ok"),
        "crc_expected": tx_record["crc16"],
        "crc_rx": rx_frame.get("rx_crc"),
        "crc_calc": rx_frame.get("calc_crc"),
        "bit_order_tx": tx_record["bit_order"],
        "bit_order_rx": bit_order_rx,
        "map_preset": map_preset,
        "locked_rot_deg": rot_used * 90,
        "error_positions": error_positions,
        "rows": comparison_rows,
    }


def compute_fft_spectrum(samples_mv: List[float], fs_hz: float,
                         window_type: str = "hann") -> Dict[str, Any]:
    """
    Calcula espectro FFT usando APENAS amostras reais adquiridas.
    Remove média DC antes da janela para destacar portadora e espúrios sem vazamento DC.
    """
    n = len(samples_mv)
    if n < 32 or fs_hz <= 0:
        return {
            "valid": False,
            "reason": "Amostras reais insuficientes para análise FFT (mínimo 32 pontos).",
            "freqs_hz": [],
            "mag_dbv": [],
            "mag_mv": [],
        }

    x = np.asarray(samples_mv, dtype=np.float64) / 1000.0  # Volts
    dc_v = float(np.mean(x))
    x_ac = x - dc_v

    w_name = window_type.lower()
    if w_name == "hamming":
        win = np.hamming(n)
    elif w_name == "rect":
        win = np.ones(n)
    else:
        w_name = "hann"
        win = np.hanning(n)

    coherent_gain = float(np.sum(win))
    if coherent_gain <= 0:
        coherent_gain = float(n)

    spec = np.fft.rfft(x_ac * win)
    freqs = np.fft.rfftfreq(n, d=1.0 / float(fs_hz))

    # Magnitude single-sided em pico (Volts e mV)
    mag_vpk = (2.0 * np.abs(spec)) / coherent_gain
    mag_vpk[0] /= 2.0
    if n % 2 == 0:
        mag_vpk[-1] /= 2.0

    mag_mv = mag_vpk * 1000.0
    mag_dbv = 20.0 * np.log10(np.maximum(mag_vpk, 1e-7))

    # Exclui DC bin 0 para busca de pico espectral
    ac_slice = mag_vpk[1:]
    peak_idx = int(np.argmax(ac_slice)) + 1
    peak_freq = float(freqs[peak_idx])
    peak_dbv = float(mag_dbv[peak_idx])
    peak_mv = float(mag_mv[peak_idx])

    # Ruído de fundo estimado via mediana da magnitude fora do pico (+-3 bins)
    mask = np.ones(len(mag_dbv), dtype=bool)
    mask[0] = False
    mask[max(1, peak_idx - 3):min(len(mask), peak_idx + 4)] = False
    noise_floor_dbv = float(np.median(mag_dbv[mask])) if np.any(mask) else -100.0

    # Harmônicos 2*fp, 3*fp, 4*fp dentro de Nyquist
    harmonics: List[Dict[str, float]] = []
    for h in (2, 3, 4):
        fh = h * peak_freq
        if fh < 0.95 * (fs_hz / 2.0):
            b = int(np.argmin(np.abs(freqs - fh)))
            harmonics.append({
                "order": h,
                "freq_hz": round(float(freqs[b]), 2),
                "mag_dbv": round(float(mag_dbv[b]), 2),
                "mag_mv": round(float(mag_mv[b]), 3),
            })

    # Largura de banda observada (-6 dB ao redor do pico AC, definição explícita)
    thresh_6db = peak_dbv - 6.0
    left = peak_idx
    while left > 1 and mag_dbv[left - 1] >= thresh_6db:
        left -= 1
    right = peak_idx
    while right < (len(mag_dbv) - 1) and mag_dbv[right + 1] >= thresh_6db:
        right += 1
    bw_6db_hz = float(freqs[right] - freqs[left])

    return {
        "valid": True,
        "fft_size": n,
        "sample_rate_hz": fs_hz,
        "window": w_name,
        "df_hz": round(float(fs_hz) / n, 3),
        "dc_v": round(dc_v, 4),
        "peak_freq_hz": round(peak_freq, 2),
        "peak_mag_dbv": round(peak_dbv, 2),
        "peak_mag_mv": round(peak_mv, 3),
        "noise_floor_dbv": round(noise_floor_dbv, 2),
        "snr_spectral_db": round(peak_dbv - noise_floor_dbv, 2),
        "bw_6db_hz": round(bw_6db_hz, 2),
        "bw_method": "Largura de pico a -6 dB após janela " + w_name,
        "harmonics": harmonics,
        "freqs_hz": [round(float(f), 2) for f in freqs],
        "mag_dbv": [round(float(v), 2) for v in mag_dbv],
        "mag_mv": [round(float(v), 3) for v in mag_mv],
    }


def compute_rc2_bode(r1_ohm: float = 1000.0, c1_nf: float = 33.0,
                     r2_ohm: float = 10000.0, c2_nf: float = 3.3,
                     f_pwm_hz: float = 80000.0,
                     fc_hz: float = 1000.0) -> Dict[str, Any]:
    """
    Calcula a função de transferência EXATA do filtro RC passa-baixas de 2ª ordem em cascata,
    incluindo o termo de acoplamento entre estágios s*R1*C2:
      H(s) = 1 / [ 1 + s*(R1*C1 + R2*C2 + R1*C2) + s^2*(R1*R2*C1*C2) ]
    """
    c1 = max(1e-12, c1_nf * 1e-9)
    c2 = max(1e-12, c2_nf * 1e-9)
    r1 = max(1.0, r1_ohm)
    r2 = max(1.0, r2_ohm)

    a1 = r1 * c1 + r2 * c2 + r1 * c2
    a2 = r1 * r2 * c1 * c2

    def eval_h(f: float) -> Tuple[float, float]:
        w = 2.0 * math.pi * f
        re = 1.0 - (w * w * a2)
        im = w * a1
        mag = 1.0 / math.hypot(re, im)
        phase = -math.degrees(math.atan2(im, re))
        return 20.0 * math.log10(max(mag, 1e-12)), phase

    freqs = np.logspace(1, 5.3, 180)  # 10 Hz .. 200 kHz
    mag_db = []
    phase_deg = []
    for f in freqs:
        m, p = eval_h(float(f))
        mag_db.append(round(m, 3))
        phase_deg.append(round(p, 2))

    # Frequência exata -3 dB por busca binária
    lo, hi = 1.0, 1e7
    for _ in range(50):
        mid = 0.5 * (lo + hi)
        if eval_h(mid)[0] > -3.0:
            lo = mid
        else:
            hi = mid
    f_3db = 0.5 * (lo + hi)

    # Polos reais do denominador a2*s^2 + a1*s + 1 = 0
    disc = math.sqrt(max(0.0, a1 * a1 - 4.0 * a2))
    p1_hz = abs((-a1 + disc) / (2.0 * a2)) / (2.0 * math.pi)
    p2_hz = abs((-a1 - disc) / (2.0 * a2)) / (2.0 * math.pi)

    att_fc_db, phase_fc = eval_h(fc_hz)
    att_pwm_db, phase_pwm = eval_h(f_pwm_hz)
    fn_hz = 1.0 / (2.0 * math.pi * math.sqrt(a2))
    q_factor = math.sqrt(a2) / a1

    return {
        "is_theoretical_model": True,
        "r1_ohm": r1,
        "c1_nf": c1_nf,
        "r2_ohm": r2,
        "c2_nf": c2_nf,
        "a1_us": round(a1 * 1e6, 3),
        "a2_us2": round(a2 * 1e12, 3),
        "fn_hz": round(fn_hz, 2),
        "q_factor": round(q_factor, 4),
        "f_3db_hz": round(f_3db, 2),
        "poles_hz": [round(p1_hz, 2), round(p2_hz, 2)],
        "gain_fc_db": round(att_fc_db, 3),
        "phase_fc_deg": round(phase_fc, 2),
        "attenuation_pwm_db": round(att_pwm_db, 2),
        "phase_pwm_deg": round(phase_pwm, 2),
        "freqs_hz": [round(float(f), 2) for f in freqs],
        "mag_db": mag_db,
        "phase_deg": phase_deg,
    }
