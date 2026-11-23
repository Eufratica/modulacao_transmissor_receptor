#ifndef TX_TELEM_H
#define TX_TELEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "qam_config.h"
#include "qam_framer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TELEMETRIA BINÁRIA VERSIONADA DO TRANSMISSOR 4-QAM (4QAM-TLM v1)
 * ============================================================================
 * Separação estrita entre DSP (Core 1) e UART (Core 0):
 *  - Core 1 apenas grava eventos/capturas em fila FreeRTOS sem bloqueio (timeout 0).
 *  - Core 0 executa tarefa de baixa prioridade que serializa pacotes com CRC-16
 *    protegidos por mutex de console para não colidir com a CLI ASCII.
 * ============================================================================ */

#define TX_TLM_SYNC_0           0xAAU
#define TX_TLM_SYNC_1           0x55U
#define TX_TLM_PROTO_VER        0x01U

#define TX_TLM_MSG_HELLO_STATUS 0x01U
#define TX_TLM_MSG_FRAME_EVENT  0x02U
#define TX_TLM_MSG_PWM_CHUNK    0x03U

#define TX_TLM_PWM_SNAP_MAX     512U

typedef enum {
    TX_FRAME_EV_QUEUED  = 0,
    TX_FRAME_EV_STARTED = 1,
    TX_FRAME_EV_DONE    = 2,
    TX_FRAME_EV_DROPPED = 3,
} tx_frame_ev_status_t;

esp_err_t tx_telem_init(void);
void      tx_telem_set_enabled(bool enabled);
bool      tx_telem_is_enabled(void);

/* Controle do mutex de UART para evitar intercalamento entre printf ASCII e pacotes binários */
void tx_telem_console_lock(void);
void tx_telem_console_unlock(void);

/* Dispara pacotes sob demanda (/id, /status, /telem 1) */
void tx_telem_send_hello_now(void);

/* Notifica eventos de quadro sem bloquear (seguro para Core 1 e Core 0) */
void tx_telem_emit_frame_event(uint32_t frame_id,
                               const qam_tx_config_t *cfg,
                               const uint8_t *payload,
                               size_t payload_len,
                               const qam_tx_frame_t *frame,
                               tx_frame_ev_status_t status);

/* Solicita captura única da sequência de ticks PWM efetivamente programados */
void tx_telem_request_pwm_capture(void);

/* Chamado exclusivamente pelo Core 1 dentro de tx_dsp_task */
bool tx_telem_pwm_capture_active(void);
void tx_telem_pwm_capture_push(uint16_t cmp_val, uint32_t frame_id, uint32_t sample_rate_hz, uint16_t period_ticks);

uint32_t tx_telem_get_dropped_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TX_TELEM_H */
