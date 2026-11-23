#include "qam_mapper.h"

void qam_mapper_byte_to_symbols(uint8_t byte, qam_bit_order_t bit_order, uint8_t out_syms[4])
{
    if (!out_syms) {
        return;
    }

    switch (bit_order) {
    case QAM_BIT_ORDER_LSB_BIT_FIRST:
        /* Fluxo serial LSB->MSB: b0, b1, b2, b3, b4, b5, b6, b7.
         * Em cada par (first_bit, second_bit), first_bit ocupa o bit mais à esquerda do par. */
        for (uint8_t i = 0; i < 4U; i++) {
            uint8_t b_first  = (uint8_t)((byte >> (2U * i)) & 0x01U);
            uint8_t b_second = (uint8_t)((byte >> (2U * i + 1U)) & 0x01U);
            out_syms[i] = (uint8_t)((b_first << 1U) | b_second);
        }
        break;

    case QAM_BIT_ORDER_LSB_DIBIT_FIRST:
        out_syms[0] = (uint8_t)((byte >> 0U) & 0x03U);
        out_syms[1] = (uint8_t)((byte >> 2U) & 0x03U);
        out_syms[2] = (uint8_t)((byte >> 4U) & 0x03U);
        out_syms[3] = (uint8_t)((byte >> 6U) & 0x03U);
        break;

    case QAM_BIT_ORDER_MSB_FIRST:
    default:
        out_syms[0] = (uint8_t)((byte >> 6U) & 0x03U);
        out_syms[1] = (uint8_t)((byte >> 4U) & 0x03U);
        out_syms[2] = (uint8_t)((byte >> 2U) & 0x03U);
        out_syms[3] = (uint8_t)((byte >> 0U) & 0x03U);
        break;
    }
}

uint8_t qam_mapper_symbols_to_byte(const uint8_t in_syms[4], qam_bit_order_t bit_order)
{
    if (!in_syms) {
        return 0U;
    }

    uint8_t byte = 0U;

    switch (bit_order) {
    case QAM_BIT_ORDER_LSB_BIT_FIRST:
        for (uint8_t i = 0; i < 4U; i++) {
            uint8_t s = (uint8_t)(in_syms[i] & 0x03U);
            uint8_t b_first  = (uint8_t)((s >> 1U) & 0x01U);
            uint8_t b_second = (uint8_t)(s & 0x01U);
            byte |= (uint8_t)(b_first << (2U * i));
            byte |= (uint8_t)(b_second << (2U * i + 1U));
        }
        break;

    case QAM_BIT_ORDER_LSB_DIBIT_FIRST:
        byte |= (uint8_t)((in_syms[0] & 0x03U) << 0U);
        byte |= (uint8_t)((in_syms[1] & 0x03U) << 2U);
        byte |= (uint8_t)((in_syms[2] & 0x03U) << 4U);
        byte |= (uint8_t)((in_syms[3] & 0x03U) << 6U);
        break;

    case QAM_BIT_ORDER_MSB_FIRST:
    default:
        byte |= (uint8_t)((in_syms[0] & 0x03U) << 6U);
        byte |= (uint8_t)((in_syms[1] & 0x03U) << 4U);
        byte |= (uint8_t)((in_syms[2] & 0x03U) << 2U);
        byte |= (uint8_t)((in_syms[3] & 0x03U) << 0U);
        break;
    }

    return byte;
}

bool qam_mapper_bytes_to_symbols(const uint8_t *data, size_t data_len,
                                 qam_bit_order_t bit_order,
                                 uint8_t *out_syms, size_t max_syms,
                                 size_t *out_sym_count)
{
    if (!out_syms || (data_len > 0U && !data)) {
        return false;
    }
    if (data_len > (max_syms / 4U)) {
        return false;
    }

    for (size_t i = 0; i < data_len; i++) {
        qam_mapper_byte_to_symbols(data[i], bit_order, &out_syms[i * 4U]);
    }

    if (out_sym_count) {
        *out_sym_count = data_len * 4U;
    }
    return true;
}

bool qam_mapper_get_point(const qam_tx_config_t *cfg, uint8_t sym, qam_symbol_point_t *out_pt)
{
    if (!cfg || !out_pt || sym >= QAM_CONSTELLATION_SIZE) {
        return false;
    }
    *out_pt = cfg->constellation[sym];
    return true;
}

uint16_t qam_crc16_ccitt(const uint8_t *data, size_t len)
{
    if (!data && len > 0U) {
        return 0U;
    }

    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8U);
        for (uint8_t bit = 0; bit < 8U; bit++) {
            if (crc & 0x8000U) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1U);
            }
        }
    }
    return crc;
}
