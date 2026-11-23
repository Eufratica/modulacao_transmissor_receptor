#include "qam_config.h"

#include <stdio.h>
#include <string.h>

static bool is_valid_esp32_output_gpio(int gpio)
{
    if (gpio < 0 || gpio >= 34) {
        return false; /* GPIOs >= 34 são apenas entrada no ESP32 */
    }
    if (gpio >= 6 && gpio <= 11) {
        return false; /* Pinos reservados para Flash SPI interna */
    }
    if (gpio == 20 || gpio == 24 || (gpio >= 28 && gpio <= 31)) {
        return false; /* Pinos inexistentes no encapsulamento ESP32-WROOM-32 */
    }
    return true;
}

void qam_config_apply_map_preset(qam_tx_config_t *cfg, qam_map_preset_t preset)
{
    if (!cfg) {
        return;
    }

    cfg->map_preset = preset;

    if (preset == QAM_MAP_GRAY) {
        /* Mapeamento Gray alternativo:
         * 0 (00): +1, +1 | 1 (01): -1, +1 | 2 (10): +1, -1 | 3 (11): -1, -1 */
        cfg->constellation[0] = (qam_symbol_point_t){ .i_val = +1, .q_val = +1, .phase_deg = 45U,  .bits_label = "00" };
        cfg->constellation[1] = (qam_symbol_point_t){ .i_val = -1, .q_val = +1, .phase_deg = 135U, .bits_label = "01" };
        cfg->constellation[2] = (qam_symbol_point_t){ .i_val = +1, .q_val = -1, .phase_deg = 315U, .bits_label = "10" };
        cfg->constellation[3] = (qam_symbol_point_t){ .i_val = -1, .q_val = -1, .phase_deg = 225U, .bits_label = "11" };
        return;
    }

    /* Tabela padrão confirmada pelo usuário (Binária Natural: bit 0 -> -1, bit 1 -> +1):
     * Símbolo | Bits |  I  |  Q  | Ângulo no plano I/Q
     *    0    |  00  | -1  | -1  | 225°
     *    1    |  01  | -1  | +1  | 135°
     *    2    |  10  | +1  | -1  | 315°
     *    3    |  11  | +1  | +1  |  45°
     */
    cfg->constellation[0] = (qam_symbol_point_t){ .i_val = -1, .q_val = -1, .phase_deg = 225U, .bits_label = "00" };
    cfg->constellation[1] = (qam_symbol_point_t){ .i_val = -1, .q_val = +1, .phase_deg = 135U, .bits_label = "01" };
    cfg->constellation[2] = (qam_symbol_point_t){ .i_val = +1, .q_val = -1, .phase_deg = 315U, .bits_label = "10" };
    cfg->constellation[3] = (qam_symbol_point_t){ .i_val = +1, .q_val = +1, .phase_deg = 45U,  .bits_label = "11" };
}

void qam_config_set_defaults(qam_tx_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    /* Parâmetros de modulação iniciais (taxa de símbolos marcada como provisória) */
    cfg->carrier_freq_hz   = 1000U;
    cfg->symbol_rate_baud  = 100U;
    cfg->sample_rate_hz    = 40000U;
    cfg->bits_per_symbol   = 2U;

    /* Escala elétrica segura para ESP32 e faixa linear do ADC1 (0,40 V .. 2,20 V) */
    cfg->vdd_mv            = 3300U;
    cfg->dc_offset_mv      = 1300U;
    cfg->amplitude_peak_mv = 900U; /* Vpk = 900 mV => A_eq = 900/sqrt(2) ≈ 636,4 mV */
    cfg->invert_polarity   = false;

    /* Equação com sinal POSITIVO: s(t) = A * [ I*cos(2*pi*fc*t) + Q*cos(2*pi*fc*t - 90°) ]
     * Como cos(wt - 90°) = +sin(wt), equivale a A * [ I*cos(wt) + Q*sin(wt) ]. */
    cfg->q_sign_minus      = false;

    qam_config_apply_map_preset(cfg, QAM_MAP_NATURAL);
    cfg->bit_order         = QAM_BIT_ORDER_MSB_FIRST;
    cfg->pulse_shape       = QAM_PULSE_RECTANGULAR;

    /* Modo 1 padrão estrito: envia dados sem cabeçalho próprio ou CRC extra */
    cfg->tx_mode             = QAM_TX_MODE_1_PROFESSOR;
    cfg->idle_mode           = QAM_IDLE_DC_SILENCE;
    cfg->mode1_use_preamble  = false;
    cfg->mode1_use_sync_word = false;
    cfg->preamble_bytes      = 4U; /* 2 bytes 0x00 (portadora) + 2 bytes 0x33 (transições 180°) */
    cfg->sync_word[0]        = 0xD3U;
    cfg->sync_word[1]        = 0x91U;
    cfg->sync_word_len       = 2U;

    /* Padrão contínuo para testes de bancada */
    cfg->cont_pattern      = QAM_CONT_PATTERN_CYCLE_4;
    cfg->cont_fixed_symbol = 0U;
    snprintf(cfg->cont_text, sizeof(cfg->cont_text), "4QAM-ESP32");

    /* MCPWM simétrico a 80 kHz (160 MHz / (2 * 1000) = 80 kHz, divisor 2 para fs = 40 kHz) */
    cfg->pwm_gpio          = 18;
    cfg->pwm_freq_hz       = 80000U;
    cfg->pwm_symmetric     = true;
    cfg->pwm_invert_output = false;
}

static bool validate_constellation(const qam_tx_config_t *cfg, char *err_buf, size_t err_buf_sz)
{
    for (uint8_t s = 0; s < QAM_CONSTELLATION_SIZE; s++) {
        int8_t iv = cfg->constellation[s].i_val;
        int8_t qv = cfg->constellation[s].q_val;
        if ((iv != 1 && iv != -1) || (qv != 1 && qv != -1)) {
            if (err_buf && err_buf_sz > 0) {
                snprintf(err_buf, err_buf_sz,
                         "Constelacao invalida no simbolo %u: I=%d Q=%d (esperado +1 ou -1)",
                         (unsigned)s, (int)iv, (int)qv);
            }
            return false;
        }
    }
    return true;
}

bool qam_config_validate(const qam_tx_config_t *cfg, char *err_buf, size_t err_buf_sz)
{
    if (!cfg) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "Ponteiro de configuracao nulo");
        }
        return false;
    }

    if (cfg->bits_per_symbol != 2U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "Somente 2 bits/simbolo (4-QAM) implementado atualmente");
        }
        return false;
    }

    if (cfg->carrier_freq_hz < 50U || cfg->carrier_freq_hz > 20000U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "Frequencia da portadora fora da faixa (50..20000 Hz)");
        }
        return false;
    }

    if (cfg->symbol_rate_baud < 1U || cfg->symbol_rate_baud > 5000U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "Taxa de simbolos fora da faixa (1..5000 baud)");
        }
        return false;
    }

    if (cfg->sample_rate_hz < 4000U || cfg->sample_rate_hz > 100000U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "Taxa de amostragem fs fora da faixa (4000..100000 Hz)");
        }
        return false;
    }

    if (cfg->sample_rate_hz < cfg->carrier_freq_hz * 8U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz,
                     "fs (%u Hz) muito baixa para portadora (%u Hz); minimo 8x fc",
                     (unsigned)cfg->sample_rate_hz, (unsigned)cfg->carrier_freq_hz);
        }
        return false;
    }

    if (cfg->sample_rate_hz < cfg->symbol_rate_baud * 4U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz,
                     "fs (%u Hz) deve ser >= 4x taxa de simbolos (%u baud)",
                     (unsigned)cfg->sample_rate_hz, (unsigned)cfg->symbol_rate_baud);
        }
        return false;
    }

    if (cfg->pwm_freq_hz < 10000U || cfg->pwm_freq_hz > 200000U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "Frequencia PWM fora da faixa (10000..200000 Hz)");
        }
        return false;
    }

    if (cfg->pwm_freq_hz < cfg->sample_rate_hz || (cfg->pwm_freq_hz % cfg->sample_rate_hz) != 0U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz,
                     "pwm_freq_hz (%u) deve ser multiplo inteiro >= sample_rate_hz (%u)",
                     (unsigned)cfg->pwm_freq_hz, (unsigned)cfg->sample_rate_hz);
        }
        return false;
    }

    if (cfg->vdd_mv < 2500U || cfg->vdd_mv > 3600U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "vdd_mv invalido (%u mV)", (unsigned)cfg->vdd_mv);
        }
        return false;
    }

    if (cfg->amplitude_peak_mv < 50U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "amplitude_peak_mv muito baixa (< 50 mV)");
        }
        return false;
    }

    if (cfg->dc_offset_mv < (uint32_t)cfg->amplitude_peak_mv + 50U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz,
                     "Risco de ceifamento inferior: dc_offset_mv (%u) < amplitude_peak_mv (%u) + 50 mV",
                     (unsigned)cfg->dc_offset_mv, (unsigned)cfg->amplitude_peak_mv);
        }
        return false;
    }

    if ((uint32_t)cfg->dc_offset_mv + (uint32_t)cfg->amplitude_peak_mv > (uint32_t)cfg->vdd_mv - 50U) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz,
                     "Risco de saturacao superior: dc_offset + amp (%u mV) > vdd - 50 mV (%u mV)",
                     (unsigned)(cfg->dc_offset_mv + cfg->amplitude_peak_mv),
                     (unsigned)(cfg->vdd_mv - 50U));
        }
        return false;
    }

    if (!is_valid_esp32_output_gpio(cfg->pwm_gpio)) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "GPIO %d invalido para saida PWM no ESP32", cfg->pwm_gpio);
        }
        return false;
    }

    if (cfg->sync_word_len == 0U || cfg->sync_word_len > QAM_MAX_SYNC_WORD_LEN) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "sync_word_len deve estar entre 1 e %u", QAM_MAX_SYNC_WORD_LEN);
        }
        return false;
    }

    if (cfg->cont_fixed_symbol >= QAM_CONSTELLATION_SIZE) {
        if (err_buf && err_buf_sz > 0) {
            snprintf(err_buf, err_buf_sz, "cont_fixed_symbol deve estar entre 0 e 3");
        }
        return false;
    }

    return validate_constellation(cfg, err_buf, err_buf_sz);
}

const char *qam_config_mode_name(qam_tx_mode_t mode)
{
    switch (mode) {
    case QAM_TX_MODE_1_PROFESSOR:  return "Modo 1 (Compatibilidade Professor)";
    case QAM_TX_MODE_2_FRAMED:     return "Modo 2 (Quadro Proprio + CRC-16)";
    case QAM_TX_MODE_3_CONTINUOUS: return "Modo 3 (Continuo / Teste)";
    default:                       return "Desconhecido";
    }
}

const char *qam_config_bit_order_name(qam_bit_order_t order)
{
    switch (order) {
    case QAM_BIT_ORDER_MSB_FIRST:       return "MSB-first";
    case QAM_BIT_ORDER_LSB_BIT_FIRST:   return "LSB-bit-first";
    case QAM_BIT_ORDER_LSB_DIBIT_FIRST: return "LSB-dibit-first";
    default:                            return "Desconhecido";
    }
}

const char *qam_config_cont_pattern_name(qam_cont_pattern_t pattern)
{
    switch (pattern) {
    case QAM_CONT_PATTERN_TEXT:         return "Texto Continuo";
    case QAM_CONT_PATTERN_FIXED_SYMBOL: return "Simbolo Fixo";
    case QAM_CONT_PATTERN_CYCLE_4:      return "Ciclo 00-01-11-10";
    case QAM_CONT_PATTERN_RAW_CARRIER:  return "Portadora Pura";
    default:                            return "Desconhecido";
    }
}

const char *qam_config_map_name(qam_map_preset_t preset)
{
    switch (preset) {
    case QAM_MAP_NATURAL: return "Natural (0:-1,-1 | 1:-1,+1 | 2:+1,-1 | 3:+1,+1)";
    case QAM_MAP_GRAY:    return "Gray (0:+1,+1 | 1:-1,+1 | 2:+1,-1 | 3:-1,-1)";
    default:              return "Desconhecido";
    }
}
