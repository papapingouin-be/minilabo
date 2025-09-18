const DEFAULT_INPUT_FALLBACK = (index) => `IN${index + 1}`;
const DEFAULT_OUTPUT_FALLBACK = (index) => `OUT${index + 1}`;
const MAX_METER_BITS = 32;
const MIN_METER_BITS = 1;

function normaliseNumber(value) {
  if (value === null || value === undefined) {
    return null;
  }
  const num = typeof value === 'number' ? value : Number(value);
  return Number.isFinite(num) ? num : null;
}

function normaliseMeterChannel(entry, index, inputsByName) {
  if (!entry || typeof entry !== 'object') {
    return null;
  }
  const idSource = typeof entry.id === 'string' && entry.id.trim().length
    ? entry.id.trim()
    : typeof entry.name === 'string' && entry.name.trim().length
      ? entry.name.trim()
      : `meter${index + 1}`;
  const inputName = typeof entry.input === 'string' && entry.input.trim().length
    ? entry.input.trim()
    : '';
  const scale = normaliseNumber(entry.scale);
  const offset = normaliseNumber(entry.offset);
  const rangeMin = normaliseNumber(entry.rangeMin);
  const rangeMax = normaliseNumber(entry.rangeMax);
  const rawBits = normaliseNumber(entry.bits);
  const bits = Number.isFinite(rawBits)
    ? Math.min(MAX_METER_BITS, Math.max(MIN_METER_BITS, Math.round(rawBits)))
    : 10;
  const unit = typeof entry.unit === 'string' ? entry.unit.trim() : '';
  const symbol = typeof entry.symbol === 'string' ? entry.symbol.trim() : '';
  const enabled = entry.enabled !== false;
  const label = typeof entry.label === 'string' && entry.label.trim().length
    ? entry.label.trim()
    : idSource;
  const baseInput = inputsByName.get(inputName);
  return {
    id: idSource,
    name: idSource,
    label,
    input: inputName,
    unit: unit || (baseInput && baseInput.unit ? baseInput.unit : ''),
    symbol,
    enabled,
    scale: Number.isFinite(scale) ? scale : 1,
    offset: Number.isFinite(offset) ? offset : 0,
    rangeMin: Number.isFinite(rangeMin) ? rangeMin : null,
    rangeMax: Number.isFinite(rangeMax) ? rangeMax : null,
    bits
  };
}

function normaliseName(name, fallbackFactory, index) {
  if (name && typeof name === 'string' && name.trim().length) {
    return name.trim();
  }
  return fallbackFactory(index);
}

async function ensureAuthSession() {
  if (typeof window.ensureSession === 'function') {
    try {
      await window.ensureSession();
    } catch (err) {
      console.warn('[VirtualLab] Session check failed:', err);
    }
  }
}

function getFetcher() {
  if (typeof window.authFetch === 'function') {
    return window.authFetch.bind(window);
  }
  return (url, options = {}) => fetch(url, { credentials: 'include', ...options });
}

async function fetchJson(url) {
  const response = await getFetcher()(url, { headers: { Accept: 'application/json' } });
  if (!response.ok) {
    const message = await response.text().catch(() => '');
    throw new Error(message || `Requête HTTP ${response.status}`);
  }
  return response.json();
}

export async function loadIoCatalog() {
  await ensureAuthSession();
  try {
    const config = await fetchJson('/api/config/get');
    const inputs = Array.isArray(config.inputs)
      ? config.inputs
          .map((item, index) => ({ item, index }))
          .filter(({ item }) => item && (item.active ?? true))
          .map(({ item, index }) => ({
            name: normaliseName(item.name, DEFAULT_INPUT_FALLBACK, index),
            unit: item.unit || '',
            type: item.type || 'unknown',
            scale: normaliseNumber(item.scale),
            offset: normaliseNumber(item.offset)
          }))
      : [];
    const inputsByName = new Map(inputs.map((item) => [item.name, item]));
    const meterChannels = [];
    if (config.virtualMultimeter && Array.isArray(config.virtualMultimeter.channels)) {
      config.virtualMultimeter.channels.forEach((entry, index) => {
        const channel = normaliseMeterChannel(entry, index, inputsByName);
        if (!channel || !channel.enabled || !channel.input) {
          return;
        }
        meterChannels.push(channel);
        const inputInfo = inputsByName.get(channel.input);
        if (inputInfo) {
          inputInfo.meter = channel;
        }
      });
    }
    const outputs = Array.isArray(config.outputs)
      ? config.outputs
          .map((item, index) => ({ item, index }))
          .filter(({ item }) => item && (item.active ?? true))
          .map(({ item, index }) => ({
            name: normaliseName(item.name, DEFAULT_OUTPUT_FALLBACK, index),
            type: item.type || 'unknown',
            scale: normaliseNumber(item.scale),
            offset: normaliseNumber(item.offset)
          }))
      : [];
    return { inputs, outputs, multimeterChannels: meterChannels, error: null };
  } catch (error) {
    console.warn('[VirtualLab] Chargement configuration IO impossible:', error);
    return { inputs: [], outputs: [], multimeterChannels: [], error };
  }
}

export async function fetchInputsSnapshot() {
  await ensureAuthSession();
  try {
    const data = await fetchJson('/api/inputs');
    const map = {};
    if (data && Array.isArray(data.inputs)) {
      data.inputs.forEach((entry) => {
        if (entry && entry.name) {
          map[entry.name] = normaliseNumber(entry.value);
        }
      });
    }
    return map;
  } catch (error) {
    console.warn('[VirtualLab] Lecture des entrées impossible:', error);
    throw error;
  }
}

export async function fetchOutputsSnapshot() {
  await ensureAuthSession();
  try {
    const data = await fetchJson('/api/outputs');
    const map = {};
    if (data && Array.isArray(data.outputs)) {
      data.outputs.forEach((entry) => {
        if (entry && entry.name) {
          map[entry.name] = {
            value: normaliseNumber(entry.value),
            active: !!entry.active
          };
        }
      });
    }
    return map;
  } catch (error) {
    console.warn('[VirtualLab] Lecture des sorties impossible:', error);
    throw error;
  }
}

export async function sendOutputValue(name, value) {
  await ensureAuthSession();
  const response = await getFetcher()('/api/output/set', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name, value })
  });
  if (!response.ok) {
    const text = await response.text().catch(() => '');
    throw new Error(text || `Écriture sortie refusée (${response.status})`);
  }
  return response.json().catch(() => ({}));
}
