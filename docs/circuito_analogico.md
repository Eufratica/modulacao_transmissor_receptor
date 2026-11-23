# Circuito Analógico, Filtro Passa-Baixas e Validação no Osciloscópio (Etapa C)

Este documento descreve o projeto completo do estágio analógico de reconstrução do sinal 4-QAM a partir da saída PWM do ESP32 transmissor, a interface elétrica segura com o ESP32 receptor (ou receptor externo) e o roteiro de testes em osciloscópio.

---

## 1. Parâmetros do Sinal Gerado pelo Transmissor

Conforme a implementação da **Etapa B** ([qam_config.c](../transmitter/main/qam_config.c) e [pwm_driver.c](../transmitter/main/pwm_driver.c)):

| Parâmetro | Valor Padrão | Observação |
|---|---:|---|
| Pino de saída PWM | `GPIO 18` | `MCPWM0A` em modo simétrico (`UP_DOWN_COUNTER`) |
| Frequência de chaveamento PWM ($f_{\text{PWM}}$) | `80.000 Hz` | Período $T_{\text{PWM}} = 12{,}5\ \mu\text{s}$ (`1000` níveis @ `160 MHz`) |
| Taxa de atualização das amostras ($f_s$) | `40.000 Hz` | 40 amostras/ciclo da portadora (1 amostra a cada 2 ciclos PWM) |
| Frequência da portadora ($f_c$) | `1.000 Hz` | Período $T_c = 1{,}00\text{ ms}$ |
| Taxa de símbolos ($R_s$, provisória) | `100 baud` | $T_s = 10{,}0\text{ ms}$ ($10$ ciclos de portadora por símbolo) |
| Banda ocupada principal ($f_c \pm R_s$) | `900 .. 1.100 Hz` | Até ~`1.300 Hz` considerando lóbulos laterais principais |
| Tensão lógica de saída ($V_{DD}$) | `3,30 V` | Níveis PWM `0 V` / `3,30 V` |
| Offset DC configurado ($V_{DC}$) | `1.300 mV` (`1,30 V`) | Centro seguro para faixa linear do ADC1 do ESP32 |
| Amplitude de pico ($V_{pk}$) | `900 mV` (`0,90 V`) | Excursão ideal `[0,40 V .. 2,20 V]` ($1{,}80\text{ V}_{pp}$) |

### Espectro antes do filtro
Na saída digital `GPIO 18`, o sinal PWM possui:
1. **Componente DC:** $V_{DC} = 1{,}30\text{ V}$.
2. **Sinal útil 4-QAM:** centrado em $f_c = 1\text{ kHz}$ com bandas laterais principais entre $900\text{ Hz}$ e $1{,}3\text{ kHz}$.
3. **Imagens da amostragem ZOH ($f_s = 40\text{ kHz}$):** primeira imagem em $f_s - f_c = 39\text{ kHz}$ (já atenuada em $\sim 32\text{ dB}$ pelo próprio degrau de retenção de ordem zero).
4. **Portadora de chaveamento PWM ($f_{\text{PWM}} = 80\text{ kHz}$):** harmônica fundamental com pior caso em *duty* 50%:
   $$V_{\text{ripple,pk}}(80\text{ kHz}) = \frac{2\,V_{DD}}{\pi} \approx 2{,}10\text{ V} \quad (4{,}20\text{ V}_{pp})$$

O filtro passa-baixas deve preservar a faixa de `0 Hz` a `1,3 kHz` (tanto amplitude quanto linearidade de fase) e rejeitar fortemente `39 kHz` e `80 kHz`.

---

## 2. Análise Comparativa: Filtro RC de 1ª Ordem vs. 2ª Ordem

### 2.1 Avaliação do Filtro RC de 1ª Ordem
Com um único estágio $R = 3{,}3\text{ k}\Omega$ e $C = 10\text{ nF}$ ($f_{-3\text{ dB}} = 4{,}82\text{ kHz}$):
- **Em $1\text{ kHz}$:** $|H_1| = 0{,}979$ (`-0,18 dB`) — excelente preservação da portadora.
- **Em $80\text{ kHz}$** (inclinação de apenas `-20 dB/década`):
  $$|H_1(80\text{ kHz})| = \frac{1}{\sqrt{1 + (80000 / 4822{,}9)^2}} = 0{,}0602 \quad (-24{,}4\text{ dB})$$
- **Ripple residual em $80\text{ kHz}$:** $4{,}20\text{ V}_{pp} \times 0{,}0602 \approx \mathbf{253\text{ mV}_{pp}}$ (~14% do sinal útil).

**Conclusão:** O filtro de 1ª ordem deixa muito resíduo de `80 kHz` sobre a senoide de `1 kHz` (visível como traço espesso/serrilhado no osciloscópio e ruído de alta frequência no ADC).

### 2.2 Projeto Recomendado: Filtro RC Passivo de 2ª Ordem com Escalonamento 10:1
Dois estágios RC passivos em cascata com **escalonamento de impedância 10×** ($R_2 = 10\,R_1$, $C_2 = C_1/10$) evitam que o segundo estágio carregue fortemente o primeiro, obtendo inclinação assintótica de **`-40 dB/década`** sem necessidade de amplificador operacional.

#### Valores comerciais selecionados (Projeto Principal — Série E12):
- **$R_1 = 1{,}0\text{ k}\Omega$** (1/4 W, 1% ou 5%)
- **$C_1 = 33\text{ nF}$** (código `333`, cerâmico NP0/C0G ou poliéster; evitar cerâmico Y5V por variação com tensão)
- **$R_2 = 10\text{ k}\Omega$** (1/4 W, 1% ou 5%)
- **$C_2 = 3{,}3\text{ nF}$** (código `332`, cerâmico NP0/C0G ou poliéster)

#### Função de transferência exata (incluindo o termo de acoplamento $R_1 C_2$):
$$H_2(s) = \frac{1}{1 + s(R_1 C_1 + R_2 C_2 + R_1 C_2) + s^2 R_1 R_2 C_1 C_2}$$

Substituindo os valores:
- $\tau_1 = R_1 C_1 = 33{,}0\ \mu\text{s} \implies f_{p1} = \frac{1}{2\pi \tau_1} = 4{,}82\text{ kHz}$
- $\tau_2 = R_2 C_2 = 33{,}0\ \mu\text{s} \implies f_{p2} = \frac{1}{2\pi \tau_2} = 4{,}82\text{ kHz}$
- Termo de carregamento: $R_1 C_2 = 3{,}3\ \mu\text{s}$
- $b_1 = 33{,}0 + 33{,}0 + 3{,}3 = 69{,}3\ \mu\text{s}$
- $b_2 = 33{,}0\ \mu\text{s} \times 33{,}0\ \mu\text{s} = 1{,}089 \times 10^{-9}\text{ s}^2$

#### Resposta calculada por frequência:

| Frequência | Função no Sistema | Ganho $\|H_2\|$ | Atenuação (dB) | Efeito Físico |
|---:|---|---:|---:|---|
| `0 Hz` | Offset DC ($1{,}30\text{ V}$) | `1,0000` | `0,00 dB` | Offset preservado intacto |
| `900 Hz` | Banda lateral inferior ($R_s=100$) | `0,9600` | `-0,35 dB` | Preservação excelente |
| **`1.000 Hz`** | **Portadora 4-QAM ($f_c$)** | **`0,9511`** | **`-0,44 dB`** | **Senoide passa com $V_{pk} \approx 856\text{ mV}$ ($1{,}71\text{ V}_{pp}$)** |
| `1.100 Hz` | Banda lateral superior ($R_s=100$) | `0,9415` | `-0,52 dB` | Assimetria lateral $< 0{,}17\text{ dB}$ |
| `2.900 Hz` | Ponto efetivo de $-3\text{ dB}$ | `0,706` | `-3,02 dB` | Permite elevar $R_s$ até ~`500 baud` |
| `39.000 Hz` | 1ª imagem ZOH ($f_s - f_c$) | `0,0150` | `-36,47 dB` | Somado ao `sinc` ZOH: $\sim -68\text{ dB}$ |
| **`80.000 Hz`** | **Fundamental PWM ($f_{\text{PWM}}$)** | **`0,00362`** | **`-48,83 dB`** | **Ripple cai para $\approx 15{,}2\text{ mV}_{pp}$ ($< 0{,}9\%$)** |

#### Alternativas comerciais equivalentes (conforme estoque da bancada):
- **Alternativa 1 (mesmo corte $f_{-3\text{ dB}} \approx 2{,}9\text{ kHz}$, capacitores mais comuns):**
  `R1 = 1,5 kΩ`, `C1 = 22 nF (223)`, `R2 = 15 kΩ`, `C2 = 2,2 nF (222)` ($\tau = 33\ \mu\text{s}$).
- **Alternativa 2 (se só possuir `10 nF` e `1 nF`, maior impedância de saída):**
  `R1 = 3,3 kΩ`, `C1 = 10 nF (103)`, `R2 = 33 kΩ`, `C2 = 1 nF (102)` ($\tau = 33\ \mu\text{s}$; manter `C2` conectado bem próximo ao pino do ADC).
- **Alternativa 3 (maior atenuação PWM se usar apenas $R_s \le 200\text{ baud}$):**
  `R1 = 1,5 kΩ`, `C1 = 33 nF`, `R2 = 15 kΩ`, `C2 = 3,3 nF` $\implies f_{-3\text{ dB}} \approx 1{,}93\text{ kHz}$, `-0,98 dB` em `1 kHz` e **`-55,8 dB`** em `80 kHz` (ripple de apenas $\approx 6{,}8\text{ mV}_{pp}$).
- *(Nota via software: alterar PWM para `100 kHz` e `fs = 50 kHz` via `/pwm 100000 sym` + `/fs 50000` reduz o ripple em mais ~`3,9 dB` adicionais sem trocar componentes!)*

---

## 3. Esquema Elétrico Completo e Proteção entre TX e RX

```text
  ESP32 TRANSMISSOR                                          ESP32 RECEPTOR
  =================                                          ==============
                         TP1          TP2              TP3
  GPIO 18 (MCPWM0A) o----+----[ R1 ]---+----[ R2 ]------+----o GPIO 34 (ADC1_CH6)
     (0 .. 3,3V PWM)          1,0 kΩ   |    10 kΩ       |       (entrada 0,44..2,16V)
                                       |                |
                                     [ C1 ]           [ C2 ]
                                     33 nF            3,3 nF
                                       |                |
  GND               o------------------+----------------+----o GND (OBRIGATÓRIO)
                                       |
                                      === (Terra comum do Osciloscópio)
```

### 3.1 Pontos de Medição (Test Points)
- **TP1 (`GPIO 18`):** Onda PWM digital bruta (`0..3,3 V` a `80 kHz`).
- **TP2 (entre `R1/C1` e `R2`):** Sinal após o 1º estágio RC (senoide com ripple de ~`250 mVpp`).
- **TP3 (`V_OUT` / `GPIO 34`):** Saída analógica final reconstruída (`1 kHz`, $V_{DC} \approx 1{,}30\text{ V}$, $V_{pp} \approx 1{,}71\text{ V}$, faixa `+0,44 V` a `+2,16 V`).

### 3.2 Verificação de Limites Elétricos e Proteção
1. **Sem conexão direta:** `GPIO 18` nunca deve ir diretamente a `GPIO 34` sem o filtro RC. Os resistores em série (`1 kΩ + 10 kΩ = 11 kΩ`) limitam a corrente em qualquer condição anormal para $< 0{,}3\text{ mA}$, protegendo os diodos internos de ESD do ESP32 receptor mesmo se uma placa for desligada antes da outra.
2. **Faixa linear do ADC1 do ESP32:** Com atenuação `11 dB` (`ADC_ATTEN_DB_12`), a região linear recomendada vai de `150 mV` a `2.450 mV`. Nossa faixa filtrada (`440 mV` a `2.160 mV`) respeita essa janela com folga e nunca fica negativa nem acima de `3,3 V`.
3. **Impedância de entrada do ADC:** A resistência série de `11 kΩ` em conjunto com `C2 = 3,3 nF` funcionando como capacitor de reservatório local ($C_2 \gg C_{\text{sample}}$ interno do ADC SAR) evita queda de tensão nos instantes de amostragem a `40 kHz`.
4. **Caso conecte um receptor externo acoplado em AC (placa de som, sistema bipolar $\pm 1\text{ V}$):**
   Adicione um capacitor de desacoplamento após `TP3` (NÃO use isso ao conectar no segundo ESP32):
   ```text
   TP3 o----||----+----o Saida AC centrada em 0 V (+/- 0,86 V)
           10 uF  |
               [100 kΩ]
                  |
   GND o----------+----o GND externo
   ```

---

## 4. Procedimento de Validação no Osciloscópio (Testes 2, 3 e 4)

> **Pré-requisito:** Grave o firmware da Etapa B no transmissor (`idf.py -p COMx flash monitor`), monte o filtro em protoboard com `GND` ligado à garra jacaré da ponta de prova `10x` (ou `1x`) e confira as conexões antes de energizar.

### Passo 1 — Inspeção do PWM bruto (TP1)
- Conecte a ponta de prova em **TP1**.
- Ajuste o osciloscópio: **Acoplamento `DC`**, **`1 V/div`**, base de tempo **`5 us/div`**, Trigger em borda de subida em **`1,65 V`**.
- **Esperado:** pulso retangular de `0 V` a `3,30 V` com frequência exata de **`80,0 kHz`** ($T = 12{,}5\ \mu\text{s}$). Em repouso (`DC silence`), a largura do pulso permanece fixa em ~`39,4%` (`~4,92 us` em nível alto).

### Passo 2 — Validação da Portadora Pura (Teste 2 — TP3)
- Mova a ponta de prova para **TP3 (`V_OUT`)**.
- No terminal serial do transmissor, envie:
  `/test carrier`
- Ajuste o osciloscópio: **Acoplamento `DC`**, **`500 mV/div`**, base de tempo **`500 us/div`** (ou `250 us/div`), Trigger em **`1,30 V`**.
- **Esperado:**
  - Senoide limpa e estável de **`1,000 kHz`** ($T = 1{,}00\text{ ms}$);
  - Tensão média (`Vavg` / `Mean`): **`1,30 V`** ($\pm 30\text{ mV}$);
  - Pico-a-pico (`Vpp`): **`~1,71 V`** (mínimo `~0,44 V`, máximo `~2,16 V`);
  - Para medir o ripple residual de `80 kHz`, mude o canal para **Acoplamento `AC`**, **`20 mV/div`**, base de tempo **`10 us/div`** — deve aparecer ondulação de `80 kHz` da ordem de **`15 mVpp`** (ou `~7 mVpp` com o filtro alternativo de `1,5 kΩ/15 kΩ`).

### Passo 3 — Verificação de Fase dos 4 Símbolos 4-QAM (Teste 3)
1. **Teste de símbolo estacionário:** envie `/test sym00`, depois `/test sym01`, `/test sym10`, `/test sym11`. Em cada um, confirme no osciloscópio em **TP3** que a frequência permanece em `1,000 kHz` e o `Vpp` permanece constante em `~1,71 V` (característica de envoltória constante do 4-QAM!).
2. **Teste de inversão de fase de `180°` (mais fácil de enxergar em 1 canal):**
   - Envie `/test text 3` (caractere `'3' = 0x33 = 00 11 00 11`, alterna `(-1,-1)` e `(+1,+1)` — salto de `180°` a cada símbolo de `10 ms`!).
   - Ajuste o osciloscópio para **`2 ms/div`** ou **`5 ms/div`** e aperte **Single / Stop** (ou Trigger por largura de pulso / subida).
   - **Esperado:** a cada `10,0 ms`, exatamente na transição de símbolo, a senoide inverte instantaneamente de fase (`180°`), formando uma "dobra" nítida na onda sem alterar a amplitude fora da transição.
3. **Teste de ciclo completo dos 4 símbolos (`/test cycle`):**
   - Envie `/test cycle` (sequência contínua `00 → 01 → 11 → 10`, com salto de `90°` a cada `10 ms`).
   - Capture com **Single** em **`5 ms/div`** para observar as quebras periódicas de `90°` a cada `10,0 ms`.

### Passo 4 — Visualização de Mensagem ASCII Real (Modo 1 e Modo 2)
- Envie `/test stop` (retorna ao Modo 1 — Professor, repouso DC em `1,30 V`).
- Ajuste Trigger para borda de subida em **`1,75 V`** e modo **Normal** (não Auto) ou **Single**, base de tempo **`10 ms/div`**.
- Digite `AB` no terminal e pressione Enter:
  - `'A'` (4 símbolos) + `'B'` (4 símbolos) = 8 símbolos $\times\ 10\text{ ms} = \mathbf{80\text{ ms}}$ de rajada modulada, retornando exatamente para `1,30 V` ao final!
- Envie `/mode 2`, repita `AB` e observe o aumento da rajada devido ao preâmbulo + sync + cabeçalho + CRC-16 (`14` bytes $\times 4 \times 10\text{ ms} = 560\text{ ms}$).
