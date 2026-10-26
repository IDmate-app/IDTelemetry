// ── i18n — Internationalisierung ──────────────────────────
// Lädt /lang/{lang}.json und ersetzt Texte via data-i18n Attribute.
// Globale Funktion t('key') für JS-Strings.

var _i18n = {};
var _i18nReady = false;
var _i18nCallbacks = [];

function t(key, args) {
  var s = _i18n[key] || key;
  if (args) {
    if (Array.isArray(args)) {
      for (var i = 0; i < args.length; i++) s = s.replace('{' + i + '}', args[i]);
    } else {
      s = s.replace('{0}', args);
    }
  }
  return s;
}

function _i18nApply() {
  document.querySelectorAll('[data-i18n]').forEach(function(el) {
    var key = el.getAttribute('data-i18n');
    if (!_i18n[key]) return;
    if (el.children.length > 0) {
      // Element hat Kind-Elemente (Buttons, Links etc.) — nur ersten Text-Node ersetzen
      for (var i = 0; i < el.childNodes.length; i++) {
        if (el.childNodes[i].nodeType === 3 && el.childNodes[i].textContent.trim()) {
          el.childNodes[i].textContent = _i18n[key] + ' ';
          return;
        }
      }
      el.insertBefore(document.createTextNode(_i18n[key] + ' '), el.firstChild);
    } else {
      el.textContent = _i18n[key];
    }
  });
  document.querySelectorAll('[data-i18n-html]').forEach(function(el) {
    var key = el.getAttribute('data-i18n-html');
    if (_i18n[key]) el.innerHTML = _i18n[key];
  });
  document.querySelectorAll('[data-i18n-ph]').forEach(function(el) {
    var key = el.getAttribute('data-i18n-ph');
    if (_i18n[key]) el.placeholder = _i18n[key];
  });
}

function onI18nReady(fn) {
  if (_i18nReady) fn();
  else _i18nCallbacks.push(fn);
}

(function() {
  // Sprache aus /api/config holen, dann JSON laden
  fetch('/api/config').then(function(r) { return r.json(); }).then(function(cfg) {
    var lang = cfg.lang || 'de';
    document.documentElement.lang = lang;
    return fetch('/lang/' + lang + '.json');
  }).then(function(r) { return r.json(); }).then(function(data) {
    _i18n = data;
    _i18nReady = true;
    _i18nApply();
    _i18nCallbacks.forEach(function(fn) { fn(); });
    _i18nCallbacks = [];
  }).catch(function() {
    // Fallback: Deutsch-Texte bleiben stehen (HTML hat DE als Default)
    _i18nReady = true;
    _i18nCallbacks.forEach(function(fn) { fn(); });
    _i18nCallbacks = [];
  });
})();
