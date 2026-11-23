#include "rx_selftest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qam_demod.h"
#include "rx_config.h"
#include "rx_deframer.h"
#include "rx_mapper.h"

#define TEST_TWO_PI 6.2831853071795864769

static void byte_to_syms_msb(uint8_t b, uint8_t out[4])
{
    out[0] = (uint8_t)((b >> 6U) & 0x03U);
    out[1] = (uint8_t)((b >> 4U) & 0x03U);
    out[2] = (uint8_t)((b >> 2U) & 0x03U);
    out[3] = (uint8_t)((b >> 0U) & 0x03U);
}

static bool test_closed_loop_mode2(bool q_sign_minus, float phase_offset_deg,
                                   float freq_offset_hz, float sym_rate_err_ratio,
                                   bool verbose)
{
    qam_rx_config_t *cfg = (qam_rx_config_t *)calloc(1U, sizeof(qam_rx_config_t));
    qam_demod_t *demod = (qam_demod_t *)calloc(1U, sizeof(qam_demod_t));
    rx_deframer_t *df = (rx_deframer_t *)calloc(1U, sizeof(rx_deframer_t));
    rx_deframer_event_t *ev = (rx_deframer_event_t *)calloc(1U, sizeof(rx_deframer_event_t));
    if (!cfg || !demod || !df || !ev) {
        free(ev);
        free(df);
        free(demod);
        free(cfg);
        return false;
    }

    rx_config_set_defaults(cfg);
    cfg->q_sign_minus = q_sign_minus;
    cfg->rx_mode      = RX_MODE_2_FRAMED;

    if (!qam_demod_init(demod, cfg)) {
        free(ev);
        free(df);
        free(demod);
        free(cfg);
        return false;
    }
    rx_deframer_init(df, cfg);

    /* Monta quadro Modo 2 sintético idêntico ao transmissor */
    const char *msg = "4QAM-OK!";
    const size_t msg_len = strlen(msg);
    uint8_t frame_bytes[32];
    size_t fb_len = 0U;

    frame_bytes[fb_len++] = 0x00U;
    frame_bytes[fb_len++] = 0x00U;
    frame_bytes[fb_len++] = 0x33U;
    frame_bytes[fb_len++] = 0x33U;
    frame_bytes[fb_len++] = cfg->sync_word[0];
    frame_bytes[fb_len++] = cfg->sync_word[1];

    uint8_t hdr[4] = {
        0x10U, /* v1, 4-QAM, MSB-first */
        (uint8_t)((cfg->symbol_rate_baud >> 8U) & 0xFFU),
        (uint8_t)(cfg->symbol_rate_baud & 0xFFU),
        (uint8_t)msg_len
    };
    memcpy(&frame_bytes[fb_len], hdr, 4U);
    fb_len += 4U;

    memcpy(&frame_bytes[fb_len], msg, msg_len);
    fb_len += msg_len;

    uint16_t crc = rx_crc16_ccitt(&frame_bytes[6U], 4U + msg_len);
    frame_bytes[fb_len++] = (uint8_t)((crc >> 8U) & 0xFFU);
    frame_bytes[fb_len++] = (uint8_t)(crc & 0xFFU);

    uint8_t syms[128];
    size_t total_syms = fb_len * 4U;
    for (size_t b = 0U; b < fb_len; b++) {
        byte_to_syms_msb(frame_bytes[b], &syms[b * 4U]);
    }

    /* 1. Silêncio DC inicial (30 ms = 600 amostras) */
    const double dc_mv = 1320.0;
    const double amp_a = 620.0 / sqrt(2.0); /* Sinal atenuado (~620 mV pk) */
    qam_demod_out_t dout;

    for (size_t n = 0U; n < 600U; n++) {
        qam_demod_process_sample(demod, (float)dc_mv, &dout);
    }

    /* 2. Canal com fase inicial, desvio de portadora e deriva de relógio de símbolo */
    const double fs = (double)cfg->sample_rate_hz;
    const double fc_tx = (double)cfg->carrier_freq_hz + (double)freq_offset_hz;
    const double rs_tx = (double)cfg->symbol_rate_baud * (1.0 + (double)sym_rate_err_ratio);
    const double phase0_rad = (double)phase_offset_deg * (TEST_TWO_PI / 360.0);

    /* Deslocamento fracionário inicial de 37 amostras para testar recuperação Gardner */
    const size_t total_tx_samples = (size_t)(((double)total_syms / rs_tx) * fs) + 40U;
    bool recovered = false;

    for (size_t n = 0U; n < total_tx_samples; n++) {
        double t = ((double)n + 0.37) / fs;
        size_t s_idx = (size_t)(t * rs_tx);
        double sample = dc_mv;

        if (s_idx < total_syms) {
            uint8_t s = syms[s_idx];
            int8_t iv = cfg->constellation[s].i_val;
            int8_t qv = cfg->constellation[s].q_val;
            double theta = TEST_TWO_PI * fc_tx * t + phase0_rad;
            double q_part = q_sign_minus ? -((double)qv * sin(theta)) : +((double)qv * sin(theta));
            double noise = (((int)((n * 1103515245U + 12345U) >> 16U) & 0xFF) - 128) * 0.12;
            sample = dc_mv + amp_a * ((double)iv * cos(theta) + q_part) + noise;
        }

        qam_demod_process_sample(demod, (float)sample, &dout);
        if (dout.symbol_ready) {
            rx_deframer_push_symbol(df, dout.i_hat, dout.q_hat, ev);
            if (ev->frame_ready && ev->crc_ok && strcmp((const char *)ev->payload, msg) == 0) {
                recovered = true;
            }
        }
    }

    float df_est_hz = demod->freq_offset_hz;
    free(ev);
    free(df);
    free(demod);
    free(cfg);

    if (!recovered) {
        printf("[FAIL] Teste RX (q_minus=%u, phase=%.0f, df=%.2fHz, dRs=%.4f): nao recuperou quadro\n",
               (unsigned)q_sign_minus, phase_offset_deg, freq_offset_hz, sym_rate_err_ratio);
        return false;
    }

    if (verbose) {
        printf("  [OK] Malha fechada (phase=%.0f deg, df=%+.2f Hz, dRs=%+.1f%%): \"%s\" CRC=0x%04X (df_est=%+.2f Hz)\n",
               phase_offset_deg, freq_offset_hz, sym_rate_err_ratio * 100.0f,
               msg, (unsigned)crc, df_est_hz);
    }
    return true;
}

bool rx_selftest_run_all(bool verbose)
{
    printf("=== AUTO-TESTE 4-QAM RECEPTOR (MALHA FECHADA) ===\n");
    bool ok = true;
    ok &= test_closed_loop_mode2(false,  73.0f, +0.40f, +0.0015f, verbose);
    ok &= test_closed_loop_mode2(false, 212.0f, -0.50f, -0.0015f, verbose);
    ok &= test_closed_loop_mode2(true,  140.0f, +0.30f,  0.0000f, verbose);
    printf("=== RESULTADO AUTO-TESTE RX: %s ===\n", ok ? "TODOS PASSARAM" : "FALHOU");
    return ok;
}
