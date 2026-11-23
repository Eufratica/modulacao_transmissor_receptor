#include "rx_deframer.h"
#include "rx_mapper.h"

#include <string.h>

static void enter_initial_state(rx_deframer_t *df)
{
    df->sync_fill   = 0U;
    df->sym_acc_idx = 0U;
    df->hdr_idx     = 0U;
    df->payload_idx = 0U;
    df->crc_idx     = 0U;

    if (df->cfg.rx_mode == RX_MODE_1_RAW_STREAM && !df->cfg.mode1_use_sync_word) {
        df->state      = RX_STATE_RAW_STREAM;
        df->locked_rot = df->cfg.fixed_rotation_quad & 0x03U;
    } else {
        df->state      = RX_STATE_SYNC_SEARCH;
        df->locked_rot = 0U;
    }
}

void rx_deframer_init(rx_deframer_t *df, const qam_rx_config_t *cfg)
{
    if (!df || !cfg) {
        return;
    }
    memset(df, 0, sizeof(*df));
    df->cfg = *cfg;
    enter_initial_state(df);
}

void rx_deframer_reset(rx_deframer_t *df)
{
    if (!df) {
        return;
    }
    enter_initial_state(df);
}

void rx_deframer_on_burst_end(rx_deframer_t *df)
{
    if (!df) {
        return;
    }
    enter_initial_state(df);
}

static bool try_match_sync_word(rx_deframer_t *df, float i_hat, float q_hat, uint8_t *out_rot)
{
    const size_t need_syms = (size_t)df->cfg.sync_word_len * 4U;
    if (need_syms == 0U || need_syms > RX_DEFRAMER_MAX_SYNC_SYMS) {
        return false;
    }

    if (df->sync_fill < need_syms) {
        df->sync_i[df->sync_fill] = i_hat;
        df->sync_q[df->sync_fill] = q_hat;
        df->sync_fill++;
    } else {
        memmove(&df->sync_i[0], &df->sync_i[1], (need_syms - 1U) * sizeof(float));
        memmove(&df->sync_q[0], &df->sync_q[1], (need_syms - 1U) * sizeof(float));
        df->sync_i[need_syms - 1U] = i_hat;
        df->sync_q[need_syms - 1U] = q_hat;
    }

    if (df->sync_fill < need_syms) {
        return false;
    }

    /* Testa as 4 rotações possíveis do Costas Loop (0°, 90°, 180°, 270°) */
    for (uint8_t rot = 0U; rot < 4U; rot++) {
        bool match = true;
        for (size_t b = 0U; b < df->cfg.sync_word_len; b++) {
            uint8_t syms[4];
            for (size_t k = 0U; k < 4U; k++) {
                size_t idx = b * 4U + k;
                syms[k] = rx_mapper_decide_symbol(&df->cfg, df->sync_i[idx], df->sync_q[idx], rot);
            }
            uint8_t byte = rx_mapper_symbols_to_byte(syms, RX_BIT_ORDER_MSB_FIRST);
            if (byte != df->cfg.sync_word[b]) {
                match = false;
                break;
            }
        }
        if (match) {
            *out_rot = rot;
            return true;
        }
    }
    return false;
}

static void finalize_mode2_frame(rx_deframer_t *df, rx_deframer_event_t *out)
{
    uint8_t crc_buf[4U + RX_MAX_PAYLOAD_BYTES];
    memcpy(crc_buf, df->hdr, 4U);
    if (df->payload_len > 0U) {
        memcpy(&crc_buf[4U], df->payload, df->payload_len);
    }

    uint16_t calc_crc = rx_crc16_ccitt(crc_buf, 4U + df->payload_len);
    uint16_t rx_crc   = (uint16_t)(((uint16_t)df->crc_bytes[0] << 8U) | df->crc_bytes[1]);

    out->frame_ready = true;
    out->crc_ok      = (rx_crc == calc_crc);
    out->payload_len = df->payload_len;
    memcpy(out->payload, df->payload, df->payload_len);
    out->payload[df->payload_len] = '\0';
    out->tx_baud     = df->tx_baud;
    out->locked_rot  = df->locked_rot;
    out->rx_crc      = rx_crc;
    out->calc_crc    = calc_crc;

    enter_initial_state(df);
}

void rx_deframer_push_symbol(rx_deframer_t *df, float i_hat, float q_hat, rx_deframer_event_t *out)
{
    if (!df || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (df->state == RX_STATE_SYNC_SEARCH) {
        uint8_t rot = 0U;
        if (try_match_sync_word(df, i_hat, q_hat, &rot)) {
            df->locked_rot  = rot;
            df->sym_acc_idx = 0U;
            df->hdr_idx     = 0U;
            df->payload_idx = 0U;
            df->crc_idx     = 0U;
            df->sync_fill   = 0U;
            df->state = (df->cfg.rx_mode == RX_MODE_1_RAW_STREAM) ? RX_STATE_RAW_STREAM : RX_STATE_HEADER;
        }
        return;
    }

    uint8_t sym = rx_mapper_decide_symbol(&df->cfg, i_hat, q_hat, df->locked_rot);
    df->sym_acc[df->sym_acc_idx++] = sym;
    if (df->sym_acc_idx < 4U) {
        return;
    }
    df->sym_acc_idx = 0U;

    if (df->state == RX_STATE_RAW_STREAM) {
        out->raw_byte_ready = true;
        out->raw_byte = rx_mapper_symbols_to_byte(df->sym_acc, df->cfg.bit_order);
        memcpy(out->raw_sym_group, df->sym_acc, 4U);
        out->locked_rot = df->locked_rot;
        return;
    }

    if (df->state == RX_STATE_HEADER) {
        df->hdr[df->hdr_idx++] = rx_mapper_symbols_to_byte(df->sym_acc, RX_BIT_ORDER_MSB_FIRST);
        if (df->hdr_idx >= 4U) {
            uint8_t ver   = (uint8_t)((df->hdr[0] >> 4U) & 0x0FU);
            uint8_t order = (uint8_t)(df->hdr[0] & 0x03U);
            df->tx_baud     = (uint16_t)(((uint16_t)df->hdr[1] << 8U) | df->hdr[2]);
            df->payload_len = df->hdr[3];

            if (ver != 0x1U || order > 2U || df->payload_len > RX_MAX_PAYLOAD_BYTES) {
                enter_initial_state(df);
                return;
            }
            df->payload_order = (rx_bit_order_t)order;
            df->payload_idx   = 0U;
            df->state = (df->payload_len > 0U) ? RX_STATE_PAYLOAD : RX_STATE_CRC;
        }
        return;
    }

    if (df->state == RX_STATE_PAYLOAD) {
        df->payload[df->payload_idx++] = rx_mapper_symbols_to_byte(df->sym_acc, df->payload_order);
        if (df->payload_idx >= df->payload_len) {
            df->crc_idx = 0U;
            df->state   = RX_STATE_CRC;
        }
        return;
    }

    if (df->state == RX_STATE_CRC) {
        df->crc_bytes[df->crc_idx++] = rx_mapper_symbols_to_byte(df->sym_acc, RX_BIT_ORDER_MSB_FIRST);
        if (df->crc_idx >= 2U) {
            finalize_mode2_frame(df, out);
        }
    }
}
