# Arquitetura do Sistema de Comunicação Digital 4-QAM em ESP32

Este documento descreve a arquitetura de hardware, software e particionamento em tempo real do sistema **4-QAM completo** implementado em dois projetos **ESP-IDF v4.4.8** independentes:

- **Transmissor (`transmitter/`):** [`transmitter.c`](../transmitter/main/transmitter.c) — síntese digital 4-QAM em banda passante e saída PWM de alta frequência (`80 kHz` simétrico via `MCPWM0`).
- **Canal / Estágio Analógico:** [`circuito_analogico.md`](./circuito_analogico.md) — filtro passa-baixas RC passivo de 2ª ordem (`GPIO 18 -> R1/C1/R2/C2 -> GPIO 34`).
- **Receptor (`receiver/`):** [`receiver.c`](../receiver/main/receiver.c) — aquisição contínua via `ADC1 DMA` (`20 kHz`), demodulação em quadratura, sincronização de portadora (`Costas Loop`) e símbolo (`Gardner TED`) e desenquadramento com resolução de ambiguidade de $90^\circ$ e CRC-16.

---

## 1. Visão Geral em Blocos

```mermaid
flowchart LR
    UART_TX["Terminal UART0 TX (115200 bps)"] --> CLI_TX["uart_cli (Core 0)"]
    CLI_TX --> FRAMER["qam_framer + qam_mapper"]
    FRAMER -->|"Fila FreeRTOS (4 quadros)"| DSP_TX["tx_dsp_task (Core 1)"]
    DSP_TX --> MOD["qam_modulator (LUT + NCO Racional)"]
    MOD -->|"Ring Buffer SPSC 1024"| ISR_TX["pwm_tez_isr IRAM (40 kHz)"]
    ISR_TX --> MCPWM["MCPWM0A GPIO 18 (80 kHz PWM)"]
    MCPWM --> LPF["Filtro RC 2a Ordem (1k/33nF + 10k/3.3nF)"]
    LPF -->|"0,40 V .. 2,20 V (DC=1,30 V)"| ADC_PIN["ADC1 Ch6 GPIO 34"]
    ADC_PIN --> DMA_RX["ADC1 DMA I2S0 (20 kHz)"]
    DMA_RX --> DSP_RX["rx_dsp_task (Core 1)"]
    DSP_RX --> DEMOD["qam_demod (DC + AGC + Costas + Gardner)"]
    DEMOD --> DEFRAMER["rx_deframer (Sync 4-Rot + CRC-16)"]
    DEFRAMER --> UART_RX["Terminal UART0 RX (115200 bps)"]
```

---

## 2. Alocação Dual-Core e Prioridades (FreeRTOS a 160 MHz)

Ambos os projetos operam com `CONFIG_IDF_TARGET="esp32"`, clock de CPU em `160 MHz` e `CONFIG_FREERTOS_HZ=100` (`tick = 10 ms`). O DSP de tempo crítico é isolado no **Core 1** para evitar preempção pelo driver UART/console do **Core 0**:

| Projeto | Tarefa / ISR | Núcleo | Pilha / Buffer | Prioridade | Função |
|---|---|---:|---:|---:|---|
| **TX** | `pwm_tez_isr` (`IRAM_ATTR`) | Core 1 (onde `pwm_driver_init` inicia) | Ring buffer DRAM `1024` | Hardware ISR | Consome amostra a `40 kHz` (subdivisão `2:1` do PWM `80 kHz`) e atualiza shadow compare no `TEZ` |
| **TX** | `tx_dsp_task` | Core 1 | `4096 B` | `configMAX_PRIORITIES - 2` | Gera amostras `128` por lote mantendo reserva `TX_DSP_TARGET_FILL = 768` (`~19 ms`) |
| **TX** | `uart_cli_task` | Core 0 | `4096 B` | Interativa UART | Lê comandos `/send`, `/test`, `/status`, reconfigura parâmetros em execução |
| **RX** | `ADC1 DMA (I2S0)` | Hardware DMA | `4096 B` (`chunk 256` amostras) | DMA contínuo | Captura `ADC1_CHANNEL_6` (`GPIO 34`) a `20.000 Hz` em `ADC_ATTEN_DB_12` + `esp_adc_cal` |
| **RX** | `rx_dsp_task` | Core 1 | `6144 B` | `configMAX_PRIORITIES - 3` | Lê chunks de `256` amostras (`12,8 ms`), executa `qam_demod` + `rx_deframer` |
| **RX** | `rx_cli_task` | Core 0 | Tarefa CLI RX | Interativa UART | Configura `/mode`, `/fc`, `/baud`, `/map`, `/qsign`, `/monitor`, `/selftest` |

---

## 3. Mapa de Pinos e Conexões Físicas

| Sinal | Pino ESP32 TX | Periférico TX | Estágio Analógico | Pino ESP32 RX | Periférico RX |
|---|---|---|---|---|---|
| **Saída 4-QAM PWM** | `GPIO 18` | `MCPWM_UNIT_0` (`MCPWM0A`) | Entrada do filtro RC (`R1 = 1,0 kΩ`) | — | — |
| **Saída Analógica Filtrada** | — | — | Nó `C2` (`R2 = 10 kΩ`, `C2 = 3,3 nF`) | `GPIO 34` | `ADC1_CHANNEL_6` (input-only) |
| **Referência Comum** | `GND` | Terra TX | Malha de retorno `C1/C2` | `GND` | Terra RX |
| **Console Serial CLI** | `UART0` (`TX/RX` USB) | `115200 bps` | Monitoramento PC | `UART0` (`TX/RX` USB) | `115200 bps` |

> **Segurança elétrica do pino `GPIO 34`:** `GPIO 34` é um pino **somente entrada** no ESP32 (sem driver de saída interno), o que protege o circuito contra conflito de barramento. A excursão nominal pós-filtro (`0,40 V` a `2,20 V`) mantém folga superior a `250 mV` nas extremidades da janela linear de `150 .. 2450 mV` do ADC1 com atenuação de 11/12 dB (`ADC_ATTEN_DB_12`).

---

## 4. Organização Modular dos Repositórios

### 4.1 Transmissor ([`transmitter/main/`](../transmitter/main/))
- [`qam_config.h`](../transmitter/main/qam_config.h) / [`qam_config.c`](../transmitter/main/qam_config.c) — estrutura centralizada `qam_tx_config_t` e validação de Nyquist/PWM/limites elétricos.
- [`qam_mapper.h`](../transmitter/main/qam_mapper.h) / [`qam_mapper.c`](../transmitter/main/qam_mapper.c) — conversão ASCII ↔ símbolos, presets Natural/Gray e `CRC-16/CCITT-FALSE`.
- [`qam_framer.h`](../transmitter/main/qam_framer.h) / [`qam_framer.c`](../transmitter/main/qam_framer.c) — montagem de quadros **Modo 1 (Professor)** e **Modo 2 (Quadro Próprio)**.
- [`qam_modulator.h`](../transmitter/main/qam_modulator.h) / [`qam_modulator.c`](../transmitter/main/qam_modulator.c) — LUT senoidal de `1024` pontos com interpolação linear, acumuladores racionais de fase/símbolo e quantização PWM.
- [`pwm_driver.h`](../transmitter/main/pwm_driver.h) / [`pwm_driver.c`](../transmitter/main/pwm_driver.c) — driver `MCPWM0` `160 MHz` simétrico a `80 kHz`, ISR `TEZ` em `IRAM` e ring buffer lock-free SPSC.
- [`tx_engine.h`](../transmitter/main/tx_engine.h) / [`tx_engine.c`](../transmitter/main/tx_engine.c) — motor de tempo real (`Core 1`), fila de quadros, padrões contínuos do **Modo 3** e reconfiguração dinâmica.
- [`qam_selftest.h`](../transmitter/main/qam_selftest.h) / [`qam_selftest.c`](../transmitter/main/qam_selftest.c) — auto-teste matemático embarcado no boot e via `/selftest`.
- [`uart_cli.h`](../transmitter/main/uart_cli.h) / [`uart_cli.c`](../transmitter/main/uart_cli.c) — CLI interativa completa com decomposição por caractere.

### 4.2 Receptor ([`receiver/main/`](../receiver/main/))
- [`rx_config.h`](../receiver/main/rx_config.h) / [`rx_config.c`](../receiver/main/rx_config.c) — parâmetros espelhados de recepção, ganhos das malhas e validação de entrada ADC1.
- [`rx_mapper.h`](../receiver/main/rx_mapper.h) / [`rx_mapper.c`](../receiver/main/rx_mapper.c) — decisor de quadrante com rotação $0^\circ/90^\circ/180^\circ/270^\circ$, reconstrução de bytes e `CRC-16`.
- [`qam_demod.h`](../receiver/main/qam_demod.h) / [`qam_demod.c`](../receiver/main/qam_demod.c) — removedor DC, squelch com histerese, AGC, misturador NCO, filtro boxcar ($2f_c = 0$), `Costas Loop` PI e `Gardner TED` + integrador de platô.
- [`rx_deframer.h`](../receiver/main/rx_deframer.h) / [`rx_deframer.c`](../receiver/main/rx_deframer.c) — busca deslizante da `Sync Word` nas 4 rotações possíveis, decodificação do cabeçalho Modo 2 e fluxo Modo 1.
- [`adc_sampler.h`](../receiver/main/adc_sampler.h) / [`adc_sampler.c`](../receiver/main/adc_sampler.c) — aquisição contínua `ADC1 DMA` (`I2S0`) a `20 kHz` com calibração `eFuse`.
- [`rx_engine.h`](../receiver/main/rx_engine.h) / [`rx_engine.c`](../receiver/main/rx_engine.c) — orquestração DSP no Core 1 e telemetria `/monitor 1`.
- [`rx_selftest.h`](../receiver/main/rx_selftest.h) / [`rx_selftest.c`](../receiver/main/rx_selftest.c) — auto-teste em malha fechada com deslocamentos de fase, portadora, baud e ruído.
- [`rx_cli.h`](../receiver/main/rx_cli.h) / [`rx_cli.c`](../receiver/main/rx_cli.c) — CLI interativa do receptor.

---

## 5. Mapa da Documentação

1. [`especificacao_4qam.md`](./especificacao_4qam.md) — Equação matemática exata, equivalência polar/retangular e formato de quadro.
2. [`mapeamento.md`](./mapeamento.md) — Tabelas Natural e Gray, ordens de bits e rotação de quadrante.
3. [`circuito_analogico.md`](./circuito_analogico.md) — Projeto do filtro RC de 2ª ordem, esquemático, proteção ADC e guia de osciloscópio.
4. [`transmissor.md`](./transmissor.md) — Detalhamento de hardware/software do TX e guia de comandos CLI.
5. [`receptor.md`](./receptor.md) — Detalhamento da cadeia ADC DMA, demodulador e comandos CLI do RX.
6. [`sincronizacao.md`](./sincronizacao.md) — Teoria e implementação de Boxcar, Costas Loop, Gardner TED, Preâmbulo e Sync Word.
7. [`interoperabilidade.md`](./interoperabilidade.md) — Matriz de compatibilidade para operação com receptor ou transmissor externo.
8. [`testes.md`](./testes.md) — Plano completo de validação (Testes 1 a 8) e critérios de aceitação.
9. [`vetores_teste/vetores_referencia.md`](./vetores_teste/vetores_referencia.md) — Vetores numéricos e [`vetores_amostras_4qam.csv`](./vetores_teste/vetores_amostras_4qam.csv) de referência.
10. [`protocolo_telemetria.md`](./protocolo_telemetria.md) — Protocolo binário versionado `4QAM-TLM v1` e separação Core 1 / Core 0.
11. [`../platform/README.md`](../platform/README.md) — Guia de operação da plataforma web de instrumentação.
