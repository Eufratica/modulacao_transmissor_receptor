#ifndef TX_ENGINE_H
#define TX_ENGINE_H

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
 * MOTOR DE TRANSMISSÃO 4-QAM EM TEMPO REAL
 * ============================================================================
 * Coordena configuração thread-safe, fila de quadros, síntese DSP em tarefa
 * dedicada (Core 1) e envio de amostras ao driver MCPWM determinístico.
 * ============================================================================ */

typedef struct {
    uint32_t frames_sent;        /**< Total de quadros/mensagens concluídos. */
    uint32_t symbols_sent;       /**< Total de símbolos transmitidos desde o boot. */
    uint32_t queue_dropped;      /**< Mensagens descartadas por fila cheia. */
    bool     transmitting;       /**< true enquanto um quadro ativo está em modulação. */
} tx_engine_stats_t;

/**
 * @brief Inicializa o motor de transmissão, cria a task DSP no Core 1 e inicia o MCPWM.
 *
 * @param initial_cfg Configuração inicial validada (ou NULL para usar padrões de fábrica).
 * @return ESP_OK em sucesso ou erro ESP-IDF.
 */
esp_err_t tx_engine_start(const qam_tx_config_t *initial_cfg);

/**
 * @brief Obtém uma cópia thread-safe da configuração ativa.
 */
void tx_engine_get_config(qam_tx_config_t *out_cfg);

/**
 * @brief Valida e aplica nova configuração (atualiza modulador e MCPWM se necessário).
 *
 * @param new_cfg   Nova configuração proposta.
 * @param err_buf   Buffer opcional para mensagem de erro.
 * @param err_sz    Tamanho de err_buf.
 * @return ESP_OK se válida e agendada; ESP_ERR_INVALID_ARG se inválida.
 */
esp_err_t tx_engine_apply_config(const qam_tx_config_t *new_cfg, char *err_buf, size_t err_sz);

/**
 * @brief Enfileira uma mensagem ASCII/binária para transmissão no modo ativo (Modo 1 ou Modo 2).
 *
 * No Modo 3, atualiza o texto contínuo caso cont_pattern == QAM_CONT_PATTERN_TEXT.
 *
 * @param data           Bytes da mensagem.
 * @param len            Quantidade de bytes.
 * @param out_frame_info Ponteiro opcional que recebe o quadro de símbolos gerado (para inspeção CLI).
 * @return ESP_OK em sucesso, ESP_ERR_NO_MEM se fila cheia, ESP_ERR_INVALID_ARG se exceder limite.
 */
esp_err_t tx_engine_send_message(const uint8_t *data, size_t len, qam_tx_frame_t *out_frame_info);

/**
 * @brief Retorna estatísticas combinadas do motor de transmissão.
 */
void tx_engine_get_stats(tx_engine_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* TX_ENGINE_H */
