#include "rx_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "adc_sampler.h"
#include "rx_config.h"
#include "rx_engine.h"
#include "rx_selftest.h"
#include "rx_telem.h"

static const char *TAG = "rx_cli";

#define RX_CLI_UART_PORT   CONFIG_ESP_CONSOLE_UART_NUM
#define RX_CLI_LINE_MAX    128U

static void print_help(void)
{
    printf("\n=== COMANDOS DO RECEPTOR 4-QAM ===\n");
    printf("  /status                         Mostra configuracao, ADC1 DMA e metricas\n");
    printf("  /mode <1|2>                     1=Modo Professor (direto) | 2=Quadro+CRC\n");
    printf("  /fc <hz>                        Frequencia esperada da portadora (ex: 1000)\n");
    printf("  /baud <baud>                    Taxa nominal de simbolos (ex: 100)\n");
    printf("  /fs <hz>                        Taxa de amostragem ADC1 DMA (20000..80000)\n");
    printf("  /ch <0..7>                      Canal ADC1 (ch 6 = GPIO 34)\n");
    printf("  /map <natural|gray>             Tabela 4-QAM esperada\n");
    printf("  /qsign <plus|minus>             plus: +cos(wt-90) [+sin] | minus: -sin\n");
    printf("  /order <msb|lsb_bit|lsb_dibit>  Ordem dos bits no Modo 1\n");
    printf("  /pol <norm|inv>                 Polaridade do sinal de entrada\n");
    printf("  /rot <0|90|180|270>             Rotacao manual de fase (Modo 1 sem sync)\n");
    printf("  /sync1 <0|1>                    Exigir sync word no Modo 1\n");
    printf("  /squelch <mv>                   Limiar RMS AC de deteccao de portadora\n");
    printf("  /monitor <0|1>                  Liga/desliga telemetria I/Q ao vivo\n");
    printf("  /selftest                       Executa auto-teste em malha fechada\n");
    printf("  /id                             Identifica firmware e emite pacote HELLO\n");
    printf("  /telem <0|1>                    Ativa/desativa telemetria binaria 4QAM-TLM v1\n");
    printf("  /cap                            Captura 2048 amostras ADC em taxa cheia (20kHz)\n");
    printf("  /adcstream <0|5..50>            Decimacao do fluxo ADC ao vivo (0=off, padrao=10)\n");
    printf("  /reset                          Restaura configuracao padrao\n");
    printf("  /help                           Exibe esta ajuda\n\n");
}

static void print_status(void)
{
    qam_rx_config_t cfg;
    adc_sampler_stats_t adc;
    rx_engine_stats_t eng;

    rx_engine_get_config(&cfg);
    adc_sampler_get_stats(&adc);
    rx_engine_get_stats(&eng);

    printf("\n=== STATUS DO RECEPTOR 4-QAM ===\n");
    printf("  Modo ativo       : %s\n", rx_config_mode_name(cfg.rx_mode));
    printf("  Entrada ADC1     : Canal %d (GPIO %d) | fs=%u Hz | eFuse cal=%s\n",
           adc.adc1_channel, adc.gpio_num, (unsigned)adc.sample_rate_hz,
           adc.efuse_cal_active ? "ON" : "OFF");
    printf("  Portadora / Baud : fc=%u Hz | Rs=%u baud | squelch=%.1f mV RMS\n",
           (unsigned)cfg.carrier_freq_hz, (unsigned)cfg.symbol_rate_baud, cfg.squelch_rms_mv);
    printf("  Equacao / Tabela : %s | %s\n",
           cfg.q_sign_minus ? "I*cos - Q*sin" : "I*cos + Q*cos(wt-90) [+Q*sin]",
           rx_config_map_name(cfg.map_preset));
    printf("  Ordem / Polarid. : %s | pol=%s | rot_m1=%udeg | sync_m1=%s\n",
           rx_config_bit_order_name(cfg.bit_order),
           cfg.invert_polarity ? "invertida(-)" : "normal(+)",
           (unsigned)((cfg.fixed_rotation_quad & 0x03U) * 90U),
           cfg.mode1_use_sync_word ? "ON" : "OFF");
    printf("  Sinal ao vivo    : ativo=%s | DC=%.0f mV | Vpk=%.0f mV | df=%+.2f Hz | I/Q=(%+.2f,%+.2f)\n",
           eng.signal_active ? "SIM" : "NAO",
           eng.dc_est_mv, eng.amp_pk_est_mv, eng.freq_offset_hz,
           eng.last_i_hat, eng.last_q_hat);
    printf("  Estatisticas     : quadros_ok=%u | crc_err=%u | bytes_m1=%u | rajadas=%u | simb=%u\n",
           (unsigned)eng.frames_ok, (unsigned)eng.frames_crc_err,
           (unsigned)eng.bytes_raw_rx, (unsigned)eng.bursts_detected,
           (unsigned)eng.symbols_demod);
    printf("  DMA ADC1         : amostras=%u | invalidas=%u | overflows=%u | monitor=%s\n\n",
           (unsigned)adc.samples_read, (unsigned)adc.invalid_samples,
           (unsigned)adc.overflow_count, rx_engine_get_monitor() ? "ON" : "OFF");
}

static void apply_and_report(const qam_rx_config_t *new_cfg, const char *ok_msg)
{
    char err_buf[128] = { 0 };
    esp_err_t err = rx_engine_apply_config(new_cfg, err_buf, sizeof(err_buf));
    if (err == ESP_OK) {
        printf("OK: %s\n", ok_msg);
    } else {
        printf("Erro de configuracao: %s\n", (err_buf[0] != '\0') ? err_buf : esp_err_to_name(err));
    }
}

static bool parse_u32_arg(const char *str, uint32_t *out_val)
{
    if (!str || *str == '\0') {
        return false;
    }
    char *endptr = NULL;
    unsigned long val = strtoul(str, &endptr, 10);
    if (endptr == str || *endptr != '\0') {
        return false;
    }
    *out_val = (uint32_t)val;
    return true;
}

static void cmd_help(qam_rx_config_t *cfg, char *args)     { (void)cfg; (void)args; print_help(); }
static void cmd_status(qam_rx_config_t *cfg, char *args)   { (void)cfg; (void)args; print_status(); }
static void cmd_selftest(qam_rx_config_t *cfg, char *args) { (void)cfg; (void)args; (void)rx_selftest_run_all(true); }

static void cmd_id(qam_rx_config_t *cfg, char *args)
{
    (void)cfg;
    (void)args;
    printf("ID: ROLE=RX PROTOCOL=4QAM-TLM-v1 ESP_IDF=v4.4.8\n");
    rx_telem_send_hello_now();
}

static void cmd_telem(qam_rx_config_t *cfg, char *args)
{
    (void)cfg;
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || v > 1U) {
        printf("Uso: /telem <0|1>\n");
        return;
    }
    rx_telem_set_enabled(v == 1U);
    printf("OK: telem=%u (4QAM-TLM v1)\n", (unsigned)v);
}

static void cmd_cap(qam_rx_config_t *cfg, char *args)
{
    (void)cfg;
    (void)args;
    rx_telem_request_adc_capture();
    printf("OK: captura ADC em taxa cheia solicitada (2048 amostras).\n");
}

static void cmd_adcstream(qam_rx_config_t *cfg, char *args)
{
    (void)cfg;
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || (v != 0U && (v < 5U || v > 50U))) {
        printf("Uso: /adcstream <0|5..50>\n");
        return;
    }
    rx_telem_set_adc_decim((uint16_t)v);
    printf("OK: adcstream decim=%u\n", (unsigned)v);
}

static void cmd_reset(qam_rx_config_t *cfg, char *args)
{
    (void)args;
    rx_config_set_defaults(cfg);
    apply_and_report(cfg, "Configuracao padrao do receptor restaurada.");
}

static void cmd_mode(qam_rx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || (v != 1U && v != 2U)) {
        printf("Uso: /mode <1|2>\n");
        return;
    }
    cfg->rx_mode = (rx_decode_mode_t)v;
    apply_and_report(cfg, rx_config_mode_name(cfg->rx_mode));
}

static void cmd_u32_field(qam_rx_config_t *cfg, char *args, uint32_t *target, const char *usage, const char *msg)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v)) {
        printf("Uso: %s\n", usage);
        return;
    }
    *target = v;
    apply_and_report(cfg, msg);
}

static void cmd_fc(qam_rx_config_t *cfg, char *args)   { cmd_u32_field(cfg, args, &cfg->carrier_freq_hz, "/fc <hz>", "Portadora atualizada."); }
static void cmd_baud(qam_rx_config_t *cfg, char *args) { cmd_u32_field(cfg, args, &cfg->symbol_rate_baud, "/baud <baud>", "Taxa de simbolos atualizada."); }
static void cmd_fs(qam_rx_config_t *cfg, char *args)   { cmd_u32_field(cfg, args, &cfg->sample_rate_hz, "/fs <hz>", "Taxa ADC atualizada."); }

static void cmd_ch(qam_rx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || v > 7U) {
        printf("Uso: /ch <0..7>\n");
        return;
    }
    cfg->adc1_channel = (int)v;
    apply_and_report(cfg, "Canal ADC1 atualizado.");
}

static void cmd_map(qam_rx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "natural") == 0) rx_config_apply_map_preset(cfg, RX_MAP_NATURAL);
    else if (a && strcmp(a, "gray") == 0) rx_config_apply_map_preset(cfg, RX_MAP_GRAY);
    else {
        printf("Uso: /map <natural|gray>\n");
        return;
    }
    apply_and_report(cfg, rx_config_map_name(cfg->map_preset));
}

static void cmd_qsign(qam_rx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "plus") == 0) cfg->q_sign_minus = false;
    else if (a && strcmp(a, "minus") == 0) cfg->q_sign_minus = true;
    else {
        printf("Uso: /qsign <plus|minus>\n");
        return;
    }
    apply_and_report(cfg, cfg->q_sign_minus ? "Equacao: I*cos - Q*sin" : "Equacao: I*cos + Q*cos(wt-90) [+Q*sin]");
}

static void cmd_order(qam_rx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "msb") == 0) cfg->bit_order = RX_BIT_ORDER_MSB_FIRST;
    else if (a && strcmp(a, "lsb_bit") == 0) cfg->bit_order = RX_BIT_ORDER_LSB_BIT_FIRST;
    else if (a && strcmp(a, "lsb_dibit") == 0) cfg->bit_order = RX_BIT_ORDER_LSB_DIBIT_FIRST;
    else {
        printf("Uso: /order <msb|lsb_bit|lsb_dibit>\n");
        return;
    }
    apply_and_report(cfg, rx_config_bit_order_name(cfg->bit_order));
}

static void cmd_pol(qam_rx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "norm") == 0) cfg->invert_polarity = false;
    else if (a && strcmp(a, "inv") == 0) cfg->invert_polarity = true;
    else {
        printf("Uso: /pol <norm|inv>\n");
        return;
    }
    apply_and_report(cfg, cfg->invert_polarity ? "Polaridade invertida." : "Polaridade normal.");
}

static void cmd_rot(qam_rx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || (v != 0U && v != 90U && v != 180U && v != 270U)) {
        printf("Uso: /rot <0|90|180|270>\n");
        return;
    }
    cfg->fixed_rotation_quad = (uint8_t)(v / 90U);
    apply_and_report(cfg, "Rotacao manual do Modo 1 atualizada.");
}

static void cmd_sync1(qam_rx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || v > 1U) {
        printf("Uso: /sync1 <0|1>\n");
        return;
    }
    cfg->mode1_use_sync_word = (v == 1U);
    apply_and_report(cfg, cfg->mode1_use_sync_word ? "Sync word ativada no Modo 1." : "Sync word desativada no Modo 1.");
}

static void cmd_squelch(qam_rx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v)) {
        printf("Uso: /squelch <mv>\n");
        return;
    }
    cfg->squelch_rms_mv = (float)v;
    apply_and_report(cfg, "Limiar de squelch atualizado.");
}

static void cmd_monitor(qam_rx_config_t *cfg, char *args)
{
    (void)cfg;
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || v > 1U) {
        printf("Uso: /monitor <0|1>\n");
        return;
    }
    rx_engine_set_monitor(v == 1U);
    printf("OK: monitor=%s\n", (v == 1U) ? "ON" : "OFF");
}

typedef void (*rx_cmd_fn_t)(qam_rx_config_t *cfg, char *args);

static void handle_line(char *line)
{
    char *args = NULL;
    char *cmd = strtok_r(line, " \t", &args);
    if (!cmd) {
        return;
    }

    static const struct { const char *name; rx_cmd_fn_t fn; } cmds[] = {
        { "/help",      cmd_help },
        { "/status",    cmd_status },
        { "/selftest",  cmd_selftest },
        { "/id",        cmd_id },
        { "/telem",     cmd_telem },
        { "/cap",       cmd_cap },
        { "/adcstream", cmd_adcstream },
        { "/reset",     cmd_reset },
        { "/mode",     cmd_mode },
        { "/fc",       cmd_fc },
        { "/baud",     cmd_baud },
        { "/fs",       cmd_fs },
        { "/ch",       cmd_ch },
        { "/map",      cmd_map },
        { "/qsign",    cmd_qsign },
        { "/order",    cmd_order },
        { "/pol",      cmd_pol },
        { "/rot",      cmd_rot },
        { "/sync1",    cmd_sync1 },
        { "/squelch",  cmd_squelch },
        { "/monitor",  cmd_monitor },
    };

    qam_rx_config_t cfg;
    rx_engine_get_config(&cfg);

    for (size_t i = 0U; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (strcmp(cmd, cmds[i].name) == 0) {
            cmds[i].fn(&cfg, args);
            return;
        }
    }
    printf("Comando desconhecido '%s'. Digite /help.\n", cmd);
}

static void rx_cli_task(void *arg)
{
    (void)arg;
    char line[RX_CLI_LINE_MAX];
    size_t line_len = 0U;
    bool last_was_cr = false;

    print_help();
    print_status();
    printf("4QAM-RX> ");
    fflush(stdout);

    while (1) {
        uint8_t ch = 0U;
        if (uart_read_bytes(RX_CLI_UART_PORT, &ch, 1, pdMS_TO_TICKS(100)) <= 0) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (ch == '\n' && last_was_cr) {
                last_was_cr = false;
                continue;
            }
            last_was_cr = (ch == '\r');
            rx_telem_console_lock();
            printf("\n");
            if (line_len > 0U) {
                line[line_len] = '\0';
                handle_line(line);
                line_len = 0U;
            }
            printf("4QAM-RX> ");
            fflush(stdout);
            rx_telem_console_unlock();
            continue;
        }

        last_was_cr = false;
        if (ch == '\b' || ch == 0x7FU) {
            if (line_len > 0U) {
                line_len--;
                rx_telem_console_lock();
                printf("\b \b");
                fflush(stdout);
                rx_telem_console_unlock();
            }
        } else if (ch >= 0x20U && ch <= 0x7EU && line_len < (sizeof(line) - 1U)) {
            line[line_len++] = (char)ch;
            rx_telem_console_lock();
            putchar((int)ch);
            fflush(stdout);
            rx_telem_console_unlock();
        }
    }
}

esp_err_t rx_cli_start(void)
{
    fflush(stdout);
    fsync(fileno(stdout));

    esp_vfs_dev_uart_port_set_rx_line_endings(RX_CLI_UART_PORT, ESP_LINE_ENDINGS_CR);
    esp_vfs_dev_uart_port_set_tx_line_endings(RX_CLI_UART_PORT, ESP_LINE_ENDINGS_CRLF);

    esp_err_t err = uart_driver_install(RX_CLI_UART_PORT, 1024, 2048, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao instalar driver UART%d: %s", RX_CLI_UART_PORT, esp_err_to_name(err));
        return err;
    }
    esp_vfs_dev_uart_use_driver(RX_CLI_UART_PORT);

    BaseType_t ok = xTaskCreatePinnedToCore(rx_cli_task, "rx_cli_task", 6144, NULL, 4, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task rx_cli_task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
