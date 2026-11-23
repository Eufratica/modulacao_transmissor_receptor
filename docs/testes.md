# Plano de Testes e Validação do Sistema 4-QAM

Este documento consolida todos os critérios de verificação (Testes 1 a 8) do transmissor, circuito analógico, receptor e interoperabilidade. **Por regra estrita de engenharia**, cada verificação distingue claramente o status já observado (compilação e análise) do que será executado em simulação computacional e na bancada física.

---

## 1. Matriz Geral de Testes (Testes 1 a 8)

| # | Teste | Objeto | Critério de Aprovação | Como Executar | Status Atual |
|---:|---|---|---|---|---|
| **1** | **Validação elétrica estática e de segurança** | `qam_config.c`, `rx_config.c` | `qam_config_validate()` aceita padrão (`DC=1300`, `Vpk=900`, `[400..2200] mV`); rejeita `dc < Vpk+50`, `dc+Vpk > vdd-50` e `pwm_freq % fs != 0` | `/selftest` no TX (`Teste 6`) | Compilado no TX; aguardando execução em placa |
| **2** | **Portadora pura e escala** | `qam_modulator.c` | Senoide pura `1000 Hz`, média `1300 ± 1 mV`, amplitude DFT `900 ± 2 mV`, faixa `[400..2200] mV` | `/selftest`, `/test carrier` e [`validar_sistema_e2e.py`](../tools/validar_sistema_e2e.py) | Compilado no TX; **aprovado em simulação E2E**; osciloscópio pendente |
| **3** | **Modulação 4-QAM e fases por símbolo** | `qam_mapper.c`, `qam_modulator.c` | DFT de 1 ciclo retorna exatamente $I, Q \in \{-1, +1\}$ e ângulo esperado em `Natural/+sin` e `Gray/-sin` | `/selftest`, `/test sym00..sym11` e [`validar_sistema_e2e.py`](../tools/validar_sistema_e2e.py) | Compilado no TX; **aprovado em simulação E2E**; osciloscópio pendente |
| **4** | **ASCII $\leftrightarrow$ bits $\leftrightarrow$ símbolos** | `qam_mapper.c` | *Round-trip* `0x00..0xFF` (`256/256` bytes) sem erro em `MSB-first`, `LSB-bit-first` e `LSB-dibit-first` | `/selftest` (`Teste 4`) e [`validar_sistema_e2e.py`](../tools/validar_sistema_e2e.py) | Compilado no TX; **aprovado (`256 x 3`) em Python** |
| **5** | **Enquadramento Modo 1 / Modo 2 / CRC-16** | `qam_framer.c`, `rx_deframer.c` | `"123456789"` gera `CRC = 0x29B1`; `"XYZ"` gera `12` símbolos no Modo 1 e `60` no Modo 2 | `/selftest` (`Teste 5`) e [`validar_sistema_e2e.py`](../tools/validar_sistema_e2e.py) | Compilado no TX; **aprovado em Python (`CRC=0x29B1`)** |
| **6** | **Estágio analógico e filtro RC 2ª ordem** | `GPIO 18` + `1k/33nF + 10k/3.3nF` | Ripple PWM $\le 20\text{ mV}_{pp}$ após `R2/C2`; amplitude `~855 mV`; inversão `180°` em `/test text 3` | Simulação EDO [`validar_sistema_e2e.py`](../tools/validar_sistema_e2e.py) + roteiro [`circuito_analogico.md`](./circuito_analogico.md) | **EDO exata aprovada (`Vpk=856,5 mV`, ripple `14,74 mVpp`)**; osciloscópio físico pendente |
| **7** | **Recepção ponta a ponta e malha fechada** | `qam_demod.c`, `rx_deframer.c`, `adc_sampler.c` | Recuperação integral com `CRC OK` sob $\theta_0 \in \{73^\circ, 212^\circ, 140^\circ\}$, $\Delta f_c$, $\Delta R_s$, ADC 12 bits e filtro RC | `/selftest` no RX, [`validar_sistema_e2e.py`](../tools/validar_sistema_e2e.py) e ligação TX $\leftrightarrow$ RX | Compilado no RX (com correção Gardner TED); **4 cenários E2E aprovados em Python**; bancada física pendente |
| **8** | **Vetores de teste reproduzíveis** | [`docs/vetores_teste/`](./vetores_teste/vetores_referencia.md) | Tabelas auditáveis com bits, símbolos, $(I,Q)$, ângulos, frames Modo 1/2, CRC-16 e CSV de `3200` amostras de `"AB"` | [`gerar_vetores_teste.py`](../tools/gerar_vetores_teste.py) e [`validar_sistema_e2e.py`](../tools/validar_sistema_e2e.py) | **Concluído e conferido (`3200` amostras `[400..2200] mV`)** |

---

## 2. Auto-Testes Embarcados em C

### 2.1 Transmissor ([`qam_selftest.c`](../transmitter/main/qam_selftest.c))
Executado automaticamente no boot de [`transmitter.c`](../transmitter/main/transmitter.c) ou sob demanda via `/selftest`:
1. **`test_mapping_table()`** — Verifica os 4 pares $(I, Q)$ da tabela padrão Natural (`0:(-1,-1)`, `1:(-1,+1)`, `2:(+1,-1)`, `3:(+1,+1)`).
2. **`test_carrier_generation()`** — Amostra 1 ciclo (`40` pontos) de `/test carrier` e mede DC e amplitude por DFT.
3. **`test_4qam_quadrature_for_mode()`** — Avalia cada um dos 4 símbolos via DFT para `Natural +` e `Gray -`, checando projeções $I, Q$ contra $\pm A$.
4. **`test_ascii_and_bit_orders()`** — Varre todos os `256` bytes possíveis nas 3 ordens (`MSB-first`, `LSB-bit-first`, `LSB-dibit-first`).
5. **`test_framing_and_crc()`** — Checa vetor padrão `"123456789" -> 0x29B1` e contagem de símbolos de `"XYZ"` nos Modos 1 e 2.
6. **`test_safety_validation()`** — Confirma bloqueio de configurações inseguras (ceifamento inferior, saturação acima de $V_{DD}-50$ e `pwm_freq` não múltiplo de `fs`).

### 2.2 Receptor ([`rx_selftest.c`](../receiver/main/rx_selftest.c))
Executado automaticamente no boot de [`receiver.c`](../receiver/main/receiver.c) ou via `/selftest`:
- Executa `test_closed_loop_mode2()` injetando silêncio DC (`30 ms`), rajada `"4QAM-OK!"` atenuada (`Vpk = 620 mV`, `Vdc = 1320 mV`), deslocamento fracionário (`0,37` amostra), ruído pseudo-aleatório e três combinações severas de canal:
  1. `q_sign_minus = false`, $\theta_0 = 73^\circ$, $\Delta f_c = +0{,}40\text{ Hz}$, $\Delta R_s = +0{,}15\%$
  2. `q_sign_minus = false`, $\theta_0 = 212^\circ$, $\Delta f_c = -0{,}50\text{ Hz}$, $\Delta R_s = -0{,}15\%$
  3. `q_sign_minus = true`, $\theta_0 = 140^\circ$, $\Delta f_c = +0{,}30\text{ Hz}$, $\Delta R_s = 0\%$

---

## 3. Checklist Rápido de Bancada (Após Gravação dos ESP32)

1. Conectar `GPIO 18 (TX)` $\rightarrow$ Filtro RC 2ª Ordem (`R1=1kΩ, C1=33nF, R2=10kΩ, C2=3,3nF`) $\rightarrow$ `GPIO 34 (RX)`, compartilhando `GND`.
2. Abrir terminais seriais (`115200 bps`) para TX e RX e conferir `[OK]` de boot dos dois auto-testes.
3. No TX, rodar `/test carrier` e confirmar no osciloscópio `TP3` senoide limpa de `1 kHz`, `[0,40 V .. 2,20 V]`.
4. No TX, rodar `/test text 3` e verificar inversões de fase de `180°` a cada `10 ms`; retornar com `/test stop` e `/mode 2`.
5. No RX, ativar `/monitor 1` e enviar no TX `/send Teste 4QAM ESP32!`, confirmando no RX a chegada do payload com **`CRC OK`** e pontos $(\hat{I}, \hat{Q}) \approx (\pm 0{,}71,\ \pm 0{,}71)$.
