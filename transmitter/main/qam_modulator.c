#include "qam_modulator.h"

#include <math.h>
#include <string.h>

#define QAM_LUT_BITS   10U
#define QAM_LUT_SIZE   (1U << QAM_LUT_BITS) /* 1024 pontos por ciclo completo */
#define QAM_LUT_MASK   (QAM_LUT_SIZE - 1U)
#define QAM_SQRT2_F    1.4142135623730951f
#define QAM_TWO_PI_F   6.2831853071795865f

/* Tabela senoidal compartilhada de 1024 pontos + 1 ponto de guarda para interpolação linear */
static float s_sin_lut[QAM_LUT_SIZE + 1U];
static bool s_lut_initialized = false;

static void ensure_sin_lut(void)
{
    if (s_lut_initialized) {
        return;
    }
    for (uint32_t i = 0; i <= QAM_LUT_SIZE; i++) {
        float angle = QAM_TWO_PI_F * ((float)i / (float)QAM_LUT_SIZE);
        s_sin_lut[i] = sinf(angle);
    }
    s_sin_lut[QAM_LUT_SIZE] = s_sin_lut[0];
    s_lut_initialized = true;
}

static inline void lut_sin_cos_from_turns(float turns, float *out_sin, float *out_cos)
{
    float pos  = turns * (float)QAM_LUT_SIZE;
    uint32_t idx = (uint32_t)pos;
    float frac = pos - (float)idx;

    idx &= QAM_LUT_MASK;
    float s0 = s_sin_lut[idx];
    float s1 = s_sin_lut[idx + 1U];
    *out_sin = s0 + frac * (s1 - s0);

    /* cos(theta) = sin(theta + pi/2) -> deslocamento exato de 1/4 de tabela (256 amostras) */
    uint32_t cidx = (idx + (QAM_LUT_SIZE / 4U)) & QAM_LUT_MASK;
    float c0 = s_sin_lut[cidx];
    float c1 = s_sin_lut[cidx + 1U];
    *out_cos = c0 + frac * (c1 - c0);
}

static float compute_pulse_envelope(const qam_modulator_t *mod)
{
    if (mod->cfg.pulse_shape != QAM_PULSE_RAISED_EDGE_TAPER) {
        return 1.0f;
    }

    float u = (float)mod->sym_phase_num / (float)mod->cfg.sample_rate_hz;
    const float edge = 0.10f; /* 10% de rampa suave em cada extremidade do símbolo */
    float t_norm = 1.0f;

    if (u < edge) {
        t_norm = u / edge;
    } else if (u > (1.0f - edge)) {
        t_norm = (1.0f - u) / edge;
    } else {
        return 1.0f;
    }

    /* Meio ciclo de cosseno elevado: 0.5 * (1 - cos(pi * t_norm)) */
    float sin_v = 0.0f;
    float cos_v = 0.0f;
    lut_sin_cos_from_turns(0.5f * t_norm, &sin_v, &cos_v);
    return 0.5f * (1.0f - cos_v);
}

static inline void advance_carrier_phase(qam_modulator_t *mod)
{
    mod->carrier_phase_num += mod->cfg.carrier_freq_hz;
    if (mod->carrier_phase_num >= mod->cfg.sample_rate_hz) {
        mod->carrier_phase_num -= mod->cfg.sample_rate_hz;
    }
}

bool qam_modulator_init(qam_modulator_t *mod, const qam_tx_config_t *cfg, uint32_t pwm_period_ticks)
{
    if (!mod || !cfg || pwm_period_ticks < 2U) {
        return false;
    }
    if (!qam_config_validate(cfg, NULL, 0U)) {
        return false;
    }

    ensure_sin_lut();
    memset(mod, 0, sizeof(*mod));
    mod->lut_ready = true;
    return qam_modulator_reconfigure(mod, cfg, pwm_period_ticks, true);
}

bool qam_modulator_reconfigure(qam_modulator_t *mod, const qam_tx_config_t *cfg,
                               uint32_t pwm_period_ticks, bool reset_phase)
{
    if (!mod || !cfg || pwm_period_ticks < 2U) {
        return false;
    }
    if (!qam_config_validate(cfg, NULL, 0U)) {
        return false;
    }

    ensure_sin_lut();
    mod->cfg = *cfg;
    mod->pwm_period_ticks = pwm_period_ticks;
    mod->a_eq_mv = (float)cfg->amplitude_peak_mv / QAM_SQRT2_F;
    mod->polarity_sign = cfg->invert_polarity ? -1.0f : 1.0f;
    mod->lut_ready = true;

    if (reset_phase) {
        qam_modulator_reset_phase(mod);
    } else {
        mod->carrier_phase_num %= cfg->sample_rate_hz;
        mod->sym_phase_num %= cfg->sample_rate_hz;
    }
    return true;
}

void qam_modulator_reset_phase(qam_modulator_t *mod)
{
    if (!mod) {
        return;
    }
    mod->carrier_phase_num = 0U;
    mod->sym_phase_num = 0U;
}

float qam_modulator_next_symbol_mv(qam_modulator_t *mod, uint8_t symbol, bool *symbol_finished)
{
    if (symbol_finished) {
        *symbol_finished = false;
    }
    if (!mod || !mod->lut_ready) {
        return 0.0f;
    }

    symbol &= 0x03U;
    const qam_symbol_point_t *pt = &mod->cfg.constellation[symbol];

    float sin_v = 0.0f;
    float cos_v = 0.0f;
    float turns = (float)mod->carrier_phase_num / (float)mod->cfg.sample_rate_hz;
    lut_sin_cos_from_turns(turns, &sin_v, &cos_v);

    /* Equação 4-QAM configurável:
     *   q_sign_minus == false (padrão):
     *     s(t) = A * [ I*cos(2*pi*fc*t) + Q*cos(2*pi*fc*t - 90°) ]
     *          = A * [ I*cos(2*pi*fc*t) + Q*sin(2*pi*fc*t) ]
     *   q_sign_minus == true:
     *     s(t) = A * [ I*cos(2*pi*fc*t) - Q*sin(2*pi*fc*t) ] */
    float q_term = mod->cfg.q_sign_minus ? -((float)pt->q_val * sin_v)
                                         : +((float)pt->q_val * sin_v);
    float qam_core = ((float)pt->i_val * cos_v) + q_term;
    float env = compute_pulse_envelope(mod);
    float v_mv = (float)mod->cfg.dc_offset_mv +
                 (mod->polarity_sign * env * mod->a_eq_mv * qam_core);

    /* Avança fase da portadora e acumulador de símbolo com erro zero acumulado */
    advance_carrier_phase(mod);
    mod->sym_phase_num += mod->cfg.symbol_rate_baud;
    if (mod->sym_phase_num >= mod->cfg.sample_rate_hz) {
        mod->sym_phase_num -= mod->cfg.sample_rate_hz;
        if (symbol_finished) {
            *symbol_finished = true;
        }
    }

    return v_mv;
}

float qam_modulator_next_carrier_mv(qam_modulator_t *mod)
{
    if (!mod || !mod->lut_ready) {
        return 0.0f;
    }

    float sin_v = 0.0f;
    float cos_v = 0.0f;
    float turns = (float)mod->carrier_phase_num / (float)mod->cfg.sample_rate_hz;
    lut_sin_cos_from_turns(turns, &sin_v, &cos_v);

    float v_mv = (float)mod->cfg.dc_offset_mv +
                 (mod->polarity_sign * (float)mod->cfg.amplitude_peak_mv * cos_v);

    advance_carrier_phase(mod);
    return v_mv;
}

uint16_t qam_modulator_mv_to_pwm_cmp(const qam_modulator_t *mod, float voltage_mv)
{
    if (!mod || mod->pwm_period_ticks < 2U) {
        return 1U;
    }

    float duty = voltage_mv / (float)mod->cfg.vdd_mv;
    float ticks_f = (duty * (float)mod->pwm_period_ticks) + 0.5f;
    int32_t ticks = (int32_t)ticks_f;

    /* Clamping defensivo em [1 .. period - 1] para evitar colisão de comparador em 0 ou 100% */
    if (ticks < 1) {
        ticks = 1;
    } else if ((uint32_t)ticks >= mod->pwm_period_ticks) {
        ticks = (int32_t)(mod->pwm_period_ticks - 1U);
    }
    return (uint16_t)ticks;
}

uint16_t qam_modulator_next_symbol_cmp(qam_modulator_t *mod, uint8_t symbol, bool *symbol_finished)
{
    float mv = qam_modulator_next_symbol_mv(mod, symbol, symbol_finished);
    return qam_modulator_mv_to_pwm_cmp(mod, mv);
}

uint16_t qam_modulator_next_carrier_cmp(qam_modulator_t *mod)
{
    float mv = qam_modulator_next_carrier_mv(mod);
    return qam_modulator_mv_to_pwm_cmp(mod, mv);
}

uint16_t qam_modulator_idle_dc_cmp(const qam_modulator_t *mod)
{
    if (!mod) {
        return 1U;
    }
    return qam_modulator_mv_to_pwm_cmp(mod, (float)mod->cfg.dc_offset_mv);
}
