const WAVEFORMS = {
  sinus: {
    label: 'Sinusoïdale',
    generator: (x) => Math.sin(x)
  },
  carree: {
    label: 'Carrée',
    generator: (x) => (Math.sin(x) >= 0 ? 1 : -1)
  },
  dentDeScie: {
    label: 'Dent de scie',
    generator: (x) => ((x / Math.PI) % 2) - 1
  }
};

function formatTimebase(value) {
  if (value < 1) {
    return `${(value * 1000).toFixed(0)} µs/div`;
  }
  if (value >= 1000) {
    return `${(value / 1000).toFixed(2)} s/div`;
  }
  return `${value.toFixed(1)} ms/div`;
}

function drawWaveform(canvas, options) {
  const ctx = canvas.getContext('2d');
  const { waveform, amplitude, offset, intensity } = options;
  const { width, height } = canvas;

  ctx.clearRect(0, 0, width, height);

  ctx.lineWidth = 1;
  ctx.strokeStyle = 'rgba(96, 165, 250, 0.25)';
  const divisions = 8;
  for (let i = 1; i < divisions; i += 1) {
    const x = (width / divisions) * i;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, height);
    ctx.stroke();
    const y = (height / divisions) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }

  ctx.strokeStyle = `rgba(69, 209, 255, ${intensity})`;
  ctx.lineWidth = 2.2;
  ctx.beginPath();
  const samples = 640;
  for (let i = 0; i <= samples; i += 1) {
    const ratio = i / samples;
    const x = ratio * width;
    const angle = ratio * Math.PI * 8;
    const value = waveform(angle);
    const y = height / 2 - (value * amplitude + offset) * (height / 3);
    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }
  ctx.stroke();
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

export function mountOscilloscope(container, { inputs = [], error = null } = {}) {
  if (!container) return;
  container.innerHTML = `
    <div class="device-shell oscilloscope-shell">
      <div class="device-header">
        <div class="device-branding">
          <span class="device-brand">Tektronix</span>
          <span class="device-model" id="oscilloscope-title">Série 3 MSO</span>
          <span class="device-subtitle">Oscilloscope numérique quatre voies</span>
        </div>
        <div class="io-selector">
          <label for="oscilloscope-channel">Entrée active</label>
          <select id="oscilloscope-channel" class="io-select"></select>
        </div>
      </div>
      <div class="device-status" id="oscilloscope-status"></div>
      <div class="oscilloscope-body">
        <div class="oscilloscope-screen">
          <div class="oscilloscope-display">
            <canvas class="oscilloscope-display-canvas" width="640" height="280" role="img"
              aria-label="Affichage oscillogramme"></canvas>
          </div>
          <div class="oscilloscope-readout">
            <span id="oscilloscope-readout-channel">—</span>
            <span id="oscilloscope-readout-timebase">1.0 ms/div</span>
          </div>
        </div>
        <div class="oscilloscope-controls">
          <label>
            Forme d'onde
            <select id="oscilloscope-waveform" class="io-select"></select>
          </label>
          <label>
            Amplitude (div)
            <input id="oscilloscope-amplitude" type="range" min="0.2" max="1.4" step="0.1" value="1">
          </label>
          <label>
            Décalage (div)
            <input id="oscilloscope-offset" type="range" min="-0.6" max="0.6" step="0.1" value="0">
          </label>
          <label>
            Intensité
            <input id="oscilloscope-intensity" type="range" min="0.3" max="1" step="0.1" value="0.8">
          </label>
        </div>
      </div>
    </div>
  `;

  const canvas = container.querySelector('canvas');
  const waveformSelect = container.querySelector('#oscilloscope-waveform');
  const amplitudeRange = container.querySelector('#oscilloscope-amplitude');
  const offsetRange = container.querySelector('#oscilloscope-offset');
  const intensityRange = container.querySelector('#oscilloscope-intensity');
  const timebaseLabel = container.querySelector('#oscilloscope-readout-timebase');
  const channelLabel = container.querySelector('#oscilloscope-readout-channel');
  const channelSelect = container.querySelector('#oscilloscope-channel');
  const statusLabel = container.querySelector('#oscilloscope-status');

  populateSelect(channelSelect, inputs, 'Aucune entrée active');
  const ioMap = new Map(inputs.map((item) => [item.name, item]));
  if (inputs.length) {
    channelSelect.value = inputs[0].name;
    statusLabel.textContent = `${inputs.length} voie(s) disponibles`;
  } else if (error) {
    statusLabel.textContent = 'Impossible de charger les entrées actives.';
  } else {
    statusLabel.textContent = 'Activez une entrée analogique dans la configuration.';
  }

  Object.entries(WAVEFORMS).forEach(([value, { label }]) => {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = label;
    waveformSelect.appendChild(option);
  });
  waveformSelect.value = 'sinus';

  let timebase = 1;
  let direction = -0.1;

  const updateTimebase = () => {
    timebase += direction;
    if (timebase <= 0.2 || timebase >= 5) {
      direction *= -1;
    }
    timebaseLabel.textContent = formatTimebase(timebase);
  };

  const updateChannelLabel = () => {
    if (!inputs.length || !channelSelect.value) {
      channelLabel.textContent = 'Aucune entrée assignée';
      return;
    }
    const selected = ioMap.get(channelSelect.value);
    const unit = selected && selected.unit ? selected.unit : 'unités';
    channelLabel.textContent = `${channelSelect.value} • ${parseFloat(amplitudeRange.value).toFixed(1)} div • ${unit}`;
  };

  const draw = () => {
    const currentWaveform = WAVEFORMS[waveformSelect.value] || WAVEFORMS.sinus;
    drawWaveform(canvas, {
      waveform: currentWaveform.generator,
      amplitude: parseFloat(amplitudeRange.value),
      offset: parseFloat(offsetRange.value),
      intensity: parseFloat(intensityRange.value)
    });
    updateChannelLabel();
  };

  draw();

  const redraw = () => {
    draw();
  };

  waveformSelect.addEventListener('change', redraw);
  amplitudeRange.addEventListener('input', redraw);
  offsetRange.addEventListener('input', redraw);
  intensityRange.addEventListener('input', redraw);
  channelSelect.addEventListener('change', updateChannelLabel);

  setInterval(() => {
    updateTimebase();
    draw();
  }, 1200);
}
