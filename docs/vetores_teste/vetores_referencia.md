# Vetores de Teste Reproduzíveis 4-QAM (Teste 8)

Gerado automaticamente por [`tools/gerar_vetores_teste.py`](../../tools/gerar_vetores_teste.py) com exatamente a mesma matemática e enquadramento dos firmwares `transmitter/` e `receiver/`.

## 1. Parâmetros de Referência do Cenário Padrão

| Parâmetro | Valor Padrão | Observação |
|---|---:|---|
| Portadora ($f_c$) | `1000 Hz` | Período $T_c = 1{,}000\text{ ms}$ |
| Taxa de símbolos ($R_s$, provisória) | `100 baud` | $T_s = 10{,}000\text{ ms}$ (400 amostras/símbolo a `40 kHz`) |
| Taxa de amostragem TX ($f_s$) | `40000 Hz` | Divisor 2:1 do PWM a `80000 Hz` (`MCPWM0A`, topo `1000` ticks) |
| Offset DC ($V_{DC}$) | `1300 mV` | Centro PWM teórico = `394` ticks (`39,39%`) |
| Amplitude de pico ($V_{pk}$) | `900 mV` | Excursão ideal `[400 .. 2200] mV` |
| Fator $A = V_{pk}/\sqrt{2}$ | `636.3961 mV` | Garante $\sqrt{(A\,I)^2 + (A\,Q)^2} = 900.00\text{ mV}$ |
| Equação padrão (`qsign plus`) | $s(t) = A[I\cos(\omega_c t) + Q\cos(\omega_c t - 90^\circ)]$ | Equivalente a $A[I\cos(\omega_c t) + Q\sin(\omega_c t)]$ |
| Constelação padrão (`map natural`) | `0:(-1,-1)`, `1:(-1,+1)`, `2:(+1,-1)`, `3:(+1,+1)` | Alternativa Gray disponível via `/map gray` |
| Ordem de bits padrão (`order msb`) | `MSB-first` | `(b7,b6), (b5,b4), (b3,b2), (b1,b0)` |

## 2. Vetor Base por Símbolo (Modo 3 — `/test sym00|sym01|sym10|sym11`)

### 2.1 Tabela Padrão Natural (`map natural`)

| Símbolo | Bits | $I$ | $Q$ | $I_{\text{mV}} = A\,I$ | $Q_{\text{mV}} = A\,Q$ | Ângulo geom. $\theta_k = \text{atan2}(Q,I)$ | Fase onda $\phi$ em $\cos(\omega_c t + \phi)$ (`+sin` / `-sin`) | $v(0\text{ ms})$ (`+sin`) | `PWM` (`t=0`) |
|---:|:---:|---:|---:|---:|---:|---:|:---:|---:|---:|
| `0` | `00` | `-1` | `-1` | `-636.40` | `-636.40` | `225°` | `135°` / `225°` | `663.60 mV` | `201` |
| `1` | `01` | `-1` | `+1` | `-636.40` | `+636.40` | `135°` | `225°` / `135°` | `663.60 mV` | `201` |
| `2` | `10` | `+1` | `-1` | `+636.40` | `-636.40` | `315°` | `45°` / `315°` | `1936.40 mV` | `587` |
| `3` | `11` | `+1` | `+1` | `+636.40` | `+636.40` | `45°` | `315°` / `45°` | `1936.40 mV` | `587` |

### 2.2 Tabela Alternativa Gray (`map gray`)

| Símbolo | Bits | $I$ | $Q$ | Ângulo geom. $\text{atan2}(Q,I)$ | Fase onda $\phi$ (`+sin` / `-sin`) |
|---:|:---:|---:|---:|---:|:---:|
| `0` | `00` | `+1` | `+1` | `45°` | `315°` / `45°` |
| `1` | `01` | `-1` | `+1` | `135°` | `225°` / `135°` |
| `2` | `10` | `+1` | `-1` | `315°` | `45°` / `315°` |
| `3` | `11` | `-1` | `-1` | `225°` | `135°` / `225°` |

### 2.3 Primeiras 10 Amostras ($0 \le n \le 9$, $1/4$ de ciclo a `40 kHz`) para Cada Símbolo (`Natural`, `+sin`)

| Amostra $n$ | $t$ (ms) | Símbolo `0` (`-1,-1`) | Símbolo `1` (`-1,+1`) | Símbolo `2` (`+1,-1`) | Símbolo `3` (`+1,+1`) |
|---:|---:|---:|---:|---:|---:|
| `0` | `0.000` | ` 663.60 mV` (`201`) | ` 663.60 mV` (`201`) | `1936.40 mV` (`587`) | `1936.40 mV` (`587`) |
| `1` | `0.025` | ` 571.88 mV` (`173`) | ` 770.99 mV` (`234`) | `1829.01 mV` (`554`) | `2028.12 mV` (`615`) |
| `2` | `0.050` | ` 498.09 mV` (`151`) | ` 891.41 mV` (`270`) | `1708.59 mV` (`518`) | `2101.91 mV` (`637`) |
| `3` | `0.075` | ` 444.05 mV` (`135`) | `1021.88 mV` (`310`) | `1578.12 mV` (`478`) | `2155.95 mV` (`653`) |
| `4` | `0.100` | ` 411.08 mV` (`125`) | `1159.21 mV` (`351`) | `1440.79 mV` (`437`) | `2188.92 mV` (`663`) |
| `5` | `0.125` | ` 400.00 mV` (`121`) | `1300.00 mV` (`394`) | `1300.00 mV` (`394`) | `2200.00 mV` (`667`) |
| `6` | `0.150` | ` 411.08 mV` (`125`) | `1440.79 mV` (`437`) | `1159.21 mV` (`351`) | `2188.92 mV` (`663`) |
| `7` | `0.175` | ` 444.05 mV` (`135`) | `1578.12 mV` (`478`) | `1021.88 mV` (`310`) | `2155.95 mV` (`653`) |
| `8` | `0.200` | ` 498.09 mV` (`151`) | `1708.59 mV` (`518`) | ` 891.41 mV` (`270`) | `2101.91 mV` (`637`) |
| `9` | `0.225` | ` 571.88 mV` (`173`) | `1829.01 mV` (`554`) | ` 770.99 mV` (`234`) | `2028.12 mV` (`615`) |

## 3. Vetores ASCII nas 3 Ordens de Bits (`A`, `B`, `3`, `AB`)

### 3.1 Caractere `'A'` (`0x41` = `0100 0001`), `'B'` (`0x42` = `0100 0010`) e `'3'` (`0x33` = `0011 0011`)

| Caractere (Hex / Binário) | Ordem de bits | Sequência de Símbolos | Pares $(I, Q)$ na Tabela Natural |
|:---|:---|:---|:---|
| `'A'` (`0x41` = `01000001`) | `MSB-first` | `[1, 0, 0, 1]` | `[01:-1,+1] [00:-1,-1] [00:-1,-1] [01:-1,+1]` |
| `'A'` (`0x41` = `01000001`) | `LSB-bit-first` | `[2, 0, 0, 2]` | `[10:+1,-1] [00:-1,-1] [00:-1,-1] [10:+1,-1]` |
| `'A'` (`0x41` = `01000001`) | `LSB-dibit-first` | `[1, 0, 0, 1]` | `[01:-1,+1] [00:-1,-1] [00:-1,-1] [01:-1,+1]` |
| `'B'` (`0x42` = `01000010`) | `MSB-first` | `[1, 0, 0, 2]` | `[01:-1,+1] [00:-1,-1] [00:-1,-1] [10:+1,-1]` |
| `'B'` (`0x42` = `01000010`) | `LSB-bit-first` | `[1, 0, 0, 2]` | `[01:-1,+1] [00:-1,-1] [00:-1,-1] [10:+1,-1]` |
| `'B'` (`0x42` = `01000010`) | `LSB-dibit-first` | `[2, 0, 0, 1]` | `[10:+1,-1] [00:-1,-1] [00:-1,-1] [01:-1,+1]` |
| `'3'` (`0x33` = `00110011`) | `MSB-first` | `[0, 3, 0, 3]` | `[00:-1,-1] [11:+1,+1] [00:-1,-1] [11:+1,+1]` |
| `'3'` (`0x33` = `00110011`) | `LSB-bit-first` | `[3, 0, 3, 0]` | `[11:+1,+1] [00:-1,-1] [11:+1,+1] [00:-1,-1]` |
| `'3'` (`0x33` = `00110011`) | `LSB-dibit-first` | `[3, 0, 3, 0]` | `[11:+1,+1] [00:-1,-1] [11:+1,+1] [00:-1,-1]` |

> **Nota didática para osciloscópio (`/test text 3`):** o caractere `'3'` (`0x33` = `00 11 00 11` em `MSB-first`) alterna exatamente entre o símbolo `0` (`I=-1, Q=-1`, $225^\circ$) e o símbolo `3` (`I=+1, Q=+1`, $45^\circ$), produzindo inversões puras de **$180^\circ$** a cada $10\text{ ms}$ (`100 baud`).

## 4. Vetores de Enquadramento Completo: Modo 1 vs. Modo 2

- **Teste padrão CRC-16/CCITT-FALSE:** string `"123456789"` $\rightarrow$ **`0x29B1`** (polinômio `0x1021`, inicial `0xFFFF`).

### 4.1 Mensagem `"A"` (`1 byte(s)`)

- **Modo 1 (`MSB-first`, sem preâmbulo/sync):** `4 símbolos` = `[1, 0, 0, 1]` (duração a `100 baud`: **`40.0 ms`**).
- **Modo 2 (`Quadro Completo`):** `13 bytes` $\rightarrow$ `52 símbolos` (duração a `100 baud`: **`520.0 ms`**):
  - Preâmbulo (`4B`): `00 00 33 33`
  - Sync Word (`2B`): `D3 91`
  - Cabeçalho (`4B`): `10 00 64 01` (ver=`0x1`, 4-QAM, order=`0/MSB`, baud=`100`, len=`1`)
  - Payload (`1B`): `41`
  - CRC-16 (`2B` sobre Header+Payload): **`0x3929`** (`39 29`)
  - Bytes completos do quadro: `00 00 33 33 D3 91 10 00 64 01 41 39 29`

### 4.2 Mensagem `"AB"` (`2 byte(s)`)

- **Modo 1 (`MSB-first`, sem preâmbulo/sync):** `8 símbolos` = `[1, 0, 0, 1, 1, 0, 0, 2]` (duração a `100 baud`: **`80.0 ms`**).
- **Modo 2 (`Quadro Completo`):** `14 bytes` $\rightarrow$ `56 símbolos` (duração a `100 baud`: **`560.0 ms`**):
  - Preâmbulo (`4B`): `00 00 33 33`
  - Sync Word (`2B`): `D3 91`
  - Cabeçalho (`4B`): `10 00 64 02` (ver=`0x1`, 4-QAM, order=`0/MSB`, baud=`100`, len=`2`)
  - Payload (`2B`): `41 42`
  - CRC-16 (`2B` sobre Header+Payload): **`0xBFAC`** (`BF AC`)
  - Bytes completos do quadro: `00 00 33 33 D3 91 10 00 64 02 41 42 BF AC`

### 4.3 Mensagem `"XYZ"` (`3 byte(s)`)

- **Modo 1 (`MSB-first`, sem preâmbulo/sync):** `12 símbolos` = `[1, 1, 2, 0, 1, 1, 2, 1, 1, 1, 2, 2]` (duração a `100 baud`: **`120.0 ms`**).
- **Modo 2 (`Quadro Completo`):** `15 bytes` $\rightarrow$ `60 símbolos` (duração a `100 baud`: **`600.0 ms`**):
  - Preâmbulo (`4B`): `00 00 33 33`
  - Sync Word (`2B`): `D3 91`
  - Cabeçalho (`4B`): `10 00 64 03` (ver=`0x1`, 4-QAM, order=`0/MSB`, baud=`100`, len=`3`)
  - Payload (`3B`): `58 59 5A`
  - CRC-16 (`2B` sobre Header+Payload): **`0x7544`** (`75 44`)
  - Bytes completos do quadro: `00 00 33 33 D3 91 10 00 64 03 58 59 5A 75 44`

### 4.4 Mensagem `"4QAM"` (`4 byte(s)`)

- **Modo 1 (`MSB-first`, sem preâmbulo/sync):** `16 símbolos` = `[0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 3, 1]` (duração a `100 baud`: **`160.0 ms`**).
- **Modo 2 (`Quadro Completo`):** `16 bytes` $\rightarrow$ `64 símbolos` (duração a `100 baud`: **`640.0 ms`**):
  - Preâmbulo (`4B`): `00 00 33 33`
  - Sync Word (`2B`): `D3 91`
  - Cabeçalho (`4B`): `10 00 64 04` (ver=`0x1`, 4-QAM, order=`0/MSB`, baud=`100`, len=`4`)
  - Payload (`4B`): `34 51 41 4D`
  - CRC-16 (`2B` sobre Header+Payload): **`0xF447`** (`F4 47`)
  - Bytes completos do quadro: `00 00 33 33 D3 91 10 00 64 04 34 51 41 4D F4 47`

### 4.5 Mensagem `"4QAM-OK!"` (`8 byte(s)`)

- **Modo 1 (`MSB-first`, sem preâmbulo/sync):** `32 símbolos` = `[0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 3, 1, 0, 2, 3, 1, 1, 0, 3, 3, 1, 0, 2, 3, 0, 2, 0, 1]` (duração a `100 baud`: **`320.0 ms`**).
- **Modo 2 (`Quadro Completo`):** `20 bytes` $\rightarrow$ `80 símbolos` (duração a `100 baud`: **`800.0 ms`**):
  - Preâmbulo (`4B`): `00 00 33 33`
  - Sync Word (`2B`): `D3 91`
  - Cabeçalho (`4B`): `10 00 64 08` (ver=`0x1`, 4-QAM, order=`0/MSB`, baud=`100`, len=`8`)
  - Payload (`8B`): `34 51 41 4D 2D 4F 4B 21`
  - CRC-16 (`2B` sobre Header+Payload): **`0xA2B0`** (`A2 B0`)
  - Bytes completos do quadro: `00 00 33 33 D3 91 10 00 64 08 34 51 41 4D 2D 4F 4B 21 A2 B0`

## 5. Arquivo CSV de Amostras Temporais

O arquivo [`vetores_amostras_4qam.csv`](./vetores_amostras_4qam.csv) contém **`3200` amostras** (exatamente `8 símbolos` = `80,000 ms` a `40 kHz`) da transmissão Modo 1 de `"AB"` (MSB-first, Natural, `+cos(wt-90)` / `+sin`, `fc=1000 Hz`, `Rs=100 baud`, `Vdc=1300 mV`, `Vpk=900 mV`), prontas para conferência em Python, MATLAB, GNU Radio ou comparação com captura de osciloscópio.
