#ifndef RX_CLI_H
#define RX_CLI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o terminal serial UART0 do receptor para configuração e diagnóstico.
 */
esp_err_t rx_cli_start(void);

#ifdef __cplusplus
}
#endif

#endif /* RX_CLI_H */
