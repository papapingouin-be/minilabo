import { fetchInputsSnapshot } from './ioCatalog.js';

const MEASUREMENT_PROFILES = {
  voltage: {
    id: 'voltage',
    symbol: 'U',
    label: 'Tension',
    defaultCalibre: '20V',
    calibrations: [
      { id: '200mV', label: '200 mV', max: 0.2, unit: 'mV', displayMultiplier: 1000, decimals: 2, fallbackDisplay: [12, 180] },
      { id: '2V', label: '2 V', max: 2, unit: 'V', displayMultiplier: 1, decimals: 4, fallbackDisplay: [0.6, 1.8] },
      { id: '20V', label: '20 V', max: 20, unit: 'V', displayMultiplier: 1, decimals: 3, fallbackDisplay: [2.5, 18] },
      { id: '200V', label: '200 V', max: 200, unit: 'V', displayMultiplier: 1, decimals: 2, fallbackDisplay: [190, 230] },
      { id: '1000V', label: '1000 V', max: 1000, unit: 'V', displayMultiplier: 1, decimals: 1, fallbackDisplay: [215, 230] }
    ]
  },
  resistance: {
    id: 'resistance',
    symbol: 'Ω',
    label: 'Résistance',
    defaultCalibre: '20k',
    calibrations: [
      { id: '200', label: '200 Ω', max: 200, unit: 'Ω', displayMultiplier: 1, decimals: 1, fallbackDisplay: [24, 180] },
      { id: '2k', label: '2 kΩ', max: 2000, unit: 'kΩ', displayMultiplier: 0.001, decimals: 3, fallbackDisplay: [0.65, 1.95] },
      { id: '20k', label: '20 kΩ', max: 20000, unit: 'kΩ', displayMultiplier: 0.001, decimals: 2, fallbackDisplay: [1.8, 12.5] },
      { id: '200k', label: '200 kΩ', max: 200000, unit: 'kΩ', displayMultiplier: 0.001, decimals: 1, fallbackDisplay: [15, 120] },
      { id: '2M', label: '2 MΩ', max: 2000000, unit: 'MΩ', displayMultiplier: 0.000001, decimals: 3, fallbackDisplay: [0.35, 1.25] }
    ]
  },
  current: {
    id: 'current',
    symbol: 'I',
    label: 'Courant',
    defaultCalibre: '200mA',
    calibrations: [
      { id: '200uA', label: '200 µA', max: 0.0002, unit: 'µA', displayMultiplier: 1000000, decimals: 2, fallbackDisplay: [12, 160] },
      { id: '2mA', label: '2 mA', max: 0.002, unit: 'mA', displayMultiplier: 1000, decimals: 3, fallbackDisplay: [0.85, 1.65] },
      { id: '20mA', label: '20 mA', max: 0.02, unit: 'mA', displayMultiplier: 1000, decimals: 2, fallbackDisplay: [4, 18] },
      { id: '200mA', label: '200 mA', max: 0.2, unit: 'mA', displayMultiplier: 1000, decimals: 1, fallbackDisplay: [12, 180] },
      { id: '10A', label: '10 A', max: 10, unit: 'A', displayMultiplier: 1, decimals: 2, fallbackDisplay: [0.2, 6.5] }
    ]
  },
  frequency: {
    id: 'frequency',
    symbol: 'Hz',
    label: 'Fréquence',
    defaultCalibre: '20kHz',
    calibrations: [
      { id: '200Hz', label: '200 Hz', max: 200, unit: 'Hz', displayMultiplier: 1, decimals: 1, fallbackDisplay: [40, 180] },
      { id: '2kHz', label: '2 kHz', max: 2000, unit: 'kHz', displayMultiplier: 0.001, decimals: 3, fallbackDisplay: [0.3, 1.5] },
      { id: '20kHz', label: '20 kHz', max: 20000, unit: 'kHz', displayMultiplier: 0.001, decimals: 2, fallbackDisplay: [3.2, 18.4] },
      { id: '200kHz', label: '200 kHz', max: 200000, unit: 'kHz', displayMultiplier: 0.001, decimals: 1, fallbackDisplay: [22, 160] }
    ]
  },
  capacitance: {
    id: 'capacitance',
    symbol: 'F',
    label: 'Capacité',
    defaultCalibre: '200uF',
    calibrations: [
      { id: '200nF', label: '200 nF', max: 0.0000002, unit: 'nF', displayMultiplier: 1000000000, decimals: 1, fallbackDisplay: [12, 180] },
      { id: '2uF', label: '2 µF', max: 0.000002, unit: 'µF', displayMultiplier: 1000000, decimals: 2, fallbackDisplay: [0.18, 1.8] },
      { id: '200uF', label: '200 µF', max: 0.0002, unit: 'µF', displayMultiplier: 1000000, decimals: 1, fallbackDisplay: [18, 160] },
      { id: '2mF', label: '2 mF', max: 0.002, unit: 'mF', displayMultiplier: 1000, decimals: 2, fallbackDisplay: [0.4, 1.7] }
    ]
  },
  inductance: {
    id: 'inductance',
    symbol: 'H',
    label: 'Inductance',
    defaultCalibre: '200mH',
    calibrations: [
      { id: '200uH', label: '200 µH', max: 0.0002, unit: 'µH', displayMultiplier: 1000000, decimals: 1, fallbackDisplay: [12, 180] },
      { id: '2mH', label: '2 mH', max: 0.002, unit: 'mH', displayMultiplier: 1000, decimals: 2, fallbackDisplay: [0.45, 1.6] },
      { id: '200mH', label: '200 mH', max: 0.2, unit: 'mH', displayMultiplier: 1000, decimals: 1, fallbackDisplay: [12, 160] },
      { id: '2H', label: '2 H', max: 2, unit: 'H', displayMultiplier: 1, decimals: 3, fallbackDisplay: [0.35, 1.85] }
    ]
  }
};

const MEASUREMENT_ORDER = ['voltage', 'resistance', 'current', 'frequency', 'capacitance', 'inductance'];

function randomInRange([min, max]) {
  return Math.random() * (max - min) + min;
}

function formatDisplay(value, decimals) {
  if (value === null || Number.isNaN(value)) {
    return '—';
  }
  return new Intl.NumberFormat('fr-FR', {
    minimumFractionDigits: decimals,
    maximumFractionDigits: decimals
  }).format(value);
}

function populateSelect(select, options, placeholder) {
  select.innerHTML = '';
  if (!options.length) {
    const opt = document.createElement('option');
    opt.textContent = placeholder;
    opt.value = '';
    select.appendChild(opt);
    select.disabled = true;
    return;
  }
  options.forEach((item) => {
    const opt = document.createElement('option');
    opt.value = item.name;
    opt.textContent = item.name;
    select.appendChild(opt);
  });
  select.disabled = false;
}

export function mountMultimeter(container, { inputs = [], error = null } = {}) {
  if (!container) return;

  container.innerHTML = `
    <div class="device-shell multimeter-shell">
      <div class="multimeter-head">
        <div class="device-branding">
          <span class="device-brand">Keysight</span>
          <span class="device-model" id="multimeter-title">34470A</span>
          <span class="device-subtitle">Multimètre 6½ digits</span>
        </div>
        <div class="io-selector">
          <label for="multimeter-input">Entrée mesurée</label>
          <select id="multimeter-input" class="io-select"></select>
        </div>
      </div>
      <div class="multimeter-body">
        <div class="multimeter-display" role="img" aria-label="Affichage de mesure">
          <div class="multimeter-value-line">
            <span class="multimeter-readout" id="multimeter-value">0,0000</span>
            <span class="multimeter-symbol" id="multimeter-symbol">U</span>
          </div>
          <div class="multimeter-display-footer">
            <span class="multimeter-unit" id="multimeter-unit">V</span>
            <span class="multimeter-acdc" id="multimeter-acdc-label">DC</span>
            <span class="multimeter-hold-indicator" id="multimeter-hold-indicator" hidden>HOLD</span>
          </div>
          <progress id="multimeter-bar" max="100" value="0" aria-label="Barregraph numérique"></progress>
        </div>
        <div class="multimeter-sidepanel">
          <div class="multimeter-actions">
            <button type="button" class="meter-button meter-button--power is-active" data-action="power" aria-pressed="true">Power</button>
            <button type="button" class="meter-button" data-action="acdc" aria-pressed="false">AC/DC</button>
            <div class="measurement-buttons" role="group" aria-label="Type de mesure"></div>
            <button type="button" class="meter-button" data-action="hold" aria-pressed="false">HOLD</button>
          </div>
          <div class="rotary-section">
            <div class="rotary-wrapper">
              <div class="rotary-knob" id="multimeter-calibre-knob">
                <div class="rotary-indicator"></div>
                <div class="rotary-center"></div>
              </div>
              <div class="rotary-options" id="multimeter-calibre-options" role="list"></div>
            </div>
            <span class="rotary-label">Calibre</span>
          </div>
          <div class="multimeter-status-panel">
            <div class="device-status" id="multimeter-status"></div>
            <div class="multimeter-updates">
              <span>Dernière lecture</span>
              <span id="multimeter-updated">—</span>
            </div>
          </div>
        </div>
      </div>
    </div>
  `;

  const defaultMode = MEASUREMENT_ORDER[0];
  const state = {
    powered: true,
    acMode: 'dc',
    hold: false,
    measurementMode: defaultMode,
    calibreId: MEASUREMENT_PROFILES[defaultMode].defaultCalibre,
    timer: null,
    previousValue: null,
    lastBaseValue: null,
    heldValue: null
  };

  const valueDisplay = container.querySelector('#multimeter-value');
  const symbolDisplay = container.querySelector('#multimeter-symbol');
  const unitDisplay = container.querySelector('#multimeter-unit');
  const acdcLabel = container.querySelector('#multimeter-acdc-label');
  const holdIndicator = container.querySelector('#multimeter-hold-indicator');
  const bar = container.querySelector('#multimeter-bar');
  const updatedLabel = container.querySelector('#multimeter-updated');
  const inputSelect = container.querySelector('#multimeter-input');
  const statusLabel = container.querySelector('#multimeter-status');
  const powerButton = container.querySelector('[data-action="power"]');
  const acdcButton = container.querySelector('[data-action="acdc"]');
  const holdButton = container.querySelector('[data-action="hold"]');
  const measurementContainer = container.querySelector('.measurement-buttons');
  const knob = container.querySelector('#multimeter-calibre-knob');
  const knobOptions = container.querySelector('#multimeter-calibre-options');

  populateSelect(inputSelect, inputs, 'Aucune entrée active');
  const ioMap = new Map(inputs.map((item) => [item.name, item]));
  if (inputs.length) {
    inputSelect.value = inputs[0].name;
    statusLabel.textContent = `${inputs.length} entrée(s) disponibles`;
  } else if (error) {
    statusLabel.textContent = 'Impossible de charger les entrées.';
  } else {
    statusLabel.textContent = 'Activez une entrée dans la configuration.';
  }

  const getCurrentProfile = () => MEASUREMENT_PROFILES[state.measurementMode] || MEASUREMENT_PROFILES[defaultMode];

  const getCurrentCalibre = () => {
    const profile = getCurrentProfile();
    return profile.calibrations.find((cal) => cal.id === state.calibreId) || profile.calibrations[0];
  };

  const updateSymbolAndUnit = () => {
    const profile = getCurrentProfile();
    const calibre = getCurrentCalibre();
    symbolDisplay.textContent = profile.symbol;
    unitDisplay.textContent = calibre.unit;
  };

  const updateCalibrationButtons = () => {
    knobOptions.querySelectorAll('[data-calibre]').forEach((btn) => {
      const isActive = btn.dataset.calibre === state.calibreId;
      btn.classList.toggle('is-active', isActive);
      btn.setAttribute('aria-pressed', isActive ? 'true' : 'false');
    });
  };

  const updateKnobRotation = () => {
    const profile = getCurrentProfile();
    const index = profile.calibrations.findIndex((cal) => cal.id === state.calibreId);
    if (index === -1) {
      knob.style.setProperty('--rotation', '-90deg');
      return;
    }
    const step = 360 / profile.calibrations.length;
    const angle = -90 + index * step;
    knob.style.setProperty('--rotation', `${angle}deg`);
  };

  const renderCalibrations = (profile) => {
    knobOptions.innerHTML = '';
    const step = 360 / profile.calibrations.length;
    profile.calibrations.forEach((cal, index) => {
      const angle = -90 + index * step;
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'rotary-option';
      button.dataset.calibre = cal.id;
      button.style.setProperty('--angle', `${angle}deg`);
      button.textContent = cal.label;
      button.setAttribute('role', 'listitem');
      button.setAttribute('aria-label', `Calibre ${cal.label}`);
      button.setAttribute('aria-pressed', cal.id === state.calibreId ? 'true' : 'false');
      button.addEventListener('click', () => {
        if (state.calibreId === cal.id) {
          return;
        }
        state.calibreId = cal.id;
        state.previousValue = null;
        state.lastBaseValue = null;
        updateKnobRotation();
        updateCalibrationButtons();
        refreshMeasurement();
      });
      knobOptions.appendChild(button);
    });
    updateCalibrationButtons();
    updateKnobRotation();
  };

  MEASUREMENT_ORDER.forEach((mode) => {
    const profile = MEASUREMENT_PROFILES[mode];
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'meter-button meter-button--measurement';
    button.dataset.measurement = mode;
    button.textContent = profile.symbol;
    button.setAttribute('aria-label', `${profile.label} (${profile.symbol})`);
    const isActive = state.measurementMode === mode;
    button.classList.toggle('is-active', isActive);
    button.setAttribute('aria-pressed', isActive ? 'true' : 'false');
    measurementContainer.appendChild(button);
  });

  const measurementButtons = Array.from(measurementContainer.querySelectorAll('[data-measurement]'));

  const getSmoothedValue = (raw) => {
    if (raw === null || Number.isNaN(raw)) {
      state.previousValue = null;
      return null;
    }
    if (state.previousValue === null || Number.isNaN(state.previousValue)) {
      state.previousValue = raw;
      return raw;
    }
    const alpha = 0.18;
    state.previousValue = state.previousValue + (raw - state.previousValue) * alpha;
    return state.previousValue;
  };

  const randomFallbackBase = (calibre) => {
    if (!calibre || !calibre.fallbackDisplay) {
      return null;
    }
    const displayValue = randomInRange(calibre.fallbackDisplay);
    return displayValue / calibre.displayMultiplier;
  };

  const applyAcMode = (value) => {
    if (value === null || Number.isNaN(value)) {
      return null;
    }
    if (state.acMode === 'ac') {
      return Math.abs(value);
    }
    return value;
  };

  const updateDisplay = (baseValue) => {
    const profile = getCurrentProfile();
    const calibre = getCurrentCalibre();
    if (baseValue === null || Number.isNaN(baseValue)) {
      valueDisplay.textContent = '—';
      unitDisplay.textContent = calibre.unit;
      symbolDisplay.textContent = profile.symbol;
      bar.value = 0;
      return;
    }
    if (Math.abs(baseValue) > calibre.max) {
      valueDisplay.textContent = 'OL';
      unitDisplay.textContent = calibre.unit;
      symbolDisplay.textContent = profile.symbol;
      bar.value = 100;
      return;
    }
    const displayValue = baseValue * calibre.displayMultiplier;
    valueDisplay.textContent = formatDisplay(displayValue, calibre.decimals);
    unitDisplay.textContent = calibre.unit;
    symbolDisplay.textContent = profile.symbol;
    const rangeDisplayMax = calibre.max * calibre.displayMultiplier;
    const percent = rangeDisplayMax === 0 ? 0 : Math.min(100, Math.abs(displayValue) / rangeDisplayMax * 100);
    bar.value = percent;
  };

  const displayOff = () => {
    valueDisplay.textContent = '----';
    unitDisplay.textContent = '';
    symbolDisplay.textContent = '';
    acdcLabel.textContent = '';
    bar.value = 0;
  };

  const refreshMeasurement = async ({ force = false } = {}) => {
    if (!state.powered) {
      return;
    }
    const profile = getCurrentProfile();
    const calibre = getCurrentCalibre();
    acdcLabel.textContent = state.acMode === 'ac' ? 'AC' : 'DC';
    if (state.hold && !force) {
      const held = state.heldValue !== null ? state.heldValue : state.lastBaseValue;
      if (held !== null) {
        updateDisplay(held);
        statusLabel.textContent = 'Maintien de la lecture (HOLD)';
      } else {
        updateDisplay(null);
      }
      return;
    }

    const selectedName = inputSelect.value;
    if (!selectedName) {
      state.lastBaseValue = null;
      updateDisplay(null);
      updatedLabel.textContent = '—';
      statusLabel.textContent = 'Aucune entrée sélectionnée';
      return;
    }

    try {
      const snapshot = await fetchInputsSnapshot();
      const rawValue = Object.prototype.hasOwnProperty.call(snapshot, selectedName)
        ? Number(snapshot[selectedName])
        : NaN;
      const processed = applyAcMode(Number.isFinite(rawValue) ? rawValue : NaN);
      const smoothed = getSmoothedValue(processed);
      state.lastBaseValue = smoothed;
      updateDisplay(smoothed);
      const now = new Date();
      updatedLabel.textContent = now.toLocaleTimeString('fr-FR', { hour12: false });
      const ioInfo = ioMap.get(selectedName);
      const unit = ioInfo && ioInfo.unit ? ` (${ioInfo.unit})` : '';
      statusLabel.textContent = `Lecture ${state.acMode === 'ac' ? 'AC' : 'DC'} sur ${selectedName}${unit}`;
    } catch (err) {
      const fallbackBase = randomFallbackBase(calibre);
      const smoothed = getSmoothedValue(fallbackBase);
      state.lastBaseValue = smoothed;
      updateDisplay(smoothed);
      updatedLabel.textContent = '—';
      statusLabel.textContent = 'Lecture simulée (pas de données disponibles)';
    }
  };

  const scheduleRefresh = () => {
    if (state.timer) {
      clearInterval(state.timer);
    }
    if (!state.powered) {
      return;
    }
    state.timer = setInterval(refreshMeasurement, 1200);
  };

  measurementButtons.forEach((button) => {
    button.addEventListener('click', () => {
      const mode = button.dataset.measurement;
      if (state.measurementMode === mode) {
        return;
      }
      state.measurementMode = mode;
      const profile = getCurrentProfile();
      state.calibreId = profile.defaultCalibre || profile.calibrations[0].id;
      state.previousValue = null;
      state.lastBaseValue = null;
      state.heldValue = null;
      state.hold = false;
      holdIndicator.hidden = true;
      holdButton.classList.remove('is-active');
      holdButton.setAttribute('aria-pressed', 'false');
      measurementButtons.forEach((btn) => {
        const isActive = btn.dataset.measurement === mode;
        btn.classList.toggle('is-active', isActive);
        btn.setAttribute('aria-pressed', isActive ? 'true' : 'false');
      });
      updateSymbolAndUnit();
      renderCalibrations(profile);
      refreshMeasurement();
    });
  });

  powerButton.addEventListener('click', () => {
    state.powered = !state.powered;
    powerButton.classList.toggle('is-active', state.powered);
    powerButton.setAttribute('aria-pressed', state.powered ? 'true' : 'false');
    if (state.powered) {
      holdIndicator.hidden = !state.hold;
      statusLabel.textContent = 'Alimentation activée';
      scheduleRefresh();
      refreshMeasurement();
    } else {
      if (state.timer) {
        clearInterval(state.timer);
        state.timer = null;
      }
      updatedLabel.textContent = '—';
      statusLabel.textContent = 'Appareil hors tension';
      holdIndicator.hidden = true;
      displayOff();
    }
  });

  acdcButton.addEventListener('click', () => {
    state.acMode = state.acMode === 'dc' ? 'ac' : 'dc';
    const isAc = state.acMode === 'ac';
    acdcButton.classList.toggle('is-active', isAc);
    acdcButton.setAttribute('aria-pressed', isAc ? 'true' : 'false');
    state.previousValue = null;
    refreshMeasurement();
  });

  holdButton.addEventListener('click', async () => {
    const wantHold = !state.hold;
    state.hold = wantHold;
    holdButton.classList.toggle('is-active', wantHold);
    holdButton.setAttribute('aria-pressed', wantHold ? 'true' : 'false');
    if (wantHold) {
      if (state.lastBaseValue === null) {
        await refreshMeasurement({ force: true });
      }
      state.heldValue = state.lastBaseValue;
      holdIndicator.hidden = false;
      if (state.heldValue !== null) {
        updateDisplay(state.heldValue);
        statusLabel.textContent = 'Maintien de la lecture (HOLD)';
      } else {
        updateDisplay(null);
        statusLabel.textContent = 'Maintien actif - en attente de mesure';
      }
    } else {
      state.heldValue = null;
      holdIndicator.hidden = true;
      refreshMeasurement();
    }
  });

  inputSelect.addEventListener('change', () => {
    state.previousValue = null;
    state.lastBaseValue = null;
    state.heldValue = null;
    if (state.hold) {
      state.hold = false;
      holdButton.classList.remove('is-active');
      holdButton.setAttribute('aria-pressed', 'false');
      holdIndicator.hidden = true;
    }
    refreshMeasurement();
  });

  window.addEventListener('beforeunload', () => {
    if (state.timer) {
      clearInterval(state.timer);
    }
  });

  updateSymbolAndUnit();
  renderCalibrations(getCurrentProfile());
  refreshMeasurement();
  scheduleRefresh();
}
