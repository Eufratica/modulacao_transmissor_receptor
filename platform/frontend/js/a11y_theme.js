// Sistema compartilhado de Temas (Cyber / SCADA / Claro) e Acessibilidade (Fonte / Foco / Canvas Palette)

const THEME_KEY = "qam_lab_theme";
const FONT_KEY = "qam_lab_font";

export function applyTheme(mode) {
  const valid = ["cyber", "scada", "light"].includes(mode) ? mode : "cyber";
  document.body.classList.remove("theme-scada-industrial", "theme-light");
  if (valid === "scada") document.body.classList.add("theme-scada-industrial");
  if (valid === "light") document.body.classList.add("theme-light");
  try { localStorage.setItem(THEME_KEY, valid); } catch {}

  document.querySelectorAll("[data-set-theme]").forEach((el) => {
    const active = el.getAttribute("data-set-theme") === valid;
    el.classList.toggle("active", active);
    el.setAttribute("aria-pressed", active ? "true" : "false");
  });

  const labelEl = document.getElementById("a11y-theme-label");
  if (labelEl) {
    labelEl.textContent = valid === "light" ? "Claro" : valid === "scada" ? "SCADA" : "Cyber";
  }
  window.dispatchEvent(new CustomEvent("qam-theme-change", { detail: { theme: valid } }));
}

export function applyFontScale(scale) {
  const valid = ["sm", "md", "lg"].includes(scale) ? scale : "md";
  document.documentElement.classList.remove("font-sm", "font-md", "font-lg");
  document.documentElement.classList.add(`font-${valid}`);
  try { localStorage.setItem(FONT_KEY, valid); } catch {}

  document.querySelectorAll("[data-set-font]").forEach((el) => {
    const active = el.getAttribute("data-set-font") === valid;
    el.classList.toggle("active", active);
    el.setAttribute("aria-pressed", active ? "true" : "false");
  });
}

export function getCanvasPalette() {
  const cs = getComputedStyle(document.body);
  return {
    bg: (cs.getPropertyValue("--canvas-bg") || "#020306").trim(),
    grid: (cs.getPropertyValue("--canvas-grid") || "#111c2f").trim(),
    axis: (cs.getPropertyValue("--canvas-axis") || "#8a99b2").trim(),
    zero: (cs.getPropertyValue("--canvas-zero") || "#2b3b55").trim(),
    textStrong: (cs.getPropertyValue("--text-strong") || "#f1f5f9").trim(),
    accentCyan: (cs.getPropertyValue("--cyan-soft") || "#38bdf8").trim(),
    accentOk: (cs.getPropertyValue("--ok-bright") || "#34d399").trim(),
    accentWarn: (cs.getPropertyValue("--warn") || "#f59e0b").trim(),
    accentErr: (cs.getPropertyValue("--err") || "#fb7185").trim(),
    isLight: document.body.classList.contains("theme-light"),
  };
}

export function initA11yTheme() {
  let savedTheme = "cyber";
  let savedFont = "md";
  try {
    savedTheme = localStorage.getItem(THEME_KEY) || "cyber";
    savedFont = localStorage.getItem(FONT_KEY) || "md";
  } catch {}
  applyTheme(savedTheme);
  applyFontScale(savedFont);

  const btn = document.getElementById("btn-a11y-menu");
  const pop = document.getElementById("a11y-popover");
  if (btn && pop) {
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      const willOpen = pop.classList.toggle("hidden") === false;
      btn.setAttribute("aria-expanded", willOpen ? "true" : "false");
    });
    document.addEventListener("click", (e) => {
      if (!pop.classList.contains("hidden") && !pop.contains(e.target) && e.target !== btn) {
        pop.classList.add("hidden");
        btn.setAttribute("aria-expanded", "false");
      }
    });
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && !pop.classList.contains("hidden")) {
        pop.classList.add("hidden");
        btn.setAttribute("aria-expanded", "false");
        btn.focus();
      }
    });
  }

  document.querySelectorAll("[data-set-theme]").forEach((el) => {
    el.addEventListener("click", () => applyTheme(el.getAttribute("data-set-theme")));
  });
  document.querySelectorAll("[data-set-font]").forEach((el) => {
    el.addEventListener("click", () => applyFontScale(el.getAttribute("data-set-font")));
  });
}
