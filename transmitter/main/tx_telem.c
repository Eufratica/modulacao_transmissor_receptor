#include "tx_telem.h"

#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pwm_driver.h"
#include "qam_mapper.h"
#include "tx_engine.h"

static const char *TAG = "tx_telem";

#define TX_TLM_UART_PORT   CONFIG_ESP_CONSOLE_UART_NUM
#define TX_TLM_QUEUE_LEN   12U
#define TX_TLM_MAX_PAYLOAD 512U

typedef enum {
    QITEM_HELLO = 0,
    QITEM_FRAME = 1,
    QITEM_PWM_SNAP = 2,
} tx_qitem_kind_t;

typedef struct {
    tx_qitem_kind_t kind;
    uint32_t frame_id;
    uint32_t ts_us;
    uint8_t  status;
    uint8_t  tx_mode;
    uint8_t  map_preset;
    uint8_t  bit_order;
    uint8_t  q_sign_minus;
    uint32_t fc_hz;
    uint32_t baud;
    uint16_t dc_mv;
    uint16_t amp_mv;
    uint16_t header_sym_count;
    uint16_t crc16;
    uint16_t payload_len;
    uint8_t  payload[QAM_MAX_PAYLOAD_BYTES];
} tx_tlm_qitem_t;

static SemaphoreHandle_t s_uart_mutex = NULL;
static QueueHandle_t s_telem_queue = NULL;
static TaskHandle_t s_telem_task = NULL;

static volatile bool s_telem_enabled = false;
static volatile uint32_t s_telem_dropped = 0U;
static uint16_t s_seq = 0U;

/* Estado de captura PWM usado apenas no Core 1 (exceto handshake booleano) */
static volatile bool s_pwm_cap_requested = false;
static bool s_pwm_cap_collecting = false;
static uint16_t s_pwm_snap_ticks[TX_TLM_PWM_SNAP_MAX];
static uint16_t s_pwm_snap_len = 0U;
static uint32_t s_pwm_snap_frame_id = 0U;
static uint32_t s_pwm_snap_fs = 40000U;
static uint16_t s_pwm_snap_period = 1000U;

/* Buffer de snapshot entregue ao Core 0 após conclusão */
static uint16_t s_pwm_tx_buf[TX_TLM_PWM_SNAP_MAX];
static volatile bool s_pwm_tx_buf_busy = false;

static inline void put_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

static inline void put_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((v >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

void tx_telem_console_lock(void)
{
    if (s_uart_mutex) {
        xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    }
}

void tx_telem_console_unlock(void)
{
    if (s_uart_mutex) {
        fflush(stdout);
        xSemaphoreGive(s_uart_mutex);
    }
}

static void send_packet(uint8_t msg_type, uint8_t flags, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len > TX_TLM_MAX_PAYLOAD) {
        return;
    }

    uint8_t pkt[15U + TX_TLM_MAX_PAYLOAD];
    uint32_t ts_us = (uint32_t)esp_timer_get_time();

    pkt[0] = TX_TLM_SYNC_0;
    pkt[1] = TX_TLM_SYNC_1;
    pkt[2] = TX_TLM_PROTO_VER;
    pkt[3] = msg_type;
    pkt[4] = flags;
    put_u16_le(&pkt[5], s_seq++);
    put_u32_le(&pkt[7], ts_us);
    put_u16_le(&pkt[11], payload_len);
    if (payload_len > 0U && payload != NULL) {
        memcpy(&pkt[13], payload, payload_len);
    }

    /* CRC-16/CCITT-FALSE sobre [VER .. PAYLOAD_END] */
    uint16_t crc = qam_crc16_ccitt(&pkt[2], 11U + payload_len);
    put_u16_le(&pkt[13U + payload_len], crc);

    tx_telem_console_lock();
    fflush(stdout);
    uart_write_bytes(TX_TLM_UART_PORT, (const char *)pkt, 15U + payload_len);
    tx_telem_console_unlock();
}

static void build_and_send_hello(void)
{
    qam_tx_config_t cfg;
    pwm_driver_stats_t pwm;
    tx_engine_stats_t eng;

    tx_engine_get_config(&cfg);
    pwm_driver_get_stats(&pwm);
    tx_engine_get_stats(&eng);

    /* Payload binário fixo de 52 bytes para TLM_HELLO_STATUS (TX) */
    uint8_t p[52];
    memset(p, 0, sizeof(p));

    p[0] = 1U; /* Role: 1 = Transmissor */
    p[1] = TX_TLM_PROTO_VER;
    p[2] = (uint8_t)cfg.tx_mode;
    p[3] = (uint8_t)cfg.map_preset;
    p[4] = (uint8_t)cfg.bit_order;
    p[5] = cfg.q_sign_minus ? 1U : 0U;
    p[6] = cfg.invert_polarity ? 1U : 0U;
    p[7] = (uint8_t)cfg.pulse_shape;
    p[8] = (uint8_t)cfg.idle_mode;
    p[9] = s_telem_enabled ? 1U : 0U;
    p[10] = eng.transmitting ? 1U : 0U;
    p[11] = (uint8_t)(cfg.pwm_gpio & 0xFF);

    put_u32_le(&p[12], cfg.carrier_freq_hz);
    put_u32_le(&p[16], cfg.symbol_rate_baud);
    put_u32_le(&p[20], cfg.sample_rate_hz);
    put_u32_le(&p[24], pwm.actual_pwm_hz);
    put_u16_le(&p[28], cfg.dc_offset_mv);
    put_u16_le(&p[30], cfg.amplitude_peak_mv);
    put_u16_le(&p[32], (uint16_t)pwm.period_ticks);
    put_u16_le(&p[34], (cfg.mode1_use_preamble ? 1U : 0U) | (cfg.mode1_use_sync_word ? 2U : 0U));
    put_u32_le(&p[36], eng.frames_sent);
    put_u32_le(&p[40], eng.symbols_sent);
    put_u32_le(&p[44], pwm.underrun_count + eng.queue_dropped);
    put_u32_le(&p[48], s_telem_dropped);

    send_packet(TX_TLM_MSG_HELLO_STATUS, 0x00U, p, sizeof(p));
}

static void send_pwm_snapshot(void)
{
    const uint16_t chunk_max = 128U; /* 128 * 2B + 14B hdr = 270B <= 512B */
    uint16_t offset = 0U;

    while (offset < s_pwm_snap_len) {
        uint16_t cnt = (uint16_t)(s_pwm_snap_len - offset);
        if (cnt > chunk_max) {
            cnt = chunk_max;
        }
        uint8_t buf[14U + 128U * 2U];
        put_u32_le(&buf[0], s_pwm_snap_frame_id);
        put_u32_le(&buf[4], offset);
        put_u32_le(&buf[8], s_pwm_snap_fs);
        put_u16_le(&buf[12], s_pwm_snap_period);
        for (uint16_t i = 0; i < cnt; i++) {
            put_u16_le(&buf[14U + i * 2U], s_pwm_tx_buf[offset + i]);
        }
        send_packet(TX_TLM_MSG_PWM_CHUNK, 0x02U, buf, (uint16_t)(14U + cnt * 2U));
        offset += cnt;
    }
    s_pwm_tx_buf_busy = false;
}

static void tx_telem_task(void *arg)
{
    (void)arg;
    tx_tlm_qitem_t item;
    TickType_t last_hello = xTaskGetTickCount();

    while (1) {
        if (xQueueReceive(s_telem_queue, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (item.kind == QITEM_HELLO) {
                build_and_send_hello();
            } else if (item.kind == QITEM_FRAME && s_telem_enabled) {
                uint8_t buf[28U + QAM_MAX_PAYLOAD_BYTES];
                put_u32_le(&buf[0], item.frame_id);
                buf[4] = item.tx_mode;
                buf[5] = item.map_preset;
                buf[6] = item.bit_order;
                buf[7] = item.q_sign_minus;
                put_u32_le(&buf[8], item.fc_hz);
                put_u32_le(&buf[12], item.baud);
                put_u16_le(&buf[16], item.dc_mv);
                put_u16_le(&buf[18], item.amp_mv);
                put_u16_le(&buf[20], item.header_sym_count);
                put_u16_le(&buf[22], item.crc16);
                put_u16_le(&buf[24], item.payload_len);
                buf[26] = item.status;
                buf[27] = 0U;
                if (item.payload_len > 0U) {
                    memcpy(&buf[28], item.payload, item.payload_len);
                }
                send_packet(TX_TLM_MSG_FRAME_EVENT, 0x00U, buf, (uint16_t)(28U + item.payload_len));
            } else if (item.kind == QITEM_PWM_SNAP && s_telem_enabled) {
                send_pwm_snapshot();
            }
        }

        if (s_telem_enabled) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_hello) >= pdMS_TO_TICKS(500)) {
                last_hello = now;
                build_and_send_hello();
            }
        }
    }
}

esp_err_t tx_telem_init(void)
{
    if (s_telem_task != NULL) {
        return ESP_OK;
    }

    s_uart_mutex = xSemaphoreCreateMutex();
    s_telem_queue = xQueueCreate(TX_TLM_QUEUE_LEN, sizeof(tx_tlm_qitem_t));
    if (!s_uart_mutex || !s_telem_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(tx_telem_task, "tx_telem_task", 3072, NULL, 3, &s_telem_task, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Telemetria TX pronta (Core 0, /telem 0 por padrao).");
    return ESP_OK;
}

void tx_telem_set_enabled(bool enabled)
{
    s_telem_enabled = enabled;
    tx_telem_send_hello_now();
}

bool tx_telem_is_enabled(void)
{
    return s_telem_enabled;
}

void tx_telem_send_hello_now(void)
{
    if (!s_telem_queue) {
        return;
    }
    tx_tlm_qitem_t item;
    memset(&item, 0, sizeof(item));
    item.kind = QITEM_HELLO;
    if (xQueueSend(s_telem_queue, &item, 0) != pdTRUE) {
        s_telem_dropped++;
    }
}

void tx_telem_emit_frame_event(uint32_t frame_id,
                               const qam_tx_config_t *cfg,
                               const uint8_t *payload,
                               size_t payload_len,
                               const qam_tx_frame_t *frame,
                               tx_frame_ev_status_t status)
{
    if (!s_telem_enabled || !s_telem_queue || !cfg) {
        return;
    }
    tx_tlm_qitem_t item;
    memset(&item, 0, sizeof(item));
    item.kind = QITEM_FRAME;
    item.frame_id = frame_id;
    item.ts_us = (uint32_t)esp_timer_get_time();
    item.status = (uint8_t)status;
    item.tx_mode = (uint8_t)cfg->tx_mode;
    item.map_preset = (uint8_t)cfg->map_preset;
    item.bit_order = (uint8_t)cfg->bit_order;
    item.q_sign_minus = cfg->q_sign_minus ? 1U : 0U;
    item.fc_hz = cfg->carrier_freq_hz;
    item.baud = cfg->symbol_rate_baud;
    item.dc_mv = cfg->dc_offset_mv;
    item.amp_mv = cfg->amplitude_peak_mv;
    if (frame) {
        item.header_sym_count = (uint16_t)frame->header_symbol_count;
        item.crc16 = frame->crc16;
    }
    size_t copy_len = (payload_len <= QAM_MAX_PAYLOAD_BYTES) ? payload_len : QAM_MAX_PAYLOAD_BYTES;
    item.payload_len = (uint16_t)copy_len;
    if (copy_len > 0U && payload != NULL) {
        memcpy(item.payload, payload, copy_len);
    }

    if (xQueueSend(s_telem_queue, &item, 0) != pdTRUE) {
        s_telem_dropped++;
    }
}

void tx_telem_request_pwm_capture(void)
{
    if (!s_pwm_tx_buf_busy) {
        s_pwm_cap_requested = true;
    }
}

bool tx_telem_pwm_capture_active(void)
{
    return s_pwm_cap_collecting || (s_pwm_cap_requested && !s_pwm_tx_buf_busy);
}

void tx_telem_pwm_capture_push(uint16_t cmp_val, uint32_t frame_id, uint32_t sample_rate_hz, uint16_t period_ticks)
{
    if (!s_pwm_cap_collecting) {
        if (!s_pwm_cap_requested || s_pwm_tx_buf_busy) {
            return;
        }
        s_pwm_cap_requested = false;
        s_pwm_cap_collecting = true;
        s_pwm_snap_len = 0U;
        s_pwm_snap_frame_id = frame_id;
        s_pwm_snap_fs = sample_rate_hz;
        s_pwm_snap_period = period_ticks;
    }

    if (s_pwm_snap_len < TX_TLM_PWM_SNAP_MAX) {
        s_pwm_snap_ticks[s_pwm_snap_len++] = cmp_val;
    }

    if (s_pwm_snap_len >= TX_TLM_PWM_SNAP_MAX) {
        s_pwm_cap_collecting = false;
        memcpy(s_pwm_tx_buf, s_pwm_snap_ticks, sizeof(s_pwm_snap_ticks));
        s_pwm_tx_buf_busy = true;
        tx_tlm_qitem_t item;
        memset(&item, 0, sizeof(item));
        item.kind = QITEM_PWM_SNAP;
        if (xQueueSend(s_telem_queue, &item, 0) != pdTRUE) {
            s_pwm_tx_buf_busy = false;
            s_telem_dropped++;
        }
    }
}

uint32_t tx_telem_get_dropped_count(void)
{
    return s_telem_dropped;
}
