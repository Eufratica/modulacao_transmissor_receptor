#include "qam_demod.h"

#include <math.h>
#include <string.h>

#define DEMOD_LUT_BITS   10U
#define DEMOD_LUT_SIZE   (1U << DEMOD_LUT_BITS)
#define DEMOD_LUT_MASK   (DEMOD_LUT_SIZE - 1U)
#define DEMOD_TWO_PI_F   6.2831853071795865f
#define DEMOD_INV_2PI_F  0.15915494309189534f

static float s_rx_sin_lut[DEMOD_LUT_SIZE + 1U];
static bool s_rx_lut_ready = false;

static void ensure_rx_lut(void)
{
    if (s_rx_lut_ready) {
        return;
    }
    for (uint32_t i = 0; i <= DEMOD_LUT_SIZE; i++) {
        s_rx_sin_lut[i] = sinf(DEMOD_TWO_PI_F * ((float)i / (float)DEMOD_LUT_SIZE));
    }
    s_rx_sin_lut[DEMOD_LUT_SIZE] = s_rx_sin_lut[0];
    s_rx_lut_ready = true;
}

static inline void lut_sin_cos_rad(float rad, float *out_sin, float *out_cos)
{
    float turns = rad * DEMOD_INV_2PI_F;
    if (turns < 0.0f) {
        turns += 1.0f;
    }
    float pos = turns * (float)DEMOD_LUT_SIZE;
    uint32_t idx = (uint32_t)pos;
    float frac = pos - (float)idx;

    idx &= DEMOD_LUT_MASK;
    float s0 = s_rx_sin_lut[idx];
    float s1 = s_rx_sin_lut[idx + 1U];
    *out_sin = s0 + frac * (s1 - s0);

    uint32_t cidx = (idx + (DEMOD_LUT_SIZE / 4U)) & DEMOD_LUT_MASK;
    float c0 = s_rx_sin_lut[cidx];
    float c1 = s_rx_sin_lut[cidx + 1U];
    *out_cos = c0 + frac * (c1 - c0);
}

static inline float clampf_val(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

void qam_demod_reset_tracking(qam_demod_t *demod)
{
    if (!demod) {
        return;
    }
    demod->nco_phase_rad    = 0.0f;
    demod->costas_freq_rad  = 0.0f;
    demod->costas_decim_cnt = 0U;

    memset(demod->box_i, 0, sizeof(demod->box_i));
    memset(demod->box_q, 0, sizeof(demod->box_q));
    demod->box_sum_i = 0.0f;
    demod->box_sum_q = 0.0f;
    demod->box_idx   = 0U;

    demod->sym_phase     = 0.0f;
    demod->sym_rate_corr = 0.0f;
    demod->mid_i_curr    = 0.0f;
    demod->mid_q_curr    = 0.0f;
    demod->mid_i_prev    = 0.0f;
    demod->mid_q_prev    = 0.0f;
    demod->has_prev_mid  = false;
    demod->bnd_i_curr    = 0.0f;
    demod->bnd_q_curr    = 0.0f;
    demod->has_curr_bnd  = false;
    demod->acc_i         = 0.0f;
    demod->acc_q         = 0.0f;
    demod->acc_cnt          = 0U;
    demod->freq_offset_hz   = 0.0f;
    demod->last_ped         = 0.0f;
    demod->last_gardner_err = 0.0f;
}

bool qam_demod_init(qam_demod_t *demod, const qam_rx_config_t *cfg)
{
    if (!demod || !cfg) {
        return false;
    }
    ensure_rx_lut();
    memset(demod, 0, sizeof(*demod));
    return qam_demod_reconfigure(demod, cfg);
}

bool qam_demod_reconfigure(qam_demod_t *demod, const qam_rx_config_t *cfg)
{
    if (!demod || !cfg || !rx_config_validate(cfg, NULL, 0U)) {
        return false;
    }
    ensure_rx_lut();
    demod->cfg = *cfg;

    demod->nco_omega0_rad = DEMOD_TWO_PI_F * ((float)cfg->carrier_freq_hz / (float)cfg->sample_rate_hz);
    /* Limite de rastreio de frequência da portadora em +/- 35 Hz */
    demod->costas_freq_max_rad = DEMOD_TWO_PI_F * (35.0f / (float)cfg->sample_rate_hz);

    uint32_t blen = (cfg->sample_rate_hz + (cfg->carrier_freq_hz / 2U)) / cfg->carrier_freq_hz;
    if (blen < 4U) {
        blen = 4U;
    } else if (blen > QAM_DEMOD_MAX_BOXCAR) {
        blen = QAM_DEMOD_MAX_BOXCAR;
    }
    demod->box_len = blen;

    demod->sym_inc_nom = (float)cfg->symbol_rate_baud / (float)cfg->sample_rate_hz;
    demod->quiet_limit_samples = (uint32_t)(2.5f * ((float)cfg->sample_rate_hz / (float)cfg->symbol_rate_baud));
    if (demod->quiet_limit_samples < 40U) {
        demod->quiet_limit_samples = 40U;
    }

    demod->signal_active = false;
    demod->active_cnt    = 0U;
    demod->quiet_cnt     = 0U;
    qam_demod_reset_tracking(demod);
    return true;
}

void qam_demod_process_sample(qam_demod_t *demod, float sample_mv, qam_demod_out_t *out)
{
    if (!demod || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /* 1. Estimador adaptativo e remoção do offset DC */
    if (!demod->dc_primed) {
        demod->dc_est_mv = sample_mv;
        demod->dc_primed = true;
    }
    float alpha_dc = demod->signal_active ? 0.0010f : 0.0050f;
    demod->dc_est_mv += alpha_dc * (sample_mv - demod->dc_est_mv);

    float x_ac = sample_mv - demod->dc_est_mv;
    if (demod->cfg.invert_polarity) {
        x_ac = -x_ac;
    }

    /* 2. Detector de energia RMS (squelch com histerese) e AGC */
    const float beta_pwr = 0.02f;
    demod->pwr_ema = ((1.0f - beta_pwr) * demod->pwr_ema) + (beta_pwr * x_ac * x_ac);
    float rms_mv = sqrtf(demod->pwr_ema);
    demod->amp_pk_est_mv = 1.41421356f * rms_mv;

    if (!demod->signal_active) {
        if (rms_mv >= demod->cfg.squelch_rms_mv) {
            demod->active_cnt++;
            if (demod->active_cnt >= 8U) {
                demod->signal_active = true;
                demod->quiet_cnt = 0U;
                qam_demod_reset_tracking(demod);
                out->burst_started = true;
            }
        } else {
            demod->active_cnt = 0U;
        }
        if (!demod->signal_active) {
            return;
        }
    } else {
        if (rms_mv < (0.45f * demod->cfg.squelch_rms_mv)) {
            demod->quiet_cnt++;
            if (demod->quiet_cnt >= demod->quiet_limit_samples) {
                demod->signal_active = false;
                demod->active_cnt = 0U;
                out->burst_ended = true;
                return;
            }
        } else {
            demod->quiet_cnt = 0U;
        }
    }

    float norm_scale = (demod->amp_pk_est_mv > 10.0f) ? (1.0f / demod->amp_pk_est_mv) : 0.1f;
    float x_norm = x_ac * norm_scale;

    /* 3. Mistura em quadratura + filtro passa-baixas boxcar de 1 ciclo de portadora */
    float sin_nco = 0.0f;
    float cos_nco = 0.0f;
    lut_sin_cos_rad(demod->nco_phase_rad, &sin_nco, &cos_nco);

    float i_mix = 2.0f * x_norm * cos_nco;
    float q_mix = 2.0f * x_norm * (demod->cfg.q_sign_minus ? -sin_nco : sin_nco);

    uint32_t bidx = demod->box_idx;
    demod->box_sum_i += i_mix - demod->box_i[bidx];
    demod->box_sum_q += q_mix - demod->box_q[bidx];
    demod->box_i[bidx] = i_mix;
    demod->box_q[bidx] = q_mix;
    bidx++;
    if (bidx >= demod->box_len) {
        bidx = 0U;
    }
    demod->box_idx = bidx;

    float inv_blen = 1.0f / (float)demod->box_len;
    float i_b = demod->box_sum_i * inv_blen;
    float q_b = demod->box_sum_q * inv_blen;

    /* 4. Costas Loop PI de 2ª ordem (atualizado 1x por ciclo de portadora) */
    demod->costas_decim_cnt++;
    if (demod->costas_decim_cnt >= demod->box_len) {
        demod->costas_decim_cnt = 0U;
        float mag2 = (i_b * i_b) + (q_b * q_b);
        if (mag2 > 0.35f) {
            float inv_mag = 1.0f / sqrtf(mag2);
            float iu = i_b * inv_mag;
            float qu = q_b * inv_mag;
            float sgn_i = (iu >= 0.0f) ? 1.0f : -1.0f;
            float sgn_q = (qu >= 0.0f) ? 1.0f : -1.0f;

            /* Sinal do PED ajustado conforme a convenção de Q (+sin vs -sin) */
            float ped = demod->cfg.q_sign_minus
                        ? ((sgn_i * qu) - (sgn_q * iu))
                        : ((sgn_q * iu) - (sgn_i * qu));
            demod->last_ped = ped;

            demod->nco_phase_rad += demod->cfg.costas_kp * ped;
            demod->costas_freq_rad += (demod->cfg.costas_ki * ped) / (float)demod->box_len;
            demod->costas_freq_rad = clampf_val(demod->costas_freq_rad,
                                                -demod->costas_freq_max_rad,
                                                demod->costas_freq_max_rad);
            demod->freq_offset_hz = (demod->costas_freq_rad / DEMOD_TWO_PI_F) * (float)demod->cfg.sample_rate_hz;
        }
    }

    /* Avança fase do NCO */
    demod->nco_phase_rad += demod->nco_omega0_rad + demod->costas_freq_rad;
    while (demod->nco_phase_rad >= DEMOD_TWO_PI_F) {
        demod->nco_phase_rad -= DEMOD_TWO_PI_F;
    }
    while (demod->nco_phase_rad < 0.0f) {
        demod->nco_phase_rad += DEMOD_TWO_PI_F;
    }

    /* 5. Recuperação de temporização (Gardner TED) e integração no platô do símbolo */
    float sym_inc = demod->sym_inc_nom + demod->sym_rate_corr;
    float prev_sym_phase = demod->sym_phase;

    if (prev_sym_phase < 0.5f && (prev_sym_phase + sym_inc) >= 0.5f) {
        demod->mid_i_curr = i_b;
        demod->mid_q_curr = q_b;
    }
    if (prev_sym_phase >= 0.15f && prev_sym_phase <= 0.85f) {
        demod->acc_i += i_b;
        demod->acc_q += q_b;
        demod->acc_cnt++;
    }

    float next_sym_phase = prev_sym_phase + sym_inc;
    if (next_sym_phase >= 1.0f) {
        next_sym_phase -= 1.0f;

        float i_hat = (demod->acc_cnt > 0U) ? (demod->acc_i / (float)demod->acc_cnt) : demod->mid_i_curr;
        float q_hat = (demod->acc_cnt > 0U) ? (demod->acc_q / (float)demod->acc_cnt) : demod->mid_q_curr;

        if (demod->has_prev_mid && demod->has_curr_bnd) {
            /* Fronteira bnd_*_curr situa-se exatamente entre mid_*_prev e mid_*_curr */
            float e_tau = ((demod->mid_i_curr - demod->mid_i_prev) * demod->bnd_i_curr) +
                          ((demod->mid_q_curr - demod->mid_q_prev) * demod->bnd_q_curr);
            e_tau = clampf_val(e_tau, -2.0f, 2.0f);
            demod->last_gardner_err = e_tau;

            next_sym_phase += demod->cfg.gardner_kp * e_tau;
            demod->sym_rate_corr += demod->cfg.gardner_ki * e_tau * demod->sym_inc_nom;
            demod->sym_rate_corr = clampf_val(demod->sym_rate_corr,
                                              -0.02f * demod->sym_inc_nom,
                                               0.02f * demod->sym_inc_nom);
            next_sym_phase = clampf_val(next_sym_phase, -0.25f, 0.80f);
        }

        demod->mid_i_prev   = demod->mid_i_curr;
        demod->mid_q_prev   = demod->mid_q_curr;
        demod->has_prev_mid = true;
        demod->bnd_i_curr   = i_b;
        demod->bnd_q_curr   = q_b;
        demod->has_curr_bnd = true;
        demod->acc_i        = 0.0f;
        demod->acc_q        = 0.0f;
        demod->acc_cnt      = 0U;

        /* Estima SNR aproximada pelo erro vetorial até o ponto ideal (±0.7071, ±0.7071) */
        const float ideal = 0.70710678f;
        float ei = fabsf(i_hat) - ideal;
        float eq = fabsf(q_hat) - ideal;
        float evm2 = (ei * ei) + (eq * eq);
        float snr_db = (evm2 > 1e-5f) ? (10.0f * log10f(1.0f / evm2)) : 40.0f;

        demod->last_i_hat = i_hat;
        demod->last_q_hat = q_hat;
        demod->total_symbols++;

        out->symbol_ready = true;
        out->i_hat        = i_hat;
        out->q_hat        = q_hat;
        out->snr_est_db   = snr_db;
    }

    demod->sym_phase = next_sym_phase;
}
