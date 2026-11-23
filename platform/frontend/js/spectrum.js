// Analisador de Espectro FFT (utiliza exclusivamente dados adquiridos no ADC RX)
import { getCanvasPalette } from "./a11y_theme.js?v=pwm3";

export class SpectrumView {
  constructor(canvasId, metricsId) {
    this.canvas = document.getElementById(canvasId);
    this.metricsEl = document.getElementById(metricsId);
    this.lastFft = null;
  }

  render(fftRes, expectedFc = 1000) {
    this.lastFft = fftRes;
    const dpr = window.devicePixelRatio || 1;
    const rect = this.canvas.getBoundingClientRect();
    if (rect.width < 10 || rect.height < 10) return;
    this.canvas.width = Math.floor(rect.width * dpr);
    this.canvas.height = Math.floor(rect.height * dpr);
    const ctx = this.canvas.getContext("2d");
    ctx.scale(dpr, dpr);

    const W = rect.width;
    const H = rect.height;
    const padL = 54, padR = 16, padT = 18, padB = 28;
    const pw = Math.max(10, W - padL - padR);
    const ph = Math.max(10, H - padT - padB);

    const pal = getCanvasPalette();
    ctx.fillStyle = pal.bg;
    ctx.fillRect(0, 0, W, H);

    if (!fftRes || !fftRes.valid) {
      ctx.fillStyle = pal.axis;
      ctx.font = "12.5px Consolas, monospace";
      ctx.textAlign = "center";
      ctx.fillText(fftRes?.reason || "Sem amostras reais do ADC1 para calcular FFT.", W / 2, H / 2);
      this.metricsEl.textContent = fftRes?.reason || "Aguardando amostras reais.";
      return;
    }

    const fMaxZoom = Math.min(fftRes.sample_rate_hz / 2, parseFloat(document.getElementById("inp-fft-fmax").value) || 5000);
    const yMin = -90, yMax = 5;
    const mapX = (f) => padL + (f / Math.max(10, fMaxZoom)) * pw;
    const mapY = (db) => padT + (1.0 - (Math.max(yMin, Math.min(yMax, db)) - yMin) / (yMax - yMin)) * ph;

    // Grade
    ctx.strokeStyle = pal.grid;
    ctx.font = "10.5px Consolas, monospace";
    ctx.fillStyle = pal.axis;
    for (let db = -80; db <= 0; db += 20) {
      const y = mapY(db);
      ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(padL + pw, y); ctx.stroke();
      ctx.textAlign = "right";
      ctx.fillText(`${db} dBV`, padL - 5, y + 3);
    }
    for (let g = 0; g <= 5; g++) {
      const f = (g / 5) * fMaxZoom;
      const x = mapX(f);
      ctx.beginPath(); ctx.moveTo(x, padT); ctx.lineTo(x, padT + ph); ctx.stroke();
      ctx.textAlign = "center";
      ctx.fillText(`${f.toFixed(0)} Hz`, x, padT + ph + 16);
    }

    // Linha fc esperada
    if (expectedFc <= fMaxZoom) {
      const xFc = mapX(expectedFc);
      ctx.strokeStyle = "#f59e0b";
      ctx.setLineDash([4, 4]);
      ctx.beginPath(); ctx.moveTo(xFc, padT); ctx.lineTo(xFc, padT + ph); ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = "#fbbf24";
      ctx.textAlign = "left";
      ctx.fillText(`fc esp=${expectedFc} Hz`, xFc + 4, padT + 14);
    }

    // Ruído de fundo estimado
    const yFloor = mapY(fftRes.noise_floor_dbv);
    ctx.strokeStyle = "#475569";
    ctx.setLineDash([3, 3]);
    ctx.beginPath(); ctx.moveTo(padL, yFloor); ctx.lineTo(padL + pw, yFloor); ctx.stroke();
    ctx.setLineDash([]);

    // Espectro real
    ctx.strokeStyle = "#22c55e";
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    let started = false;
    for (let i = 0; i < fftRes.freqs_hz.length; i++) {
      const f = fftRes.freqs_hz[i];
      if (f > fMaxZoom) break;
      const x = mapX(f);
      const y = mapY(fftRes.mag_dbv[i]);
      if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
    }
    ctx.stroke();

    // Pico detectado
    if (fftRes.peak_freq_hz <= fMaxZoom) {
      const xp = mapX(fftRes.peak_freq_hz);
      const yp = mapY(fftRes.peak_mag_dbv);
      ctx.fillStyle = "#38bdf8";
      ctx.beginPath(); ctx.arc(xp, yp, 4, 0, Math.PI * 2); ctx.fill();
      ctx.fillText(`Pico=${fftRes.peak_freq_hz}Hz (${fftRes.peak_mag_dbv}dBV)`, Math.min(W - 140, xp + 6), Math.max(padT + 24, yp));
    }

    const harmTxt = (fftRes.harmonics || []).map((h) => `${h.order}f:${h.freq_hz}Hz(${h.mag_dbv}dBV)`).join(" | ");
    this.metricsEl.textContent =
      `N=${fftRes.fft_size} | Janela=${fftRes.window} | df=${fftRes.df_hz} Hz/bin | Pico=${fftRes.peak_freq_hz} Hz (${fftRes.peak_mag_mv} mVpk) | ` +
      `Piso=${fftRes.noise_floor_dbv} dBV | SNR_esp=${fftRes.snr_spectral_db} dB | BW(-6dB)=${fftRes.bw_6db_hz} Hz` +
      (harmTxt ? ` | Harm: ${harmTxt}` : "");
  }
}
