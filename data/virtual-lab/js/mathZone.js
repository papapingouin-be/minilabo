const EXAMPLES = [
  'FFT(200 MSa/s, 1024 points)',
  "Puissance moyenne d'une sinusoïde",
  'Conversion Vpp -> Vrms'
];

function renderExamples(list) {
  return list.map((item) => `<li>${item}</li>`).join('');
}

function safeEval(expression) {
  // Autorise seulement des caractères mathématiques de base
  const sanitized = expression.replace(/[^0-9+\-*/().,^%\sA-Za-z]/g, '');
  if (!sanitized.trim()) {
    throw new Error('Expression vide');
  }
  // eslint-disable-next-line no-new-func
  const evaluator = Function('Math', `"use strict"; return (${sanitized});`);
  return evaluator(Math);
}

export function mountMathZone(container) {
  if (!container) return;
  container.innerHTML = `
    <div class="device-shell math-shell">
      <div class="device-header">
        <div class="device-branding">
          <span class="device-brand">MiniLabBox</span>
          <span class="device-model" id="math-zone-title">Console mathématique</span>
          <span class="device-subtitle">Calculs temps réel</span>
        </div>
        <div class="device-toolbar">
          <a class="device-config-button" href="settings.html#math" aria-label="Configurer la console mathématique">
            ⚙️ Configurer
          </a>
        </div>
      </div>
      <div class="math-console">
        <div>
          <h3>Calculs rapides</h3>
          <p>
            Évaluez instantanément vos formules : rapports de division, conversions d'unités ou calculs de puissance.
            La console est isolée pour simplifier la maintenance du code.
          </p>
        </div>
        <div class="math-input">
          <input type="text" id="math-expression" placeholder="Exemple : (5.0/2) * Math.PI" aria-label="Expression mathématique">
          <button type="button" id="math-run">Calculer</button>
        </div>
        <div class="math-result" id="math-result" aria-live="polite"></div>
        <div>
          <h4>Idées de calculs</h4>
          <ul>${renderExamples(EXAMPLES)}</ul>
        </div>
      </div>
    </div>
  `;

  const input = container.querySelector('#math-expression');
  const result = container.querySelector('#math-result');
  const runButton = container.querySelector('#math-run');

  const evaluate = () => {
    const expression = input.value;
    if (!expression.trim()) {
      result.textContent = 'Saisissez une expression à évaluer.';
      return;
    }
    try {
      const value = safeEval(expression);
      result.textContent = Number.isFinite(value)
        ? `Résultat : ${value}`
        : 'Expression non numérique.';
    } catch (error) {
      result.textContent = `Erreur : ${error.message}`;
    }
  };

  runButton.addEventListener('click', evaluate);
  input.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      evaluate();
    }
  });
}
