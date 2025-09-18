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
  return `${value.toFixed(1)} ms/div`;
}

function drawWaveform(canvas, options) {
  const ctx = canvas.getContext('2d');
  const { waveform, amplitude, offset } = options;
  const { width, height } = canvas;

  ctx.clearRect(0, 0, width, height);

  ctx.strokeStyle = 'rgba(148, 163, 184, 0.3)';
  ctx.lineWidth = 1;
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

  ctx.strokeStyle = '#45d1ff';
  ctx.lineWidth = 2;
  ctx.beginPath();
  const samples = 600;
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

export function mountOscilloscope(container) {
  if (!container) return;
  container.innerHTML = `
    <span class="status-badge">Tektronix Série 3</span>
    <h3>Oscilloscope numérique 4 voies</h3>
    <p>
      Visualisez vos signaux avec une précision professionnelle. Le rendu ci-dessous simule la persistance
      et la grille d'un oscilloscope Tektronix moderne.
    </p>
    <div class="instrument-display" aria-live="polite">
      <canvas class="oscilloscope-display" width="600" height="220" role="img"
        aria-label="Affichage de la forme d'onde"></canvas>
    </div>
    <div class="instrument-controls">
      <label>
        Forme d'onde
        <select id="oscilloscope-waveform"></select>
      </label>
      <label>
        Amplitude
        <input id="oscilloscope-amplitude" type="range" min="0.2" max="1.4" step="0.1" value="1">
      </label>
      <label>
        Décalage
        <input id="oscilloscope-offset" type="range" min="-0.6" max="0.6" step="0.1" value="0">
      </label>
      <label>
        Base de temps
        <span id="oscilloscope-timebase" aria-live="polite">1.0 ms/div</span>
      </label>
    </div>
  `;

  const canvas = container.querySelector('canvas');
  const waveformSelect = container.querySelector('#oscilloscope-waveform');
  const amplitudeRange = container.querySelector('#oscilloscope-amplitude');
  const offsetRange = container.querySelector('#oscilloscope-offset');
  const timebaseLabel = container.querySelector('#oscilloscope-timebase');

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

  const draw = () => {
    const currentWaveform = WAVEFORMS[waveformSelect.value] || WAVEFORMS.sinus;
    drawWaveform(canvas, {
      waveform: currentWaveform.generator,
      amplitude: parseFloat(amplitudeRange.value),
      offset: parseFloat(offsetRange.value)
    });
  };

  draw();
  const redraw = () => {
    draw();
  };

  waveformSelect.addEventListener('change', redraw);
  amplitudeRange.addEventListener('input', redraw);
  offsetRange.addEventListener('input', redraw);

  setInterval(() => {
    updateTimebase();
    draw();
  }, 1200);
}
