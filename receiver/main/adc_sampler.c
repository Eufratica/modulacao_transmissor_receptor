#include "adc_sampler.h"

#include <string.h>
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "rx_config.h"

static const char *TAG = "adc_sampler";

#define ADC_DMA_CONV_BYTES   (ADC_SAMPLER_DMA_CHUNK_SAMPLES * sizeof(adc_digi_output_data_t))
#define ADC_DMA_STORE_BYTES  4096U

static bool s_started = false;
static int s_channel = 6;
static uint32_t s_sample_rate_hz = 20000U;
static bool s_use_cal = true;
static bool s_cal_ready = false;
static esp_adc_cal_characteristics_t s_adc_chars;

static uint32_t s_samples_read = 0U;
static uint32_t s_invalid_samples = 0U;
static uint32_t s_overflow_count = 0U;

static void init_calibration(bool use_cal)
{
    s_use_cal = use_cal;
    s_cal_ready = false;
    if (!use_cal) {
        return;
    }

    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1,
                                                            ADC_ATTEN_DB_12,
                                                            ADC_WIDTH_BIT_12,
                                                            1100U,
                                                            &s_adc_chars);
    s_cal_ready = true;
    ESP_LOGI(TAG, "Calibracao ADC1: tipo=%d (0=Vref eFuse, 1=TwoPoint eFuse, 2=Padrao 1100mV)", (int)val_type);
}

static esp_err_t start_internal(int adc1_channel, uint32_t sample_rate_hz, bool use_cal)
{
    if (adc1_channel < 0 || adc1_channel >= ADC1_CHANNEL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    init_calibration(use_cal);

    adc_digi_init_config_t init_cfg = {
        .max_store_buf_size = ADC_DMA_STORE_BYTES,
        .conv_num_each_intr = ADC_DMA_CONV_BYTES,
        .adc1_chan_mask     = (uint32_t)(1U << (uint32_t)adc1_channel),
        .adc2_chan_mask     = 0U,
    };
    esp_err_t err = adc_digi_initialize(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha em adc_digi_initialize: %s", esp_err_to_name(err));
        return err;
    }

    adc_digi_pattern_config_t pattern = {
        .atten     = ADC_ATTEN_DB_12,
        .channel   = (uint8_t)adc1_channel,
        .unit      = 0U, /* ESP32 DMA exige unit == 0 (ADC1) */
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH, /* ESP32 DMA exige largura numerica 9..12, nao enum ADC_WIDTH_BIT_12 (3) */
    };

    adc_digi_configuration_t dig_cfg = {
        .conv_limit_en  = true, /* Obrigatório no ESP32 conforme driver/adc.c */
        .conv_limit_num = 250U,
        .pattern_num    = 1U,
        .adc_pattern    = &pattern,
        .sample_freq_hz = sample_rate_hz,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    err = adc_digi_controller_configure(&dig_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha em adc_digi_controller_configure: %s", esp_err_to_name(err));
        (void)adc_digi_deinitialize();
        return err;
    }

    err = adc_digi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha em adc_digi_start: %s", esp_err_to_name(err));
        (void)adc_digi_deinitialize();
        return err;
    }

    s_channel        = adc1_channel;
    s_sample_rate_hz = sample_rate_hz;
    s_started        = true;

    ESP_LOGI(TAG, "ADC1 DMA iniciado: ch=%d (GPIO %d) | fs=%u Hz | faixa linear 150..2450 mV",
             adc1_channel, rx_config_adc1_channel_to_gpio(adc1_channel), (unsigned)sample_rate_hz);
    return ESP_OK;
}

esp_err_t adc_sampler_start(int adc1_channel, uint32_t sample_rate_hz, bool use_cal)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_samples_read    = 0U;
    s_invalid_samples = 0U;
    s_overflow_count  = 0U;
    return start_internal(adc1_channel, sample_rate_hz, use_cal);
}

esp_err_t adc_sampler_reconfigure(int adc1_channel, uint32_t sample_rate_hz, bool use_cal)
{
    if (s_started) {
        (void)adc_digi_stop();
        (void)adc_digi_deinitialize();
        s_started = false;
    }
    return start_internal(adc1_channel, sample_rate_hz, use_cal);
}

esp_err_t adc_sampler_read_mv(float *out_mv, size_t max_samples, size_t *out_count, uint32_t timeout_ms)
{
    if (!out_mv || !out_count || max_samples == 0U || !s_started) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0U;
    uint8_t raw_buf[ADC_DMA_CONV_BYTES];
    size_t req_bytes = max_samples * sizeof(adc_digi_output_data_t);
    if (req_bytes > sizeof(raw_buf)) {
        req_bytes = sizeof(raw_buf);
    }

    uint32_t out_bytes = 0U;
    esp_err_t ret = adc_digi_read_bytes(raw_buf, (uint32_t)req_bytes, &out_bytes, timeout_ms);
    if (ret == ESP_ERR_TIMEOUT) {
        return ret;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        s_overflow_count++;
    } else if (ret != ESP_OK) {
        return ret;
    }

    size_t count = 0U;
    for (uint32_t i = 0U; (i + sizeof(adc_digi_output_data_t)) <= out_bytes; i += sizeof(adc_digi_output_data_t)) {
        const adc_digi_output_data_t *p = (const adc_digi_output_data_t *)&raw_buf[i];
        if ((int)p->type1.channel != s_channel) {
            s_invalid_samples++;
            continue;
        }
        uint32_t raw = p->type1.data;
        float mv = (s_use_cal && s_cal_ready)
                   ? (float)esp_adc_cal_raw_to_voltage(raw, &s_adc_chars)
                   : (((float)raw * 3300.0f) / 4095.0f);
        out_mv[count++] = mv;
        s_samples_read++;
    }

    *out_count = count;
    return ESP_OK;
}

void adc_sampler_get_stats(adc_sampler_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    out_stats->sample_rate_hz   = s_sample_rate_hz;
    out_stats->adc1_channel     = s_channel;
    out_stats->gpio_num         = rx_config_adc1_channel_to_gpio(s_channel);
    out_stats->samples_read     = s_samples_read;
    out_stats->invalid_samples  = s_invalid_samples;
    out_stats->overflow_count   = s_overflow_count;
    out_stats->efuse_cal_active = s_use_cal && s_cal_ready;
}
