// Controlador leve da Landing Page (/) — reflete estado real do backend sem inventar dados
import { initA11yTheme } from "./a11y_theme.js?v=pwm3";

initA11yTheme();

async function syncRealStatus() {
  try {
    const res = await fetch("/api/state");
    if (!res.ok) return;
    const st = await res.json();

    const txConn = st.connections?.tx?.status === "connected";
    const rxConn = st.connections?.rx?.status === "connected";
    const anyHw = txConn || rxConn;

    const pill = document.getElementById("landing-hw-pill");
    const heroBadge = document.getElementById("hero-state-badge");
    const heroDetail = document.getElementById("hero-state-detail");
    const footerStatus = document.getElementById("footer-status");

    if (st.is_offline_test) {
      pill.className = "badge badge-wait";
      pill.textContent = "Teste Offline Ativo";
      heroBadge.className = "badge badge-wait";
      heroBadge.textContent = "Modo Teste Offline";
      heroDetail.textContent = "Sessão offline de protocolo ativa no laboratório (/lab).";
      footerStatus.textContent = "Modo Teste Offline";
    } else if (anyHw) {
      const ports = [];
      if (txConn) ports.push(`TX=${st.connections.tx.port}`);
      if (rxConn) ports.push(`RX=${st.connections.rx.port}`);
      pill.className = "badge badge-ok";
      pill.textContent = `Conectado (${ports.join(" · ")})`;
      heroBadge.className = "badge badge-ok";
      heroBadge.textContent = `Hardware Conectado (${ports.join(" · ")})`;
      heroDetail.textContent = "Dispositivos ESP32 ativos via porta serial COM. Acesse /lab para acompanhar.";
      footerStatus.textContent = `Hardware Ativo (${ports.join(", ")})`;
    } else {
      pill.className = "badge badge-wait";
      pill.textContent = "Aguardando hardware";
      heroBadge.className = "badge badge-wait";
      heroBadge.textContent = "Aguardando hardware";
      heroDetail.textContent = "Nenhum ESP32 conectado nesta sessão — nenhuma medição fictícia é exibida.";
      footerStatus.textContent = "Aguardando Hardware";
    }

    // Secao Ground Truth da Landing reflete dados reais SOMENTE se nao estiver aguardando hardware
    const gt = st.last_ground_truth;
    const txF = st.last_tx_frame;
    const rxF = st.last_rx_frame;
    if (txF || rxF) {
      document.getElementById("lnd-gt-tx-text").textContent = txF ? `"${txF.payload_text}"` : "AGUARDANDO DADOS DO HARDWARE";
      document.getElementById("lnd-gt-tx-bits").textContent = txF ? `${txF.bit_order} · ${txF.payload_len * 8} bits` : "—";
      document.getElementById("lnd-gt-tx-syms").textContent = txF ? `${txF.payload_len * 4} símbolos (${txF.map_preset})` : "—";
      document.getElementById("lnd-gt-tx-iq").textContent = txF ? `${txF.q_sign_minus ? "-sin" : "+cos(wt-90)"} · fc=${txF.carrier_freq_hz}Hz` : "—";

      document.getElementById("lnd-gt-rx-text").textContent = rxF ? `"${rxF.payload_text}"` : "AGUARDANDO DADOS DO HARDWARE";
      document.getElementById("lnd-gt-rx-bits").textContent = rxF ? `${rxF.payload_order} · ${rxF.payload_len * 8} bits` : "—";
      document.getElementById("lnd-gt-rx-syms").textContent = rxF ? `Rotação ${rxF.locked_rot_deg}° · df=${(rxF.df_est_hz ?? 0).toFixed(2)}Hz` : "—";
      document.getElementById("lnd-gt-rx-crc").textContent = rxF ? (rxF.crc_ok ? "CRC OK" : "CRC ERRO") : "—";

      const stBadge = document.getElementById("lnd-gt-status");
      const stNote = document.getElementById("lnd-gt-note");
      if (gt && gt.calculable) {
        stBadge.className = st.is_offline_test ? "badge badge-wait" : "badge badge-ok";
        stBadge.textContent = st.is_offline_test ? "TESTE OFFLINE" : "COMPARAÇÃO ATIVA";
        stNote.textContent = `BER=${gt.ber.toExponential(2)} | SER=${gt.ser.toExponential(2)} | EVM=${gt.evm_rms_pct ?? "—"}%`;
      }
    }
  } catch {
    // Silencioso se backend reiniciar
  }
}

document.getElementById("btn-mobile-nav")?.addEventListener("click", () => {
  document.getElementById("site-nav")?.classList.toggle("open");
});

document.querySelectorAll("#site-nav a").forEach((a) => {
  a.addEventListener("click", () => {
    document.getElementById("site-nav")?.classList.remove("open");
  });
});

syncRealStatus();
setInterval(syncRealStatus, 4000);
