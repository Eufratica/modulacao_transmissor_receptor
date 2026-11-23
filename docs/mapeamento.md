# Mapeamento de Bits, Tabelas 4-QAM e Ordem de Extração

Este documento detalha o mapeamento adotado entre bytes ASCII, pares de bits (*dibits*), coordenadas $(I, Q)$ da constelação 4-QAM e o efeito de rotações de fase no canal.

---

## 1. Tabelas de Constelação Disponíveis

O sistema suporta dois presets selecionáveis em tempo de execução via `/map <natural|gray>` tanto no transmissor quanto no receptor, implementados de forma idêntica em [`qam_config.c`](../transmitter/main/qam_config.c) e [`rx_config.c`](../receiver/main/rx_config.c).

### 1.1 Tabela Padrão: **Binário Natural (`QAM_MAP_NATURAL`)**

Mapeamento direto onde o primeiro bit controla o sinal de $I$ (`0 -> -1`, `1 -> +1`) e o segundo bit controla o sinal de $Q$ (`0 -> -1`, `1 -> +1`), equivalente a dois moduladores BPSK independentes em fase e em quadratura:

| Símbolo (dec) | Par de bits | $I$ | $Q$ | Ângulo geométrico $\text{atan2}(Q,I)$ | Quadrante IQ | Fase onda (`qsign plus`) | Fase onda (`qsign minus`) |
|---:|:---:|---:|---:|---:|:---:|---:|---:|
| `0` | `"00"` | `-1` | `-1` | `225°` | 3º quadrante | `135°` | `225°` |
| `1` | `"01"` | `-1` | `+1` | `135°` | 2º quadrante | `225°` | `135°` |
| `2` | `"10"` | `+1` | `-1` | `315°` | 4º quadrante | `45°` | `315°` |
| `3` | `"11"` | `+1` | `+1` | `45°` | 1º quadrante | `315°` | `45°` |

### 1.2 Tabela Alternativa: **Código Gray (`QAM_MAP_GRAY`)**

No mapeamento Gray, símbolos adjacentes ($90^\circ$ de distância angular) diferem em **apenas 1 bit**, reduzindo erros de bit em canais muito ruidosos:

| Símbolo (dec) | Par de bits | $I$ | $Q$ | Ângulo geométrico $\text{atan2}(Q,I)$ | Quadrante IQ | Fase onda (`qsign plus`) | Fase onda (`qsign minus`) |
|---:|:---:|---:|---:|---:|:---:|---:|---:|
| `0` | `"00"` | `+1` | `+1` | `45°` | 1º quadrante | `315°` | `45°` |
| `1` | `"01"` | `-1` | `+1` | `135°` | 2º quadrante | `225°` | `135°` |
| `2` | `"10"` | `+1` | `-1` | `315°` | 4º quadrante | `45°` | `315°` |
| `3` | `"11"` | `-1` | `-1` | `225°` | 3º quadrante | `135°` | `225°` |

```text
                Q (+sin / cos - 90 deg)
                ^
   (-1, +1)     |     (+1, +1)
   Nat: 01      |     Nat: 11
   Gray: 01     |     Gray: 00
 ---------------+---------------> I (cos)
   (-1, -1)     |     (+1, -1)
   Nat: 00      |     Nat: 10
   Gray: 11     |     Gray: 10
```

> **Atenção:** a tabela padrão compilada na inicialização é a **Natural** (`0=(-1,-1), 1=(-1,+1), 2=(+1,-1), 3=(+1,+1)`). Para testes com um receptor externo que adote Gray, envie `/map gray` em ambos os terminais.

---

## 2. As 3 Ordens de Extração de Bits de um Byte ASCII

Cada byte de `8 bits` ($b_7 b_6 b_5 b_4 b_3 b_2 b_1 b_0$) gera **4 símbolos** consecutivos de 2 bits. Como sistemas diferentes serializam o byte de modos distintos, [`qam_mapper.c`](../transmitter/main/qam_mapper.c) e [`rx_mapper.c`](../receiver/main/rx_mapper.c) implementam as 3 variantes via `/order <msb|lsb_bit|lsb_dibit>`:

| Ordem | Símbolo 0 | Símbolo 1 | Símbolo 2 | Símbolo 3 | Aplicação típica |
|---|---|---|---|---|---|
| **`MSB-first`** *(padrão)* | `(b7, b6)` | `(b5, b4)` | `(b3, b2)` | `(b1, b0)` | Padrão clássico de rede, big-endian e protocolos seriais |
| **`LSB-bit-first`** | `(b0, b1)` | `(b2, b3)` | `(b4, b5)` | `(b6, b7)` | Fluxo UART/bitstream LSB-primeiro empacotado em dibits |
| **`LSB-dibit-first`** | `(b1, b0)` | `(b3, b2)` | `(b5, b4)` | `(b7, b6)` | Extração LSB-primeiro por blocos de 2 bits mantendo MSB interno do par |

### Exemplo passo a passo: Caractere `'B'` (`0x42` = `0100 0010`)
- **`MSB-first`:** `01` (`1`), `00` (`0`), `00` (`0`), `10` (`2`) $\rightarrow$ `[1, 0, 0, 2]`
- **`LSB-bit-first`:** `b0=0,b1=1 -> 01 (1)`; `b2=0,b3=0 -> 00 (0)`; `b4=0,b5=0 -> 00 (0)`; `b6=1,b7=0 -> 10 (2)` $\rightarrow$ `[1, 0, 0, 2]`
- **`LSB-dibit-first`:** `(b1,b0)=10 (2)`; `(b3,b2)=00 (0)`; `(b5,b4)=00 (0)`; `(b7,b6)=01 (1)` $\rightarrow$ `[2, 0, 0, 1]`

Veja mais exemplos (`'A'`, `'3'`, `"AB"`, `"4QAM"`) em [`vetores_referencia.md`](./vetores_teste/vetores_referencia.md).

---

## 3. Simetria de $90^\circ$ e Rotações de Quadrante no Receptor

O `Costas Loop` é um recuperador de portadora não-assistido por dados (*decision-directed* usando `sgn(I)` e `sgn(Q)`), portanto possui **ambiguidade de fase de múltiplos de $90^\circ$** ($0^\circ, 90^\circ, 180^\circ, 270^\circ$).

Em [`rx_mapper.c`](../receiver/main/rx_mapper.c), a rotação inversa aplicada ao vetor medido $(\hat{I}, \hat{Q})$ antes da decisão do quadrante segue exatamente a matriz discreta de rotação por `rot = 0..3`:

| `rot` | Ângulo compensado | $\hat{I}_{\text{rot}}$ | $\hat{Q}_{\text{rot}}$ | Como o receptor resolve |
|---:|---:|---|---|---|
| `0` | `0°` | $+\hat{I}$ | $+\hat{Q}$ | Modo 2 testa automaticamente as 4 hipóteses com a `Sync Word` (`0xD3 0x91`) |
| `1` | `90°` | $+\hat{Q}$ | $-\hat{I}$ | Resolvido automaticamente no Modo 2 ou Modo 1 com `/sync1 1` |
| `2` | `180°` | $-\hat{I}$ | $-\hat{Q}$ | Caso comum ao alternar polaridade; resolvido pela `Sync Word` ou `/rot 180` |
| `3` | `270°` | $-\hat{Q}$ | $+\hat{I}$ | Resolvido automaticamente no Modo 2 ou via `/rot 270` no Modo 1 puro |
