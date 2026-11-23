# Sistema de Comunicação Digital 4-QAM em Dois ESP32 (ESP-IDF v4.4.8)

Projeto completo em **C puro (ESP-IDF v4.4.8)** implementando um **transmissor** e um **receptor** **4-QAM** independentes em placas **ESP32-WROOM-32 DevKit V1**, com filtro analógico passa-baixas RC de 2ª ordem, recuperação automática de portadora (`Costas Loop`), recuperação de temporização (`Gardner TED`), resolução automática de ambiguidade de $90^\circ$ (`Sync Word`), validação de integridade (`CRC-16/CCITT-FALSE`), interoperabilidade total com receptores externos (Modo Professor) e vetores de teste reproduzíveis.

---

## Ligações Rápidas (Hardware)

```text
  ESP32 Transmissor                                      ESP32 Receptor
  +---------------+     R1=1kΩ          R2=10kΩ         +---------------+
  | GPIO 18       |-----[====]----+-----[====]----+---->| GPIO 34       |
  | (MCPWM0A 80k) |               |               |     | (ADC1_CH6)    |
  +---------------+            C1 === 33nF     C2 === 3.3nF +---------------+
        |                         |               |           |
       GND -----------------------+---------------+---------- GND
```

- **Saída TX:** `GPIO 18` (`MCPWM0A`, PWM simétrico `80 kHz`, `fs = 40 kHz`, faixa `[0,40 V .. 2,20 V]`, `DC = 1,30 V`)
- **Entrada RX:** `GPIO 34` (`ADC1_CHANNEL_6`, DMA I2S0 contínuo a `20 kHz`, faixa linear `150 .. 2450 mV`)
- **Console serial CLI (TX e RX):** `UART0` a **`115200 bps`**

---

## Compilação e Gravação (ESP-IDF v4.4.8)

```powershell
# Carregar ambiente ESP-IDF v4.4.8
. C:\Users\gerson.marques\esp\v4.4.8\esp-idf\export.ps1

# Compilar e gravar Transmissor (substitua COMx pela porta do TX)
idf.py -C C:\Modulador_ESP\transmitter build
idf.py -C C:\Modulador_ESP\transmitter -p COMx flash monitor

# Compilar e gravar Receptor (substitua COMy pela porta do RX)
idf.py -C C:\Modulador_ESP\receiver build
idf.py -C C:\Modulador_ESP\receiver -p COMy flash monitor
```

---

## Parâmetros Padrão (Configuráveis em Execução sem Recompilar)

| Parâmetro | Valor Padrão | Comando CLI TX / RX |
|---|---:|---|
| Portadora ($f_c$) | `1000 Hz` | `/fc 1000` |
| Taxa de símbolos ($R_s$, provisória) | `100 baud` (`200 bps`) | `/baud 100` |
| Equação 4-QAM | $A[I\cos(\omega_c t) + Q\cos(\omega_c t - 90^\circ)]$ | `/qsign plus` *(ou `minus` para $-Q\sin$)* |
| Tabela 4-QAM (`Natural`) | `0:(-1,-1)`, `1:(-1,+1)`, `2:(+1,-1)`, `3:(+1,+1)` | `/map natural` *(ou `gray`)* |
| Ordem dos bits | `MSB-first` | `/order msb` *(`lsb_bit` \| `lsb_dibit`)* |
| Modo inicial compilado | TX=`Modo 1` (Professor) / RX=`Modo 2` (Quadro+CRC) | Use `/mode 2` no TX para parear com o RX padrão |

---

## Central Web de Instrumentação & Monitoramento (`platform/`)

Para operar, visualizar sinais ADC reais, constelação 4-QAM, Costas Loop, Gardner TED, FFT, filtros RC e comparar **Ground Truth (BER / SER / EVM / CRC)** no navegador:

```powershell
.\platform\start_platform.ps1
```

Veja o guia completo em [`platform/README.md`](./platform/README.md) e a especificação do protocolo de telemetria em [`docs/protocolo_telemetria.md`](./docs/protocolo_telemetria.md).

---

## Estrutura e Documentação Técnica

- [`platform/README.md`](./platform/README.md) — Central web de instrumentação (FastAPI + WebSocket + pyserial + Canvas).
- [`docs/protocolo_telemetria.md`](./docs/protocolo_telemetria.md) — Protocolo binário versionado `4QAM-TLM v1` e instrumentação Core 1 / Core 0.

- [`docs/arquitetura.md`](./docs/arquitetura.md) — Arquitetura geral, alocação Dual-Core FreeRTOS e pinagem.
- [`docs/especificacao_4qam.md`](./docs/especificacao_4qam.md) — Equações exatas, fator $A = V_{pk}/\sqrt{2}$ e formato de quadros.
- [`docs/mapeamento.md`](./docs/mapeamento.md) — Tabelas Natural/Gray, ordens de bits e rotação de quadrante.
- [`docs/circuito_analogico.md`](./docs/circuito_analogico.md) — Projeto do filtro RC 2ª ordem (`-48,83 dB @ 80 kHz`) e guia de osciloscópio.
- [`docs/transmissor.md`](./docs/transmissor.md) — Hardware `MCPWM0`, modulador racional e guia completo da CLI TX.
- [`docs/receptor.md`](./docs/receptor.md) — `ADC1 DMA`, demodulador DSP, desenquadrador e guia completo da CLI RX.
- [`docs/sincronizacao.md`](./docs/sincronizacao.md) — Boxcar (`2fc = 0`), Costas Loop PI, Gardner TED e resolução de $90^\circ$.
- [`docs/interoperabilidade.md`](./docs/interoperabilidade.md) — Guia de compatibilidade com receptores/transmissores externos.
- [`docs/testes.md`](./docs/testes.md) — Matriz de validação (Testes 1 a 8) e checklist de bancada.
- [`docs/vetores_teste/vetores_referencia.md`](./docs/vetores_teste/vetores_referencia.md) e [`vetores_amostras_4qam.csv`](./docs/vetores_teste/vetores_amostras_4qam.csv) — Vetores numéricos auditáveis (**Teste 8**).
- [`tools/gerar_vetores_teste.py`](./tools/gerar_vetores_teste.py) e [`tools/validar_sistema_e2e.py`](./tools/validar_sistema_e2e.py) — Scripts de geração de vetores e simulação física/DSP ponta a ponta.
