#ifndef RX_ENGINE_H
#define RX_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "rx_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MOTOR DE RECEPÇÃO 4-QAM EM TEMPO REAL
 * ============================================================================ */

typedef struct {
    uint32_t frames_ok;
    uint32_t frames_crc_err;
    uint32_t bytes_raw_rx;
    uint32_t bursts_detected;
    uint32_t symbols_demod;
    float    dc_est_mv;
    float    amp_pk_est_mv;
    float    freq_offset_hz;
    float    last_i_hat;
    float    last_q_hat;
    bool     signal_active;
} rx_engine_stats_t;

esp_err_t rx_engine_start(const qam_rx_config_t *initial_cfg);
void      rx_engine_get_config(qam_rx_config_t *out_cfg);
esp_err_t rx_engine_apply_config(const qam_rx_config_t *new_cfg, char *err_buf, size_t err_sz);
void      rx_engine_set_monitor(bool enabled);
bool      rx_engine_get_monitor(void);
void      rx_engine_get_stats(rx_engine_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* RX_ENGINE_H */
