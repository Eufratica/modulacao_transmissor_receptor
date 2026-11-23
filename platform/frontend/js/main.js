// Controlador Principal da Central de Instrumentacao 4-QAM (Cockpit SCADA)

import { initA11yTheme, applyTheme } from "./a11y_theme.js?v=pwm3";
import { OscilloscopeView, PwmScopeView } from "./scope.js?v=pwm3";
import { ConstellationView } from "./constellation.js?v=pwm3";
import { SpectrumView } from "./spectrum.js?v=pwm3";
import { FiltersLabView } from "./filters_rc.js?v=pwm3";
import { DigitalTruthView } from "./digital_truth.js?v=pwm3";
import { DspLabView } from "./dsp_lab.js?v=pwm3";
import { renderMathReference } from "./math_ref.js?v=pwm3";
import { HistoryView } from "./history.js?v=pwm3";

let latestState = null;
let viewPaused = false;
let lastRenderedTab = "overview";
let lastFftSig = "";
let lastFftFetchMs = 0;

const scopeView = new OscilloscopeView("canvas-scope", "scope-metrics", "scope-source-badge");
const pwmScopeView = new PwmScopeView("canvas-pwm-scope", "pwm-scope-metrics", "pwm-scope-badge");
const constView = new ConstellationView("canvas-const", "const-info", "#tbl-const-map tbody");
const specView = new SpectrumView("canvas-fft", "fft-metrics");
const filtersView = new FiltersLabView();
const dspView = new DspLabView();
const historyView = new HistoryView();
const digitalView = new DigitalTruthView((symIdx) => {
  scopeView.setHighlightSymbol(symIdx);
  constView.setHighlightSymbol(symIdx);
  if (latestState) constView.render(latestState);
});

async function apiPost(url, body = {}) {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  return res.json();
}

function showConnError(msg) {
  const box = document.getElementById("conn-error");
  if (!msg) {
    box.classList.add("hidden");
    box.textContent = "";
  } else {
    box.classList.remove("hidden");
    box.textContent = msg;
  }
}

async function refreshComPorts() {
  const data = await (await fetch("/api/ports")).json();
  const ports = data.ports || [];
  ["sel-tx-port", "sel-rx-port"].forEach((id) => {
    const sel = document.getElementById(id);
    const prev = sel.value;
    sel.innerHTML = ports.length
      ? ports.map((p) => `<option value="${p.device}">${p.device} — ${p.description}</option>`).join("")
      : `<option value="">Nenhuma porta COM detectada</option>`;
    if (prev && ports.some((p) => p.device === prev)) sel.value = prev;
  });
}

function kvHtml(items) {
  return items.map(([k, v]) => `<div class="kv"><span class="kv-k">${k}</span><span class="kv-v">${v ?? "não disponível"}</span></div>`).join("");
}

function renderOverview(state) {
  const txC = state.connections?.tx;
  const rxC = state.connections?.rx;
  const txS = state.tx_status;
  const rxS = state.rx_status;

  // Conexões
  const setConnUI = (role, conn) => {
    const btn = document.getElementById(`btn-${role}-conn`);
    const info = document.getElementById(`${role}-conn-info`);
    const topB = document.getElementById(`badge-${role}-conn`);
    const ovB = document.getElementById(`ov-${role}-badge`);
    if (conn?.status === "connected") {
      btn.textContent = `Desconectar ${role.toUpperCase()}`;
      btn.className = "btn btn-warn btn-sm";
      const idStr = conn.identified ? `Identificado (${(conn.detected_role || role).toUpperCase()})` : "Aguardando /id...";
      info.textContent = `Conectado em ${conn.port} @ ${conn.baudrate} bps · ${idStr} · ${conn.stats?.packets_ok || 0} pkts`;
      topB.className = "badge badge-ok";
      topB.textContent = `${role.toUpperCase()}: ${conn.port}`;
      ovB.className = "badge badge-ok";
      ovB.textContent = conn.identified ? "Ativo" : "Conectado";
    } else if (conn?.status === "error") {
      btn.textContent = `Conectar ${role.toUpperCase()}`;
      btn.className = "btn btn-primary btn-sm";
      info.textContent = `Erro: ${conn.last_error || "falha serial"}`;
      topB.className = "badge badge-err";
      topB.textContent = `${role.toUpperCase()}: Erro`;
      ovB.className = "badge badge-err";
      ovB.textContent = "Erro serial";
    } else {
      btn.textContent = `Conectar ${role.toUpperCase()}`;
      btn.className = "btn btn-primary btn-sm";
      info.textContent = "Estado: Desconectado";
      topB.className = "badge badge-off";
      topB.textContent = `${role.toUpperCase()}: Desconectado`;
      ovB.className = state.is_offline_test ? "badge badge-wait" : "badge badge-wait";
      ovB.textContent = state.is_offline_test ? "Teste Offline" : "Aguardando hardware";
    }
  };
  setConnUI("tx", txC);
  setConnUI("rx", rxC);

  // Badge global de origem de dados
  const srcB = document.getElementById("badge-source");
  if (state.is_offline_test) {
    srcB.className = "badge badge-wait";
    srcB.textContent = "MODO OFFLINE (SIMULAÇÃO)";
    document.getElementById("banner-offline").classList.remove("hidden");
  } else {
    document.getElementById("banner-offline").classList.add("hidden");
    if (txC?.status === "connected" || rxC?.status === "connected") {
      srcB.className = "badge badge-ok";
      srcB.textContent = "HARDWARE REAL CONECTADO";
    } else {
      srcB.className = "badge badge-wait";
      srcB.textContent = "Aguardando hardware";
    }
  }

  // Banner divergência de modo TX vs RX
  const mmBanner = document.getElementById("banner-mismatch");
  if (state.mode_mismatch_warning) {
    document.getElementById("mm-tx").textContent = `Modo ${txS.tx_mode}`;
    document.getElementById("mm-rx").textContent = `Modo ${rxS.rx_mode}`;
    mmBanner.classList.remove("hidden");
  } else {
    mmBanner.classList.add("hidden");
  }

  // Taxa serial e perda
  const bpsTot = (txC?.stats?.wire_bps || 0) + (rxC?.stats?.wire_bps || 0);
  document.getElementById("badge-wire-rate").textContent = `Serial: ${bpsTot.toFixed(0)} B/s`;
  const lostTot = (txC?.stats?.packets_lost || 0) + (rxC?.stats?.packets_lost || 0);
  const crcErrPkt = (txC?.stats?.crc_errors || 0) + (rxC?.stats?.crc_errors || 0);
  document.getElementById("ov-loss-badge").textContent = `Perda pkts: ${lostTot} | CRC pkt: ${crcErrPkt}`;

  // Card TX
  const pwmSnap = state.tx_pwm_ref;
  let pwmSnapText = "— (envie texto ou /cap)";
  if (pwmSnap?.cmp_ticks?.length) {
    const pt = pwmSnap.pwm_period_ticks || 1000;
    const minP = Math.min(...pwmSnap.cmp_ticks);
    const maxP = Math.max(...pwmSnap.cmp_ticks);
    pwmSnapText = `${pwmSnap.cmp_ticks.length} pts (#${pwmSnap.frame_id}) · duty ${((minP / pt) * 100).toFixed(1)}–${((maxP / pt) * 100).toFixed(1)}%`;
  }
  document.getElementById("ov-tx-grid").innerHTML = kvHtml([
    ["Porta / Serial", txC?.status === "connected" ? `${txC.port} @ ${txC.baudrate}` : "Desconectado"],
    ["Estado Firmware", txS ? (txS.transmitting ? "Transmitindo" : "Ocioso / Pronto") : "Aguardando hardware"],
    ["Freq. PWM / LUT fs", txS ? `${txS.pwm_freq_hz} Hz / ${txS.sample_rate_hz} Hz` : "não disponível"],
    ["Portadora (fc) / Baud", txS ? `${txS.carrier_freq_hz} Hz / ${txS.symbol_rate_baud} baud` : "não disponível"],
    ["Modo / Mapeamento", txS ? `Modo ${txS.tx_mode} · ${txS.map_preset}` : "não disponível"],
    ["Sinal Q / Ordem bits", txS ? `${txS.q_sign_minus ? "-sin" : "+cos(wt-90)"} · ${txS.bit_order}` : "não disponível"],
    ["DC / Vpk / GPIO", txS ? `${txS.dc_offset_mv} mV / ${txS.amplitude_peak_mv} mV / GPIO ${txS.pwm_gpio}` : "não disponível"],
    ["Quadros / PWM (/cap)", txS ? `${txS.frames_sent} env. · ${pwmSnapText}` : pwmSnapText],
  ]);

  // Card RX
  document.getElementById("ov-rx-grid").innerHTML = kvHtml([
    ["Porta / ADC1 GPIO", rxC?.status === "connected" ? `${rxC.port} · GPIO ${rxS?.adc_gpio ?? 34}` : `GPIO ${rxS?.adc_gpio ?? "—"} (Desconectado)`],
    ["Amostragem ADC", rxS ? `${rxS.sample_rate_hz} Hz · 12 bits (${rxS.efuse_cal_active ? "eFuse ON" : "sem eFuse"})` : "não disponível"],
    ["Estado Sinal / Sync", rxS ? (rxS.signal_active ? "Recebendo / Sincronizando" : "Silêncio (Squelch)") : "Aguardando hardware"],
    ["Costas Loop / df", rxS ? `${rxS.freq_offset_hz.toFixed(2)} Hz (±35 Hz)` : "não disponível"],
    ["Gardner TED / Rotação", state.dsp_symbols?.length ? `${state.dsp_symbols.slice(-1)[0].sym_rate_corr_ppm} ppm · ${state.dsp_symbols.slice(-1)[0].rot_deg}°` : "não disponível"],
    ["SNR Último Símbolo", state.dsp_symbols?.length ? `${state.dsp_symbols.slice(-1)[0].snr_db.toFixed(1)} dB` : "não calculado"],
    ["Quadros CRC OK / Erro", rxS ? `${rxS.frames_ok} OK / ${rxS.frames_crc_err} erro` : "0 / 0"],
    ["Overflows ADC / Drop TLM", rxS ? `${rxS.adc_errors} adc / ${rxS.telem_dropped} tlm` : "0 / 0"],
  ]);

  // Canal & Ground Truth resumo
  const gt = state.last_ground_truth;
  document.getElementById("ov-channel-grid").innerHTML = kvHtml([
    ["Último Quadro TX", state.last_tx_frame ? `#${state.last_tx_frame.frame_id}: "${state.last_tx_frame.payload_text}"` : "não disponível"],
    ["Último Quadro RX", state.last_rx_frame ? `Rajada #${state.last_rx_frame.burst_id}: "${state.last_rx_frame.payload_text}" (${state.last_rx_frame.crc_ok ? "CRC OK" : "CRC ERRO"})` : "não disponível"],
    ["Integridade GT (BER)", gt?.calculable ? `${gt.ber.toExponential(2)} (${gt.bit_errors}/${gt.total_bits} bits)` : "não calculável"],
    ["Sessão / Origem", state.is_offline_test ? "Teste Offline (Simulação)" : state.data_source],
  ]);

  // Console CLI logs
  const con = document.getElementById("cli-console");
  const atBottom = (con.scrollHeight - con.scrollTop - con.clientHeight) < 30;
  con.innerHTML = (state.cli_logs || []).length
    ? state.cli_logs.map((l) => `<div class="${l.role === "tx" ? "log-tx" : "log-rx"}">[${l.role.toUpperCase()}] ${escapeHtml(l.line)}</div>`).join("")
    : "Aguardando logs UART dos dispositivos ESP32...";
  if (atBottom) con.scrollTop = con.scrollHeight;

  // Ticker de telemetria no header duplo
  const tickFc = document.getElementById("tick-fc-baud");
  const tickAdc = document.getElementById("tick-adc");
  const tickRx = document.getElementById("tick-rx-level");
  const tickSync = document.getElementById("tick-sync");
  const tickGt = document.getElementById("tick-gt");
  const refCfg = rxS || txS;
  if (tickFc) {
    tickFc.textContent = refCfg ? `${refCfg.carrier_freq_hz} Hz · ${refCfg.symbol_rate_baud} baud` : "Aguardando hardware";
  }
  if (tickAdc) {
    tickAdc.textContent = rxS ? `${rxS.sample_rate_hz} Hz · ${rxS.efuse_cal_active ? "eFuse ON" : "sem cal"} · err=${rxS.adc_errors}` : "Aguardando RX";
  }
  if (tickRx) {
    tickRx.textContent = rxS ? `DC ${rxS.dc_est_mv.toFixed(0)} mV · Vpk ${rxS.amp_pk_est_mv.toFixed(0)} mV` : "DC — · Vpk —";
  }
  if (tickSync) {
    const lastSym = state.dsp_symbols?.length ? state.dsp_symbols[state.dsp_symbols.length - 1] : null;
    tickSync.textContent = rxS
      ? `df ${rxS.freq_offset_hz.toFixed(2)} Hz${lastSym ? ` · ${lastSym.rot_deg}°` : ""}`
      : "df — · Gardner —";
  }
  if (tickGt) {
    tickGt.textContent = gt?.calculable
      ? `BER ${gt.ber.toExponential(1)} (${gt.crc_ok ? "CRC OK" : "CRC ERR"})`
      : state.last_rx_frame ? (state.last_rx_frame.crc_ok ? "CRC OK (sem TX)" : "CRC FALHOU") : "não calculável";
  }

  // Diagnostico preventivo TX <-> RX e saude ADC/DMA
  const diagBadge = document.getElementById("diag-overall-badge");
  const diagGrid = document.getElementById("diag-health-grid");
  if (diagBadge && diagGrid) {
    const txUp = txC?.status === "connected" && txC?.identified;
    const rxUp = rxC?.status === "connected" && rxC?.identified;
    if (state.is_offline_test) {
      diagBadge.className = "badge badge-wait";
      diagBadge.textContent = "MODO OFFLINE (SIMULAÇÃO)";
    } else if (state.mode_mismatch_warning) {
      diagBadge.className = "badge badge-err";
      diagBadge.textContent = `Divergência TX=${txS?.tx_mode} / RX=${rxS?.rx_mode}`;
    } else if (txUp && rxUp) {
      diagBadge.className = "badge badge-ok";
      diagBadge.textContent = "TX + RX Operacionais";
    } else if (txUp || rxUp) {
      diagBadge.className = "badge badge-tx";
      diagBadge.textContent = txUp ? "Somente TX Conectado" : "Somente RX Conectado";
    } else {
      diagBadge.className = "badge badge-wait";
      diagBadge.textContent = "Aguardando placas";
    }

    const modeStr = txS && rxS
      ? (state.mode_mismatch_warning ? `TX Modo ${txS.tx_mode} ≠ RX Modo ${rxS.rx_mode}` : `Alinhados · Modo ${txS.tx_mode}`)
      : (txS ? `TX Modo ${txS.tx_mode}` : rxS ? `RX Modo ${rxS.rx_mode}` : "Não definido");
    diagGrid.innerHTML = [
      ["TX ESP32 (COM)", txUp ? `${txC.port} · ${txC.stats?.packets_ok || 0} pkts` : (txC?.status === "connected" ? "Conectado (aguard. /id)" : "Desconectado"), txUp ? "badge-ok" : "badge-off"],
      ["RX ADC1 DMA", rxS ? `${rxS.sample_rate_hz} Hz · ${rxS.adc_errors} ovf` : "Aguardando hardware", rxS ? (rxS.adc_errors === 0 ? "badge-ok" : "badge-err") : "badge-wait"],
      ["Alinhamento Modo", modeStr, state.mode_mismatch_warning ? "badge-err" : (txS && rxS ? "badge-ok" : "badge-neutral")],
      ["Integridade TLM", `Perda: ${lostTot} · CRC pkt: ${crcErrPkt}`, (lostTot + crcErrPkt) > 0 ? "badge-err" : "badge-ok"],
    ].map(([k, v, cls]) => (
      `<div class="diag-pill"><div class="diag-pill-top"><span>${k}</span><span class="badge ${cls}" style="padding:1px 6px;font-size:9.5px">${cls === "badge-ok" ? "OK" : cls === "badge-err" ? "ATENÇÃO" : "INFO"}</span></div><div class="diag-pill-val">${escapeHtml(v)}</div></div>`
    )).join("");
  }

  // Ultimos retornos na sidebar
  const miniList = document.getElementById("sidebar-mini-events");
  if (miniList) {
    const recent = (state.cli_logs || []).filter((l) => l.line && !l.line.includes("[0;")).slice(-4).reverse();
    miniList.innerHTML = recent.length
      ? recent.map((l) => (
          `<div class="mini-ev-card"><div class="mini-ev-top"><span class="${l.role === "tx" ? "log-tx" : "log-rx"}">${l.role.toUpperCase()}</span><span>UART</span></div><div class="mini-ev-line" title="${escapeHtml(l.line)}">${escapeHtml(l.line)}</div></div>`
        )).join("")
      : `<div class="mini-ev-empty">Aguardando eventos seriais...</div>`;
  }
}

function escapeHtml(s) {
  return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function renderActiveTab(state) {
  if (!state) return;
  renderOverview(state);
  if (lastRenderedTab === "scope") {
    scopeView.render(state);
    pwmScopeView.render(state);
  }
  if (lastRenderedTab === "constellation") constView.render(state);
  if (lastRenderedTab === "digital") digitalView.render(state);
  if (lastRenderedTab === "dsp") dspView.render(state);
  if (lastRenderedTab === "math") renderMathReference("math-cards", state);
  if (lastRenderedTab === "spectrum") {
    const sc = state.scope;
    const sig = `${state.data_source}|${state.is_offline_test}|${sc?.available}|${sc?.is_full_rate_snapshot}|${sc?.samples_mv?.length || 0}|${sc?.stats?.max_mv ?? ""}`;
    const now = Date.now();
    if (sig !== lastFftSig && now - lastFftFetchMs > 400) {
      lastFftSig = sig;
      lastFftFetchMs = now;
      runRealFft();
    }
  }
}

async function updateRcLab() {
  const params = new URLSearchParams({
    r1: document.getElementById("rc-r1").value,
    c1: document.getElementById("rc-c1").value,
    r2: document.getElementById("rc-r2").value,
    c2: document.getElementById("rc-c2").value,
    fpwm: document.getElementById("rc-fpwm").value,
    fc: document.getElementById("rc-fc").value,
  });
  const model = await (await fetch(`/api/rc_model?${params}`)).json();
  filtersView.render(model);
}

async function runRealFft() {
  const win = document.getElementById("sel-fft-win").value;
  const res = await (await fetch(`/api/fft?window=${win}`)).json();
  const fcExp = latestState?.rx_status?.carrier_freq_hz || latestState?.tx_status?.carrier_freq_hz || 1000;
  specView.render(res, fcExp);
}

async function sendFirmwareCommand(role, command) {
  document.getElementById("cmd-preview").textContent = `Enviando ao ${role.toUpperCase()}: "${command}" ...`;
  const res = await apiPost("/api/command", { role, command });
  if (!res.ok) {
    document.getElementById("cmd-preview").textContent = `RECUSADO (${role.toUpperCase()}): ${res.error}`;
    showConnError(res.error);
  } else {
    showConnError(null);
    document.getElementById("cmd-preview").textContent = `Enviado ao ${role.toUpperCase()}: "${res.command}" (aguarde confirmação no Console UART).`;
  }
}

function activateTab(requestedTab) {
  const panel = document.getElementById(`tab-${requestedTab}`);
  const tabName = panel ? requestedTab : "overview";
  const btn = document.querySelector(`.tab[data-tab="${tabName}"]`);

  document.querySelectorAll(".tab").forEach((b) => {
    b.classList.remove("active");
    b.setAttribute("aria-selected", "false");
  });
  document.querySelectorAll(".tab-panel").forEach((p) => p.classList.remove("active"));
  if (btn) {
    btn.classList.add("active");
    btn.setAttribute("aria-selected", "true");
  }
  (panel || document.getElementById("tab-overview"))?.classList.add("active");

  lastRenderedTab = tabName;
  const sidebar = document.getElementById("lab-sidebar");
  const menuBtn = document.getElementById("btn-lab-menu");
  sidebar?.classList.remove("open");
  menuBtn?.setAttribute("aria-expanded", "false");

  const url = new URL(window.location.href);
  if (url.searchParams.get("tab") !== tabName) {
    url.searchParams.set("tab", tabName);
    window.history.replaceState({}, "", url);
  }

  if (tabName === "filters") updateRcLab();
  if (tabName === "spectrum") runRealFft();
  if (tabName === "history") historyView.refreshList();
  if (latestState) renderActiveTab(latestState);
}

function setupUiEvents() {
  // Menu mobile/tablet da barra lateral
  const menuBtn = document.getElementById("btn-lab-menu");
  const sidebar = document.getElementById("lab-sidebar");
  if (menuBtn && sidebar) {
    menuBtn.addEventListener("click", () => {
      const open = sidebar.classList.toggle("open");
      menuBtn.setAttribute("aria-expanded", open ? "true" : "false");
    });
  }

  // Tabs + navegacao por setas do teclado (acessibilidade tablist)
  const tabs = Array.from(document.querySelectorAll(".tab"));
  tabs.forEach((btn, idx) => {
    btn.addEventListener("click", () => {
      activateTab(btn.getAttribute("data-tab") || "overview");
    });
    btn.addEventListener("keydown", (e) => {
      if (e.key === "ArrowDown" || e.key === "ArrowRight") {
        e.preventDefault();
        tabs[(idx + 1) % tabs.length].focus();
      } else if (e.key === "ArrowUp" || e.key === "ArrowLeft") {
        e.preventDefault();
        tabs[(idx - 1 + tabs.length) % tabs.length].focus();
      } else if (e.key === "Home") {
        e.preventDefault();
        tabs[0].focus();
      } else if (e.key === "End") {
        e.preventDefault();
        tabs[tabs.length - 1].focus();
      }
    });
  });

  // Recolher/expandir barra lateral no desktop
  const labShell = document.getElementById("lab-shell");
  const collapseBtn = document.getElementById("btn-sidebar-collapse");
  if (labShell && collapseBtn) {
    try {
      if (localStorage.getItem("qam_sidebar_collapsed") === "1") {
        labShell.classList.add("sidebar-collapsed");
        collapseBtn.setAttribute("aria-expanded", "false");
      }
    } catch {}
    collapseBtn.addEventListener("click", () => {
      const collapsed = labShell.classList.toggle("sidebar-collapsed");
      collapseBtn.setAttribute("aria-expanded", collapsed ? "false" : "true");
      try { localStorage.setItem("qam_sidebar_collapsed", collapsed ? "1" : "0"); } catch {}
      setTimeout(() => {
        if (lastRenderedTab === "filters") updateRcLab();
        if (lastRenderedTab === "spectrum") runRealFft();
        if (latestState) renderActiveTab(latestState);
      }, 200);
    });
  }

  document.getElementById("btn-jump-console")?.addEventListener("click", () => {
    activateTab("overview");
    document.getElementById("cli-console")?.scrollIntoView({ behavior: "smooth", block: "center" });
  });

  // Alinhamento rapido TX + RX Modo 2 / Modo 1
  const alignModes = async (mode) => {
    if (latestState?.connections?.tx?.status === "connected") await sendFirmwareCommand("tx", `/mode ${mode}`);
    if (latestState?.connections?.rx?.status === "connected") await sendFirmwareCommand("rx", `/mode ${mode}`);
  };
  document.getElementById("btn-align-mode2")?.addEventListener("click", () => alignModes(2));
  document.getElementById("btn-banner-align-m2")?.addEventListener("click", () => alignModes(2));
  document.getElementById("btn-align-mode1")?.addEventListener("click", () => alignModes(1));

  // Macro sequencial real em 1 clique
  document.getElementById("btn-run-macro")?.addEventListener("click", async () => {
    const stepsEl = Array.from(document.querySelectorAll("#macro-steps .macro-step"));
    const setStep = (i, cls, text) => {
      const el = stepsEl[i];
      const lbl = document.getElementById(`macro-s${i}`);
      if (el) el.className = `macro-step ${cls}`;
      if (lbl) lbl.textContent = text;
    };
    const txConn = latestState?.connections?.tx?.status === "connected";
    const rxConn = latestState?.connections?.rx?.status === "connected";

    setStep(0, "step-run", "Executando...");
    if (txConn) {
      await sendFirmwareCommand("tx", "/selftest");
      setStep(0, "step-ok", "Enviado");
    } else {
      setStep(0, "step-err", "TX desconect.");
    }
    await new Promise((r) => setTimeout(r, 450));

    setStep(1, "step-run", "Executando...");
    if (rxConn) {
      await sendFirmwareCommand("rx", "/selftest");
      setStep(1, "step-ok", "Enviado");
    } else {
      setStep(1, "step-err", "RX desconect.");
    }
    await new Promise((r) => setTimeout(r, 450));

    setStep(2, "step-run", "Consultando...");
    if (txConn) await sendFirmwareCommand("tx", "/status");
    if (rxConn) await sendFirmwareCommand("rx", "/status");
    setStep(2, (txConn || rxConn) ? "step-ok" : "step-err", (txConn || rxConn) ? "Atualizado" : "Sem placas");
    await new Promise((r) => setTimeout(r, 350));

    setStep(3, "step-run", "Capturando...");
    if (rxConn) {
      await sendFirmwareCommand("rx", "/cap");
      setStep(3, "step-ok", "2048 amostras");
    } else {
      setStep(3, "step-err", "Sem RX");
    }
  });

  // Re-renderizar graficos Canvas ao trocar tema (Cyber / SCADA / Claro)
  window.addEventListener("qam-theme-change", () => {
    if (lastRenderedTab === "filters") updateRcLab();
    if (lastRenderedTab === "spectrum") runRealFft();
    if (latestState) renderActiveTab(latestState);
  });

  // Command Palette (Ctrl+K / Cmd+K)
  setupCommandPalette();

  // Redimensionamento responsivo dos graficos Canvas e altura exata do header sticky
  const topbarEl = document.querySelector(".topbar");
  const syncTopbarH = () => {
    if (topbarEl) {
      document.documentElement.style.setProperty("--topbar-h", `${topbarEl.offsetHeight}px`);
    }
  };
  syncTopbarH();
  if (topbarEl && typeof ResizeObserver !== "undefined") {
    new ResizeObserver(syncTopbarH).observe(topbarEl);
  }

  let resizeTimer = null;
  window.addEventListener("resize", () => {
    syncTopbarH();
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => {
      if (lastRenderedTab === "filters") updateRcLab();
      if (lastRenderedTab === "spectrum") runRealFft();
      if (latestState) renderActiveTab(latestState);
    }, 120);
  });

  // COM Connections
  document.getElementById("btn-refresh-ports").addEventListener("click", refreshComPorts);

  ["tx", "rx"].forEach((role) => {
    document.getElementById(`btn-${role}-conn`).addEventListener("click", async () => {
      showConnError(null);
      const isConn = latestState?.connections?.[role]?.status === "connected";
      if (isConn) {
        await apiPost("/api/disconnect", { role });
      } else {
        const port = document.getElementById(`sel-${role}-port`).value;
        const baudrate = parseInt(document.getElementById(`sel-${role}-baud`).value, 10);
        if (!port) { showConnError(`Selecione uma porta COM válida para ${role.toUpperCase()}.`); return; }
        const res = await apiPost("/api/connect", { role, port, baudrate });
        if (!res.ok) showConnError(res.error);
      }
      latestState = await (await fetch("/api/state")).json();
      renderActiveTab(latestState);
    });
  });

  // Topbar actions
  document.getElementById("btn-toggle-acq").addEventListener("click", async (e) => {
    const next = !(latestState?.acquisition_enabled ?? true);
    await apiPost("/api/acquisition", { enabled: next });
    e.target.textContent = `Aquisição: ${next ? "ATIVA" : "PARADA"}`;
  });

  document.getElementById("btn-toggle-pause").addEventListener("click", (e) => {
    viewPaused = !viewPaused;
    e.target.textContent = viewPaused ? "Retomar Visão" : "Pausar Visão";
    e.target.classList.toggle("btn-warn", viewPaused);
  });

  document.getElementById("btn-clear-plots").addEventListener("click", async () => {
    await apiPost("/api/clear_plots");
    latestState = await (await fetch("/api/state")).json();
    renderActiveTab(latestState);
  });

  document.getElementById("btn-offline-sim").addEventListener("click", async () => {
    const res = await apiPost("/api/offline_test", { text: "4QAM-OK!" });
    latestState = res.state;
    renderActiveTab(latestState);
    if (lastRenderedTab === "spectrum") runRealFft();
  });

  // Send text / commands
  document.getElementById("btn-send-msg").addEventListener("click", () => {
    const txt = document.getElementById("inp-tx-msg").value.trim();
    if (!txt) return;
    sendFirmwareCommand("tx", txt.startsWith("/") ? `/send ${txt}` : txt);
  });

  const inpCmd = document.getElementById("inp-raw-cmd");
  const cmdTarget = document.getElementById("cmd-target");
  inpCmd.addEventListener("input", () => {
    document.getElementById("cmd-preview").textContent = `Comando pronto para ${cmdTarget.value.toUpperCase()}: "${inpCmd.value.trim()}"`;
  });
  document.getElementById("btn-send-cmd").addEventListener("click", () => {
    sendFirmwareCommand(cmdTarget.value, inpCmd.value);
  });
  document.querySelectorAll("button[data-cmd]").forEach((btn) => {
    btn.addEventListener("click", () => {
      sendFirmwareCommand(btn.getAttribute("data-role"), btn.getAttribute("data-cmd"));
    });
  });

  // Scope / PWM / Const / FFT / Filters controls
  ["chk-scope-raw", "chk-scope-ac", "chk-scope-filt", "chk-scope-ref", "chk-scope-diff", "chk-scope-pwm", "chk-scope-carrier", "chk-scope-bb", "chk-scope-align", "sel-scope-win", "sel-scope-xaxis"].forEach((id) => {
    document.getElementById(id)?.addEventListener("change", () => scopeView.render(latestState));
  });
  document.getElementById("btn-scope-preset-mod")?.addEventListener("click", () => {
    const raw = document.getElementById("chk-scope-raw");
    const ref = document.getElementById("chk-scope-ref");
    const car = document.getElementById("chk-scope-carrier");
    const bb = document.getElementById("chk-scope-bb");
    const pwm = document.getElementById("chk-scope-pwm");
    const winSel = document.getElementById("sel-scope-win");
    if (raw) raw.checked = Boolean(latestState?.is_offline_test);
    if (ref) ref.checked = true;
    if (car) car.checked = true;
    if (bb) bb.checked = true;
    if (pwm) pwm.checked = false;
    if (winSel) winSel.value = "50";
    scopeView.resetZoom();
  });
  ["sel-pwm-win", "chk-pwm-avg"].forEach((id) => {
    document.getElementById(id)?.addEventListener("change", () => pwmScopeView.render(latestState));
  });
  document.getElementById("rng-pwm-offset")?.addEventListener("input", () => pwmScopeView.render(latestState));
  document.getElementById("btn-scope-reset-zoom").addEventListener("click", () => scopeView.resetZoom());
  document.getElementById("btn-jump-pwm")?.addEventListener("click", async () => {
    activateTab("scope");
    setTimeout(() => {
      document.getElementById("pwm-scope-card")?.scrollIntoView({ behavior: "smooth", block: "center" });
    }, 60);
    if (latestState?.connections?.tx?.status === "connected" && !latestState?.tx_pwm_ref?.cmp_ticks?.length) {
      await sendFirmwareCommand("tx", "/cap");
    }
  });
  document.getElementById("sel-const-stage").addEventListener("change", () => constView.render(latestState));
  document.getElementById("btn-fft-run").addEventListener("click", runRealFft);
  document.getElementById("inp-fft-fmax").addEventListener("change", () => specView.render(specView.lastFft, latestState?.rx_status?.carrier_freq_hz || 1000));

  ["rc-r1", "rc-c1", "rc-r2", "rc-c2", "rc-fpwm", "rc-fc"].forEach((id) => {
    document.getElementById(id).addEventListener("input", updateRcLab);
  });
  document.getElementById("btn-rc-reset").addEventListener("click", () => {
    document.getElementById("rc-r1").value = 1000;
    document.getElementById("rc-c1").value = 33;
    document.getElementById("rc-r2").value = 10000;
    document.getElementById("rc-c2").value = 3.3;
    document.getElementById("rc-fpwm").value = 80000;
    document.getElementById("rc-fc").value = 1000;
    updateRcLab();
  });

  // History & Export
  document.getElementById("btn-save-sess").addEventListener("click", async () => {
    const name = document.getElementById("inp-sess-name").value.trim();
    await apiPost("/api/sessions/save", { name: name || null });
    historyView.refreshList();
  });
  document.getElementById("btn-refresh-sess").addEventListener("click", () => historyView.refreshList());
  document.getElementById("btn-print-report").addEventListener("click", () => historyView.printTechnicalReport(latestState));
}

function setupCommandPalette() {
  const modal = document.getElementById("cmd-palette");
  const input = document.getElementById("cmd-palette-input");
  const list = document.getElementById("cmd-palette-list");
  const backdrop = document.getElementById("cmd-backdrop");
  const closeBtn = document.getElementById("btn-cmd-close");
  const openBtn = document.getElementById("btn-cmd-palette");
  if (!modal || !input || !list) return;

  const actions = [
    { cat: "MÓDULO", label: "Visão Geral & Hardware (Portas COM / Console)", run: () => activateTab("overview") },
    { cat: "MÓDULO", label: "Osciloscópio Digital (Domínio do Tempo)", run: () => activateTab("scope") },
    { cat: "MÓDULO", label: "Constelação 4-QAM (Plano I/Q Real)", run: () => activateTab("constellation") },
    { cat: "MÓDULO", label: "Análise Digital & Ground Truth (BER/SER/EVM)", run: () => activateTab("digital") },
    { cat: "MÓDULO", label: "Laboratório DSP (Costas Loop & Gardner TED)", run: () => activateTab("dsp") },
    { cat: "MÓDULO", label: "Analisador de Espectro FFT", run: () => activateTab("spectrum") },
    { cat: "MÓDULO", label: "Filtros RC Analógicos (Diagrama de Bode)", run: () => activateTab("filters") },
    { cat: "MÓDULO", label: "Fundamentos Matemáticos (Equações ao Vivo)", run: () => activateTab("math") },
    { cat: "MÓDULO", label: "Histórico & Exportação CSV / JSON", run: () => activateTab("history") },
    { cat: "HARDWARE", label: "Executar /selftest no TX", run: () => sendFirmwareCommand("tx", "/selftest") },
    { cat: "HARDWARE", label: "Executar /selftest no RX", run: () => sendFirmwareCommand("rx", "/selftest") },
    { cat: "HARDWARE", label: "Alinhar TX + RX em Modo 2 (CRC-16)", run: async () => { await sendFirmwareCommand("tx", "/mode 2"); await sendFirmwareCommand("rx", "/mode 2"); } },
    { cat: "HARDWARE", label: "Capturar PWM programado do TX (/cap TX)", run: () => sendFirmwareCommand("tx", "/cap") },
    { cat: "HARDWARE", label: "Ver pulsos PWM GPIO 18 no Osciloscópio (janela 0.5 ms)", run: () => document.getElementById("btn-jump-pwm")?.click() },
    { cat: "HARDWARE", label: "Capturar 2048 amostras ADC em 20 kHz (/cap RX)", run: () => sendFirmwareCommand("rx", "/cap") },
    { cat: "SIMULAÇÃO", label: "Executar Teste Offline (Simulação E2E)", run: () => document.getElementById("btn-offline-sim")?.click() },
    { cat: "SIMULAÇÃO", label: "Limpar Gráficos e resetar sessão offline", run: () => document.getElementById("btn-clear-plots")?.click() },
    { cat: "TEMA", label: "Tema: Cyber Escuro (Padrão Obsidian)", run: () => applyTheme("cyber") },
    { cat: "TEMA", label: "Tema: SCADA Alto Contraste (Âmbar/Preto)", run: () => applyTheme("scada") },
    { cat: "TEMA", label: "Tema: Claro (Bancada muito iluminada)", run: () => applyTheme("light") },
  ];

  let selectedIdx = 0;
  let filtered = actions;
  let prevFocus = null;

  const renderList = () => {
    const q = input.value.trim().toLowerCase();
    filtered = actions.filter((a) => `${a.cat} ${a.label}`.toLowerCase().includes(q));
    if (selectedIdx >= filtered.length) selectedIdx = Math.max(0, filtered.length - 1);
    list.innerHTML = filtered.map((a, idx) => (
      `<button type="button" class="cmd-item ${idx === selectedIdx ? "selected" : ""}" role="option" aria-selected="${idx === selectedIdx}" data-idx="${idx}"><div><strong>${escapeHtml(a.label)}</strong><small>${a.cat}</small></div><span class="badge badge-neutral">ENTER</span></button>`
    )).join("") || `<div class="hint" style="padding:12px">Nenhum comando encontrado.</div>`;
    list.querySelectorAll(".cmd-item").forEach((el) => {
      el.addEventListener("click", () => {
        const idx = parseInt(el.getAttribute("data-idx"), 10);
        closeModal();
        filtered[idx]?.run();
      });
    });
  };

  const openModal = () => {
    prevFocus = document.activeElement;
    modal.classList.remove("hidden");
    input.value = "";
    selectedIdx = 0;
    renderList();
    setTimeout(() => input.focus(), 20);
  };

  const closeModal = () => {
    modal.classList.add("hidden");
    if (prevFocus && typeof prevFocus.focus === "function") prevFocus.focus();
  };

  openBtn?.addEventListener("click", openModal);
  closeBtn?.addEventListener("click", closeModal);
  backdrop?.addEventListener("click", closeModal);
  input.addEventListener("input", () => { selectedIdx = 0; renderList(); });
  input.addEventListener("keydown", (e) => {
    if (e.key === "ArrowDown") {
      e.preventDefault();
      if (filtered.length) selectedIdx = (selectedIdx + 1) % filtered.length;
      renderList();
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      if (filtered.length) selectedIdx = (selectedIdx - 1 + filtered.length) % filtered.length;
      renderList();
    } else if (e.key === "Enter" && filtered[selectedIdx]) {
      e.preventDefault();
      const item = filtered[selectedIdx];
      closeModal();
      item.run();
    } else if (e.key === "Escape") {
      e.preventDefault();
      closeModal();
    }
  });

  document.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") {
      e.preventDefault();
      if (modal.classList.contains("hidden")) openModal();
      else closeModal();
    } else if (e.key === "Escape" && !modal.classList.contains("hidden")) {
      closeModal();
    }
  });
}

function connectWebSocket() {
  const proto = window.location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${window.location.host}/ws`);
  ws.onmessage = (ev) => {
    if (viewPaused) return;
    latestState = JSON.parse(ev.data);
    renderActiveTab(latestState);
  };
  ws.onclose = () => {
    setTimeout(connectWebSocket, 1500);
  };
}

async function init() {
  initA11yTheme();
  setupUiEvents();
  const initialTab = new URLSearchParams(window.location.search).get("tab");
  if (initialTab) activateTab(initialTab);
  await refreshComPorts();
  latestState = await (await fetch("/api/state")).json();
  renderActiveTab(latestState);
  updateRcLab();
  connectWebSocket();
}

init();
