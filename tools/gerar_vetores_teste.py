#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Gerador de Vetores de Referencia 4-QAM (Teste 8 - Etapa E / Etapa F)
====================================================================
Reproduz com fidelidade matematica as equacoes, tabelas de constelacao,
ordens de bits, enquadramento (Modo 1 / Modo 2), CRC-16/CCITT-FALSE e
quantizacao PWM implementados nos firmwares ESP32 (transmitter/ e receiver/).

Arquivos gerados:
  - docs/vetores_teste/vetores_referencia.md
  - docs/vetores_teste/vetores_amostras_4qam.csv
"""

import csv
import math
import os
from typing import Dict, List, Tuple

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT_DIR = os.path.join(ROOT_DIR, "docs", "vetores_teste")

# Parametros padrao identicos a qam_config.c
FC_HZ = 1000
RS_BAUD = 100
FS_HZ = 40000
PWM_FREQ_HZ = 80000
PWM_PERIOD_TICKS = 1000  # 160 MHz / (2 * 80 kHz)
VDD_MV = 3300.0
DC_OFFSET_MV = 1300.0
VPK_MV = 900.0
A_EQ_MV = VPK_MV / math.sqrt(2.0)

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


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: polinomio 0x1021, inicializacao 0xFFFF, sem reflexao."""
    crc = 0xFFFF
    for b in data:
        crc ^= (b & 0xFF) << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def byte_to_symbols(byte_val: int, bit_order: str) -> List[int]:
    """Extrai os 4 simbolos (0..3) de 1 byte na ordem escolhida (identico a qam_mapper.c)."""
    b = byte_val & 0xFF
    if bit_order == "msb":
        return [(b >> 6) & 0x03, (b >> 4) & 0x03, (b >> 2) & 0x03, b & 0x03]
    if bit_order == "lsb_bit":
        syms = []
        for i in range(4):
            b_first = (b >> (2 * i)) & 0x01
            b_second = (b >> (2 * i + 1)) & 0x01
            syms.append((b_first << 1) | b_second)
        return syms
    if bit_order == "lsb_dibit":
        return [b & 0x03, (b >> 2) & 0x03, (b >> 4) & 0x03, (b >> 6) & 0x03]
    raise ValueError(f"Ordem desconhecida: {bit_order}")


def build_mode2_frame(payload: bytes, baud: int = RS_BAUD, bit_order: str = "msb",
                      preamble_bytes: int = 4, sync_word: bytes = b"\xD3\x91") -> Dict:
    """Constroi quadro Modo 2 exatamente como qam_framer.c."""
    order_code = {"msb": 0, "lsb_bit": 1, "lsb_dibit": 2}[bit_order]
    half = preamble_bytes // 2
    preamble = bytes([0x00 if i < half else 0x33 for i in range(preamble_bytes)])
    header = bytes([
        (0x1 << 4) | ((2 - 2) << 2) | (order_code & 0x03),
        (baud >> 8) & 0xFF,
        baud & 0xFF,
        len(payload) & 0xFF,
    ])
    crc_val = crc16_ccitt_false(header + payload)
    crc_bytes = bytes([(crc_val >> 8) & 0xFF, crc_val & 0xFF])

    # Preamble, Sync, Header e CRC usam MSB-first; apenas Payload usa bit_order
    syms: List[int] = []
    for chunk_bytes, chunk_order in [
        (preamble, "msb"),
        (sync_word, "msb"),
        (header, "msb"),
        (payload, bit_order),
        (crc_bytes, "msb"),
    ]:
        for b in chunk_bytes:
            syms.extend(byte_to_symbols(b, chunk_order))

    return {
        "preamble": preamble,
        "sync_word": sync_word,
        "header": header,
        "payload": payload,
        "crc_val": crc_val,
        "crc_bytes": crc_bytes,
        "all_bytes": preamble + sync_word + header + payload + crc_bytes,
        "symbols": syms,
    }


def mv_to_pwm_cmp(v_mv: float) -> int:
    duty = v_mv / VDD_MV
    ticks = int((duty * PWM_PERIOD_TICKS) + 0.5)
    return max(1, min(PWM_PERIOD_TICKS - 1, ticks))


def waveform_sample_mv(i_val: int, q_val: int, t_s: float, q_sign_minus: bool = False) -> float:
    theta = 2.0 * math.pi * FC_HZ * t_s
    q_term = -q_val * math.sin(theta) if q_sign_minus else +q_val * math.sin(theta)
    return DC_OFFSET_MV + A_EQ_MV * (i_val * math.cos(theta) + q_term)


def effective_waveform_phase_deg(i_val: int, q_val: int, q_sign_minus: bool) -> float:
    """
    Fase phi na forma polar Vpk * cos(2*pi*fc*t + phi):
      cos(theta + phi) = cos(phi)*cos(theta) - sin(phi)*sin(theta)
    Para +Q*sin(theta): cos(phi)=I/sqrt(2), sin(phi)=-Q/sqrt(2) -> phi = atan2(-Q, I)
    Para -Q*sin(theta): cos(phi)=I/sqrt(2), sin(phi)=+Q/sqrt(2) -> phi = atan2(+Q, I)
    """
    phi = math.degrees(math.atan2(+q_val if q_sign_minus else -q_val, i_val))
    return phi % 360.0


def fmt_bytes_hex(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def generate_csv_for_ab() -> int:
    """Gera vetores_amostras_4qam.csv para \"AB\" no Modo 1 padrao (MSB-first, Natural, +sin)."""
    os.makedirs(OUT_DIR, exist_ok=True)
    csv_path = os.path.join(OUT_DIR, "vetores_amostras_4qam.csv")
    payload = b"AB"
    syms: List[int] = []
    for b in payload:
        syms.extend(byte_to_symbols(b, "msb"))

    samples_per_sym = FS_HZ // RS_BAUD  # 400
    table = CONSTELLATIONS["natural"]

    total_rows = 0
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "sample_idx",
            "time_ms",
            "symbol_idx",
            "symbol_val",
            "bits",
            "I",
            "Q",
            "iq_angle_deg",
            "v_ideal_mv",
            "pwm_cmp_ticks",
        ])
        sample_idx = 0
        for s_idx, sym in enumerate(syms):
            bits, iv, qv, iq_deg = table[sym]
            for _ in range(samples_per_sym):
                t_s = sample_idx / FS_HZ
                v_mv = waveform_sample_mv(iv, qv, t_s, q_sign_minus=False)
                cmp_ticks = mv_to_pwm_cmp(v_mv)
                writer.writerow([
                    sample_idx,
                    f"{t_s * 1000.0:.4f}",
                    s_idx,
                    sym,
                    bits,
                    f"{iv:+d}",
                    f"{qv:+d}",
                    iq_deg,
                    f"{v_mv:.4f}",
                    cmp_ticks,
                ])
                sample_idx += 1
                total_rows += 1
    return total_rows


def generate_markdown_vectors(csv_rows: int) -> None:
    os.makedirs(OUT_DIR, exist_ok=True)
    md_path = os.path.join(OUT_DIR, "vetores_referencia.md")

    crc_check = crc16_ccitt_false(b"123456789")
    assert crc_check == 0x29B1, f"CRC check mismatch: {crc_check:#06x}"

    lines: List[str] = []
    lines.append("# Vetores de Teste Reproduzíveis 4-QAM (Teste 8)")
    lines.append("")
    lines.append("Gerado automaticamente por [`tools/gerar_vetores_teste.py`](../../tools/gerar_vetores_teste.py) "
                 "com exatamente a mesma matemática e enquadramento dos firmwares `transmitter/` e `receiver/`.")
    lines.append("")
    lines.append("## 1. Parâmetros de Referência do Cenário Padrão")
    lines.append("")
    lines.append("| Parâmetro | Valor Padrão | Observação |")
    lines.append("|---|---:|---|")
    lines.append(f"| Portadora ($f_c$) | `{FC_HZ} Hz` | Período $T_c = 1{{,}}000\\text{{ ms}}$ |")
    lines.append(f"| Taxa de símbolos ($R_s$, provisória) | `{RS_BAUD} baud` | $T_s = 10{{,}}000\\text{{ ms}}$ ({FS_HZ // RS_BAUD} amostras/símbolo a `40 kHz`) |")
    lines.append(f"| Taxa de amostragem TX ($f_s$) | `{FS_HZ} Hz` | Divisor 2:1 do PWM a `{PWM_FREQ_HZ} Hz` (`MCPWM0A`, topo `{PWM_PERIOD_TICKS}` ticks) |")
    lines.append(f"| Offset DC ($V_{{DC}}$) | `{int(DC_OFFSET_MV)} mV` | Centro PWM teórico = `{mv_to_pwm_cmp(DC_OFFSET_MV)}` ticks (`39,39%`) |")
    lines.append(f"| Amplitude de pico ($V_{{pk}}$) | `{int(VPK_MV)} mV` | Excursão ideal `[400 .. 2200] mV` |")
    lines.append(f"| Fator $A = V_{{pk}}/\\sqrt{{2}}$ | `{A_EQ_MV:.4f} mV` | Garante $\\sqrt{{(A\\,I)^2 + (A\\,Q)^2}} = {VPK_MV:.2f}\\text{{ mV}}$ |")
    lines.append("| Equação padrão (`qsign plus`) | $s(t) = A[I\\cos(\\omega_c t) + Q\\cos(\\omega_c t - 90^\\circ)]$ | Equivalente a $A[I\\cos(\\omega_c t) + Q\\sin(\\omega_c t)]$ |")
    lines.append("| Constelação padrão (`map natural`) | `0:(-1,-1)`, `1:(-1,+1)`, `2:(+1,-1)`, `3:(+1,+1)` | Alternativa Gray disponível via `/map gray` |")
    lines.append("| Ordem de bits padrão (`order msb`) | `MSB-first` | `(b7,b6), (b5,b4), (b3,b2), (b1,b0)` |")
    lines.append("")

    lines.append("## 2. Vetor Base por Símbolo (Modo 3 — `/test sym00|sym01|sym10|sym11`)")
    lines.append("")
    lines.append("### 2.1 Tabela Padrão Natural (`map natural`)")
    lines.append("")
    lines.append("| Símbolo | Bits | $I$ | $Q$ | $I_{\\text{mV}} = A\\,I$ | $Q_{\\text{mV}} = A\\,Q$ | Ângulo geom. $\\theta_k = \\text{atan2}(Q,I)$ | Fase onda $\\phi$ em $\\cos(\\omega_c t + \\phi)$ (`+sin` / `-sin`) | $v(0\\text{ ms})$ (`+sin`) | `PWM` (`t=0`) |")
    lines.append("|---:|:---:|---:|---:|---:|---:|---:|:---:|---:|---:|")
    for sym, (bits, iv, qv, iq_deg) in enumerate(CONSTELLATIONS["natural"]):
        phi_plus = effective_waveform_phase_deg(iv, qv, False)
        phi_minus = effective_waveform_phase_deg(iv, qv, True)
        v0 = waveform_sample_mv(iv, qv, 0.0, False)
        cmp0 = mv_to_pwm_cmp(v0)
        lines.append(
            f"| `{sym}` | `{bits}` | `{iv:+d}` | `{qv:+d}` | `{iv * A_EQ_MV:+.2f}` | `{qv * A_EQ_MV:+.2f}` | "
            f"`{iq_deg}°` | `{phi_plus:.0f}°` / `{phi_minus:.0f}°` | `{v0:.2f} mV` | `{cmp0}` |"
        )
    lines.append("")

    lines.append("### 2.2 Tabela Alternativa Gray (`map gray`)")
    lines.append("")
    lines.append("| Símbolo | Bits | $I$ | $Q$ | Ângulo geom. $\\text{atan2}(Q,I)$ | Fase onda $\\phi$ (`+sin` / `-sin`) |")
    lines.append("|---:|:---:|---:|---:|---:|:---:|")
    for sym, (bits, iv, qv, iq_deg) in enumerate(CONSTELLATIONS["gray"]):
        phi_plus = effective_waveform_phase_deg(iv, qv, False)
        phi_minus = effective_waveform_phase_deg(iv, qv, True)
        lines.append(f"| `{sym}` | `{bits}` | `{iv:+d}` | `{qv:+d}` | `{iq_deg}°` | `{phi_plus:.0f}°` / `{phi_minus:.0f}°` |")
    lines.append("")

    lines.append("### 2.3 Primeiras 10 Amostras ($0 \\le n \\le 9$, $1/4$ de ciclo a `40 kHz`) para Cada Símbolo (`Natural`, `+sin`)")
    lines.append("")
    lines.append("| Amostra $n$ | $t$ (ms) | Símbolo `0` (`-1,-1`) | Símbolo `1` (`-1,+1`) | Símbolo `2` (`+1,-1`) | Símbolo `3` (`+1,+1`) |")
    lines.append("|---:|---:|---:|---:|---:|---:|")
    for n in range(10):
        t_s = n / FS_HZ
        vals = []
        for sym in range(4):
            _, iv, qv, _ = CONSTELLATIONS["natural"][sym]
            v = waveform_sample_mv(iv, qv, t_s, False)
            c = mv_to_pwm_cmp(v)
            vals.append(f"`{v:7.2f} mV` (`{c}`)")
        lines.append(f"| `{n}` | `{t_s * 1000:.3f}` | " + " | ".join(vals) + " |")
    lines.append("")

    lines.append("## 3. Vetores ASCII nas 3 Ordens de Bits (`A`, `B`, `3`, `AB`)")
    lines.append("")
    lines.append("### 3.1 Caractere `'A'` (`0x41` = `0100 0001`), `'B'` (`0x42` = `0100 0010`) e `'3'` (`0x33` = `0011 0011`)")
    lines.append("")
    lines.append("| Caractere (Hex / Binário) | Ordem de bits | Sequência de Símbolos | Pares $(I, Q)$ na Tabela Natural |")
    lines.append("|:---|:---|:---|:---|")
    for ch in ["A", "B", "3"]:
        b = ord(ch)
        for order_key, order_label in [("msb", "MSB-first"), ("lsb_bit", "LSB-bit-first"), ("lsb_dibit", "LSB-dibit-first")]:
            syms = byte_to_symbols(b, order_key)
            iq_str = " ".join(
                f"[{CONSTELLATIONS['natural'][s][0]}:{CONSTELLATIONS['natural'][s][1]:+d},{CONSTELLATIONS['natural'][s][2]:+d}]"
                for s in syms
            )
            sym_str = ", ".join(str(s) for s in syms)
            lines.append(f"| `'{ch}'` (`0x{b:02X}` = `{b:08b}`) | `{order_label}` | `[{sym_str}]` | `{iq_str}` |")
    lines.append("")

    lines.append("> **Nota didática para osciloscópio (`/test text 3`):** o caractere `'3'` (`0x33` = `00 11 00 11` em `MSB-first`) "
                 "alterna exatamente entre o símbolo `0` (`I=-1, Q=-1`, $225^\\circ$) e o símbolo `3` (`I=+1, Q=+1`, $45^\\circ$), "
                 "produzindo inversões puras de **$180^\\circ$** a cada $10\\text{ ms}$ (`100 baud`).")
    lines.append("")

    lines.append("## 4. Vetores de Enquadramento Completo: Modo 1 vs. Modo 2")
    lines.append("")
    lines.append(f"- **Teste padrão CRC-16/CCITT-FALSE:** string `\"123456789\"` $\\rightarrow$ **`0x{crc_check:04X}`** (polinômio `0x1021`, inicial `0xFFFF`).")
    lines.append("")
    for idx_msg, txt in enumerate([b"A", b"AB", b"XYZ", b"4QAM", b"4QAM-OK!"], start=1):
        f_m2 = build_mode2_frame(txt, baud=RS_BAUD, bit_order="msb")
        m1_syms: List[int] = []
        for b in txt:
            m1_syms.extend(byte_to_symbols(b, "msb"))
        lines.append(f"### 4.{idx_msg} Mensagem `\"{txt.decode('ascii')}\"` (`{len(txt)} byte(s)`)")
        lines.append("")
        lines.append(f"- **Modo 1 (`MSB-first`, sem preâmbulo/sync):** `{len(m1_syms)} símbolos` = `{m1_syms}` "
                     f"(duração a `{RS_BAUD} baud`: **`{len(m1_syms) * 1000.0 / RS_BAUD:.1f} ms`**).")
        lines.append(f"- **Modo 2 (`Quadro Completo`):** `{len(f_m2['all_bytes'])} bytes` $\\rightarrow$ `{len(f_m2['symbols'])} símbolos` "
                     f"(duração a `{RS_BAUD} baud`: **`{len(f_m2['symbols']) * 1000.0 / RS_BAUD:.1f} ms`**):")
        lines.append(f"  - Preâmbulo (`4B`): `{fmt_bytes_hex(f_m2['preamble'])}`")
        lines.append(f"  - Sync Word (`2B`): `{fmt_bytes_hex(f_m2['sync_word'])}`")
        lines.append(f"  - Cabeçalho (`4B`): `{fmt_bytes_hex(f_m2['header'])}` (ver=`0x1`, 4-QAM, order=`0/MSB`, baud=`{RS_BAUD}`, len=`{len(txt)}`)")
        lines.append(f"  - Payload (`{len(txt)}B`): `{fmt_bytes_hex(f_m2['payload'])}`")
        lines.append(f"  - CRC-16 (`2B` sobre Header+Payload): **`0x{f_m2['crc_val']:04X}`** (`{fmt_bytes_hex(f_m2['crc_bytes'])}`)")
        lines.append(f"  - Bytes completos do quadro: `{fmt_bytes_hex(f_m2['all_bytes'])}`")
        lines.append("")

    lines.append("## 5. Arquivo CSV de Amostras Temporais")
    lines.append("")
    lines.append(f"O arquivo [`vetores_amostras_4qam.csv`](./vetores_amostras_4qam.csv) contém **`{csv_rows}` amostras** "
                 f"(exatamente `8 símbolos` = `80,000 ms` a `40 kHz`) da transmissão Modo 1 de `\"AB\"` "
                 "(MSB-first, Natural, `+cos(wt-90)` / `+sin`, `fc=1000 Hz`, `Rs=100 baud`, `Vdc=1300 mV`, `Vpk=900 mV`), "
                 "prontas para conferência em Python, MATLAB, GNU Radio ou comparação com captura de osciloscópio.")
    lines.append("")

    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main() -> None:
    rows = generate_csv_for_ab()
    generate_markdown_vectors(rows)
    print(f"OK: gerados vetores_referencia.md e vetores_amostras_4qam.csv ({rows} amostras) em {OUT_DIR}")


if __name__ == "__main__":
    main()
