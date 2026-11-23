# Sincronização de Portadora, Temporização de Símbolo e Ambiguidade de Quadrante

Um modulador/demodulador 4-QAM só recupera dados com confiabilidade em duas placas **ESP32 independentes** (cujos cristais de quartzo diferem em algumas dezenas de ppm e cujo instante inicial de amostragem é aleatório) quando o receptor resolve quatro problemas fundamentais de DSP. Este documento explica cada etapa implementada em [`qam_demod.c`](../receiver/main/qam_demod.c) e [`rx_deframer.c`](../receiver/main/rx_deframer.c).

---

## 1. Demodulação em Quadratura e Filtro Boxcar (Zero Exato em $2f_c$)

Multiplicando o sinal normalizado em banda passante $x_{\text{norm}}(t) = I\cos(\omega_c t + \theta) \pm Q\sin(\omega_c t + \theta)$ pelas referências locais $2\cos(\omega_c t)$ e $\pm 2\sin(\omega_c t)$ surgem dois termos espectrais:
- **Banda base útil (DC por símbolo):** proporcional a $I\cos\theta - Q\sin\theta$ e $I\sin\theta + Q\cos\theta$;
- **Componente harmônica de alta frequência em $2f_c$ (`2000 Hz`):** termo espúrio com amplitude unitária.

Em vez de um filtro IIR genérico (que deixaria *ripple* residual de $2f_c$), [`qam_demod.c`](../receiver/main/qam_demod.c) usa um **filtro FIR de média móvel (*boxcar*) com comprimento exato de 1 período da portadora**:

$$N_{\text{box}} = \operatorname{round}\!\left(\frac{f_s}{f_c}\right) = \frac{20000}{1000} = 20\text{ amostras}$$

Como a média de exatamente 1 ciclo inteiro de qualquer senoide de frequência $f_c$ (ou múltiplos $2f_c, 3f_c, \dots$) é **rigorosamente nula**:
- Em $2f_c = 2000\text{ Hz}$, a resposta em frequência $\left|\frac{\sin(\pi f / f_c)}{N\sin(\pi f / (N f_c))}\right|$ possui um **nulo matemático completo** (`-∞ dB` ideal);
- O atraso de grupo é determinístico e curto: apenas metade de um ciclo de portadora (`0,5 ms`), preservando `90%` do patamar plano de cada símbolo de `10 ms` (`100 baud`).

---

## 2. Recuperação de Portadora e Desvio de Frequência (`Costas Loop` PI de 2ª Ordem)

Mesmo configurando `fc = 1000 Hz` nos dois ESP32, a diferença entre osciladores e pequenos desvios cria um erro de fase dinâmico $\phi_e(t) = \theta_{\text{TX}}(t) - \theta_{\text{NCO}}(t)$, fazendo a constelação girar no plano I/Q.

### 2.1 Detector de Erro de Fase (*Decision-Directed PED*)
Uma vez por ciclo de portadora (`box_len = 20` amostras), o vetor filtrado $(I_b, Q_b)$ é normalizado para módulo unitário $(I_u, Q_u)$ quando $I_b^2 + Q_b^2 > 0{,}35$. Comparando com o quadrante mais próximo $(\operatorname{sgn}(I_u), \operatorname{sgn}(Q_u))$, calcula-se o produto vetorial proporcional a $\sin(\phi_e)$:

| Convenção ativa (`q_sign_minus`) | Expressão do PED em `qam_demod.c` | Sinal de realimentação |
|---|---|---|
| `false` (`I*cos + Q*sin`) | $\text{ped} = \operatorname{sgn}(Q_u)\,I_u - \operatorname{sgn}(I_u)\,Q_u$ | Corrige NCO no sentido negativo de erro |
| `true` (`I*cos - Q*sin`) | $\text{ped} = \operatorname{sgn}(I_u)\,Q_u - \operatorname{sgn}(Q_u)\,I_u$ | Inverte sinal do PED para manter realimentação negativa estável |

> **Por que o sinal do PED precisa inverter com `q_sign_minus`?** Inverter o sinal de $\sin(\omega_c t)$ no misturador inverte o sentido de giro aparente no plano $(I_b, Q_b)$ quando o NCO está adiantado vs. atrasado. Sem a inversão do PED na linha 220 de `qam_demod.c`, a malha se tornaria positivamente realimentada (*runaway*) ao usar `/qsign minus`.

### 2.2 Malha Proporcional-Integral (PI)
```c
demod->nco_phase_rad   += demod->cfg.costas_kp * ped;                    // kp = 0.16
demod->costas_freq_rad += (demod->cfg.costas_ki * ped) / (float)box_len; // ki = 0.017
```
- O termo **Proporcional (`costas_kp = 0,16`)** corrige saltos instantâneos de fase inicial $\theta_0$.
- O termo **Integral (`costas_ki = 0,017`)** acumula o desvio de frequência $\Delta f_c$ em regime permanente até **`±35 Hz`** (`costas_freq_max_rad`), permitindo manter travamento estável mesmo se o transmissor externo tiver cristal levemente deslocado.

---

## 3. Recuperação de Temporização de Símbolo (`Gardner TED` + Integrador 70%)

O receptor não sabe em que amostra começou o símbolo (`0..199` a `100 baud / 20 kHz`) nem se a taxa real de bauds do transmissor é `99,85` ou `100,15 baud`.

O algoritmo **Gardner Timing Error Detector (TED)** atua de forma não-assistida usando os pontos médios dos símbolos adjacentes (`mid_prev` em $k-1$ e `mid_curr` em $k$, coletados em `sym_phase = 0,5`) e a amostra exatamente na **fronteira entre eles** (`bnd_curr`, capturada na entrada do símbolo $k$ em `sym_phase = 0`):

$$e_\tau = \big(I_{\text{mid,curr}} - I_{\text{mid,prev}}\big)\cdot I_{\text{bnd,curr}} + \big(Q_{\text{mid,curr}} - Q_{\text{mid,prev}}\big)\cdot Q_{\text{bnd,curr}}$$

- Quando o relógio local está adiantado ou atrasado durante uma transição entre símbolos diferentes, o valor amostrado na suposta fronteira pende para o lado errado, gerando $e_\tau \ne 0$ proporcional ao deslocamento temporal.
- Quando dois símbolos consecutivos são idênticos, $(I_{\text{mid,curr}} - I_{\text{mid,prev}}) \approx 0$, e o TED naturalmente não perturba o relógio!
- A malha PI ajusta instantaneamente `sym_phase` (`gardner_kp = 0,030`) e corrige a deriva contínua da taxa de símbolos via `sym_rate_corr` (`gardner_ki = 0,0015`), limitada a **`±2%`** da taxa nominal (`±0.02 * sym_inc_nom`).
- **Integração robusta ao platô:** em vez de decidir sobre 1 única amostra, o símbolo $(\hat{I}, \hat{Q})$ é a média de todas as amostras filtradas dentro de `0,15 <= sym_phase <= 0,85` (`70%` centrais), rejeitando completamente os transitórios do filtro boxcar e do filtro RC analógico nas bordas de transição.

---

## 4. Preâmbulo Composto e Resolução da Ambiguidade de $90^\circ$

### 4.1 Por que `0x00 0x00 0x33 0x33` no Modo 2?
1. **`0x00 0x00` (`8` símbolos iguais `00`):** durante `80 ms`, não há transições de fase — cenário ideal para que a AGC estabilize $V_{pk}$ e o **Costas Loop** trave frequência e fase da portadora sem saltos.
2. **`0x33 0x33` (`8` símbolos alternando `00 -> 11 -> 00 -> 11`):** com a tabela Natural (`(-1,-1) <-> (+1,+1)`), cada transição muda a fase em exatos **$180^\circ$**, maximizando o gradiente de erro do **Gardner TED** para alinhar o início exato de cada símbolo em poucos dezenas de milissegundos.

### 4.2 Resolvendo os 4 Quadrantes Possíveis (`0°`, `90°`, `180°`, `270°`)
Como o plano 4-QAM é simétrico a rotações de $90^\circ$, o Costas Loop possui 4 pontos de equilíbrio idênticos. No **Modo 2** (e Modo 1 com `/sync1 1`), [`rx_deframer.c`](../receiver/main/rx_deframer.c) armazena as estimativas $(\hat{I}, \hat{Q})$ dos últimos `8` símbolos (`sync_word_len * 4`) e verifica em paralelo se a `Sync Word` (`0xD3 0x91`) aparece sob rotação `0`, `1` (`90°`), `2` (`180°`) ou `3` (`270°`). Ao casar exatamente `0xD3 0x91`, fixa `locked_rot` para todo o restante do quadro.

### 4.3 Limitações Conhecidas e Contornos
- **Modo 1 sem Sync Word (`/sync1 0`):** sem preâmbulo ou `Sync Word`, não há referência de ângulo absoluto em rajadas aleatórias; se a palavra decodificada sair embaralhada por rotação fixa de quadrante, utilize `/rot <90|180|270>` ou ative `/pre1 1` + `/sync1 1`.
- **Sequências muito longas sem transição:** longas cadeias de bytes `0x00` no Modo 1 reduzem a taxa de atualização do Gardner TED (embora o erro acumulado de cristal em `100 baud` permaneça pequeno); no Modo 2 o limite máximo de payload de `240 bytes` e o preâmbulo `0x33` garantem sincronismo sólido.
- **Faixa de captura de frequência:** o Costas Loop rastreia até `±35 Hz` ao redor de `fc`; acima disso, ajuste `/fc` no RX.
