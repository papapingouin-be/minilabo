const MAX_METER_CHANNELS = 6;

const MEASUREMENT_OPTIONS = [
  { id: 'voltage', label: 'Tension (U)' },
  { id: 'resistance', label: 'Résistance (R)' },
  { id: 'current', label: 'Courant (I)' },
  { id: 'frequency', label: 'Fréquence (f)' },
  { id: 'capacitance', label: 'Capacité (C)' },
  { id: 'inductance', label: 'Inductance (L)' },
];

const DISPLAY_OPTIONS = [
  { id: 'digital', label: 'Affichage numérique' },
  { id: 'binary', label: 'Code binaire' },
  { id: 'gauge', label: 'Cadran analogique' },
];

const MEASUREMENT_IDS = new Set(MEASUREMENT_OPTIONS.map((option) => option.id));
const DISPLAY_IDS = new Set(DISPLAY_OPTIONS.map((option) => option.id));

const state = {
  channels: [],
  inputs: [],
};

function getInputInfoByName(name) {
  if (!name) {
    return null;
  }
  return state.inputs.find((item) => item.name === name) || null;
}

function isEsp8266AdcInput(inputInfo) {
  if (!inputInfo) {
    return false;
  }
  const pin = normaliseString(inputInfo.pin).toUpperCase();
  const type = normaliseString(inputInfo.type).toLowerCase();
  return pin === 'A0' || type === 'adc' || type === 'analog';
}

function buildEsp8266AdcScript() {
  return `// Conversion complète depuis la broche A0 de l'ESP8266 (ADC interne 10 bits, 0-1 V).
const {
  raw = 0,
  scale = 1,
  offset = 0,
  history = [],
  timestamp = Date.now(),
  measurement = 'voltage',
  acMode = 'dc',
} = context;

const ADC_RESOLUTION = 1023;
const ADC_VREF = 1.0; // Tension pleine échelle sur A0.

// Tension instantanée issue de l'échantillon brut.
const tensionInstantanee = (raw / ADC_RESOLUTION) * ADC_VREF;

// Fenêtre d'échantillonnage en volts pour calculer moyenne, RMS et crêtes.
const fenetre = [];
for (let i = 0; i < history.length; i += 1) {
  const point = history[i];
  if (point && Number.isFinite(point.raw)) {
    fenetre.push((point.raw / ADC_RESOLUTION) * ADC_VREF);
  }
}
fenetre.push(tensionInstantanee);

const tailleFenetre = fenetre.length || 1;
const tensionMoyenne = fenetre.reduce((acc, valeur) => acc + valeur, 0) / tailleFenetre;
const tensionRms = Math.sqrt(fenetre.reduce((acc, valeur) => acc + valeur * valeur, 0) / tailleFenetre);
const creteMax = fenetre.reduce((acc, valeur) => (valeur > acc ? valeur : acc), fenetre[0]);
const creteMin = fenetre.reduce((acc, valeur) => (valeur < acc ? valeur : acc), fenetre[0]);
const amplitudeCreteAC = creteMax - creteMin;

// Application de l'échelle/décalage configurés sur le canal.
const mesureCalibree = tensionInstantanee * scale + offset;

// Mode de mesure sélectionné sur le multimètre (tension, courant, etc.).
const mode = typeof measurement === 'string' && measurement.length ? measurement : 'voltage';

// Estimation de fréquence par passages par zéro sur la fenêtre d'échantillonnage.
const marqueursTemps = [];
for (let i = 0; i < history.length; i += 1) {
  const point = history[i];
  if (point && Number.isFinite(point.timestamp)) {
    marqueursTemps.push(point.timestamp);
  }
}
marqueursTemps.push(timestamp);
let passages = 0;
for (let i = 1; i < fenetre.length; i += 1) {
  const precedent = fenetre[i - 1] - tensionMoyenne;
  const courant = fenetre[i] - tensionMoyenne;
  if ((precedent <= 0 && courant > 0) || (precedent >= 0 && courant < 0)) {
    passages += 1;
  }
}
const dureeFenetre = marqueursTemps.length > 1 ? (marqueursTemps[marqueursTemps.length - 1] - marqueursTemps[0]) / 1000 : 0;
const frequenceEstimee = dureeFenetre > 0 ? (passages / 2) / dureeFenetre : 0;

// Table de conversion pour chaque fonction de mesure disponible.
const conversions = {
  voltage: {
    valeur: mesureCalibree,
    unite: 'V',
    symbole: 'U',
    commentaire: "Tension instantanée calibrée (DC : moyenne, AC : RMS).",
  },
  current: {
    valeur: mesureCalibree,
    unite: 'A',
    symbole: 'I',
    commentaire: "Adapter scale = 1/Rshunt pour I = U/R.",
  },
  resistance: {
    valeur: mesureCalibree,
    unite: 'Ω',
    symbole: 'R',
    commentaire: "Injecter un courant connu et fixer scale = 1/Itest pour R = U/I.",
  },
  frequency: {
    valeur: frequenceEstimee * scale + offset,
    unite: 'Hz',
    symbole: 'f',
    commentaire: "Estimation par comptage des passages par zéro sur l'échantillonnage.",
  },
  capacitance: {
    valeur: mesureCalibree,
    unite: 'F',
    symbole: 'C',
    commentaire: "Utiliser l'échelon scale pour appliquer I = C·dU/dt.",
  },
  inductance: {
    valeur: mesureCalibree,
    unite: 'H',
    symbole: 'L',
    commentaire: "Utiliser l'échelon scale pour appliquer U = L·dI/dt.",
  },
};

const mesureActive = conversions[mode] || conversions.voltage;

// Synthèse des grandeurs utiles pour l'analyse (moyenne, RMS, crêtes).
const synthese = {
  moyenne: tensionMoyenne,
  rms: tensionRms,
  creteMax,
  creteMin,
  creteAC: amplitudeCreteAC,
  acMode,
};

return {
  value: mesureActive.valeur,
  unit: mesureActive.unite,
  symbol: mesureActive.symbole,
  decimals: mode === 'voltage' ? 3 : 2,
  measurementMode: mode,
  details: synthese,
  commentaire: mesureActive.commentaire,
};`;
}

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

function getDefaultProcessingScript(profile, inputInfo = null) {
  if (isEsp8266AdcInput(inputInfo)) {
    return buildEsp8266AdcScript();
  }
  if (profile === 'frequency') {
    return [
      "// Estimation simple de la fréquence à partir d'un compteur cumulatif",
      "// raw : valeur brute, scale : coefficient d'échelle, offset : décalage",
      "// history : dernières mesures (raw, scaled, value, timestamp)",
      "// L'objet Math de JavaScript est disponible.",
      'if (!history.length) {',
      '  return raw * scale + offset;',
      '}',
      'const previous = history[history.length - 1];',
      'const dt = (timestamp - previous.timestamp) / 1000;',
      'if (dt <= 0) {',
      '  return previous.value ?? 0;',
      '}',
      'const delta = raw - previous.raw;',
      'const estimate = Math.abs(delta) / dt;',
      "// Pour personnaliser l'affichage, retournez un objet { value, unit, symbol }.",
      '// Retourne une estimation en hertz.',
      'return Number.isFinite(estimate) && estimate > 0 ? estimate : Math.max(previous.value ?? 0, 0);',
    ].join('\n');
  }
  return [
    "// raw : valeur brute provenant de l'entrée",
    "// scale : coefficient multiplicatif • offset : décalage ajouté ensuite",
    "// Retournez la valeur à afficher.",
    "// L'objet Math de JavaScript est disponible.",
    "// Il est aussi possible de retourner un objet { value, unit, symbol } pour ajuster l'affichage.",
    'return raw * scale + offset;',
  ].join('\n');
}

function clampBits(value) {
  let bits = normaliseNumber(value, 10);
  if (!Number.isFinite(bits)) {
    bits = 10;
  }
  bits = Math.round(bits);
  if (bits < 1) bits = 1;
  if (bits > 32) bits = 32;
  return bits;
}

function normaliseChannel(entry, index) {
  const id = normaliseString(entry && entry.id) || `meter${index + 1}`;
  const name = normaliseString(entry && entry.name) || id;
  const label = normaliseString(entry && entry.label) || name;
  const input = normaliseString(entry && entry.input);
  const unit = normaliseString(entry && entry.unit);
  const symbol = normaliseString(entry && entry.symbol);
  const enabled = entry && entry.enabled === false ? false : true;
  const scale = normaliseNumber(entry && entry.scale, 1);
  const offset = normaliseNumber(entry && entry.offset, 0);
  const rangeMin = entry && Object.prototype.hasOwnProperty.call(entry, 'rangeMin')
    ? normaliseNumber(entry.rangeMin, null)
    : null;
  const rangeMax = entry && Object.prototype.hasOwnProperty.call(entry, 'rangeMax')
    ? normaliseNumber(entry.rangeMax, null)
    : null;
  const bits = clampBits(entry && entry.bits);
  let profile = normaliseString(entry && entry.profile);
  if (!MEASUREMENT_IDS.has(profile)) {
    profile = 'voltage';
  }
  let displayMode = normaliseString(entry && entry.displayMode);
  if (!DISPLAY_IDS.has(displayMode)) {
    displayMode = 'digital';
  }
  const calibre = normaliseString(entry && entry.calibre);
  const processing = typeof entry?.processing === 'string' ? entry.processing : '';
  return {
    id,
    name,
    label,
    input,
    unit,
    symbol,
    enabled,
    scale,
    offset,
    rangeMin,
    rangeMax,
    bits,
    profile,
    displayMode,
    calibre,
    processing,
    _lastTemplate: null,
  };
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
  const profile = 'voltage';
  const processing = getDefaultProcessingScript(profile, baseInput);
  return {
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
    profile,
    displayMode: 'digital',
    calibre: '',
    processing,
    _lastTemplate: processing,
  };
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

function handleInputSelection(select, channel, refreshSummary, onInputChanged) {
  select.innerHTML = '';
  const placeholder = document.createElement('option');
  placeholder.value = '';
  placeholder.textContent = '— Aucune entrée —';
  select.appendChild(placeholder);
  state.inputs.forEach((item) => {
    const opt = document.createElement('option');
    opt.value = item.name;
    const unit = item.unit ? ` (${item.unit})` : '';
    opt.textContent = `${item.name}${unit}`;
    select.appendChild(opt);
  });
  select.value = channel.input || '';
  select.addEventListener('change', (event) => {
    channel.input = event.target.value.trim();
    refreshSummary();
    if (typeof onInputChanged === 'function') {
      onInputChanged(channel.input);
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

function createNumberField(label, channel, key, options = {}) {
  const input = document.createElement('input');
  input.type = 'number';
  if (options.step) input.step = options.step;
  if (options.min !== undefined) input.min = options.min;
  if (options.max !== undefined) input.max = options.max;
  if (options.placeholder) input.placeholder = options.placeholder;
  const value = channel[key];
  input.value = value === null || value === undefined ? '' : value;
  input.addEventListener('change', (event) => {
    const raw = event.target.value.trim();
    if (raw.length === 0 && options.allowNull) {
      channel[key] = null;
      event.target.value = '';
      return;
    }
    const num = Number(raw);
    if (!Number.isFinite(num)) {
      if (options.allowNull) {
        channel[key] = null;
        event.target.value = '';
      }
      return;
    }
    let nextValue = num;
    if (options.clamp) {
      nextValue = Math.max(options.clamp.min, Math.min(options.clamp.max, nextValue));
    }
    if (options.round) {
      nextValue = Math.round(nextValue);
    }
    channel[key] = nextValue;
    event.target.value = nextValue;
  });
  return createField(label, input);
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

function createToggle(label, channel, key, refreshSummary) {
  const wrapper = document.createElement('label');
  wrapper.className = 'toggle';
  const checkbox = document.createElement('input');
  checkbox.type = 'checkbox';
  checkbox.checked = channel[key] !== false;
  checkbox.addEventListener('change', (event) => {
    channel[key] = !!event.target.checked;
    if (refreshSummary) {
      refreshSummary();
    }
  });
  wrapper.append(checkbox, document.createTextNode(label));
  return wrapper;
}

function renderChannels() {
  const container = document.getElementById('channelsContainer');
  const emptyState = document.getElementById('emptyState');
  const addButton = document.getElementById('addChannelBtn');
  if (!container) return;
  container.innerHTML = '';
  state.channels.forEach((channel, index) => {
    const card = document.createElement('div');
    card.className = 'channel-card';

    const header = document.createElement('header');
    const title = document.createElement('h2');
    const subtitle = document.createElement('span');
    const refreshSummary = () => {
      const fallback = channel.id || `Canal ${index + 1}`;
      title.textContent = channel.label || channel.name || fallback;
      if (channel.input) {
        const modeLabel = MEASUREMENT_OPTIONS.find((option) => option.id === (channel.profile || 'voltage'));
        const measurement = modeLabel ? modeLabel.label : 'Mesure';
        subtitle.textContent = `${measurement} • Entrée : ${channel.input}`;
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
      createTextField('Identifiant (JSON)', channel, 'id', refreshSummary),
      createTextField('Nom interne', channel, 'name', refreshSummary),
      createTextField('Libellé affiché', channel, 'label', refreshSummary),
    );
    card.appendChild(firstRow);

    const inputSelect = document.createElement('select');
    card.appendChild(createField('Entrée associée', inputSelect));

    const measurementRow = document.createElement('div');
    measurementRow.className = 'field-row';

    const profileSelect = document.createElement('select');
    MEASUREMENT_OPTIONS.forEach((option) => {
      const opt = document.createElement('option');
      opt.value = option.id;
      opt.textContent = option.label;
      profileSelect.appendChild(opt);
    });
    const currentProfile = MEASUREMENT_IDS.has(channel.profile) ? channel.profile : 'voltage';
    profileSelect.value = currentProfile;
    channel.profile = currentProfile;
    measurementRow.appendChild(createField('Mesure par défaut', profileSelect));

    const displaySelect = document.createElement('select');
    DISPLAY_OPTIONS.forEach((option) => {
      const opt = document.createElement('option');
      opt.value = option.id;
      opt.textContent = option.label;
      displaySelect.appendChild(opt);
    });
    const currentDisplay = DISPLAY_IDS.has(channel.displayMode) ? channel.displayMode : 'digital';
    displaySelect.value = currentDisplay;
    channel.displayMode = currentDisplay;
    measurementRow.appendChild(createField('Affichage préféré', displaySelect));

    const calibreInput = document.createElement('input');
    calibreInput.type = 'text';
    calibreInput.placeholder = 'ex. 20V';
    calibreInput.value = channel.calibre || '';
    calibreInput.addEventListener('input', (event) => {
      channel.calibre = event.target.value.trim();
      if (channel.calibre) {
        channel.activeCalibre = channel.calibre;
      }
    });
    measurementRow.appendChild(createField('Calibre initial (optionnel)', calibreInput));
    card.appendChild(measurementRow);

    const scriptArea = document.createElement('textarea');
    scriptArea.classList.add('code-editor');
    scriptArea.spellcheck = false;
    scriptArea.autocapitalize = 'off';
    scriptArea.autocomplete = 'off';
    scriptArea.autocorrect = 'off';
    scriptArea.rows = 14;
    scriptArea.wrap = 'off';
    scriptArea.placeholder = 'Variables disponibles : raw, scale, offset, history, timestamp, measurement, acMode…';
    const scriptId = `${channel.id || `channel-${index + 1}`}-script`;
    scriptArea.id = scriptId;

    const applyTemplate = (force = false) => {
      const inputInfo = getInputInfoByName(channel.input);
      const template = getDefaultProcessingScript(channel.profile, inputInfo);
      if (
        force ||
        !scriptArea.value.trim().length ||
        (channel._lastTemplate && channel._lastTemplate === scriptArea.value)
      ) {
        channel.processing = template;
        channel._lastTemplate = template;
        scriptArea.value = template;
      }
    };

    if (!channel.processing || !channel.processing.trim().length) {
      applyTemplate(true);
    } else {
      scriptArea.value = channel.processing;
      const inputInfo = getInputInfoByName(channel.input);
      const defaultTemplate = getDefaultProcessingScript(channel.profile, inputInfo);
      if (channel.processing && channel.processing.trim() === defaultTemplate.trim()) {
        channel._lastTemplate = defaultTemplate;
      } else {
        channel._lastTemplate = null;
      }
    }

    scriptArea.addEventListener('input', (event) => {
      channel.processing = event.target.value;
      if (channel._lastTemplate && event.target.value !== channel._lastTemplate) {
        channel._lastTemplate = null;
      }
    });

    profileSelect.addEventListener('change', (event) => {
      const value = event.target.value;
      channel.profile = MEASUREMENT_IDS.has(value) ? value : 'voltage';
      refreshSummary();
      applyTemplate(false);
    });

    displaySelect.addEventListener('change', (event) => {
      const value = event.target.value;
      channel.displayMode = DISPLAY_IDS.has(value) ? value : 'digital';
    });

    handleInputSelection(inputSelect, channel, refreshSummary, () => applyTemplate(false));

    const secondRow = document.createElement('div');
    secondRow.className = 'field-row';
    secondRow.append(
      createTextField('Unité', channel, 'unit'),
      createTextField('Symbole', channel, 'symbol'),
    );
    card.appendChild(secondRow);

    const thirdRow = document.createElement('div');
    thirdRow.className = 'field-row';
    thirdRow.append(
      createNumberField('Échelle', channel, 'scale', { step: 'any' }),
      createNumberField('Décalage', channel, 'offset', { step: 'any' }),
    );
    card.appendChild(thirdRow);

    const scriptField = document.createElement('div');
    scriptField.className = 'field code-field';
    const scriptLabel = document.createElement('label');
    scriptLabel.textContent = 'Logique de mesure (JavaScript)';
    scriptLabel.setAttribute('for', scriptId);
    const codeToolbar = document.createElement('div');
    codeToolbar.className = 'code-toolbar';
    const helpButton = document.createElement('button');
    helpButton.type = 'button';
    helpButton.className = 'code-help-btn';
    helpButton.textContent = '📘 Aide mesures';
    helpButton.setAttribute('aria-expanded', 'false');
    const editorWrapper = document.createElement('div');
    editorWrapper.className = 'code-editor-wrapper';
    editorWrapper.appendChild(scriptArea);
    const helpPanel = document.createElement('div');
    helpPanel.className = 'code-help-panel';
    helpPanel.hidden = true;
    helpPanel.setAttribute('role', 'dialog');
    helpPanel.setAttribute('aria-label', 'Aide programmation des mesures');
    helpPanel.tabIndex = -1;
    helpPanel.innerHTML = `
      <p><strong>Variables de calcul</strong> – valeurs disponibles pour construire vos formules&nbsp;:</p>
      <ul>
        <li><code>raw</code> : lecture brute (0 à 1023 pour A0) fournie par l'échantillonnage.</li>
        <li><code>scale</code> et <code>offset</code> : réglages du canal pour l'étalonnage.</li>
        <li><code>history</code> : tableau des derniers points {raw, scaled, value, timestamp} (fenêtre d'échantillonnage).</li>
        <li><code>timestamp</code> : horodatage en millisecondes du point courant.</li>
        <li><code>measurement</code> : mode sélectionné (tension, courant, résistance, etc.).</li>
        <li><code>acMode</code> : lecture en DC ou AC (bouton AC/DC du multimètre).</li>
        <li><code>Math</code> : fonctions mathématiques JavaScript (sin, sqrt, pow...).</li>
      </ul>
      <p>Utilisez ces grandeurs pour calculer une moyenne glissante, une valeur efficace (RMS), des valeurs de crête ou encore le pic à pic à partir de la fenêtre d'échantillonnage.</p>
      <p>Retournez soit un nombre, soit un objet <code>{ value, unit, symbol, decimals }</code> pour piloter l'affichage du multimètre.</p>
    `;
    helpButton.addEventListener('click', () => {
      const expanded = helpPanel.hidden;
      helpPanel.hidden = !expanded;
      helpButton.setAttribute('aria-expanded', expanded ? 'true' : 'false');
      if (!helpPanel.hidden) {
        helpPanel.focus();
      }
    });
    codeToolbar.appendChild(helpButton);
    scriptField.append(scriptLabel, codeToolbar, editorWrapper, helpPanel);
    card.appendChild(scriptField);

    const fourthRow = document.createElement('div');
    fourthRow.className = 'field-row';
    fourthRow.append(
      createNumberField('Plage min. (optionnelle)', channel, 'rangeMin', { step: 'any', allowNull: true, placeholder: '—' }),
      createNumberField('Plage max. (optionnelle)', channel, 'rangeMax', { step: 'any', allowNull: true, placeholder: '—' }),
    );
    card.appendChild(fourthRow);

    const fifthRow = document.createElement('div');
    fifthRow.className = 'field-row';
    fifthRow.append(
      createNumberField('Résolution (bits)', channel, 'bits', {
        min: 1,
        max: 32,
        clamp: { min: 1, max: 32 },
        round: true,
      }),
    );
    card.appendChild(fifthRow);

    const toggleWrapper = document.createElement('div');
    toggleWrapper.appendChild(createToggle('Activer ce canal', channel, 'enabled', refreshSummary));
    card.appendChild(toggleWrapper);

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
    const enabled = channel.enabled !== false;
    const scale = Number.isFinite(channel.scale) ? channel.scale : 1;
    const offset = Number.isFinite(channel.offset) ? channel.offset : 0;
    const rangeMin = channel.rangeMin === null || channel.rangeMin === undefined
      ? null
      : Number(channel.rangeMin);
    const rangeMax = channel.rangeMax === null || channel.rangeMax === undefined
      ? null
      : Number(channel.rangeMax);
    const bits = clampBits(channel.bits);
    const profile = MEASUREMENT_IDS.has(channel.profile) ? channel.profile : 'voltage';
    const displayMode = DISPLAY_IDS.has(channel.displayMode) ? channel.displayMode : 'digital';
    const calibre = normaliseString(channel.calibre);
    const processing = typeof channel.processing === 'string' ? channel.processing : '';
    return {
      id,
      name,
      label,
      input,
      unit,
      symbol,
      enabled,
      scale,
      offset,
      rangeMin,
      rangeMax,
      bits,
      profile,
      displayMode,
      calibre,
      processing,
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
