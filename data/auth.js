(function() {
  let sessionCheckPromise = null;
  let loginPromise = null;

  async function loginWithPin(pin) {
    const response = await fetch('/api/session/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      credentials: 'include',
      body: JSON.stringify({ pin })
    });
    if (!response.ok) {
      return response;
    }
    return response;
  }

  async function promptForPin() {
    if (loginPromise) {
      return loginPromise;
    }
    loginPromise = (async () => {
      while (true) {
        const input = window.prompt('Code PIN affiché sur l\'OLED (4 chiffres) :');
        if (input === null) {
          throw new Error('Authentification annulée');
        }
        const pin = input.trim();
        if (!/^\d{4}$/.test(pin)) {
          alert('Le code PIN doit contenir exactement 4 chiffres.');
          continue;
        }
        let response;
        try {
          response = await loginWithPin(pin);
        } catch (err) {
          alert('Erreur réseau pendant l\'authentification.');
          continue;
        }
        if (response.ok) {
          return true;
        }
        if (response.status === 401) {
          alert('Code PIN incorrect.');
          continue;
        }
        const text = await response.text();
        alert(text || 'Impossible d\'ouvrir la session.');
      }
    })();
    try {
      return await loginPromise;
    } finally {
      loginPromise = null;
    }
  }

  async function ensureSession() {
    if (sessionCheckPromise) {
      return sessionCheckPromise;
    }
    sessionCheckPromise = (async () => {
      try {
        const response = await fetch('/api/session/status', { credentials: 'include' });
        if (response.ok) {
          return true;
        }
      } catch (err) {
        // Ignore network errors and fall back to prompting.
      }
      await promptForPin();
      return true;
    })();
    try {
      return await sessionCheckPromise;
    } finally {
      sessionCheckPromise = null;
    }
  }

  async function authFetch(url, options = {}) {
    const opts = Object.assign({}, options || {});
    if (!opts.credentials) {
      opts.credentials = 'include';
    }
    let response;
    try {
      response = await fetch(url, opts);
    } catch (err) {
      throw err;
    }
    if (response.status !== 401) {
      return response;
    }
    await promptForPin();
    response = await fetch(url, opts);
    if (response.status === 401) {
      throw new Error('Authentification requise');
    }
    return response;
  }

  window.ensureSession = ensureSession;
  window.authFetch = authFetch;
})();
