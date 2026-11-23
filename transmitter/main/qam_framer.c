#include "qam_framer.h"
#include "qam_mapper.h"

#include <string.h>

static bool append_bytes(const uint8_t *bytes, size_t len, qam_bit_order_t order,
                         qam_tx_frame_t *frame)
{
    size_t needed_syms = len * 4U;
    if (frame->symbol_count + needed_syms > QAM_MAX_FRAME_SYMBOLS) {
        return false;
    }

    size_t written = 0U;
    if (!qam_mapper_bytes_to_symbols(bytes, len, order,
                                     &frame->symbols[frame->symbol_count],
                                     QAM_MAX_FRAME_SYMBOLS - frame->symbol_count,
                                     &written)) {
        return false;
    }

    frame->symbol_count += written;
    return true;
}

static bool append_preamble(uint8_t preamble_bytes, qam_tx_frame_t *frame)
{
    if (preamble_bytes == 0U) {
        return true;
    }
    if (preamble_bytes > QAM_MAX_PREAMBLE_BYTES) {
        preamble_bytes = QAM_MAX_PREAMBLE_BYTES;
    }

    /* Primeira metade: 0x00 (símbolos 00 constantes -> trava fase/AGC da portadora).
     * Segunda metade: 0x33 (símbolos 00,11,00,11 -> saltos de 180° para recuperar clock de símbolo). */
    uint8_t buf[QAM_MAX_PREAMBLE_BYTES];
    uint8_t carrier_half = (uint8_t)(preamble_bytes / 2U);
    for (uint8_t i = 0; i < preamble_bytes; i++) {
        buf[i] = (i < carrier_half) ? 0x00U : 0x33U;
    }

    return append_bytes(buf, preamble_bytes, QAM_BIT_ORDER_MSB_FIRST, frame);
}

bool qam_framer_build_frame(const qam_tx_config_t *cfg,
                            const uint8_t *data, size_t data_len,
                            qam_tx_frame_t *out_frame)
{
    if (!cfg || !out_frame || (data_len > 0U && !data)) {
        return false;
    }
    if (data_len > QAM_MAX_PAYLOAD_BYTES) {
        return false;
    }

    memset(out_frame, 0, sizeof(*out_frame));

    if (cfg->tx_mode == QAM_TX_MODE_1_PROFESSOR) {
        /* Modo 1: NÃO acrescenta cabeçalho próprio ou CRC.
         * Preâmbulo e sync só são inseridos se o usuário habilitar explicitamente. */
        if (cfg->mode1_use_preamble) {
            if (!append_preamble(cfg->preamble_bytes, out_frame)) {
                return false;
            }
        }
        if (cfg->mode1_use_sync_word) {
            if (!append_bytes(cfg->sync_word, cfg->sync_word_len, QAM_BIT_ORDER_MSB_FIRST, out_frame)) {
                return false;
            }
        }
        out_frame->header_symbol_count = out_frame->symbol_count;

        if (!append_bytes(data, data_len, cfg->bit_order, out_frame)) {
            return false;
        }
        out_frame->payload_symbol_count = out_frame->symbol_count - out_frame->header_symbol_count;
        out_frame->crc16 = 0U;
        return true;
    }

    if (cfg->tx_mode == QAM_TX_MODE_2_FRAMED) {
        /* Modo 2: Quadro próprio completo e verificável */
        uint8_t pre_len = (cfg->preamble_bytes < 2U) ? 2U : cfg->preamble_bytes;
        if (!append_preamble(pre_len, out_frame)) {
            return false;
        }

        if (!append_bytes(cfg->sync_word, cfg->sync_word_len, QAM_BIT_ORDER_MSB_FIRST, out_frame)) {
            return false;
        }

        /* Cabeçalho de metadados (4 bytes em MSB-first para leitura universal antes do payload):
         * [0] = Versão (bits 7..4) | (bits_per_symbol - 2) (bits 3..2) | bit_order (bits 1..0)
         * [1..2] = Taxa de símbolos em baud (uint16 big-endian)
         * [3] = Comprimento do payload em bytes (0..240)
         */
        uint8_t header[4];
        header[0] = (uint8_t)(((QAM_FRAME_PROTOCOL_VERSION & 0x0FU) << 4U) |
                              (((cfg->bits_per_symbol - 2U) & 0x03U) << 2U) |
                              ((uint8_t)cfg->bit_order & 0x03U));
        header[1] = (uint8_t)((cfg->symbol_rate_baud >> 8U) & 0xFFU);
        header[2] = (uint8_t)(cfg->symbol_rate_baud & 0xFFU);
        header[3] = (uint8_t)(data_len & 0xFFU);

        if (!append_bytes(header, sizeof(header), QAM_BIT_ORDER_MSB_FIRST, out_frame)) {
            return false;
        }
        out_frame->header_symbol_count = out_frame->symbol_count;

        if (!append_bytes(data, data_len, cfg->bit_order, out_frame)) {
            return false;
        }
        out_frame->payload_symbol_count = out_frame->symbol_count - out_frame->header_symbol_count;

        /* Calcula CRC-16 sobre [header(4) + payload(data_len)] */
        uint8_t crc_input[4U + QAM_MAX_PAYLOAD_BYTES];
        memcpy(crc_input, header, sizeof(header));
        if (data_len > 0U) {
            memcpy(&crc_input[sizeof(header)], data, data_len);
        }
        uint16_t crc = qam_crc16_ccitt(crc_input, sizeof(header) + data_len);
        out_frame->crc16 = crc;

        uint8_t crc_bytes[2] = {
            (uint8_t)((crc >> 8U) & 0xFFU),
            (uint8_t)(crc & 0xFFU)
        };
        if (!append_bytes(crc_bytes, sizeof(crc_bytes), QAM_BIT_ORDER_MSB_FIRST, out_frame)) {
            return false;
        }
        return true;
    }

    /* Para Modo 3 ou modo desconhecido, trata como sequência direta de payload se chamado */
    if (!append_bytes(data, data_len, cfg->bit_order, out_frame)) {
        return false;
    }
    out_frame->payload_symbol_count = out_frame->symbol_count;
    return true;
}
