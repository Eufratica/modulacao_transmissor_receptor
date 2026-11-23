// Analise Digital & Ground Truth (Cadeia Completa Rastreavel + BER / SER / EVM)

export class DigitalTruthView {
  constructor(onSelectSymbol) {
    this.badgeEl = document.getElementById("gt-badge");
    this.summaryEl = document.getElementById("gt-summary");
    this.noteEl = document.getElementById("gt-note");
    this.pipeEl = document.getElementById("digital-pipeline");
    this.tbody = document.querySelector("#tbl-digital-chain tbody");
    this.onSelectSymbol = onSelectSymbol;
    this.selectedSym = null;
  }

  render(state) {
    const gt = state?.last_ground_truth;
    const txF = state?.last_tx_frame;
    const rxF = state?.last_rx_frame;

    if (!txF && !rxF) {
      this.badgeEl.className = "badge badge-wait";
      this.badgeEl.textContent = "Aguardando transmissão e recepção";
      this.summaryEl.innerHTML = `<div class="kv"><span class="kv-k">Estado Ground Truth</span><span class="kv-v">Aguardando hardware / quadros</span></div>`;
      this.pipeEl.innerHTML = `<div class="pipe-step"><strong>Cadeia 4-QAM</strong>Transmita um texto pelo ESP32 TX para inspecionar texto → ASCII → bits → símbolos → ADC → texto RX.</div>`;
      this.tbody.innerHTML = "";
      return;
    }

    if (!gt || !gt.calculable) {
      this.badgeEl.className = "badge badge-wait";
      this.badgeEl.textContent = gt ? "Não calculável / Parcial" : "Aguardando par RX";
      this.summaryEl.innerHTML = `
        <div class="kv"><span class="kv-k">Mensagem TX</span><span class="kv-v" style="color:var(--tx)">${txF ? `"${txF.payload_text}"` : "não disponível"}</span></div>
        <div class="kv"><span class="kv-k">Mensagem RX</span><span class="kv-v" style="color:var(--rx)">${rxF ? `"${rxF.payload_text}"` : "não disponível"}</span></div>
        <div class="kv"><span class="kv-k">BER / SER / EVM</span><span class="kv-v" style="color:#fbbf24">Não calculável</span></div>
        <div class="kv"><span class="kv-k">Motivo Técnico</span><span class="kv-v">${gt?.reason || "Aguardando quadro correspondente do receptor."}</span></div>
      `;
    } else {
      const zeroErr = gt.bit_errors === 0 && gt.crc_ok;
      this.badgeEl.className = zeroErr ? "badge badge-ok" : "badge badge-err";
      this.badgeEl.textContent = zeroErr
        ? "Recuperado sem erros (BER=0)"
        : `${gt.bit_errors} erro(s) de bit | SER=${(gt.ser * 100).toFixed(2)}%`;

      this.summaryEl.innerHTML = `
        <div class="kv"><span class="kv-k">Original TX</span><span class="kv-v" style="color:var(--tx)">"${gt.tx_text}"</span></div>
        <div class="kv"><span class="kv-k">Recebido RX</span><span class="kv-v" style="color:var(--rx)">"${gt.rx_text}"</span></div>
        <div class="kv"><span class="kv-k">Bytes Iguais</span><span class="kv-v">${gt.bytes_equal}/${gt.bytes_total} (${gt.byte_match_pct}%)</span></div>
        <div class="kv"><span class="kv-k">BER (Bits Errados)</span><span class="kv-v">${gt.ber.toExponential(2)} (${gt.bit_errors}/${gt.total_bits})</span></div>
        <div class="kv"><span class="kv-k">SER (Símbolos Errados)</span><span class="kv-v">${gt.ser.toExponential(2)} (${gt.symbol_errors}/${gt.total_symbols})</span></div>
        <div class="kv"><span class="kv-k">EVM RMS</span><span class="kv-v">${gt.evm_rms_pct !== null ? gt.evm_rms_pct + "%" : "não disponível"}</span></div>
        <div class="kv"><span class="kv-k">CRC RX</span><span class="kv-v">${gt.crc_ok ? "CRC OK" : "CRC ERRO"} (0x${(gt.crc_rx ?? 0).toString(16).toUpperCase().padStart(4, "0")})</span></div>
        <div class="kv"><span class="kv-k">Mapeamento / Ordem / Rot.</span><span class="kv-v">${gt.map_preset} · ${gt.bit_order_tx} · ${gt.locked_rot_deg}°</span></div>
      `;
      this.noteEl.textContent = (gt.evm_note || "") + " Atenção: CRC OK valida integridade do quadro decodificado no RX, mas não prova BER=0 sem comparação direta com o TX.";
    }

    // Cadeia completa rastreável
    const chain = txF?.chain;
    const payRows = chain?.payload_rows || [];
    const bytesHex = payRows.map((r) => r.hex).join(" ");
    const bitsAll = payRows.map((r) => r.bits).join(" ");
    const symsStr = (chain?.payload_symbols || []).map((s) => `${s.sym_val}(${s.bits})`).slice(0, 16).join(" ");
    const rxHex = (rxF?.payload_bytes || []).map((b) => "0x" + b.toString(16).toUpperCase().padStart(2, "0")).join(" ");

    this.pipeEl.innerHTML = `
      <div class="pipe-step"><strong>1. Texto TX</strong>${txF ? `"${txF.payload_text}"` : "—"}</div>
      <div class="pipe-step"><strong>2. Bytes ASCII</strong>${bytesHex || "—"}</div>
      <div class="pipe-step"><strong>3. Bits (${txF?.bit_order || "—"})</strong>${bitsAll.slice(0, 44)}${bitsAll.length > 44 ? "..." : ""}</div>
      <div class="pipe-step"><strong>4. Símbolos (${txF?.map_preset || "—"})</strong>${symsStr || "—"}</div>
      <div class="pipe-step"><strong>5. ADC RX</strong>${state.scope?.available ? `${state.scope.stats?.count} am. @ ${state.scope.sample_rate_hz}Hz` : "Aguardando"}</div>
      <div class="pipe-step"><strong>6. Texto RX</strong>${rxF ? `"${rxF.payload_text}" (${rxHex})` : "—"}</div>
    `;

    // Tabela símbolo-a-símbolo (com comparação real quando gt.rows existir, ou referência TX enquanto RX não fecha)
    const rows = gt?.rows || (chain?.payload_symbols || []).map((s, idx) => ({
      sym_idx: idx, byte_idx: s.byte_idx, tx_byte: s.byte_val, rx_byte: null,
      tx_bits: s.bits, rx_bits: "—", tx_sym: s.sym_val, rx_sym: null,
      i_exp: s.i_exp, q_exp: s.q_exp, i_rx: null, q_rx: null, ok: null,
    }));

    this.tbody.innerHTML = rows.slice(0, 160).map((r) => {
      const cls = r.ok === false ? "row-err" : (r.ok === true ? "row-ok" : "");
      const sel = (this.selectedSym === r.sym_idx) ? " row-selected" : "";
      const ch = r.tx_byte >= 32 && r.tx_byte <= 126 ? `'${String.fromCharCode(r.tx_byte)}'` : ".";
      return `<tr class="${cls}${sel}" data-sym="${r.sym_idx}">
        <td>${r.sym_idx}</td>
        <td>0x${r.tx_byte.toString(16).toUpperCase().padStart(2, "0")} (${ch})</td>
        <td><code>${r.tx_bits}</code></td>
        <td>${r.tx_sym}</td>
        <td>${r.i_exp > 0 ? "+1" : "-1"}</td>
        <td>${r.q_exp > 0 ? "+1" : "-1"}</td>
        <td>${r.i_rx !== null && r.i_rx !== undefined ? r.i_rx.toFixed(3) : "—"}</td>
        <td>${r.q_rx !== null && r.q_rx !== undefined ? r.q_rx.toFixed(3) : "—"}</td>
        <td>${r.rx_sym !== null ? `${r.rx_sym} (${r.rx_bits})` : "aguardando"}</td>
        <td>${r.ok === true ? "OK" : (r.ok === false ? "ERRO" : "—")}</td>
      </tr>`;
    }).join("");

    this.tbody.querySelectorAll("tr[data-sym]").forEach((tr) => {
      tr.addEventListener("click", () => {
        const sIdx = parseInt(tr.getAttribute("data-sym"), 10);
        this.selectedSym = sIdx;
        if (this.onSelectSymbol) this.onSelectSymbol(sIdx);
        this.render(state);
      });
    });
  }
}
