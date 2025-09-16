(function(global) {
  'use strict';

  if (global.ace && typeof global.ace.edit === 'function') {
    return;
  }

  function AceSession(textarea) {
    this._textarea = textarea;
    this._mode = '';
    this._listeners = { change: [] };
    var self = this;
    textarea.addEventListener('input', function() {
      self._emitChange();
    });
  }

  AceSession.prototype.setMode = function(mode) {
    this._mode = mode || '';
  };

  AceSession.prototype.getMode = function() {
    return this._mode;
  };

  AceSession.prototype.getUndoManager = function() {
    return {
      reset: function() {
        /* no-op for stub */
      }
    };
  };

  AceSession.prototype.on = function(event, handler) {
    if (!event || typeof handler !== 'function') return;
    if (!this._listeners[event]) {
      this._listeners[event] = [];
    }
    this._listeners[event].push(handler);
  };

  AceSession.prototype.off = function(event, handler) {
    var handlers = this._listeners[event];
    if (!handlers) return;
    if (!handler) {
      this._listeners[event] = [];
      return;
    }
    this._listeners[event] = handlers.filter(function(cb) { return cb !== handler; });
  };

  AceSession.prototype._emitChange = function() {
    var handlers = this._listeners.change || [];
    handlers.forEach(function(cb) {
      try {
        cb();
      } catch (err) {
        if (global.console && typeof global.console.error === 'function') {
          global.console.error(err);
        }
      }
    });
  };

  function AceEditor(container) {
    if (!container) {
      throw new Error('Ace stub: container introuvable.');
    }
    if (container.firstChild) {
      container.innerHTML = '';
    }
    if (!container.style.position) {
      container.style.position = 'relative';
    }
    container.style.display = 'flex';
    container.style.flexDirection = 'column';

    var textarea = global.document.createElement('textarea');
    textarea.className = 'ace-stub-textarea';
    textarea.style.resize = 'none';
    textarea.style.width = '100%';
    textarea.style.height = '100%';
    textarea.style.flex = '1 1 auto';
    textarea.style.border = 'none';
    textarea.style.outline = 'none';
    textarea.style.padding = '12px';
    textarea.style.fontFamily = 'Menlo, Monaco, Consolas, "Courier New", monospace';
    textarea.style.fontSize = '14px';
    textarea.style.boxSizing = 'border-box';
    textarea.spellcheck = false;

    container.appendChild(textarea);

    this._container = container;
    this._textarea = textarea;
    this.session = new AceSession(textarea);
    this.commands = {
      addCommand: function() {
        /* no-op in stub */
      }
    };
  }

  AceEditor.prototype.setTheme = function() {
    /* theme not supported in stub */
  };

  AceEditor.prototype.setShowPrintMargin = function() {
    /* print margin not supported */
  };

  AceEditor.prototype.setValue = function(value, cursorPos) {
    var text = value == null ? '' : String(value);
    this._textarea.value = text;
    if (cursorPos === -1) {
      var len = this._textarea.value.length;
      this._textarea.selectionStart = len;
      this._textarea.selectionEnd = len;
    } else {
      this._textarea.selectionStart = 0;
      this._textarea.selectionEnd = 0;
    }
    this.session._emitChange();
  };

  AceEditor.prototype.getValue = function() {
    return this._textarea.value;
  };

  AceEditor.prototype.focus = function() {
    this._textarea.focus();
  };

  AceEditor.prototype.resize = function() {
    /* textarea adapts automatically */
  };

  var aceStub = {
    edit: function(el) {
      var element = typeof el === 'string' ? global.document.getElementById(el) : el;
      if (!element) {
        throw new Error('Ace stub: élément "' + el + '" introuvable.');
      }
      return new AceEditor(element);
    },
    config: {
      set: function() {},
      get: function() {},
      setModuleUrl: function() {},
      setModuleLoader: function() {},
      loadModule: function(name, callback) {
        if (typeof callback === 'function') {
          callback();
        }
      }
    },
    require: function() {
      throw new Error('Ace stub: modules non disponibles.');
    },
    version: '1.4.14-stub'
  };

  Object.defineProperty(aceStub, 'config', { writable: false, value: aceStub.config });

  global.ace = aceStub;
})(typeof window !== 'undefined' ? window : this);

