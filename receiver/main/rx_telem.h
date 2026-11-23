#ifndef RX_TELEM_H
#define RX_TELEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "qam_demod.h"
#include "rx_config.h"
#include "rx_deframer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TELEMETRIA BINÁRIA VERSIONADA DO RECEPTOR 4-QAM (4QAM-TLM v1)
 * ============================================================================
 * Core 1 (ADC1 DMA + DSP) empurra métricas e blocos decimados/snapshots sem
 * bloqueio; Core 0 serializa os pacotes com CRC-16/CCITT-FALSE sobre UART0.
 * ============================================================================ */

#define RX_TLM_SYNC_0           0xAAU
#define RX_TLM_SYNC_1           0x55U
#define RX_TLM_PROTO_VER        0x01U

#define RX_TLM_MSG_HELLO_STATUS 0x01U
#define RX_TLM_MSG_BURST_EVENT  0x10U
#define RX_TLM_MSG_SYMBOL_BATCH 0x11U
#define RX_TLM_MSG_FRAME_EVENT  0x12U
#define RX_TLM_MSG_ADC_CHUNK    0x13U

#define RX_TLM_ADC_SNAP_LEN     2048U

esp_err_t rx_telem_init(void);
void      rx_telem_set_enabled(bool enabled);
bool      rx_telem_is_enabled(void);

void      rx_telem_set_adc_decim(uint16_t decim);
uint16_t  rx_telem_get_adc_decim(void);

void      rx_telem_console_lock(void);
void      rx_telem_console_unlock(void);

void      rx_telem_send_hello_now(void);
void      rx_telem_request_adc_capture(void);

/* Ganchos chamados exclusivamente na rx_dsp_task (Core 1) sem bloqueio */
void rx_telem_on_adc_block(const float *mv_samples, size_t count, uint32_t sample_rate_hz, bool cal_active);
void rx_telem_on_burst_event(uint8_t event_kind, uint32_t burst_id, float dc_mv, float amp_pk_mv);
void rx_telem_on_symbol_dsp(uint32_t burst_id,
                            const qam_rx_config_t *cfg,
                            const qam_demod_t *demod,
                            const qam_demod_out_t *dout,
                            const rx_deframer_t *df,
                            uint8_t sym_dec,
                            bool force_flush);
void rx_telem_on_frame_event(uint32_t burst_id,
                             uint8_t rx_mode,
                             bool crc_ok,
                             uint8_t locked_rot,
                             uint8_t payload_order,
                             uint16_t tx_baud,
                             uint16_t rx_crc,
                             uint16_t calc_crc,
                             float df_est_hz,
                             float dc_est_mv,
                             float amp_pk_mv,
                             float snr_db,
                             const uint8_t *payload,
                             size_t payload_len);

uint32_t rx_telem_get_dropped_count(void);

#ifdef __cplusplus
}
#endif

#endif /* RX_TELEM_H */
