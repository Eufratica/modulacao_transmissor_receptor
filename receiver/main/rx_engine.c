#include "rx_engine.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "adc_sampler.h"
#include "qam_demod.h"
#include "rx_deframer.h"
#include "rx_mapper.h"
#include "rx_telem.h"

static const char *TAG = "rx_engine";

static SemaphoreHandle_t s_cfg_mutex = NULL;
static SemaphoreHandle_t s_init_sem = NULL;
static TaskHandle_t s_rx_task = NULL;

static qam_rx_config_t s_active_cfg;
static qam_demod_t s_demod;
static rx_deframer_t s_deframer;
static esp_err_t s_init_err = ESP_FAIL;

static volatile bool s_reconfig_pending = false;
static volatile bool s_adc_reconfig_needed = false;
static volatile bool s_monitor_enabled = false;

static volatile uint32_t s_frames_ok = 0U;
static volatile uint32_t s_frames_crc_err = 0U;
static volatile uint32_t s_bytes_raw_rx = 0U;
static volatile uint32_t s_bursts_detected = 0U;
static uint32_t s_burst_id = 0U;

static uint8_t s_raw_line[RX_MAX_PAYLOAD_BYTES + 1U];
static size_t  s_raw_line_len = 0U;

static void flush_raw_burst_line(void)
{
    if (s_raw_line_len == 0U) {
        return;
    }
    s_raw_line[s_raw_line_len] = '\0';
    rx_telem_console_lock();
    printf("\nRX [MODO 1 | %u bytes | rot=%udeg | df=%+.2fHz]: \"",
           (unsigned)s_raw_line_len,
           (unsigned)((s_deframer.locked_rot & 0x03U) * 90U),
           s_demod.freq_offset_hz);
    for (size_t i = 0U; i < s_raw_line_len; i++) {
        uint8_t c = s_raw_line[i];
        putchar((c >= 32U && c <= 126U) ? (int)c : '.');
    }
    printf("\" (hex:");
    for (size_t i = 0U; i < s_raw_line_len && i < 16U; i++) {
        printf(" %02X", (unsigned)s_raw_line[i]);
    }
    if (s_raw_line_len > 16U) {
        printf(" ...");
    }
    printf(")\n4QAM-RX> ");
    fflush(stdout);
    rx_telem_console_unlock();
    s_raw_line_len = 0U;
}

static void handle_pending_reconfig(qam_rx_config_t *local_cfg)
{
    if (!s_reconfig_pending) {
        return;
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    qam_rx_config_t next_cfg = s_active_cfg;
    bool adc_needed = s_adc_reconfig_needed;
    s_reconfig_pending = false;
    s_adc_reconfig_needed = false;
    xSemaphoreGive(s_cfg_mutex);

    if (adc_needed) {
        esp_err_t err = adc_sampler_reconfigure(next_cfg.adc1_channel,
                                                next_cfg.sample_rate_hz,
                                                next_cfg.adc_use_cal);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao reconfigurar ADC1: %s", esp_err_to_name(err));
        }
    }

    (void)qam_demod_reconfigure(&s_demod, &next_cfg);
    rx_deframer_init(&s_deframer, &next_cfg);
    s_raw_line_len = 0U;
    *local_cfg = next_cfg;
}

static void process_demod_event(const qam_rx_config_t *cfg, const qam_demod_out_t *dout)
{
    if (dout->burst_started) {
        s_bursts_detected++;
        s_burst_id++;
        rx_deframer_reset(&s_deframer);
        s_raw_line_len = 0U;
        rx_telem_on_burst_event(0U, s_burst_id, s_demod.dc_est_mv, s_demod.amp_pk_est_mv);
        if (s_monitor_enabled) {
            rx_telem_console_lock();
            printf("\n[MON] Rajada detectada (DC=%.0fmV, Vpk=%.0fmV)\n",
                   s_demod.dc_est_mv, s_demod.amp_pk_est_mv);
            fflush(stdout);
            rx_telem_console_unlock();
        }
    }

    if (dout->symbol_ready) {
        rx_deframer_event_t ev;
        rx_deframer_push_symbol(&s_deframer, dout->i_hat, dout->q_hat, &ev);
        uint8_t rot_used = ev.frame_ready ? ev.locked_rot : s_deframer.locked_rot;
        uint8_t sym_dec = rx_mapper_decide_symbol(cfg, dout->i_hat, dout->q_hat, rot_used);

        rx_telem_on_symbol_dsp(s_burst_id, cfg, &s_demod, dout, &s_deframer, sym_dec,
                               ev.frame_ready || ev.raw_byte_ready);

        if (s_monitor_enabled && (s_demod.total_symbols % 10U) == 0U) {
            rx_telem_console_lock();
            printf("[MON] sym=%u(%s) I=%+.2f Q=%+.2f | df=%+.2fHz | Vpk=%.0fmV | SNR=%.1fdB\n",
                   (unsigned)sym_dec, cfg->constellation[sym_dec].bits_label,
                   dout->i_hat, dout->q_hat, s_demod.freq_offset_hz,
                   s_demod.amp_pk_est_mv, dout->snr_est_db);
            fflush(stdout);
            rx_telem_console_unlock();
        }

        if (ev.frame_ready) {
            rx_telem_on_frame_event(s_burst_id,
                                    (uint8_t)cfg->rx_mode,
                                    ev.crc_ok,
                                    ev.locked_rot,
                                    (uint8_t)s_deframer.payload_order,
                                    ev.tx_baud,
                                    ev.rx_crc,
                                    ev.calc_crc,
                                    s_demod.freq_offset_hz,
                                    s_demod.dc_est_mv,
                                    s_demod.amp_pk_est_mv,
                                    dout->snr_est_db,
                                    ev.payload,
                                    ev.payload_len);
            if (ev.crc_ok) {
                s_frames_ok++;
                rx_telem_console_lock();
                printf("\nRX [MODO 2 | CRC=OK 0x%04X | rot=%udeg | baud=%u | df=%+.2fHz | SNR~%.1fdB]: \"%s\"\n4QAM-RX> ",
                       (unsigned)ev.rx_crc,
                       (unsigned)((ev.locked_rot & 0x03U) * 90U),
                       (unsigned)ev.tx_baud,
                       s_demod.freq_offset_hz,
                       dout->snr_est_db,
                       (const char *)ev.payload);
                fflush(stdout);
                rx_telem_console_unlock();
            } else {
                s_frames_crc_err++;
                rx_telem_console_lock();
                printf("\nRX [MODO 2 | CRC=FALHOU rx=0x%04X calc=0x%04X | rot=%udeg | len=%u]\n4QAM-RX> ",
                       (unsigned)ev.rx_crc,
                       (unsigned)ev.calc_crc,
                       (unsigned)((ev.locked_rot & 0x03U) * 90U),
                       (unsigned)ev.payload_len);
                fflush(stdout);
                rx_telem_console_unlock();
            }
        }

        if (ev.raw_byte_ready) {
            s_bytes_raw_rx++;
            if (s_raw_line_len < RX_MAX_PAYLOAD_BYTES) {
                s_raw_line[s_raw_line_len++] = ev.raw_byte;
            }
            if (s_raw_line_len >= RX_MAX_PAYLOAD_BYTES) {
                flush_raw_burst_line();
            }
        }
    }

    if (dout->burst_ended) {
        if (cfg->rx_mode == RX_MODE_1_RAW_STREAM) {
            if (s_raw_line_len > 0U) {
                rx_telem_on_frame_event(s_burst_id,
                                        (uint8_t)cfg->rx_mode,
                                        true,
                                        s_deframer.locked_rot,
                                        (uint8_t)cfg->bit_order,
                                        (uint16_t)cfg->symbol_rate_baud,
                                        0U,
                                        0U,
                                        s_demod.freq_offset_hz,
                                        s_demod.dc_est_mv,
                                        s_demod.amp_pk_est_mv,
                                        0.0f,
                                        s_raw_line,
                                        s_raw_line_len);
            }
            flush_raw_burst_line();
        }
        rx_telem_on_burst_event(1U, s_burst_id, s_demod.dc_est_mv, s_demod.amp_pk_est_mv);
        rx_deframer_on_burst_end(&s_deframer);
        if (s_monitor_enabled) {
            rx_telem_console_lock();
            printf("[MON] Fim de rajada.\n");
            fflush(stdout);
            rx_telem_console_unlock();
        }
    }
}

static void rx_dsp_task(void *arg)
{
    (void)arg;
    qam_rx_config_t local_cfg;

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    local_cfg = s_active_cfg;
    xSemaphoreGive(s_cfg_mutex);

    if (!qam_demod_init(&s_demod, &local_cfg)) {
        s_init_err = ESP_FAIL;
        xSemaphoreGive(s_init_sem);
        vTaskDelete(NULL);
        return;
    }
    rx_deframer_init(&s_deframer, &local_cfg);

    s_init_err = adc_sampler_start(local_cfg.adc1_channel,
                                   local_cfg.sample_rate_hz,
                                   local_cfg.adc_use_cal);
    xSemaphoreGive(s_init_sem);
    if (s_init_err != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }

    float mv_buf[ADC_SAMPLER_DMA_CHUNK_SAMPLES];
    uint32_t block_counter = 0U;

    while (1) {
        handle_pending_reconfig(&local_cfg);

        size_t count = 0U;
        esp_err_t err = adc_sampler_read_mv(mv_buf, ADC_SAMPLER_DMA_CHUNK_SAMPLES, &count, 50U);
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        rx_telem_on_adc_block(mv_buf, count, local_cfg.sample_rate_hz, local_cfg.adc_use_cal);

        for (size_t i = 0U; i < count; i++) {
            qam_demod_out_t dout;
            qam_demod_process_sample(&s_demod, mv_buf[i], &dout);
            process_demod_event(&local_cfg, &dout);
        }

        block_counter++;
        if ((block_counter % 8U) == 0U) {
            vTaskDelay(1);
        }
    }
}

esp_err_t rx_engine_start(const qam_rx_config_t *initial_cfg)
{
    if (s_rx_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (initial_cfg != NULL) {
        s_active_cfg = *initial_cfg;
    } else {
        rx_config_set_defaults(&s_active_cfg);
    }

    char err_msg[128];
    if (!rx_config_validate(&s_active_cfg, err_msg, sizeof(err_msg))) {
        ESP_LOGE(TAG, "Configuracao inicial invalida: %s", err_msg);
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg_mutex = xSemaphoreCreateMutex();
    s_init_sem  = xSemaphoreCreateBinary();
    if (!s_cfg_mutex || !s_init_sem) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(rx_dsp_task,
                                            "rx_dsp_task",
                                            8192,
                                            NULL,
                                            configMAX_PRIORITIES - 3,
                                            &s_rx_task,
                                            1); /* Core 1 dedicado ao DSP de recepção */
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_init_sem, portMAX_DELAY);
    return s_init_err;
}

void rx_engine_get_config(qam_rx_config_t *out_cfg)
{
    if (!out_cfg || !s_cfg_mutex) {
        return;
    }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    *out_cfg = s_active_cfg;
    xSemaphoreGive(s_cfg_mutex);
}

esp_err_t rx_engine_apply_config(const qam_rx_config_t *new_cfg, char *err_buf, size_t err_sz)
{
    if (!new_cfg || !s_cfg_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rx_config_validate(new_cfg, err_buf, err_sz)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    bool adc_changed = (new_cfg->adc1_channel != s_active_cfg.adc1_channel) ||
                       (new_cfg->sample_rate_hz != s_active_cfg.sample_rate_hz) ||
                       (new_cfg->adc_use_cal != s_active_cfg.adc_use_cal);
    s_active_cfg = *new_cfg;
    if (adc_changed) {
        s_adc_reconfig_needed = true;
    }
    s_reconfig_pending = true;
    xSemaphoreGive(s_cfg_mutex);
    if (rx_telem_is_enabled()) {
        rx_telem_send_hello_now();
    }
    return ESP_OK;
}

void rx_engine_set_monitor(bool enabled) { s_monitor_enabled = enabled; }
bool rx_engine_get_monitor(void)         { return s_monitor_enabled; }

void rx_engine_get_stats(rx_engine_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    out_stats->frames_ok       = s_frames_ok;
    out_stats->frames_crc_err  = s_frames_crc_err;
    out_stats->bytes_raw_rx    = s_bytes_raw_rx;
    out_stats->bursts_detected = s_bursts_detected;
    out_stats->symbols_demod   = s_demod.total_symbols;
    out_stats->dc_est_mv       = s_demod.dc_est_mv;
    out_stats->amp_pk_est_mv   = s_demod.amp_pk_est_mv;
    out_stats->freq_offset_hz  = s_demod.freq_offset_hz;
    out_stats->last_i_hat      = s_demod.last_i_hat;
    out_stats->last_q_hat      = s_demod.last_q_hat;
    out_stats->signal_active   = s_demod.signal_active;
}
