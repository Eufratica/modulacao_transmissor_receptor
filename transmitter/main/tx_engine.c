#include "tx_engine.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "pwm_driver.h"
#include "qam_mapper.h"
#include "qam_modulator.h"
#include "tx_telem.h"

static const char *TAG = "tx_engine";

#define TX_FRAME_QUEUE_LEN      4U
#define TX_DSP_CHUNK_SAMPLES    128U
#define TX_DSP_TARGET_FILL      768U /* Mantém ~19 ms de reserva a 40 kHz */

typedef struct {
    uint32_t       frame_id;
    uint16_t       payload_len;
    uint8_t        payload[QAM_MAX_PAYLOAD_BYTES];
    qam_tx_frame_t frame;
} tx_queued_frame_t;

static SemaphoreHandle_t s_cfg_mutex = NULL;
static SemaphoreHandle_t s_init_done_sem = NULL;
static QueueHandle_t s_frame_queue = NULL;
static TaskHandle_t s_dsp_task = NULL;

static qam_tx_config_t s_active_cfg;
static qam_modulator_t s_mod;
static esp_err_t s_init_result = ESP_FAIL;

static volatile bool s_reconfig_pending = false;
static volatile bool s_hw_reconfig_needed = false;

static volatile uint32_t s_frames_sent = 0U;
static volatile uint32_t s_symbols_sent = 0U;
static volatile uint32_t s_queue_dropped = 0U;
static volatile bool s_is_transmitting = false;
static volatile uint32_t s_next_frame_id = 1U;

/* Estado local exclusivo da task DSP */
static tx_queued_frame_t s_cur_item;
static size_t s_cur_sym_idx = 0U;
static bool s_has_active_frame = false;
static bool s_was_dc_idle = true;

/* Estado do Modo 3 contínuo */
static const uint8_t s_cycle_order[4] = { 0U, 1U, 3U, 2U }; /* 00(45°)->01(135°)->11(225°)->10(315°) */
static uint8_t s_cycle_idx = 0U;
static uint8_t s_cont_syms[QAM_MAX_CONT_TEXT_LEN * 4U];
static size_t s_cont_sym_count = 0U;
static size_t s_cont_sym_idx = 0U;

static void rebuild_continuous_symbols(const qam_tx_config_t *cfg)
{
    size_t len = strnlen(cfg->cont_text, sizeof(cfg->cont_text));
    if (len == 0U) {
        const uint8_t fallback[4] = { '4', 'Q', 'A', 'M' };
        (void)qam_mapper_bytes_to_symbols(fallback, sizeof(fallback), cfg->bit_order,
                                          s_cont_syms, sizeof(s_cont_syms), &s_cont_sym_count);
    } else {
        (void)qam_mapper_bytes_to_symbols((const uint8_t *)cfg->cont_text, len, cfg->bit_order,
                                          s_cont_syms, sizeof(s_cont_syms), &s_cont_sym_count);
    }
    if (s_cont_sym_idx >= s_cont_sym_count) {
        s_cont_sym_idx = 0U;
    }
}

static void handle_pending_reconfig(qam_tx_config_t *local_cfg)
{
    if (!s_reconfig_pending) {
        return;
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    qam_tx_config_t next_cfg = s_active_cfg;
    bool hw_needed = s_hw_reconfig_needed;
    s_reconfig_pending = false;
    s_hw_reconfig_needed = false;
    xSemaphoreGive(s_cfg_mutex);

    uint32_t period_ticks = pwm_driver_calc_period_ticks(next_cfg.pwm_freq_hz, next_cfg.pwm_symmetric);
    (void)qam_modulator_reconfigure(&s_mod, &next_cfg, period_ticks, false);

    if (hw_needed) {
        uint16_t idle_cmp = qam_modulator_idle_dc_cmp(&s_mod);
        esp_err_t err = pwm_driver_reconfigure(next_cfg.pwm_gpio,
                                               next_cfg.pwm_freq_hz,
                                               next_cfg.sample_rate_hz,
                                               next_cfg.pwm_symmetric,
                                               next_cfg.pwm_invert_output,
                                               idle_cmp);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao reconfigurar PWM: %s", esp_err_to_name(err));
        }
    } else {
        pwm_driver_set_idle_cmp(qam_modulator_idle_dc_cmp(&s_mod));
    }

    if (next_cfg.tx_mode == QAM_TX_MODE_3_CONTINUOUS) {
        s_has_active_frame = false;
        s_is_transmitting = true;
    } else {
        s_is_transmitting = s_has_active_frame;
    }

    rebuild_continuous_symbols(&next_cfg);
    *local_cfg = next_cfg;
}

static uint16_t next_sample_mode_1_or_2(const qam_tx_config_t *cfg)
{
    if (!s_has_active_frame) {
        if (xQueueReceive(s_frame_queue, &s_cur_item, 0) == pdTRUE && s_cur_item.frame.symbol_count > 0U) {
            s_has_active_frame = true;
            s_cur_sym_idx = 0U;
            s_is_transmitting = true;
            if (s_was_dc_idle) {
                /* Começando do silêncio DC: inicia portadora exatamente em t = 0 */
                qam_modulator_reset_phase(&s_mod);
            }
            s_was_dc_idle = false;
            tx_telem_emit_frame_event(s_cur_item.frame_id, cfg, s_cur_item.payload,
                                      s_cur_item.payload_len, &s_cur_item.frame, TX_FRAME_EV_STARTED);
            if (tx_telem_is_enabled()) {
                tx_telem_request_pwm_capture();
            }
        }
    }

    if (s_has_active_frame) {
        bool sym_done = false;
        uint8_t sym = s_cur_item.frame.symbols[s_cur_sym_idx];
        uint16_t cmp = qam_modulator_next_symbol_cmp(&s_mod, sym, &sym_done);
        if (sym_done) {
            s_symbols_sent++;
            s_cur_sym_idx++;
            if (s_cur_sym_idx >= s_cur_item.frame.symbol_count) {
                s_frames_sent++;
                tx_telem_emit_frame_event(s_cur_item.frame_id, cfg, s_cur_item.payload,
                                          s_cur_item.payload_len, &s_cur_item.frame, TX_FRAME_EV_DONE);
                s_has_active_frame = false;
                /* Verifica imediatamente se há outro quadro encadeado sem quebra */
                if (xQueueReceive(s_frame_queue, &s_cur_item, 0) == pdTRUE && s_cur_item.frame.symbol_count > 0U) {
                    s_has_active_frame = true;
                    s_cur_sym_idx = 0U;
                    s_is_transmitting = true;
                    tx_telem_emit_frame_event(s_cur_item.frame_id, cfg, s_cur_item.payload,
                                              s_cur_item.payload_len, &s_cur_item.frame, TX_FRAME_EV_STARTED);
                } else {
                    s_is_transmitting = false;
                }
            }
        }
        return cmp;
    }

    /* Estado ocioso */
    if (cfg->idle_mode == QAM_IDLE_PILOT_SYMBOL_00) {
        s_was_dc_idle = false;
        bool sym_done = false;
        return qam_modulator_next_symbol_cmp(&s_mod, 0U, &sym_done);
    }

    s_was_dc_idle = true;
    return qam_modulator_idle_dc_cmp(&s_mod);
}

static uint16_t next_sample_mode_3(const qam_tx_config_t *cfg)
{
    s_was_dc_idle = false;
    s_is_transmitting = true;

    bool sym_done = false;
    uint16_t cmp = 1U;

    switch (cfg->cont_pattern) {
    case QAM_CONT_PATTERN_RAW_CARRIER:
        cmp = qam_modulator_next_carrier_cmp(&s_mod);
        break;

    case QAM_CONT_PATTERN_FIXED_SYMBOL:
        cmp = qam_modulator_next_symbol_cmp(&s_mod, cfg->cont_fixed_symbol, &sym_done);
        if (sym_done) {
            s_symbols_sent++;
        }
        break;

    case QAM_CONT_PATTERN_CYCLE_4:
        cmp = qam_modulator_next_symbol_cmp(&s_mod, s_cycle_order[s_cycle_idx], &sym_done);
        if (sym_done) {
            s_symbols_sent++;
            s_cycle_idx = (uint8_t)((s_cycle_idx + 1U) & 0x03U);
        }
        break;

    case QAM_CONT_PATTERN_TEXT:
    default:
        if (s_cont_sym_count == 0U) {
            cmp = qam_modulator_idle_dc_cmp(&s_mod);
        } else {
            cmp = qam_modulator_next_symbol_cmp(&s_mod, s_cont_syms[s_cont_sym_idx], &sym_done);
            if (sym_done) {
                s_symbols_sent++;
                s_cont_sym_idx++;
                if (s_cont_sym_idx >= s_cont_sym_count) {
                    s_cont_sym_idx = 0U;
                    s_frames_sent++;
                }
            }
        }
        break;
    }

    return cmp;
}

static void tx_dsp_task(void *arg)
{
    (void)arg;
    qam_tx_config_t local_cfg;

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    local_cfg = s_active_cfg;
    xSemaphoreGive(s_cfg_mutex);

    uint32_t period_ticks = pwm_driver_calc_period_ticks(local_cfg.pwm_freq_hz, local_cfg.pwm_symmetric);
    if (!qam_modulator_init(&s_mod, &local_cfg, period_ticks)) {
        ESP_LOGE(TAG, "Falha ao inicializar modulador 4-QAM");
        s_init_result = ESP_FAIL;
        xSemaphoreGive(s_init_done_sem);
        vTaskDelete(NULL);
        return;
    }

    rebuild_continuous_symbols(&local_cfg);
    uint16_t idle_cmp = qam_modulator_idle_dc_cmp(&s_mod);

    /* Inicializa o MCPWM e registra a ISR TEZ neste mesmo núcleo (Core 1) */
    s_init_result = pwm_driver_init(local_cfg.pwm_gpio,
                                    local_cfg.pwm_freq_hz,
                                    local_cfg.sample_rate_hz,
                                    local_cfg.pwm_symmetric,
                                    local_cfg.pwm_invert_output,
                                    idle_cmp,
                                    xTaskGetCurrentTaskHandle());
    xSemaphoreGive(s_init_done_sem);
    if (s_init_result != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar driver PWM: %s", esp_err_to_name(s_init_result));
        vTaskDelete(NULL);
        return;
    }

    uint16_t chunk[TX_DSP_CHUNK_SAMPLES];

    while (1) {
        handle_pending_reconfig(&local_cfg);

        /* Mantém o ring buffer preenchido até TX_DSP_TARGET_FILL para evitar latência excessiva */
        while (1) {
            size_t free_slots = pwm_driver_free_space();
            size_t used_slots = PWM_DRIVER_RINGBUF_LEN - free_slots;
            if (used_slots >= TX_DSP_TARGET_FILL || free_slots == 0U) {
                break;
            }

            size_t needed = TX_DSP_TARGET_FILL - used_slots;
            if (needed > TX_DSP_CHUNK_SAMPLES) {
                needed = TX_DSP_CHUNK_SAMPLES;
            }
            if (needed > free_slots) {
                needed = free_slots;
            }

            for (size_t i = 0; i < needed; i++) {
                uint16_t sample_cmp;
                if (local_cfg.tx_mode == QAM_TX_MODE_3_CONTINUOUS) {
                    sample_cmp = next_sample_mode_3(&local_cfg);
                } else {
                    sample_cmp = next_sample_mode_1_or_2(&local_cfg);
                }
                chunk[i] = sample_cmp;
                if (tx_telem_pwm_capture_active()) {
                    uint32_t fid = s_has_active_frame ? s_cur_item.frame_id : 0U;
                    tx_telem_pwm_capture_push(sample_cmp, fid, local_cfg.sample_rate_hz,
                                              (uint16_t)s_mod.pwm_period_ticks);
                }
            }
            (void)pwm_driver_push_samples(chunk, needed);
        }

        /* Aguarda notificação da ISR TEZ ou chegada de nova mensagem/reconfiguração */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
}

esp_err_t tx_engine_start(const qam_tx_config_t *initial_cfg)
{
    if (s_dsp_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (initial_cfg != NULL) {
        s_active_cfg = *initial_cfg;
    } else {
        qam_config_set_defaults(&s_active_cfg);
    }

    char err_msg[128];
    if (!qam_config_validate(&s_active_cfg, err_msg, sizeof(err_msg))) {
        ESP_LOGE(TAG, "Configuracao inicial invalida: %s", err_msg);
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg_mutex = xSemaphoreCreateMutex();
    s_init_done_sem = xSemaphoreCreateBinary();
    s_frame_queue = xQueueCreate(TX_FRAME_QUEUE_LEN, sizeof(tx_queued_frame_t));

    if (!s_cfg_mutex || !s_init_done_sem || !s_frame_queue) {
        ESP_LOGE(TAG, "Falha ao alocar objetos FreeRTOS do tx_engine");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(tx_dsp_task,
                                            "tx_dsp_task",
                                            4096,
                                            NULL,
                                            configMAX_PRIORITIES - 2,
                                            &s_dsp_task,
                                            1); /* Core 1 dedicado a DSP + PWM */
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task tx_dsp_task");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_init_done_sem, portMAX_DELAY);
    return s_init_result;
}

void tx_engine_get_config(qam_tx_config_t *out_cfg)
{
    if (!out_cfg || !s_cfg_mutex) {
        return;
    }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    *out_cfg = s_active_cfg;
    xSemaphoreGive(s_cfg_mutex);
}

esp_err_t tx_engine_apply_config(const qam_tx_config_t *new_cfg, char *err_buf, size_t err_sz)
{
    if (!new_cfg || !s_cfg_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!qam_config_validate(new_cfg, err_buf, err_sz)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    bool hw_changed = (new_cfg->pwm_gpio != s_active_cfg.pwm_gpio) ||
                      (new_cfg->pwm_freq_hz != s_active_cfg.pwm_freq_hz) ||
                      (new_cfg->sample_rate_hz != s_active_cfg.sample_rate_hz) ||
                      (new_cfg->pwm_symmetric != s_active_cfg.pwm_symmetric) ||
                      (new_cfg->pwm_invert_output != s_active_cfg.pwm_invert_output);
    s_active_cfg = *new_cfg;
    if (hw_changed) {
        s_hw_reconfig_needed = true;
    }
    s_reconfig_pending = true;
    xSemaphoreGive(s_cfg_mutex);

    if (s_dsp_task != NULL) {
        xTaskNotifyGive(s_dsp_task);
    }
    if (tx_telem_is_enabled()) {
        tx_telem_send_hello_now();
    }
    return ESP_OK;
}

esp_err_t tx_engine_send_message(const uint8_t *data, size_t len, qam_tx_frame_t *out_frame_info)
{
    if (!data || len == 0U || len > QAM_MAX_PAYLOAD_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    qam_tx_config_t cfg;
    tx_engine_get_config(&cfg);

    if (cfg.tx_mode == QAM_TX_MODE_3_CONTINUOUS) {
        size_t copy_len = (len < sizeof(cfg.cont_text) - 1U) ? len : (sizeof(cfg.cont_text) - 1U);
        memcpy(cfg.cont_text, data, copy_len);
        cfg.cont_text[copy_len] = '\0';
        cfg.cont_pattern = QAM_CONT_PATTERN_TEXT;
        if (out_frame_info) {
            (void)qam_framer_build_frame(&cfg, data, len, out_frame_info);
        }
        return tx_engine_apply_config(&cfg, NULL, 0U);
    }

    /* Aloca temporariamente em heap para não sobrecarregar a pilha da task CLI */
    tx_queued_frame_t *qitem = (tx_queued_frame_t *)pvPortMalloc(sizeof(tx_queued_frame_t));
    if (!qitem) {
        return ESP_ERR_NO_MEM;
    }

    if (!qam_framer_build_frame(&cfg, data, len, &qitem->frame)) {
        vPortFree(qitem);
        return ESP_ERR_INVALID_ARG;
    }

    qitem->frame_id = s_next_frame_id++;
    qitem->payload_len = (uint16_t)len;
    memcpy(qitem->payload, data, len);

    if (out_frame_info) {
        *out_frame_info = qitem->frame;
    }

    if (xQueueSend(s_frame_queue, qitem, pdMS_TO_TICKS(50)) != pdTRUE) {
        s_queue_dropped++;
        tx_telem_emit_frame_event(qitem->frame_id, &cfg, data, len, &qitem->frame, TX_FRAME_EV_DROPPED);
        vPortFree(qitem);
        return ESP_ERR_NO_MEM;
    }

    tx_telem_emit_frame_event(qitem->frame_id, &cfg, data, len, &qitem->frame, TX_FRAME_EV_QUEUED);
    vPortFree(qitem);
    if (s_dsp_task != NULL) {
        xTaskNotifyGive(s_dsp_task);
    }
    if (tx_telem_is_enabled()) {
        tx_telem_send_hello_now();
    }
    return ESP_OK;
}

void tx_engine_get_stats(tx_engine_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    out_stats->frames_sent   = s_frames_sent;
    out_stats->symbols_sent  = s_symbols_sent;
    out_stats->queue_dropped = s_queue_dropped;
    out_stats->transmitting  = s_is_transmitting;
}
