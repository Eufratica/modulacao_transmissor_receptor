#ifndef UART_CLI_H
#define UART_CLI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * INTERFACE DE TERMINAL SERIAL (USB / UART0)
 * ============================================================================
 * Recebe mensagens ASCII digitadas pelo usuário para transmissão 4-QAM e
 * comandos iniciados por '/' para reconfigurar parâmetros em tempo real.
 * ============================================================================ */

/**
 * @brief Inicializa o driver UART0 em modo interrupção/VFS e cria a task CLI no Core 0.
 *
 * @return ESP_OK em sucesso ou erro ESP-IDF.
 */
esp_err_t uart_cli_start(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_CLI_H */
