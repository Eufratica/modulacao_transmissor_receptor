#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "rx_cli.h"
#include "rx_config.h"
#include "rx_engine.h"
#include "rx_selftest.h"
#include "rx_telem.h"

static const char *TAG = "receiver_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Inicializando Receptor 4-QAM (ESP-IDF v4.4.8)");

    if (!rx_selftest_run_all(true)) {
        ESP_LOGE(TAG, "Auto-teste interno em malha fechada falhou! Abortando.");
        return;
    }

    qam_rx_config_t cfg;
    rx_config_set_defaults(&cfg);

    esp_err_t err = rx_engine_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro fatal ao iniciar rx_engine: %s", esp_err_to_name(err));
        return;
    }

    err = rx_cli_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro fatal ao iniciar rx_cli: %s", esp_err_to_name(err));
        return;
    }

    err = rx_telem_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro fatal ao iniciar telemetria RX: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Receptor 4-QAM pronto no ADC1 canal %d (GPIO %d).",
             cfg.adc1_channel, rx_config_adc1_channel_to_gpio(cfg.adc1_channel));
}
