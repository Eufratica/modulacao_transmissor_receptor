#ifndef QAM_CONFIG_H
#define QAM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CONFIGURAÇÃO CENTRALIZADA DO TRANSMISSOR 4-QAM
 * ============================================================================
 * Todos os parâmetros de modulação, enquadramento, escala elétrica e PWM
 * ficam centralizados nesta estrutura para permitir ajustes de bancada ou
 * adaptação a requisitos do receptor sem reescrever o modulador.
 * ============================================================================ */

#define QAM_CONSTELLATION_SIZE      4U
#define QAM_MAX_SYNC_WORD_LEN       8U
#define QAM_MAX_CONT_TEXT_LEN       128U
#define QAM_MAX_PAYLOAD_BYTES       240U
#define QAM_MAX_FRAME_BYTES         272U
#define QAM_MAX_FRAME_SYMBOLS       (QAM_MAX_FRAME_BYTES * 4U)

/**
 * @brief Presets de tabela de constelação 4-QAM suportados.
 */
typedef enum {
    /** Padrão natural (BPSK em I e Q): 0=(-1,-1), 1=(-1,+1), 2=(+1,-1), 3=(+1,+1). */
    QAM_MAP_NATURAL = 0,
    /** Tabela Gray alternativa: 0=(+1,+1), 1=(-1,+1), 2=(+1,-1), 3=(-1,-1). */
    QAM_MAP_GRAY = 1,
} qam_map_preset_t;

/**
 * @brief Ordem de extração dos bits dentro de cada byte ASCII.
 */
typedef enum {
    /** Bit mais significativo primeiro: pares (b7,b6), (b5,b4), (b3,b2), (b1,b0). */
    QAM_BIT_ORDER_MSB_FIRST = 0,
    /** Fluxo serial LSB primeiro: pares (b0,b1), (b2,b3), (b4,b5), (b6,b7). */
    QAM_BIT_ORDER_LSB_BIT_FIRST = 1,
    /** Dibits do menos significativo ao mais significativo mantendo MSB interno: (b1,b0), (b3,b2), (b5,b4), (b7,b6). */
    QAM_BIT_ORDER_LSB_DIBIT_FIRST = 2,
} qam_bit_order_t;

/**
 * @brief Modos principais de operação do transmissor.
 */
typedef enum {
    /** Modo 1: Compatibilidade estrita com receptor externo (sem cabeçalho próprio/CRC por padrão). */
    QAM_TX_MODE_1_PROFESSOR = 1,
    /** Modo 2: Quadro próprio estruturado (Preâmbulo + Sync + Metadados + Payload + CRC-16). */
    QAM_TX_MODE_2_FRAMED = 2,
    /** Modo 3: Transmissão contínua de símbolos, portadora pura ou texto em loop. */
    QAM_TX_MODE_3_CONTINUOUS = 3,
} qam_tx_mode_t;

/**
 * @brief Padrões disponíveis quando em Modo 3 (transmissão contínua).
 */
typedef enum {
    QAM_CONT_PATTERN_TEXT = 0,         /**< Repete continuamente o texto configurado em cont_text. */
    QAM_CONT_PATTERN_FIXED_SYMBOL = 1, /**< Transmite continuamente um único símbolo fixo (0..3). */
    QAM_CONT_PATTERN_CYCLE_4 = 2,      /**< Alterna ciclicamente entre os 4 símbolos: 00 -> 01 -> 11 -> 10. */
    QAM_CONT_PATTERN_RAW_CARRIER = 3,  /**< Portadora senoidal pura A_pk * cos(2*pi*fc*t) para calibração. */
} qam_cont_pattern_t;

/**
 * @brief Tipo de conformação de pulso aplicada sobre o envelope de cada símbolo.
 */
typedef enum {
    QAM_PULSE_RECTANGULAR = 0,         /**< Pulso retangular clássico (padrão de maior interoperabilidade). */
    QAM_PULSE_RAISED_EDGE_TAPER = 1,   /**< Janela suave de cosseno nas bordas (10%) para reduzir cliques. */
} qam_pulse_shape_t;

/**
 * @brief Comportamento do transmissor quando não há dados na fila (Modos 1 e 2).
 */
typedef enum {
    QAM_IDLE_DC_SILENCE = 0,           /**< Mantém tensão fixa em dc_offset_mv (sem portadora). */
    QAM_IDLE_PILOT_SYMBOL_00 = 1,      /**< Mantém símbolo piloto 00 contínuo para travamento de PLL. */
} qam_idle_mode_t;

/**
 * @brief Ponto da constelação 4-QAM para um par de bits (símbolo 0..3).
 */
typedef struct {
    int8_t i_val;        /**< Componente em fase I (+1 ou -1). */
    int8_t q_val;        /**< Componente em quadratura Q (+1 ou -1). */
    uint16_t phase_deg;  /**< Ângulo geométrico atan2(Q, I) no plano I/Q em graus. */
    char bits_label[3];  /**< Rótulo textual dos bits ("00", "01", "10", "11"). */
} qam_symbol_point_t;

/**
 * @brief Estrutura completa e centralizada de configuração do transmissor 4-QAM.
 */
typedef struct {
    /* Parâmetros de modulação e sinal */
    uint32_t carrier_freq_hz;          /**< Frequência da portadora fc em Hz (padrão: 1000 Hz). */
    uint32_t symbol_rate_baud;         /**< Taxa de símbolos Rs em baud (provisório: 100 baud). */
    uint32_t sample_rate_hz;           /**< Taxa de atualização das amostras fs em Hz (padrão: 40000 Hz). */
    uint8_t bits_per_symbol;           /**< Bits por símbolo (4-QAM = 2). */

    /* Escala elétrica e convenção da equação */
    uint16_t vdd_mv;                   /**< Tensão lógica de alimentação da saída PWM em mV (3300 mV). */
    uint16_t dc_offset_mv;             /**< Offset DC analógico em mV após o filtro (padrão: 1300 mV). */
    uint16_t amplitude_peak_mv;        /**< Amplitude de pico Vpk da senoide resultante em mV (padrão: 900 mV). */
    bool invert_polarity;              /**< Inverte o sinal global da onda (-s(t)) se true. */
    bool q_sign_minus;                 /**< false (padrão): I*cos + Q*cos(wt-90°) = I*cos + Q*sin; true: I*cos - Q*sin. */

    /* Mapeamento e ordem de bits */
    qam_map_preset_t map_preset;       /**< Preset ativo da constelação (Natural ou Gray). */
    qam_symbol_point_t constellation[QAM_CONSTELLATION_SIZE]; /**< Indexado pelo valor de 2 bits (0..3). */
    qam_bit_order_t bit_order;         /**< Ordem de extração dos bits por byte ASCII. */
    qam_pulse_shape_t pulse_shape;     /**< Conformação de pulso de símbolo. */

    /* Enquadramento, sincronismo e modos */
    qam_tx_mode_t tx_mode;             /**< Modo ativo de transmissão (1, 2 ou 3). */
    qam_idle_mode_t idle_mode;         /**< Saída enquanto ocioso nos Modos 1 e 2. */
    bool mode1_use_preamble;           /**< No Modo 1, opcionalmente precede dados com preâmbulo (padrão: false). */
    bool mode1_use_sync_word;          /**< No Modo 1, opcionalmente precede dados com sync word (padrão: false). */
    uint8_t preamble_bytes;            /**< Quantidade de bytes de preâmbulo (metade 0x00 + metade 0x33). */
    uint8_t sync_word[QAM_MAX_SYNC_WORD_LEN]; /**< Sequência de sincronização (padrão: 0xD3, 0x91). */
    uint8_t sync_word_len;             /**< Tamanho efetivo de sync_word em bytes (1..8). */

    /* Parâmetros do Modo 3 (contínuo) */
    qam_cont_pattern_t cont_pattern;   /**< Padrão contínuo ativo no Modo 3. */
    uint8_t cont_fixed_symbol;         /**< Símbolo fixo (0..3) para QAM_CONT_PATTERN_FIXED_SYMBOL. */
    char cont_text[QAM_MAX_CONT_TEXT_LEN]; /**< Texto repetido para QAM_CONT_PATTERN_TEXT. */

    /* Configuração de hardware PWM (MCPWM) */
    int pwm_gpio;                      /**< Pino GPIO de saída PWM (padrão: GPIO 18). */
    uint32_t pwm_freq_hz;              /**< Frequência de chaveamento PWM em Hz (padrão: 80000 Hz). */
    bool pwm_symmetric;                /**< true: PWM simétrico UP-DOWN; false: assimétrico UP. */
    bool pwm_invert_output;            /**< Inversão lógica do pino PWM em hardware (padrão: false). */
} qam_tx_config_t;

/**
 * @brief Carrega os valores padrão (tabela Natural 0..3 e equação com sinal positivo +).
 */
void qam_config_set_defaults(qam_tx_config_t *cfg);

/**
 * @brief Aplica um dos presets de mapeamento (Natural ou Gray) à estrutura de configuração.
 */
void qam_config_apply_map_preset(qam_tx_config_t *cfg, qam_map_preset_t preset);

/**
 * @brief Valida coerência matemática, limites de Nyquist e segurança elétrica da configuração.
 */
bool qam_config_validate(const qam_tx_config_t *cfg, char *err_buf, size_t err_buf_sz);

/**
 * @brief Retorna o texto descritivo do modo de transmissão.
 */
const char *qam_config_mode_name(qam_tx_mode_t mode);

/**
 * @brief Retorna o texto descritivo da ordem de bits.
 */
const char *qam_config_bit_order_name(qam_bit_order_t order);

/**
 * @brief Retorna o texto descritivo do padrão contínuo.
 */
const char *qam_config_cont_pattern_name(qam_cont_pattern_t pattern);

/**
 * @brief Retorna o texto descritivo do preset de constelação.
 */
const char *qam_config_map_name(qam_map_preset_t preset);

#ifdef __cplusplus
}
#endif

#endif /* QAM_CONFIG_H */
