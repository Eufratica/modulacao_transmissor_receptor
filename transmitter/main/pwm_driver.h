#ifndef PWM_DRIVER_H
#define PWM_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DRIVER PWM DETERMINÍSTICO VIA MCPWM0 (ESP32 @ 160 MHz)
 * ============================================================================
 * Utiliza o periférico MCPWM0 Timer 0 com registradores shadow atualizados
 * estritamente no evento TEZ (Timer Equal Zero) dentro de ISR em IRAM,
 * alimentada por ring buffer lock-free SPSC preenchido pela task DSP.
 * ============================================================================ */

#define PWM_DRIVER_RINGBUF_LEN   1024U /* Deve ser potência de 2 */

/**
 * @brief Estatísticas de execução do driver PWM para diagnóstico e validação.
 */
typedef struct {
    uint32_t period_ticks;       /**< Topo do contador PWM (ex: 1000 para 80 kHz simétrico). */
    uint32_t actual_pwm_hz;      /**< Frequência PWM real gerada pelo divisor de hardware. */
    uint32_t sample_rate_hz;     /**< Taxa efetiva de atualização de amostras. */
    uint32_t isr_subdiv;         /**< Razão inteira pwm_freq_hz / sample_rate_hz. */
    uint32_t samples_played;     /**< Total de amostras enviadas ao comparador PWM. */
    uint32_t underrun_count;     /**< Contador de esvaziamento do ring buffer (deve ser 0). */
    uint32_t buffered_samples;   /**< Amostras atualmente enfileiradas no ring buffer. */
} pwm_driver_stats_t;

/**
 * @brief Calcula o número de ticks correspondente a 100% do ciclo PWM no MCPWM (160 MHz).
 */
uint32_t pwm_driver_calc_period_ticks(uint32_t pwm_freq_hz, bool symmetric);

/**
 * @brief Inicializa o MCPWM0, configura o GPIO e registra a ISR TEZ em IRAM no núcleo atual.
 *
 * @param gpio_num       GPIO de saída do sinal PWM.
 * @param pwm_freq_hz    Frequência de chaveamento PWM em Hz.
 * @param sample_rate_hz Taxa de atualização de amostras (divisor inteiro de pwm_freq_hz).
 * @param symmetric      true para modo simétrico (UP-DOWN); false para assimétrico (UP).
 * @param invert_output  true para inverter polaridade lógica na saída do pino.
 * @param idle_cmp       Valor inicial de comparação PWM em repouso.
 * @param notify_task    Handle da task FreeRTOS a ser notificada para reabastecer o buffer.
 * @return ESP_OK em sucesso ou código de erro ESP-IDF.
 */
esp_err_t pwm_driver_init(int gpio_num,
                          uint32_t pwm_freq_hz,
                          uint32_t sample_rate_hz,
                          bool symmetric,
                          bool invert_output,
                          uint16_t idle_cmp,
                          TaskHandle_t notify_task);

/**
 * @brief Reconfigura parâmetros de frequência, modo ou GPIO sem perder o controle da ISR.
 */
esp_err_t pwm_driver_reconfigure(int gpio_num,
                                 uint32_t pwm_freq_hz,
                                 uint32_t sample_rate_hz,
                                 bool symmetric,
                                 bool invert_output,
                                 uint16_t idle_cmp);

/**
 * @brief Insere amostras no ring buffer lock-free SPSC.
 *
 * @param samples     Ponteiro para vetor de ticks de comparação.
 * @param count       Quantidade desejada para escrita.
 * @return Quantidade efetivamente inserida (<= count).
 */
size_t pwm_driver_push_samples(const uint16_t *samples, size_t count);

/**
 * @brief Retorna quantas posições livres ainda existem no ring buffer.
 */
size_t pwm_driver_free_space(void);

/**
 * @brief Atualiza o valor de segurança usado em caso de esvaziamento (idle_cmp).
 */
void pwm_driver_set_idle_cmp(uint16_t idle_cmp);

/**
 * @brief Lê métricas e estatísticas atuais do driver PWM.
 */
void pwm_driver_get_stats(pwm_driver_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWM_DRIVER_H */
