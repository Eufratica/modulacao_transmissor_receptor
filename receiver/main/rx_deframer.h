#ifndef RX_DEFRAMER_H
#define RX_DEFRAMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rx_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DESENQUADRADOR E RECONSTRUÇÃO DE MENSAGENS (MODO 1 / MODO 2)
 * ============================================================================ */

#define RX_DEFRAMER_MAX_SYNC_SYMS  (RX_MAX_SYNC_WORD_LEN * 4U)

typedef enum {
    RX_STATE_SYNC_SEARCH = 0,
    RX_STATE_HEADER,
    RX_STATE_PAYLOAD,
    RX_STATE_CRC,
    RX_STATE_RAW_STREAM,
} rx_deframer_state_t;

typedef struct {
    bool     frame_ready;         /**< true quando um quadro Modo 2 completo foi recebido. */
    bool     crc_ok;              /**< Resultado da verificação CRC-16 no Modo 2. */
    uint8_t  payload[RX_MAX_PAYLOAD_BYTES + 1U]; /**< Payload nulo-terminado. */
    size_t   payload_len;         /**< Comprimento útil do payload. */
    uint16_t tx_baud;             /**< Taxa de símbolos reportada no cabeçalho Modo 2. */
    uint8_t  locked_rot;          /**< Rotação de quadrante resolvida (0..3 -> 0/90/180/270°). */
    uint16_t rx_crc;              /**< CRC recebido no quadro. */
    uint16_t calc_crc;            /**< CRC calculado localmente. */

    bool     raw_byte_ready;      /**< true quando 1 byte foi reconstruído no Modo 1. */
    uint8_t  raw_byte;            /**< Byte reconstruído no Modo 1. */
    uint8_t  raw_sym_group[4];    /**< Os 4 símbolos (0..3) que formaram o byte atual. */
} rx_deframer_event_t;

typedef struct {
    qam_rx_config_t cfg;
    rx_deframer_state_t state;

    /* Buffer deslizante de amostras I/Q para busca simultânea nas 4 rotações */
    float   sync_i[RX_DEFRAMER_MAX_SYNC_SYMS];
    float   sync_q[RX_DEFRAMER_MAX_SYNC_SYMS];
    size_t  sync_fill;

    uint8_t locked_rot;
    uint8_t sym_acc[4];
    uint8_t sym_acc_idx;

    /* Campos do quadro Modo 2 */
    uint8_t  hdr[4];
    size_t   hdr_idx;
    rx_bit_order_t payload_order;
    uint16_t tx_baud;
    uint8_t  payload[RX_MAX_PAYLOAD_BYTES + 1U];
    size_t   payload_len;
    size_t   payload_idx;
    uint8_t  crc_bytes[2];
    size_t   crc_idx;
} rx_deframer_t;

void rx_deframer_init(rx_deframer_t *df, const qam_rx_config_t *cfg);
void rx_deframer_reset(rx_deframer_t *df);
void rx_deframer_push_symbol(rx_deframer_t *df, float i_hat, float q_hat, rx_deframer_event_t *out);
void rx_deframer_on_burst_end(rx_deframer_t *df);

#ifdef __cplusplus
}
#endif

#endif /* RX_DEFRAMER_H */
