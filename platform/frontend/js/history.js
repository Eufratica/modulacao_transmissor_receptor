// Historico de Sessoes, Comparacao Lado a Lado e Relatorio Imprimivel HTML/PDF

export class HistoryView {
  constructor() {
    this.tbody = document.querySelector("#tbl-sessions tbody");
    this.compareEl = document.getElementById("session-compare");
    this.sessionA = null;
    this.sessionB = null;
  }

  async refreshList() {
    const res = await fetch("/api/sessions");
    const data = await res.json();
    const list = data.sessions || [];
    this.tbody.innerHTML = list.length
      ? list.map((s) => `<tr>
          <td><code>${s.session_id}</code></td>
          <td>${(s.created_at || "").replace("T", " ").slice(0, 19)}</td>
          <td>${s.is_offline_test ? '<span class="badge badge-wait">OFFLINE</span>' : '<span class="badge badge-ok">HARDWARE</span>'}</td>
          <td>${s.tx_count}</td>
          <td>${s.rx_count}</td>
          <td>${s.adc_samples}</td>
          <td>
            <button class="btn btn-xs" data-load="A" data-sid="${s.session_id}">Comp A</button>
            <button class="btn btn-xs" data-load="B" data-sid="${s.session_id}">Comp B</button>
          </td>
        </tr>`).join("")
      : `<tr><td colspan="7">Nenhuma sessão salva ainda. Clique em "Salvar Sessão Local".</td></tr>`;

    this.tbody.querySelectorAll("button[data-load]").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const slot = btn.getAttribute("data-load");
        const sid = btn.getAttribute("data-sid");
        const doc = await (await fetch(`/api/sessions/${sid}`)).json();
        if (slot === "A") this.sessionA = doc; else this.sessionB = doc;
        this.renderComparison();
      });
    });
  }

  renderComparison() {
    if (!this.sessionA && !this.sessionB) return;
    const fmtSess = (label, s) => {
      if (!s) return `<div class="card"><h3>Sessão ${label}</h3><p class="hint">Selecione uma sessão na tabela.</p></div>`;
      const lastGt = (s.ground_truth_history || []).slice(-1)[0];
      return `
        <div class="card">
          <h3>${label}: ${s.name}</h3>
          <p class="meta-line">ID: ${s.session_id} | Fonte: ${s.metadata?.is_offline_test ? "Teste Offline" : (s.metadata?.data_source || "—")}</p>
          <div class="kv-grid mt-xs">
            <div class="kv"><span class="kv-k">TX fc / Baud</span><span class="kv-v">${s.config_tx?.carrier_freq_hz ?? "—"} Hz / ${s.config_tx?.symbol_rate_baud ?? "—"}</span></div>
            <div class="kv"><span class="kv-k">Mapeamento / qsign</span><span class="kv-v">${s.config_tx?.map_preset ?? s.config_rx?.map_preset ?? "—"} / ${s.config_tx?.q_sign_minus ? "-sin" : "+sin"}</span></div>
            <div class="kv"><span class="kv-k">Quadros TX / RX</span><span class="kv-v">${(s.tx_frames || []).length} / ${(s.rx_frames || []).length}</span></div>
            <div class="kv"><span class="kv-k">Último BER / SER</span><span class="kv-v">${lastGt?.calculable ? `${lastGt.ber} / ${lastGt.ser}` : "não calculável"}</span></div>
            <div class="kv"><span class="kv-k">Último EVM RMS</span><span class="kv-v">${lastGt?.evm_rms_pct !== null && lastGt?.evm_rms_pct !== undefined ? lastGt.evm_rms_pct + "%" : "—"}</span></div>
            <div class="kv"><span class="kv-k">Amostras ADC Salvas</span><span class="kv-v">${(s.adc_snapshot?.samples_mv || []).length}</span></div>
          </div>
        </div>
      `;
    };
    this.compareEl.innerHTML = `<div class="grid-2">${fmtSess("A", this.sessionA)}${fmtSess("B", this.sessionB)}</div>`;
  }

  printTechnicalReport(state) {
    const win = window.open("", "_blank");
    if (!win) { window.print(); return; }
    const gt = state?.last_ground_truth;
    const tx = state?.tx_status;
    const rx = state?.rx_status;
    win.document.write(`<!DOCTYPE html><html><head><meta charset="utf-8"><title>Relatório Técnico 4-QAM</title>
      <style>body{font-family:Segoe UI,sans-serif;padding:24px;color:#111}table{border-collapse:collapse;width:100%;margin:12px 0}th,td{border:1px solid #999;padding:6px;text-align:left;font-size:12px}code{background:#eee;padding:2px 4px}</style></head><body>
      <h1>Relatório Técnico de Ensaio 4-QAM (ESP32-WROOM-32)</h1>
      <p><b>Data:</b> ${new Date().toLocaleString()} | <b>Fonte dos dados:</b> ${state?.is_offline_test ? "SIMULAÇÃO DE PROTOCOLO OFFLINE (NÃO HARDWARE REAL)" : state?.data_source}</p>
      <h2>Parâmetros do Transmissor e Receptor</h2>
      <table><tr><th>TX fc / Baud / Modo</th><td>${tx?.carrier_freq_hz ?? "—"} Hz / ${tx?.symbol_rate_baud ?? "—"} baud / Modo ${tx?.tx_mode ?? "—"}</td>
      <th>RX fc / Baud / Modo</th><td>${rx?.carrier_freq_hz ?? "—"} Hz / ${rx?.symbol_rate_baud ?? "—"} baud / Modo ${rx?.rx_mode ?? "—"}</td></tr>
      <tr><th>Mapeamento / Quadratura</th><td>${tx?.map_preset ?? "—"} / ${tx?.q_sign_minus ? "-sin" : "+cos(wt-90)"}</td>
      <th>DC RX / Vpk RX / df RX</th><td>${rx?.dc_est_mv ?? "—"} mV / ${rx?.amp_pk_est_mv ?? "—"} mV / ${rx?.freq_offset_hz ?? "—"} Hz</td></tr></table>
      <h2>Resultado Ground Truth</h2>
      ${gt && gt.calculable ? `<ul><li>Mensagem TX: <code>${gt.tx_text}</code></li><li>Mensagem RX: <code>${gt.rx_text}</code></li>
      <li>BER: <b>${gt.ber}</b> (${gt.bit_errors}/${gt.total_bits}) | SER: <b>${gt.ser}</b> | EVM RMS: <b>${gt.evm_rms_pct ?? "—"}%</b> | CRC: <b>${gt.crc_ok ? "OK" : "ERRO"}</b></li></ul>`
      : `<p>Ground Truth não calculável: ${gt?.reason || "sem pares TX/RX comparáveis"}.</p>`}
      <p><i>Gerado localmente pela Central de Instrumentação 4-QAM ESP32 (sem serviços externos).</i></p>
      <script>window.print();</script></body></html>`);
    win.document.close();
  }
}
