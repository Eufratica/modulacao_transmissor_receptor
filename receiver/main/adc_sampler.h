#ifndef ADC_SAMPLER_H
#define ADC_SAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * AQUISIÇÃO CONTÍNUA ADC1 VIA DMA (I2S0 NO ESP32) + CALIBRAÇÃO eFUSE
 * ============================================================================ */

#define ADC_SAMPLER_DMA_CHUNK_SAMPLES  256U

typedef struct {
    uint32_t sample_rate_hz;
    int      adc1_channel;
    int      gpio_num;
    uint32_t samples_read;
    uint32_t invalid_samples;
    uint32_t overflow_count;
    bool     efuse_cal_active;
} adc_sampler_stats_t;

/**
 * @brief Inicializa o controlador digital do ADC1 em modo DMA contínuo e inicia a captura.
 */
esp_err_t adc_sampler_start(int adc1_channel, uint32_t sample_rate_hz, bool use_cal);

/**
 * @brief Para o DMA, reconfigura canal/frequência e reinicia a aquisição.
 */
esp_err_t adc_sampler_reconfigure(int adc1_channel, uint32_t sample_rate_hz, bool use_cal);

/**
 * @brief Lê um bloco de amostras convertidas em milivolts (mV).
 *
 * @param out_mv      Vetor de saída em milivolts.
 * @param max_samples Capacidade máxima de out_mv em amostras.
 * @param out_count   Quantidade real de amostras gravadas em out_mv.
 * @param timeout_ms  Tempo máximo de espera pelos dados DMA.
 * @return ESP_OK, ESP_ERR_TIMEOUT ou código de erro.
 */
esp_err_t adc_sampler_read_mv(float *out_mv, size_t max_samples, size_t *out_count, uint32_t timeout_ms);

/**
 * @brief Retorna estatísticas acumuladas da aquisição DMA.
 */
void adc_sampler_get_stats(adc_sampler_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* ADC_SAMPLER_H */
