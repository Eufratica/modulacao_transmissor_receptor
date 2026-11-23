#include "rx_config.h"

#include <stdio.h>
#include <string.h>

void rx_config_apply_map_preset(qam_rx_config_t *cfg, rx_map_preset_t preset)
{
    if (!cfg) {
        return;
    }
    cfg->map_preset = preset;

    if (preset == RX_MAP_GRAY) {
        cfg->constellation[0] = (rx_symbol_point_t){ .i_val = +1, .q_val = +1, .phase_deg = 45U,  .bits_label = "00" };
        cfg->constellation[1] = (rx_symbol_point_t){ .i_val = -1, .q_val = +1, .phase_deg = 135U, .bits_label = "01" };
        cfg->constellation[2] = (rx_symbol_point_t){ .i_val = +1, .q_val = -1, .phase_deg = 315U, .bits_label = "10" };
        cfg->constellation[3] = (rx_symbol_point_t){ .i_val = -1, .q_val = -1, .phase_deg = 225U, .bits_label = "11" };
        return;
    }

    /* Tabela padrão Natural idêntica ao transmissor:
     * 0(00): -1,-1 | 1(01): -1,+1 | 2(10): +1,-1 | 3(11): +1,+1 */
    cfg->constellation[0] = (rx_symbol_point_t){ .i_val = -1, .q_val = -1, .phase_deg = 225U, .bits_label = "00" };
    cfg->constellation[1] = (rx_symbol_point_t){ .i_val = -1, .q_val = +1, .phase_deg = 135U, .bits_label = "01" };
    cfg->constellation[2] = (rx_symbol_point_t){ .i_val = +1, .q_val = -1, .phase_deg = 315U, .bits_label = "10" };
    cfg->constellation[3] = (rx_symbol_point_t){ .i_val = +1, .q_val = +1, .phase_deg = 45U,  .bits_label = "11" };
}

void rx_config_set_defaults(qam_rx_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    cfg->carrier_freq_hz  = 1000U;
    cfg->symbol_rate_baud = 100U;
    cfg->sample_rate_hz   = 20000U; /* Mínimo exato do DMA ADC1 no ESP32: 20 amostras/ciclo 1 kHz */

    cfg->q_sign_minus     = false;  /* Casa com +Q*cos(wt - 90°) = +Q*sin(wt) */
    cfg->invert_polarity  = false;
    rx_config_apply_map_preset(cfg, RX_MAP_NATURAL);
    cfg->bit_order        = RX_BIT_ORDER_MSB_FIRST;

    cfg->rx_mode             = RX_MODE_2_FRAMED;
    cfg->mode1_use_sync_word = false;
    cfg->fixed_rotation_quad = 0U;
    cfg->sync_word[0]        = 0xD3U;
    cfg->sync_word[1]        = 0x91U;
    cfg->sync_word_len       = 2U;

    /* Ganhos estáveis para Costas Loop (fn ~ 25 Hz @ 1 kHz) e Gardner TED */
    cfg->squelch_rms_mv   = 60.0f;
    cfg->costas_kp        = 0.16f;
    cfg->costas_ki        = 0.017f;
    cfg->gardner_kp       = 0.030f;
    cfg->gardner_ki       = 0.0015f;

    cfg->adc1_channel     = 6;      /* ADC1_CHANNEL_6 = GPIO 34 (apenas entrada, seguro) */
    cfg->adc_use_cal      = true;
}

bool rx_config_validate(const qam_rx_config_t *cfg, char *err_buf, size_t err_buf_sz)
{
    if (!cfg) {
        if (err_buf && err_buf_sz > 0) snprintf(err_buf, err_buf_sz, "Configuracao nula");
        return false;
    }
    if (cfg->carrier_freq_hz < 100U || cfg->carrier_freq_hz > 10000U) {
        if (err_buf && err_buf_sz > 0) snprintf(err_buf, err_buf_sz, "carrier_freq_hz fora da faixa (100..10000 Hz)");
        return false;
    }
    if (cfg->symbol_rate_baud < 5U || cfg->symbol_rate_baud > 2000U) {
        if (err_buf && err_buf_sz > 0) snprintf(err_buf, err_buf_sz, "symbol_rate_baud fora da faixa (5..2000)");
        return false;
    }
    /* ESP32 soc_caps.h exige >= 20 kHz para ADC DMA contínuo */
    if (cfg->sample_rate_hz < 20000U || cfg->sample_rate_hz > 80000U) {
        if (err_buf && err_buf_sz > 0) snprintf(err_buf, err_buf_sz, "sample_rate_hz ADC DMA deve estar em 20000..80000 Hz");
        return false;
    }
    if (cfg->sample_rate_hz < cfg->carrier_freq_hz * 8U) {
        if (err_buf && err_buf_sz > 0) snprintf(err_buf, err_buf_sz, "sample_rate_hz deve ser >= 8x carrier_freq_hz");
        return false;
    }
    if (cfg->sample_rate_hz < cfg->symbol_rate_baud * 10U) {
        if (err_buf && err_buf_sz > 0) snprintf(err_buf, err_buf_sz, "sample_rate_hz deve ser >= 10x symbol_rate_baud");
        return false;
    }
    if (cfg->adc1_channel < 0 || cfg->adc1_channel > 7) {
        if (err_buf && err_buf_sz > 0) snprintf(err_buf, err_buf_sz, "Canal ADC1 invalido (0..7)");
        return false;
    }
    if (cfg->sync_word_len == 0U || cfg->sync_word_len > RX_MAX_SYNC_WORD_LEN) {
        if (err_buf && err_buf_sz > 0) snprintf(err_buf, err_buf_sz, "sync_word_len invalido");
        return false;
    }
    if (cfg->squelch_rms_mv < 5.0f || cfg->squelch_rms_mv > 1000.0f) {
        if (err_buf && err_buf_sz > 0) snprintf(err_buf, err_buf_sz, "squelch_rms_mv fora de 5..1000 mV");
        return false;
    }
    return true;
}

int rx_config_adc1_channel_to_gpio(int channel)
{
    static const int map[8] = { 36, 37, 38, 39, 32, 33, 34, 35 };
    if (channel < 0 || channel > 7) {
        return -1;
    }
    return map[channel];
}

const char *rx_config_mode_name(rx_decode_mode_t mode)
{
    switch (mode) {
    case RX_MODE_1_RAW_STREAM: return "Modo 1 (Fluxo Direto / Professor)";
    case RX_MODE_2_FRAMED:     return "Modo 2 (Quadro Proprio + Sync + CRC-16)";
    default:                   return "Desconhecido";
    }
}

const char *rx_config_bit_order_name(rx_bit_order_t order)
{
    switch (order) {
    case RX_BIT_ORDER_MSB_FIRST:       return "MSB-first";
    case RX_BIT_ORDER_LSB_BIT_FIRST:   return "LSB-bit-first";
    case RX_BIT_ORDER_LSB_DIBIT_FIRST: return "LSB-dibit-first";
    default:                           return "Desconhecido";
    }
}

const char *rx_config_map_name(rx_map_preset_t preset)
{
    switch (preset) {
    case RX_MAP_NATURAL: return "Natural (0:-1,-1 | 1:-1,+1 | 2:+1,-1 | 3:+1,+1)";
    case RX_MAP_GRAY:    return "Gray (0:+1,+1 | 1:-1,+1 | 2:+1,-1 | 3:-1,-1)";
    default:             return "Desconhecido";
    }
}
