// Garage Monitor Hotkey Layout builder
//
// Visual editor for the M5Paper hotkey pad's button grid. Twelve grid slots
// are provisioned in HA (input_text.garage_monitor_slot_0..11) as a fixed
// pool; input_text.garage_monitor_layout_config picks how many of them are
// actually active (cols x rows, up to 12) and the pad's screen orientation.
// Deliberately avoids ha-entity-picker/ha-dialog/mwc-button - HA lazy-loads
// a lot of its polished frontend components and a standalone custom card
// can end up with an inert, non-upgraded element if nothing else on the
// page has already triggered that component's chunk to load. Plain
// <select>/<input> and native <button> sidestep that entirely. <ha-card>
// and <ha-icon> are foundational/always-loaded, so those are used directly.
//
// Action targeting is domain -> entity + domain -> service (both driven
// off hass.states / hass.services, no picker components needed), rather
// than a flat single "all entities" dropdown, which would be enormous and
// unusable as a single list.
//
// Command-only, no active-state highlighting: buttons fire an action, full
// stop. The earlier "Highlight When Active" matchType/match feature (and
// the matching pad-side/BLE-side logic) was removed as unnecessary latency
// for a feature that wasn't worth the wait.

const MAX_SLOTS = 12;
const LAYOUT_ENTITY = "input_text.garage_monitor_layout_config";

// Must match the curated bitmap set actually baked into the firmware
// (m5paper-hotkey/icons.h) - anything outside this list silently falls
// back to text-only on the physical device, so the picker is constrained
// to exactly what's supported rather than being free text. Regenerate
// this list (grep icons.h for "mdi:...") if the icon set changes.
const AVAILABLE_ICONS = [
  "mdi:air-conditioner", "mdi:bell", "mdi:bell-off", "mdi:camera", "mdi:camera-off",
  "mdi:door", "mdi:door-open", "mdi:fan", "mdi:fan-off", "mdi:fire",
  "mdi:garage", "mdi:garage-open", "mdi:gesture-tap-button", "mdi:help-circle-outline",
  "mdi:home", "mdi:home-outline", "mdi:laptop", "mdi:lightbulb", "mdi:lightbulb-off",
  "mdi:lock", "mdi:lock-open", "mdi:monitor", "mdi:monitor-off", "mdi:pause", "mdi:play",
  "mdi:power", "mdi:power-plug", "mdi:power-plug-off", "mdi:refresh", "mdi:robot-vacuum",
  "mdi:speaker", "mdi:speaker-off", "mdi:stop", "mdi:television", "mdi:television-off",
  "mdi:thermostat", "mdi:toggle-switch", "mdi:toggle-switch-off", "mdi:video-input-hdmi",
  "mdi:view-split-vertical", "mdi:volume-high", "mdi:volume-mute", "mdi:volume-off",
  "mdi:water", "mdi:water-boiler", "mdi:weather-night", "mdi:white-balance-sunny",
];

class GarageMonitorHotkeyCard extends HTMLElement {
  setConfig(config) {
    this._config = config || {};
    this._slots = new Array(MAX_SLOTS).fill(null);
    this._raw = new Array(MAX_SLOTS).fill("");
    this._layout = { orientation: "landscape", cols: 3, rows: 2, fontSize: 2, headerText: "Garage Monitor", headerFontSize: 2 };
    this._layoutRaw = "";
    this._editingSlot = null;
    this._mode = "view";
    this._built = false;
  }

  set hass(hass) {
    this._hass = hass;
    if (!this._built) {
      this._buildDom();
      this._built = true;
    }
    this._syncFromHass();
  }

  getCardSize() {
    return 8;
  }

  _entityId(i) {
    return `input_text.garage_monitor_slot_${i}`;
  }

  _defaultSlot() {
    return { label: "", icon: "mdi:help-circle-outline", domain: "", service: "", entity: "" };
  }

  _activeCount() {
    const n = (this._layout.cols || 1) * (this._layout.rows || 1);
    return Math.max(1, Math.min(MAX_SLOTS, n));
  }

  _syncFromHass() {
    let changed = false;
    for (let i = 0; i < MAX_SLOTS; i++) {
      const st = this._hass.states[this._entityId(i)];
      const raw = st ? st.state : "";
      if (raw !== this._raw[i]) {
        this._raw[i] = raw;
        try {
          this._slots[i] = { ...this._defaultSlot(), ...JSON.parse(raw) };
        } catch (e) {
          this._slots[i] = this._defaultSlot();
        }
        changed = true;
      }
    }
    const layoutSt = this._hass.states[LAYOUT_ENTITY];
    const layoutRaw = layoutSt ? layoutSt.state : "";
    if (layoutRaw !== this._layoutRaw) {
      this._layoutRaw = layoutRaw;
      try {
        this._layout = { ...this._layout, ...JSON.parse(layoutRaw) };
      } catch (e) {
        // keep previous layout on parse failure
      }
      this._syncLayoutForm();
      changed = true;
    }
    if (changed) this._renderGrid();
  }

  _buildDom() {
    this.innerHTML = `
      <ha-card>
        <style>
          .gm-wrap { padding: 16px; }
          .gm-panel-header { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 4px; }
          .gm-panel-title { font-size: 1.2em; font-weight: 500; color: var(--primary-text-color, #000); overflow-wrap: break-word; }
          .gm-mode-toggle {
            flex-shrink: 0;
            padding: 6px 14px;
            border-radius: 4px;
            border: 1px solid var(--divider-color, #ccc);
            background: var(--card-background-color, #fff);
            color: var(--primary-color, #03a9f4);
            cursor: pointer;
          }
          .gm-layout-settings { display: none; margin-top: 12px; }
          .gm-layout-settings.open { display: block; }
          .gm-grid { display: grid; gap: 10px; margin-top: 8px; }
          .gm-tile {
            position: relative;
            min-width: 0;
            box-sizing: border-box;
            border: 1px solid var(--divider-color, #ccc);
            border-radius: 8px;
            padding: 12px;
            text-align: center;
            cursor: pointer;
            background: var(--card-background-color, #fff);
            color: var(--primary-text-color, #000);
            user-select: none;
            overflow: hidden;
          }
          .gm-tile-controls {
            position: absolute;
            top: 2px;
            right: 2px;
            display: flex;
            flex-direction: column;
          }
          .gm-tile-controls button {
            width: 22px;
            height: 18px;
            line-height: 1;
            padding: 0;
            border: none;
            background: transparent;
            color: var(--secondary-text-color, #888);
            cursor: pointer;
          }
          .gm-tile-controls button:hover:not(:disabled) { color: var(--primary-color, #03a9f4); }
          .gm-tile-controls button:disabled { opacity: 0.25; cursor: default; }
          .gm-tile.gm-inert { cursor: default; }
          .gm-tile .gm-icon { font-size: var(--gm-icon-px, 28px); --mdc-icon-size: var(--gm-icon-px, 28px); max-width: 100%; }
          .gm-tile .gm-label {
            margin-top: 6px;
            font-size: var(--gm-label-px, 13px);
            min-height: 16px;
            overflow-wrap: break-word;
          }
          .gm-edit { margin-top: 16px; border-top: 1px solid var(--divider-color, #ccc); padding-top: 12px; display: none; }
          .gm-edit.open { display: block; }
          .gm-row { display: flex; align-items: center; margin-bottom: 8px; gap: 8px; }
          .gm-row label { width: 90px; font-size: 13px; flex-shrink: 0; }
          .gm-row input, .gm-row select {
            flex: 1;
            min-width: 0;
            max-width: 100%;
            padding: 6px;
            background: var(--card-background-color, #fff);
            color: var(--primary-text-color, #000);
            border: 1px solid var(--divider-color, #ccc);
            border-radius: 4px;
            overflow: hidden;
            text-overflow: ellipsis;
            white-space: nowrap;
          }
          .gm-row select:disabled { opacity: 0.5; }
          .gm-row input[type="number"] { flex: 0 0 70px; }
          .gm-actions { margin-top: 10px; display: flex; gap: 8px; justify-content: flex-end; }
          .gm-actions button {
            padding: 8px 16px;
            border-radius: 4px;
            border: 1px solid var(--divider-color, #ccc);
            background: var(--card-background-color, #fff);
            color: var(--primary-text-color, #000);
            cursor: pointer;
          }
          .gm-actions button.gm-save {
            background: var(--primary-color, #03a9f4);
            color: var(--text-primary-color, #fff);
            border-color: var(--primary-color, #03a9f4);
          }
          .gm-hint { font-size: 12px; color: var(--secondary-text-color, #888); margin-top: 10px; }
          .gm-section-label { font-size: 12px; font-weight: 600; color: var(--secondary-text-color, #888); margin: 12px 0 4px; }
        </style>
        <div class="gm-wrap">
          <div class="gm-panel-header">
            <div class="gm-panel-title">Garage Monitor</div>
            <button class="gm-mode-toggle" type="button">Edit Layout</button>
          </div>

          <div class="gm-layout-settings">
            <div class="gm-section-label">Pad Layout</div>
            <div class="gm-row"><label>Orientation</label>
              <select class="gm-f-orientation">
                <option value="landscape">Landscape</option>
                <option value="portrait">Portrait</option>
              </select>
            </div>
            <div class="gm-row"><label>Columns</label><input class="gm-f-cols" type="number" min="1" max="4"></div>
            <div class="gm-row"><label>Rows</label><input class="gm-f-rows" type="number" min="1" max="4"></div>
            <div class="gm-row"><label>Font Size</label>
              <select class="gm-f-fontsize">
                <option value="1">Small</option>
                <option value="2">Medium</option>
                <option value="3">Large</option>
                <option value="4">Extra Large</option>
              </select>
            </div>
            <div class="gm-row"><label>Header Text</label><input class="gm-f-headertext" type="text" placeholder="Garage Monitor"></div>
            <div class="gm-row"><label>Header Size</label>
              <select class="gm-f-headerfontsize">
                <option value="1">Small</option>
                <option value="2">Medium</option>
                <option value="3">Large</option>
                <option value="4">Extra Large</option>
              </select>
            </div>
            <div class="gm-section-label">Buttons</div>
          </div>

          <div class="gm-grid"></div>

          <div class="gm-edit">
            <div class="gm-row"><label>Label</label><input class="gm-f-label" type="text"></div>
            <div class="gm-row"><label>Icon</label><select class="gm-f-icon"></select><span class="gm-icon-preview"></span></div>

            <div class="gm-section-label">Action (Leave Domain Empty for a Refresh-Only Button)</div>
            <div class="gm-row"><label>Domain</label><select class="gm-f-domain"></select></div>
            <div class="gm-row"><label>Entity</label><select class="gm-f-entity"></select></div>
            <div class="gm-row"><label>Service</label><select class="gm-f-service"></select></div>

            <div class="gm-actions">
              <button class="gm-cancel" type="button">Cancel</button>
              <button class="gm-save" type="button">Save Slot</button>
            </div>
          </div>
        </div>
      </ha-card>
    `;
    this._gridEl = this.querySelector(".gm-grid");
    this._editEl = this.querySelector(".gm-edit");
    this.querySelector(".gm-mode-toggle").addEventListener("click", () => {
      this._setMode(this._mode === "edit" ? "view" : "edit");
    });
    this.querySelector(".gm-cancel").addEventListener("click", () => this._closeEdit());
    this.querySelector(".gm-save").addEventListener("click", () => this._saveEdit());
    this._populateIconOptions();
    this.querySelector(".gm-f-icon").addEventListener("change", () => this._updateIconPreview());
    this.querySelector(".gm-f-domain").addEventListener("change", (e) => {
      const domain = e.target.value;
      this._populateEntityOptions(domain, "");
      this._populateServiceOptions(domain, "");
    });
    this.querySelector(".gm-f-orientation").addEventListener("change", () => this._saveLayout());
    this.querySelector(".gm-f-cols").addEventListener("change", () => this._saveLayout());
    this.querySelector(".gm-f-rows").addEventListener("change", () => this._saveLayout());
    this.querySelector(".gm-f-fontsize").addEventListener("change", () => this._saveLayout());
    this.querySelector(".gm-f-headertext").addEventListener("change", () => this._saveLayout());
    this.querySelector(".gm-f-headerfontsize").addEventListener("change", () => this._saveLayout());
  }

  _syncLayoutForm() {
    this.querySelector(".gm-f-orientation").value = this._layout.orientation || "landscape";
    this.querySelector(".gm-f-cols").value = this._layout.cols || 3;
    this.querySelector(".gm-f-rows").value = this._layout.rows || 2;
    this.querySelector(".gm-f-fontsize").value = this._layout.fontSize || 2;
    this.querySelector(".gm-f-headertext").value = this._layout.headerText || "Garage Monitor";
    this.querySelector(".gm-f-headerfontsize").value = this._layout.headerFontSize || 2;
    this.querySelector(".gm-panel-title").textContent = this._layout.headerText || "Garage Monitor";
  }

  _setMode(mode) {
    this._mode = mode;
    const editing = mode === "edit";
    this.querySelector(".gm-mode-toggle").textContent = editing ? "Done" : "Edit Layout";
    this.querySelector(".gm-layout-settings").classList.toggle("open", editing);
    if (!editing) this._closeEdit();
    this._renderGrid();
  }

  _saveLayout() {
    let cols = parseInt(this.querySelector(".gm-f-cols").value, 10) || 1;
    let rows = parseInt(this.querySelector(".gm-f-rows").value, 10) || 1;
    cols = Math.max(1, Math.min(4, cols));
    rows = Math.max(1, Math.min(4, rows));
    if (cols * rows > MAX_SLOTS) {
      alert(`${cols} x ${rows} = ${cols * rows} exceeds the 12 provisioned slots. Reduce columns or rows.`);
      this._syncLayoutForm();
      return;
    }
    const orientation = this.querySelector(".gm-f-orientation").value;
    const fontSize = parseInt(this.querySelector(".gm-f-fontsize").value, 10) || 2;
    const headerText = this.querySelector(".gm-f-headertext").value.trim() || "Garage Monitor";
    const headerFontSize = parseInt(this.querySelector(".gm-f-headerfontsize").value, 10) || 2;
    this._layout = { orientation, cols, rows, fontSize, headerText, headerFontSize };
    const value = JSON.stringify(this._layout);
    if (value.length > 255) {
      alert(`Layout JSON is ${value.length} chars, over the 255-char input_text limit. Shorten the header text.`);
      this._syncLayoutForm();
      return;
    }
    this._layoutRaw = value;
    this._hass.callService("input_text", "set_value", { entity_id: LAYOUT_ENTITY, value });
    this._renderGrid();
  }

  _iconEl(iconName) {
    if (customElements.get("ha-icon")) {
      const el = document.createElement("ha-icon");
      el.setAttribute("icon", iconName || "mdi:help-circle-outline");
      return el;
    }
    const span = document.createElement("span");
    span.textContent = iconName || "";
    return span;
  }

  _updateIconPreview() {
    const preview = this.querySelector(".gm-icon-preview");
    preview.innerHTML = "";
    const val = this.querySelector(".gm-f-icon").value.trim();
    if (val) preview.appendChild(this._iconEl(val));
  }

  _populateIconOptions() {
    const sel = this.querySelector(".gm-f-icon");
    sel.innerHTML = "";
    for (const icon of AVAILABLE_ICONS) {
      const opt = document.createElement("option");
      opt.value = icon;
      opt.textContent = icon.replace("mdi:", "");
      sel.appendChild(opt);
    }
  }

  _fireSlotAction(s) {
    if (!s.domain || !s.service) return;
    this._hass.callService(s.domain, s.service, { entity_id: s.entity });
  }

  _renderGrid() {
    const cols = this._layout.cols || 3;
    const count = this._activeCount();
    this._gridEl.style.gridTemplateColumns = `repeat(${cols}, minmax(0, 1fr))`;
    const iconPx = Math.max(16, Math.min(36, Math.floor(200 / cols)));
    this._gridEl.style.setProperty("--gm-icon-px", `${iconPx}px`);
    const labelPxByFontSize = { 1: 11, 2: 13, 3: 16, 4: 19 };
    const labelPx = labelPxByFontSize[this._layout.fontSize] || 13;
    this._gridEl.style.setProperty("--gm-label-px", `${labelPx}px`);
    this._gridEl.innerHTML = "";
    const editing = this._mode === "edit";
    for (let i = 0; i < count; i++) {
      const s = this._slots[i] || this._defaultSlot();
      const tile = document.createElement("div");
      tile.className = "gm-tile";
      tile.dataset.index = String(i);

      if (editing) {
        const controls = document.createElement("div");
        controls.className = "gm-tile-controls";
        const upBtn = document.createElement("button");
        upBtn.type = "button";
        upBtn.textContent = "▲";
        upBtn.title = "Move up a position";
        upBtn.disabled = i === 0;
        upBtn.addEventListener("click", (e) => {
          e.stopPropagation();
          this._moveSlot(i, -1);
        });
        const downBtn = document.createElement("button");
        downBtn.type = "button";
        downBtn.textContent = "▼";
        downBtn.title = "Move down a position";
        downBtn.disabled = i === count - 1;
        downBtn.addEventListener("click", (e) => {
          e.stopPropagation();
          this._moveSlot(i, 1);
        });
        controls.appendChild(upBtn);
        controls.appendChild(downBtn);
        tile.appendChild(controls);
      } else {
        if (!s.label && !(s.domain && s.service)) tile.classList.add("gm-inert");
      }

      const iconWrap = document.createElement("div");
      iconWrap.className = "gm-icon";
      iconWrap.appendChild(this._iconEl(s.icon));
      tile.appendChild(iconWrap);

      const labelEl = document.createElement("div");
      labelEl.className = "gm-label";
      labelEl.textContent = s.label || "(empty)";
      tile.appendChild(labelEl);

      tile.addEventListener("click", () => {
        if (editing) this._openEdit(i);
        else this._fireSlotAction(s);
      });

      this._gridEl.appendChild(tile);
    }
  }

  _moveSlot(i, delta) {
    const j = i + delta;
    if (j < 0 || j >= this._activeCount()) return;
    const tmp = this._slots[i];
    this._slots[i] = this._slots[j];
    this._slots[j] = tmp;
    this._persistSlot(i);
    this._persistSlot(j);
    this._renderGrid();
  }

  _persistSlot(i) {
    const value = JSON.stringify(this._slots[i]);
    if (value.length > 255) {
      alert(`Slot ${i} JSON is ${value.length} chars, over the 255-char input_text limit. Shorten the label/icon.`);
      return;
    }
    this._raw[i] = value;
    this._hass.callService("input_text", "set_value", {
      entity_id: this._entityId(i),
      value,
    });
  }

  _populateDomainOptions(selected) {
    const sel = this.querySelector(".gm-f-domain");
    sel.innerHTML = "";
    const noneOpt = document.createElement("option");
    noneOpt.value = "";
    noneOpt.textContent = "(none - refresh only)";
    sel.appendChild(noneOpt);
    const domains = new Set();
    for (const id of Object.keys(this._hass.states)) {
      domains.add(id.split(".")[0]);
    }
    [...domains].sort().forEach((d) => {
      const opt = document.createElement("option");
      opt.value = d;
      opt.textContent = d;
      sel.appendChild(opt);
    });
    sel.value = selected;
  }

  _populateEntityOptions(domain, selected) {
    const sel = this.querySelector(".gm-f-entity");
    sel.innerHTML = "";
    sel.disabled = !domain;
    if (!domain) return;
    const ids = Object.keys(this._hass.states)
      .filter((id) => id.startsWith(domain + "."))
      .sort();
    for (const id of ids) {
      const opt = document.createElement("option");
      opt.value = id;
      opt.textContent = this._hass.states[id].attributes.friendly_name || id;
      sel.appendChild(opt);
    }
    if (ids.includes(selected)) sel.value = selected;
  }

  _populateServiceOptions(domain, selected) {
    const sel = this.querySelector(".gm-f-service");
    sel.innerHTML = "";
    sel.disabled = !domain;
    if (!domain) return;
    const services = this._hass.services && this._hass.services[domain]
      ? Object.keys(this._hass.services[domain]).sort()
      : [];
    for (const s of services) {
      const opt = document.createElement("option");
      opt.value = s;
      opt.textContent = s;
      sel.appendChild(opt);
    }
    if (services.includes(selected)) {
      sel.value = selected;
    } else if (domain === "script" && services.includes("turn_on")) {
      sel.value = "turn_on";
    } else if (services.length) {
      sel.value = services[0];
    }
  }

  _openEdit(i) {
    this._editingSlot = i;
    const s = this._slots[i] || this._defaultSlot();
    this.querySelector(".gm-f-label").value = s.label || "";
    this.querySelector(".gm-f-icon").value = s.icon || "";
    this._populateDomainOptions(s.domain || "");
    this._populateEntityOptions(s.domain || "", s.entity || "");
    this._populateServiceOptions(s.domain || "", s.service || "");
    this._updateIconPreview();
    this._editEl.classList.add("open");
    this._editEl.scrollIntoView({ behavior: "smooth", block: "nearest" });
  }

  _closeEdit() {
    this._editingSlot = null;
    this._editEl.classList.remove("open");
  }

  _saveEdit() {
    const i = this._editingSlot;
    if (i === null) return;
    const label = this.querySelector(".gm-f-label").value.trim();
    const icon = this.querySelector(".gm-f-icon").value.trim() || "mdi:help-circle-outline";
    const domain = this.querySelector(".gm-f-domain").value;
    const entity = domain ? this.querySelector(".gm-f-entity").value : "";
    const service = domain ? this.querySelector(".gm-f-service").value : "";
    this._slots[i] = { label, icon, domain, service, entity };
    this._persistSlot(i);
    this._renderGrid();
    this._closeEdit();
  }
}

customElements.define("garage-monitor-hotkey-card", GarageMonitorHotkeyCard);
