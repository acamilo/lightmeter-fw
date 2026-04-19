/*
 * Lightmeter spectrum card — custom Lovelace card for the Acamilo lightmeter.
 *
 * Install:
 *   1. Copy this file to <HA config>/www/lightmeter-card.js
 *   2. Add a Lovelace resource:
 *        Settings -> Dashboards -> (kebab menu) -> Resources -> Add
 *        URL:  /local/lightmeter-card.js
 *        Type: JavaScript Module
 *   3. On your dashboard: Add Card -> Manual -> paste:
 *        type: custom:lightmeter-card
 *
 * Optional config keys:
 *   entity_base:  "sensor.espressif_lightmeter"          (default)
 *   binary_base:  "binary_sensor.espressif_lightmeter"   (default)
 *   title:        "Lightmeter"                           (default)
 */
(() => {
  // Nine bands in the spectrum bar chart: the eight visible F1..F8 channels
  // plus the AS7341's ~910 nm NIR channel. Bar colors approximate sRGB for
  // the visible bands; NIR gets a dark maroon since it's beyond human
  // vision. The NIR bar follows F8 visually with no gap — bars are evenly
  // spaced rather than proportional to wavelength, which keeps the chart
  // compact on narrow HA dashboards.
  const BANDS = [
    { key: "f1_415nm_ppfd", nm: 415, color: "#7a00ff", label: "415" },
    { key: "f2_445nm_ppfd", nm: 445, color: "#0033ff", label: "445" },
    { key: "f3_480nm_ppfd", nm: 480, color: "#00bfff", label: "480" },
    { key: "f4_515nm_ppfd", nm: 515, color: "#00d08c", label: "515" },
    { key: "f5_555nm_ppfd", nm: 555, color: "#a4d000", label: "555" },
    { key: "f6_590nm_ppfd", nm: 590, color: "#ffb300", label: "590" },
    { key: "f7_630nm_ppfd", nm: 630, color: "#ff5a00", label: "630" },
    { key: "f8_680nm_ppfd", nm: 680, color: "#c40000", label: "680" },
    { key: "nir_910nm_pfd", nm: 910, color: "#4a0404", label: "910" },
  ];

  class LightmeterCard extends HTMLElement {
    setConfig(config) {
      this._config = {
        title: "Lightmeter",
        entity_base: "sensor.espressif_lightmeter",
        binary_base: "binary_sensor.espressif_lightmeter",
        ...(config || {}),
      };
    }

    set hass(hass) {
      this._hass = hass;
      this._render();
    }

    getCardSize() { return 5; }

    static getConfigElement() { return null; }
    static getStubConfig() { return {}; }

    _num(id) {
      const s = this._hass?.states?.[id]?.state;
      if (s === undefined || s === "unknown" || s === "unavailable") return null;
      const n = parseFloat(s);
      return Number.isFinite(n) ? n : null;
    }

    _onOff(id) { return this._hass?.states?.[id]?.state; }

    _fmt(v, digits = 2) { return v === null ? "—" : v.toFixed(digits); }

    _render() {
      if (!this._hass) return;
      const E = this._config.entity_base;
      const B = this._config.binary_base;

      const values = BANDS.map((b) => this._num(`${E}_${b.key}`));
      const par   = this._num(`${E}_par_total`);
      const lux   = this._num(`${E}_illuminance`);
      const flicker   = this._onOff(`${B}_flicker_detected`)    === "on";
      const saturated = this._onOff(`${B}_spectral_saturated`)  === "on";

      const validVals = values.filter((v) => v !== null);
      const scaleMax = Math.max(...validVals, 0.001);

      const BW = 34, BS = 8, H = 140, LABELH = 40;
      const W = BANDS.length * (BW + BS) - BS;
      let bars = "", labels = "", ticks = "";
      BANDS.forEach((b, i) => {
        const v = values[i];
        const x = i * (BW + BS);
        if (v !== null) {
          const h = Math.max(1, (v / scaleMax) * H);
          bars += `<rect x="${x}" y="${H - h}" width="${BW}" height="${h}" fill="${b.color}" rx="3"/>`;
          labels += `<text x="${x + BW / 2}" y="${H - h - 4}" class="val">${v.toFixed(2)}</text>`;
        } else {
          bars += `<rect x="${x}" y="${H - 6}" width="${BW}" height="4" fill="var(--disabled-text-color)" rx="2" opacity="0.4"/>`;
          labels += `<text x="${x + BW / 2}" y="${H - 12}" class="val muted">—</text>`;
        }
        ticks += `<text x="${x + BW / 2}" y="${H + 16}" class="tick">${b.label}</text>`;
      });

      const warn = (on, label) => `
        <span class="chip ${on ? "on" : "off"}">
          <span class="dot"></span>${label}
        </span>`;

      this.innerHTML = `
        <ha-card header="${this._config.title}">
          <style>
            .body { padding: 8px 16px 16px; }
            .top { display: flex; justify-content: space-between; gap: 16px; }
            .stat .label { font-size: 0.75em; text-transform: uppercase;
                           letter-spacing: 0.5px; color: var(--secondary-text-color); }
            .stat .val-big { font-size: 2.1em; font-weight: 300; line-height: 1.1; }
            .stat .unit { font-size: 0.45em; margin-left: 4px;
                          color: var(--secondary-text-color); }
            .chart { margin-top: 14px; }
            .chart svg { width: 100%; height: auto; display: block; color: var(--primary-text-color); }
            .val { font-size: 9px; text-anchor: middle; fill: var(--primary-text-color); }
            .val.muted { fill: var(--disabled-text-color); }
            .tick { font-size: 10px; text-anchor: middle; fill: var(--secondary-text-color); }
            .axis { text-align: center; font-size: 0.7em; color: var(--secondary-text-color); }
            .footer { margin-top: 12px; display: flex; justify-content: space-between; align-items: center;
                      font-size: 0.85em; color: var(--secondary-text-color); }
            .chip { display: inline-flex; align-items: center; gap: 6px; padding: 3px 10px;
                    border-radius: 12px; font-size: 0.8em; }
            .chip .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--disabled-text-color); }
            .chip.on { background: rgba(255, 90, 0, 0.15); color: var(--error-color); }
            .chip.on .dot { background: var(--error-color); }
            .chip.off { background: transparent; }
            .footer-chips { display: flex; gap: 8px; }
          </style>
          <div class="body">
            <div class="top">
              <div class="stat">
                <div class="label">PAR</div>
                <div class="val-big">${this._fmt(par, 2)}<span class="unit">µmol/m²/s</span></div>
              </div>
              <div class="stat" style="text-align: right;">
                <div class="label">Illuminance</div>
                <div class="val-big">${this._fmt(lux, 0)}<span class="unit">lx</span></div>
              </div>
            </div>
            <div class="chart">
              <svg viewBox="0 0 ${W} ${H + LABELH}" preserveAspectRatio="xMidYMid meet">
                ${bars}
                ${labels}
                ${ticks}
              </svg>
            </div>
            <div class="footer">
              <div>wavelength (nm) — last bar is NIR outside the PAR band</div>
              <div class="footer-chips">
                ${warn(flicker,   "flicker")}
                ${warn(saturated, "saturated")}
              </div>
            </div>
          </div>
        </ha-card>
      `;
    }
  }

  customElements.define("lightmeter-card", LightmeterCard);

  window.customCards = window.customCards || [];
  window.customCards.push({
    type: "lightmeter-card",
    name: "Lightmeter Spectrum",
    description: "AS7341 spectral readings with PAR / lux / flicker / saturation indicators",
    preview: false,
  });
})();
