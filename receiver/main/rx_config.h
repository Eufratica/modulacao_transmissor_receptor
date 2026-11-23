#ifndef RX_CONFIG_H
#define RX_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CONFIGURAÇÃO CENTRALIZADA DO RECEPTOR 4-QAM
 * ============================================================================
 * Espelha os parâmetros de modulação, mapeamento e enquadramento do
 * transmissor para permitir ajustes dinâmicos na bancada sem recompilar.
 * ============================================================================ */

#define RX_CONSTELLATION_SIZE      4U
#define RX_MAX_SYNC_WORD_LEN       8U
#define RX_MAX_PAYLOAD_BYTES       240U

/**
 * @brief Presets de constelação 4-QAM (idênticos ao transmissor).
 */
typedef enum {
    RX_MAP_NATURAL = 0, /**< Padrão: 0=(-1,-1), 1=(-1,+1), 2=(+1,-1), 3=(+1,+1). */
    RX_MAP_GRAY    = 1, /**< Alternativo: 0=(+1,+1), 1=(-1,+1), 2=(+1,-1), 3=(-1,-1). */
} rx_map_preset_t;

/**
 * @brief Ordem de reconstrução dos bits em cada byte ASCII.
 */
typedef enum {
    RX_BIT_ORDER_MSB_FIRST       = 0,
    RX_BIT_ORDER_LSB_BIT_FIRST   = 1,
    RX_BIT_ORDER_LSB_DIBIT_FIRST = 2,
} rx_bit_order_t;

/**
 * @brief Modo de operação do desenquadrador no receptor.
 */
typedef enum {
    /** Modo 1: Decodificação direta de fluxo ASCII (com ou sem sync_word configurada). */
    RX_MODE_1_RAW_STREAM = 1,
    /** Modo 2: Sincronismo por Sync Word, extração de metadados e validação CRC-16. */
    RX_MODE_2_FRAMED     = 2,
} rx_decode_mode_t;

typedef struct {
    int8_t i_val;
    int8_t q_val;
    uint16_t phase_deg;
    char bits_label[3];
} rx_symbol_point_t;

typedef struct {
    /* Parâmetros do sinal esperado */
    uint32_t carrier_freq_hz;       /**< Frequência nominal da portadora fc (padrão: 1000 Hz). */
    uint32_t symbol_rate_baud;      /**< Taxa nominal de símbolos Rs (provisório: 100 baud). */
    uint32_t sample_rate_hz;        /**< Taxa de amostragem do ADC1 DMA (padrão: 20000 Hz). */

    /* Convenção de quadratura e constelação */
    bool q_sign_minus;              /**< false: +cos(wt-90) [+sin] | true: -sin. */
    bool invert_polarity;           /**< Inversão global do sinal recebido. */
    rx_map_preset_t map_preset;     /**< Tabela Natural ou Gray. */
    rx_symbol_point_t constellation[RX_CONSTELLATION_SIZE];
    rx_bit_order_t bit_order;       /**< Ordem dos bits no Modo 1 (Modo 2 lê do cabeçalho). */

    /* Enquadramento e sincronismo */
    rx_decode_mode_t rx_mode;       /**< Modo 1 (direto) ou Modo 2 (quadro + CRC-16). */
    bool mode1_use_sync_word;       /**< Se true no Modo 1, aguarda sync_word antes de imprimir. */
    uint8_t fixed_rotation_quad;    /**< Rotação manual 0..3 (0/90/180/270°) usada no Modo 1 sem sync. */
    uint8_t sync_word[RX_MAX_SYNC_WORD_LEN];
    uint8_t sync_word_len;

    /* Limiares de detecção e ganhos das malhas de sincronização */
    float squelch_rms_mv;           /**< Limiar RMS AC mínimo para detectar presença de portadora (mV). */
    float costas_kp;                /**< Ganho proporcional do Costas Loop. */
    float costas_ki;                /**< Ganho integral do Costas Loop. */
    float gardner_kp;               /**< Ganho proporcional da recuperação de símbolo (Gardner). */
    float gardner_ki;               /**< Ganho integral de deriva de relógio de símbolo. */

    /* Hardware ADC1 (DMA I2S0 no ESP32) */
    int adc1_channel;               /**< Canal ADC1 (padrão: 6 = GPIO 34 no ESP32). */
    bool adc_use_cal;               /**< Aplica calibração eFuse (mV) quando disponível. */
} qam_rx_config_t;

void rx_config_set_defaults(qam_rx_config_t *cfg);
void rx_config_apply_map_preset(qam_rx_config_t *cfg, rx_map_preset_t preset);
bool rx_config_validate(const qam_rx_config_t *cfg, char *err_buf, size_t err_buf_sz);
int  rx_config_adc1_channel_to_gpio(int channel);

const char *rx_config_mode_name(rx_decode_mode_t mode);
const char *rx_config_bit_order_name(rx_bit_order_t order);
const char *rx_config_map_name(rx_map_preset_t preset);

#ifdef __cplusplus
}
#endif

#endif /* RX_CONFIG_H */
