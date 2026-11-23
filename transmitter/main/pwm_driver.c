#include "pwm_driver.h"

#include "driver/gpio.h"
#include "driver/mcpwm.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "hal/mcpwm_ll.h"
#include "soc/mcpwm_struct.h"
#include "soc/soc_caps.h"

static const char *TAG = "pwm_driver";

#define PWM_RINGBUF_MASK (PWM_DRIVER_RINGBUF_LEN - 1U)

_Static_assert((PWM_DRIVER_RINGBUF_LEN & (PWM_DRIVER_RINGBUF_LEN - 1U)) == 0U,
               "PWM_DRIVER_RINGBUF_LEN deve ser potencia de 2");

/* Estado compartilhado com a ISR em DRAM (nunca colocar em Flash) */
static DRAM_ATTR uint16_t s_ringbuf[PWM_DRIVER_RINGBUF_LEN];
static DRAM_ATTR volatile uint32_t s_head = 0U;
static DRAM_ATTR volatile uint32_t s_tail = 0U;
static DRAM_ATTR volatile uint16_t s_idle_cmp = 500U;
static DRAM_ATTR volatile uint16_t s_last_cmp = 500U;
static DRAM_ATTR volatile uint32_t s_isr_subdiv = 2U;
static DRAM_ATTR volatile uint32_t s_subdiv_cnt = 0U;
static DRAM_ATTR volatile uint32_t s_samples_played = 0U;
static DRAM_ATTR volatile uint32_t s_underrun_count = 0U;
static DRAM_ATTR TaskHandle_t s_notify_task = NULL;

static intr_handle_t s_isr_handle = NULL;
static bool s_initialized = false;
static int s_active_gpio = -1;
static uint32_t s_period_ticks = 1000U;
static uint32_t s_actual_pwm_hz = 80000U;
static uint32_t s_sample_rate_hz = 40000U;

uint32_t pwm_driver_calc_period_ticks(uint32_t pwm_freq_hz, bool symmetric)
{
    if (pwm_freq_hz == 0U) {
        return 0U;
    }
    uint32_t base_clk = (uint32_t)SOC_MCPWM_BASE_CLK_HZ; /* 160 MHz no ESP32 */
    uint32_t ticks = symmetric ? (base_clk / (2U * pwm_freq_hz))
                               : (base_clk / pwm_freq_hz);
    if (ticks < 2U) {
        ticks = 2U;
    } else if (ticks > 65535U) {
        ticks = 65535U;
    }
    return ticks;
}

static void IRAM_ATTR pwm_tez_isr(void *arg)
{
    (void)arg;
    mcpwm_dev_t *hw = MCPWM_LL_GET_HW(MCPWM_UNIT_0);

    uint32_t tez_status = mcpwm_ll_intr_get_timer_tez_status(hw);
    if ((tez_status & BIT(MCPWM_TIMER_0)) == 0U) {
        return;
    }
    mcpwm_ll_intr_clear_timer_tez_status(hw, BIT(MCPWM_TIMER_0));

    uint32_t cnt = s_subdiv_cnt + 1U;
    if (cnt < s_isr_subdiv) {
        s_subdiv_cnt = cnt;
        return;
    }
    s_subdiv_cnt = 0U;

    uint32_t head = s_head;
    uint32_t tail = s_tail;
    uint32_t avail = head - tail;
    uint16_t cmp_val;

    if (avail > 0U) {
        cmp_val = s_ringbuf[tail & PWM_RINGBUF_MASK];
        __sync_synchronize();
        s_tail = tail + 1U;
        s_last_cmp = cmp_val;
        s_samples_played++;
        avail--;
    } else {
        cmp_val = s_idle_cmp;
        s_last_cmp = cmp_val;
        s_underrun_count++;
    }

    /* Atualiza o registrador shadow do comparador A; hardware carrega no próximo TEZ */
    mcpwm_ll_operator_set_compare_value(hw, MCPWM_TIMER_0, 0, cmp_val);

    /* Acorda a task DSP periodicamente ou quando o nível do buffer cair abaixo da metade */
    if (s_notify_task != NULL && ((s_tail & 0x7FU) == 0U || avail <= (PWM_DRIVER_RINGBUF_LEN / 2U))) {
        BaseType_t higher_prio_woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_notify_task, &higher_prio_woken);
        if (higher_prio_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

static esp_err_t apply_mcpwm_hardware_config(int gpio_num,
                                             uint32_t pwm_freq_hz,
                                             uint32_t sample_rate_hz,
                                             bool symmetric,
                                             bool invert_output,
                                             uint16_t idle_cmp)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pwm_freq_hz == 0U || sample_rate_hz == 0U || (pwm_freq_hz % sample_rate_hz) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t period_ticks = pwm_driver_calc_period_ticks(pwm_freq_hz, symmetric);
    uint32_t subdiv = pwm_freq_hz / sample_rate_hz;
    if (subdiv == 0U) {
        subdiv = 1U;
    }

    if (idle_cmp >= period_ticks) {
        idle_cmp = (uint16_t)(period_ticks - 1U);
    } else if (idle_cmp < 1U) {
        idle_cmp = 1U;
    }

    mcpwm_dev_t *hw = MCPWM_LL_GET_HW(MCPWM_UNIT_0);

    if (s_initialized) {
        mcpwm_ll_intr_enable_timer_tez(hw, MCPWM_TIMER_0, false);
        mcpwm_ll_intr_clear_timer_tez_status(hw, BIT(MCPWM_TIMER_0));
        ESP_ERROR_CHECK(mcpwm_stop(MCPWM_UNIT_0, MCPWM_TIMER_0));
    }

    if (s_active_gpio >= 0 && s_active_gpio != gpio_num) {
        gpio_reset_pin(s_active_gpio);
    }

    esp_err_t err = mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, gpio_num);
    if (err != ESP_OK) {
        return err;
    }
    s_active_gpio = gpio_num;

    /* Configura clock base de 160 MHz sem pré-divisores artificiais */
    err = mcpwm_group_set_resolution(MCPWM_UNIT_0, (unsigned long int)SOC_MCPWM_BASE_CLK_HZ);
    if (err != ESP_OK) {
        return err;
    }
    err = mcpwm_timer_set_resolution(MCPWM_UNIT_0, MCPWM_TIMER_0, (unsigned long int)SOC_MCPWM_BASE_CLK_HZ);
    if (err != ESP_OK) {
        return err;
    }

    /* mcpwm_init espera frequência equivalente de contagem simples; corrigimos o topo exato logo abaixo */
    mcpwm_config_t mcpwm_cfg = {
        .frequency    = symmetric ? (pwm_freq_hz * 2U) : pwm_freq_hz,
        .cmpr_a       = 50.0f,
        .cmpr_b       = 0.0f,
        .duty_mode    = invert_output ? MCPWM_DUTY_MODE_1 : MCPWM_DUTY_MODE_0,
        .counter_mode = symmetric ? MCPWM_UP_DOWN_COUNTER : MCPWM_UP_COUNTER,
    };
    err = mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &mcpwm_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* Ajusta o período exato em ticks e garante carga de shadow APENAS no TEZ */
    mcpwm_ll_timer_set_peak(hw, MCPWM_TIMER_0, period_ticks, symmetric);
    mcpwm_ll_timer_enable_update_period_on_tez(hw, MCPWM_TIMER_0, true);
    mcpwm_ll_group_enable_shadow_mode(hw);
    mcpwm_ll_operator_enable_update_compare_on_tez(hw, MCPWM_TIMER_0, 0, true);
    mcpwm_ll_operator_enable_update_compare_on_tep(hw, MCPWM_TIMER_0, 0, false);
    mcpwm_ll_operator_set_compare_value(hw, MCPWM_TIMER_0, 0, idle_cmp);
    mcpwm_ll_group_flush_shadow(hw);

    /* Pré-carrega 256 amostras de repouso para evitar underrun na partida da ISR */
    for (uint32_t i = 0; i < 256U; i++) {
        s_ringbuf[i] = idle_cmp;
    }
    s_head = 256U;
    s_tail = 0U;
    s_subdiv_cnt = 0U;
    s_isr_subdiv = subdiv;
    s_idle_cmp = idle_cmp;
    s_last_cmp = idle_cmp;
    s_period_ticks = period_ticks;
    s_sample_rate_hz = sample_rate_hz;
    s_actual_pwm_hz = symmetric ? ((uint32_t)SOC_MCPWM_BASE_CLK_HZ / (2U * period_ticks))
                                : ((uint32_t)SOC_MCPWM_BASE_CLK_HZ / period_ticks);
    s_underrun_count = 0U;

    if (s_isr_handle == NULL) {
        err = mcpwm_isr_register(MCPWM_UNIT_0,
                                 pwm_tez_isr,
                                 NULL,
                                 ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1,
                                 &s_isr_handle);
        if (err != ESP_OK) {
            return err;
        }
    }

    mcpwm_ll_intr_clear_timer_tez_status(hw, BIT(MCPWM_TIMER_0));
    mcpwm_ll_intr_enable_timer_tez(hw, MCPWM_TIMER_0, true);
    err = mcpwm_start(MCPWM_UNIT_0, MCPWM_TIMER_0);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "MCPWM0 inicializado: GPIO=%d | modo=%s | f_pwm=%u Hz (period=%u ticks @ 160 MHz) | fs=%u Hz (subdiv=%u)",
             gpio_num,
             symmetric ? "simetrico(UP_DOWN)" : "assimetrico(UP)",
             (unsigned)s_actual_pwm_hz,
             (unsigned)s_period_ticks,
             (unsigned)s_sample_rate_hz,
             (unsigned)s_isr_subdiv);
    return ESP_OK;
}

esp_err_t pwm_driver_init(int gpio_num,
                          uint32_t pwm_freq_hz,
                          uint32_t sample_rate_hz,
                          bool symmetric,
                          bool invert_output,
                          uint16_t idle_cmp,
                          TaskHandle_t notify_task)
{
    if (notify_task == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_notify_task = notify_task;
    s_samples_played = 0U;
    s_underrun_count = 0U;

    esp_err_t err = apply_mcpwm_hardware_config(gpio_num, pwm_freq_hz, sample_rate_hz,
                                                symmetric, invert_output, idle_cmp);
    if (err == ESP_OK) {
        s_initialized = true;
    }
    return err;
}

esp_err_t pwm_driver_reconfigure(int gpio_num,
                                 uint32_t pwm_freq_hz,
                                 uint32_t sample_rate_hz,
                                 bool symmetric,
                                 bool invert_output,
                                 uint16_t idle_cmp)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return apply_mcpwm_hardware_config(gpio_num, pwm_freq_hz, sample_rate_hz,
                                       symmetric, invert_output, idle_cmp);
}

size_t pwm_driver_push_samples(const uint16_t *samples, size_t count)
{
    if (!samples || count == 0U) {
        return 0U;
    }

    uint32_t head = s_head;
    uint32_t tail = s_tail;
    uint32_t used = head - tail;
    uint32_t free_slots = (used < PWM_DRIVER_RINGBUF_LEN) ? (PWM_DRIVER_RINGBUF_LEN - used) : 0U;

    size_t to_write = (count < free_slots) ? count : free_slots;
    for (size_t i = 0; i < to_write; i++) {
        s_ringbuf[(head + i) & PWM_RINGBUF_MASK] = samples[i];
    }

    __sync_synchronize();
    s_head = head + (uint32_t)to_write;
    return to_write;
}

size_t pwm_driver_free_space(void)
{
    uint32_t used = s_head - s_tail;
    return (used < PWM_DRIVER_RINGBUF_LEN) ? (size_t)(PWM_DRIVER_RINGBUF_LEN - used) : 0U;
}

void pwm_driver_set_idle_cmp(uint16_t idle_cmp)
{
    if (idle_cmp < 1U) {
        idle_cmp = 1U;
    } else if (idle_cmp >= s_period_ticks) {
        idle_cmp = (uint16_t)(s_period_ticks - 1U);
    }
    s_idle_cmp = idle_cmp;
}

void pwm_driver_get_stats(pwm_driver_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    uint32_t head = s_head;
    uint32_t tail = s_tail;
    out_stats->period_ticks     = s_period_ticks;
    out_stats->actual_pwm_hz    = s_actual_pwm_hz;
    out_stats->sample_rate_hz   = s_sample_rate_hz;
    out_stats->isr_subdiv       = s_isr_subdiv;
    out_stats->samples_played   = s_samples_played;
    out_stats->underrun_count   = s_underrun_count;
    out_stats->buffered_samples = (head >= tail) ? (head - tail) : 0U;
}
