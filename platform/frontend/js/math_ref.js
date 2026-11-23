// Fundamentos Matematicos e Referencia Exata aos Firmwares C

export function renderMathReference(containerId, state) {
  const el = document.getElementById(containerId);
  const tx = state?.tx_status;
  const rx = state?.rx_status;

  const fc = tx?.carrier_freq_hz ?? rx?.carrier_freq_hz ?? 1000;
  const baud = tx?.symbol_rate_baud ?? rx?.symbol_rate_baud ?? 100;
  const fsTx = tx?.sample_rate_hz ?? 40000;
  const fsRx = rx?.sample_rate_hz ?? 20000;
  const amp = tx?.amplitude_peak_mv ?? 900;
  const aEq = (amp / Math.sqrt(2)).toFixed(2);
  const qMinus = (tx?.q_sign_minus ?? rx?.q_sign_minus) ? true : false;
  const mapPreset = tx?.map_preset || rx?.map_preset || "natural";

  const cards = [
    {
      title: "1. Modulação 4-QAM em Banda Passante",
      formula: qMinus
        ? "s(t) = A · [ I_k·cos(2π·f_c·t) - Q_k·sin(2π·f_c·t) ]   (/qsign minus)"
        : "s(t) = A · [ I_k·cos(2π·f_c·t) + Q_k·cos(2π·f_c·t - 90°) ] = A · [ I_k·cos(2π·f_c·t) + Q_k·sin(2π·f_c·t) ]",
      vars: "A = V_pk / √2 garante envoltória constante igual a V_pk; I_k, Q_k ∈ {-1, +1}; k = índice do símbolo; f_c = frequência da portadora; t = tempo contínuo.",
      app: `Valores ativos: f_c = ${fc} Hz | V_pk = ${amp} mV → A = ${aEq} mV | Convenção = ${qMinus ? "-sin" : "+cos(wt-90) [+sin]"}.`,
      ref: "transmitter/main/qam_modulator.c:130-169 · receiver/main/qam_demod.c:185-190",
    },
    {
      title: "2. Tabela de Mapeamento Ativa (Natural vs. Gray)",
      formula: mapPreset === "gray"
        ? "Gray ativo: 0(00)=(+1,+1) | 1(01)=(-1,+1) | 2(10)=(+1,-1) | 3(11)=(-1,-1)"
        : "Natural ativo: 0(00)=(-1,-1) | 1(01)=(-1,+1) | 2(10)=(+1,-1) | 3(11)=(+1,+1)",
      vars: "Cada par de bits seleciona um dos 4 quadrantes I/Q (espaçamento angular de 90°). Pode ser alternada sem recompilar via /map natural|gray.",
      app: `Preset atual refletido pelo sistema: ${mapPreset.toUpperCase()} | Taxa = 2 bits/símbolo (${baud * 2} bps a ${baud} baud).`,
      ref: "transmitter/main/qam_config.c:20-49 · receiver/main/rx_config.c:6-27",
    },
    {
      title: "3. Amostragem e Síntese PWM Determinística",
      formula: "f_s = 1 / T_s   |   f_PWM = f_clk / (2 · period_ticks)",
      vars: "f_s TX = taxa de atualização da LUT (40 kHz = 40 amostras/ciclo); f_PWM = 80 kHz simétrico UP-DOWN em 160 MHz (1000 níveis); f_s RX = ADC1 DMA a 20 kHz.",
      app: `Valores ativos: TX f_s = ${fsTx} Hz | RX f_s = ${fsRx} Hz (${(fsRx / fc).toFixed(1)} amostras/ciclo de portadora).`,
      ref: "transmitter/main/pwm_driver.c:37-191 · receiver/main/adc_sampler.c:42-99",
    },
    {
      title: "4. Filtro Passa-Baixas RC 2ª Ordem (Acoplamento Completo)",
      formula: "H(s) = 1 / [ 1 + s·(R1·C1 + R2·C2 + R1·C2) + s²·(R1·R2·C1·C2) ]",
      vars: "Inclui explicitamente o termo cruzado s·R1·C2 (não trata estágios como independentes). Com 1 kΩ / 33 nF e 10 kΩ / 3,3 nF: f_-3dB ≈ 1,73 kHz, ganho em 1 kHz = -0,44 dB, atenuação em 80 kHz = -48,83 dB.",
      app: "Suprime a portadora de chaveamento PWM (80 kHz) preservando banda útil de 4-QAM (900..1100 Hz).",
      ref: "docs/circuito_analogico.md · platform/backend/dsp_analysis.py (compute_rc2_bode)",
    },
    {
      title: "5. Costas Loop PI & Filtro Boxcar (Nulo Exato em 2f_c)",
      formula: qMinus
        ? "ped = sgn(Iu)·Qu - sgn(Qu)·Iu  |  θ[k+1] = θ[k] + Kp·ped + ω_i"
        : "ped = sgn(Qu)·Iu - sgn(Iu)·Qu  |  θ[k+1] = θ[k] + Kp·ped + ω_i",
      vars: "Boxcar N = round(fs/fc) = 20 amostras zera exatamente 2f_c. Ganhos atuais: Kp = 0,16, Ki = 0,017, faixa = ±35 Hz.",
      app: `Desvio de frequência estimado atual no RX: ${rx?.freq_offset_hz !== undefined ? rx.freq_offset_hz.toFixed(2) + " Hz" : "aguardando sinal"}.`,
      ref: "receiver/main/qam_demod.c:192-235 · docs/sincronizacao.md",
    },
    {
      title: "6. Gardner TED (Índice de Fronteira Corrigido bnd_i_curr)",
      formula: "e_τ = (mid_i_curr - mid_i_prev)·bnd_i_curr + (mid_q_curr - mid_q_prev)·bnd_q_curr",
      vars: "mid_*_prev = centro do símbolo k-1; bnd_*_curr = amostra da fronteira k-1|k (em sym_phase=0); mid_*_curr = centro do símbolo k; Kp=0,030, Ki=0,0015 (±2%); integração em 15%..85%.",
      app: "Recupera fronteira exata do símbolo mesmo com deriva de cristal e deslocamento fracionário inicial.",
      ref: "receiver/main/qam_demod.c:242-304",
    },
    {
      title: "7. Análise Espectral FFT & Integridade (BER, SER, EVM, CRC-16)",
      formula: "BER = bits_err / bits_tot | SER = sim_err / sim_tot | EVM_RMS = √(Σ|z_k - s_k|² / Σ|s_k|²)·100%",
      vars: "FFT discreta (X[k] = Σ x[n]·w[n]·e^(-j2πkn/N)) com resolução Δf = f_s/N. CRC-16/CCITT-FALSE: polinômio 0x1021, init 0xFFFF ('123456789' -> 0x29B1).",
      app: "BER/SER/EVM exigem alinhamento válido TX↔RX; CRC OK verifica integridade do bloco [Header+Payload] sem provar BER zero por si só.",
      ref: "platform/backend/dsp_analysis.py · transmitter/main/qam_mapper.c:106-124 · receiver/main/rx_deframer.c",
    },
  ];

  el.innerHTML = cards.map((c) => `
    <article class="math-card">
      <h3>${c.title}</h3>
      <div class="math-formula">${c.formula}</div>
      <p class="hint"><strong>Variáveis:</strong> ${c.vars}</p>
      <p class="hint"><strong>No sistema:</strong> ${c.app}</p>
      <p class="meta-line">Referência exata: <code>${c.ref}</code></p>
    </article>
  `).join("");
}
