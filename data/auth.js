(function() {
  const TOKEN_STORAGE_KEY = 'mlbSessionToken';
  let sessionCheckPromise = null;
  let loginPromise = null;
  let sessionToken = null;

  function loadStoredToken() {
    try {
      const stored = window.localStorage.getItem(TOKEN_STORAGE_KEY);
      if (stored && stored.length > 0) {
        sessionToken = stored;
      }
    } catch (err) {
      sessionToken = null;
    }
  }

  function storeSessionToken(token) {
    sessionToken = token || null;
    try {
      if (sessionToken) {
        window.localStorage.setItem(TOKEN_STORAGE_KEY, sessionToken);
      } else {
        window.localStorage.removeItem(TOKEN_STORAGE_KEY);
      }
    } catch (err) {
      // Ignore storage failures (private mode, etc.)
    }
  }

  function clearSessionToken() {
    storeSessionToken(null);
  }

  function prepareOptions(options) {
    const opts = Object.assign({}, options || {});
    if (!opts.credentials) {
      opts.credentials = 'include';
    }
    const headers = new Headers(opts.headers || {});
    if (sessionToken) {
      headers.set('Authorization', 'Bearer ' + sessionToken);
      headers.set('X-Session-Token', sessionToken);
    }
    opts.headers = headers;
    return opts;
  }

  async function rawAuthFetch(url, options) {
    return fetch(url, prepareOptions(options));
  }

  async function loginWithPin(pin) {
    const response = await fetch('/api/session/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      credentials: 'include',
      body: JSON.stringify({ pin })
    });
    if (!response.ok) {
      clearSessionToken();
      return response;
    }
    try {
      const data = await response.clone().json();
      if (data && data.token) {
        storeSessionToken(data.token);
      }
    } catch (err) {
      // Ignore JSON parsing issues – cookie may still work.
    }
    return response;
  }

  loadStoredToken();

  async function promptForPin() {
    if (loginPromise) {
      return loginPromise;
    }
    loginPromise = (async () => {
      clearSessionToken();
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
        const response = await rawAuthFetch('/api/session/status');
        if (response.ok) {
          return true;
        }
        if (response.status === 401) {
          clearSessionToken();
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
    let response;
    try {
      response = await rawAuthFetch(url, options);
    } catch (err) {
      throw err;
    }
    if (response.status !== 401) {
      return response;
    }
    clearSessionToken();
    await promptForPin();
    response = await rawAuthFetch(url, options);
    if (response.status === 401) {
      throw new Error('Authentification requise');
    }
    return response;
  }

  window.ensureSession = ensureSession;
  window.authFetch = authFetch;
})();
