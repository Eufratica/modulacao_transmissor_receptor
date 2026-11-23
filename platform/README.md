# Plataforma de Instrumentação e Monitoramento 4-QAM (ESP32)

Central web local (**FastAPI + WebSocket + pyserial + HTML5 Canvas/ES Modules**) para operar, visualizar e auditar o sistema de comunicação digital **4-QAM** entre os dois ESP32 (`transmitter/` e `receiver/`) e comparar em tempo real **Ground Truth vs. Recepção Física**.

---

## 1. Requisitos e Execução Simples (Windows)

A plataforma reutiliza integralmente o ambiente Python já instalado com o **ESP-IDF v4.4.8** (`FastAPI`, `Uvicorn`, `WebSockets`, `pyserial` e `NumPy`), **sem necessidade de internet, nuvem ou instalação de Node.js**:

```powershell
# Comando único para abrir o servidor local (http://127.0.0.1:8420) e o navegador:
.\platform\start_platform.ps1
```

Ou manualmente:
```powershell
C:\Users\gerson.marques\.espressif\python_env\idf4.4_py3.8_env\Scripts\python.exe C:\Modulador_ESP\platform\backend\app.py
```

### Executar a Suíte de Testes Automatizados (Seção 17)
```powershell
C:\Users\gerson.marques\.espressif\python_env\idf4.4_py3.8_env\Scripts\python.exe C:\Modulador_ESP\platform\tests\test_platform.py
```

---

## 2. Regra de Controle Exclusivo das Portas COM

- A plataforma abre uma porta COM dedicada para o **Transmissor (TX)** e outra para o **Receptor (RX)** simultaneamente.
- O sistema **impede** que TX e RX abram a mesma porta COM ao mesmo tempo.
- **Antes de conectar pela plataforma, feche qualquer outro monitor serial** (`idf.py monitor`, PuTTY, Arduino IDE, VS Code Serial Monitor) conectado às portas usadas pelos ESP32, caso contrário o Windows retornará porta ocupada.
- A plataforma **nunca** grava (`flash`), apaga ou altera firmware automaticamente.

---

## 3. Fluxo de Operação em Bancada

1. Grave os dois firmwares instrumentados uma única vez via ESP-IDF v4.4.8:
   ```powershell
   . C:\Users\gerson.marques\esp\v4.4.8\esp-idf\export.ps1
   idf.py -C C:\Modulador_ESP\transmitter -p COMx flash
   idf.py -C C:\Modulador_ESP\receiver    -p COMy flash
   ```
2. Ligue o estágio analógico: `GPIO 18 (TX) -> R1 (1kΩ) -> [C1 33nF para GND] -> R2 (10kΩ) -> [C2 3,3nF para GND] -> GPIO 34 (RX)`, com `GND` comum.
3. Inicie `.\platform\start_platform.ps1`. Enquanto nenhum ESP32 estiver conectado, o dashboard exibirá **`Aguardando hardware`** sem inventar curvas ou métricas.
4. Na aba **Visão Geral & Hardware**, atualize as portas COM, selecione `COMx` para **TX** e `COMy` para **RX** (`115200 bps`) e conecte ambos. A plataforma identificará automaticamente cada firmware via `/id` e ativará `/telem 1`.
5. **Atenção aos modos iniciais:** por padrão de compilação, o TX inicia em `Modo 1` (`QAM_TX_MODE_1_PROFESSOR`) e o RX inicia em `Modo 2` (`RX_MODE_2_FRAMED`). A plataforma exibe um aviso visual se TX e RX estiverem em modos divergentes — clique no atalho **`/mode 2`** no TX (ou `/mode 1` no RX) para alinhar antes de transmitir.
6. Digite uma mensagem na caixa de texto, clique em **Transmitir Texto** e acompanhe todas as abas: **Osciloscópio Digital**, **Constelação 4-QAM**, **Análise Digital & Ground Truth** (`BER/SER/EVM/CRC`), **Laboratório DSP** (`Costas Loop` + `Gardner TED bnd_i_curr`), **Analisador FFT** (com amostras reais do ADC), **Filtros RC** e **Histórico & Exportação** (`CSV`, `JSON` e relatório imprimível).

> **Modo Teste Offline:** sem placas conectadas, você pode validar todo o fluxo do parser e da interface clicando em **Teste Offline** no topo — todo dado originado por esse modo fica permanentemente marcado com o banner âmbar `MODO DE TESTE OFFLINE (SIMULAÇÃO)`, garantindo zero confusão com medições experimentais reais.
