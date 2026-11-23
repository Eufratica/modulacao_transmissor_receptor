#ifndef QAM_FRAMER_H
#define QAM_FRAMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "qam_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ENQUADRAMENTO DE TRANSMISSÃO (MODO 1 PROFESSOR / MODO 2 QUADRO PRÓPRIO)
 * ============================================================================ */

#define QAM_FRAME_PROTOCOL_VERSION  0x1U
#define QAM_MAX_PREAMBLE_BYTES      16U

/**
 * @brief Representa um quadro pronto de símbolos 4-QAM (valores 0..3) para modulação.
 */
typedef struct {
    uint8_t symbols[QAM_MAX_FRAME_SYMBOLS]; /**< Sequência linear de símbolos (cada item em 0..3). */
    size_t symbol_count;                    /**< Quantidade total de símbolos no quadro. */
    size_t header_symbol_count;             /**< Quantidade de símbolos de preâmbulo/sync/cabeçalho. */
    size_t payload_symbol_count;            /**< Quantidade de símbolos de dados úteis. */
    uint16_t crc16;                         /**< CRC-16 calculado (válido no Modo 2; 0 no Modo 1). */
} qam_tx_frame_t;

/**
 * @brief Constrói a sequência de símbolos 4-QAM para uma mensagem ASCII/binária conforme o modo ativo.
 *
 * - No Modo 1 (Professor): emite somente os símbolos dos dados (preâmbulo/sync só se habilitados na config).
 * - No Modo 2 (Quadro Próprio): emite [Preâmbulo][Sync Word][Flags+Baud+Len][Payload][CRC-16].
 *
 * @param cfg       Configuração ativa do transmissor.
 * @param data      Buffer de entrada com a mensagem.
 * @param data_len  Tamanho da mensagem em bytes (0..QAM_MAX_PAYLOAD_BYTES).
 * @param out_frame Estrutura de saída que receberá os símbolos.
 * @return true em sucesso; false se argumentos inválidos ou mensagem exceder o limite.
 */
bool qam_framer_build_frame(const qam_tx_config_t *cfg,
                            const uint8_t *data, size_t data_len,
                            qam_tx_frame_t *out_frame);

#ifdef __cplusplus
}
#endif

#endif /* QAM_FRAMER_H */
