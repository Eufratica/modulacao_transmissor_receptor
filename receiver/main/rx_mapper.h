#ifndef RX_MAPPER_H
#define RX_MAPPER_H

#include <stddef.h>
#include <stdint.h>
#include "rx_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DECISÃO I/Q, ROTAÇÃO DE CONSTELAÇÃO, MAPEAMENTO INVERSO E CRC-16
 * ============================================================================ */

/**
 * @brief Aplica rotação de quadrante (0=0°, 1=+90°, 2=+180°, 3=+270°) ao par (I, Q).
 */
void rx_mapper_rotate_iq(float i_in, float q_in, uint8_t rot_quad, float *i_out, float *q_out);

/**
 * @brief Decide qual símbolo 4-QAM (0..3) está mais próximo de (i_hat, q_hat) na tabela ativa.
 */
uint8_t rx_mapper_decide_symbol(const qam_rx_config_t *cfg, float i_hat, float q_hat, uint8_t rot_quad);

/**
 * @brief Reconstrói 1 byte ASCII a partir de 4 símbolos (0..3) conforme bit_order.
 */
uint8_t rx_mapper_symbols_to_byte(const uint8_t syms[4], rx_bit_order_t bit_order);

/**
 * @brief Calcula CRC-16/CCITT-FALSE (0x1021, init 0xFFFF) idêntico ao transmissor.
 */
uint16_t rx_crc16_ccitt(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RX_MAPPER_H */
