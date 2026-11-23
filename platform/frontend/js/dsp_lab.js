// Laboratorio DSP: Costas Loop, Gardner TED (com bnd_i_curr), Sync Word e CRC-16
import { getCanvasPalette } from "./a11y_theme.js?v=pwm3";

export class DspLabView {
  constructor() {
    this.costasCanvas = document.getElementById("canvas-costas");
    this.gardnerCanvas = document.getElementById("canvas-gardner");
    this.costasEqEl = document.getElementById("costas-eq-box");
    this.syncBody = document.querySelector("#tbl-sync-events tbody");
    this.crcEl = document.getElementById("dsp-crc-panel");
  }

  _drawSeries(canvas, syms, seriesA, seriesB, labelA, labelB, colorA, colorB, minY, maxY) {
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    if (rect.width < 10 || rect.height < 10) return;
    canvas.width = Math.floor(rect.width * dpr);
    canvas.height = Math.floor(rect.height * dpr);
    const ctx = canvas.getContext("2d");
    ctx.scale(dpr, dpr);
    const W = rect.width, H = rect.height;
    const padL = 46, padR = 12, padT = 18, padB = 22;
    const pw = Math.max(10, W - padL - padR), ph = Math.max(10, H - padT - padB);

    const pal = getCanvasPalette();
    ctx.fillStyle = pal.bg; ctx.fillRect(0, 0, W, H);
    if (!syms || syms.length === 0) {
      ctx.fillStyle = pal.axis; ctx.font = "11.5px Consolas, monospace"; ctx.textAlign = "center";
      ctx.fillText("Aguardando símbolos reais do RX DSP...", W / 2, H / 2);
      return;
    }

    const n = syms.length;
    const mapX = (i) => padL + (i / Math.max(1, n - 1)) * pw;
    const mapY = (v) => padT + (1.0 - (Math.max(minY, Math.min(maxY, v)) - minY) / (maxY - minY)) * ph;

    ctx.strokeStyle = pal.grid; ctx.font = "10px Consolas, monospace"; ctx.fillStyle = pal.axis;
    [minY, 0, maxY].forEach((v) => {
      const y = mapY(v);
      ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(padL + pw, y); ctx.stroke();
      ctx.textAlign = "right"; ctx.fillText(v.toFixed(1), padL - 4, y + 3);
    });

    const plotLine = (arr, col) => {
      ctx.strokeStyle = col; ctx.lineWidth = 1.6; ctx.beginPath();
      arr.forEach((v, i) => {
        const x = mapX(i), y = mapY(v);
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      });
      ctx.stroke();
    };
    plotLine(seriesA, colorA);
    if (seriesB) plotLine(seriesB, colorB);

    ctx.textAlign = "left";
    ctx.fillStyle = colorA; ctx.fillText(labelA, padL + 4, padT + 11);
    if (seriesB) {
      ctx.fillStyle = colorB; ctx.fillText(labelB, padL + 150, padT + 11);
    }
  }

  render(state) {
    const qMinus = state?.rx_status?.q_sign_minus;
    this.costasEqEl.innerHTML = qMinus
      ? `<code>ped = (sgn(Iu) * Qu) - (sgn(Qu) * Iu)</code> (convenção <b>-sin</b>)<br/>Atualiza 1x/ciclo (boxcar N=20): Kp=0,16 | Ki=0,017 | Faixa=±35 Hz`
      : `<code>ped = (sgn(Qu) * Iu) - (sgn(Iu) * Qu)</code> (convenção <b>+cos(wt-90) = +sin</b>)<br/>Atualiza 1x/ciclo (boxcar N=20): Kp=0,16 | Ki=0,017 | Faixa=±35 Hz`;

    const syms = (state?.dsp_symbols || []).slice(-180);
    this._drawSeries(
      this.costasCanvas, syms,
      syms.map((s) => s.df_hz),
      syms.map((s) => s.costas_ped),
      "df estimado (Hz)", "PED (erro de fase)",
      "#38bdf8", "#a78bfa", -5, 5
    );
    this._drawSeries(
      this.gardnerCanvas, syms,
      syms.map((s) => (s.sym_rate_corr_ppm || 0) / 1000.0),
      syms.map((s) => s.gardner_err || 0),
      "Corr. taxa (% x10)", "e_tau Gardner",
      "#22c55e", "#f59e0b", -2, 2
    );

    const events = (state?.sync_events || []).slice().reverse().slice(0, 30);
    this.syncBody.innerHTML = events.length
      ? events.map((ev) => {
          const dt = new Date((ev.ts || 0) * 1000).toLocaleTimeString();
          const kind = ev.event === "rx_burst" ? `Rajada ${ev.burst_kind}` : "Quadro/Sync";
          const crcTxt = ev.crc_ok === undefined ? "—" : (ev.crc_ok ? "OK" : "ERRO");
          return `<tr>
            <td>${dt}</td><td>${kind}</td><td>${ev.burst_id ?? "—"}</td>
            <td>${ev.locked_rot_deg !== undefined ? ev.locked_rot_deg + "°" : "—"}</td>
            <td>${ev.df_est_hz !== undefined ? ev.df_est_hz.toFixed(2) : "—"}</td>
            <td>${ev.tx_baud ?? "—"}</td><td>${crcTxt}</td>
          </tr>`;
        }).join("")
      : `<tr><td colspan="7">Nenhum evento de sincronização recebido ainda.</td></tr>`;

    const rxF = state?.last_rx_frame;
    const txF = state?.last_tx_frame;
    this.crcEl.innerHTML = rxF
      ? `
        <div class="kv"><span class="kv-k">Polinômio / Init</span><span class="kv-v">CRC-16/CCITT-FALSE (0x1021 / 0xFFFF)</span></div>
        <div class="kv"><span class="kv-k">Última Rajada RX</span><span class="kv-v">#${rxF.burst_id} ("${rxF.payload_text}")</span></div>
        <div class="kv"><span class="kv-k">CRC Recebido no Quadro</span><span class="kv-v">0x${(rxF.rx_crc ?? 0).toString(16).toUpperCase().padStart(4, "0")}</span></div>
        <div class="kv"><span class="kv-k">CRC Calculado pelo RX</span><span class="kv-v">0x${(rxF.calc_crc ?? 0).toString(16).toUpperCase().padStart(4, "0")}</span></div>
        <div class="kv"><span class="kv-k">Resultado CRC RX</span><span class="kv-v" style="color:${rxF.crc_ok ? "#4ade80" : "#f87171"}">${rxF.crc_ok ? "VÁLIDO (CRC OK)" : "FALHOU"}</span></div>
        <div class="kv"><span class="kv-k">CRC Esperado no TX</span><span class="kv-v">${txF ? "0x" + (txF.crc16 ?? 0).toString(16).toUpperCase().padStart(4, "0") : "não disponível"}</span></div>
      `
      : `<div class="kv"><span class="kv-k">Estado</span><span class="kv-v">Aguardando quadro decodificado pelo RX</span></div>`;
  }
}
