// Laboratorio do Filtro RC Passa-Baixas 2a Ordem (Acoplamento Exato s*R1*C2)
import { getCanvasPalette } from "./a11y_theme.js?v=pwm3";

export class FiltersLabView {
  constructor() {
    this.svgWrap = document.getElementById("svg-rc-wrap");
    this.statsEl = document.getElementById("rc-stats");
    this.magCanvas = document.getElementById("canvas-bode-mag");
    this.phaseCanvas = document.getElementById("canvas-bode-phase");
  }

  renderSchematic(r1, c1, r2, c2) {
    this.svgWrap.innerHTML = `
      <svg viewBox="0 0 640 165" style="width:100%;height:auto;font-family:var(--mono);font-size:12px">
        <text x="14" y="22" fill="var(--cyan-soft)" font-weight="bold">GPIO 18 (MCPWM0A 80kHz)</text>
        <line x1="30" y1="55" x2="115" y2="55" stroke="var(--cyan-soft)" stroke-width="2.5"/>
        <rect x="115" y="42" width="76" height="26" rx="3" fill="var(--surface-sub)" stroke="var(--text-secondary)" stroke-width="2"/>
        <text x="153" y="59" text-anchor="middle" fill="var(--text-strong)">R1=${r1}Ω</text>
        <line x1="191" y1="55" x2="325" y2="55" stroke="var(--text-secondary)" stroke-width="2.5"/>
        <circle cx="255" cy="55" r="4" fill="var(--warn)"/>
        <line x1="255" y1="55" x2="255" y2="85" stroke="var(--text-secondary)" stroke-width="2"/>
        <line x1="237" y1="85" x2="273" y2="85" stroke="var(--warn)" stroke-width="3"/>
        <line x1="237" y1="95" x2="273" y2="95" stroke="var(--warn)" stroke-width="3"/>
        <line x1="255" y1="95" x2="255" y2="132" stroke="var(--text-secondary)" stroke-width="2"/>
        <text x="278" y="94" fill="var(--warn)">C1=${c1}nF</text>

        <rect x="325" y="42" width="84" height="26" rx="3" fill="var(--surface-sub)" stroke="var(--text-secondary)" stroke-width="2"/>
        <text x="367" y="59" text-anchor="middle" fill="var(--text-strong)">R2=${r2}Ω</text>
        <line x1="409" y1="55" x2="545" y2="55" stroke="var(--ok-bright)" stroke-width="2.5"/>
        <circle cx="475" cy="55" r="4" fill="var(--warn)"/>
        <line x1="475" y1="55" x2="475" y2="85" stroke="var(--text-secondary)" stroke-width="2"/>
        <line x1="457" y1="85" x2="493" y2="85" stroke="var(--warn)" stroke-width="3"/>
        <line x1="457" y1="95" x2="493" y2="95" stroke="var(--warn)" stroke-width="3"/>
        <line x1="475" y1="95" x2="475" y2="132" stroke="var(--text-secondary)" stroke-width="2"/>
        <text x="498" y="94" fill="var(--warn)">C2=${c2}nF</text>

        <text x="530" y="22" fill="var(--ok-bright)" font-weight="bold">GPIO 34 (ADC1)</text>
        <line x1="30" y1="132" x2="560" y2="132" stroke="var(--text-muted)" stroke-width="2"/>
        <text x="295" y="152" text-anchor="middle" fill="var(--text-muted)">GND Comum (TX - Filtro - RX)</text>
      </svg>
    `;
  }

  _drawBode(canvas, freqs, vals, yMin, yMax, unit, color, fc, fpwm, label) {
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    if (rect.width < 10 || rect.height < 10) return;
    canvas.width = Math.floor(rect.width * dpr);
    canvas.height = Math.floor(rect.height * dpr);
    const ctx = canvas.getContext("2d");
    ctx.scale(dpr, dpr);
    const W = rect.width, H = rect.height;
    const padL = 52, padR = 16, padT = 28, padB = 24;
    const pw = Math.max(10, W - padL - padR), ph = Math.max(10, H - padT - padB);

    const pal = getCanvasPalette();
    ctx.fillStyle = pal.bg; ctx.fillRect(0, 0, W, H);
    const f0 = Math.log10(10), f1 = Math.log10(200000);
    const mapX = (f) => padL + ((Math.log10(Math.max(10, f)) - f0) / (f1 - f0)) * pw;
    const mapY = (v) => padT + (1.0 - (v - yMin) / (yMax - yMin)) * ph;

    ctx.strokeStyle = pal.grid; ctx.font = "10px Consolas, monospace"; ctx.fillStyle = pal.axis;
    for (let p = 1; p <= 5; p++) {
      const fx = Math.pow(10, p);
      const x = mapX(fx);
      ctx.beginPath(); ctx.moveTo(x, padT); ctx.lineTo(x, padT + ph); ctx.stroke();
      ctx.textAlign = "center"; ctx.fillText(fx >= 1000 ? `${fx / 1000}k` : `${fx}`, x, padT + ph + 14);
    }
    for (let g = 0; g <= 4; g++) {
      const yv = yMin + (g / 4) * (yMax - yMin);
      const y = mapY(yv);
      ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(padL + pw, y); ctx.stroke();
      ctx.textAlign = "right"; ctx.fillText(`${yv.toFixed(0)}${unit}`, padL - 5, y + 3);
    }

    // Marcadores fc e fpwm posicionados sem colisao com o titulo
    const fcX = mapX(fc);
    ctx.strokeStyle = "#22c55e"; ctx.setLineDash([3, 3]);
    ctx.beginPath(); ctx.moveTo(fcX, padT); ctx.lineTo(fcX, padT + ph); ctx.stroke();
    ctx.setLineDash([]); ctx.fillStyle = "#22c55e"; ctx.textAlign = "left";
    ctx.fillText(`fc=${fc}Hz`, Math.min(W - 86, fcX + 5), padT + ph - 8);

    const pwmX = mapX(fpwm);
    ctx.strokeStyle = "#ef4444"; ctx.setLineDash([3, 3]);
    ctx.beginPath(); ctx.moveTo(pwmX, padT); ctx.lineTo(pwmX, padT + ph); ctx.stroke();
    ctx.setLineDash([]); ctx.fillStyle = "#ef4444"; ctx.textAlign = "right";
    ctx.fillText(`PWM=${(fpwm / 1000).toFixed(0)}k`, Math.max(padL + 60, pwmX - 5), padT + 14);

    ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.beginPath();
    freqs.forEach((f, i) => {
      const x = mapX(f), y = mapY(Math.max(yMin, Math.min(yMax, vals[i])));
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();

    ctx.fillStyle = pal.textStrong; ctx.textAlign = "left";
    ctx.font = "bold 10.5px Consolas, monospace";
    ctx.fillText(label, padL, 16);
  }

  render(model) {
    if (!model) return;
    this.renderSchematic(model.r1_ohm, model.c1_nf, model.r2_ohm, model.c2_nf);
    this.statsEl.innerHTML = `
      <div class="kv"><span class="kv-k">Tipo da Curva</span><span class="kv-v" style="color:#fbbf24">TEÓRICA CALCULADA</span></div>
      <div class="kv"><span class="kv-k">Frequência -3 dB</span><span class="kv-v">${model.f_3db_hz} Hz</span></div>
      <div class="kv"><span class="kv-k">Ganho na Portadora (fc)</span><span class="kv-v">${model.gain_fc_db} dB (${model.phase_fc_deg}°)</span></div>
      <div class="kv"><span class="kv-k">Atenuação em f_PWM</span><span class="kv-v">${model.attenuation_pwm_db} dB</span></div>
      <div class="kv"><span class="kv-k">Polos Reais Acoplados</span><span class="kv-v">${model.poles_hz[0]} Hz / ${model.poles_hz[1]} Hz</span></div>
      <div class="kv"><span class="kv-k">Termo a1 = R1C1+R2C2+R1C2</span><span class="kv-v">${model.a1_us} us (Q=${model.q_factor})</span></div>
    `;
    const fc = parseFloat(document.getElementById("rc-fc").value) || 1000;
    const fpwm = parseFloat(document.getElementById("rc-fpwm").value) || 80000;
    this._drawBode(this.magCanvas, model.freqs_hz, model.mag_db, -70, 5, "dB", "#38bdf8", fc, fpwm, "Magnitude |H(jw)| [Teórico]");
    this._drawBode(this.phaseCanvas, model.freqs_hz, model.phase_deg, -180, 0, "°", "#a78bfa", fc, fpwm, "Fase ∠H(jw) [Teórico]");
  }
}
