const MEASUREMENTS = {
  voltage: { label: 'Tension DC', unit: 'V', range: [3.28, 3.34] },
  voltageAc: { label: 'Tension AC', unit: 'V RMS', range: [229.2, 231.5] },
  current: { label: 'Courant', unit: 'mA', range: [12.3, 12.9] },
  resistance: { label: 'Résistance', unit: 'kΩ', range: [1.48, 1.53] }
};

function randomInRange([min, max]) {
  return Math.random() * (max - min) + min;
}

function formatValue(value) {
  return value.toFixed(3).replace('.', ',');
}

export function mountMultimeter(container) {
  if (!container) return;
  container.innerHTML = `
    <span class="status-badge">DMM de laboratoire</span>
    <h3>Multimètre 6½ digits</h3>
    <p>
      Mesurez avec stabilité et précision. La simulation reproduit une lecture typique d'un multimètre
      de paillasse haut de gamme avec filtrage numérique.
    </p>
    <div class="instrument-display" role="img" aria-label="Affichage de mesure">
      <div class="measurement-value" id="multimeter-value">0,000</div>
      <div class="measurement-unit" id="multimeter-unit">V</div>
    </div>
    <div class="instrument-controls">
      <label>
        Mode
        <select id="multimeter-mode"></select>
      </label>
      <label>
        Filtre numérique
        <input type="range" id="multimeter-filter" min="0" max="100" value="40">
      </label>
      <label>
        Barregraph
        <progress id="multimeter-bar" max="100" value="30" aria-label="Barregraph numérique"></progress>
      </label>
    </div>
  `;

  const modeSelect = container.querySelector('#multimeter-mode');
  const valueDisplay = container.querySelector('#multimeter-value');
  const unitDisplay = container.querySelector('#multimeter-unit');
  const filterRange = container.querySelector('#multimeter-filter');
  const bar = container.querySelector('#multimeter-bar');

  Object.entries(MEASUREMENTS).forEach(([key, { label }]) => {
    const option = document.createElement('option');
    option.value = key;
    option.textContent = label;
    modeSelect.appendChild(option);
  });

  let previousValue = null;
  const getSmoothedValue = (raw, smoothing) => {
    if (previousValue === null) {
      previousValue = raw;
      return raw;
    }
    const alpha = Math.max(0.02, 1 - smoothing);
    previousValue = previousValue + (raw - previousValue) * alpha;
    return previousValue;
  };

  const updateDisplay = () => {
    const { range, unit } = MEASUREMENTS[modeSelect.value] || MEASUREMENTS.voltage;
    const rawValue = randomInRange(range);
    const smoothing = parseInt(filterRange.value, 10) / 100;
    const value = getSmoothedValue(rawValue, smoothing);
    valueDisplay.textContent = formatValue(value);
    unitDisplay.textContent = unit;
    bar.value = Math.min(100, Math.max(0, ((value - range[0]) / (range[1] - range[0])) * 100));
  };

  modeSelect.addEventListener('change', () => {
    previousValue = null;
    updateDisplay();
  });

  filterRange.addEventListener('input', updateDisplay);

  modeSelect.value = 'voltage';
  updateDisplay();
  setInterval(updateDisplay, 1100);
}
