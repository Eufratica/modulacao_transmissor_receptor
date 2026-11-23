#include "rx_mapper.h"

void rx_mapper_rotate_iq(float i_in, float q_in, uint8_t rot_quad, float *i_out, float *q_out)
{
    switch (rot_quad & 0x03U) {
    case 1U: /* +90° */
        *i_out = -q_in;
        *q_out =  i_in;
        break;
    case 2U: /* +180° */
        *i_out = -i_in;
        *q_out = -q_in;
        break;
    case 3U: /* +270° */
        *i_out =  q_in;
        *q_out = -i_in;
        break;
    case 0U:
    default:
        *i_out = i_in;
        *q_out = q_in;
        break;
    }
}

uint8_t rx_mapper_decide_symbol(const qam_rx_config_t *cfg, float i_hat, float q_hat, uint8_t rot_quad)
{
    if (!cfg) {
        return 0U;
    }

    float ir = 0.0f;
    float qr = 0.0f;
    rx_mapper_rotate_iq(i_hat, q_hat, rot_quad, &ir, &qr);

    int8_t si = (ir >= 0.0f) ? 1 : -1;
    int8_t sq = (qr >= 0.0f) ? 1 : -1;

    for (uint8_t s = 0U; s < RX_CONSTELLATION_SIZE; s++) {
        if (cfg->constellation[s].i_val == si && cfg->constellation[s].q_val == sq) {
            return s;
        }
    }
    return 0U;
}

uint8_t rx_mapper_symbols_to_byte(const uint8_t syms[4], rx_bit_order_t bit_order)
{
    if (!syms) {
        return 0U;
    }

    uint8_t byte = 0U;

    switch (bit_order) {
    case RX_BIT_ORDER_LSB_BIT_FIRST:
        for (uint8_t i = 0U; i < 4U; i++) {
            uint8_t s = (uint8_t)(syms[i] & 0x03U);
            uint8_t b_first  = (uint8_t)((s >> 1U) & 0x01U);
            uint8_t b_second = (uint8_t)(s & 0x01U);
            byte |= (uint8_t)(b_first << (2U * i));
            byte |= (uint8_t)(b_second << (2U * i + 1U));
        }
        break;

    case RX_BIT_ORDER_LSB_DIBIT_FIRST:
        byte |= (uint8_t)((syms[0] & 0x03U) << 0U);
        byte |= (uint8_t)((syms[1] & 0x03U) << 2U);
        byte |= (uint8_t)((syms[2] & 0x03U) << 4U);
        byte |= (uint8_t)((syms[3] & 0x03U) << 6U);
        break;

    case RX_BIT_ORDER_MSB_FIRST:
    default:
        byte |= (uint8_t)((syms[0] & 0x03U) << 6U);
        byte |= (uint8_t)((syms[1] & 0x03U) << 4U);
        byte |= (uint8_t)((syms[2] & 0x03U) << 2U);
        byte |= (uint8_t)((syms[3] & 0x03U) << 0U);
        break;
    }

    return byte;
}

uint16_t rx_crc16_ccitt(const uint8_t *data, size_t len)
{
    if (!data && len > 0U) {
        return 0U;
    }

    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8U);
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            if (crc & 0x8000U) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1U);
            }
        }
    }
    return crc;
}
