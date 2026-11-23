#include "rx_telem.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "adc_sampler.h"
#include "rx_engine.h"
#include "rx_mapper.h"

static const char *TAG = "rx_telem";

#define RX_TLM_UART_PORT        CONFIG_ESP_CONSOLE_UART_NUM
#define RX_TLM_QUEUE_LEN        16U
#define RX_TLM_MAX_PAYLOAD      512U
#define RX_TLM_SYM_BATCH_MAX    5U
#define RX_TLM_ADC_STREAM_CHUNK 64U

typedef struct __attribute__((packed)) {
    uint16_t sym_idx;
    int16_t  i_hat_q12;
    int16_t  q_hat_q12;
    int16_t  i_mid_q12;
    int16_t  q_mid_q12;
    int16_t  ped_q12;
    int16_t  df_centihz;
    uint16_t nco_mrad;
    int16_t  gardner_q12;
    int16_t  sym_rate_ppm;
    int16_t  snr_cdb;
    int16_t  sym_phase_q12;
    uint8_t  dec_sym;
    uint8_t  rot_state; /* bits 0..1 rot | bits 2..4 state | bit 5 locked */
} rx_sym_rec_t; /* 26 bytes por símbolo */

typedef enum {
    RX_Q_HELLO = 0,
    RX_Q_BURST,
    RX_Q_SYMS,
    RX_Q_FRAME,
    RX_Q_ADC_STREAM,
    RX_Q_ADC_SNAP,
} rx_qkind_t;

typedef struct {
    rx_qkind_t kind;
    uint32_t   burst_id;
    uint16_t   aux_u16;
    uint16_t   aux2_u16;
    uint32_t   aux_u32;
    uint8_t    u8_a;
    uint8_t    u8_b;
    uint8_t    u8_c;
    uint8_t    u8_d;
    float      f_a;
    float      f_b;
    float      f_c;
    union {
        struct {
            uint8_t      count;
            rx_sym_rec_t recs[RX_TLM_SYM_BATCH_MAX];
        } syms;
        struct {
            uint16_t crc_rx;
            uint16_t crc_calc;
            uint16_t len;
            uint8_t  data[RX_MAX_PAYLOAD_BYTES];
        } frame;
        struct {
            uint16_t count;
            int16_t  samples[RX_TLM_ADC_STREAM_CHUNK];
        } adc;
    } u;
} rx_tlm_qitem_t;

static SemaphoreHandle_t s_uart_mutex = NULL;
static QueueHandle_t s_telem_queue = NULL;
static TaskHandle_t s_telem_task = NULL;

static volatile bool s_telem_enabled = false;
static volatile uint16_t s_adc_stream_decim = 10U; /* 2000 Sa/s padrão quando ativo */
static volatile uint32_t s_telem_dropped = 0U;
static uint16_t s_seq = 0U;

/* Estado local de batch/amostragem exclusivo da task DSP Core 1 */
static rx_sym_rec_t s_sym_batch[RX_TLM_SYM_BATCH_MAX];
static uint8_t s_sym_batch_cnt = 0U;
static uint32_t s_sym_counter = 0U;

static uint16_t s_decim_phase = 0U;
static uint32_t s_adc_stream_idx = 0U;
static int16_t s_stream_chunk[RX_TLM_ADC_STREAM_CHUNK];
static uint16_t s_stream_chunk_len = 0U;

/* Buffer de snapshot em taxa cheia (2048 amostras @ 20 kHz = 4096 B) */
static volatile bool s_snap_requested = false;
static volatile bool s_snap_busy = false;
static bool s_snap_collecting = false;
static uint16_t s_snap_write = 0U;
static uint16_t s_snap_id = 0U;
static uint32_t s_snap_fs = 20000U;
static bool s_snap_cal = true;
static uint32_t s_current_burst_id = 0U;
static int16_t s_snap_core1[RX_TLM_ADC_SNAP_LEN];
static int16_t s_snap_core0[RX_TLM_ADC_SNAP_LEN];

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

static inline int16_t clamp_i16(float x)
{
    if (x > 32767.0f) return 32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)lrintf(x);
}

void rx_telem_console_lock(void)
{
    if (s_uart_mutex) {
        xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    }
}

void rx_telem_console_unlock(void)
{
    if (s_uart_mutex) {
        fflush(stdout);
        xSemaphoreGive(s_uart_mutex);
    }
}

static void send_packet(uint8_t msg_type, uint8_t flags, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len > RX_TLM_MAX_PAYLOAD) {
        return;
    }

    uint8_t pkt[15U + RX_TLM_MAX_PAYLOAD];
    uint32_t ts_us = (uint32_t)esp_timer_get_time();

    pkt[0] = RX_TLM_SYNC_0;
    pkt[1] = RX_TLM_SYNC_1;
    pkt[2] = RX_TLM_PROTO_VER;
    pkt[3] = msg_type;
    pkt[4] = (uint8_t)(flags | 0x01U); /* Bit 0 = 1 indica Receptor */
    put_u16_le(&pkt[5], s_seq++);
    put_u32_le(&pkt[7], ts_us);
    put_u16_le(&pkt[11], payload_len);
    if (payload_len > 0U && payload != NULL) {
        memcpy(&pkt[13], payload, payload_len);
    }

    uint16_t crc = rx_crc16_ccitt(&pkt[2], 11U + payload_len);
    put_u16_le(&pkt[13U + payload_len], crc);

    rx_telem_console_lock();
    fflush(stdout);
    uart_write_bytes(RX_TLM_UART_PORT, (const char *)pkt, 15U + payload_len);
    rx_telem_console_unlock();
}

static void send_hello_packet(void)
{
    qam_rx_config_t cfg;
    adc_sampler_stats_t adc;
    rx_engine_stats_t eng;

    rx_engine_get_config(&cfg);
    adc_sampler_get_stats(&adc);
    rx_engine_get_stats(&eng);

    /* Payload RX_HELLO: 60 bytes */
    uint8_t p[60];
    memset(p, 0, sizeof(p));

    p[0] = 2U; /* Role: 2 = Receptor */
    p[1] = RX_TLM_PROTO_VER;
    p[2] = (uint8_t)cfg.rx_mode;
    p[3] = (uint8_t)cfg.map_preset;
    p[4] = (uint8_t)cfg.bit_order;
    p[5] = cfg.q_sign_minus ? 1U : 0U;
    p[6] = cfg.invert_polarity ? 1U : 0U;
    p[7] = cfg.fixed_rotation_quad & 0x03U;
    p[8] = cfg.mode1_use_sync_word ? 1U : 0U;
    p[9] = s_telem_enabled ? 1U : 0U;
    p[10] = eng.signal_active ? 1U : 0U;
    p[11] = (uint8_t)(adc.gpio_num & 0xFF);
    p[12] = (uint8_t)(adc.adc1_channel & 0xFF);
    p[13] = adc.efuse_cal_active ? 1U : 0U;
    put_u16_le(&p[14], s_adc_stream_decim);

    put_u32_le(&p[16], cfg.carrier_freq_hz);
    put_u32_le(&p[20], cfg.symbol_rate_baud);
    put_u32_le(&p[24], adc.sample_rate_hz);
    put_u16_le(&p[28], (uint16_t)clamp_i16(cfg.squelch_rms_mv * 10.0f));
    put_u16_le(&p[30], (uint16_t)clamp_i16(eng.dc_est_mv * 10.0f));
    put_u16_le(&p[32], (uint16_t)clamp_i16(eng.amp_pk_est_mv * 10.0f));
    put_u16_le(&p[34], (uint16_t)clamp_i16(eng.freq_offset_hz * 100.0f));
    put_u32_le(&p[36], eng.frames_ok);
    put_u32_le(&p[40], eng.frames_crc_err);
    put_u32_le(&p[44], eng.bytes_raw_rx);
    put_u32_le(&p[48], eng.symbols_demod);
    put_u32_le(&p[52], adc.overflow_count + adc.invalid_samples);
    put_u32_le(&p[56], s_telem_dropped);

    send_packet(RX_TLM_MSG_HELLO_STATUS, 0x00U, p, sizeof(p));
}

static void send_adc_snapshot(uint32_t burst_id, uint16_t snap_id, uint32_t fs_hz, bool cal_active)
{
    const uint16_t chunk_max = 128U; /* 16B hdr + 256B = 272B */
    uint16_t offset = 0U;

    while (offset < RX_TLM_ADC_SNAP_LEN) {
        uint16_t cnt = (uint16_t)(RX_TLM_ADC_SNAP_LEN - offset);
        if (cnt > chunk_max) {
            cnt = chunk_max;
        }
        uint8_t buf[16U + 128U * 2U];
        put_u32_le(&buf[0], burst_id);
        put_u16_le(&buf[4], snap_id);
        put_u32_le(&buf[6], offset);
        put_u32_le(&buf[10], fs_hz);
        put_u16_le(&buf[14], 1U); /* decim = 1 (taxa cheia 20 kHz) */
        for (uint16_t i = 0; i < cnt; i++) {
            put_u16_le(&buf[16U + i * 2U], (uint16_t)s_snap_core0[offset + i]);
        }
        uint8_t fl = (uint8_t)(0x02U | (cal_active ? 0x04U : 0x00U));
        send_packet(RX_TLM_MSG_ADC_CHUNK, fl, buf, (uint16_t)(16U + cnt * 2U));
        offset += cnt;
    }
    s_snap_busy = false;
}

static void rx_telem_task(void *arg)
{
    (void)arg;
    rx_tlm_qitem_t *item = (rx_tlm_qitem_t *)pvPortMalloc(sizeof(rx_tlm_qitem_t));
    if (!item) {
        ESP_LOGE(TAG, "Sem heap para rx_tlm_task");
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_hello = xTaskGetTickCount();

    while (1) {
        if (xQueueReceive(s_telem_queue, item, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (item->kind == RX_Q_HELLO) {
                send_hello_packet();
            } else if (item->kind == RX_Q_BURST && s_telem_enabled) {
                uint8_t b[14];
                b[0] = item->u8_a;
                b[1] = 0U;
                put_u32_le(&b[2], item->burst_id);
                int32_t dc_c = (int32_t)lrintf(item->f_a * 10.0f);
                int32_t amp_c = (int32_t)lrintf(item->f_b * 10.0f);
                put_u32_le(&b[6], (uint32_t)dc_c);
                put_u32_le(&b[10], (uint32_t)amp_c);
                send_packet(RX_TLM_MSG_BURST_EVENT, 0x00U, b, sizeof(b));
            } else if (item->kind == RX_Q_SYMS && s_telem_enabled) {
                uint8_t buf[6U + RX_TLM_SYM_BATCH_MAX * sizeof(rx_sym_rec_t)];
                put_u32_le(&buf[0], item->burst_id);
                buf[4] = item->u.syms.count;
                buf[5] = 0U;
                size_t bytes = (size_t)item->u.syms.count * sizeof(rx_sym_rec_t);
                memcpy(&buf[6], item->u.syms.recs, bytes);
                send_packet(RX_TLM_MSG_SYMBOL_BATCH, 0x00U, buf, (uint16_t)(6U + bytes));
            } else if (item->kind == RX_Q_FRAME && s_telem_enabled) {
                uint8_t buf[24U + RX_MAX_PAYLOAD_BYTES];
                put_u32_le(&buf[0], item->burst_id);
                buf[4] = item->u8_a; /* rx_mode */
                buf[5] = item->u8_b; /* crc_ok */
                buf[6] = item->u8_c; /* locked_rot */
                buf[7] = item->u8_d; /* payload_order */
                put_u16_le(&buf[8], item->aux_u16); /* tx_baud */
                put_u16_le(&buf[10], item->u.frame.crc_rx);
                put_u16_le(&buf[12], item->u.frame.crc_calc);
                put_u16_le(&buf[14], item->u.frame.len);
                put_u16_le(&buf[16], (uint16_t)clamp_i16(item->f_a * 100.0f)); /* df centiHz */
                put_u16_le(&buf[18], (uint16_t)clamp_i16(item->f_b * 10.0f));  /* dc dmv */
                put_u16_le(&buf[20], (uint16_t)clamp_i16(item->f_c * 10.0f));  /* amp dmv */
                put_u16_le(&buf[22], (uint16_t)clamp_i16(item->aux2_u16));     /* snr centi-dB */
                if (item->u.frame.len > 0U) {
                    memcpy(&buf[24], item->u.frame.data, item->u.frame.len);
                }
                send_packet(RX_TLM_MSG_FRAME_EVENT, 0x00U, buf, (uint16_t)(24U + item->u.frame.len));
            } else if (item->kind == RX_Q_ADC_STREAM && s_telem_enabled) {
                uint8_t buf[16U + RX_TLM_ADC_STREAM_CHUNK * 2U];
                put_u32_le(&buf[0], item->burst_id);
                put_u16_le(&buf[4], 0U);
                put_u32_le(&buf[6], item->aux_u32); /* start idx */
                put_u32_le(&buf[10], 20000U);       /* base fs nominal fallback */
                put_u16_le(&buf[14], item->aux_u16); /* decim */
                for (uint16_t i = 0; i < item->u.adc.count; i++) {
                    put_u16_le(&buf[16U + i * 2U], (uint16_t)item->u.adc.samples[i]);
                }
                uint8_t fl = (item->u8_a != 0U) ? 0x04U : 0x00U;
                send_packet(RX_TLM_MSG_ADC_CHUNK, fl, buf, (uint16_t)(16U + item->u.adc.count * 2U));
            } else if (item->kind == RX_Q_ADC_SNAP && s_telem_enabled) {
                send_adc_snapshot(item->burst_id, item->aux_u16, item->aux_u32, item->u8_a != 0U);
            }
        }

        if (s_telem_enabled) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_hello) >= pdMS_TO_TICKS(500)) {
                last_hello = now;
                send_hello_packet();
            }
        }
    }
}

esp_err_t rx_telem_init(void)
{
    if (s_telem_task != NULL) {
        return ESP_OK;
    }

    s_uart_mutex = xSemaphoreCreateMutex();
    s_telem_queue = xQueueCreate(RX_TLM_QUEUE_LEN, sizeof(rx_tlm_qitem_t));
    if (!s_uart_mutex || !s_telem_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(rx_telem_task, "rx_telem_task", 4096, NULL, 3, &s_telem_task, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Telemetria RX pronta (Core 0, /telem 0 por padrao).");
    return ESP_OK;
}

void rx_telem_set_enabled(bool enabled)
{
    s_telem_enabled = enabled;
    rx_telem_send_hello_now();
}

bool rx_telem_is_enabled(void)
{
    return s_telem_enabled;
}

void rx_telem_set_adc_decim(uint16_t decim)
{
    s_adc_stream_decim = decim;
    if (s_telem_enabled) {
        rx_telem_send_hello_now();
    }
}

uint16_t rx_telem_get_adc_decim(void)
{
    return s_adc_stream_decim;
}

void rx_telem_send_hello_now(void)
{
    if (!s_telem_queue) {
        return;
    }
    rx_tlm_qitem_t item;
    memset(&item, 0, sizeof(item));
    item.kind = RX_Q_HELLO;
    if (xQueueSend(s_telem_queue, &item, 0) != pdTRUE) {
        s_telem_dropped++;
    }
}

void rx_telem_request_adc_capture(void)
{
    if (!s_snap_busy) {
        s_snap_requested = true;
    }
}

static void push_adc_stream_chunk(uint32_t start_idx, uint16_t decim, bool cal_active)
{
    if (!s_telem_queue || s_stream_chunk_len == 0U) {
        return;
    }
    rx_tlm_qitem_t item;
    memset(&item, 0, sizeof(item));
    item.kind = RX_Q_ADC_STREAM;
    item.burst_id = s_current_burst_id;
    item.aux_u32 = start_idx;
    item.aux_u16 = decim;
    item.u8_a = cal_active ? 1U : 0U;
    item.u.adc.count = s_stream_chunk_len;
    memcpy(item.u.adc.samples, s_stream_chunk, (size_t)s_stream_chunk_len * sizeof(int16_t));
    if (xQueueSend(s_telem_queue, &item, 0) != pdTRUE) {
        s_telem_dropped++;
    }
    s_stream_chunk_len = 0U;
}

void rx_telem_on_adc_block(const float *mv_samples, size_t count, uint32_t sample_rate_hz, bool cal_active)
{
    if (!s_telem_enabled || !mv_samples || count == 0U) {
        return;
    }

    /* 1. Captura em taxa cheia (20 kHz, decim = 1) se solicitada por burst ou /cap */
    if (!s_snap_collecting && s_snap_requested && !s_snap_busy) {
        s_snap_requested = false;
        s_snap_collecting = true;
        s_snap_write = 0U;
        s_snap_fs = sample_rate_hz;
        s_snap_cal = cal_active;
        s_snap_id++;
    }

    if (s_snap_collecting) {
        for (size_t i = 0U; i < count && s_snap_write < RX_TLM_ADC_SNAP_LEN; i++) {
            s_snap_core1[s_snap_write++] = clamp_i16(mv_samples[i]);
        }
        if (s_snap_write >= RX_TLM_ADC_SNAP_LEN) {
            s_snap_collecting = false;
            memcpy(s_snap_core0, s_snap_core1, sizeof(s_snap_core1));
            s_snap_busy = true;
            rx_tlm_qitem_t item;
            memset(&item, 0, sizeof(item));
            item.kind = RX_Q_ADC_SNAP;
            item.burst_id = s_current_burst_id;
            item.aux_u16 = s_snap_id;
            item.aux_u32 = s_snap_fs;
            item.u8_a = s_snap_cal ? 1U : 0U;
            if (xQueueSend(s_telem_queue, &item, 0) != pdTRUE) {
                s_snap_busy = false;
                s_telem_dropped++;
            }
        }
    }

    /* 2. Fluxo contínuo decimado para osciloscópio ao vivo */
    uint16_t decim = s_adc_stream_decim;
    if (decim == 0U) {
        return;
    }

    static uint32_t s_chunk_start_idx = 0U;
    for (size_t i = 0U; i < count; i++) {
        if (s_decim_phase == 0U) {
            if (s_stream_chunk_len == 0U) {
                s_chunk_start_idx = s_adc_stream_idx;
            }
            s_stream_chunk[s_stream_chunk_len++] = clamp_i16(mv_samples[i]);
            if (s_stream_chunk_len >= RX_TLM_ADC_STREAM_CHUNK) {
                push_adc_stream_chunk(s_chunk_start_idx, decim, cal_active);
            }
        }
        s_adc_stream_idx++;
        s_decim_phase++;
        if (s_decim_phase >= decim) {
            s_decim_phase = 0U;
        }
    }
}

void rx_telem_on_burst_event(uint8_t event_kind, uint32_t burst_id, float dc_mv, float amp_pk_mv)
{
    if (event_kind == 0U) {
        s_current_burst_id = burst_id;
        s_sym_batch_cnt = 0U;
        s_sym_counter = 0U;
        if (s_telem_enabled && !s_snap_busy && !s_snap_collecting) {
            s_snap_requested = true;
        }
    }

    if (!s_telem_enabled || !s_telem_queue) {
        return;
    }
    rx_tlm_qitem_t item;
    memset(&item, 0, sizeof(item));
    item.kind = RX_Q_BURST;
    item.u8_a = event_kind;
    item.burst_id = burst_id;
    item.f_a = dc_mv;
    item.f_b = amp_pk_mv;
    if (xQueueSend(s_telem_queue, &item, 0) != pdTRUE) {
        s_telem_dropped++;
    }
}

static void flush_sym_batch(uint32_t burst_id)
{
    if (!s_telem_queue || s_sym_batch_cnt == 0U) {
        return;
    }
    rx_tlm_qitem_t item;
    memset(&item, 0, sizeof(item));
    item.kind = RX_Q_SYMS;
    item.burst_id = burst_id;
    item.u.syms.count = s_sym_batch_cnt;
    memcpy(item.u.syms.recs, s_sym_batch, (size_t)s_sym_batch_cnt * sizeof(rx_sym_rec_t));
    if (xQueueSend(s_telem_queue, &item, 0) != pdTRUE) {
        s_telem_dropped++;
    }
    s_sym_batch_cnt = 0U;
}

void rx_telem_on_symbol_dsp(uint32_t burst_id,
                            const qam_rx_config_t *cfg,
                            const qam_demod_t *demod,
                            const qam_demod_out_t *dout,
                            const rx_deframer_t *df,
                            uint8_t sym_dec,
                            bool force_flush)
{
    (void)cfg;
    if (!s_telem_enabled || !demod || !dout || !df) {
        return;
    }

    rx_sym_rec_t rec;
    rec.sym_idx = (uint16_t)(s_sym_counter++ & 0xFFFFU);
    rec.i_hat_q12 = clamp_i16(dout->i_hat * 4096.0f);
    rec.q_hat_q12 = clamp_i16(dout->q_hat * 4096.0f);
    rec.i_mid_q12 = clamp_i16(demod->mid_i_curr * 4096.0f);
    rec.q_mid_q12 = clamp_i16(demod->mid_q_curr * 4096.0f);
    rec.ped_q12 = clamp_i16(demod->last_ped * 4096.0f);
    rec.df_centihz = clamp_i16(demod->freq_offset_hz * 100.0f);

    float nco_mrad = demod->nco_phase_rad * 1000.0f;
    if (nco_mrad < 0.0f) nco_mrad = 0.0f;
    if (nco_mrad > 65535.0f) nco_mrad = 65535.0f;
    rec.nco_mrad = (uint16_t)lrintf(nco_mrad);

    rec.gardner_q12 = clamp_i16(demod->last_gardner_err * 4096.0f);
    float ppm = (demod->sym_inc_nom > 1e-9f)
                ? ((demod->sym_rate_corr / demod->sym_inc_nom) * 1000000.0f)
                : 0.0f;
    rec.sym_rate_ppm = clamp_i16(ppm);
    rec.snr_cdb = clamp_i16(dout->snr_est_db * 100.0f);
    rec.sym_phase_q12 = clamp_i16(demod->sym_phase * 4096.0f);
    rec.dec_sym = sym_dec & 0x03U;

    uint8_t locked_bit = (df->state != RX_STATE_SYNC_SEARCH) ? 0x20U : 0x00U;
    rec.rot_state = (uint8_t)((df->locked_rot & 0x03U) |
                              (((uint8_t)df->state & 0x07U) << 2U) |
                              locked_bit);

    s_sym_batch[s_sym_batch_cnt++] = rec;
    if (s_sym_batch_cnt >= RX_TLM_SYM_BATCH_MAX || force_flush) {
        flush_sym_batch(burst_id);
    }
}

void rx_telem_on_frame_event(uint32_t burst_id,
                             uint8_t rx_mode,
                             bool crc_ok,
                             uint8_t locked_rot,
                             uint8_t payload_order,
                             uint16_t tx_baud,
                             uint16_t rx_crc,
                             uint16_t calc_crc,
                             float df_est_hz,
                             float dc_est_mv,
                             float amp_pk_mv,
                             float snr_db,
                             const uint8_t *payload,
                             size_t payload_len)
{
    if (!s_telem_enabled || !s_telem_queue) {
        return;
    }

    /* Descarrega símbolos pendentes antes de notificar quadro completo */
    flush_sym_batch(burst_id);

    rx_tlm_qitem_t item;
    memset(&item, 0, sizeof(item));
    item.kind = RX_Q_FRAME;
    item.burst_id = burst_id;
    item.u8_a = rx_mode;
    item.u8_b = crc_ok ? 1U : 0U;
    item.u8_c = locked_rot & 0x03U;
    item.u8_d = payload_order;
    item.aux_u16 = tx_baud;
    item.u.frame.crc_rx = rx_crc;
    item.u.frame.crc_calc = calc_crc;
    item.f_a = df_est_hz;
    item.f_b = dc_est_mv;
    item.f_c = amp_pk_mv;
    item.aux2_u16 = (uint16_t)clamp_i16(snr_db * 100.0f);

    size_t copy_len = (payload_len <= RX_MAX_PAYLOAD_BYTES) ? payload_len : RX_MAX_PAYLOAD_BYTES;
    item.u.frame.len = (uint16_t)copy_len;
    if (copy_len > 0U && payload != NULL) {
        memcpy(item.u.frame.data, payload, copy_len);
    }

    if (xQueueSend(s_telem_queue, &item, 0) != pdTRUE) {
        s_telem_dropped++;
    }
}

uint32_t rx_telem_get_dropped_count(void)
{
    return s_telem_dropped;
}
