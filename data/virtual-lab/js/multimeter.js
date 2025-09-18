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
const DISPLAY_MODES = ['digital', 'binary', 'gauge'];
const SVG_NS = 'http://www.w3.org/2000/svg';
const GAUGE_CENTER_X = 130;
const GAUGE_CENTER_Y = 118;
const GAUGE_RADIUS = 88;
const GAUGE_TICK_OUTER_RADIUS = 90;
const GAUGE_TICK_INNER_RADIUS = 74;
const GAUGE_START_ANGLE = -110;
const GAUGE_END_ANGLE = 110;
const POSITIVE_ONLY_PROFILES = new Set(['voltage', 'resistance', 'capacitance', 'inductance', 'frequency']);

function clamp(value, min, max) {
  if (value < min) return min;
  if (value > max) return max;
  return value;
}

function polarToCartesian(angleDeg, radius) {
  const rad = (Math.PI / 180) * angleDeg;
  return {
    x: GAUGE_CENTER_X + Math.cos(rad) * radius,
    y: GAUGE_CENTER_Y + Math.sin(rad) * radius
  };
}

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
    const value = item.id || item.name;
    opt.value = value;
    opt.textContent = item.label || item.name || value;
    select.appendChild(opt);
  });
  select.disabled = false;
}

export function mountMultimeter(container, { inputs = [], error = null, meterChannels = [] } = {}) {
  if (!container) return;

  container.innerHTML = `
    <div class="device-shell multimeter-shell">
      <div class="multimeter-head">
        <div class="device-branding">
          <span class="device-model" id="multimeter-title">Multimètre virtuel</span>
        </div>
        <div class="io-selector">
          <label for="multimeter-input">Entrée mesurée</label>
          <select id="multimeter-input" class="io-select"></select>
        </div>
      </div>
      <div class="multimeter-body">
        <div class="multimeter-display" role="img" aria-label="Affichage de mesure">
          <div class="multimeter-display-modes">
            <div class="multimeter-display-mode multimeter-display-mode--digital" data-mode="digital">
              <div class="multimeter-value-line">
                <span class="multimeter-readout" id="multimeter-value">0,0000</span>
                <span class="multimeter-symbol" id="multimeter-symbol">U</span>
              </div>
              <progress id="multimeter-bar" max="100" value="0" aria-label="Barregraph numérique"></progress>
            </div>
            <div class="multimeter-display-mode multimeter-display-mode--binary" data-mode="binary" hidden>
              <div class="multimeter-binary-value" id="multimeter-binary-value">—</div>
              <div class="multimeter-binary-meta">
                <span id="multimeter-bits-label">—</span>
              </div>
            </div>
            <div class="multimeter-display-mode multimeter-display-mode--gauge" data-mode="gauge" hidden>
              <div class="multimeter-gauge-wrapper">
                <svg class="multimeter-gauge" viewBox="0 0 260 140" role="img" aria-label="Cadran analogique">
                  <path id="multimeter-gauge-arc" class="multimeter-gauge-arc" />
                  <g id="multimeter-gauge-ticks" class="multimeter-gauge-ticks"></g>
                  <line id="multimeter-gauge-needle" class="multimeter-gauge-needle" x1="130" y1="118" x2="130" y2="42" />
                </svg>
                <div class="multimeter-gauge-labels">
                  <span id="multimeter-gauge-min">—</span>
                  <span id="multimeter-gauge-max">—</span>
                </div>
                <div class="multimeter-gauge-readout" id="multimeter-gauge-readout">—</div>
              </div>
            </div>
          </div>
          <div class="multimeter-display-footer">
            <span class="multimeter-unit" id="multimeter-unit">V</span>
            <span class="multimeter-acdc" id="multimeter-acdc-label">DC</span>
            <span class="multimeter-hold-indicator" id="multimeter-hold-indicator" hidden>HOLD</span>
          </div>
        </div>
        <div class="multimeter-sidepanel">
          <div class="multimeter-actions">
            <button type="button" class="meter-button meter-button--power is-active" data-action="power" aria-pressed="true">Power</button>
            <button type="button" class="meter-button" data-action="acdc" aria-pressed="false">AC/DC</button>
            <div class="measurement-buttons" role="group" aria-label="Type de mesure"></div>
            <div class="visual-buttons" role="group" aria-label="Mode d'affichage">
              <button type="button" class="meter-button meter-button--visual is-active" data-display="digital" aria-pressed="true">Décimal</button>
              <button type="button" class="meter-button meter-button--visual" data-display="binary" aria-pressed="false">Binaire</button>
              <button type="button" class="meter-button meter-button--visual" data-display="gauge" aria-pressed="false">Cadran</button>
            </div>
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

  const availableInputs = Array.isArray(inputs) ? inputs : [];
  const inputsByName = new Map(availableInputs.map((item) => [item.name, item]));
  const rawChannels = Array.isArray(meterChannels) ? meterChannels : [];
  const channels = rawChannels.length
    ? rawChannels
        .filter((channel) => channel && channel.input && inputsByName.has(channel.input))
        .map((channel, index) => ({
          id: channel.id || channel.name || `meter${index + 1}`,
          name: channel.id || channel.name || `meter${index + 1}`,
          label: channel.label || channel.name || channel.input,
          input: channel.input,
          unit: channel.unit || (inputsByName.get(channel.input)?.unit || ''),
          symbol: channel.symbol || '',
          enabled: channel.enabled !== false,
          scale: Number.isFinite(channel.scale) ? channel.scale : 1,
          offset: Number.isFinite(channel.offset) ? channel.offset : 0,
          rangeMin: Number.isFinite(channel.rangeMin) ? channel.rangeMin : null,
          rangeMax: Number.isFinite(channel.rangeMax) ? channel.rangeMax : null,
          bits: Number.isFinite(channel.bits)
            ? Math.min(32, Math.max(1, Math.round(channel.bits)))
            : 10
        }))
    : availableInputs.map((item, index) => ({
        id: item.name,
        name: item.name,
        label: item.name,
        input: item.name,
        unit: item.unit || '',
        symbol: '',
        enabled: true,
        scale: Number.isFinite(item.scale) ? item.scale : 1,
        offset: Number.isFinite(item.offset) ? item.offset : 0,
        rangeMin: null,
        rangeMax: null,
        bits: 10
      }));

  const activeChannels = channels.filter((channel) => channel.enabled && channel.input);

  const defaultMode = MEASUREMENT_ORDER[0];
  const state = {
    powered: true,
    acMode: 'dc',
    hold: false,
    measurementMode: defaultMode,
    calibreId: MEASUREMENT_PROFILES[defaultMode].defaultCalibre,
    timer: null,
    previousValue: null,
    lastRawValue: null,
    lastScaledValue: null,
    heldRawValue: null,
    heldScaledValue: null,
    displayMode: 'digital'
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
  const displayModes = Array.from(container.querySelectorAll('.multimeter-display-mode'));
  const binaryValueDisplay = container.querySelector('#multimeter-binary-value');
  const binaryBitsLabel = container.querySelector('#multimeter-bits-label');
  const gaugeNeedle = container.querySelector('#multimeter-gauge-needle');
  const gaugeArc = container.querySelector('#multimeter-gauge-arc');
  const gaugeTicksGroup = container.querySelector('#multimeter-gauge-ticks');
  const gaugeMinLabel = container.querySelector('#multimeter-gauge-min');
  const gaugeMaxLabel = container.querySelector('#multimeter-gauge-max');
  const gaugeReadout = container.querySelector('#multimeter-gauge-readout');
  const displayButtons = Array.from(container.querySelectorAll('[data-display]'));

  const channelMap = new Map(activeChannels.map((channel) => [channel.id, channel]));
  populateSelect(inputSelect, activeChannels, 'Aucune configuration disponible');
  if (activeChannels.length) {
    inputSelect.value = activeChannels[0].id;
    statusLabel.textContent = `${activeChannels.length} canal(aux) disponible(s)`;
  } else if (error) {
    statusLabel.textContent = 'Impossible de charger la configuration.';
  } else {
    statusLabel.textContent = 'Configurez des canaux pour le multimètre dans la page de configuration.';
  }

  const getCurrentChannel = () => {
    const selected = inputSelect.value;
    if (!selected) {
      return null;
    }
    return channelMap.get(selected) || null;
  };

  const getCurrentProfile = () => MEASUREMENT_PROFILES[state.measurementMode] || MEASUREMENT_PROFILES[defaultMode];

  const getCurrentCalibre = () => {
    const profile = getCurrentProfile();
    return profile.calibrations.find((cal) => cal.id === state.calibreId) || profile.calibrations[0];
  };

  const getChannelSymbol = (channel, profile) => {
    if (channel && channel.symbol && channel.symbol.length) {
      return channel.symbol;
    }
    return profile.symbol;
  };

  const getChannelUnit = (channel, calibre) => {
    if (channel && channel.unit && channel.unit.length) {
      return channel.unit;
    }
    return calibre.unit;
  };

  const getEffectiveRange = (calibre, channel) => {
    const defaultMax = calibre && Number.isFinite(calibre.max) ? calibre.max : 1;
    let min = channel && Number.isFinite(channel.rangeMin) ? channel.rangeMin : null;
    let max = channel && Number.isFinite(channel.rangeMax) ? channel.rangeMax : null;
    if (min === null || max === null || min >= max) {
      const positiveOnly = POSITIVE_ONLY_PROFILES.has(state.measurementMode);
      min = positiveOnly ? 0 : -defaultMax;
      max = defaultMax;
    }
    return { min, max };
  };

  const updateGaugeLabels = (calibre, channel) => {
    const range = getEffectiveRange(calibre, channel);
    const multiplier = calibre && Number.isFinite(calibre.displayMultiplier) ? calibre.displayMultiplier : 1;
    const decimals = calibre && Number.isFinite(calibre.decimals) ? calibre.decimals : 2;
    const unit = getChannelUnit(channel, calibre);
    const minText = formatDisplay(range.min * multiplier, decimals);
    const maxText = formatDisplay(range.max * multiplier, decimals);
    gaugeMinLabel.textContent = unit ? `${minText} ${unit}` : minText;
    gaugeMaxLabel.textContent = unit ? `${maxText} ${unit}` : maxText;
  };

  const updateSymbolAndUnit = () => {
    const profile = getCurrentProfile();
    const calibre = getCurrentCalibre();
    const channel = getCurrentChannel();
    symbolDisplay.textContent = getChannelSymbol(channel, profile);
    unitDisplay.textContent = getChannelUnit(channel, calibre);
    updateGaugeLabels(calibre, channel);
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
        state.lastScaledValue = null;
        state.lastRawValue = null;
        state.heldScaledValue = null;
        state.heldRawValue = null;
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

  const initialiseGauge = () => {
    if (gaugeArc) {
      const start = polarToCartesian(GAUGE_START_ANGLE, GAUGE_RADIUS);
      const end = polarToCartesian(GAUGE_END_ANGLE, GAUGE_RADIUS);
      const largeArc = GAUGE_END_ANGLE - GAUGE_START_ANGLE <= 180 ? 0 : 1;
      const path = `M ${start.x} ${start.y} A ${GAUGE_RADIUS} ${GAUGE_RADIUS} 0 ${largeArc} 1 ${end.x} ${end.y}`;
      gaugeArc.setAttribute('d', path);
    }
    if (gaugeTicksGroup) {
      gaugeTicksGroup.innerHTML = '';
      const tickCount = 10;
      for (let i = 0; i <= tickCount; i += 1) {
        const ratio = i / tickCount;
        const angle = GAUGE_START_ANGLE + (GAUGE_END_ANGLE - GAUGE_START_ANGLE) * ratio;
        const outer = polarToCartesian(angle, GAUGE_TICK_OUTER_RADIUS);
        const inner = polarToCartesian(angle, GAUGE_TICK_INNER_RADIUS);
        const tick = document.createElementNS(SVG_NS, 'line');
        tick.setAttribute('x1', inner.x.toFixed(2));
        tick.setAttribute('y1', inner.y.toFixed(2));
        tick.setAttribute('x2', outer.x.toFixed(2));
        tick.setAttribute('y2', outer.y.toFixed(2));
        tick.classList.add('multimeter-gauge-tick');
        gaugeTicksGroup.appendChild(tick);
      }
    }
  };

  const getMeterScale = (channel) => {
    if (!channel || !Number.isFinite(channel.scale)) {
      return 1;
    }
    return channel.scale || 1;
  };

  const getMeterOffset = (channel) => {
    if (!channel || !Number.isFinite(channel.offset)) {
      return 0;
    }
    return channel.offset;
  };

  const applyMeterScaling = (rawValue, channel) => {
    if (rawValue === null || Number.isNaN(rawValue) || !channel) {
      return null;
    }
    const scale = getMeterScale(channel);
    const offset = getMeterOffset(channel);
    return rawValue * scale + offset;
  };

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

  const updateDigitalDisplay = (value, profile, calibre, channel) => {
    const unit = getChannelUnit(channel, calibre);
    if (value === null || Number.isNaN(value)) {
      valueDisplay.textContent = '—';
      bar.value = 0;
      unitDisplay.textContent = unit;
      symbolDisplay.textContent = getChannelSymbol(channel, profile);
      return;
    }
    const range = getEffectiveRange(calibre, channel);
    if (value < range.min || value > range.max) {
      valueDisplay.textContent = 'OL';
      bar.value = 100;
      unitDisplay.textContent = unit;
      symbolDisplay.textContent = getChannelSymbol(channel, profile);
      return;
    }
    const multiplier = calibre && Number.isFinite(calibre.displayMultiplier) ? calibre.displayMultiplier : 1;
    const decimals = calibre && Number.isFinite(calibre.decimals) ? calibre.decimals : 2;
    const displayValue = value * multiplier;
    valueDisplay.textContent = formatDisplay(displayValue, decimals);
    unitDisplay.textContent = unit;
    symbolDisplay.textContent = getChannelSymbol(channel, profile);
    const span = (range.max - range.min) * multiplier;
    const ratio = span === 0 ? 0 : (displayValue - range.min * multiplier) / span;
    bar.value = clamp(ratio * 100, 0, 100);
  };

  const updateBinaryDisplay = (value, channel) => {
    const bits = channel && Number.isFinite(channel.bits)
      ? Math.min(32, Math.max(1, Math.round(channel.bits)))
      : 10;
    if (!channel || value === null || Number.isNaN(value)) {
      binaryValueDisplay.textContent = '—';
      binaryBitsLabel.textContent = `${bits} bit${bits > 1 ? 's' : ''}`;
      return;
    }
    const scale = getMeterScale(channel);
    if (!Number.isFinite(scale) || scale === 0) {
      binaryValueDisplay.textContent = '—';
      binaryBitsLabel.textContent = `${bits} bit${bits > 1 ? 's' : ''}`;
      return;
    }
    const offset = getMeterOffset(channel);
    const maxCode = Math.pow(2, bits) - 1;
    const rawCode = Math.round((value - offset) / scale);
    const clampedCode = clamp(rawCode, 0, maxCode);
    const binaryString = clampedCode.toString(2).padStart(bits, '0');
    binaryValueDisplay.textContent = `0b${binaryString}`;
    binaryBitsLabel.textContent = `${bits} bit${bits > 1 ? 's' : ''} • ${clampedCode}/${maxCode}`;
  };

  const updateGaugeDisplay = (value, profile, calibre, channel) => {
    const unit = getChannelUnit(channel, calibre);
    const multiplier = calibre && Number.isFinite(calibre.displayMultiplier) ? calibre.displayMultiplier : 1;
    const decimals = calibre && Number.isFinite(calibre.decimals) ? calibre.decimals : 2;
    const range = getEffectiveRange(calibre, channel);
    let ratio = 0;
    let readout = '—';
    if (value !== null && !Number.isNaN(value)) {
      const span = range.max - range.min;
      if (span !== 0) {
        ratio = clamp((value - range.min) / span, 0, 1);
      }
      if (value < range.min || value > range.max) {
        readout = 'OL';
      } else {
        readout = `${formatDisplay(value * multiplier, decimals)} ${unit}`.trim();
      }
    }
    const angle = GAUGE_START_ANGLE + (GAUGE_END_ANGLE - GAUGE_START_ANGLE) * ratio;
    if (gaugeNeedle) {
      const tip = polarToCartesian(angle, GAUGE_RADIUS);
      gaugeNeedle.setAttribute('x2', tip.x.toFixed(2));
      gaugeNeedle.setAttribute('y2', tip.y.toFixed(2));
    }
    if (gaugeReadout) {
      gaugeReadout.textContent = readout;
    }
  };

  const renderCurrentValue = () => {
    const profile = getCurrentProfile();
    const calibre = getCurrentCalibre();
    const channel = getCurrentChannel();
    const value = state.hold ? state.heldScaledValue : state.lastScaledValue;
    updateDigitalDisplay(value, profile, calibre, channel);
    updateBinaryDisplay(value, channel);
    updateGaugeDisplay(value, profile, calibre, channel);
  };

  const updateDisplayModeVisibility = () => {
    displayModes.forEach((element) => {
      const targetMode = element.dataset.mode;
      element.hidden = targetMode !== state.displayMode;
    });
    displayButtons.forEach((button) => {
      const isActive = button.dataset.display === state.displayMode;
      button.classList.toggle('is-active', isActive);
      button.setAttribute('aria-pressed', isActive ? 'true' : 'false');
    });
  };

  const setDisplayMode = (mode) => {
    const desired = DISPLAY_MODES.includes(mode) ? mode : 'digital';
    if (state.displayMode === desired) {
      return;
    }
    state.displayMode = desired;
    updateDisplayModeVisibility();
    renderCurrentValue();
  };

  const displayOff = () => {
    valueDisplay.textContent = '----';
    unitDisplay.textContent = '';
    symbolDisplay.textContent = '';
    bar.value = 0;
    acdcLabel.textContent = '';
    binaryValueDisplay.textContent = '—';
    binaryBitsLabel.textContent = '—';
    if (gaugeReadout) {
      gaugeReadout.textContent = '—';
    }
    if (gaugeNeedle) {
      const tip = polarToCartesian(GAUGE_START_ANGLE, GAUGE_RADIUS);
      gaugeNeedle.setAttribute('x2', tip.x.toFixed(2));
      gaugeNeedle.setAttribute('y2', tip.y.toFixed(2));
    }
  };

  const refreshMeasurement = async ({ force = false } = {}) => {
    if (!state.powered) {
      return;
    }
    const channel = getCurrentChannel();
    const profile = getCurrentProfile();
    const calibre = getCurrentCalibre();
    acdcLabel.textContent = state.acMode === 'ac' ? 'AC' : 'DC';

    if (!channel) {
      state.lastRawValue = null;
      state.lastScaledValue = null;
      state.previousValue = null;
      updatedLabel.textContent = '—';
      statusLabel.textContent = 'Aucun canal sélectionné';
      renderCurrentValue();
      return;
    }

    if (state.hold && !force) {
      statusLabel.textContent = 'Maintien de la lecture (HOLD)';
      renderCurrentValue();
      return;
    }

    try {
      const snapshot = await fetchInputsSnapshot();
      const raw = Object.prototype.hasOwnProperty.call(snapshot, channel.input)
        ? Number(snapshot[channel.input])
        : NaN;
      state.lastRawValue = Number.isFinite(raw) ? raw : null;
      const scaled = applyMeterScaling(state.lastRawValue, channel);
      const processed = applyAcMode(Number.isFinite(scaled) ? scaled : null);
      const smoothed = getSmoothedValue(processed);
      state.lastScaledValue = Number.isFinite(smoothed) ? smoothed : null;
      renderCurrentValue();
      const now = new Date();
      updatedLabel.textContent = now.toLocaleTimeString('fr-FR', { hour12: false });
      const label = channel.label || channel.name || channel.input;
      const suffix = channel.input && channel.input !== label ? ` (source ${channel.input})` : '';
      statusLabel.textContent = `Lecture ${state.acMode === 'ac' ? 'AC' : 'DC'} sur ${label}${suffix}`;
    } catch (err) {
      console.warn('[VirtualLab] Lecture des entrées impossible:', err);
      const fallbackBase = randomFallbackBase(calibre);
      const processed = applyAcMode(fallbackBase);
      const smoothed = getSmoothedValue(processed);
      state.lastRawValue = null;
      state.lastScaledValue = Number.isFinite(smoothed) ? smoothed : null;
      renderCurrentValue();
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
      state.lastRawValue = null;
      state.lastScaledValue = null;
      state.heldRawValue = null;
      state.heldScaledValue = null;
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
      renderCurrentValue();
      refreshMeasurement();
    });
  });

  displayButtons.forEach((button) => {
    button.addEventListener('click', () => {
      const desiredMode = button.dataset.display;
      setDisplayMode(desiredMode);
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
      if (state.lastScaledValue === null) {
        await refreshMeasurement({ force: true });
      }
      state.heldRawValue = state.lastRawValue;
      state.heldScaledValue = state.lastScaledValue;
      holdIndicator.hidden = false;
      statusLabel.textContent = state.heldScaledValue !== null
        ? 'Maintien de la lecture (HOLD)'
        : 'Maintien actif - en attente de mesure';
      renderCurrentValue();
    } else {
      state.heldRawValue = null;
      state.heldScaledValue = null;
      holdIndicator.hidden = true;
      refreshMeasurement();
    }
  });

  inputSelect.addEventListener('change', () => {
    state.previousValue = null;
    state.lastRawValue = null;
    state.lastScaledValue = null;
    state.heldRawValue = null;
    state.heldScaledValue = null;
    if (state.hold) {
      state.hold = false;
      holdButton.classList.remove('is-active');
      holdButton.setAttribute('aria-pressed', 'false');
      holdIndicator.hidden = true;
    }
    updateSymbolAndUnit();
    renderCurrentValue();
    refreshMeasurement();
  });

  window.addEventListener('beforeunload', () => {
    if (state.timer) {
      clearInterval(state.timer);
    }
  });

  initialiseGauge();
  updateSymbolAndUnit();
  renderCalibrations(getCurrentProfile());
  updateDisplayModeVisibility();
  renderCurrentValue();
  refreshMeasurement();
  scheduleRefresh();
}
