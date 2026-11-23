#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "qam_config.h"
#include "qam_selftest.h"
#include "tx_engine.h"
#include "tx_telem.h"
#include "uart_cli.h"

static const char *TAG = "transmitter_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Inicializando Transmissor 4-QAM (ESP-IDF v4.4.8)");

    /* Executa verificação automática da matemática 4-QAM, mapeamento e segurança elétrica */
    if (!qam_selftest_run_all(true)) {
        ESP_LOGE(TAG, "Auto-teste interno falhou! Abortando inicializacao do transmissor.");
        return;
    }

    qam_tx_config_t cfg;
    qam_config_set_defaults(&cfg);

    esp_err_t err = tx_engine_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro fatal ao iniciar tx_engine: %s", esp_err_to_name(err));
        return;
    }

    err = uart_cli_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro fatal ao iniciar interface UART CLI: %s", esp_err_to_name(err));
        return;
    }

    err = tx_telem_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro fatal ao iniciar telemetria TX: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Transmissor 4-QAM pronto no GPIO %d.", cfg.pwm_gpio);
}
