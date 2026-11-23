# Protocolo de Telemetria Híbrida 4QAM-TLM v1

Este documento especifica o protocolo binário versionado projetado para instrumentar os firmwares ESP32 (`transmitter/` e `receiver/`) mantendo a **CLI ASCII interativa em `UART0` (`115200 bps`)** totalmente funcional e isolando o processamento em tempo real (**Core 1**) da transmissão UART (**Core 0**).

---

## 1. Filosofia e Garantias de Tempo Real

- **Desabilitado na inicialização (`/telem 0` padrão):** ao ligar qualquer placa ESP32 em um monitor serial comum (`idf.py monitor`, PuTTY), o console permanece 100% em português ASCII limpo. Ao conectar pela plataforma web, o backend envia `/id` e `/telem 1` (e `/telem 0` ao desconectar).
- **Separação estrita de núcleos:**
  - `pwm_tez_isr`, `tx_dsp_task` e `rx_dsp_task` executam exclusivamente no **Core 1** e nunca chamam `printf()` nem `uart_write_bytes()`.
  - Eventos DSP, quadros e snapshots são colocados em fila FreeRTOS com timeout zero (`xQueueSend(..., 0)`). Se a fila lotar, incrementa `telem_dropped` sem bloquear a modulação ou demodulação.
  - Tarefas dedicadas de baixa prioridade (`tx_telem_task` / `rx_telem_task`, Core 0) protegem a `UART0` (`tx_buffer_size = 2048 B`) via mutex contra colisões com `printf()` da CLI.

---

## 2. Formato do Pacote Binário (`4QAM-TLM v1`)

| Campo | Offset | Tamanho | Codificação | Descrição |
|---|---:|---:|---|---|
| `SYNC` | `0` | `2 B` | `0xAA 0x55` | Sincronismo de quadro binário |
| `VER` | `2` | `1 B` | `0x01` | Versão do protocolo (`4QAM-TLM v1`) |
| `TYPE` | `3` | `1 B` | `uint8` | Código da mensagem (tabela abaixo) |
| `FLAGS` | `4` | `1 B` | bitmask | `bit 0`: `0=TX, 1=RX`; `bit 1`: snapshot taxa cheia; `bit 2`: `eFuse cal` ativa |
| `SEQ` | `5` | `2 B` | `uint16 LE` | Contador monotônico por placa para detecção de perdas/duplicações |
| `TS_US` | `7` | `4 B` | `uint32 LE` | Timestamp em microssegundos (`esp_timer_get_time()`) |
| `LEN` | `11` | `2 B` | `uint16 LE` | Comprimento do payload (`0 .. 512 B`) |
| `PAYLOAD` | `13` | `LEN B` | binário LE | Estrutura específica do `TYPE` |
| `CRC16` | `13+LEN` | `2 B` | `uint16 LE` | `CRC-16/CCITT-FALSE` (`0x1021`, init `0xFFFF`) cobrindo `VER .. PAYLOAD` |

---

## 3. Tipos de Mensagens

| `TYPE` | Nome | Emissor | Payload | Observações |
|---:|---|---|---|---|
| `0x01` | `TLM_HELLO_STATUS` | TX e RX | `52 B` (TX) / `60 B` (RX) | Estado completo, configuração ativa (`fc`, `baud`, `fs`, `map`, `qsign`, `mode`) e contadores de erros/drops |
| `0x02` | `TLM_TX_FRAME_EVENT` | TX | `28 B + payload_len` | `frame_id`, parâmetros ativos no momento do quadro, `crc16`, status (`queued/started/done/dropped`) e payload ASCII/binário |
| `0x03` | `TLM_TX_PWM_CHUNK` | TX | `14 B + N*2 B` | Captura de `512` valores `cmp_ticks` (`uint16`) programados no `MCPWM0` (`@ 40 kHz`) — **referência programada, não medição elétrica** |
| `0x10` | `TLM_RX_BURST_EVENT` | RX | `14 B` | Início (`0`) ou fim (`1`) de rajada no squelch, `burst_id`, `dc_est_mv` e `amp_pk_est_mv` |
| `0x11` | `TLM_RX_SYMBOL_BATCH` | RX | `6 B + K*26 B` (`K<=5`) | Por símbolo: `(i_hat, q_hat)`, `(i_mid, q_mid)`, `costas_ped`, `df_hz`, `nco_phase_rad`, `gardner_err`, `sym_rate_corr_ppm`, `snr_db`, `sym_phase`, `decision`, `rot_quad`, `state` |
| `0x12` | `TLM_RX_FRAME_EVENT` | RX | `24 B + payload_len` | Quadro decodificado (Modo 2 ou rajada Modo 1): `crc_ok`, `locked_rot`, `rx_crc`, `calc_crc`, `df_est`, `dc`, `amp`, `snr`, `payload` |
| `0x13` | `TLM_RX_ADC_CHUNK` | RX | `16 B + N*2 B` (`int16 mV`) | Modo A (`flags bit 1=1`, `decim=1`): snapshot automático de **`2.048` amostras a `20 kHz`** (`102,4 ms`). Modo B (`decim=10` padrão via `/adcstream`): fluxo contínuo decimado a `2 kSa/s` |

---

## 4. Orçamento de Banda Serial em `115200 bps`

- Taxa máxima teórica em `115200 bps 8N1`: `11.520 bytes/s`.
- A `100 baud`, o lote de símbolos DSP consome ~`2,78 kB/s` (`24%`), o fluxo contínuo decimado ADC (`10x` = `2 kSa/s`) consome ~`4,4 kB/s` (`38%`) e os *snapshots* completos de `2.048` amostras (`20 kHz`) são drenados de RAM pelo Core 0 após o disparo em `~0,4 s`, preservando largura de banda e impedindo interferência no ADC DMA ou PWM.
