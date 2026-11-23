# Especificação de Interoperabilidade (TX/RX Próprios e Externos)

Este documento estabelece exatamente o que precisa ser combinado previamente entre transmissor e receptor e o que pode ser estimado automaticamente no ar, atendendo cenários onde o **transmissor conversa com o receptor do professor** ou o **receptor analisa o sinal de um modulador externo**.

---

## 1. Matriz de Dependência: *A Priori* vs. *Recuperado Automaticamente*

| Parâmetro | Deve ser conhecido *a priori*? | Comportamento no ESP32 RX (deste projeto) | Comportamento típico em um RX externo |
|---|:---:|---|---|
| **Frequência da portadora ($f_c$)** | **Parcial** *(nominal)* | NCO inicia em `fc` nominal (`1000 Hz`) e rastreia desvios até **`±35 Hz`** via Costas Loop | Precisa conhecer `fc` nominal ou fazer varredura espectral (FFT) |
| **Taxa de símbolos ($R_s$)** | **Sim no Modo 1** | No Modo 1 usa `Rs` configurado + Gardner até **`±2%`**. No Modo 2, `tx_baud` vem também no cabeçalho | RX externo sem cabeçalho precisa conhecer `Rs` nominal |
| **Tabela de constelação** | **Sim** | Configurável em tempo real: `/map natural` *(padrão)* ou `/map gray` | Precisa usar exatamente os mesmos rótulos de bits por quadrante |
| **Convenção de quadratura (`±sin`)** | **Sim** | Configurável em tempo real: `/qsign plus` (`+cos(wt-90)` = `+sin`, *padrão*) ou `/qsign minus` (`-sin`) | Trocar o sinal espelha o eixo $Q$; deve casar TX e RX |
| **Ordem dos bits no byte** | **Sim no Modo 1** / **Não no Modo 2** | Modo 1 exige `/order <msb\|lsb_bit\|lsb_dibit>`. Modo 2 informa a ordem no byte `header[0]` | Modo 1 permite casar qualquer convenção do RX externo (`msb`, `lsb_bit`, `lsb_dibit`) |
| **Enquadramento / Preâmbulo / Sync** | **Sim** | Modo 1 emite/recebe fluxo bruto; Modo 2 usa `00 00 33 33` + `D3 91` + Header + CRC-16 | Use **Modo 1** ao conectar com RX que lê símbolos/bytes diretamente sem cabeçalho |
| **Offset DC analógico ($V_{DC}$)** | **Não** | Removido adaptativamente por média móvel exponencial (`alpha = 0,005` / `0,001`) | Se RX externo for sensível a DC diferente de $V_{DD}/2$, ajuste `/offset` ou use acoplamento AC (`C = 1 uF`) |
| **Amplitude de pico ($V_{pk}$) / Ganho** | **Não** | Normalizada continuamente por AGC baseado em potência RMS (`1,4142 * rms_mv`) | Caso RX externo espere amplitude específica sem AGC, ajuste `/amp <mv>` (`Vpk >= 50 mV`, respeitando $V_{DC} \ge V_{pk}+50$ e $V_{DC}+V_{pk} \le V_{DD}-50$) |
| **Fase inicial da portadora ($\theta_0$)** | **Não** *(c/ Sync)* | Costas Loop trava automaticamente o módulo de $90^\circ$; Modo 2 e `/sync1 1` resolvem `0/90/180/270°` | Sem palavra de sincronismo no Modo 1, pode ocorrer rotação fixa de múltiplo de $90^\circ$ (corrigível com `/rot`) |
| **Fronteira inicial do símbolo ($\tau_0$)** | **Não** | `Gardner TED` encontra o centro e alinha a janela de integração `15% .. 85%` | Exige que a taxa $R_s$ contenha transições (Modo 2 garante via `0x33 0x33` no preâmbulo) |
| **Polaridade elétrica** | **Não** *(c/ Sync)* | `/pol <norm\|inv>` disponível; no Modo 2, `180°` é indistinguível de rotação `2` e resolvido pela `Sync Word` | Ajustável via `/pol inv` no TX se houver etapa inversora no circuito analógico |

---

## 2. Receitas Rápidas de Adaptação na Bancada (Sem Recompilar)

Todos os ajustes abaixo são executados diretamente pelo terminal serial `UART0` (`115200 bps`):

### Cenário A — Comunicar entre os dois ESP32 do projeto (Modo Completo)
Ambos iniciam prontos nesse modo após o boot (basta conectar `GPIO 18 -> Filtro RC -> GPIO 34` e `GND <-> GND`):
- **TX:** Modo `2`, `fc=1000 Hz`, `Rs=100 baud`, `Natural`, `+cos(wt-90)`/`+sin`, `MSB-first`, `DC=1300 mV`, `Vpk=900 mV`.
- **RX:** `/send Mensagem` ou digite a mensagem diretamente no prompt do TX.

### Cenário B — Enviar texto do ESP32 TX para o Receptor do Professor (Modo 1)
No terminal do **TX**, ajuste apenas o necessário para espelhar a especificação esperada pelo professor:
```text
/mode 1
/baud <valor_esperado>       # Ex.: /baud 100 ou /baud 50
/fc <valor_esperado>         # Ex.: /fc 1000
/map <natural|gray>          # Padrao ja e natural (0:-1,-1 | 1:-1,+1 | 2:+1,-1 | 3:+1,+1)
/qsign <plus|minus>          # plus = I*cos + Q*cos(wt-90) | minus = I*cos - Q*sin
/order <msb|lsb_bit|lsb_dibit>
/pre1 <0|1>                  # 0 = zero bytes extras (padrao) | 1 = adiciona preambulo
/idle <dc|pilot>             # dc = silencio DC em 1300 mV | pilot = portadora continua simbolo 00
/send Texto de teste
```
> **Dica de bancada:** se o receptor externo não possuir recuperação autônoma de fase em rajadas curtas, ligue temporariamente `/idle pilot` para manter uma portadora contínua de símbolo `00` entre mensagens, permitindo que a PLL externa trave antes da transmissão.

### Cenário C — Usar o ESP32 RX para inspecionar um Transmissor Externo
No terminal do **RX**:
```text
/mode 1                      # Se o TX externo envia ASCII direto sem cabecalho deste projeto
/fc <fc_tx_externo>
/baud <baud_tx_externo>
/map <natural|gray>
/qsign <plus|minus>
/order <msb|lsb_bit|lsb_dibit>
/sync1 <0|1>                 # 0 se nao houver Sync Word; use /rot <0|90|180|270> se girar quadrante
/monitor 1                   # Exibe (I, Q), SNR estimada e desvio df em tempo real
```

---

## 3. Pontos de Atenção para Evitar Falhas de Interoperabilidade

1. **Divergência entre `+sin` e `-sin`:** trocar o sinal do termo em quadratura não muda a amplitude nem o espectro, mas inverte o sinal de $Q$ no plano I/Q (`01 <-> 00` e `11 <-> 10` na tabela Natural). Use `/qsign minus` se o código de referência externo implementar $I\cos - Q\sin$.
2. **Ordem de bits dentro do byte (`LSB-bit` vs `LSB-dibit` vs `MSB`):** em `'B'` (`0x42`), `MSB-first` e `LSB-bit-first` coincidentemente produzem `[1, 0, 0, 2]`, mas em `'A'` (`0x41`) divergem (`[1, 0, 0, 1]` vs `[2, 0, 0, 2]`). Sempre teste com `'A'` ou `"AB"`, nunca apenas com caracteres simétricos!
3. **Resolução de $90^\circ$ sem Sync Word:** no **Modo 1 sem sync**, o Costas Loop pode travar em qualquer múltiplo de $90^\circ$ dependendo da fase do instante em que o sinal começou. Habilite `/pre1 1` + `/sync1 1` (se combinado entre TX/RX) ou teste os 4 passos `/rot 0`, `/rot 90`, `/rot 180`, `/rot 270` no RX em transmissões contínuas (`/mode 3`).
4. **Diferença de nível DC com receptor externo:** o circuito padrão entrega $V_{DC} = 1{,}30\text{ V}$ ideal para o ADC do ESP32 (`150 .. 2450 mV`). Se um receptor de bancada exigir $0\text{ V}$ DC ou $1{,}65\text{ V}$ DC, escolha entre alterar `/offset 1650` (respeitando $V_{pk}$) ou inserir o capacitor de bloqueio DC em série descrito em [`circuito_analogico.md`](./circuito_analogico.md).
