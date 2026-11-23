# Especificação Matemática do Sinal 4-QAM

Este documento formaliza o modelo matemático utilizado pelo transmissor ([`qam_modulator.c`](../transmitter/main/qam_modulator.c)) e pelo receptor ([`qam_demod.c`](../receiver/main/qam_demod.c)), esclarecendo a equivalência trigonométrica entre as formas com dois cossenos e seno/cosseno, bem como a relação entre a amplitude de pico $V_{pk}$ e o fator $A$.

---

## 1. Forma Geral em Banda Passante (Modulação I/Q)

Na saída analógica após o filtro passa-baixas, a tensão contínua transmitida durante o $k$-ésimo intervalo de símbolo $t \in [k T_s,\ (k+1)T_s)$ é:

$$v(t) = V_{DC} + p \cdot w(t) \cdot s(t)$$

onde:
- $V_{DC}$ é o nível médio em corrente contínua (`dc_offset_mv`, padrão **`1300 mV`**);
- $p \in \{+1, -1\}$ é a polaridade global (`invert_polarity`);
- $w(t) \in [0, 1]$ é o envelope de conformação de pulso (`QAM_PULSE_RECTANGULAR`: $w(t) = 1$ durante todo o símbolo; ou `QAM_PULSE_RAISED_EDGE_TAPER`: rampa suave em $10\%$ das bordas).

### 1.1 Equação com Dois Cossenos ($0^\circ$ e $-90^\circ$) — Padrão (`q_sign_minus = false`)

Pela identidade trigonométrica $\cos(\theta - 90^\circ) = +\sin(\theta)$, somar duas portadoras senoidais em quadratura defasadas de $-90^\circ$:

$$s(t) = A \Big[ I_k \cos(2\pi f_c t) + Q_k \cos(2\pi f_c t - 90^\circ) \Big]$$

é **matematicamente idêntico** a:

$$s(t) = A \Big[ I_k \cos(2\pi f_c t) + Q_k \sin(2\pi f_c t) \Big]$$

No transmissor e no receptor deste projeto, **esta é a convenção padrão** (`q_sign_minus = false`).

### 1.2 Forma Alternativa com Subtração (`q_sign_minus = true`)

Muitos livros-texto definem o sinal em banda passante como a parte real da envoltória complexa $\operatorname{Re}\big\{(I_k + j Q_k)\,e^{j 2\pi f_c t}\big\}$:

$$s(t) = A \Big[ I_k \cos(2\pi f_c t) - Q_k \sin(2\pi f_c t) \Big]$$

Alternando `/qsign minus` no TX e no RX (ou para casar com um receptor de referência que implemente o sinal negativo no ramo de quadratura), ambas as placas passam a operar instantaneamente nesta convenção sem recompilar.

---

## 2. Onde Fica o Fator $A$: Fora vs. Dentro de $(I_k, Q_k)$

Existem duas formas algebricamente equivalentes de escrever o mesmo sinal 4-QAM:

1. **Amplitude $A$ fatorada externamente (símbolos normalizados $\pm 1$):**
   $$I \in \{-1, +1\}, \quad Q \in \{-1, +1\}, \quad s(t) = A \big[ I \cos(2\pi f_c t) \pm Q \sin(2\pi f_c t) \big]$$
2. **Amplitude absorvida em $(I_k, Q_k)$ (notação polar com raio $A_k$):**
   $$I_k = A_k \cos(\theta_k), \quad Q_k = A_k \sin(\theta_k), \quad s(t) = I_k \cos(2\pi f_c t) \pm Q_k \sin(2\pi f_c t)$$

No **4-QAM (QPSK)** os 4 símbolos situam-se nos ângulos diagonais $\theta_k \in \{45^\circ, 135^\circ, 225^\circ, 315^\circ\}$, nos quais $|\cos(\theta_k)| = |\sin(\theta_k)| = \frac{1}{\sqrt{2}}$. Definindo $A_k = V_{pk}$, obtém-se:

$$I_k = V_{pk} \cos(\theta_k) = \frac{V_{pk}}{\sqrt{2}} \cdot I, \qquad Q_k = V_{pk} \sin(\theta_k) = \frac{V_{pk}}{\sqrt{2}} \cdot Q$$

Logo, $A = \frac{V_{pk}}{\sqrt{2}}$ (no código `mod->a_eq_mv = cfg->amplitude_peak_mv / QAM_SQRT2_F`). Assim:
- Para $V_{pk} = 900\text{ mV}$, temos $A \approx 636{,}40\text{ mV}$.
- A amplitude resultante instantânea da onda modulada é sempre exatamente:
  $$\sqrt{(A\,I)^2 + (A\,Q)^2} = A \sqrt{1^2 + 1^2} = A\sqrt{2} = V_{pk} = 900\text{ mV}$$
- Se o firmware tivesse usado $A = V_{pk}$ diretamente sem dividir por $\sqrt{2}$, o pico atingiria $900\sqrt{2} = 1272{,}8\text{ mV}$, gerando risco de ceifamento em `0 V` no PWM!

> **Consequência no receptor:** como o receptor normaliza a amplitude via AGC antes do slicer e o sinal $(I_k, Q_k)$ só se distingue por um escalar positivo constante ($A$ ou $1$), qualquer receptor que use $A$ externo ou $A$ embutido em $I_k/Q_k$ demodula exatamente o mesmo sinal físico.

---

## 3. Ângulo Geométrico $\theta_k$ no Plano I/Q vs. Fase $\phi$ da Forma de Onda

É importante não confundir o ângulo geométrico da constelação $\theta_k = \operatorname{atan2}(Q, I)$ com o avanço de fase $\phi$ quando escrevemos a onda real como um único cosseno $V_{pk}\cos(\omega_c t + \phi)$:

Pela expansão trigonométrica:
$$V_{pk}\cos(\omega_c t + \phi) = V_{pk}\cos(\phi)\cos(\omega_c t) - V_{pk}\sin(\phi)\sin(\omega_c t)$$

| Convenção da equação | $\cos(\phi)$ | $\sin(\phi)$ | Relação com $\theta_k = \operatorname{atan2}(Q, I)$ |
|---|---|---|---|
| **Padrão `+sin` (`qsign plus`)** | $I / \sqrt{2}$ | $-Q / \sqrt{2}$ | $\phi = -\theta_k = \operatorname{atan2}(-Q, I)$ |
| **Alternativa `-sin` (`qsign minus`)** | $I / \sqrt{2}$ | $+Q / \sqrt{2}$ | $\phi = +\theta_k = \operatorname{atan2}(+Q, I)$ |

Na tabela padrão **Natural** (`0:(-1,-1)`, `1:(-1,+1)`, `2:(+1,-1)`, `3:(+1,+1)`):
- Os 4 pontos permanecem igualmente espaçados de **$90^\circ$**, com distância euclidiana mínima normalizada igual a `2` (ou `2 * A` em tensão), garantindo robustez idêntica frente a ruído gaussiano aditivo em ambas as convenções.

---

## 4. Parâmetros Temporais e Espectrais (Padrão Inicial)

| Grandeza | Símbolo | Padrão | Fórmulas e Limites no Código (`qam_config.c`) |
|---|---|---:|---|
| Frequência da portadora | $f_c$ | `1000 Hz` | `50 .. 20000 Hz` (`100 .. 10000 Hz` no RX) |
| Taxa de símbolos | $R_s$ | `100 baud` *(provisório)* | `1 .. 5000 baud` (`5 .. 2000 baud` no RX) |
| Taxa de bits bruta (4-QAM) | $R_b$ | `200 bps` | $R_b = 2 \cdot R_s$ (`2 bits/símbolo`) |
| Ciclos de portadora por símbolo | $N_c$ | `10` | $N_c = f_c / R_s = 1000 / 100 = 10$ |
| Banda principal (lóbulo principal) | $B_{\text{nulo-nulo}}$ | `200 Hz` | $2 R_s$ centrada em $f_c$ (`900 .. 1100 Hz`) |
| Taxa de amostragem TX | $f_{s,\text{TX}}$ | `40.000 Hz` | Exige $f_{s,\text{TX}} \ge 8 f_c$ e $f_{s,\text{TX}} \ge 4 R_s$ |
| Taxa de amostragem RX (`ADC DMA`) | $f_{s,\text{RX}}$ | `20.000 Hz` | Mínimo DMA ESP32 (`20000 .. 80000 Hz`), $\ge 8 f_c$ e $\ge 10 R_s$ |
| Excursão de tensão analógica | $[V_{\min}, V_{\max}]$ | `[400 .. 2200] mV` | Exige $V_{DC} \ge V_{pk} + 50$ e $V_{DC} + V_{pk} \le V_{DD} - 50$ |

---

## 5. Estrutura de Quadros (Modo 1 vs. Modo 2) e CRC-16

### Modo 1 — Compatibilidade Estrita (Professor)
Por padrão, `mode1_use_preamble = false` e `mode1_use_sync_word = false`:
```text
[ Payload (N bytes -> 4N símbolos na ordem configurada em bit_order) ]
```
Zero bytes extras são adicionados ao fluxo. Preâmbulo ou `Sync Word` só são inseridos se o usuário habilitar manualmente (`/pre1 1` no TX, `/sync1 1` no RX).

### Modo 2 — Quadro Próprio Estruturado (`rx_mode = 2`, `tx_mode = 2`)
Todos os campos de controle (`Preâmbulo`, `Sync Word`, `Cabeçalho` e `CRC-16`) utilizam ordem fixa **`MSB-first`** para decodificação universal; o **`Payload`** utiliza o parâmetro `bit_order` gravado no cabeçalho:

```text
+------------------+------------+----------------+-------------------+-------------------+
| Preâmbulo (4 B)  | Sync (2 B) | Cabeçalho (4B) | Payload (0..240B) | CRC-16 (2 B)      |
| 00 00 33 33      | D3 91      | Ver|Ord|Rs|Len | Dados ASCII/bin   | CCITT-FALSE (MSB) |
+------------------+------------+----------------+-------------------+-------------------+
     16 símb.          8 símb.       16 símb.          4*Len símb.           8 símb.
```

Detalhamento do **Cabeçalho (`4 bytes`)**:
- `header[0]`: bits `[7..4]` = versão do protocolo (`0x1`); bits `[3..2]` = `bits_per_symbol - 2` (`0` para 4-QAM); bits `[1..0]` = `bit_order` (`0=MSB-first`, `1=LSB-bit-first`, `2=LSB-dibit-first`).
- `header[1..2]`: `symbol_rate_baud` em *big-endian* (`uint16_t`).
- `header[3]`: comprimento do payload `L` em bytes (`0 .. 240`).

Detalhamento do **CRC-16/CCITT-FALSE**:
- **Polinômio:** `0x1021` ($x^{16} + x^{12} + x^5 + 1$)
- **Valor inicial:** `0xFFFF`, sem reflexão de entrada/saída e `XOR final = 0x0000`.
- **Área coberta:** exatamente os bytes `[header[0..3], payload[0..L-1]]` (excluindo preâmbulo e sync).
- **Vetor de verificação padrão:** `"123456789"` $\rightarrow$ **`0x29B1`**.
