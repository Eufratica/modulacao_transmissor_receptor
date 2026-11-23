// Diagrama de Constelacao 4-QAM (Canvas 2D)
// Adapta automaticamente os rotulos a tabela ativa no firmware (Natural vs Gray).
import { getCanvasPalette } from "./a11y_theme.js?v=pwm3";

const TABLES = {
  natural: [
    { sym: 0, bits: "00", i: -1, q: -1, deg: 225 },
    { sym: 1, bits: "01", i: -1, q: +1, deg: 135 },
    { sym: 2, bits: "10", i: +1, q: -1, deg: 315 },
    { sym: 3, bits: "11", i: +1, q: +1, deg: 45 },
  ],
  gray: [
    { sym: 0, bits: "00", i: +1, q: +1, deg: 45 },
    { sym: 1, bits: "01", i: -1, q: +1, deg: 135 },
    { sym: 2, bits: "10", i: +1, q: -1, deg: 315 },
    { sym: 3, bits: "11", i: -1, q: -1, deg: 225 },
  ],
};

function rotatePoint(i, q, rotQuad) {
  const r = rotQuad & 3;
  if (r === 1) return [-q, i];
  if (r === 2) return [-i, -q];
  if (r === 3) return [q, -i];
  return [i, q];
}

export class ConstellationView {
  constructor(canvasId, infoId, tableBodySel) {
    this.canvas = document.getElementById(canvasId);
    this.infoEl = document.getElementById(infoId);
    this.tbody = document.querySelector(tableBodySel);
    this.highlightSymIdx = null;
  }

  setHighlightSymbol(symIdx) {
    this.highlightSymIdx = symIdx;
  }

  render(state) {
    const mapPreset = state?.rx_status?.map_preset || state?.tx_status?.map_preset || "natural";
    const tbl = TABLES[mapPreset] || TABLES.natural;

    this.tbody.innerHTML = tbl.map((p) => (
      `<tr><td>${p.sym}</td><td><code>${p.bits}</code></td><td>${p.i > 0 ? "+1" : "-1"}</td><td>${p.q > 0 ? "+1" : "-1"}</td><td>${p.deg}°</td></tr>`
    )).join("");

    const dpr = window.devicePixelRatio || 1;
    const rect = this.canvas.getBoundingClientRect();
    if (rect.width < 10 || rect.height < 10) return;
    this.canvas.width = Math.floor(rect.width * dpr);
    this.canvas.height = Math.floor(rect.height * dpr);
    const ctx = this.canvas.getContext("2d");
    ctx.scale(dpr, dpr);

    const W = rect.width;
    const H = rect.height;
    const cx = W / 2;
    const cy = H / 2;
    const scale = Math.min(W, H) * 0.34; // +-1.0 -> +-scale

    const pal = getCanvasPalette();
    ctx.fillStyle = pal.bg;
    ctx.fillRect(0, 0, W, H);

    // Regiões de decisão e eixos
    ctx.strokeStyle = pal.grid;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(cx, cy, scale * 0.7071, 0, Math.PI * 2); // Raio ideal normalizado AGC (1/sqrt(2))
    ctx.stroke();

    ctx.strokeStyle = pal.zero;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(0, cy); ctx.lineTo(W, cy);
    ctx.moveTo(cx, 0); ctx.lineTo(cx, H);
    ctx.stroke();
    ctx.setLineDash([]);

    ctx.fillStyle = pal.axis;
    ctx.font = "11px Consolas, monospace";
    ctx.fillText("I (Fase cos)", W - 88, cy - 6);
    ctx.fillText("Q (+cos(wt-90) / +-sin)", cx + 6, 16);

    // Pontos ideais esperados (normalizados em 1/sqrt(2) conforme AGC do receptor)
    const idealR = 0.70710678;
    for (const pt of tbl) {
      const px = cx + pt.i * idealR * scale;
      const py = cy - pt.q * idealR * scale;
      ctx.strokeStyle = pal.accentWarn;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(px, py, 9, 0, Math.PI * 2);
      ctx.stroke();
      ctx.fillStyle = pal.accentWarn;
      ctx.font = "bold 11px Consolas, monospace";
      ctx.fillText(`S${pt.sym} (${pt.bits})`, px + 12, py - 8);
    }

    const syms = state?.dsp_symbols || [];
    const stage = document.getElementById("sel-const-stage").value;

    if (syms.length === 0) {
      ctx.fillStyle = pal.axis;
      ctx.font = "12px Consolas, monospace";
      ctx.textAlign = "center";
      ctx.fillText("Aguardando símbolos reais do DSP RX (nunca gera nuvens artificiais)", cx, H - 18);
      this.infoEl.innerHTML = `<div class="kv"><span class="kv-k">Mapeamento ativo</span><span class="kv-v">${mapPreset.toUpperCase()}</span></div>
        <div class="kv"><span class="kv-k">Pontos reais</span><span class="kv-v">0 (aguardando)</span></div>`;
      return;
    }

    let dispSum = 0;
    const recent = syms.slice(-240);
    recent.forEach((s, idx) => {
      let ix = s.i_hat;
      let qx = s.q_hat;
      if (stage === "final") {
        [ix, qx] = rotatePoint(s.i_hat, s.q_hat, s.rot_quad || 0);
      } else if (stage === "mid") {
        [ix, qx] = rotatePoint(s.i_mid, s.q_mid, s.rot_quad || 0);
      }

      const px = cx + ix * scale;
      const py = cy - qx * scale;
      const isHi = (this.highlightSymIdx !== null && s.sym_idx === this.highlightSymIdx);

      const errI = Math.abs(ix) - idealR;
      const errQ = Math.abs(qx) - idealR;
      dispSum += Math.sqrt(errI * errI + errQ * errQ);

      ctx.fillStyle = isHi ? "#38bdf8" : (s.sync_locked ? "rgba(34,197,94,0.85)" : "rgba(167,139,250,0.65)");
      ctx.beginPath();
      ctx.arc(px, py, isHi ? 6 : 3.2, 0, Math.PI * 2);
      ctx.fill();
    });

    const lastSym = recent[recent.length - 1];
    const dispMean = dispSum / recent.length;
    this.infoEl.innerHTML = `
      <div class="kv"><span class="kv-k">Mapeamento Ativo</span><span class="kv-v">${mapPreset.toUpperCase()}</span></div>
      <div class="kv"><span class="kv-k">Símbolos Reais Plotados</span><span class="kv-v">${recent.length}</span></div>
      <div class="kv"><span class="kv-k">Rotação Aplicada</span><span class="kv-v">${lastSym.rot_deg ?? 0}°</span></div>
      <div class="kv"><span class="kv-k">Dispersão Vetorial Média</span><span class="kv-v">${dispMean.toFixed(4)}</span></div>
      <div class="kv"><span class="kv-k">Offset DC / Vpk RX</span><span class="kv-v">${(state.rx_status?.dc_est_mv ?? 0).toFixed(0)} mV / ${(state.rx_status?.amp_pk_est_mv ?? 0).toFixed(0)} mV</span></div>
      <div class="kv"><span class="kv-k">Desvio Portadora Costas</span><span class="kv-v">${(lastSym.df_hz ?? 0).toFixed(2)} Hz</span></div>
    `;
  }
}
