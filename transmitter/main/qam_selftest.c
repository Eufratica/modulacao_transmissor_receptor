#include "qam_selftest.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "qam_config.h"
#include "qam_framer.h"
#include "qam_mapper.h"
#include "qam_modulator.h"

#define TWO_PI_D 6.2831853071795864769
#define PI_HALF_D 1.5707963267948966192

static bool test_mapping_table(bool verbose)
{
    qam_tx_config_t cfg;
    qam_config_set_defaults(&cfg);

    /* Verifica tabela padrão Natural: 0=(-1,-1), 1=(-1,+1), 2=(+1,-1), 3=(+1,+1) */
    const struct {
        uint8_t sym;
        int8_t exp_i;
        int8_t exp_q;
    } expected[4] = {
        { 0U, -1, -1 }, /* 00 */
        { 1U, -1, +1 }, /* 01 */
        { 2U, +1, -1 }, /* 10 */
        { 3U, +1, +1 }, /* 11 */
    };

    for (size_t k = 0; k < 4U; k++) {
        qam_symbol_point_t pt;
        if (!qam_mapper_get_point(&cfg, expected[k].sym, &pt)) {
            printf("[FAIL] Teste 1 (Mapeamento): falha ao consultar simbolo %u\n", (unsigned)expected[k].sym);
            return false;
        }
        if (pt.i_val != expected[k].exp_i || pt.q_val != expected[k].exp_q) {
            printf("[FAIL] Teste 1 (Mapeamento): simbolo %u divergiu (I=%d Q=%d)\n",
                   (unsigned)expected[k].sym, (int)pt.i_val, (int)pt.q_val);
            return false;
        }
        if (verbose) {
            printf("  [OK] Simbolo %u (%s) -> I=%+d, Q=%+d (plano IQ=%u deg)\n",
                   (unsigned)expected[k].sym, pt.bits_label,
                   (int)pt.i_val, (int)pt.q_val, (unsigned)pt.phase_deg);
        }
    }
    return true;
}

static bool test_carrier_generation(bool verbose)
{
    qam_tx_config_t cfg;
    qam_config_set_defaults(&cfg);

    qam_modulator_t mod;
    if (!qam_modulator_init(&mod, &cfg, 1000U)) {
        printf("[FAIL] Teste 2 (Portadora): falha ao inicializar modulador\n");
        return false;
    }

    const uint32_t samples_per_cycle = cfg.sample_rate_hz / cfg.carrier_freq_hz; /* 40 */
    double sum_mv = 0.0;
    double min_mv = 1e9;
    double max_mv = -1e9;
    double xc = 0.0;
    double xs = 0.0;

    for (uint32_t n = 0; n < samples_per_cycle; n++) {
        float v = qam_modulator_next_carrier_mv(&mod);
        double theta = TWO_PI_D * ((double)n / (double)samples_per_cycle);
        sum_mv += v;
        if (v < min_mv) min_mv = v;
        if (v > max_mv) max_mv = v;
        double ac = (double)v - (double)cfg.dc_offset_mv;
        xc += ac * cos(theta);
        xs += ac * cos(theta - PI_HALF_D);
    }

    double mean_mv = sum_mv / (double)samples_per_cycle;
    double amp_est = (2.0 / (double)samples_per_cycle) * hypot(xc, xs);
    double expected_min = (double)cfg.dc_offset_mv - (double)cfg.amplitude_peak_mv;
    double expected_max = (double)cfg.dc_offset_mv + (double)cfg.amplitude_peak_mv;

    if (fabs(mean_mv - (double)cfg.dc_offset_mv) > 1.0 ||
        fabs(amp_est - (double)cfg.amplitude_peak_mv) > 2.0 ||
        fabs(min_mv - expected_min) > 2.0 ||
        fabs(max_mv - expected_max) > 2.0) {
        printf("[FAIL] Teste 2 (Portadora): media=%.2f mV, amp_dft=%.2f mV, min=%.2f, max=%.2f\n",
               mean_mv, amp_est, min_mv, max_mv);
        return false;
    }

    if (verbose) {
        printf("  [OK] Portadora %u Hz: DC=%.2f mV | Amp=%.2f mV | Faixa=[%.1f .. %.1f] mV\n",
               (unsigned)cfg.carrier_freq_hz, mean_mv, amp_est, min_mv, max_mv);
    }
    return true;
}

static bool test_4qam_quadrature_for_mode(bool q_sign_minus, qam_map_preset_t preset, bool verbose)
{
    qam_tx_config_t cfg;
    qam_config_set_defaults(&cfg);
    cfg.q_sign_minus = q_sign_minus;
    qam_config_apply_map_preset(&cfg, preset);

    qam_modulator_t mod;
    if (!qam_modulator_init(&mod, &cfg, 1000U)) {
        printf("[FAIL] Teste 3 (4-QAM): falha ao inicializar modulador\n");
        return false;
    }

    const uint32_t samples_per_cycle = cfg.sample_rate_hz / cfg.carrier_freq_hz; /* 40 */
    const double a_eq = (double)cfg.amplitude_peak_mv / sqrt(2.0);

    for (uint8_t s = 0U; s < 4U; s++) {
        qam_modulator_reset_phase(&mod);

        double xc = 0.0;
        double xs = 0.0;
        double min_mv = 1e9;
        double max_mv = -1e9;

        for (uint32_t n = 0; n < samples_per_cycle; n++) {
            bool sym_done = false;
            float v = qam_modulator_next_symbol_mv(&mod, s, &sym_done);
            if (v < min_mv) min_mv = v;
            if (v > max_mv) max_mv = v;

            double ac = (double)v - (double)cfg.dc_offset_mv;
            double theta = TWO_PI_D * ((double)n / (double)samples_per_cycle);
            /* Demodulação em quadratura:
             * Ramo I correlaciona com cos(theta)
             * Ramo Q correlaciona com cos(theta - 90°) = +sin(theta) se q_sign_minus==false,
             * ou com -sin(theta) se q_sign_minus==true. */
            double ref_q = q_sign_minus ? -sin(theta) : cos(theta - PI_HALF_D);
            xc += ac * cos(theta);
            xs += ac * ref_q;
        }

        double i_meas = ((2.0 / (double)samples_per_cycle) * xc) / a_eq;
        double q_meas = ((2.0 / (double)samples_per_cycle) * xs) / a_eq;
        double phase_iq = atan2(q_meas, i_meas) * (180.0 / 3.14159265358979323846);
        if (phase_iq < 0.0) {
            phase_iq += 360.0;
        }

        const qam_symbol_point_t *pt = &cfg.constellation[s];
        if (fabs(i_meas - (double)pt->i_val) > 0.01 ||
            fabs(q_meas - (double)pt->q_val) > 0.01 ||
            min_mv < 50.0 || max_mv > (double)(cfg.vdd_mv - 50U)) {
            printf("[FAIL] Teste 3 (4-QAM): sym=%u(%s) I=%.3f Q=%.3f\n",
                   (unsigned)s, pt->bits_label, i_meas, q_meas);
            return false;
        }

        if (verbose) {
            printf("  [OK] [%s | %s] Simb %u(%s): I_med=%+.3f Q_med=%+.3f IQ=%.1f deg | V=[%.1f..%.1f] mV\n",
                   (preset == QAM_MAP_NATURAL) ? "NAT" : "GRAY",
                   q_sign_minus ? "-sin" : "+cos(-90)",
                   (unsigned)s, pt->bits_label, i_meas, q_meas, phase_iq, min_mv, max_mv);
        }
    }

    return true;
}

static bool test_ascii_and_bit_orders(bool verbose)
{
    const uint8_t msg[] = "AB4!";
    const size_t msg_len = sizeof(msg) - 1U;
    uint8_t syms[16];
    size_t sym_cnt = 0U;

    uint8_t a_syms[4];
    qam_mapper_byte_to_symbols((uint8_t)'A', QAM_BIT_ORDER_MSB_FIRST, a_syms);
    if (a_syms[0] != 1U || a_syms[1] != 0U || a_syms[2] != 0U || a_syms[3] != 1U) {
        printf("[FAIL] Teste 4 (ASCII): mapeamento de 'A' incorreto\n");
        return false;
    }

    const qam_bit_order_t orders[3] = {
        QAM_BIT_ORDER_MSB_FIRST,
        QAM_BIT_ORDER_LSB_BIT_FIRST,
        QAM_BIT_ORDER_LSB_DIBIT_FIRST,
    };

    for (size_t oi = 0; oi < 3U; oi++) {
        if (!qam_mapper_bytes_to_symbols(msg, msg_len, orders[oi], syms, sizeof(syms), &sym_cnt) ||
            sym_cnt != msg_len * 4U) {
            printf("[FAIL] Teste 4 (ASCII): falha na conversao em ordem %u\n", (unsigned)oi);
            return false;
        }
        for (size_t b = 0; b < msg_len; b++) {
            uint8_t rec = qam_mapper_symbols_to_byte(&syms[b * 4U], orders[oi]);
            if (rec != msg[b]) {
                printf("[FAIL] Teste 4 (ASCII): falha round-trip byte %u ordem %u\n", (unsigned)b, (unsigned)oi);
                return false;
            }
        }
    }

    if (verbose) {
        printf("  [OK] Conversao ASCII->Bits->Simbolos e round-trip validados nas 3 ordens de bits\n");
    }
    return true;
}

static bool test_framing_and_crc(bool verbose)
{
    const uint8_t crc_vec[] = "123456789";
    uint16_t crc = qam_crc16_ccitt(crc_vec, 9U);
    if (crc != 0x29B1U) {
        printf("[FAIL] Teste 5 (CRC-16): esperado 0x29B1, obtido 0x%04X\n", (unsigned)crc);
        return false;
    }

    qam_tx_config_t cfg;
    qam_config_set_defaults(&cfg);
    qam_tx_frame_t frame;

    cfg.tx_mode = QAM_TX_MODE_1_PROFESSOR;
    if (!qam_framer_build_frame(&cfg, (const uint8_t *)"XYZ", 3U, &frame) ||
        frame.symbol_count != 12U || frame.header_symbol_count != 0U) {
        printf("[FAIL] Teste 5 (Modo 1): contagem de simbolos inesperada (%u)\n", (unsigned)frame.symbol_count);
        return false;
    }

    cfg.tx_mode = QAM_TX_MODE_2_FRAMED;
    if (!qam_framer_build_frame(&cfg, (const uint8_t *)"XYZ", 3U, &frame) ||
        frame.symbol_count != 60U || frame.payload_symbol_count != 12U || frame.crc16 == 0U) {
        printf("[FAIL] Teste 5 (Modo 2): contagem de simbolos inesperada (%u)\n", (unsigned)frame.symbol_count);
        return false;
    }

    if (verbose) {
        printf("  [OK] CRC-16=0x29B1 | Modo 1 (3B->12 sym sem overhead) | Modo 2 (3B->60 sym c/ CRC 0x%04X)\n",
               (unsigned)frame.crc16);
    }
    return true;
}

static bool test_safety_validation(bool verbose)
{
    qam_tx_config_t cfg;
    qam_config_set_defaults(&cfg);

    if (!qam_config_validate(&cfg, NULL, 0U)) {
        printf("[FAIL] Teste 6 (Seguranca): configuracao padrao rejeitada\n");
        return false;
    }

    cfg.dc_offset_mv = 500U;
    cfg.amplitude_peak_mv = 600U;
    if (qam_config_validate(&cfg, NULL, 0U)) {
        printf("[FAIL] Teste 6 (Seguranca): deveria rejeitar dc_offset < amplitude\n");
        return false;
    }

    qam_config_set_defaults(&cfg);
    cfg.dc_offset_mv = 2500U;
    cfg.amplitude_peak_mv = 900U;
    if (qam_config_validate(&cfg, NULL, 0U)) {
        printf("[FAIL] Teste 6 (Seguranca): deveria rejeitar dc_offset + amp > vdd\n");
        return false;
    }

    qam_config_set_defaults(&cfg);
    cfg.pwm_freq_hz = 75000U;
    cfg.sample_rate_hz = 40000U;
    if (qam_config_validate(&cfg, NULL, 0U)) {
        printf("[FAIL] Teste 6 (Seguranca): deveria rejeitar pwm_freq nao multiplo de fs\n");
        return false;
    }

    if (verbose) {
        printf("  [OK] Validacao eletrica e deterministica rejeitou todas as configs inseguras\n");
    }
    return true;
}

bool qam_selftest_run_all(bool verbose)
{
    printf("=== AUTO-TESTE 4-QAM TRANSMISSOR ===\n");
    bool ok = true;
    ok &= test_mapping_table(verbose);
    ok &= test_carrier_generation(verbose);
    ok &= test_4qam_quadrature_for_mode(false, QAM_MAP_NATURAL, verbose);
    ok &= test_4qam_quadrature_for_mode(true, QAM_MAP_GRAY, false);
    ok &= test_ascii_and_bit_orders(verbose);
    ok &= test_framing_and_crc(verbose);
    ok &= test_safety_validation(verbose);
    printf("=== RESULTADO AUTO-TESTE: %s ===\n", ok ? "TODOS PASSARAM" : "FALHOU");
    return ok;
}
