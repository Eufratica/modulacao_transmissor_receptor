// Osciloscopio Digital de Alta Performance (Canvas 2D)
// Separa estritamente medicao real ADC de referencia teorica calculada TX.
import { getCanvasPalette } from "./a11y_theme.js?v=pwm3";

function drawPwmStaircase(ctx, ticks, t0, winEnd, fsPwm, pwmPeriod, mapX, mapY) {
  const k0 = Math.max(0, Math.floor((t0 / 1000.0) * fsPwm));
  const k1 = Math.min(ticks.length - 1, Math.ceil((winEnd / 1000.0) * fsPwm));
  ctx.beginPath();
  let started = false;
  for (let k = k0; k <= k1; k++) {
    const ta = (k / fsPwm) * 1000.0;
    const tb = ((k + 1) / fsPwm) * 1000.0;
    const ym = mapY((ticks[k] / pwmPeriod) * 3300.0);
    if (!started) { ctx.moveTo(mapX(ta), ym); started = true; } else ctx.lineTo(mapX(ta), ym);
    ctx.lineTo(mapX(tb), ym);
  }
  ctx.stroke();
}

function drawSymmetricPwmTrain(ctx, ticks, t0, winEnd, fsPwm, pwmPeriod, fPwmHz, mapX, mapY, yZero, yVdd, pal, showAvg) {
  const tPwmMs = 1000.0 / fPwmHz;
  const pwmEndMs = (ticks.length / fsPwm) * 1000.0;
  const p0 = Math.max(0, Math.floor(t0 / tPwmMs));
  const p1 = Math.min(Math.ceil(pwmEndMs / tPwmMs) - 1, Math.ceil(winEnd / tPwmMs));

  ctx.fillStyle = "rgba(34, 211, 238, 0.16)";
  for (let p = p0; p <= p1; p++) {
    const ts = p * tPwmMs;
    const tm = ts + 0.5 * tPwmMs;
    const k = Math.min(ticks.length - 1, Math.max(0, Math.floor((tm / 1000.0) * fsPwm)));
    const duty = Math.max(0, Math.min(1, ticks[k] / pwmPeriod));
    const halfOn = 0.5 * duty * tPwmMs;
    const xA = mapX(tm - halfOn);
    const xB = mapX(tm + halfOn);
    if (xB > xA) {
      ctx.fillRect(xA, yVdd, xB - xA, yZero - yVdd);
    }
  }

  ctx.strokeStyle = pal.accentCyan || "#22d3ee";
  ctx.lineWidth = 1.65;
  ctx.beginPath();
  let started = false;
  for (let p = p0; p <= p1; p++) {
    const ts = p * tPwmMs;
    const te = (p + 1) * tPwmMs;
    const tm = ts + 0.5 * tPwmMs;
    const k = Math.min(ticks.length - 1, Math.max(0, Math.floor((tm / 1000.0) * fsPwm)));
    const duty = Math.max(0, Math.min(1, ticks[k] / pwmPeriod));
    const halfOn = 0.5 * duty * tPwmMs;
    const xs = mapX(ts), xh0 = mapX(tm - halfOn), xh1 = mapX(tm + halfOn), xe = mapX(te);
    if (!started) { ctx.moveTo(xs, yZero); started = true; } else ctx.lineTo(xs, yZero);
    ctx.lineTo(xh0, yZero);
    ctx.lineTo(xh0, yVdd);
    ctx.lineTo(xh1, yVdd);
    ctx.lineTo(xh1, yZero);
    ctx.lineTo(xe, yZero);
  }
  ctx.stroke();

  if (showAvg) {
    ctx.strokeStyle = pal.isLight ? "#4f46e5" : "#818cf8";
    ctx.lineWidth = 1.6;
    ctx.setLineDash([4, 3]);
    drawPwmStaircase(ctx, ticks, t0, winEnd, fsPwm, pwmPeriod, mapX, mapY);
    ctx.setLineDash([]);
  }
}

export class OscilloscopeView {
  constructor(canvasId, metricsId, sourceBadgeId) {
    this.canvas = document.getElementById(canvasId);
    this.metricsEl = document.getElementById(metricsId);
    this.badgeEl = document.getElementById(sourceBadgeId);

    this.zoom = 1.0;
    this.panRatio = 0.0; // 0..1 dentro da janela total
    this.dragging = false;
    this.dragStartX = 0;
    this.dragStartPan = 0;
    this.cursorA = null; // fracao x [0..1]
    this.cursorB = null;
    this.highlightSymIdx = null;
    this.lastState = null;

    this._bindEvents();
  }

  _bindEvents() {
    this.canvas.addEventListener("wheel", (e) => {
      e.preventDefault();
      const factor = e.deltaY < 0 ? 1.25 : 0.8;
      this.zoom = Math.max(1.0, Math.min(40.0, this.zoom * factor));
      this.render(this.lastState);
    });

    this.canvas.addEventListener("mousedown", (e) => {
      if (e.shiftKey) {
        const rect = this.canvas.getBoundingClientRect();
        const fx = Math.max(0, Math.min(1, (e.clientX - rect.left - 56) / Math.max(1, rect.width - 72)));
        if (this.cursorA === null || (this.cursorA !== null && this.cursorB !== null)) {
          this.cursorA = fx;
          this.cursorB = null;
        } else {
          this.cursorB = fx;
        }
        this.render(this.lastState);
        return;
      }
      this.dragging = true;
      this.dragStartX = e.clientX;
      this.dragStartPan = this.panRatio;
    });

    window.addEventListener("mousemove", (e) => {
      if (!this.dragging) return;
      const dx = e.clientX - this.dragStartX;
      const w = Math.max(100, this.canvas.clientWidth - 72);
      this.panRatio = Math.max(0, Math.min(1, this.dragStartPan - (dx / w) / this.zoom));
      this.render(this.lastState);
    });

    window.addEventListener("mouseup", () => {
      this.dragging = false;
    });
  }

  resetZoom() {
    this.zoom = 1.0;
    this.panRatio = 0.0;
    this.cursorA = null;
    this.cursorB = null;
    this.render(this.lastState);
  }

  setHighlightSymbol(symIdx) {
    this.highlightSymIdx = symIdx;
    this.render(this.lastState);
  }

  render(state) {
    this.lastState = state;
    const dpr = window.devicePixelRatio || 1;
    const rect = this.canvas.getBoundingClientRect();
    if (rect.width < 10 || rect.height < 10) return;
    this.canvas.width = Math.floor(rect.width * dpr);
    this.canvas.height = Math.floor(rect.height * dpr);
    const ctx = this.canvas.getContext("2d");
    ctx.scale(dpr, dpr);

    const W = rect.width;
    const H = rect.height;
    const padL = 56, padR = 16, padT = 18, padB = 28;
    const pw = Math.max(10, W - padL - padR);
    const ph = Math.max(10, H - padT - padB);

    const pal = getCanvasPalette();
    ctx.fillStyle = pal.bg;
    ctx.fillRect(0, 0, W, H);

    const scope = state?.scope;
    const refWave = state?.tx_theoretical_wave;
    const pwmRef = state?.tx_pwm_ref;
    const hasAdc = Boolean(scope && scope.available && scope.samples_mv?.length > 1);
    const hasRef = Boolean(refWave && refWave.v_mv?.length > 1);
    const hasPwm = Boolean(pwmRef && pwmRef.cmp_ticks?.length > 1);

    const showRaw = document.getElementById("chk-scope-raw").checked;
    const showAc = document.getElementById("chk-scope-ac").checked;
    const showFilt = document.getElementById("chk-scope-filt").checked;
    const showRef = document.getElementById("chk-scope-ref").checked;
    const showDiff = document.getElementById("chk-scope-diff").checked;
    const showPwm = Boolean(document.getElementById("chk-scope-pwm")?.checked);
    const showCarrier = Boolean(document.getElementById("chk-scope-carrier")?.checked);
    const showBb = Boolean(document.getElementById("chk-scope-bb")?.checked);
    const alignRef = document.getElementById("chk-scope-align").checked;
    const winSel = document.getElementById("sel-scope-win").value;
    const xMode = document.getElementById("sel-scope-xaxis").value;

    const hasCarrier = showCarrier && Boolean(hasRef || state?.tx_status || state?.rx_status);
    if (!hasAdc && !hasRef && !hasPwm && !hasCarrier) {
      this.badgeEl.className = "badge badge-wait";
      this.badgeEl.textContent = "Aguardando hardware";
      ctx.fillStyle = pal.axis;
      ctx.font = "13px Consolas, monospace";
      ctx.textAlign = "center";
      ctx.fillText("Aguardando amostras ADC1 (RX) ou captura PWM TX (/cap)...", W / 2, H / 2);
      this.metricsEl.textContent = "Sem amostras disponíveis (Shift+Clique para cursores A/B, scroll para zoom, arraste para pan).";
      return;
    }

    // Atualiza badge informativo de origem/decimação
    if (state.is_offline_test) {
      this.badgeEl.className = "badge badge-wait";
      this.badgeEl.textContent = "OFFLINE TESTE (Simulado)";
    } else if (hasAdc) {
      this.badgeEl.className = "badge badge-ok";
      this.badgeEl.textContent = scope.is_full_rate_snapshot
        ? `ADC Real 20 kHz (${scope.decimated_for_view ? "Envelope visual" : "1:1"})`
        : `Fluxo Decimado ${scope.decim_factor}x`;
    } else if (hasPwm) {
      this.badgeEl.className = "badge badge-tx";
      this.badgeEl.textContent = "PWM Programado TX (GPIO 18 · /cap)";
    } else {
      this.badgeEl.className = "badge badge-tx";
      this.badgeEl.textContent = "Somente Referência Teórica TX";
    }

    const adcVals = hasAdc ? scope.samples_mv : [];
    const fsAdc = hasAdc ? scope.sample_rate_hz : (hasPwm ? (pwmRef.sample_rate_hz || 40000) : 20000);
    const fsPwm = hasPwm ? (pwmRef.sample_rate_hz || 40000) : 40000;
    const pwmPeriod = hasPwm ? Math.max(1, pwmRef.pwm_period_ticks || 1000) : 1000;
    const fPwmHz = state?.tx_status?.pwm_freq_hz || (fsPwm * 2) || 80000;
    const pwmTotalMs = hasPwm ? ((pwmRef.cmp_ticks.length - 1) / fsPwm) * 1000.0 : 0;
    const refTotalMs = hasRef ? (refWave.t_ms[refWave.t_ms.length - 1] || 100) : 0;
    const adcTotalMs = hasAdc ? ((adcVals.length - 1) / scope.sample_rate_hz) * 1000.0 : Math.max(pwmTotalMs, refTotalMs, 1.0);
    const totalMs = Math.max(adcTotalMs, 1.0);
    const winMs = winSel === "all" ? totalMs / this.zoom : Math.min(totalMs, parseFloat(winSel)) / this.zoom;
    const maxStart = Math.max(0, totalMs - winMs);
    const t0 = this.panRatio * maxStart;
    const t1 = t0 + winMs;

    // Estatísticas do sinal real
    let meanMv = 1300;
    let minMv = 0, maxMv = 2500;
    if (hasAdc) {
      meanMv = scope.stats?.mean_mv ?? (adcVals.reduce((a, b) => a + b, 0) / adcVals.length);
      minMv = scope.stats?.min_mv ?? Math.min(...adcVals);
      maxMv = scope.stats?.max_mv ?? Math.max(...adcVals);
    }

    // Define faixa vertical coerente com o que está habilitado
    const onlyAc = !showRaw && (showAc || showDiff) && !showRef && !showPwm && !showCarrier && !showBb;
    const showDigitalPwm = showPwm && hasPwm && !onlyAc;
    const yMin = onlyAc ? -1200 : (showDigitalPwm ? Math.min(-120, minMv - 100) : Math.min(0, minMv - 100));
    const yMax = onlyAc ? 1200 : Math.max(showDigitalPwm ? 3480 : 2500, maxMv + 150);

    const mapX = (tMs) => padL + ((tMs - t0) / Math.max(1e-6, winMs)) * pw;
    const mapY = (v) => padT + (1.0 - (v - yMin) / Math.max(1, yMax - yMin)) * ph;

    // Grade
    ctx.strokeStyle = pal.grid;
    ctx.lineWidth = 1;
    ctx.font = "10.5px Consolas, monospace";
    ctx.fillStyle = pal.axis;
    for (let g = 0; g <= 5; g++) {
      const yv = yMin + (g / 5) * (yMax - yMin);
      const yp = mapY(yv);
      ctx.beginPath();
      ctx.moveTo(padL, yp);
      ctx.lineTo(padL + pw, yp);
      ctx.stroke();
      ctx.textAlign = "right";
      ctx.fillText(`${yv.toFixed(0)}`, padL - 6, yp + 3);
    }
    for (let g = 0; g <= 6; g++) {
      const tm = t0 + (g / 6) * winMs;
      const xp = mapX(tm);
      ctx.beginPath();
      ctx.moveTo(xp, padT);
      ctx.lineTo(xp, padT + ph);
      ctx.stroke();
      ctx.textAlign = "center";
      const dec = winMs < 5 ? 2 : 1;
      const lbl = xMode === "idx" ? `${Math.round((tm / 1000.0) * fsAdc)}` : `${tm.toFixed(dec)}ms`;
      ctx.fillText(lbl, xp, padT + ph + 16);
    }

    // Destaque de símbolo clicado na Análise Digital
    if (this.highlightSymIdx !== null) {
      const baud = state?.tx_status?.symbol_rate_baud || state?.rx_status?.symbol_rate_baud || 100;
      const symMs = 1000.0 / baud;
      const sx0 = mapX(this.highlightSymIdx * symMs);
      const sx1 = mapX((this.highlightSymIdx + 1) * symMs);
      if (sx1 >= padL && sx0 <= padL + pw) {
        ctx.fillStyle = "rgba(56, 189, 248, 0.14)";
        ctx.fillRect(Math.max(padL, sx0), padT, Math.min(padL + pw, sx1) - Math.max(padL, sx0), ph);
      }
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(padL, padT, pw, ph);
    ctx.clip();

    // 4. Referência Teórica TX (calculada)
    if (showRef && hasRef) {
      let scale = 1.0;
      let dcShift = 0.0;
      let timeShift = 0.0;
      if (alignRef && hasAdc) {
        const ampAdc = Math.max(50, (maxMv - minMv) / 2.0);
        const ampRef = state?.tx_status?.amplitude_peak_mv || 900;
        scale = ampAdc / ampRef;
        const dcRef = state?.tx_status?.dc_offset_mv || 1300;
        dcShift = meanMv - dcRef * scale;
      }
      ctx.strokeStyle = "#f59e0b";
      ctx.lineWidth = 1.4;
      ctx.setLineDash([5, 3]);
      ctx.beginPath();
      let started = false;
      for (let i = 0; i < refWave.t_ms.length; i++) {
        const tm = refWave.t_ms[i] + timeShift;
        if (tm < t0 - 2 || tm > t1 + 2) continue;
        const x = mapX(tm);
        const y = mapY(refWave.v_mv[i] * scale + dcShift);
        if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
      }
      ctx.stroke();
      ctx.setLineDash([]);
    }

    // 1. ADC Bruto Real
    if (hasAdc && showRaw) {
      ctx.strokeStyle = "#22c55e";
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      let started = false;
      for (let i = 0; i < adcVals.length; i++) {
        const tm = (i / fsAdc) * 1000.0;
        if (tm < t0 - 1 || tm > t1 + 1) continue;
        const x = mapX(tm);
        const y = mapY(adcVals[i]);
        if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    // 2. ADC sem DC
    if (hasAdc && showAc) {
      ctx.strokeStyle = "#38bdf8";
      ctx.lineWidth = 1.3;
      ctx.beginPath();
      let started = false;
      for (let i = 0; i < adcVals.length; i++) {
        const tm = (i / fsAdc) * 1000.0;
        if (tm < t0 - 1 || tm > t1 + 1) continue;
        const val = onlyAc ? (adcVals[i] - meanMv) : (adcVals[i] - meanMv + 1300);
        const x = mapX(tm);
        const y = mapY(val);
        if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    // 3. Filtro digital média móvel pós-aquisição (suavização visual transparente)
    if (hasAdc && showFilt && adcVals.length > 5) {
      ctx.strokeStyle = "#a78bfa";
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      let started = false;
      for (let i = 2; i < adcVals.length - 2; i++) {
        const tm = (i / fsAdc) * 1000.0;
        if (tm < t0 - 1 || tm > t1 + 1) continue;
        const avg = (adcVals[i - 2] + adcVals[i - 1] + adcVals[i] + adcVals[i + 1] + adcVals[i + 2]) / 5.0;
        const x = mapX(tm);
        const y = mapY(avg);
        if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    // 5. Diferença (RX - Referência Teórica interpolada por timestamp)
    if (hasAdc && hasRef && showDiff) {
      let scale = 1.0;
      let dcShift = 0.0;
      if (alignRef) {
        const ampAdc = Math.max(50, (maxMv - minMv) / 2.0);
        const ampRef = state?.tx_status?.amplitude_peak_mv || 900;
        scale = ampAdc / ampRef;
        const dcRef = state?.tx_status?.dc_offset_mv || 1300;
        dcShift = meanMv - dcRef * scale;
      }
      ctx.strokeStyle = "#ef4444";
      ctx.lineWidth = 1.2;
      ctx.beginPath();
      let started = false;
      let rPtr = 0;
      for (let i = 0; i < adcVals.length; i++) {
        const tm = (i / fsAdc) * 1000.0;
        if (tm < t0 - 1 || tm > t1 + 1) continue;
        while (rPtr + 1 < refWave.t_ms.length && refWave.t_ms[rPtr + 1] < tm) rPtr++;
        if (rPtr + 1 >= refWave.t_ms.length) break;
        const dtRef = Math.max(1e-6, refWave.t_ms[rPtr + 1] - refWave.t_ms[rPtr]);
        const frac = Math.max(0, Math.min(1, (tm - refWave.t_ms[rPtr]) / dtRef));
        const refMv = refWave.v_mv[rPtr] + frac * (refWave.v_mv[rPtr + 1] - refWave.v_mv[rPtr]);
        const diff = adcVals[i] - (refMv * scale + dcShift);
        const val = onlyAc ? diff : (meanMv + diff);
        const x = mapX(tm);
        const y = mapY(val);
        if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    // 6. PWM Programado TX (GPIO 18 · /cap: ticks -> pulsos 0..3,3V @ 80 kHz ou média duty @ 40 kHz)
    let pwmMetricsStr = "";
    if (showDigitalPwm) {
      const ticks = pwmRef.cmp_ticks;
      let minT = ticks[0], maxT = ticks[0], sumT = 0;
      for (let i = 0; i < ticks.length; i++) {
        const v = ticks[i];
        if (v < minT) minT = v;
        if (v > maxT) maxT = v;
        sumT += v;
      }
      const minDutyPct = (minT / pwmPeriod) * 100.0;
      const maxDutyPct = (maxT / pwmPeriod) * 100.0;
      const meanDutyPct = (sumT / ticks.length / pwmPeriod) * 100.0;
      const meanPwmMv = (meanDutyPct / 100.0) * 3300.0;
      pwmMetricsStr = ` | PWM #${pwmRef.frame_id} (GPIO 18): ${ticks.length} pts @ ${(fsPwm / 1000).toFixed(0)}kHz · Duty ${minDutyPct.toFixed(1)}–${maxDutyPct.toFixed(1)}% (méd ${meanDutyPct.toFixed(1)}% ≈ ${meanPwmMv.toFixed(0)} mV)`;

      const pwmEndMs = (ticks.length / fsPwm) * 1000.0;
      const yZero = mapY(0);
      const yVdd = mapY(3300);

      // Linha guia VDD = 3300 mV e GND = 0 mV
      ctx.save();
      ctx.strokeStyle = "rgba(34, 211, 238, 0.28)";
      ctx.setLineDash([3, 4]);
      ctx.beginPath();
      ctx.moveTo(padL, yVdd); ctx.lineTo(padL + pw, yVdd);
      ctx.moveTo(padL, yZero); ctx.lineTo(padL + pw, yZero);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = pal.accentCyan || "#22d3ee";
      ctx.font = "10px Consolas, monospace";
      ctx.textAlign = "left";
      ctx.fillText("3300 mV (GPIO 18 HIGH)", padL + 6, yVdd - 4);
      ctx.restore();

      if (t0 < pwmEndMs) {
        const pulsesInWin = (winMs / 1000.0) * fPwmHz;
        const winEnd = Math.min(t1, pwmEndMs);

        if (pulsesInWin <= 240) {
          drawSymmetricPwmTrain(ctx, ticks, t0, winEnd, fsPwm, pwmPeriod, fPwmHz, mapX, mapY, yZero, yVdd, pal, true);
        } else {
          // Janela longa: faixa 0..3300 mV + envelope ZOH (duty × 3300 mV)
          const xStart = mapX(Math.max(t0, 0));
          const xEnd = mapX(winEnd);
          ctx.fillStyle = "rgba(34, 211, 238, 0.07)";
          ctx.fillRect(xStart, yVdd, Math.max(1, xEnd - xStart), yZero - yVdd);

          ctx.strokeStyle = pal.accentCyan || "#22d3ee";
          ctx.lineWidth = 1.75;
          drawPwmStaircase(ctx, ticks, t0, winEnd, fsPwm, pwmPeriod, mapX, mapY);

          ctx.fillStyle = pal.accentCyan || "#22d3ee";
          ctx.font = "10.5px Consolas, monospace";
          ctx.textAlign = "right";
          ctx.fillText(`Média PWM ${(fPwmHz / 1000).toFixed(0)} kHz · veja pulsos quadrados abaixo`, padL + pw - 8, yVdd + 14);
        }
      }
    }

    // Parametros comuns para Portadora Pura e Banda Base
    const txFrame = state?.last_tx_frame;
    const fcHz = txFrame?.carrier_freq_hz || state?.tx_status?.carrier_freq_hz || state?.rx_status?.carrier_freq_hz || 1000;
    const baudRate = txFrame?.symbol_rate_baud || state?.tx_status?.symbol_rate_baud || state?.rx_status?.symbol_rate_baud || 100;
    const ampBase = txFrame?.amplitude_peak_mv || state?.tx_status?.amplitude_peak_mv || 900;
    const dcBase = txFrame?.dc_offset_mv ?? state?.tx_status?.dc_offset_mv ?? 1300;
    let modScale = 1.0;
    let modDcShift = 0.0;
    if (alignRef && hasAdc) {
      const ampAdc = Math.max(50, (maxMv - minMv) / 2.0);
      modScale = ampAdc / ampBase;
      modDcShift = meanMv - dcBase * modScale;
    }
    const dcDraw = onlyAc ? 0.0 : (dcBase * modScale + modDcShift);

    // 7. Portadora Pura (cos 2*pi*fc*t sem modulacao)
    if (showCarrier) {
      ctx.strokeStyle = pal.isLight ? "#db2777" : "#f472b6";
      ctx.lineWidth = 1.25;
      ctx.setLineDash([2, 3]);
      ctx.beginPath();
      const steps = 900;
      for (let i = 0; i <= steps; i++) {
        const tm = t0 + (i / steps) * winMs;
        const val = dcDraw + ampBase * modScale * Math.cos(2 * Math.PI * fcHz * (tm / 1000.0));
        const x = mapX(tm);
        const y = mapY(val);
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
      ctx.setLineDash([]);
    }

    // 8. Banda Base I(t) e Q(t) (degraus por simbolo)
    const allSyms = txFrame?.chain?.all_symbols;
    if (showBb && allSyms && allSyms.length > 0 && baudRate > 0) {
      const symMs = 1000.0 / baudRate;
      const aEq = (ampBase / Math.SQRT2) * modScale;
      const s0 = Math.max(0, Math.floor(t0 / symMs));
      const s1 = Math.min(allSyms.length - 1, Math.ceil(t1 / symMs));
      const symsInWin = Math.max(1, s1 - s0 + 1);

      const drawBasebandStep = (key, color, dash, yOffsetPx) => {
        ctx.strokeStyle = color;
        ctx.lineWidth = 1.75;
        ctx.setLineDash(dash);
        ctx.beginPath();
        let started = false;
        for (let s = s0; s <= s1; s++) {
          const ts = Math.max(t0, s * symMs);
          const te = Math.min(t1, (s + 1) * symMs);
          if (te <= ts) continue;
          const val = dcDraw + allSyms[s][key] * aEq;
          const xA = mapX(ts), xB = mapX(te), y = mapY(val) + yOffsetPx;
          if (!started) { ctx.moveTo(xA, y); started = true; } else ctx.lineTo(xA, y);
          ctx.lineTo(xB, y);
        }
        ctx.stroke();
        ctx.setLineDash([]);
      };

      drawBasebandStep("i_exp", pal.isLight ? "#059669" : "#34d399", [], 0);
      drawBasebandStep("q_exp", pal.isLight ? "#7c3aed" : "#c084fc", [6, 3], 2);

      // Fronteiras de simbolo e rotulos de dibits I/Q quando janela <= 14 simbolos
      if (symsInWin <= 14) {
        ctx.font = "10px Consolas, monospace";
        ctx.textAlign = "center";
        for (let s = s0; s <= s1; s++) {
          const ts = s * symMs;
          const te = (s + 1) * symMs;
          if (ts >= t0 && ts <= t1) {
            ctx.strokeStyle = "rgba(148, 163, 184, 0.25)";
            ctx.lineWidth = 1;
            ctx.setLineDash([2, 3]);
            ctx.beginPath(); ctx.moveTo(mapX(ts), padT); ctx.lineTo(mapX(ts), padT + ph); ctx.stroke();
            ctx.setLineDash([]);
          }
          const tc = 0.5 * (Math.max(t0, ts) + Math.min(t1, te));
          if ((Math.min(t1, te) - Math.max(t0, ts)) > 0.35 * symMs) {
            const pt = allSyms[s];
            const chTag = pt.segment === "payload" ? ` '${pt.byte_char}'` : "";
            ctx.fillStyle = pal.textStrong || "#f1f5f9";
            ctx.fillText(`S${s}:${pt.bits}${chTag}`, mapX(tc), padT + 12);
          }
        }
      }
    }

    // Legenda compacta quando Portadora ou Banda Base ativas
    if (showCarrier || showBb) {
      const items = [];
      if (showRef && hasRef) items.push(["Seno 4-QAM", "#f59e0b"]);
      if (showCarrier) items.push([`Portadora ${fcHz}Hz`, pal.isLight ? "#db2777" : "#f472b6"]);
      if (showBb && allSyms?.length) {
        items.push(["I(t) Banda Base", pal.isLight ? "#059669" : "#34d399"]);
        items.push(["Q(t) Banda Base", pal.isLight ? "#7c3aed" : "#c084fc"]);
      }
      ctx.font = "10px Consolas, monospace";
      ctx.textAlign = "left";
      let lx = padL + 8;
      const ly = padT + ph - 8;
      items.forEach(([txt, col]) => {
        ctx.fillStyle = col;
        ctx.fillRect(lx, ly - 7, 10, 3);
        ctx.fillText(txt, lx + 14, ly);
        lx += ctx.measureText(txt).width + 26;
      });
    }

    ctx.restore();

    // Cursores A/B e Seleção de Intervalo
    let cursorInfo = "Shift+Clique: Cursores A/B e Seleção de Intervalo";
    if (this.cursorA !== null) {
      const xa = padL + this.cursorA * pw;
      const ta = t0 + this.cursorA * winMs;
      ctx.strokeStyle = "#e2e8f0";
      ctx.setLineDash([3, 3]);
      ctx.beginPath(); ctx.moveTo(xa, padT); ctx.lineTo(xa, padT + ph); ctx.stroke();
      ctx.setLineDash([]);
      if (this.cursorB !== null) {
        const xb = padL + this.cursorB * pw;
        const tb = t0 + this.cursorB * winMs;
        ctx.fillStyle = "rgba(251, 191, 36, 0.10)";
        ctx.fillRect(Math.min(xa, xb), padT, Math.abs(xb - xa), ph);
        ctx.strokeStyle = "#fbbf24";
        ctx.beginPath(); ctx.moveTo(xb, padT); ctx.lineTo(xb, padT + ph); ctx.stroke();
        const tLo = Math.min(ta, tb), tHi = Math.max(ta, tb);
        const dt = tHi - tLo;
        const fEst = dt > 1e-4 ? (1000.0 / dt).toFixed(1) : "—";
        let intStr = "";
        if (hasAdc) {
          const i0 = Math.max(0, Math.floor((tLo / 1000.0) * fsAdc));
          const i1 = Math.min(adcVals.length - 1, Math.ceil((tHi / 1000.0) * fsAdc));
          if (i1 > i0) {
            const sl = adcVals.slice(i0, i1 + 1);
            const sMin = Math.min(...sl), sMax = Math.max(...sl);
            const sMean = sl.reduce((a, b) => a + b, 0) / sl.length;
            intStr = ` | Intervalo [${sMin}..${sMax} mV, med=${sMean.toFixed(1)} mV, Vpp=${sMax - sMin} mV]`;
          }
        }
        cursorInfo = `A=${ta.toFixed(2)}ms B=${tb.toFixed(2)}ms Δt=${dt.toFixed(3)}ms (1/Δt=${fEst} Hz)${intStr}`;
      } else {
        cursorInfo = `Cursor A=${ta.toFixed(2)}ms (Shift+Clique para definir intervalo B)`;
      }
    }

    if (hasAdc) {
      this.metricsEl.textContent =
        `DC=${meanMv.toFixed(1)} mV | Min=${minMv} mV | Max=${maxMv} mV | Vpp=${maxMv - minMv} mV | ` +
        `Amostras=${scope.original_count ?? adcVals.length} @ ${scope.sample_rate_hz} Hz | Zoom=${this.zoom.toFixed(1)}x${pwmMetricsStr} | ${cursorInfo}`;
    } else if (hasPwm) {
      this.metricsEl.textContent =
        `Modo PWM TX (Zoom=${this.zoom.toFixed(1)}x)${pwmMetricsStr} | ${cursorInfo}`;
    } else {
      this.metricsEl.textContent = `Referência teórica TX (Zoom=${this.zoom.toFixed(1)}x) | ${cursorInfo}`;
    }
  }
}

export class PwmScopeView {
  constructor(canvasId, metricsId, badgeId) {
    this.canvas = document.getElementById(canvasId);
    this.metricsEl = document.getElementById(metricsId);
    this.badgeEl = document.getElementById(badgeId);
    this.lastState = null;
  }

  render(state) {
    this.lastState = state;
    if (!this.canvas) return;
    const dpr = window.devicePixelRatio || 1;
    const rect = this.canvas.getBoundingClientRect();
    if (rect.width < 10 || rect.height < 10) return;
    this.canvas.width = Math.floor(rect.width * dpr);
    this.canvas.height = Math.floor(rect.height * dpr);
    const ctx = this.canvas.getContext("2d");
    ctx.scale(dpr, dpr);

    const W = rect.width;
    const H = rect.height;
    const padL = 62, padR = 16, padT = 20, padB = 28;
    const pw = Math.max(10, W - padL - padR);
    const ph = Math.max(10, H - padT - padB);
    const pal = getCanvasPalette();

    ctx.fillStyle = pal.bg;
    ctx.fillRect(0, 0, W, H);

    const pwmRef = state?.tx_pwm_ref;
    const ticks = pwmRef?.cmp_ticks;
    if (!ticks || ticks.length < 2) {
      if (this.badgeEl) {
        this.badgeEl.className = "badge badge-wait";
        this.badgeEl.textContent = "Aguardando /cap TX";
      }
      ctx.fillStyle = pal.axis;
      ctx.font = "13px Consolas, monospace";
      ctx.textAlign = "center";
      ctx.fillText("Envie texto ao TX ou clique em 'Capturar PWM TX (/cap)' para ver a onda quadrada do GPIO 18...", W / 2, H / 2);
      if (this.metricsEl) this.metricsEl.textContent = "Sem captura PWM (/cap TX) nesta sessão.";
      return;
    }

    const fsPwm = pwmRef.sample_rate_hz || 40000;
    const period = Math.max(1, pwmRef.pwm_period_ticks || 1000);
    const fPwmHz = state?.tx_status?.pwm_freq_hz || (fsPwm * 2) || 80000;
    const totalMs = (ticks.length / fsPwm) * 1000.0;
    const winSel = document.getElementById("sel-pwm-win");
    const rngOff = document.getElementById("rng-pwm-offset");
    const lblOff = document.getElementById("lbl-pwm-offset");
    const showAvg = Boolean(document.getElementById("chk-pwm-avg")?.checked);

    const winMs = Math.min(totalMs, parseFloat(winSel?.value || "0.4") || 0.4);
    const maxOffset = Math.max(0, totalMs - winMs);
    let t0 = 0;
    if (rngOff) {
      rngOff.max = maxOffset.toFixed(1);
      t0 = Math.min(maxOffset, Math.max(0, parseFloat(rngOff.value) || 0));
      rngOff.value = t0.toFixed(1);
    }
    if (lblOff) lblOff.textContent = `${t0.toFixed(1)} ms`;
    const t1 = t0 + winMs;

    if (this.badgeEl) {
      if (state.is_offline_test) {
        this.badgeEl.className = "badge badge-wait";
        this.badgeEl.textContent = "OFFLINE — Onda Quadrada 80 kHz";
      } else {
        this.badgeEl.className = "badge badge-ok";
        this.badgeEl.textContent = `ONDA QUADRADA ${(fPwmHz / 1000).toFixed(0)} kHz (GPIO 18)`;
      }
    }

    const yMin = -200, yMax = 3550;
    const mapX = (tMs) => padL + ((tMs - t0) / Math.max(1e-6, winMs)) * pw;
    const mapY = (v) => padT + (1.0 - (v - yMin) / (yMax - yMin)) * ph;

    // Grade fixa em tensao logica 0..3300 mV
    ctx.strokeStyle = pal.grid;
    ctx.lineWidth = 1;
    ctx.font = "10.5px Consolas, monospace";
    ctx.fillStyle = pal.axis;
    for (const yv of [0, 825, 1650, 2475, 3300]) {
      const yp = mapY(yv);
      ctx.beginPath();
      ctx.moveTo(padL, yp);
      ctx.lineTo(padL + pw, yp);
      ctx.stroke();
      ctx.textAlign = "right";
      ctx.fillText(`${yv}mV`, padL - 6, yp + 3);
    }
    for (let g = 0; g <= 6; g++) {
      const tm = t0 + (g / 6) * winMs;
      const xp = mapX(tm);
      ctx.beginPath();
      ctx.moveTo(xp, padT);
      ctx.lineTo(xp, padT + ph);
      ctx.stroke();
      ctx.textAlign = "center";
      ctx.fillText(`${tm.toFixed(2)}ms`, xp, padT + ph + 16);
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(padL, padT, pw, ph);
    ctx.clip();
    drawSymmetricPwmTrain(ctx, ticks, t0, Math.min(t1, totalMs), fsPwm, period, fPwmHz, mapX, mapY, mapY(0), mapY(3300), pal, showAvg);
    ctx.restore();

    // Estatisticas na janela visivel
    const k0 = Math.max(0, Math.floor((t0 / 1000.0) * fsPwm));
    const k1 = Math.min(ticks.length - 1, Math.ceil((t1 / 1000.0) * fsPwm));
    let minT = ticks[k0], maxT = ticks[k0], sumT = 0, cnt = 0;
    for (let k = k0; k <= k1; k++) {
      const v = ticks[k];
      if (v < minT) minT = v;
      if (v > maxT) maxT = v;
      sumT += v;
      cnt++;
    }
    const minDuty = (minT / period) * 100.0;
    const maxDuty = (maxT / period) * 100.0;
    const meanDuty = cnt ? (sumT / cnt / period) * 100.0 : 0;
    const tUs = 1e6 / fPwmHz;
    if (this.metricsEl) {
      this.metricsEl.textContent =
        `GPIO 18 (MCPWM0A @ ${(fPwmHz / 1000).toFixed(1)} kHz · T=${tUs.toFixed(1)} µs) | Captura #${pwmRef.frame_id}: ` +
        `${ticks.length} pts (${t0.toFixed(2)}..${t1.toFixed(2)} ms) | Pulso HIGH na janela: ${(minDuty * tUs / 100).toFixed(2)} µs (${minDuty.toFixed(1)}%) .. ` +
        `${(maxDuty * tUs / 100).toFixed(2)} µs (${maxDuty.toFixed(1)}%) | Média: ${meanDuty.toFixed(1)}% (${((meanDuty / 100) * 3300).toFixed(0)} mV)`;
    }
  }
}
