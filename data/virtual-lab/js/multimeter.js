import { fetchInputsSnapshot } from './ioCatalog.js';

const MEASUREMENTS = {
  voltage: { label: 'Tension DC', unit: 'V', range: [3.28, 3.34] },
  voltageAc: { label: 'Tension AC', unit: 'V RMS', range: [229.2, 231.5] },
  current: { label: 'Courant', unit: 'mA', range: [12.3, 12.9] },
  resistance: { label: 'Résistance', unit: 'kΩ', range: [1.48, 1.53] }
};

function randomInRange([min, max]) {
  return Math.random() * (max - min) + min;
}

function formatValue(value, digits = 3) {
  if (value === null || value === undefined) {
    return '—';
  }
  return Number(value).toFixed(digits).replace('.', ',');
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
      <div class="device-header">
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
      <div class="device-status" id="multimeter-status"></div>
      <div class="multimeter-display" role="img" aria-label="Affichage de mesure">
        <div class="multimeter-readout" id="multimeter-value">0,000</div>
        <div class="multimeter-unit" id="multimeter-unit">V</div>
      </div>
      <div class="multimeter-controls">
        <label>
          Mode
          <select id="multimeter-mode" class="io-select"></select>
        </label>
        <label>
          Filtre numérique
          <input type="range" id="multimeter-filter" min="0" max="100" value="40">
        </label>
        <label>
          Barregraph
          <progress id="multimeter-bar" max="100" value="30" aria-label="Barregraph numérique"></progress>
        </label>
        <label>
          Dernière lecture
          <span id="multimeter-updated">—</span>
        </label>
      </div>
    </div>
  `;

  const modeSelect = container.querySelector('#multimeter-mode');
  const valueDisplay = container.querySelector('#multimeter-value');
  const unitDisplay = container.querySelector('#multimeter-unit');
  const filterRange = container.querySelector('#multimeter-filter');
  const bar = container.querySelector('#multimeter-bar');
  const updatedLabel = container.querySelector('#multimeter-updated');
  const inputSelect = container.querySelector('#multimeter-input');
  const statusLabel = container.querySelector('#multimeter-status');

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

  Object.entries(MEASUREMENTS).forEach(([key, { label }]) => {
    const option = document.createElement('option');
    option.value = key;
    option.textContent = label;
    modeSelect.appendChild(option);
  });

  const state = {
    previousValue: null,
    timer: null
  };

  const getSmoothedValue = (raw, smoothing) => {
    if (raw === null) {
      return null;
    }
    if (state.previousValue === null || Number.isNaN(state.previousValue)) {
      state.previousValue = raw;
      return raw;
    }
    const alpha = Math.max(0.02, 1 - smoothing);
    state.previousValue = state.previousValue + (raw - state.previousValue) * alpha;
    return state.previousValue;
  };

  const updateDisplay = (value, metadata) => {
    const { unit, range } = metadata;
    valueDisplay.textContent = formatValue(value);
    unitDisplay.textContent = unit;
    if (value === null || !range || range[0] === range[1]) {
      bar.value = 0;
    } else {
      const percent = ((value - range[0]) / (range[1] - range[0])) * 100;
      bar.value = Math.min(100, Math.max(0, percent));
    }
  };

  const refreshMeasurement = async () => {
    const measurement = MEASUREMENTS[modeSelect.value] || MEASUREMENTS.voltage;
    const selectedName = inputSelect.value;
    const smoothing = parseInt(filterRange.value, 10) / 100;
    if (!selectedName) {
      state.previousValue = null;
      updateDisplay(null, { unit: measurement.unit, range: measurement.range });
      updatedLabel.textContent = '—';
      return;
    }
    try {
      const snapshot = await fetchInputsSnapshot();
      const raw = Object.prototype.hasOwnProperty.call(snapshot, selectedName)
        ? snapshot[selectedName]
        : null;
      const value = raw !== null ? getSmoothedValue(raw, smoothing) : null;
      const ioInfo = ioMap.get(selectedName);
      const unit = (ioInfo && ioInfo.unit) || measurement.unit;
      updateDisplay(value, { unit, range: measurement.range });
      const now = new Date();
      updatedLabel.textContent = now.toLocaleTimeString('fr-FR', { hour12: false });
      statusLabel.textContent = raw === null
        ? `Lecture indisponible pour ${selectedName}`
        : `Lecture en cours sur ${selectedName}`;
    } catch (err) {
      statusLabel.textContent = 'Erreur lors de la lecture des entrées';
      const fallbackValue = getSmoothedValue(randomInRange(measurement.range), smoothing);
      updateDisplay(fallbackValue, { unit: measurement.unit, range: measurement.range });
      updatedLabel.textContent = '—';
    }
  };

  const scheduleRefresh = () => {
    if (state.timer) {
      clearInterval(state.timer);
    }
    state.timer = setInterval(refreshMeasurement, 1200);
  };

  modeSelect.addEventListener('change', () => {
    state.previousValue = null;
    refreshMeasurement();
  });

  filterRange.addEventListener('input', () => {
    state.previousValue = null;
    refreshMeasurement();
  });

  inputSelect.addEventListener('change', () => {
    state.previousValue = null;
    refreshMeasurement();
  });

  window.addEventListener('beforeunload', () => {
    if (state.timer) {
      clearInterval(state.timer);
    }
  });

  modeSelect.value = 'voltage';
  refreshMeasurement();
  scheduleRefresh();
}
