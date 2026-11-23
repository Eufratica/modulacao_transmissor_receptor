#ifndef QAM_MODULATOR_H
#define QAM_MODULATOR_H

#include <stdbool.h>
#include <stdint.h>
#include "qam_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULADOR DIGITAL 4-QAM E GERADOR DE PORTADORA (NCO DE FASE EXATA)
 * ============================================================================
 * Implementa a equação obrigatória:
 *     s(t) = A * [ I * cos(2*pi*fc*t) - Q * sin(2*pi*fc*t) ]
 * onde A = amplitude_peak_mv / sqrt(2) para que a envoltória de pico
 * permaneça exatamente em amplitude_peak_mv sem saturação.
 * ============================================================================ */

/**
 * @brief Estado interno do modulador 4-QAM (acumuladores de portadora e de símbolo).
 */
typedef struct {
    qam_tx_config_t cfg;         /**< Cópia local dos parâmetros ativos de modulação. */
    uint32_t pwm_period_ticks;   /**< Topo do contador PWM (ex: 1000 para 80 kHz simétrico). */
    uint32_t carrier_phase_num;  /**< Acumulador racional de fase da portadora [0 .. fs-1]. */
    uint32_t sym_phase_num;      /**< Acumulador racional de duração de símbolo [0 .. fs-1]. */
    float a_eq_mv;               /**< Amplitude A = Vpk / sqrt(2) em milivolts. */
    float polarity_sign;         /**< +1.0f ou -1.0f conforme cfg.invert_polarity. */
    bool lut_ready;              /**< Indica se a tabela senoidal foi inicializada. */
} qam_modulator_t;

/**
 * @brief Inicializa o modulador 4-QAM e gera a LUT senoidal de alta precisão.
 *
 * @param mod              Instância do modulador.
 * @param cfg              Configuração validada.
 * @param pwm_period_ticks Valor máximo de contagem do PWM correspondente a 100% de duty cycle.
 * @return true em sucesso; false se parâmetros inválidos.
 */
bool qam_modulator_init(qam_modulator_t *mod, const qam_tx_config_t *cfg, uint32_t pwm_period_ticks);

/**
 * @brief Atualiza parâmetros de configuração e período PWM preservando ou reiniciando fase.
 *
 * @param mod              Instância do modulador.
 * @param cfg              Nova configuração validada.
 * @param pwm_period_ticks Novo valor de período PWM em ticks.
 * @param reset_phase      true para zerar fase da portadora e do símbolo; false para manter continuidade.
 * @return true em sucesso.
 */
bool qam_modulator_reconfigure(qam_modulator_t *mod, const qam_tx_config_t *cfg,
                               uint32_t pwm_period_ticks, bool reset_phase);

/**
 * @brief Reinicia os acumuladores de fase da portadora e do símbolo (t = 0).
 */
void qam_modulator_reset_phase(qam_modulator_t *mod);

/**
 * @brief Gera a próxima amostra modulada em tensão (mV) para o símbolo 4-QAM informado.
 *
 * @param mod             Instância do modulador.
 * @param symbol          Índice do símbolo (0..3).
 * @param symbol_finished Ponteiro opcional ajustado para true na última amostra deste símbolo.
 * @return Tensão instantânea analógica esperada após filtro (em mV).
 */
float qam_modulator_next_symbol_mv(qam_modulator_t *mod, uint8_t symbol, bool *symbol_finished);

/**
 * @brief Gera a próxima amostra da portadora pura V_DC + pol * V_pk * cos(2*pi*fc*t) em mV.
 */
float qam_modulator_next_carrier_mv(qam_modulator_t *mod);

/**
 * @brief Converte uma tensão analógica alvo (mV) para ticks de comparação PWM seguros.
 */
uint16_t qam_modulator_mv_to_pwm_cmp(const qam_modulator_t *mod, float voltage_mv);

/**
 * @brief Gera diretamente a próxima amostra em ticks PWM para um símbolo 4-QAM.
 */
uint16_t qam_modulator_next_symbol_cmp(qam_modulator_t *mod, uint8_t symbol, bool *symbol_finished);

/**
 * @brief Gera diretamente a próxima amostra em ticks PWM para portadora pura.
 */
uint16_t qam_modulator_next_carrier_cmp(qam_modulator_t *mod);

/**
 * @brief Retorna o valor de comparação PWM correspondente ao repouso DC (offset).
 */
uint16_t qam_modulator_idle_dc_cmp(const qam_modulator_t *mod);

#ifdef __cplusplus
}
#endif

#endif /* QAM_MODULATOR_H */
