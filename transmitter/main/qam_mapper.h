#ifndef QAM_MAPPER_H
#define QAM_MAPPER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "qam_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MAPEAMENTO 4-QAM, CONVERSÃO ASCII -> BITS -> SÍMBOLOS E CRC-16
 * ============================================================================ */

/**
 * @brief Converte 1 byte ASCII/binário em 4 símbolos 4-QAM (valores 0..3) conforme bit_order.
 *
 * @param byte      Byte de entrada (8 bits).
 * @param bit_order Convenção de ordenação dos bits.
 * @param out_syms  Vetor de saída com exatamente 4 símbolos.
 */
void qam_mapper_byte_to_symbols(uint8_t byte, qam_bit_order_t bit_order, uint8_t out_syms[4]);

/**
 * @brief Reconstrói 1 byte a partir de 4 símbolos 4-QAM conforme bit_order (útil para validação).
 *
 * @param in_syms   Vetor com 4 símbolos (0..3).
 * @param bit_order Convenção de ordenação dos bits.
 * @return Byte reconstruído.
 */
uint8_t qam_mapper_symbols_to_byte(const uint8_t in_syms[4], qam_bit_order_t bit_order);

/**
 * @brief Converte um buffer de bytes em uma sequência linear de símbolos 4-QAM.
 *
 * @param data          Ponteiro para os bytes de entrada.
 * @param data_len      Quantidade de bytes de entrada.
 * @param bit_order     Ordem de bits selecionada.
 * @param out_syms      Buffer de destino para símbolos.
 * @param max_syms      Capacidade máxima de out_syms (em símbolos).
 * @param out_sym_count Ponteiro opcional que recebe a quantidade gerada.
 * @return true se todos os bytes couberam em out_syms; false em erro ou buffer insuficiente.
 */
bool qam_mapper_bytes_to_symbols(const uint8_t *data, size_t data_len,
                                 qam_bit_order_t bit_order,
                                 uint8_t *out_syms, size_t max_syms,
                                 size_t *out_sym_count);

/**
 * @brief Obtém os componentes (I, Q) e metadados da constelação para um símbolo (0..3).
 *
 * @param cfg     Configuração ativa (contém a tabela de constelação).
 * @param sym     Índice do símbolo (0..3).
 * @param out_pt  Ponteiro para receber o ponto da constelação.
 * @return true se sym é válido (< 4) e ponteiros não são nulos.
 */
bool qam_mapper_get_point(const qam_tx_config_t *cfg, uint8_t sym, qam_symbol_point_t *out_pt);

/**
 * @brief Calcula o CRC-16/CCITT-FALSE (polinômio 0x1021, inicial 0xFFFF).
 *
 * @param data Ponteiro para os bytes.
 * @param len  Tamanho em bytes.
 * @return Valor CRC-16 calculado.
 */
uint16_t qam_crc16_ccitt(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* QAM_MAPPER_H */
