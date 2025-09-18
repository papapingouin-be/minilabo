(function() {
  'use strict';

  async function loadConfig() {
    const resp = await authFetch('/api/config/get');
    if (!resp.ok) {
      throw new Error('Impossible de charger la configuration');
    }
    return resp.json();
  }

  function normaliseNumber(value) {
    if (value === null || value === undefined) return null;
    const num = typeof value === 'number' ? value : Number(value);
    return Number.isFinite(num) ? num : null;
  }

  function mapInputsByType(cfg, type) {
    if (!cfg || !Array.isArray(cfg.inputs)) return [];
    return cfg.inputs
      .filter(item => item && item.type === type && item.active)
      .map(item => ({
        name: item.name,
        unit: item.unit || '',
        scale: normaliseNumber(item.scale) ?? 1,
        offset: normaliseNumber(item.offset) ?? 0,
        pin: item.pin || '',
        adsChannel: item.adsChannel,
        remoteNode: item.remoteNode || '',
        remoteName: item.remoteName || ''
      }));
  }

  function mapOutputsByType(cfg, type) {
    if (!cfg || !Array.isArray(cfg.outputs)) return [];
    return cfg.outputs
      .filter(item => item && item.type === type && item.active)
      .map(item => ({
        name: item.name,
        scale: normaliseNumber(item.scale) ?? 1,
        offset: normaliseNumber(item.offset) ?? 0,
        pwmFreq: item.pwmFreq,
        i2cAddress: item.i2cAddress || ''
      }));
  }

  async function fetchInputsValues() {
    const resp = await authFetch('/api/inputs');
    if (!resp.ok) {
      throw new Error('Lecture des entrées impossible');
    }
    const data = await resp.json();
    const result = {};
    if (data && Array.isArray(data.inputs)) {
      data.inputs.forEach(entry => {
        if (entry && entry.name) {
          result[entry.name] = normaliseNumber(entry.value);
        }
      });
    }
    return result;
  }

  async function fetchOutputsValues() {
    const resp = await authFetch('/api/outputs');
    if (!resp.ok) {
      throw new Error('Lecture des sorties impossible');
    }
    const data = await resp.json();
    const result = {};
    if (data && Array.isArray(data.outputs)) {
      data.outputs.forEach(entry => {
        if (entry && entry.name) {
          result[entry.name] = {
            value: normaliseNumber(entry.value),
            active: !!entry.active
          };
        }
      });
    }
    return result;
  }

  async function sendOutputValue(name, value) {
    const payload = JSON.stringify({ name, value });
    const resp = await authFetch('/api/output/set', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: payload
    });
    if (!resp.ok) {
      const text = await resp.text();
      throw new Error(text || 'Écriture de sortie refusée');
    }
    return resp.json().catch(() => ({}));
  }

  function computeRawValue(value, scale, offset) {
    const val = normaliseNumber(value);
    const gain = normaliseNumber(scale);
    const off = normaliseNumber(offset) ?? 0;
    if (val === null || gain === null || gain === 0) {
      return null;
    }
    const raw = (val - off) / gain;
    return Number.isFinite(raw) ? raw : null;
  }

  function applyOutputScale(value, scale, offset) {
    const val = normaliseNumber(value);
    const gain = normaliseNumber(scale);
    const off = normaliseNumber(offset) ?? 0;
    if (val === null || gain === null) {
      return null;
    }
    const raw = val * gain + off;
    return Number.isFinite(raw) ? raw : null;
  }

  function slugFromName(name) {
    if (!name) return '';
    return name.toString().trim().toLowerCase().normalize('NFD')
      .replace(/[\u0300-\u036f]/g, '')
      .replace(/[^a-z0-9]+/g, '_')
      .replace(/^_+|_+$/g, '');
  }

  function formatNumber(value, digits = 3) {
    const num = normaliseNumber(value);
    if (num === null) {
      return '—';
    }
    return num.toFixed(digits);
  }

  window.ExampleUtils = {
    loadConfig,
    inputsByType: mapInputsByType,
    outputsByType: mapOutputsByType,
    fetchInputsValues,
    fetchOutputsValues,
    sendOutputValue,
    computeRawValue,
    applyOutputScale,
    normaliseNumber,
    slugFromName,
    formatNumber
  };
})();
