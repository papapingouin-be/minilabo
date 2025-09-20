const WAVE_TYPES = {
  sine: { label: 'Sinus', description: 'Signal pur pour les mesures de référence.' },
  square: { label: 'Carré', description: 'Temps de montée rapide pour tests logiques.' },
  ramp: { label: 'Rampe', description: 'Variation linéaire pour stimulation progressive.' },
  pulse: { label: 'Impulsion', description: 'Impulsions fines pour tests de réponse.' }
};

function drawPreview(canvas, type) {
  const ctx = canvas.getContext('2d');
  const { width, height } = canvas;
  ctx.clearRect(0, 0, width, height);
  ctx.strokeStyle = 'rgba(255, 179, 71, 0.22)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 6; i += 1) {
    const y = (height / 6) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }

  ctx.strokeStyle = 'rgba(255, 208, 128, 0.9)';
  ctx.lineWidth = 2.1;
  ctx.beginPath();
  const cycles = 2;
  const samples = 480;
  for (let i = 0; i <= samples; i += 1) {
    const ratio = i / samples;
    const x = ratio * width;
    let yRatio = 0;
    const phase = ratio * cycles * Math.PI * 2;
    switch (type) {
      case 'square':
        yRatio = Math.sin(phase) >= 0 ? 0.15 : 0.85;
        break;
      case 'ramp':
        yRatio = 0.15 + ((ratio * cycles) % 1) * 0.7;
        break;
      case 'pulse':
        yRatio = phase % (Math.PI * 2) < 0.3 ? 0.2 : 0.85;
        break;
      case 'sine':
      default:
        yRatio = 0.5 - Math.sin(phase) * 0.35;
        break;
    }
    const y = yRatio * height;
    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }
  ctx.stroke();
}

function formatFrequency(value) {
  if (value >= 1_000_000) {
    return `${(value / 1_000_000).toFixed(2)} MHz`;
  }
  if (value >= 1000) {
    return `${(value / 1000).toFixed(1)} kHz`;
  }
  return `${value.toFixed(0)} Hz`;
}

function populateSelect(select, options, placeholder) {
  select.innerHTML = '';
  if (!options.length) {
    const opt = document.createElement('option');
    opt.value = '';
    opt.textContent = placeholder;
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

export function mountFunctionGenerator(container, { outputs = [], error = null } = {}) {
  if (!container) return;
  container.innerHTML = `
    <div class="device-shell generator-shell">
      <div class="device-header">
        <div class="device-branding">
          <span class="device-brand">MiniLabBox</span>
          <span class="device-model" id="function-generator-title">Générateur virtuel</span>
          <span class="device-subtitle">Création de signaux</span>
        </div>
        <div class="device-toolbar">
          <a class="device-config-button" href="settings.html#generator" aria-label="Configurer le générateur de fonctions virtuel">
            ⚙️ Configurer
          </a>
          <div class="io-selector">
            <label for="function-io">Sortie assignée</label>
            <select id="function-io" class="io-select"></select>
          </div>
        </div>
      </div>
      <div class="device-status" id="function-generator-status"></div>
      <div class="generator-body">
        <div class="generator-visual">
          <canvas width="520" height="160" aria-label="Aperçu signal" id="function-preview"></canvas>
          <div id="function-summary" class="generator-summary"></div>
        </div>
        <div class="generator-controls">
          <label>
            Forme d'onde
            <select id="function-wave" class="io-select"></select>
          </label>
          <label>
            Fréquence
            <input type="range" id="function-frequency" min="10" max="1000000" step="10" value="1000">
          </label>
          <label>
            Amplitude (Vpp)
            <input type="range" id="function-amplitude" min="0.2" max="10" step="0.2" value="2">
          </label>
          <label>
            Offset (V)
            <input type="range" id="function-offset" min="-5" max="5" step="0.1" value="0">
          </label>
          <label>
            Sortie
            <select id="function-output" class="io-select">
              <option value="on">Activée</option>
              <option value="off">Désactivée</option>
            </select>
          </label>
          <label>
            Harmoniques
            <input type="range" id="function-harmonics" min="0" max="100" step="10" value="20">
          </label>
        </div>
      </div>
    </div>
  `;

  const preview = container.querySelector('#function-preview');
  const summary = container.querySelector('#function-summary');
  const waveSelect = container.querySelector('#function-wave');
  const frequency = container.querySelector('#function-frequency');
  const amplitude = container.querySelector('#function-amplitude');
  const offset = container.querySelector('#function-offset');
  const output = container.querySelector('#function-output');
  const harmonics = container.querySelector('#function-harmonics');
  const ioSelect = container.querySelector('#function-io');
  const statusLabel = container.querySelector('#function-generator-status');

  populateSelect(ioSelect, outputs, 'Aucune sortie active');
  if (outputs.length) {
    ioSelect.value = outputs[0].name;
    statusLabel.textContent = `${outputs.length} sortie(s) configurées`;
  } else if (error) {
    statusLabel.textContent = 'Impossible de charger la configuration des sorties.';
  } else {
    statusLabel.textContent = 'Activez une sortie dans la configuration.';
  }

  Object.entries(WAVE_TYPES).forEach(([value, { label }]) => {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = label;
    waveSelect.appendChild(option);
  });

  const update = () => {
    const currentWave = waveSelect.value;
    drawPreview(preview, currentWave);
    const amplitudeValue = parseFloat(amplitude.value).toFixed(1);
    const offsetValue = parseFloat(offset.value).toFixed(1);
    const freqText = formatFrequency(parseFloat(frequency.value));
    const waveInfo = WAVE_TYPES[currentWave];
    const targetOutput = ioSelect.value ? `Sortie ${ioSelect.value}` : 'Pas de sortie assignée';
    const harmonicContent = `${harmonics.value}%`;
    const outputState = output.value === 'on' ? 'ON' : 'OFF';
    summary.textContent = `${waveInfo.label} • ${freqText} • ${amplitudeValue} Vpp • Offset ${offsetValue} V • ${targetOutput} • Harmoniques ${harmonicContent} • Sortie ${outputState}`;
  };

  [waveSelect, frequency, amplitude, offset, output, harmonics, ioSelect].forEach((input) => {
    input.addEventListener('input', update);
    input.addEventListener('change', update);
  });

  waveSelect.value = 'sine';
  update();
}
