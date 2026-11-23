#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Validacao Ponta a Ponta do Sistema 4-QAM (Etapa F)
===================================================
Executa verificacao matematica e de DSP espelhando fielmente os firmwares
em C (transmitter/ e receiver/), incluindo simulacao fisica do chaveamento
PWM a 80 kHz passando pelo filtro RC passivo de 2a ordem (R1=1k, C1=33nF,
R2=10k, C2=3.3nF) por solucao analitica exata da EDO e amostragem a 20 kHz
com quantizacao ADC de 12 bits.

Nota de honestidade de engenharia:
  Estes testes validam a matematica, o enquadramento, o filtro RC e as
  malhas de sincronismo em simulacao no PC; testes em hardware fisico
  (gravacao nos ESP32 e osciloscopio) devem ser executados na bancada.
"""

import csv
import math
import os
import sys
from typing import List, Optional, Tuple

sys.path.insert(0, os.path.dirname(__file__))
import gerar_vetores_teste as ref  # noqa: E402

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def exact_rc2_stepper(r1: float = 1000.0, c1: float = 33e-9,
                      r2: float = 10000.0, c2: float = 3.3e-9):
    """
    Resolve exatamente o sistema linear dx/dt = A*x + B*u para o filtro RC 2a ordem:
      x = [v1 (C1), v2 (C2)]^T, u = V_in
    Retorna funcao step(v1, v2, vin, dt) -> (v1_next, v2_next).
    """
    a11 = -(1.0 / (r1 * c1) + 1.0 / (r2 * c1))
    a12 = 1.0 / (r2 * c1)
    a21 = 1.0 / (r2 * c2)
    a22 = -1.0 / (r2 * c2)

    tr = a11 + a22
    det = a11 * a22 - a12 * a21
    disc = math.sqrt(tr * tr - 4.0 * det)
    lam1 = 0.5 * (tr + disc)
    lam2 = 0.5 * (tr - disc)

    # Autovetores [a12, lam - a11]^T
    v1_1, v2_1 = a12, lam1 - a11
    v1_2, v2_2 = a12, lam2 - a11
    det_p = v1_1 * v2_2 - v1_2 * v2_1

    def step(x1: float, x2: float, vin: float, dt: float) -> Tuple[float, float]:
        if dt <= 0.0:
            return x1, x2
        # Equilibrio DC: x_eq = [vin, vin]
        dx1 = x1 - vin
        dx2 = x2 - vin
        # Coordenadas modais c = P^-1 * (x - x_eq)
        c1_m = (v2_2 * dx1 - v1_2 * dx2) / det_p
        c2_m = (-v2_1 * dx1 + v1_1 * dx2) / det_p
        e1 = math.exp(lam1 * dt)
        e2 = math.exp(lam2 * dt)
        nx1 = vin + c1_m * e1 * v1_1 + c2_m * e2 * v1_2
        nx2 = vin + c1_m * e1 * v2_1 + c2_m * e2 * v2_2
        return nx1, nx2

    return step


class TxModulatorC:
    """Espelho fiel de qam_modulator.c (LUT 1024 pontos + acumuladores racionais)."""

    LUT_SIZE = 1024

    def __init__(self, fc_hz: int = 1000, baud: int = 100, fs_hz: int = 40000,
                 dc_mv: float = 1300.0, vpk_mv: float = 900.0, vdd_mv: float = 3300.0,
                 pwm_period: int = 1000, map_name: str = "natural", q_sign_minus: bool = False):
        self.fc = fc_hz
        self.baud = baud
        self.fs = fs_hz
        self.dc_mv = dc_mv
        self.vpk_mv = vpk_mv
        self.vdd_mv = vdd_mv
        self.pwm_period = pwm_period
        self.a_eq_mv = vpk_mv / math.sqrt(2.0)
        self.table = ref.CONSTELLATIONS[map_name]
        self.q_sign_minus = q_sign_minus
        self.carrier_phase_num = 0
        self.sym_phase_num = 0
        self.lut = [math.sin(2.0 * math.pi * i / self.LUT_SIZE) for i in range(self.LUT_SIZE)]

    def _lut_sin_cos(self, turns: float) -> Tuple[float, float]:
        t = turns - math.floor(turns)
        pos = t * self.LUT_SIZE
        idx = int(pos) % self.LUT_SIZE
        frac = pos - int(pos)
        idx_next = (idx + 1) % self.LUT_SIZE
        sin_v = self.lut[idx] + frac * (self.lut[idx_next] - self.lut[idx])

        cpos = ((t + 0.25) % 1.0) * self.LUT_SIZE
        cidx = int(cpos) % self.LUT_SIZE
        cfrac = cpos - int(cpos)
        cidx_next = (cidx + 1) % self.LUT_SIZE
        cos_v = self.lut[cidx] + cfrac * (self.lut[cidx_next] - self.lut[cidx])
        return sin_v, cos_v

    def next_carrier_mv(self) -> float:
        sin_v, cos_v = self._lut_sin_cos(self.carrier_phase_num / self.fs)
        v = self.dc_mv + self.vpk_mv * cos_v
        self.carrier_phase_num = (self.carrier_phase_num + self.fc) % self.fs
        return v

    def next_symbol_mv(self, sym: int) -> Tuple[float, bool]:
        _, iv, qv, _ = self.table[sym]
        sin_v, cos_v = self._lut_sin_cos(self.carrier_phase_num / self.fs)
        q_term = -(qv * sin_v) if self.q_sign_minus else +(qv * sin_v)
        v = self.dc_mv + self.a_eq_mv * (iv * cos_v + q_term)
        self.carrier_phase_num = (self.carrier_phase_num + self.fc) % self.fs
        self.sym_phase_num += self.baud
        finished = False
        if self.sym_phase_num >= self.fs:
            self.sym_phase_num -= self.fs
            finished = True
        return v, finished

    def mv_to_cmp(self, v_mv: float) -> int:
        duty = v_mv / self.vdd_mv
        ticks = int(duty * self.pwm_period + 0.5)
        return max(1, min(self.pwm_period - 1, ticks))


class RxDemodAndDeframerC:
    """Espelho fiel de qam_demod.c + rx_deframer.c."""

    def __init__(self, fc_hz: int = 1000, baud: int = 100, fs_hz: int = 20000,
                 map_name: str = "natural", q_sign_minus: bool = False,
                 mode: int = 2, mode1_use_sync: bool = False, fixed_rot: int = 0,
                 bit_order: str = "msb", squelch_rms_mv: float = 60.0):
        self.fc = fc_hz
        self.baud = baud
        self.fs = fs_hz
        self.table = ref.CONSTELLATIONS[map_name]
        self.q_sign_minus = q_sign_minus
        self.mode = mode
        self.mode1_use_sync = mode1_use_sync
        self.fixed_rot = fixed_rot & 0x03
        self.bit_order = bit_order
        self.squelch_rms_mv = squelch_rms_mv

        # Ganhos identicos a rx_config.c
        self.costas_kp = 0.16
        self.costas_ki = 0.017
        self.gardner_kp = 0.030
        self.gardner_ki = 0.0015

        self.dc_primed = False
        self.dc_est_mv = 0.0
        self.pwr_ema = 0.0
        self.amp_pk_est_mv = 0.0
        self.signal_active = False
        self.active_cnt = 0
        self.quiet_cnt = 0
        self.quiet_limit = max(40, int(2.5 * (fs_hz / baud)))

        self.nco_omega0 = 2.0 * math.pi * (fc_hz / fs_hz)
        self.costas_freq_max = 2.0 * math.pi * (35.0 / fs_hz)
        self.box_len = max(4, min(128, (fs_hz + fc_hz // 2) // fc_hz))
        self.sym_inc_nom = baud / fs_hz

        self.sync_word = b"\xD3\x91"
        self.reset_tracking()
        self.reset_deframer()
        self.recovered_frames: List[Tuple[bytes, bool, int, float, float]] = []
        self.raw_bytes: List[int] = []
        self.last_df_hz = 0.0
        self.last_snr_db = 0.0
        self.last_ped = 0.0
        self.last_gardner_err = 0.0

    def reset_tracking(self) -> None:
        self.nco_phase = 0.0
        self.costas_freq = 0.0
        self.costas_decim = 0
        self.box_i = [0.0] * self.box_len
        self.box_q = [0.0] * self.box_len
        self.box_sum_i = 0.0
        self.box_sum_q = 0.0
        self.box_idx = 0
        self.sym_phase = 0.0
        self.sym_rate_corr = 0.0
        self.mid_i_curr = self.mid_q_curr = 0.0
        self.mid_i_prev = self.mid_q_prev = 0.0
        self.has_prev_mid = False
        self.bnd_i_curr = self.bnd_q_curr = 0.0
        self.has_curr_bnd = False
        self.acc_i = self.acc_q = 0.0
        self.acc_cnt = 0

    def reset_deframer(self) -> None:
        self.sync_buf: List[Tuple[float, float]] = []
        self.sym_acc: List[int] = []
        self.hdr: List[int] = []
        self.payload: List[int] = []
        self.payload_len = 0
        self.payload_order = self.bit_order
        self.crc_bytes: List[int] = []
        if self.mode == 1 and not self.mode1_use_sync:
            self.state = "RAW_STREAM"
            self.locked_rot = self.fixed_rot
        else:
            self.state = "SYNC_SEARCH"
            self.locked_rot = 0

    def _rotate(self, i_v: float, q_v: float, rot: int) -> Tuple[float, float]:
        r = rot & 0x03
        if r == 1:
            return +q_v, -i_v
        if r == 2:
            return -i_v, -q_v
        if r == 3:
            return -q_v, +i_v
        return i_v, q_v

    def _decide_symbol(self, i_v: float, q_v: float, rot: int) -> int:
        ir, qr = self._rotate(i_v, q_v, rot)
        best_s = 0
        best_d2 = 1e9
        for sym, (_, ic, qc, _) in enumerate(self.table):
            d2 = (ir - ic * 0.70710678) ** 2 + (qr - qc * 0.70710678) ** 2
            if d2 < best_d2:
                best_d2 = d2
                best_s = sym
        return best_s

    @staticmethod
    def _syms_to_byte(syms: List[int], order: str) -> int:
        b = 0
        if order == "msb":
            return ((syms[0] & 3) << 6) | ((syms[1] & 3) << 4) | ((syms[2] & 3) << 2) | (syms[3] & 3)
        if order == "lsb_bit":
            for i in range(4):
                s = syms[i] & 3
                b |= ((s >> 1) & 1) << (2 * i)
                b |= (s & 1) << (2 * i + 1)
            return b
        return (syms[0] & 3) | ((syms[1] & 3) << 2) | ((syms[2] & 3) << 4) | ((syms[3] & 3) << 6)

    def _push_symbol(self, i_hat: float, q_hat: float) -> None:
        if self.state == "SYNC_SEARCH":
            self.sync_buf.append((i_hat, q_hat))
            need = len(self.sync_word) * 4
            if len(self.sync_buf) > need:
                self.sync_buf.pop(0)
            if len(self.sync_buf) == need:
                for rot in range(4):
                    matched = True
                    for b_i, exp_b in enumerate(self.sync_word):
                        gs = [self._decide_symbol(*self.sync_buf[b_i * 4 + k], rot) for k in range(4)]
                        if self._syms_to_byte(gs, "msb") != exp_b:
                            matched = False
                            break
                    if matched:
                        self.locked_rot = rot
                        self.sym_acc = []
                        self.hdr = []
                        self.payload = []
                        self.crc_bytes = []
                        self.sync_buf = []
                        self.state = "RAW_STREAM" if self.mode == 1 else "HEADER"
                        return
            return

        sym = self._decide_symbol(i_hat, q_hat, self.locked_rot)
        self.sym_acc.append(sym)
        if len(self.sym_acc) < 4:
            return
        syms = self.sym_acc
        self.sym_acc = []

        if self.state == "RAW_STREAM":
            self.raw_bytes.append(self._syms_to_byte(syms, self.bit_order))
            return

        if self.state == "HEADER":
            self.hdr.append(self._syms_to_byte(syms, "msb"))
            if len(self.hdr) == 4:
                ver = (self.hdr[0] >> 4) & 0x0F
                ord_code = self.hdr[0] & 0x03
                self.payload_len = self.hdr[3]
                if ver != 0x1 or ord_code > 2 or self.payload_len > 240:
                    self.reset_deframer()
                    return
                self.payload_order = ["msb", "lsb_bit", "lsb_dibit"][ord_code]
                self.payload = []
                self.crc_bytes = []
                self.state = "PAYLOAD" if self.payload_len > 0 else "CRC"
            return

        if self.state == "PAYLOAD":
            self.payload.append(self._syms_to_byte(syms, self.payload_order))
            if len(self.payload) >= self.payload_len:
                self.state = "CRC"
            return

        if self.state == "CRC":
            self.crc_bytes.append(self._syms_to_byte(syms, "msb"))
            if len(self.crc_bytes) == 2:
                rx_crc = (self.crc_bytes[0] << 8) | self.crc_bytes[1]
                calc_crc = ref.crc16_ccitt_false(bytes(self.hdr + self.payload))
                self.recovered_frames.append((bytes(self.payload), rx_crc == calc_crc,
                                              self.locked_rot * 90, self.last_df_hz, self.last_snr_db))
                self.reset_deframer()

    def process_sample(self, sample_mv: float) -> None:
        if not self.dc_primed:
            self.dc_est_mv = sample_mv
            self.dc_primed = True
        alpha = 0.0010 if self.signal_active else 0.0050
        self.dc_est_mv += alpha * (sample_mv - self.dc_est_mv)
        x_ac = sample_mv - self.dc_est_mv

        self.pwr_ema = 0.98 * self.pwr_ema + 0.02 * (x_ac * x_ac)
        rms = math.sqrt(self.pwr_ema)
        self.amp_pk_est_mv = 1.41421356 * rms

        if not self.signal_active:
            if rms >= self.squelch_rms_mv:
                self.active_cnt += 1
                if self.active_cnt >= 8:
                    self.signal_active = True
                    self.quiet_cnt = 0
                    self.reset_tracking()
            else:
                self.active_cnt = 0
            if not self.signal_active:
                return
        else:
            if rms < 0.45 * self.squelch_rms_mv:
                self.quiet_cnt += 1
                if self.quiet_cnt >= self.quiet_limit:
                    self.signal_active = False
                    self.active_cnt = 0
                    self.reset_deframer()
                    return
            else:
                self.quiet_cnt = 0

        x_norm = x_ac / self.amp_pk_est_mv if self.amp_pk_est_mv > 10.0 else x_ac * 0.1
        sin_n = math.sin(self.nco_phase)
        cos_n = math.cos(self.nco_phase)
        i_mix = 2.0 * x_norm * cos_n
        q_mix = 2.0 * x_norm * (-sin_n if self.q_sign_minus else +sin_n)

        bi = self.box_idx
        self.box_sum_i += i_mix - self.box_i[bi]
        self.box_sum_q += q_mix - self.box_q[bi]
        self.box_i[bi] = i_mix
        self.box_q[bi] = q_mix
        self.box_idx = (bi + 1) % self.box_len

        i_b = self.box_sum_i / self.box_len
        q_b = self.box_sum_q / self.box_len

        self.costas_decim += 1
        if self.costas_decim >= self.box_len:
            self.costas_decim = 0
            mag2 = i_b * i_b + q_b * q_b
            if mag2 > 0.35:
                inv_mag = 1.0 / math.sqrt(mag2)
                iu, qu = i_b * inv_mag, q_b * inv_mag
                sg_i = 1.0 if iu >= 0.0 else -1.0
                sg_q = 1.0 if qu >= 0.0 else -1.0
                ped = ((sg_i * qu) - (sg_q * iu)) if self.q_sign_minus else ((sg_q * iu) - (sg_i * qu))
                self.last_ped = ped
                self.nco_phase += self.costas_kp * ped
                self.costas_freq += (self.costas_ki * ped) / self.box_len
                self.costas_freq = max(-self.costas_freq_max, min(self.costas_freq_max, self.costas_freq))
                self.last_df_hz = (self.costas_freq / (2.0 * math.pi)) * self.fs

        self.nco_phase = (self.nco_phase + self.nco_omega0 + self.costas_freq) % (2.0 * math.pi)

        sym_inc = self.sym_inc_nom + self.sym_rate_corr
        prev_sp = self.sym_phase
        if prev_sp < 0.5 <= (prev_sp + sym_inc):
            self.mid_i_curr, self.mid_q_curr = i_b, q_b
        if 0.15 <= prev_sp <= 0.85:
            self.acc_i += i_b
            self.acc_q += q_b
            self.acc_cnt += 1

        next_sp = prev_sp + sym_inc
        if next_sp >= 1.0:
            next_sp -= 1.0
            i_hat = (self.acc_i / self.acc_cnt) if self.acc_cnt > 0 else self.mid_i_curr
            q_hat = (self.acc_q / self.acc_cnt) if self.acc_cnt > 0 else self.mid_q_curr

            if self.has_prev_mid and self.has_curr_bnd:
                e_tau = (self.mid_i_curr - self.mid_i_prev) * self.bnd_i_curr + (self.mid_q_curr - self.mid_q_prev) * self.bnd_q_curr
                e_tau = max(-2.0, min(2.0, e_tau))
                self.last_gardner_err = e_tau
                next_sp += self.gardner_kp * e_tau
                self.sym_rate_corr += self.gardner_ki * e_tau * self.sym_inc_nom
                self.sym_rate_corr = max(-0.02 * self.sym_inc_nom, min(0.02 * self.sym_inc_nom, self.sym_rate_corr))
                next_sp = max(-0.25, min(0.80, next_sp))

            self.mid_i_prev, self.mid_q_prev = self.mid_i_curr, self.mid_q_curr
            self.has_prev_mid = True
            self.bnd_i_curr, self.bnd_q_curr = i_b, q_b
            self.has_curr_bnd = True
            self.acc_i = self.acc_q = 0.0
            self.acc_cnt = 0

            ei = abs(i_hat) - 0.70710678
            eq = abs(q_hat) - 0.70710678
            evm2 = ei * ei + eq * eq
            self.last_snr_db = 10.0 * math.log10(1.0 / evm2) if evm2 > 1e-5 else 40.0
            self._push_symbol(i_hat, q_hat)

        self.sym_phase = next_sp


def run_test_1_to_5() -> None:
    # Teste 2 & 3: Portadora e 4 simbolos via LUT C
    for q_minus in (False, True):
        for map_name in ("natural", "gray"):
            tx = TxModulatorC(map_name=map_name, q_sign_minus=q_minus)
            for sym in range(4):
                tx.carrier_phase_num = 0
                xc = xs = 0.0
                for n in range(40):
                    v, _ = tx.next_symbol_mv(sym)
                    th = 2.0 * math.pi * n / 40.0
                    ac = v - 1300.0
                    xc += ac * math.cos(th)
                    xs += ac * ((-math.sin(th)) if q_minus else math.sin(th))
                i_meas = (2.0 / 40.0) * xc / (900.0 / math.sqrt(2.0))
                q_meas = (2.0 / 40.0) * xs / (900.0 / math.sqrt(2.0))
                _, exp_i, exp_q, _ = ref.CONSTELLATIONS[map_name][sym]
                assert abs(i_meas - exp_i) < 0.01 and abs(q_meas - exp_q) < 0.01

    # Teste 4: Round-trip ASCII 0x00..0xFF nas 3 ordens
    for order in ("msb", "lsb_bit", "lsb_dibit"):
        for b in range(256):
            syms = ref.byte_to_symbols(b, order)
            rec = RxDemodAndDeframerC._syms_to_byte(syms, order)
            assert rec == b

    # Teste 5: CRC-16 e tamanhos Modo 1 / Modo 2
    assert ref.crc16_ccitt_false(b"123456789") == 0x29B1
    f2 = ref.build_mode2_frame(b"XYZ")
    assert len(f2["symbols"]) == 60
    print("[PASS] Testes 1..5: LUT 4-QAM (Natural/Gray, +sin/-sin), 256 bytes x 3 ordens, CRC=0x29B1, Enquadramento.")


def run_test_6_pwm_rc_physics() -> Tuple[float, float]:
    """Teste 6: Simula 80 kHz PWM -> Filtro RC 2a Ordem (1k/33nF + 10k/3.3nF) com EDO analitica exata."""
    step_rc = exact_rc2_stepper(1000.0, 33e-9, 10000.0, 3.3e-9)
    tx = TxModulatorC()
    t_pwm = 1.0 / 80000.0
    v1 = v2 = 1300.0  # mV em regime DC
    v_peak_min = 9999.0
    v_peak_max = -9999.0
    max_ripple_pp = 0.0

    # Portadora pura por 10 ms (800 periodos PWM = 10 ciclos de 1 kHz)
    # Mede DC residual/ripple comparando V2 vs ideal e variacao intraciclo PWM em DC puro
    for k in range(800):
        if (k % 2) == 0:
            cmp_ticks = tx.mv_to_cmp(tx.next_carrier_mv())
        duty = cmp_ticks / 1000.0
        t_high = duty * t_pwm
        t_half_low = 0.5 * (t_pwm - t_high)
        v1, v2 = step_rc(v1, v2, 0.0, t_half_low)
        v1, v2 = step_rc(v1, v2, 3300.0, t_high)
        v1, v2 = step_rc(v1, v2, 0.0, t_half_low)
        if k >= 400:  # regime permanente apos 5 ms
            v_peak_min = min(v_peak_min, v2)
            v_peak_max = max(v_peak_max, v2)

    # Ripple pior caso em DC constante (duty 50% -> cmp=500 -> Vdd/2 = 1650 mV) medido em 64 sub-passos
    v1_r = v2_r = 1650.0
    substeps = 64
    dt_sub = t_pwm / substeps
    for k in range(200):
        vals = []
        for s_i in range(substeps):
            vin = 3300.0 if (substeps // 4 <= s_i < (3 * substeps) // 4) else 0.0
            v1_r, v2_r = step_rc(v1_r, v2_r, vin, dt_sub)
            vals.append(v2_r)
        if k >= 100:
            max_ripple_pp = max(max_ripple_pp, max(vals) - min(vals))

    vpk_out = 0.5 * (v_peak_max - v_peak_min)
    assert 830.0 <= vpk_out <= 880.0, f"Vpk filtro fora do esperado (-0.44 dB -> ~855 mV): {vpk_out:.2f}"
    assert max_ripple_pp <= 20.0, f"Ripple PWM acima de 20 mVpp: {max_ripple_pp:.2f}"
    print(f"[PASS] Teste 6 (Fisica PWM 80kHz + RC 2a Ordem): Vpk={vpk_out:.1f} mV (esperado ~855 mV, -0.44dB) | Ripple 80kHz={max_ripple_pp:.2f} mVpp.")
    return vpk_out, max_ripple_pp


def run_test_7_full_e2e_pwm_to_rx() -> None:
    """Teste 7: Transmissao real via PWM 80kHz -> Filtro RC 2a Ordem -> ADC 12 bits @ 20kHz -> RX DSP."""
    scenarios = [
        ("4QAM-OK!", "natural", False, "msb", 1000.40, 100.15, 73.0),
        ("Teste 4QAM ESP32!", "natural", False, "msb", 999.50, 99.85, 212.0),
        ("ESP32-RX", "gray", True, "lsb_dibit", 1000.30, 100.00, 140.0),
    ]

    step_rc = exact_rc2_stepper(1000.0, 33e-9, 10000.0, 3.3e-9)

    for text, map_name, q_minus, order, fc_tx, rs_tx, ph0_deg in scenarios:
        frame = ref.build_mode2_frame(text.encode("ascii"), baud=100, bit_order=order)
        syms = frame["symbols"]

        # TX gera em PWM 80 kHz com f_c e R_s com deriva fisica simulada
        fs_tx_pwm = 80000.0
        t_pwm = 1.0 / fs_tx_pwm
        a_mv = 900.0 / math.sqrt(2.0)
        table = ref.CONSTELLATIONS[map_name]
        ph0_rad = math.radians(ph0_deg)

        rx = RxDemodAndDeframerC(fc_hz=1000, baud=100, fs_hz=20000,
                                 map_name=map_name, q_sign_minus=q_minus, mode=2)

        v1 = v2 = 1300.0
        # 30 ms silencio inicial (DC = 1300 mV) + quadro completo + 15 ms cauda
        silence_pwms = int(0.030 * fs_tx_pwm)
        frame_pwms = int((len(syms) / rs_tx) * fs_tx_pwm)
        tail_pwms = int(0.015 * fs_tx_pwm)
        total_pwms = silence_pwms + frame_pwms + tail_pwms

        adc_subdiv = 4  # 80 kHz / 4 = 20 kHz ADC1 DMA!
        t_offset_frac = 0.37 / 20000.0  # deslocamento fracionario de clock
        rng_state = 0x12345678

        for k in range(total_pwms):
            # Amostra TX a 40 kHz (a cada 2 ciclos PWM)
            if (k % 2) == 0:
                t = ((k - silence_pwms) * t_pwm) + t_offset_frac
                if k < silence_pwms or k >= (silence_pwms + frame_pwms):
                    v_ideal = 1300.0
                else:
                    s_idx = min(len(syms) - 1, max(0, int(t * rs_tx)))
                    _, iv, qv, _ = table[syms[s_idx]]
                    th = 2.0 * math.pi * fc_tx * t + ph0_rad
                    q_term = -(qv * math.sin(th)) if q_minus else +(qv * math.sin(th))
                    v_ideal = 1300.0 + a_mv * (iv * math.cos(th) + q_term)
                cmp_ticks = max(1, min(999, int((v_ideal / 3300.0) * 1000.0 + 0.5)))

            duty = cmp_ticks / 1000.0
            t_high = duty * t_pwm
            t_low = 0.5 * (t_pwm - t_high)
            v1, v2 = step_rc(v1, v2, 0.0, t_low)
            v1, v2 = step_rc(v1, v2, 3300.0, t_high)
            v1, v2 = step_rc(v1, v2, 0.0, t_low)

            # A cada 4 ciclos PWM = 20 kHz, o ADC1 (12 bits, Vref ~2450 mV util, ruido) amostra v2!
            if (k % adc_subdiv) == 0:
                rng_state = (1103515245 * rng_state + 12345) & 0x7FFFFFFF
                noise_mv = (((rng_state >> 16) & 0xFF) - 128) * 0.12
                v_noisy = max(0.0, min(2600.0, v2 + noise_mv))
                # Quantizacao ADC 12 bits (~0.6 mV/LSB na faixa 0..2450 mV)
                adc_code = max(0, min(4095, int((v_noisy / 2500.0) * 4095.0 + 0.5)))
                v_adc_mv = (adc_code / 4095.0) * 2500.0
                rx.process_sample(v_adc_mv)

        assert len(rx.recovered_frames) == 1, f"Quadro nao recuperado em {text}: {rx.recovered_frames}"
        payload_rx, crc_ok, rot_deg, df_hz, snr_db = rx.recovered_frames[0]
        assert crc_ok and payload_rx.decode("ascii") == text, f"Falha decodificacao: {payload_rx}, crc={crc_ok}"
        print(f"[PASS] Teste 7 E2E (PWM->RC->ADC20kHz->RX): \"{text}\" | map={map_name}, q_minus={q_minus}, "
              f"order={order}, rot={rot_deg}deg, df_est={df_hz:+.2f}Hz, SNR={snr_db:.1f}dB, CRC=OK")

    # Teste adicional Modo 1 com sync1=1 ("AB")
    rx_m1 = RxDemodAndDeframerC(mode=1, mode1_use_sync=True)
    tx_m1 = TxModulatorC()
    m1_stream = b"\x00\x00\x33\x33\xD3\x91AB"
    syms_m1 = []
    for b in m1_stream:
        syms_m1.extend(ref.byte_to_symbols(b, "msb"))
    for _ in range(400):
        rx_m1.process_sample(1300.0)
    for s in syms_m1:
        for idx in range(400):
            v, _ = tx_m1.next_symbol_mv(s)
            if (idx % 2) == 0:  # 40 kHz -> 20 kHz
                rx_m1.process_sample(v)
    for _ in range(200):  # 10 ms de retorno ao silencio DC para encerrar janela do ultimo simbolo
        rx_m1.process_sample(1300.0)
    assert bytes(rx_m1.raw_bytes) == b"AB", f"Modo 1 falhou: {rx_m1.raw_bytes}"
    print("[PASS] Teste 7b Modo 1 (Professor c/ Sync): fluxo \"AB\" recuperado exatamente (4 simbolos/byte).")


def run_test_8_check_csv() -> None:
    csv_path = os.path.join(ROOT_DIR, "docs", "vetores_teste", "vetores_amostras_4qam.csv")
    with open(csv_path, encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == 3200, f"Esperadas 3200 linhas, encontradas {len(rows)}"
    v_vals = [float(r["v_ideal_mv"]) for r in rows]
    c_vals = [int(r["pwm_cmp_ticks"]) for r in rows]
    assert 399.9 <= min(v_vals) <= 400.1 and 2199.9 <= max(v_vals) <= 2200.1
    assert 121 <= min(c_vals) and max(c_vals) <= 667
    print(f"[PASS] Teste 8 CSV: {len(rows)} amostras conferidas ([{min(v_vals):.2f} .. {max(v_vals):.2f}] mV, PWM [{min(c_vals)}..{max(c_vals)}]).")


def main() -> None:
    print("=== VALIDACAO PONTA A PONTA DO SISTEMA 4-QAM (SIMULACAO DSP + EDO FILTRO RC) ===")
    run_test_1_to_5()
    run_test_6_pwm_rc_physics()
    run_test_7_full_e2e_pwm_to_rx()
    run_test_8_check_csv()
    print("=== RESULTADO FINAL SIMULACAO E2E: TODOS OS TESTES PASSARAM ===")


if __name__ == "__main__":
    main()
