#include "uart_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pwm_driver.h"
#include "qam_config.h"
#include "qam_framer.h"
#include "qam_mapper.h"
#include "qam_selftest.h"
#include "tx_engine.h"
#include "tx_telem.h"

static const char *TAG = "uart_cli";

#define CLI_UART_PORT    CONFIG_ESP_CONSOLE_UART_NUM
#define CLI_LINE_MAX     (QAM_MAX_PAYLOAD_BYTES + 16U)

static bool s_verbose_tx = true;

static void print_help(void)
{
    printf("\n=== COMANDOS DO TRANSMISSOR 4-QAM ===\n");
    printf("  <texto>                         Envia texto ASCII no modo ativo\n");
    printf("  /send <texto>                   Envia texto mesmo que comece com '/'\n");
    printf("  /status                         Mostra configuracao completa e estatisticas\n");
    printf("  /mode <1|2|3>                   1=Professor | 2=Quadro+CRC | 3=Continuo\n");
    printf("  /fc <hz>                        Frequencia da portadora (ex: 1000)\n");
    printf("  /baud <baud>                    Taxa de simbolos (ex: 100)\n");
    printf("  /fs <hz>                        Taxa de amostras (divisor de f_pwm, ex: 40000)\n");
    printf("  /pwm <hz> [sym|asym]            Frequencia e modo PWM (ex: /pwm 80000 sym)\n");
    printf("  /amp <mv>                       Amplitude de pico Vpk em mV (ex: 900)\n");
    printf("  /offset <mv>                    Offset DC em mV (ex: 1300)\n");
    printf("  /order <msb|lsb_bit|lsb_dibit>  Ordem dos bits por byte ASCII\n");
    printf("  /map <natural|gray>             Tabela de constelacao 4-QAM\n");
    printf("  /qsign <plus|minus>             plus: +cos(wt-90) [+sin] | minus: -sin\n");
    printf("  /pol <norm|inv>                 Polaridade do sinal 4-QAM\n");
    printf("  /pulse <rect|taper>             Pulso retangular ou borda suavizada\n");
    printf("  /idle <dc|pilot>                Saida ociosa: silencio DC ou piloto 00\n");
    printf("  /pre1 <0|1> [0|1]               Modo 1: preambulo e sync opcionais\n");
    printf("  /gpio <pino>                    Pino GPIO de saida PWM\n");
    printf("  /test <carrier|sym00|sym01|sym11|sym10|cycle|text <msg>|stop>\n");
    printf("  /verbose <0|1>                  Detalhar ou resumir simbolos por byte\n");
    printf("  /vectors                        Exibe vetores de referencia ('A', 'B', tabela)\n");
    printf("  /selftest                       Executa auto-teste matematico em software\n");
    printf("  /id                             Identifica firmware e emite pacote HELLO\n");
    printf("  /telem <0|1>                    Ativa/desativa telemetria binaria 4QAM-TLM v1\n");
    printf("  /cap                            Captura 512 amostras PWM programadas (@ 40kHz)\n");
    printf("  /reset                          Restaura configuracao padrao de fabrica\n");
    printf("  /help                           Exibe esta ajuda\n\n");
}

static void print_status(void)
{
    qam_tx_config_t cfg;
    pwm_driver_stats_t pwm;
    tx_engine_stats_t eng;

    tx_engine_get_config(&cfg);
    pwm_driver_get_stats(&pwm);
    tx_engine_get_stats(&eng);

    float a_eq = (float)cfg.amplitude_peak_mv / 1.41421356f;
    uint32_t vmin = (uint32_t)cfg.dc_offset_mv - (uint32_t)cfg.amplitude_peak_mv;
    uint32_t vmax = (uint32_t)cfg.dc_offset_mv + (uint32_t)cfg.amplitude_peak_mv;

    printf("\n=== STATUS DO TRANSMISSOR 4-QAM ===\n");
    printf("  Modo ativo       : %s\n", qam_config_mode_name(cfg.tx_mode));
    if (cfg.tx_mode == QAM_TX_MODE_3_CONTINUOUS) {
        printf("  Padrao Modo 3    : %s (simbolo=%u, texto=\"%s\")\n",
               qam_config_cont_pattern_name(cfg.cont_pattern),
               (unsigned)cfg.cont_fixed_symbol,
               cfg.cont_text);
    }
    printf("  Portadora (fc)   : %u Hz\n", (unsigned)cfg.carrier_freq_hz);
    printf("  Taxa simbolos    : %u baud (%u bps brutos, Ts=%.2f ms)\n",
           (unsigned)cfg.symbol_rate_baud,
           (unsigned)(cfg.symbol_rate_baud * cfg.bits_per_symbol),
           1000.0 / (double)cfg.symbol_rate_baud);
    printf("  Amostragem (fs)  : %u Hz (%.1f amostras/ciclo fc, %.1f amostras/simbolo)\n",
           (unsigned)cfg.sample_rate_hz,
           (double)cfg.sample_rate_hz / (double)cfg.carrier_freq_hz,
           (double)cfg.sample_rate_hz / (double)cfg.symbol_rate_baud);
    printf("  PWM (MCPWM0A)    : GPIO %d | %u Hz real | %s | topo=%u ticks | subdiv=%u\n",
           cfg.pwm_gpio,
           (unsigned)pwm.actual_pwm_hz,
           cfg.pwm_symmetric ? "simetrico" : "assimetrico",
           (unsigned)pwm.period_ticks,
           (unsigned)pwm.isr_subdiv);
    printf("  Escala eletrica  : DC=%u mV | Vpk=%u mV (A=%.1f mV) | Faixa=[%u .. %u] mV\n",
           (unsigned)cfg.dc_offset_mv,
           (unsigned)cfg.amplitude_peak_mv,
           a_eq,
           (unsigned)vmin,
           (unsigned)vmax);
    printf("  Equacao 4-QAM    : s(t) = A * [ I*cos(wt) %s ]\n",
           cfg.q_sign_minus ? "- Q*sin(wt)" : "+ Q*cos(wt - 90 deg) (+Q*sin)");
    printf("  Constelacao      : %s\n", qam_config_map_name(cfg.map_preset));
    printf("  Ordem / Pulso    : %s | %s | polaridade=%s | idle=%s\n",
           qam_config_bit_order_name(cfg.bit_order),
           (cfg.pulse_shape == QAM_PULSE_RECTANGULAR) ? "retangular" : "taper-10%",
           cfg.invert_polarity ? "invertida(-)" : "normal(+)",
           (cfg.idle_mode == QAM_IDLE_DC_SILENCE) ? "DC" : "piloto-00");
    printf("  Modo 1 flags     : preambulo=%s | sync_word=%s\n",
           cfg.mode1_use_preamble ? "ON" : "OFF",
           cfg.mode1_use_sync_word ? "ON" : "OFF");
    printf("  Estatisticas     : quadros=%u | simbolos=%u | amostras=%u | underruns=%u | fila_drop=%u\n\n",
           (unsigned)eng.frames_sent,
           (unsigned)eng.symbols_sent,
           (unsigned)pwm.samples_played,
           (unsigned)pwm.underrun_count,
           (unsigned)eng.queue_dropped);
}

static void print_reference_vectors(void)
{
    qam_tx_config_t cfg;
    tx_engine_get_config(&cfg);

    printf("\n=== TABELA 4-QAM ATIVA (%s) ===\n", qam_config_map_name(cfg.map_preset));
    printf("Equacao: s(t) = A * [ I*cos(2*pi*fc*t) %s ]\n",
           cfg.q_sign_minus ? "- Q*sin(2*pi*fc*t)" : "+ Q*cos(2*pi*fc*t - 90 deg)");
    for (uint8_t s = 0U; s < 4U; s++) {
        const qam_symbol_point_t *pt = &cfg.constellation[s];
        printf("  Simbolo %u (%s) -> I=%+d, Q=%+d (plano IQ=%3u deg)\n",
               (unsigned)s, pt->bits_label, (int)pt->i_val, (int)pt->q_val, (unsigned)pt->phase_deg);
    }

    printf("\n=== VETORES DE REFERENCIA ASCII (%s) ===\n", qam_config_bit_order_name(cfg.bit_order));
    const uint8_t sample_bytes[2] = { 'A', 'B' };
    for (size_t b = 0; b < 2U; b++) {
        uint8_t syms[4];
        qam_mapper_byte_to_symbols(sample_bytes[b], cfg.bit_order, syms);
        printf("  '%c' (0x%02X): ", (char)sample_bytes[b], (unsigned)sample_bytes[b]);
        for (size_t k = 0; k < 4U; k++) {
            const qam_symbol_point_t *pt = &cfg.constellation[syms[k]];
            printf("[%s: I=%+d Q=%+d] ", pt->bits_label, (int)pt->i_val, (int)pt->q_val);
        }
        printf("\n");
    }
    printf("\n");
}

static void print_tx_breakdown(const qam_tx_config_t *cfg, const uint8_t *data, size_t len, const qam_tx_frame_t *frame)
{
    printf("TX [%s] \"%.*s\" (%u bytes -> %u simbolos total",
           qam_config_mode_name(cfg->tx_mode),
           (int)len, (const char *)data,
           (unsigned)len,
           (unsigned)frame->symbol_count);
    if (cfg->tx_mode == QAM_TX_MODE_2_FRAMED) {
        printf(", hdr=%u sym, payload=%u sym, CRC16=0x%04X",
               (unsigned)frame->header_symbol_count,
               (unsigned)frame->payload_symbol_count,
               (unsigned)frame->crc16);
    }
    printf(")\n");

    if (!s_verbose_tx || len > 16U) {
        return;
    }

    for (size_t b = 0; b < len; b++) {
        uint8_t syms[4];
        qam_mapper_byte_to_symbols(data[b], cfg->bit_order, syms);
        char printable = (data[b] >= 32U && data[b] <= 126U) ? (char)data[b] : '.';
        printf("  '%c' (0x%02X) ->", printable, (unsigned)data[b]);
        for (size_t s = 0; s < 4U; s++) {
            const qam_symbol_point_t *pt = &cfg->constellation[syms[s]];
            printf(" [%s:%+d,%+d %udeg]", pt->bits_label, (int)pt->i_val, (int)pt->q_val, (unsigned)pt->phase_deg);
        }
        printf("\n");
    }
}

static void transmit_text_line(const char *text, size_t len)
{
    if (len == 0U) {
        return;
    }
    if (len > QAM_MAX_PAYLOAD_BYTES) {
        printf("Erro: mensagem excede o limite de %u bytes.\n", QAM_MAX_PAYLOAD_BYTES);
        return;
    }

    qam_tx_config_t cfg;
    tx_engine_get_config(&cfg);

    qam_tx_frame_t *frame_info = (qam_tx_frame_t *)pvPortMalloc(sizeof(qam_tx_frame_t));
    esp_err_t err = tx_engine_send_message((const uint8_t *)text, len, frame_info);
    if (err == ESP_OK && frame_info != NULL) {
        print_tx_breakdown(&cfg, (const uint8_t *)text, len, frame_info);
    } else if (err == ESP_OK) {
        printf("TX enfileirado (%u bytes)\n", (unsigned)len);
    } else {
        printf("Erro ao transmitir: %s\n", esp_err_to_name(err));
    }

    if (frame_info != NULL) {
        vPortFree(frame_info);
    }
}

static void apply_and_report(const qam_tx_config_t *new_cfg, const char *ok_msg)
{
    char err_buf[128] = { 0 };
    esp_err_t err = tx_engine_apply_config(new_cfg, err_buf, sizeof(err_buf));
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

static void cmd_help(qam_tx_config_t *cfg, char *args)       { (void)cfg; (void)args; print_help(); }
static void cmd_status(qam_tx_config_t *cfg, char *args)     { (void)cfg; (void)args; print_status(); }
static void cmd_vectors(qam_tx_config_t *cfg, char *args)    { (void)cfg; (void)args; print_reference_vectors(); }
static void cmd_selftest(qam_tx_config_t *cfg, char *args)   { (void)cfg; (void)args; (void)qam_selftest_run_all(true); }

static void cmd_id(qam_tx_config_t *cfg, char *args)
{
    (void)cfg;
    (void)args;
    printf("ID: ROLE=TX PROTOCOL=4QAM-TLM-v1 ESP_IDF=v4.4.8\n");
    tx_telem_send_hello_now();
}

static void cmd_telem(qam_tx_config_t *cfg, char *args)
{
    (void)cfg;
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || v > 1U) {
        printf("Uso: /telem <0|1>\n");
        return;
    }
    tx_telem_set_enabled(v == 1U);
    printf("OK: telem=%u (4QAM-TLM v1)\n", (unsigned)v);
}

static void cmd_cap(qam_tx_config_t *cfg, char *args)
{
    (void)cfg;
    (void)args;
    tx_telem_request_pwm_capture();
    printf("OK: captura PWM programada solicitada (512 amostras).\n");
}

static void cmd_reset(qam_tx_config_t *cfg, char *args)
{
    (void)args;
    qam_config_set_defaults(cfg);
    apply_and_report(cfg, "Configuracao padrao restaurada.");
}

static void cmd_mode(qam_tx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *tok = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(tok, &v) || v < 1U || v > 3U) {
        printf("Uso: /mode <1|2|3>\n");
        return;
    }
    cfg->tx_mode = (qam_tx_mode_t)v;
    apply_and_report(cfg, qam_config_mode_name(cfg->tx_mode));
}

static void cmd_u32_field(qam_tx_config_t *cfg, char *args, uint32_t *target, const char *usage, const char *ok_msg)
{
    uint32_t v = 0U;
    char *tok = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(tok, &v)) {
        printf("Uso: %s\n", usage);
        return;
    }
    *target = v;
    apply_and_report(cfg, ok_msg);
}

static void cmd_fc(qam_tx_config_t *cfg, char *args)   { cmd_u32_field(cfg, args, &cfg->carrier_freq_hz, "/fc <hz>", "Portadora atualizada."); }
static void cmd_baud(qam_tx_config_t *cfg, char *args) { cmd_u32_field(cfg, args, &cfg->symbol_rate_baud, "/baud <baud>", "Taxa de simbolos atualizada."); }
static void cmd_fs(qam_tx_config_t *cfg, char *args)   { cmd_u32_field(cfg, args, &cfg->sample_rate_hz, "/fs <hz>", "Taxa de amostragem atualizada."); }

static void cmd_pwm(qam_tx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    char *b = strtok_r(NULL, " \t", &args);
    if (!parse_u32_arg(a, &v)) {
        printf("Uso: /pwm <freq_hz> [sym|asym]\n");
        return;
    }
    cfg->pwm_freq_hz = v;
    if (b != NULL) {
        if (strcmp(b, "sym") == 0) cfg->pwm_symmetric = true;
        else if (strcmp(b, "asym") == 0) cfg->pwm_symmetric = false;
    }
    apply_and_report(cfg, "Configuracao PWM atualizada.");
}

static void cmd_amp(qam_tx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || v > 3300U) {
        printf("Uso: /amp <mv>\n");
        return;
    }
    cfg->amplitude_peak_mv = (uint16_t)v;
    apply_and_report(cfg, "Amplitude de pico Vpk atualizada.");
}

static void cmd_offset(qam_tx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || v > 3300U) {
        printf("Uso: /offset <mv>\n");
        return;
    }
    cfg->dc_offset_mv = (uint16_t)v;
    apply_and_report(cfg, "Offset DC atualizado.");
}

static void cmd_order(qam_tx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "msb") == 0) cfg->bit_order = QAM_BIT_ORDER_MSB_FIRST;
    else if (a && strcmp(a, "lsb_bit") == 0) cfg->bit_order = QAM_BIT_ORDER_LSB_BIT_FIRST;
    else if (a && strcmp(a, "lsb_dibit") == 0) cfg->bit_order = QAM_BIT_ORDER_LSB_DIBIT_FIRST;
    else {
        printf("Uso: /order <msb|lsb_bit|lsb_dibit>\n");
        return;
    }
    apply_and_report(cfg, qam_config_bit_order_name(cfg->bit_order));
}

static void cmd_map(qam_tx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "natural") == 0) {
        qam_config_apply_map_preset(cfg, QAM_MAP_NATURAL);
    } else if (a && strcmp(a, "gray") == 0) {
        qam_config_apply_map_preset(cfg, QAM_MAP_GRAY);
    } else {
        printf("Uso: /map <natural|gray>\n");
        return;
    }
    apply_and_report(cfg, qam_config_map_name(cfg->map_preset));
}

static void cmd_qsign(qam_tx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "plus") == 0) {
        cfg->q_sign_minus = false;
    } else if (a && strcmp(a, "minus") == 0) {
        cfg->q_sign_minus = true;
    } else {
        printf("Uso: /qsign <plus|minus>\n");
        return;
    }
    apply_and_report(cfg, cfg->q_sign_minus ? "Equacao: I*cos - Q*sin" : "Equacao: I*cos + Q*cos(wt-90) [+Q*sin]");
}

static void cmd_pol(qam_tx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "norm") == 0) cfg->invert_polarity = false;
    else if (a && strcmp(a, "inv") == 0) cfg->invert_polarity = true;
    else {
        printf("Uso: /pol <norm|inv>\n");
        return;
    }
    apply_and_report(cfg, cfg->invert_polarity ? "Polaridade invertida (-)." : "Polaridade normal (+).");
}

static void cmd_pulse(qam_tx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "rect") == 0) cfg->pulse_shape = QAM_PULSE_RECTANGULAR;
    else if (a && strcmp(a, "taper") == 0) cfg->pulse_shape = QAM_PULSE_RAISED_EDGE_TAPER;
    else {
        printf("Uso: /pulse <rect|taper>\n");
        return;
    }
    apply_and_report(cfg, (cfg->pulse_shape == QAM_PULSE_RECTANGULAR) ? "Pulso retangular." : "Borda suavizada.");
}

static void cmd_idle(qam_tx_config_t *cfg, char *args)
{
    char *a = strtok_r(args, " \t", &args);
    if (a && strcmp(a, "dc") == 0) cfg->idle_mode = QAM_IDLE_DC_SILENCE;
    else if (a && strcmp(a, "pilot") == 0) cfg->idle_mode = QAM_IDLE_PILOT_SYMBOL_00;
    else {
        printf("Uso: /idle <dc|pilot>\n");
        return;
    }
    apply_and_report(cfg, (cfg->idle_mode == QAM_IDLE_DC_SILENCE) ? "Repouso DC." : "Repouso piloto 00.");
}

static void cmd_pre1(qam_tx_config_t *cfg, char *args)
{
    uint32_t v1 = 0U, v2 = 0U;
    char *a = strtok_r(args, " \t", &args);
    char *b = strtok_r(NULL, " \t", &args);
    if (!parse_u32_arg(a, &v1) || v1 > 1U) {
        printf("Uso: /pre1 <0|1> [0|1]\n");
        return;
    }
    cfg->mode1_use_preamble = (v1 == 1U);
    if (b != NULL && parse_u32_arg(b, &v2) && v2 <= 1U) {
        cfg->mode1_use_sync_word = (v2 == 1U);
    }
    apply_and_report(cfg, "Flags opcionais do Modo 1 atualizadas.");
}

static void cmd_gpio(qam_tx_config_t *cfg, char *args)
{
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v)) {
        printf("Uso: /gpio <pino>\n");
        return;
    }
    cfg->pwm_gpio = (int)v;
    apply_and_report(cfg, "Pino GPIO PWM atualizado.");
}

static void cmd_verbose(qam_tx_config_t *cfg, char *args)
{
    (void)cfg;
    uint32_t v = 0U;
    char *a = strtok_r(args, " \t", &args);
    if (!parse_u32_arg(a, &v) || v > 1U) {
        printf("Uso: /verbose <0|1>\n");
        return;
    }
    s_verbose_tx = (v == 1U);
    printf("OK: verbose=%u\n", (unsigned)s_verbose_tx);
}

static void cmd_test(qam_tx_config_t *cfg, char *args)
{
    while (args && (*args == ' ' || *args == '\t')) {
        args++;
    }
    if (!args || *args == '\0') {
        printf("Uso: /test <carrier|sym00|sym01|sym11|sym10|cycle|text <msg>|stop>\n");
        return;
    }
    if (strcmp(args, "stop") == 0) {
        cfg->tx_mode = QAM_TX_MODE_1_PROFESSOR;
        apply_and_report(cfg, "Teste continuo parado; retornou ao Modo 1.");
        return;
    }

    static const struct { const char *name; qam_cont_pattern_t pat; uint8_t sym; const char *msg; } tbl[] = {
        { "carrier", QAM_CONT_PATTERN_RAW_CARRIER,  0U, "Modo 3: Portadora pura ativa." },
        { "sym00",   QAM_CONT_PATTERN_FIXED_SYMBOL, 0U, "Modo 3: Simbolo fixo 0 (00) ativo." },
        { "sym01",   QAM_CONT_PATTERN_FIXED_SYMBOL, 1U, "Modo 3: Simbolo fixo 1 (01) ativo." },
        { "sym10",   QAM_CONT_PATTERN_FIXED_SYMBOL, 2U, "Modo 3: Simbolo fixo 2 (10) ativo." },
        { "sym11",   QAM_CONT_PATTERN_FIXED_SYMBOL, 3U, "Modo 3: Simbolo fixo 3 (11) ativo." },
        { "cycle",   QAM_CONT_PATTERN_CYCLE_4,      0U, "Modo 3: Ciclo 00->01->11->10 ativo." },
    };

    cfg->tx_mode = QAM_TX_MODE_3_CONTINUOUS;
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (strcmp(args, tbl[i].name) == 0) {
            cfg->cont_pattern = tbl[i].pat;
            cfg->cont_fixed_symbol = tbl[i].sym;
            apply_and_report(cfg, tbl[i].msg);
            return;
        }
    }

    if (strncmp(args, "text ", 5) == 0 && args[5] != '\0') {
        cfg->cont_pattern = QAM_CONT_PATTERN_TEXT;
        snprintf(cfg->cont_text, sizeof(cfg->cont_text), "%s", &args[5]);
        apply_and_report(cfg, "Modo 3: Texto continuo atualizado.");
        return;
    }
    printf("Subcomando /test invalido.\n");
}

typedef void (*cli_cmd_fn_t)(qam_tx_config_t *cfg, char *args);

static void handle_slash_command(char *line)
{
    if (strncmp(line, "/send ", 6) == 0) {
        transmit_text_line(&line[6], strlen(&line[6]));
        return;
    }

    char *args = NULL;
    char *cmd = strtok_r(line, " \t", &args);
    if (!cmd) {
        return;
    }

    static const struct { const char *name; cli_cmd_fn_t fn; } cmds[] = {
        { "/help",     cmd_help },
        { "/status",   cmd_status },
        { "/vectors",  cmd_vectors },
        { "/selftest", cmd_selftest },
        { "/id",       cmd_id },
        { "/telem",    cmd_telem },
        { "/cap",      cmd_cap },
        { "/reset",    cmd_reset },
        { "/mode",     cmd_mode },
        { "/fc",       cmd_fc },
        { "/baud",     cmd_baud },
        { "/fs",       cmd_fs },
        { "/pwm",      cmd_pwm },
        { "/amp",      cmd_amp },
        { "/offset",   cmd_offset },
        { "/order",    cmd_order },
        { "/map",      cmd_map },
        { "/qsign",    cmd_qsign },
        { "/pol",      cmd_pol },
        { "/pulse",    cmd_pulse },
        { "/idle",     cmd_idle },
        { "/pre1",     cmd_pre1 },
        { "/gpio",     cmd_gpio },
        { "/verbose",  cmd_verbose },
        { "/test",     cmd_test },
    };

    qam_tx_config_t cfg;
    tx_engine_get_config(&cfg);

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (strcmp(cmd, cmds[i].name) == 0) {
            cmds[i].fn(&cfg, args);
            return;
        }
    }
    printf("Comando desconhecido '%s'. Digite /help.\n", cmd);
}

static void uart_cli_task(void *arg)
{
    (void)arg;
    char line[CLI_LINE_MAX];
    size_t line_len = 0U;
    bool last_was_cr = false;

    print_help();
    print_status();
    printf("4QAM-TX> ");
    fflush(stdout);

    while (1) {
        uint8_t ch = 0U;
        if (uart_read_bytes(CLI_UART_PORT, &ch, 1, pdMS_TO_TICKS(100)) <= 0) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (ch == '\n' && last_was_cr) {
                last_was_cr = false;
                continue;
            }
            last_was_cr = (ch == '\r');
            tx_telem_console_lock();
            printf("\n");
            if (line_len > 0U) {
                line[line_len] = '\0';
                if (line[0] == '/') {
                    handle_slash_command(line);
                } else {
                    transmit_text_line(line, line_len);
                }
                line_len = 0U;
            }
            printf("4QAM-TX> ");
            fflush(stdout);
            tx_telem_console_unlock();
            continue;
        }

        last_was_cr = false;
        if (ch == '\b' || ch == 0x7FU) {
            if (line_len > 0U) {
                line_len--;
                tx_telem_console_lock();
                printf("\b \b");
                fflush(stdout);
                tx_telem_console_unlock();
            }
        } else if (ch >= 0x20U && ch <= 0x7EU && line_len < (sizeof(line) - 1U)) {
            line[line_len++] = (char)ch;
            tx_telem_console_lock();
            putchar((int)ch);
            fflush(stdout);
            tx_telem_console_unlock();
        }
    }
}

esp_err_t uart_cli_start(void)
{
    fflush(stdout);
    fsync(fileno(stdout));

    esp_vfs_dev_uart_port_set_rx_line_endings(CLI_UART_PORT, ESP_LINE_ENDINGS_CR);
    esp_vfs_dev_uart_port_set_tx_line_endings(CLI_UART_PORT, ESP_LINE_ENDINGS_CRLF);

    esp_err_t err = uart_driver_install(CLI_UART_PORT, 1024, 2048, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao instalar driver UART%d: %s", CLI_UART_PORT, esp_err_to_name(err));
        return err;
    }
    esp_vfs_dev_uart_use_driver(CLI_UART_PORT);

    BaseType_t ok = xTaskCreatePinnedToCore(uart_cli_task, "uart_cli_task", 4096, NULL, 4, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task uart_cli_task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
