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
  ctx.strokeStyle = 'rgba(148, 163, 184, 0.35)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 6; i += 1) {
    const y = (height / 6) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }

  ctx.strokeStyle = '#45d1ff';
  ctx.lineWidth = 2;
  ctx.beginPath();
  const cycles = 2;
  const samples = 500;
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
        yRatio = 0.15 + (ratio * cycles % 1) * 0.7;
        break;
      case 'pulse':
        yRatio = (phase % (Math.PI * 2)) < 0.3 ? 0.15 : 0.85;
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

export function mountFunctionGenerator(container) {
  if (!container) return;
  container.innerHTML = `
    <span class="status-badge">Générateur RF/Arb</span>
    <h3>Générateur de fonctions 2 canaux</h3>
    <p>
      Sélectionnez vos formes d'onde et paramètres de sortie pour alimenter bancs d'essai et prototypes.
    </p>
    <div class="instrument-display">
      <canvas width="520" height="160" aria-label="Aperçu signal" id="function-preview"></canvas>
      <div id="function-summary" class="measurement-unit"></div>
    </div>
    <div class="instrument-controls">
      <label>
        Forme d'onde
        <select id="function-wave"></select>
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
        <select id="function-output">
          <option value="on">Activée</option>
          <option value="off">Désactivée</option>
        </select>
      </label>
    </div>
  `;

  const preview = container.querySelector('#function-preview');
  const summary = container.querySelector('#function-summary');
  const waveSelect = container.querySelector('#function-wave');
  const frequency = container.querySelector('#function-frequency');
  const amplitude = container.querySelector('#function-amplitude');
  const offset = container.querySelector('#function-offset');
  const output = container.querySelector('#function-output');

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
    summary.textContent = `${waveInfo.label} • ${freqText} • ${amplitudeValue} Vpp • Offset ${offsetValue} V • Sortie ${output.value === 'on' ? 'ON' : 'OFF'}`;
  };

  [waveSelect, frequency, amplitude, offset, output].forEach((input) => {
    input.addEventListener('input', update);
    input.addEventListener('change', update);
  });

  waveSelect.value = 'sine';
  update();
}
