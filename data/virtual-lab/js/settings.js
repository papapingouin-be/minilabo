const MAX_METER_CHANNELS = 6;

const DEFAULT_FORMULA = 'return raw * scale + offset;';

const MEASUREMENT_DEFINITIONS = [
  { id: 'voltageDC', label: 'Tension U (DC)', measurement: 'voltage', acMode: 'dc', defaultEnabled: true, defaultFormula: DEFAULT_FORMULA },
  { id: 'voltageAC', label: 'Tension U (AC)', measurement: 'voltage', acMode: 'ac', defaultEnabled: false, defaultFormula: DEFAULT_FORMULA },
  { id: 'frequency', label: 'Fréquence f', measurement: 'frequency', acMode: null, defaultEnabled: false, defaultFormula: 'return raw;' },
  { id: 'current', label: 'Courant I', measurement: 'current', acMode: null, defaultEnabled: false, defaultFormula: DEFAULT_FORMULA },
  { id: 'resistance', label: 'Résistance R', measurement: 'resistance', acMode: null, defaultEnabled: false, defaultFormula: DEFAULT_FORMULA },
  { id: 'capacitance', label: 'Capacité C', measurement: 'capacitance', acMode: null, defaultEnabled: false, defaultFormula: DEFAULT_FORMULA },
  { id: 'inductance', label: 'Inductance L', measurement: 'inductance', acMode: null, defaultEnabled: false, defaultFormula: DEFAULT_FORMULA },
];

const MEASUREMENT_IDS = new Set(MEASUREMENT_DEFINITIONS.map((definition) => definition.id));
const MEASUREMENT_DEFINITION_BY_ID = new Map(MEASUREMENT_DEFINITIONS.map((definition) => [definition.id, definition]));
const MEASUREMENT_GROUPS = MEASUREMENT_DEFINITIONS.reduce((groups, definition) => {
  if (!groups[definition.measurement]) {
    groups[definition.measurement] = [];
  }
  groups[definition.measurement].push(definition);
  return groups;
}, {});

const state = {
  channels: [],
  inputs: [],
};

function normaliseString(value) {
  if (typeof value !== 'string') {
    return '';
  }
  return value.trim();
}

function normaliseNumber(value, fallback = null) {
  if (value === null || value === undefined) {
    return fallback;
  }
  const num = typeof value === 'number' ? value : Number(value);
  if (!Number.isFinite(num)) {
    return fallback;
  }
  return num;
}

function createDefaultMeasurements() {
  const measurements = {};
  MEASUREMENT_DEFINITIONS.forEach((definition) => {
    measurements[definition.id] = {
      enabled: !!definition.defaultEnabled,
      min: null,
      max: null,
      formula: definition.defaultFormula || DEFAULT_FORMULA,
    };
  });
  return measurements;
}

function normaliseMeasurements(source) {
  const measurements = createDefaultMeasurements();
  if (!source || typeof source !== 'object') {
    return measurements;
  }
  Object.entries(source).forEach(([key, value]) => {
    if (!MEASUREMENT_IDS.has(key)) {
      return;
    }
    const target = measurements[key];
    const definition = MEASUREMENT_DEFINITION_BY_ID.get(key);
    target.enabled = value && value.enabled === false ? false : true;
    const min = normaliseNumber(value && (value.min ?? value.rangeMin), null);
    target.min = Number.isFinite(min) ? min : null;
    const max = normaliseNumber(value && (value.max ?? value.rangeMax), null);
    target.max = Number.isFinite(max) ? max : null;
    const formula = typeof value?.formula === 'string' ? value.formula : '';
    target.formula = formula.trim().length
      ? formula
      : (definition?.defaultFormula || DEFAULT_FORMULA);
  });
  return measurements;
}

function ensureMeasurements(channel) {
  if (!channel.measurements || typeof channel.measurements !== 'object') {
    channel.measurements = createDefaultMeasurements();
  }
  return channel.measurements;
}

function updateChannelEnabled(channel) {
  const measurements = ensureMeasurements(channel);
  channel.enabled = Object.values(measurements).some((entry) => entry && entry.enabled);
}

function updateChannelRanges(channel) {
  const measurements = ensureMeasurements(channel);
  const dc = measurements.voltageDC;
  const min = dc && Number.isFinite(dc.min) ? dc.min : null;
  const max = dc && Number.isFinite(dc.max) ? dc.max : null;
  channel.rangeMin = min;
  channel.rangeMax = max;
}

function wrapFormulaForScript(formula, fallback) {
  const body = typeof formula === 'string' && formula.trim().length
    ? formula
    : fallback;
  const lines = body.split('\n').map((line) => line.replace(/\s+$/, ''));
  const wrapped = [
    '(() => {',
    ...lines.map((line) => `      ${line}`),
    '    })()',
  ];
  return wrapped.join('\n');
}

function buildProcessingFromMeasurements(channel) {
  const measurements = ensureMeasurements(channel);
  const lines = [
    '// Script généré automatiquement à partir des formules configurées pour chaque grandeur.',
    'const ctx = context || {};',
    'const raw = Number.isFinite(ctx.raw) ? ctx.raw : 0;',
    'const scale = Number.isFinite(ctx.scale) ? ctx.scale : 1;',
    'const offset = Number.isFinite(ctx.offset) ? ctx.offset : 0;',
    'const history = Array.isArray(ctx.history) ? ctx.history : [];',
    'const timestamp = ctx.timestamp ?? Date.now();',
    "const measurement = typeof ctx.measurement === 'string' ? ctx.measurement : 'voltage';",
    "const acMode = ctx.acMode === 'ac' ? 'ac' : 'dc';",
    '',
    'switch (measurement) {',
  ];

  Object.entries(MEASUREMENT_GROUPS).forEach(([measurementKey, definitions]) => {
    lines.push(`  case '${measurementKey}': {`);
    definitions.forEach((definition) => {
      const entry = measurements[definition.id];
      if (!entry || !entry.enabled) {
        return;
      }
      const fallbackFormula = definition.defaultFormula || DEFAULT_FORMULA;
      const script = wrapFormulaForScript(entry.formula, fallbackFormula);
      if (definition.acMode === 'ac') {
        lines.push("    if (acMode === 'ac') {");
        lines.push(`      return ${script};`);
        lines.push('    }');
      } else if (definition.acMode === 'dc') {
        lines.push("    if (acMode !== 'ac') {");
        lines.push(`      return ${script};`);
        lines.push('    }');
      } else {
        lines.push(`    return ${script};`);
      }
    });
    lines.push('    return null;');
    lines.push('  }');
  });

  lines.push('  default:');
  lines.push('    return null;');
  lines.push('}');
  return lines.join('\n');
}

function updateChannelProcessing(channel) {
  channel.processing = buildProcessingFromMeasurements(channel);
}

function normaliseChannel(entry, index) {
  const id = normaliseString(entry && entry.id) || `meter${index + 1}`;
  const name = normaliseString(entry && entry.name) || id;
  const label = normaliseString(entry && entry.label) || name;
  const input = normaliseString(entry && entry.input);
  const unit = normaliseString(entry && entry.unit);
  const symbol = normaliseString(entry && entry.symbol);
  const scale = normaliseNumber(entry && entry.scale, 1);
  const offset = normaliseNumber(entry && entry.offset, 0);
  const bits = normaliseNumber(entry && entry.bits, 10);
  const profile = normaliseString(entry && entry.profile) || 'voltage';
  const displayMode = normaliseString(entry && entry.displayMode) || 'digital';
  const calibre = normaliseString(entry && entry.calibre);
  const measurements = normaliseMeasurements(entry && entry.measurements);
  const channel = {
    id,
    name,
    label,
    input,
    unit,
    symbol,
    enabled: entry && entry.enabled === false ? false : true,
    scale: Number.isFinite(scale) ? scale : 1,
    offset: Number.isFinite(offset) ? offset : 0,
    rangeMin: null,
    rangeMax: null,
    bits: Number.isFinite(bits) ? Math.min(32, Math.max(1, Math.round(bits))) : 10,
    profile,
    displayMode,
    calibre,
    processing: typeof entry?.processing === 'string' ? entry.processing : '',
    measurements,
  };
  updateChannelRanges(channel);
  updateChannelEnabled(channel);
  updateChannelProcessing(channel);
  return channel;
}

function nextDefaultId() {
  let index = state.channels.length;
  while (true) {
    const candidate = `meter${index + 1}`;
    const exists = state.channels.some((channel) => channel.id === candidate);
    if (!exists) {
      return candidate;
    }
    index += 1;
  }
}

function createDefaultChannel() {
  const id = nextDefaultId();
  const baseInput = state.inputs.find((item) => item.active) || state.inputs[0];
  const measurements = createDefaultMeasurements();
  const channel = {
    id,
    name: id,
    label: `Canal ${state.channels.length + 1}`,
    input: baseInput ? baseInput.name : '',
    unit: baseInput && baseInput.unit ? baseInput.unit : '',
    symbol: '',
    enabled: true,
    scale: 1,
    offset: 0,
    rangeMin: null,
    rangeMax: null,
    bits: 10,
    profile: 'voltage',
    displayMode: 'digital',
    calibre: '',
    processing: '',
    measurements,
  };
  updateChannelRanges(channel);
  updateChannelEnabled(channel);
  updateChannelProcessing(channel);
  return channel;
}

function updateCounter() {
  const counter = document.getElementById('channelsCounter');
  if (!counter) return;
  const count = state.channels.length;
  if (count === 0) {
    counter.textContent = 'Aucun canal configuré';
  } else if (count === 1) {
    counter.textContent = '1 canal configuré';
  } else {
    counter.textContent = `${count} canaux configurés`;
  }
}

function setStatus(message, type = '') {
  const status = document.getElementById('statusMessage');
  if (!status) return;
  status.textContent = message || '';
  status.classList.remove('error', 'success');
  if (type) {
    status.classList.add(type);
  }
}

function handleInputSelection(select, channel, refreshSummary) {
  select.innerHTML = '';
  const activeInputs = state.inputs.filter((item) => item.active);
  const placeholder = document.createElement('option');
  placeholder.value = '';
  placeholder.textContent = activeInputs.length ? '— Aucune entrée —' : 'Aucune entrée active';
  select.appendChild(placeholder);
  activeInputs.forEach((item) => {
    const opt = document.createElement('option');
    opt.value = item.name;
    const unit = item.unit ? ` (${item.unit})` : '';
    opt.textContent = `${item.name}${unit}`;
    select.appendChild(opt);
  });
  select.value = channel.input || '';
  select.disabled = !activeInputs.length;
  select.addEventListener('change', (event) => {
    channel.input = event.target.value.trim();
    if (refreshSummary) {
      refreshSummary();
    }
  });
}

function createField(labelText, element) {
  const wrapper = document.createElement('div');
  wrapper.className = 'field';
  const label = document.createElement('label');
  label.textContent = labelText;
  wrapper.append(label, element);
  return wrapper;
}

function createTextField(label, channel, key, refreshSummary) {
  const input = document.createElement('input');
  input.type = 'text';
  input.value = channel[key] || '';
  input.addEventListener('input', (event) => {
    channel[key] = event.target.value.trim();
    if (refreshSummary) {
      refreshSummary();
    }
  });
  return createField(label, input);
}

function createMeasurementCard(channel, definition, refreshSummary) {
  const measurements = ensureMeasurements(channel);
  const data = measurements[definition.id];
  const card = document.createElement('div');
  card.className = 'measurement-card';

  const header = document.createElement('div');
  header.className = 'measurement-card-header';
  const title = document.createElement('h3');
  title.textContent = definition.label;
  header.appendChild(title);

  const toggleLabel = document.createElement('label');
  toggleLabel.className = 'measurement-toggle';
  const toggleInput = document.createElement('input');
  toggleInput.type = 'checkbox';
  toggleInput.checked = data.enabled;
  const toggleText = document.createElement('span');
  toggleText.textContent = 'Active';
  toggleLabel.append(toggleInput, toggleText);
  header.appendChild(toggleLabel);
  card.appendChild(header);

  const rangeRow = document.createElement('div');
  rangeRow.className = 'measurement-range';
  const minLabel = document.createElement('label');
  minLabel.textContent = 'Valeur min.';
  const minInput = document.createElement('input');
  minInput.type = 'number';
  minInput.step = 'any';
  minInput.value = Number.isFinite(data.min) ? data.min : '';
  minInput.placeholder = '—';
  minInput.disabled = !data.enabled;
  minInput.addEventListener('change', (event) => {
    const value = normaliseNumber(event.target.value, null);
    data.min = Number.isFinite(value) ? value : null;
    if (definition.id === 'voltageDC') {
      updateChannelRanges(channel);
    }
  });
  minLabel.appendChild(minInput);

  const maxLabel = document.createElement('label');
  maxLabel.textContent = 'Valeur max.';
  const maxInput = document.createElement('input');
  maxInput.type = 'number';
  maxInput.step = 'any';
  maxInput.value = Number.isFinite(data.max) ? data.max : '';
  maxInput.placeholder = '—';
  maxInput.disabled = !data.enabled;
  maxInput.addEventListener('change', (event) => {
    const value = normaliseNumber(event.target.value, null);
    data.max = Number.isFinite(value) ? value : null;
    if (definition.id === 'voltageDC') {
      updateChannelRanges(channel);
    }
  });
  maxLabel.appendChild(maxInput);

  rangeRow.append(minLabel, maxLabel);
  card.appendChild(rangeRow);

  const formulaLabel = document.createElement('label');
  formulaLabel.className = 'measurement-formula-label';
  formulaLabel.textContent = 'Formule (JavaScript)';
  const formulaArea = document.createElement('textarea');
  formulaArea.className = 'measurement-formula';
  formulaArea.rows = 4;
  formulaArea.spellcheck = false;
  formulaArea.autocapitalize = 'off';
  formulaArea.autocomplete = 'off';
  formulaArea.autocorrect = 'off';
  formulaArea.placeholder = definition.defaultFormula || DEFAULT_FORMULA;
  formulaArea.value = data.formula || '';
  formulaArea.disabled = !data.enabled;
  formulaArea.addEventListener('input', (event) => {
    data.formula = event.target.value;
    updateChannelProcessing(channel);
  });
  card.append(formulaLabel, formulaArea);

  toggleInput.addEventListener('change', (event) => {
    data.enabled = !!event.target.checked;
    minInput.disabled = !data.enabled;
    maxInput.disabled = !data.enabled;
    formulaArea.disabled = !data.enabled;
    updateChannelEnabled(channel);
    updateChannelProcessing(channel);
    if (definition.id === 'voltageDC') {
      updateChannelRanges(channel);
    }
    if (refreshSummary) {
      refreshSummary();
    }
  });

  return card;
}

function renderChannels() {
  const container = document.getElementById('channelsContainer');
  const emptyState = document.getElementById('emptyState');
  const addButton = document.getElementById('addChannelBtn');
  if (!container) return;
  container.innerHTML = '';

  state.channels.forEach((channel, index) => {
    ensureMeasurements(channel);
    const card = document.createElement('div');
    card.className = 'channel-card';

    const header = document.createElement('header');
    const title = document.createElement('h2');
    const subtitle = document.createElement('span');
    const refreshSummary = () => {
      const fallback = channel.id || `Canal ${index + 1}`;
      title.textContent = channel.label || channel.name || fallback;
      const measurements = ensureMeasurements(channel);
      const activeCount = Object.values(measurements).filter((entry) => entry && entry.enabled).length;
      if (channel.input) {
        const measurementText = activeCount === 1 ? '1 grandeur active' : `${activeCount} grandeurs actives`;
        subtitle.textContent = `${measurementText} • Entrée : ${channel.input}`;
      } else {
        subtitle.textContent = 'Aucune entrée assignée';
      }
    };
    refreshSummary();
    header.append(title, subtitle);
    card.appendChild(header);

    const firstRow = document.createElement('div');
    firstRow.className = 'field-row';
    firstRow.append(
      createTextField('Nom', channel, 'name', refreshSummary),
      createTextField('Libellé affiché', channel, 'label', refreshSummary),
    );
    card.appendChild(firstRow);

    const inputSelect = document.createElement('select');
    handleInputSelection(inputSelect, channel, refreshSummary);
    card.appendChild(createField('Entrée associée', inputSelect));

    const measurementsGrid = document.createElement('div');
    measurementsGrid.className = 'measurement-grid';
    MEASUREMENT_DEFINITIONS.forEach((definition) => {
      measurementsGrid.appendChild(createMeasurementCard(channel, definition, refreshSummary));
    });
    card.appendChild(measurementsGrid);

    const actions = document.createElement('div');
    actions.className = 'card-actions';
    const removeBtn = document.createElement('button');
    removeBtn.type = 'button';
    removeBtn.className = 'remove';
    removeBtn.textContent = 'Supprimer';
    removeBtn.addEventListener('click', () => {
      state.channels.splice(index, 1);
      renderChannels();
      updateCounter();
      setStatus('Canal supprimé.', '');
    });
    actions.appendChild(removeBtn);
    card.appendChild(actions);

    container.appendChild(card);
  });

  if (emptyState) {
    emptyState.hidden = state.channels.length > 0;
  }
  if (addButton) {
    addButton.disabled = state.channels.length >= MAX_METER_CHANNELS;
  }
  updateCounter();
}

function serialiseChannels() {
  return state.channels.map((channel, index) => {
    const id = normaliseString(channel.id) || `meter${index + 1}`;
    const name = normaliseString(channel.name) || id;
    const label = normaliseString(channel.label) || name;
    const input = normaliseString(channel.input);
    const unit = normaliseString(channel.unit);
    const symbol = normaliseString(channel.symbol);
    const measurements = normaliseMeasurements(channel.measurements);
    channel.measurements = measurements;
    updateChannelRanges(channel);
    updateChannelEnabled(channel);
    updateChannelProcessing(channel);
    const serialisedMeasurements = {};
    Object.entries(measurements).forEach(([key, value]) => {
      serialisedMeasurements[key] = {
        enabled: value.enabled !== false,
        min: Number.isFinite(value.min) ? value.min : null,
        max: Number.isFinite(value.max) ? value.max : null,
        formula: typeof value.formula === 'string' ? value.formula : '',
      };
    });
    return {
      id,
      name,
      label,
      input,
      unit,
      symbol,
      enabled: channel.enabled !== false,
      scale: Number.isFinite(channel.scale) ? channel.scale : 1,
      offset: Number.isFinite(channel.offset) ? channel.offset : 0,
      rangeMin: channel.rangeMin === null ? null : channel.rangeMin,
      rangeMax: channel.rangeMax === null ? null : channel.rangeMax,
      bits: Number.isFinite(channel.bits) ? Math.min(32, Math.max(1, Math.round(channel.bits))) : 10,
      profile: normaliseString(channel.profile) || 'voltage',
      displayMode: normaliseString(channel.displayMode) || 'digital',
      calibre: normaliseString(channel.calibre),
      processing: typeof channel.processing === 'string' ? channel.processing : '',
      measurements: serialisedMeasurements,
    };
  });
}

async function loadConfiguration() {
  try {
    await window.ensureSession?.();
  } catch (err) {
    setStatus('Authentification requise.', 'error');
    return;
  }
  setStatus('Chargement de la configuration…');
  try {
    const response = await window.authFetch('/api/config/get');
    if (!response.ok) {
      throw new Error('HTTP ' + response.status);
    }
    const cfg = await response.json();
    const inputs = Array.isArray(cfg.inputs) ? cfg.inputs : [];
    state.inputs = inputs.map((entry) => ({
      name: normaliseString(entry && entry.name),
      unit: normaliseString(entry && entry.unit),
      active: entry && entry.active !== false,
      type: normaliseString(entry && entry.type),
      pin: normaliseString(entry && entry.pin),
    })).filter((item) => item.name.length > 0);
    let channels = [];
    if (cfg.virtualMultimeter && Array.isArray(cfg.virtualMultimeter.channels)) {
      channels = cfg.virtualMultimeter.channels.map(normaliseChannel);
    }
    state.channels = channels;
    renderChannels();
    updateCounter();
    setStatus('Configuration chargée.', 'success');
  } catch (err) {
    console.error('[VirtualLab] Impossible de charger la configuration', err);
    setStatus('Impossible de charger la configuration.', 'error');
  }
}

async function saveChannels() {
  const saveBtn = document.getElementById('saveBtn');
  const payload = {
    channelCount: state.channels.length,
    channels: serialiseChannels(),
  };
  try {
    if (saveBtn) saveBtn.disabled = true;
    setStatus('Sauvegarde en cours…');
    const response = await window.authFetch('/api/config/virtual-multimeter', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    const text = await response.text();
    let data = null;
    if (text && text.length) {
      try {
        data = JSON.parse(text);
      } catch (err) {
        data = null;
      }
    }
    if (!response.ok) {
      const message = data && data.error ? data.error : 'La sauvegarde a échoué.';
      setStatus(message, 'error');
      return;
    }
    if (data && data.applied && Array.isArray(data.applied.channels)) {
      state.channels = data.applied.channels.map(normaliseChannel);
      renderChannels();
    }
    setStatus('Canaux enregistrés avec succès.', 'success');
  } catch (err) {
    console.error('[VirtualLab] Sauvegarde impossible', err);
    setStatus('Erreur réseau lors de la sauvegarde.', 'error');
  } finally {
    if (saveBtn) saveBtn.disabled = false;
  }
}

function setupListeners() {
  const addButton = document.getElementById('addChannelBtn');
  if (addButton) {
    addButton.addEventListener('click', () => {
      if (state.channels.length >= MAX_METER_CHANNELS) {
        setStatus(`Nombre maximal de canaux atteint (${MAX_METER_CHANNELS}).`, 'error');
        return;
      }
      state.channels.push(createDefaultChannel());
      renderChannels();
      updateCounter();
      setStatus('Nouveau canal ajouté.', '');
    });
  }
  const saveBtn = document.getElementById('saveBtn');
  if (saveBtn) {
    saveBtn.addEventListener('click', saveChannels);
  }
}

window.addEventListener('DOMContentLoaded', () => {
  setupListeners();
  loadConfiguration();
});
