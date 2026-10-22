/* Compás Remote web app. Plain ES2017, no dependencies: the player serves this
 * page itself and the browser may have no Internet access. Edit here, then run
 * scripts/generate_remote_webapp.py (the Makefile does it automatically). */
'use strict';

/* ---------- Icons ---------- */
var ICONS = {
  home: '<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="2.5"/>',
  disc: '<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="2.5"/>',
  queue: '<path d="M4 6h12M4 11h12M4 16h7"/><path d="M16 14.5v5l4-2.5z" fill="currentColor"/>',
  music: '<path d="M9 18V6l11-2v12"/><circle cx="6" cy="18" r="3"/><circle cx="17" cy="16" r="3"/>',
  artist: '<circle cx="12" cy="8" r="4"/><path d="M4.5 20a7.5 7.5 0 0 1 15 0"/>',
  artists: '<circle cx="9" cy="8.5" r="3.5"/><path d="M2.5 19.5a6.5 6.5 0 0 1 13 0"/><path d="M16 5a3.5 3.5 0 0 1 0 7M18.5 14.5a6.5 6.5 0 0 1 3 5"/>',
  album: '<rect x="3.5" y="3.5" width="17" height="17" rx="3"/><circle cx="12" cy="12" r="4"/><circle cx="12" cy="12" r="1"/>',
  playlist: '<path d="M4 6h11M4 11h11M4 16h6"/><circle cx="17.5" cy="17" r="2.5"/><path d="M20 17V8l2-1"/>',
  upload: '<path d="M12 16V4M7 9l5-5 5 5"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/>',
  folder: '<path d="M3.5 7a2 2 0 0 1 2-2h4l2 2.5h7a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/>',
  folderPlus: '<path d="M3.5 7a2 2 0 0 1 2-2h4l2 2.5h7a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/><path d="M12 11v5M9.5 13.5h5"/>',
  folderUp: '<path d="M3.5 7a2 2 0 0 1 2-2h4l2 2.5h7a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/><path d="M12 16.5v-5M9.5 14l2.5-2.5 2.5 2.5"/>',
  file: '<path d="M6 3.5h8l4.5 4.5v12.5h-12.5z"/><path d="M14 3.5V8h4.5"/>',
  fileAudio: '<path d="M6 3.5h8l4.5 4.5v12.5h-12.5z"/><path d="M14 3.5V8h4.5"/><circle cx="10.5" cy="16" r="1.8"/><path d="M12.3 16v-5l2.5 1"/>',
  fileImage: '<rect x="4" y="4" width="16" height="16" rx="2.5"/><circle cx="9" cy="9.5" r="1.6"/><path d="m20 15-4.5-4.5L6 20"/>',
  sd: '<path d="M8 3.5h8.5a2 2 0 0 1 2 2v13a2 2 0 0 1-2 2h-11a2 2 0 0 1-2-2V8.5z"/><path d="M9 7v3M12 7v3M15 7v3"/>',
  play: '<path d="M7 4.5v15l13-7.5z"/>',
  pause: '<path d="M6.5 4.5h4v15h-4zM13.5 4.5h4v15h-4z"/>',
  prev: '<path d="M6 5v14"/><path d="M19 5v14l-10-7z" fill="currentColor"/>',
  next: '<path d="M18 5v14"/><path d="M5 5v14l10-7z" fill="currentColor"/>',
  modeSeq: '<path d="M4 7h13M4 12h13M4 17h8"/><path d="m17 15 3 2-3 2"/>',
  modeRepeat: '<path d="M17 3.5 20 6.5l-3 3"/><path d="M4 11.5v-1a4 4 0 0 1 4-4h12"/><path d="m7 20.5-3-3 3-3"/><path d="M20 12.5v1a4 4 0 0 1-4 4H4"/>',
  modeRepeatOne: '<path d="M17 3.5 20 6.5l-3 3"/><path d="M4 11.5v-1a4 4 0 0 1 4-4h12"/><path d="m7 20.5-3-3 3-3"/><path d="M20 12.5v1a4 4 0 0 1-4 4H4"/><path d="M11.5 10.5 12.5 10v4.5"/>',
  modeShuffle: '<path d="M4 7h3.5c2 0 3.2 1 4.3 2.7l1.4 2.6c1.1 1.7 2.3 2.7 4.3 2.7H20"/><path d="m17 12 3 3-3 3"/><path d="M4 17h3.5c1.2 0 2.1-.4 2.9-1.1"/><path d="M14.1 8.1c.8-.7 1.7-1.1 2.9-1.1H20"/><path d="m17 4 3 3-3 3"/>',
  heart: '<path d="M12 20s-7.5-4.6-7.5-10.2A4.3 4.3 0 0 1 12 7.2a4.3 4.3 0 0 1 7.5 2.6C19.5 15.4 12 20 12 20z"/>',
  volume: '<path d="M4 9.5h3.5L12 5.5v13l-4.5-4H4z"/><path d="M15.5 9a4 4 0 0 1 0 6M18 6.5a7.5 7.5 0 0 1 0 11"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  more: '<circle cx="5.5" cy="12" r="1.2" fill="currentColor"/><circle cx="12" cy="12" r="1.2" fill="currentColor"/><circle cx="18.5" cy="12" r="1.2" fill="currentColor"/>',
  search: '<circle cx="11" cy="11" r="6.5"/><path d="m20 20-4.2-4.2"/>',
  chevronLeft: '<path d="m14.5 6-6 6 6 6"/>',
  chevronRight: '<path d="m9.5 6 6 6-6 6"/>',
  rename: '<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="m13.5 6.5 4 4"/>',
  trash: '<path d="M4.5 7h15M10 7V4.5h4V7M6.5 7l1 13h9l1-13"/>',
  refresh: '<path d="M19.5 12a7.5 7.5 0 1 1-2.2-5.3"/><path d="M19.5 4.5v4h-4"/>',
  check: '<path d="m5 12.5 4.5 4.5L19 7.5"/>',
  checkCircle: '<circle cx="12" cy="12" r="8.5"/><path d="m8.5 12.2 2.4 2.4 4.7-4.9"/>',
  alert: '<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5v5.5M12 16.5h.01"/>',
  clock: '<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/>',
  skip: '<circle cx="12" cy="12" r="8.5"/><path d="M8 12h8"/>',
  lock: '<rect x="5" y="10.5" width="14" height="10" rx="2.5"/><path d="M8 10.5V8a4 4 0 0 1 8 0v2.5"/>',
  theme: '<path d="M12 3.5a8.5 8.5 0 1 0 8.5 8.5A6.5 6.5 0 0 1 12 3.5z"/>',
  x: '<path d="M6 6l12 12M18 6 6 18"/>',
  wifiOff: '<path d="M2.5 9a14 14 0 0 1 19 0M6 12.5a9 9 0 0 1 12 0M9.5 16a4 4 0 0 1 5 0M12 19.5h.01"/>'
};
var FILLED = { play: 1, pause: 1 };
/* The "compás" wordmark from the boot logo (URW Gothic Book outlines). */
var WORDMARK = '<svg viewBox="42 -782 3924 974" aria-hidden="true"><path d="M524 -161C467 -85 415 -55 336 -55C214 -55 117 -151 117 -273C117 -396 212 -492 334 -492C415 -492 482 -453 524 -381H607C563 -491 458 -560 335 -560C171 -560 42 -434 42 -275C42 -112 172 13 340 13C408 13 471 -8 522 -49C557 -77 578 -104 608 -161ZM976 -560C813 -560 689 -437 689 -277C689 -108 812 13 982 13C1142 13 1260 -108 1260 -272C1260 -439 1141 -560 976 -560ZM976 -492C1097 -492 1185 -399 1185 -271C1185 -145 1099 -55 980 -55C854 -55 764 -146 764 -275C764 -399 855 -492 976 -492ZM1365 0H1439V-319C1439 -428 1494 -492 1586 -492C1631 -492 1673 -474 1700 -442C1724 -414 1734 -376 1734 -319V0H1808V-317C1808 -370 1816 -403 1834 -430C1860 -469 1906 -492 1958 -492C2006 -492 2048 -473 2073 -440C2093 -413 2103 -372 2103 -317V0H2177V-325C2177 -397 2164 -443 2133 -483C2095 -532 2033 -560 1962 -560C1877 -560 1818 -528 1772 -455C1729 -529 1676 -560 1593 -560C1524 -560 1482 -539 1439 -484V-547H1365ZM2303 -547V192H2377V-94C2425 -24 2499 13 2590 13C2754 13 2880 -112 2880 -272C2880 -436 2755 -560 2592 -560C2498 -560 2424 -523 2377 -453V-547ZM2589 -492C2713 -492 2805 -398 2805 -272C2805 -149 2712 -55 2590 -55C2465 -55 2372 -149 2372 -276C2372 -397 2467 -492 2589 -492ZM3542 -547H3468V-450C3413 -526 3347 -560 3252 -560C3088 -560 2964 -436 2964 -272C2964 -108 3089 13 3258 13C3351 13 3409 -17 3468 -96V0H3542ZM3254 -492C3375 -492 3468 -398 3468 -276C3468 -151 3376 -55 3258 -55C3134 -55 3039 -152 3039 -277C3039 -396 3135 -492 3254 -492ZM3193 -666 3227 -610 3420 -724 3385 -782ZM3623 -161C3624 -55 3693 13 3799 13C3898 13 3966 -57 3966 -157C3966 -224 3935 -270 3871 -297C3787 -332 3783 -333 3768 -341C3734 -358 3718 -381 3718 -413C3718 -457 3753 -492 3797 -492C3845 -492 3873 -464 3877 -412H3952C3949 -500 3886 -560 3798 -560C3709 -560 3643 -495 3643 -406C3643 -366 3657 -333 3684 -308C3707 -287 3708 -286 3785 -255C3870 -220 3891 -200 3891 -150C3891 -95 3853 -55 3799 -55C3737 -55 3700 -93 3696 -161Z"/></svg>';

function icon(name) {
  var body = ICONS[name] || '';
  return FILLED[name]
    ? '<svg viewBox="0 0 24 24" style="fill:currentColor;stroke:none" aria-hidden="true">' + body + '</svg>'
    : '<svg viewBox="0 0 24 24" aria-hidden="true">' + body + '</svg>';
}

/* ---------- DOM helpers ---------- */
function $(sel, root) { return (root || document).querySelector(sel); }
function $all(sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); }

/* h('div.class', {attrs, on*: handler}, children...) */
function h(spec, attrs) {
  var parts = spec.split('.');
  var el = document.createElement(parts[0] || 'div');
  if (parts.length > 1) el.className = parts.slice(1).join(' ');
  var kids = Array.prototype.slice.call(arguments, 2);
  if (attrs && (typeof attrs !== 'object' || attrs instanceof Node || Array.isArray(attrs))) { kids.unshift(attrs); attrs = null; }
  if (attrs) Object.keys(attrs).forEach(function (k) {
    var v = attrs[k];
    if (v == null || v === false) return;
    if (k.slice(0, 2) === 'on') el.addEventListener(k.slice(2), v);
    else if (k === 'html') el.innerHTML = v;
    else if (k === 'text') el.textContent = v;
    else if (k === 'style') el.style.cssText = v;
    else el.setAttribute(k, v === true ? '' : v);
  });
  kids.forEach(function append(k) {
    if (k == null || k === false) return;
    if (Array.isArray(k)) k.forEach(append);
    else el.appendChild(k instanceof Node ? k : document.createTextNode(String(k)));
  });
  return el;
}
function iconEl(name, cls) { return h('span' + (cls ? '.' + cls : ''), { html: icon(name) }); }
function hydrateIcons(root) {
  $all('[data-icon]', root).forEach(function (el) { el.innerHTML = icon(el.getAttribute('data-icon')); });
  $all('[data-wordmark]', root).forEach(function (el) { el.innerHTML = WORDMARK; });
}

function fmtTime(s) { s = Math.max(0, s | 0); var m = (s / 60) | 0, r = s % 60; return m + ':' + (r < 10 ? '0' : '') + r; }
function fmtBytes(n) {
  n = Number(n) || 0;
  var units = ['B', 'KB', 'MB', 'GB'], i = 0;
  while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
  return (i ? n.toFixed(n >= 100 ? 0 : 1) : n) + ' ' + units[i];
}
function plural(n, one, many) { return n + ' ' + (n === 1 ? one : many); }
function initials(name) { var w = String(name || '?').trim().split(/\s+/); return ((w[0] || '?')[0] + (w[1] ? w[1][0] : '')).toUpperCase(); }

var toastTimer = null;
function toast(message) {
  var t = $('#toast');
  t.textContent = message;
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function () { t.classList.remove('show'); }, 2200);
}

/* ---------- API and PIN session ---------- */
/* Requests carry the PIN as an HttpOnly cookie set by POST /api/session, so
 * reloading or switching between :80 and :8899 never asks again. */
var pinPrompt = null;

function HttpError(code, body) {
  var data = {};
  try { data = body ? JSON.parse(body) : {}; } catch (e) { data = { error: body }; }
  var err = new Error(data.message || data.error || ('Request failed (' + code + ')'));
  err.status = code;
  err.code = data.code || data.error;
  return err;
}

function api(path, options) {
  options = options || {};
  var init = { method: options.method || 'GET', credentials: 'same-origin', signal: options.signal };
  return fetch(path, init).then(function (r) {
    if (r.status === 401 && !options.noPin) return requirePin().then(function () { return api(path, options); });
    return r.text().then(function (text) {
      if (!r.ok) throw HttpError(r.status, text);
      if (!text) return {};
      try { return JSON.parse(text); } catch (e) { return { text: text }; }
    });
  });
}

function post(path, okMessage) {
  return api(path, { method: 'POST' }).then(function () {
    if (okMessage) toast(okMessage);
    return true;
  }).catch(function (err) {
    if (err.status === 409 && path.indexOf('/api/queue/') === 0) { toast('Queue changed, refreshed'); refreshQueueViews(); return false; }
    toast(err.message === 'Failed to fetch' ? 'Player unreachable' : 'Request failed');
    return false;
  });
}

function requirePin() {
  if (pinPrompt) return pinPrompt;
  pinPrompt = new Promise(function (resolve) {
    var input = h('input.field.pin-input', { type: 'password', inputmode: 'numeric', autocomplete: 'one-time-code', maxlength: '12', 'aria-label': 'Player PIN' });
    var error = h('div.error');
    var submit = h('button.btn.primary', { type: 'submit' }, 'Connect');
    var form = h('form.dialog', { onsubmit: function (e) {
      e.preventDefault();
      var pin = input.value.trim();
      if (!/^\d{4,12}$/.test(pin)) { error.textContent = 'Enter the 4–12 digit PIN.'; return; }
      submit.disabled = true;
      error.textContent = '';
      fetch('/api/session', { method: 'POST', credentials: 'same-origin', headers: { 'X-Compas-PIN': pin } }).then(function (r) {
        submit.disabled = false;
        if (r.ok) { scrim.remove(); pinPrompt = null; setConnection('ok'); resolve(); return; }
        if (r.status === 429) error.textContent = 'Too many attempts. Wait a minute and try again.';
        else error.textContent = 'That PIN is not correct.';
        input.select();
      }).catch(function () { submit.disabled = false; error.textContent = 'Could not reach the player.'; });
    } },
      h('div.pin-mark', { html: icon('lock') }),
      h('h2', 'Connect to your player'),
      h('p', 'Enter the PIN shown on the player under Wireless › Remote. This browser will remember it.'),
      input, error,
      h('div.buttons', submit));
    var scrim = h('div.scrim', { role: 'dialog', 'aria-modal': 'true', 'aria-label': 'Enter player PIN' }, form);
    document.body.appendChild(scrim);
    setConnection('pin');
    setTimeout(function () { input.focus(); }, 50);
  });
  return pinPrompt;
}

/* ---------- Dialogs and menus ---------- */
function dialog(opts) {
  return new Promise(function (resolve) {
    var input = opts.input != null ? h('input.field', { value: opts.input, 'aria-label': opts.title, spellcheck: 'false' }) : null;
    var select = opts.options ? h('select.field', { 'aria-label': opts.title }, opts.options.map(function (o) { return h('option', { value: o.value }, o.label); })) : null;
    function close(value) { scrim.remove(); document.removeEventListener('keydown', onKey); resolve(value); }
    function onKey(e) { if (e.key === 'Escape') close(null); }
    var form = h('form.dialog', { onsubmit: function (e) {
      e.preventDefault();
      if (input) { var v = input.value.trim(); if (!v) return; close(v); }
      else if (select) close(select.value);
      else close(true);
    } },
      h('h2', opts.title),
      opts.text ? h('p', opts.text) : null,
      input, select,
      h('div.buttons',
        h('button.btn.ghost', { type: 'button', onclick: function () { close(null); } }, 'Cancel'),
        h('button.btn.' + (opts.danger ? 'danger' : 'primary'), { type: 'submit' }, opts.confirm || 'OK')));
    var scrim = h('div.scrim', { role: 'dialog', 'aria-modal': 'true', onclick: function (e) { if (e.target === scrim) close(null); } }, form);
    document.body.appendChild(scrim);
    document.addEventListener('keydown', onKey);
    setTimeout(function () {
      if (input) { input.focus(); var dot = input.value.lastIndexOf('.'); input.setSelectionRange(0, dot > 0 ? dot : input.value.length); }
      else (select || $('.btn.primary, .btn.danger', form)).focus();
    }, 30);
  });
}

var openMenuEl = null;
function closeMenu() { if (openMenuEl) { openMenuEl.remove(); openMenuEl = null; } }
function menu(anchor, items) {
  closeMenu();
  var el = h('div.menu', { role: 'menu' }, items.map(function (it) {
    return h('button' + (it.danger ? '.danger' : ''), { role: 'menuitem', onclick: function (e) { e.stopPropagation(); closeMenu(); it.run(); } }, iconEl(it.icon), it.label);
  }));
  document.body.appendChild(el);
  var r = anchor.getBoundingClientRect(), w = el.offsetWidth, hgt = el.offsetHeight;
  el.style.left = Math.max(8, Math.min(window.innerWidth - w - 8, r.right - w)) + 'px';
  el.style.top = (r.bottom + hgt + 8 > window.innerHeight ? r.top - hgt - 6 : r.bottom + 6) + 'px';
  openMenuEl = el;
  el.firstChild && el.firstChild.focus();
}
document.addEventListener('click', function (e) { if (openMenuEl && !openMenuEl.contains(e.target)) closeMenu(); });
document.addEventListener('keydown', function (e) { if (e.key === 'Escape') closeMenu(); });

/* ---------- Theme and accent ---------- */
var THEME_KEY = 'compas-remote-theme';
function setTheme(theme) {
  document.body.setAttribute('data-theme', theme);
  $('#themeLabel').textContent = theme === 'dark' ? 'Light mode' : 'Dark mode';
  $('meta[name=theme-color]').setAttribute('content', theme === 'dark' ? '#0a0b0e' : '#f4f4f6');
}
function toggleTheme() {
  var next = document.body.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
  try { localStorage.setItem(THEME_KEY, next); } catch (e) { /* storage may be blocked */ }
  setTheme(next);
}
function applyAccent(hex) {
  if (!/^#[0-9a-f]{6}$/i.test(hex || '')) return;
  var n = parseInt(hex.slice(1), 16), r = n >> 16, g = (n >> 8) & 255, b = n & 255;
  var luminance = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
  document.documentElement.style.setProperty('--accent', hex);
  document.documentElement.style.setProperty('--on-accent', luminance > 0.55 ? '#10140a' : '#ffffff');
}

/* ---------- Status polling and the player bar ---------- */
var player = null, statusFailures = 0, artKey = null, artUrl = null, seekDragging = false, volumeDragging = false;
var MODES = [
  { icon: 'modeRepeat', label: 'In order' },
  { icon: 'modeRepeat', label: 'Repeat all' },
  { icon: 'modeRepeatOne', label: 'Repeat one' },
  { icon: 'modeShuffle', label: 'Shuffle' },
  { icon: 'modeShuffle', label: 'Shuffle once' }
];

function setConnection(state) {
  var dot = $('#connDot'), text = $('#connText');
  dot.className = 'dot' + (state === 'ok' ? ' ok' : state === 'down' ? ' bad' : '');
  text.textContent = state === 'ok' ? 'Connected' : state === 'pin' ? 'PIN required' : state === 'down' ? 'Unreachable' : 'Connecting…';
}

function audioFormat(s) {
  var parts = [];
  if (s.codec) parts.push(String(s.codec).toUpperCase());
  if (s.source_bit_depth) parts.push(s.source_bit_depth + '-bit');
  if (s.source_sample_rate) parts.push((s.source_sample_rate / 1000).toFixed(s.source_sample_rate % 1000 ? 1 : 0) + ' kHz');
  return parts.join(' · ');
}

function poll() {
  if (pinPrompt) return;
  api('/api/status').then(function (s) {
    player = s;
    statusFailures = 0;
    setConnection('ok');
    renderStatus();
  }).catch(function () {
    if (++statusFailures >= 2) setConnection('down');
  });
}

function renderStatus() {
  var s = player;
  if (!s) return;
  var hasTrack = !!s.title;
  if (s.device_model) $('#deviceName').textContent = s.device_model;
  applyAccent(s.accent_color);
  document.title = hasTrack ? s.title + (s.artist ? ' · ' + s.artist : '') + ' — Compás' : 'Compás Remote';

  var key = hasTrack ? s.title + '|' + s.artist + '|' + s.album : '';
  if (key !== artKey) {
    artKey = key;
    artUrl = hasTrack ? '/api/art?t=' + Date.now() : null;
    var probe = new Image();
    probe.onload = function () { if (artKey === key) setArt(artUrl); };
    probe.onerror = function () { if (artKey === key) setArt(null); };
    if (artUrl) probe.src = artUrl; else setArt(null);
  }

  $all('[data-np-title]').concat([$('#npTitle')]).forEach(function (el) { el.textContent = hasTrack ? s.title : 'Not playing'; });
  $('#npArtist').textContent = hasTrack ? [s.artist, s.album].filter(Boolean).join(' — ') : '';
  $all('[data-np-sub]').forEach(function (el) { el.textContent = [s.artist, s.album].filter(Boolean).join(' — '); });
  $all('[data-np-format]').forEach(function (el) { var f = audioFormat(s); el.textContent = f; el.style.display = f ? '' : 'none'; });
  $all('[data-play]').forEach(function (el) { el.innerHTML = icon(s.playing ? 'pause' : 'play'); el.setAttribute('aria-label', s.playing ? 'Pause' : 'Play'); });
  var mode = MODES[s.play_mode | 0] || MODES[0];
  $all('#modeBtn, [data-mode]').forEach(function (el) {
    el.innerHTML = icon(mode.icon);
    el.classList.toggle('on', (s.play_mode | 0) !== 0);
    el.setAttribute('aria-label', 'Playback mode: ' + mode.label);
    el.title = mode.label;
  });
  $all('[data-fav]').forEach(function (el) {
    el.classList.toggle('on', !!s.favorite);
    el.innerHTML = s.favorite ? icon('heart').replace('<svg ', '<svg style="fill:currentColor" ') : icon('heart');
    el.setAttribute('aria-label', s.favorite ? 'Remove from favorites' : 'Add to favorites');
  });
  if (!seekDragging) {
    var pct = s.duration_seconds > 0 ? s.position_seconds / s.duration_seconds : 0;
    $all('[data-seek]').forEach(function (el) { paintTrack(el, pct); el.setAttribute('aria-valuenow', Math.round(pct * 100)); });
    $all('[data-pos]').forEach(function (el) { el.textContent = fmtTime(s.position_seconds); });
    $all('[data-dur]').forEach(function (el) { el.textContent = fmtTime(s.duration_seconds); });
  }
  if (!volumeDragging) paintTrack($('#volume'), Number(s.volume) || 0);
}

function setArt(url) {
  var bg = url ? 'url("' + url + '")' : '';
  $('#backdrop').style.backgroundImage = bg;
  $('#backdrop').classList.toggle('on', !!url);
  $all('#npThumb, [data-np-art]').forEach(function (el) {
    el.style.backgroundImage = bg;
    el.innerHTML = url ? '' : icon('disc');
  });
}

function paintTrack(el, fraction) {
  var pct = Math.max(0, Math.min(1, fraction)) * 100 + '%';
  $('.fill', el).style.width = pct;
  $('.knob', el).style.left = pct;
}

/* Pointer-driven slider; onChange receives the final 0..1 fraction. */
function bindSlider(el, onDrag, onChange, setDragging) {
  function fraction(e) { var r = el.getBoundingClientRect(); return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)); }
  el.addEventListener('pointerdown', function (e) {
    el.setPointerCapture(e.pointerId);
    setDragging(true);
    el.classList.add('drag');
    paintTrack(el, fraction(e));
    onDrag(fraction(e));
  });
  el.addEventListener('pointermove', function (e) { if (el.classList.contains('drag')) { paintTrack(el, fraction(e)); onDrag(fraction(e)); } });
  el.addEventListener('pointerup', function (e) {
    if (!el.classList.contains('drag')) return;
    el.classList.remove('drag');
    onChange(fraction(e));
    setTimeout(function () { setDragging(false); }, 1200);
  });
  el.addEventListener('keydown', function (e) {
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
    e.preventDefault();
    var current = parseFloat($('.fill', el).style.width) / 100 || 0;
    var next = Math.max(0, Math.min(1, current + (e.key === 'ArrowRight' ? 0.05 : -0.05)));
    paintTrack(el, next);
    onChange(next);
  });
}

function bindSeek(el) {
  bindSlider(el, function (f) {
    if (player) $all('[data-pos]').forEach(function (p) { p.textContent = fmtTime(f * player.duration_seconds); });
  }, function (f) {
    if (player && player.duration_seconds > 0) post('/api/playback/seek?seconds=' + Math.round(f * player.duration_seconds));
  }, function (v) { seekDragging = v; });
}

function command(name) {
  return post('/api/playback/' + name).then(function () { setTimeout(poll, 250); });
}

function toggleFavorite() {
  if (!player || !player.title) return;
  post('/api/favorites/' + (player.favorite ? 'remove' : 'add') + '?current=1', player.favorite ? 'Removed from favorites' : 'Added to favorites')
    .then(function () { setTimeout(poll, 250); });
}

/* ---------- Router ---------- */
var routes = {};
var currentRoute = null;
var heroObserver = null;

/* Small screens hide the mini player while the hero's own controls are in view. */
function watchHero(controls) {
  if (heroObserver) heroObserver.disconnect();
  heroObserver = null;
  var app = $('.app');
  app.classList.remove('hero-visible');
  if (!controls || !window.IntersectionObserver) return;
  heroObserver = new IntersectionObserver(function (entries) {
    var small = window.matchMedia('(max-width: 880px)').matches;
    app.classList.toggle('hero-visible', small && entries[entries.length - 1].isIntersecting);
  }, { root: $('#main') });
  heroObserver.observe(controls);
}

function go(route, params) {
  var hash = '#/' + route + (params ? '?' + new URLSearchParams(params).toString() : '');
  if (location.hash === hash) render(); else location.hash = hash;
}
function parseHash() {
  var raw = location.hash.replace(/^#\/?/, ''), q = raw.indexOf('?');
  return { name: (q < 0 ? raw : raw.slice(0, q)) || 'home', params: new URLSearchParams(q < 0 ? '' : raw.slice(q + 1)) };
}
function render() {
  var route = parseHash();
  var view = routes[route.name] || routes.home;
  currentRoute = route.name;
  closeMenu();
  watchHero(null);
  var section = route.name === 'albums' || route.name === 'album' ? (route.params.get('kind') === 'album_artist' ? 'album-artists' : 'artists')
    : route.name === 'playlist' ? 'playlists' : route.name;
  $all('[data-route]').forEach(function (el) { el.classList.toggle('active', el.getAttribute('data-route') === section && !el.classList.contains('np')); });
  var main = $('#main');
  main.textContent = '';
  var container = h('div.view');
  main.appendChild(container);
  main.scrollTop = 0;
  view(container, route.params);
}
window.addEventListener('hashchange', render);

/* back = { label, route, params } of the parent view. */
function viewHead(title, subtitle, extra, back) {
  return h('div.view-head',
    h('div.grow',
      back ? h('button.back', { onclick: function () { go(back.route, back.params); } }, iconEl('chevronLeft'), back.label) : null,
      h('h1', title),
      subtitle ? h('p', subtitle) : null),
    extra);
}
function skeleton(n) { var out = []; for (var i = 0; i < (n || 8); i++) out.push(h('div.skeleton')); return out; }
function emptyState(iconName, title, text) { return h('div.empty', iconEl(iconName), h('b', title), text ? h('span', text) : null); }
function failState(container, err) {
  container.textContent = '';
  container.appendChild(emptyState('wifiOff', 'Could not load this page', err && err.message === 'Failed to fetch' ? 'The player is unreachable. Check its Wi-Fi.' : 'Try again in a moment.'));
}

/* ---------- Rows ---------- */
/* The player serves only a few connections at once (503 beyond that), so
 * artwork loads through a small queue with one retry. */
var artQueue = [], artActive = 0, ART_PARALLEL = 1;
function loadArt(url, onReady) {
  artQueue.push({ url: url, onReady: onReady, tries: 0 });
  pumpArt();
}
function pumpArt() {
  while (artActive < ART_PARALLEL && artQueue.length) {
    var job = artQueue.shift();
    artActive++;
    (function (job) {
      var img = new Image();
      img.onload = function () { artActive--; job.onReady(job.url); pumpArt(); };
      img.onerror = function () {
        artActive--;
        if (++job.tries < 4) setTimeout(function () { artQueue.push(job); pumpArt(); }, 400);
        pumpArt();
      };
      img.src = job.url;
    })(job);
  }
}
function setCover(el, url) { el.style.backgroundImage = 'url("' + url + '")'; el.innerHTML = ''; }
function thumbEl(index, fallback) {
  var t = h('span.thumb', { html: icon(fallback || 'music') });
  if (index != null) loadArt('/api/art?index=' + index, function (url) { setCover(t, url); });
  return t;
}

function songRow(song, number, context, extraActions) {
  var actions = [
    h('button.icon-btn', { 'aria-label': 'Add ' + song.title + ' to queue', title: 'Add to queue', html: icon('queue'),
      onclick: function (e) { e.stopPropagation(); post('/api/playback/queue?index=' + song.index, 'Added to queue').then(refreshQueueViews); } }),
    h('button.icon-btn', { 'aria-label': 'More options for ' + song.title, html: icon('more'), onclick: function (e) {
      e.stopPropagation();
      menu(e.currentTarget, [
        { icon: 'play', label: 'Play now', run: function () { playSong(song, context); } },
        { icon: 'queue', label: 'Add to queue', run: function () { post('/api/playback/queue?index=' + song.index, 'Added to queue').then(refreshQueueViews); } },
        { icon: 'playlist', label: 'Add to playlist…', run: function () { addToPlaylist(song.index); } }
      ].concat(extraActions || []));
    } })
  ];
  var row = h('div.row', { role: 'button', tabindex: '0', onclick: function () { playSong(song, context); },
    onkeydown: function (e) { if (e.key === 'Enter') playSong(song, context); } },
    h('span.num', number != null ? String(number) : ''),
    thumbEl(song.index),
    h('span.meta', h('div.title', song.title || 'Unknown track'), h('div.subtitle', song.artist || '')),
    h('span.actions', actions));
  if (player && player.title === song.title && player.artist === song.artist) row.classList.add('playing');
  return row;
}

function playSong(song, context) {
  post('/api/playback/play?index=' + song.index + (context || '')).then(function () { setTimeout(poll, 300); refreshQueueViews(); });
}

function groupRow(item, onOpen, kind) {
  return h('button.row.compact', { onclick: onOpen },
    kind === 'artist' ? h('span.avatar', initials(item.name)) : thumbEl(item.index, 'playlist'),
    h('span.meta', h('div.title', item.name), item.count != null ? h('div.subtitle', plural(item.count, 'song', 'songs')) : null),
    h('span.count', { html: icon('chevronRight') }));
}

function addToPlaylist(index) {
  api('/api/playlists').then(function (d) {
    var options = (d.playlists || []).filter(function (p) { return p.writable !== false; }).map(function (p) {
      var name = p.name.replace(/\.m3u8?$/i, '');
      return { value: name, label: name };
    });
    options.push({ value: '\u0000new', label: 'New playlist…' });
    return dialog({ title: 'Add to playlist', options: options, confirm: 'Add' });
  }).then(function (choice) {
    if (!choice) return null;
    if (choice !== '\u0000new') return post('/api/playlists/add?name=' + encodeURIComponent(choice) + '&index=' + index, 'Added to ' + choice);
    return dialog({ title: 'New playlist', input: '', confirm: 'Create' }).then(function (name) {
      if (name) return post('/api/playlists?name=' + encodeURIComponent(name) + '&index=' + index, 'Created ' + name);
    });
  }).catch(function () { toast('Could not load playlists'); });
}

/* ---------- Views ---------- */
var queueViewRefresh = null;
function refreshQueueViews() { if (queueViewRefresh) setTimeout(queueViewRefresh, 250); }

routes.home = function (root) {
  var art = h('div.hero-art', { 'data-np-art': '' });
  var seek = h('div.track', { 'data-seek': '', role: 'slider', tabindex: '0', 'aria-label': 'Seek' }, h('div.fill'), h('div.knob'));
  bindSeek(seek);
  root.appendChild(h('section.hero',
    art,
    h('div',
      h('div.eyebrow', 'Now playing'),
      h('h2', { 'data-np-title': '' }, 'Not playing'),
      h('div.sub', { 'data-np-sub': '' }),
      h('div', { style: 'margin-top:14px' }, h('span.chip', { 'data-np-format': '' })),
      h('div.seek.seek-row', h('span', { 'data-pos': '' }, '0:00'), seek, h('span', { 'data-dur': '' }, '0:00')),
      h('div.controls',
        h('button.icon-btn', { 'data-mode': '', onclick: function () { command('mode'); } }),
        h('button.icon-btn', { 'aria-label': 'Previous track', html: icon('prev'), onclick: function () { command('prev'); } }),
        h('button.play-big', { 'data-play': '', onclick: function () { command('toggle'); } }),
        h('button.icon-btn', { 'aria-label': 'Next track', html: icon('next'), onclick: function () { command('next'); } }),
        h('button.icon-btn', { 'data-fav': '', onclick: toggleFavorite })))));

  watchHero($('.hero .controls', root));
  var upNext = h('div.list', skeleton(4));
  root.appendChild(h('section.section',
    h('div.section-head', h('h3', 'Up next'), h('button.link', { onclick: function () { go('queue'); } }, 'Open queue')),
    upNext));
  root.appendChild(h('section.section',
    h('div.section-head', h('h3', 'Library')),
    h('div.shortcuts', [
      ['songs', 'music', 'Songs', 'Search everything'],
      ['artists', 'artist', 'Artists', 'Browse by artist'],
      ['album-artists', 'artists', 'Album artists', 'Browse by album artist'],
      ['playlists', 'playlist', 'Playlists', 'Your saved lists'],
      ['transfer', 'upload', 'Transfer files', 'Send music to the SD card']
    ].map(function (s) {
      return h('button.shortcut', { onclick: function () { go(s[0]); } }, h('i', { html: icon(s[1]) }), h('span', s[2], h('small', s[3])));
    }))));

  function loadUpNext() {
    api('/api/queue?offset=0&limit=6').then(function (d) {
      upNext.textContent = '';
      if (!d.songs || !d.songs.length) { upNext.appendChild(emptyState('queue', 'Nothing queued', 'Play an album or add songs to the queue.')); return; }
      d.songs.forEach(function (s, i) { upNext.appendChild(queueRow(s, i + 1, d.revision)); });
    }).catch(function () { upNext.textContent = ''; upNext.appendChild(emptyState('wifiOff', 'Queue unavailable')); });
  }
  queueViewRefresh = loadUpNext;
  loadUpNext();
  renderStatus();
  setArt(artKey ? artUrl : null);
};

function queueRow(song, number, revision) {
  var row = songRow(song, number, '', [
    { icon: 'trash', label: 'Remove from queue', danger: true, run: function () {
      post('/api/queue/remove?offset=' + song.offset + '&revision=' + revision, 'Removed from queue').then(refreshQueueViews);
    } }
  ]);
  row.onclick = function () { post('/api/queue/play?offset=' + song.offset + '&revision=' + revision).then(function () { setTimeout(poll, 300); refreshQueueViews(); }); };
  return row;
}

routes.queue = function (root, params) {
  var offset = Math.max(0, Number(params.get('offset')) || 0), PAGE = 50;
  var list = h('div.list', skeleton());
  var head = viewHead('Queue', 'Up next on the player');
  root.appendChild(head);
  root.appendChild(list);
  function load() {
    api('/api/queue?offset=' + offset + '&limit=' + PAGE).then(function (d) {
      list.textContent = '';
      head.querySelector('p').textContent = d.total ? plural(d.total, 'track', 'tracks') + ' up next' : 'Up next on the player';
      if (!d.songs || !d.songs.length) { list.appendChild(emptyState('queue', 'The queue is empty', 'Play something or add songs from your library.')); return; }
      d.songs.forEach(function (s, i) { list.appendChild(queueRow(s, offset + i + 1, d.revision)); });
      var pager = h('div.more');
      if (offset > 0) pager.appendChild(h('button.btn.sm', { onclick: function () { go('queue', { offset: Math.max(0, offset - PAGE) }); } }, 'Previous'));
      if (offset + d.songs.length < d.total) pager.appendChild(h('button.btn.sm', { onclick: function () { go('queue', { offset: offset + PAGE }); } }, 'Next'));
      pager.appendChild(h('button.btn.sm.ghost', { onclick: function () {
        dialog({ title: 'Clear the queue?', text: 'This removes every upcoming track.', confirm: 'Clear', danger: true }).then(function (ok) {
          if (ok) post('/api/queue/clear?revision=' + d.revision, 'Queue cleared').then(refreshQueueViews);
        });
      } }, 'Clear queue'));
      list.appendChild(pager);
    }).catch(function (e) { failState(list, e); });
  }
  queueViewRefresh = load;
  load();
};

routes.songs = function (root, params) {
  var query = params.get('q') || '', offset = 0, total = 0, timer = null;
  var input = h('input', { type: 'search', placeholder: 'Search songs, artists, albums', value: query, 'aria-label': 'Search library' });
  var list = h('div.list'), more = h('div.more');
  root.appendChild(viewHead('Songs', 'Everything on the player', h('label.search', iconEl('search'), input)));
  root.appendChild(list);
  root.appendChild(more);
  function load(reset) {
    if (reset) { offset = 0; list.textContent = ''; list.appendChild(h('div', skeleton())); }
    more.textContent = '';
    api('/api/library?offset=' + offset + '&limit=50&q=' + encodeURIComponent(query)).then(function (d) {
      if (reset) list.textContent = '';
      total = d.total || 0;
      (d.songs || []).forEach(function (s, i) { list.appendChild(songRow(s, offset + i + 1, '')); });
      offset += (d.songs || []).length;
      if (reset && !offset) list.appendChild(emptyState('search', query ? 'No matches for “' + query + '”' : 'No songs yet', query ? 'Try a different search.' : 'Transfer music to the player to get started.'));
      if (offset < total) more.appendChild(h('button.btn', { onclick: function () { load(false); } }, 'Show more (' + (total - offset) + ')'));
    }).catch(function (e) { failState(list, e); });
  }
  input.addEventListener('input', function () {
    clearTimeout(timer);
    timer = setTimeout(function () { query = input.value.trim(); history.replaceState(null, '', '#/songs' + (query ? '?q=' + encodeURIComponent(query) : '')); load(true); }, 280);
  });
  queueViewRefresh = null;
  load(true);
  if (!('ontouchstart' in window)) input.focus();
};

function artistList(root, kind) {
  var title = kind === 'artist' ? 'Artists' : 'Album artists';
  var filter = h('input', { type: 'search', placeholder: 'Filter ' + title.toLowerCase(), 'aria-label': 'Filter' });
  var list = h('div.list', skeleton());
  root.appendChild(viewHead(title, null, h('label.search', iconEl('search'), filter)));
  root.appendChild(list);
  queueViewRefresh = null;
  api(kind === 'artist' ? '/api/library/artists' : '/api/library/album_artists').then(function (d) {
    var all = d.artists || [];
    root.querySelector('.view-head .grow').appendChild(h('p', plural(all.length, kind === 'artist' ? 'artist' : 'album artist', kind === 'artist' ? 'artists' : 'album artists')));
    function draw() {
      var f = filter.value.trim().toLowerCase();
      list.textContent = '';
      var shown = all.filter(function (a) { return !f || a.name.toLowerCase().indexOf(f) >= 0; });
      if (!shown.length) { list.appendChild(emptyState('artist', f ? 'No matches' : 'No artists yet')); return; }
      shown.forEach(function (a) { list.appendChild(groupRow(a, function () { go('albums', { kind: kind, name: a.name }); }, 'artist')); });
    }
    filter.addEventListener('input', draw);
    draw();
  }).catch(function (e) { failState(list, e); });
}
routes.artists = function (root) { artistList(root, 'artist'); };
routes['album-artists'] = function (root) { artistList(root, 'album_artist'); };

routes.albums = function (root, params) {
  var kind = params.get('kind') === 'album_artist' ? 'album_artist' : 'artist', name = params.get('name') || '';
  var grid = h('div.grid');
  root.appendChild(viewHead(name, null, null, { label: kind === 'artist' ? 'Artists' : 'Album artists', route: kind === 'artist' ? 'artists' : 'album-artists' }));
  root.appendChild(grid);
  grid.appendChild(h('div', skeleton(3)));
  queueViewRefresh = null;
  api('/api/library/albums?' + kind + '=' + encodeURIComponent(name)).then(function (d) {
    var albums = d.albums || [];
    grid.textContent = '';
    root.querySelector('.view-head .grow').appendChild(h('p', plural(albums.length, 'album', 'albums')));
    if (!albums.length) { grid.appendChild(emptyState('album', 'No albums found')); return; }
    albums.forEach(function (a) {
      var cover = h('div.cover', { html: icon('album') });
      loadArt('/api/art?index=' + a.index, function (url) { setCover(cover, url); });
      grid.appendChild(h('button.card', { onclick: function () { go('album', { kind: kind, name: name, album: a.name }); } },
        cover, h('div.title', a.name || 'Unknown album'), h('div.subtitle', plural(a.count, 'song', 'songs'))));
    });
  }).catch(function (e) { failState(grid, e); });
};

routes.album = function (root, params) {
  var kind = params.get('kind') === 'album_artist' ? 'album_artist' : 'artist', name = params.get('name') || '', album = params.get('album') || '';
  var context = '&' + kind + '=' + encodeURIComponent(name) + '&album=' + encodeURIComponent(album);
  var list = h('div.list', skeleton());
  root.appendChild(viewHead(album || 'Unknown album', name, null, { label: name, route: 'albums', params: { kind: kind, name: name } }));
  root.appendChild(list);
  queueViewRefresh = null;
  api('/api/library?limit=500' + context).then(function (d) {
    var songs = d.songs || [];
    list.textContent = '';
    if (!songs.length) { list.appendChild(emptyState('album', 'This album is empty')); return; }
    var head = root.querySelector('.view-head');
    head.appendChild(h('button.btn.primary', { onclick: function () { playSong(songs[0], context); } }, iconEl('play'), 'Play album'));
    songs.forEach(function (s, i) { list.appendChild(songRow(s, i + 1, context)); });
  }).catch(function (e) { failState(list, e); });
};

routes.playlists = function (root) {
  var list = h('div.list', skeleton());
  root.appendChild(viewHead('Playlists', null));
  root.appendChild(list);
  queueViewRefresh = null;
  api('/api/playlists').then(function (d) {
    var playlists = d.playlists || [];
    list.textContent = '';
    if (!playlists.length) { list.appendChild(emptyState('playlist', 'No playlists yet', 'Add songs to a new playlist from any song’s menu.')); return; }
    playlists.forEach(function (p) {
      var label = p.name.replace(/\.m3u8?$/i, '');
      list.appendChild(groupRow({ name: label }, function () { go('playlist', { key: p.key || label, name: label }); }));
    });
  }).catch(function (e) { failState(list, e); });
};

routes.playlist = function (root, params) {
  var key = params.get('key') || '', label = params.get('name') || key, context = '&playlist=' + encodeURIComponent(key);
  var list = h('div.list', skeleton());
  root.appendChild(viewHead(label, null, null, { label: 'Playlists', route: 'playlists' }));
  root.appendChild(list);
  queueViewRefresh = null;
  api('/api/playlists/songs?name=' + encodeURIComponent(key)).then(function (d) {
    var songs = d.songs || [];
    list.textContent = '';
    root.querySelector('.view-head .grow').appendChild(h('p', plural(songs.length, 'song', 'songs')));
    if (!songs.length) { list.appendChild(emptyState('playlist', 'This playlist is empty')); return; }
    root.querySelector('.view-head').appendChild(h('button.btn.primary', { onclick: function () { playSong(songs[0], context); } }, iconEl('play'), 'Play'));
    songs.forEach(function (s, i) { list.appendChild(songRow(s, i + 1, context)); });
  }).catch(function (e) { failState(list, e); });
};

/* ---------- Transfer files ---------- */
/* The SD card session is opened with /api/import/start and closed with
 * /api/import/stop, which also makes the player rescan its library. Uploads
 * stream each file as the raw request body to /api/import/files/upload. */
var AUDIO_EXT = /\.(flac|mp3|wav|m4a|aac|ogg|oga|opus|ape|wv|dsf|dff|aiff?|alac|mka|cue|lrc|m3u8?)$/i;
var IMAGE_EXT = /\.(jpe?g|png|webp|gif|bmp)$/i;
var T = {
  state: 'unknown', error: '', path: '', entries: [], total: 0, nextOffset: null, loading: false,
  queue: [], running: false, xhr: null, madeDirs: {}, uploadedSinceFinish: 0, ui: null
};

function importUrl(path, params) { return '/api/import/files' + path + (params ? '?' + new URLSearchParams(params).toString() : ''); }
function joinPath(a, b) { return a ? (b ? a + '/' + b : a) : b; }

function transferErrorText(err) {
  var code = err && err.code;
  var known = {
    name_exists: 'An item with that name already exists.',
    import_not_ready: 'The SD card is not open. Reopen it and try again.',
    wifi_only: 'File transfer only works over Wi-Fi.',
    file_too_large: 'Files must be smaller than 4 GB.',
    sd_card_unavailable: 'No SD card is inserted in the player.',
    player_storage_busy: 'The player is using its storage (USB mode or a library scan). Try again shortly.',
    file_import_busy: 'The player is busy with another transfer.',
    wifi_disabled: 'Wi-Fi is turned off on the player.',
    rename_not_supported: 'The SD card does not support renaming this item.'
  };
  if (code && known[code]) return known[code];
  if (err && err.status === 429) return 'Too many requests. Wait a moment.';
  if (err && err.message === 'Failed to fetch') return 'The player is unreachable.';
  return (err && err.message) || 'Something went wrong.';
}

function setTransferState(state, error) {
  T.state = state;
  T.error = error || '';
  drawTransfer();
}

function openSession() {
  if (T.state === 'starting' || T.state === 'ready') return Promise.resolve();
  setTransferState('starting');
  return api('/api/v1/capabilities', { noPin: true }).then(function (caps) {
    if ((caps.features || []).indexOf('wifi_import_files') < 0) { setTransferState('unsupported'); return; }
    return api('/api/import').then(function (d) {
      if (d.state === 'ready') return;
      return (d.state === 'starting' ? Promise.resolve() : api('/api/import/start', { method: 'POST' })).then(function () { return waitForImport('ready'); });
    }).then(function () {
      if (T.state === 'unsupported') return;
      setTransferState('ready');
      browse(T.path);
    });
  }).catch(function (err) { setTransferState('error', transferErrorText(err)); });
}

function waitForImport(target) {
  var tries = 0;
  return new Promise(function (resolve, reject) {
    (function check() {
      api('/api/import').then(function (d) {
        if (d.state === target) return resolve();
        if (d.state === 'error') { var e = new Error(d.error || 'The player could not open the SD card.'); e.code = d.error; return reject(e); }
        if (++tries > 40) return reject(new Error('The player did not respond in time.'));
        setTimeout(check, 700);
      }).catch(reject);
    })();
  });
}

function browse(path, append) {
  if (T.state !== 'ready') return;
  T.loading = true;
  if (!append) { T.path = path; T.entries = []; T.total = 0; T.nextOffset = null; }
  drawTransfer();
  var requested = T.path;
  api(importUrl('', { path: requested, offset: append ? T.nextOffset || 0 : 0, limit: 100 })).then(function (d) {
    if (requested !== T.path) return;
    T.entries = (append ? T.entries : []).concat(d.entries || []);
    T.total = d.total || 0;
    T.nextOffset = d.next_offset;
    T.loading = false;
    drawTransfer();
  }).catch(function (err) {
    T.loading = false;
    if (err.code === 'import_not_ready') { setTransferState('stopped'); return; }
    T.error = transferErrorText(err);
    drawTransfer();
  });
}

function finishSession() {
  if (T.running) return;
  setTransferState('stopping');
  api('/api/import/stop', { method: 'POST' }).then(function () { return waitForImport('stopped'); }).then(function () {
    T.uploadedSinceFinish = 0;
    T.queue = T.queue.filter(function (q) { return q.state === 'queued' || q.state === 'failed'; });
    setTransferState('stopped');
    toast('Done. The player is updating its library.');
  }).catch(function (err) { setTransferState('error', transferErrorText(err)); });
}

function mkdirDialog() {
  dialog({ title: 'New folder', text: 'Create a folder in SD/' + T.path, input: '', confirm: 'Create' }).then(function (name) {
    if (!name) return;
    api(importUrl('/mkdir', { path: T.path, name: name }), { method: 'POST' })
      .then(function () { toast('Folder created'); browse(T.path); })
      .catch(function (err) { toast(transferErrorText(err)); });
  });
}
function renameEntry(entry) {
  dialog({ title: 'Rename', input: entry.name, confirm: 'Rename' }).then(function (name) {
    if (!name || name === entry.name) return;
    api(importUrl('/rename', { path: entry.path, name: name }), { method: 'POST' })
      .then(function () { browse(T.path); })
      .catch(function (err) { toast(transferErrorText(err)); });
  });
}
function deleteEntry(entry) {
  dialog({ title: 'Delete ' + (entry.isDirectory ? 'folder' : 'file') + '?', text: '“' + entry.name + '” will be removed from the SD card. This cannot be undone.', confirm: 'Delete', danger: true }).then(function (ok) {
    if (!ok) return;
    api(importUrl('/delete', { path: entry.path }), { method: 'POST' })
      .then(function () { toast('Deleted'); browse(T.path); })
      .catch(function (err) { toast(transferErrorText(err)); });
  });
}

/* Queue entries keep their own destination, so browsing elsewhere while a
 * batch uploads never redirects files. */
function enqueue(files) {
  var base = T.path, added = 0;
  if (!T.running) T.madeDirs = {};
  files.forEach(function (f) {
    var rel = f.relDir != null ? f.relDir : (f.file.webkitRelativePath || '').split('/').slice(0, -1).join('/');
    var name = f.file.name;
    if (!name || name.charAt(0) === '.') return;
    if (f.file.size > 4294967295) { T.queue.push({ file: f.file, name: name, dest: joinPath(base, rel), state: 'failed', error: 'Larger than 4 GB', sent: 0 }); return; }
    T.queue.push({ file: f.file, name: name, dest: joinPath(base, rel), state: 'queued', sent: 0 });
    added++;
  });
  if (added) toast(plural(added, 'file', 'files') + ' added to the upload queue');
  drawTransfer();
  runQueue();
}

function ensureDir(path) {
  if (!path || T.madeDirs[path]) return Promise.resolve();
  var parts = path.split('/'), name = parts.pop(), parent = parts.join('/');
  return ensureDir(parent).then(function () {
    return api(importUrl('/mkdir', { path: parent, name: name }), { method: 'POST' }).catch(function (err) {
      if (err.code !== 'name_exists' && err.status !== 409) throw err;
    });
  }).then(function () { T.madeDirs[path] = true; });
}

function runQueue() {
  if (T.running || T.state !== 'ready') return;
  var item = T.queue.filter(function (q) { return q.state === 'queued'; })[0];
  if (!item) {
    if (T.queue.some(function (q) { return q.state === 'done'; })) browse(T.path);
    else drawTransfer();
    return;
  }
  T.running = true;
  item.state = 'uploading';
  drawTransfer();
  ensureDir(item.dest).then(function () {
    return new Promise(function (resolve) {
      if (item.state !== 'uploading') { resolve('abort'); return; }
      var xhr = new XMLHttpRequest();
      T.xhr = xhr;
      xhr.open('POST', importUrl('/upload', { path: item.dest, name: item.name }));
      xhr.withCredentials = true;
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      var lastPaint = 0;
      xhr.upload.onprogress = function (e) {
        item.sent = e.loaded;
        var now = Date.now();
        if (now - lastPaint > 120) { lastPaint = now; paintQueueProgress(); }
      };
      xhr.onload = function () {
        if (xhr.status === 201) { item.state = 'done'; item.sent = item.file.size; T.uploadedSinceFinish++; }
        else if (xhr.status === 409 && /name_exists/.test(xhr.responseText)) { item.state = 'skipped'; item.error = 'Already on the SD card'; }
        else if (xhr.status === 401) { item.state = 'queued'; requirePin().then(function () { resolve('retry'); }); return; }
        else { item.state = 'failed'; item.error = transferErrorText(HttpError(xhr.status, xhr.responseText)); }
        resolve();
      };
      xhr.onerror = function () { item.state = 'failed'; item.error = 'Connection lost'; resolve(); };
      xhr.onabort = function () { item.state = 'queued'; item.sent = 0; resolve('abort'); };
      xhr.send(item.file);
    });
  }, function (err) { item.state = 'failed'; item.error = transferErrorText(err); }).then(function (outcome) {
    T.running = false;
    T.xhr = null;
    drawTransfer();
    if (outcome === 'abort') return;
    runQueue();
  });
}

function cancelUploads() {
  T.queue.forEach(function (q) { if (q.state === 'queued') { q.state = 'skipped'; q.error = 'Cancelled'; } });
  if (T.xhr) T.xhr.abort();
  T.queue.forEach(function (q) { if (q.state === 'queued' || q.state === 'uploading') { q.state = 'skipped'; q.error = 'Cancelled'; } });
  T.running = false;
  drawTransfer();
}
function retryFailed() {
  T.queue.forEach(function (q) { if (q.state === 'failed' && q.error !== 'Larger than 4 GB') { q.state = 'queued'; q.sent = 0; q.error = ''; } });
  drawTransfer();
  runQueue();
}
function clearFinished() {
  T.queue = T.queue.filter(function (q) { return q.state === 'queued' || q.state === 'uploading'; });
  drawTransfer();
}

/* Drag-and-drop: folders arrive as entries and are walked recursively. */
function readDropped(dataTransfer) {
  var items = dataTransfer.items ? Array.prototype.slice.call(dataTransfer.items) : [];
  var entries = items.map(function (it) { return it.webkitGetAsEntry ? it.webkitGetAsEntry() : null; }).filter(Boolean);
  if (!entries.length) return Promise.resolve(Array.prototype.slice.call(dataTransfer.files || []).map(function (f) { return { file: f, relDir: '' }; }));
  var out = [];
  function walk(entry, dir) {
    if (entry.isFile) return new Promise(function (resolve) { entry.file(function (f) { out.push({ file: f, relDir: dir }); resolve(); }, resolve); });
    if (!entry.isDirectory) return Promise.resolve();
    var reader = entry.createReader(), sub = joinPath(dir, entry.name);
    return new Promise(function (resolve) {
      var all = [];
      (function readBatch() {
        reader.readEntries(function (batch) {
          if (!batch.length) { Promise.all(all.map(function (e) { return walk(e, sub); })).then(resolve); return; }
          all = all.concat(Array.prototype.slice.call(batch));
          readBatch();
        }, resolve);
      })();
    });
  }
  return Promise.all(entries.map(function (e) { return walk(e, ''); })).then(function () { return out; });
}

function pickFiles(directory) {
  var input = h('input', { type: 'file', multiple: true, style: 'display:none' });
  if (directory) { input.setAttribute('webkitdirectory', ''); input.setAttribute('directory', ''); }
  input.addEventListener('change', function () {
    enqueue(Array.prototype.slice.call(input.files || []).map(function (f) { return { file: f }; }));
    input.remove();
  });
  document.body.appendChild(input);
  input.click();
}

function fileKind(entry) { return entry.isDirectory ? 'dir' : AUDIO_EXT.test(entry.name) ? 'audio' : IMAGE_EXT.test(entry.name) ? 'image' : 'other'; }

routes.transfer = function (root) {
  queueViewRefresh = null;
  T.ui = root;
  drawTransfer();
  if (T.state === 'unknown' || T.state === 'stopped' || T.state === 'error') openSession();
};

function drawTransfer() {
  var root = T.ui;
  if (!root || !root.isConnected || currentRoute !== 'transfer') return;
  var main = $('#main'), scroll = main.scrollTop;
  root.style.minHeight = root.offsetHeight + 'px';
  root.textContent = '';
  var ready = T.state === 'ready';
  var statusText = {
    unknown: 'Checking the player…', starting: 'Opening the SD card…', ready: 'SD card open — drop files or folders to upload',
    stopping: 'Finishing and updating the library…', stopped: 'The SD card is closed', error: T.error, unsupported: 'This player needs a software update for file transfer'
  }[T.state];
  root.appendChild(h('div.view-head.transfer-head',
    h('div.grow', h('h1', 'Transfer files'),
      h('div.status', h('span.dot' + (ready ? '.ok' : T.state === 'error' ? '.bad' : '')), statusText)),
    ready && T.uploadedSinceFinish && !T.running
      ? h('button.btn.primary', { onclick: finishSession }, iconEl('check'), 'Finish & update library') : null,
    T.state === 'stopped' || T.state === 'error' ? h('button.btn.primary', { onclick: openSession }, iconEl('sd'), 'Open SD card') : null));

  if (!ready) {
    if (T.state === 'starting' || T.state === 'stopping' || T.state === 'unknown') root.appendChild(h('div.files', h('div', { style: 'padding:14px' }, skeleton(5))));
    else if (T.state === 'stopped') root.appendChild(h('div.files', h('div.drop-hint', h('div.ring', { html: icon('sd') }), h('b', 'Open the SD card to transfer files'), h('span', 'The player pauses library updates while the card is open and rescans when you finish.'))));
    if (T.queue.length) root.appendChild(queueCard());
    root.style.minHeight = '';
    return;
  }

  var crumbs = h('div.crumbs', h('button', { onclick: function () { browse(''); } }, iconEl('sd'), 'SD card'));
  var acc = '';
  T.path.split('/').filter(Boolean).forEach(function (part) {
    acc = joinPath(acc, part);
    var target = acc;
    crumbs.appendChild(h('span.sep', { html: icon('chevronRight') }));
    crumbs.appendChild(h('button', { onclick: function () { browse(target); } }, part));
  });
  var parent = T.path ? T.path.split('/').slice(0, -1).join('/') : null;
  root.appendChild(h('div.toolbar',
    h('button.icon-btn', { 'aria-label': 'Parent folder', title: 'Parent folder', html: icon('folderUp'), disabled: parent === null, onclick: function () { browse(parent); } }),
    crumbs,
    h('button.icon-btn', { 'aria-label': 'Refresh', title: 'Refresh', html: icon('refresh'), onclick: function () { browse(T.path); } }),
    h('button.btn.sm', { onclick: mkdirDialog }, iconEl('folderPlus'), 'New folder'),
    h('button.btn.sm', { onclick: function () { pickFiles(true); } }, iconEl('folder'), 'Upload folder'),
    h('button.btn.sm.primary', { onclick: function () { pickFiles(false); } }, iconEl('upload'), 'Upload files')));

  if (T.error) root.appendChild(h('div.notice', iconEl('alert'), h('span', T.error)));

  var files = h('div.files', h('div.dropzone', iconEl('upload'), 'Drop to upload to SD/' + T.path));
  files.appendChild(h('div.files-head', h('span', 'Name'), h('span.size-col', { style: 'text-align:right' }, 'Size'), h('span')));
  if (T.loading && !T.entries.length) files.appendChild(h('div', { style: 'padding:12px' }, skeleton(6)));
  else if (!T.entries.length) {
    files.appendChild(h('div.drop-hint', h('div.ring', { html: icon('upload') }), h('b', 'This folder is empty'), h('span', 'Drag music here, or use Upload files / Upload folder.')));
  }
  T.entries.forEach(function (entry) {
    var kind = fileKind(entry);
    files.appendChild(h('div.file.' + kind,
      h('button.name', { onclick: entry.isDirectory ? function () { browse(entry.path); } : null, style: entry.isDirectory ? '' : 'cursor:default', title: entry.name },
        h('span.ico', { html: icon(kind === 'dir' ? 'folder' : kind === 'audio' ? 'fileAudio' : kind === 'image' ? 'fileImage' : 'file') }),
        h('span', entry.name)),
      h('span.size', entry.isDirectory ? '—' : fmtBytes(entry.size)),
      h('button.icon-btn', { 'aria-label': 'Options for ' + entry.name, html: icon('more'), onclick: function (e) {
        e.stopPropagation();
        menu(e.currentTarget, (entry.isDirectory ? [{ icon: 'folder', label: 'Open', run: function () { browse(entry.path); } }] : []).concat([
          { icon: 'rename', label: 'Rename…', run: function () { renameEntry(entry); } },
          { icon: 'trash', label: 'Delete', danger: true, run: function () { deleteEntry(entry); } }
        ]));
      } })));
  });
  files.appendChild(h('div.files-foot',
    h('span', T.loading ? 'Loading…' : plural(T.total, 'item', 'items')),
    T.nextOffset != null && T.nextOffset < T.total ? h('button.btn.sm', { onclick: function () { browse(T.path, true); } }, 'Load more') : h('span')));

  var depth = 0;
  files.addEventListener('dragenter', function (e) { e.preventDefault(); depth++; files.classList.add('dragover'); });
  files.addEventListener('dragover', function (e) { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; });
  files.addEventListener('dragleave', function () { if (--depth <= 0) { depth = 0; files.classList.remove('dragover'); } });
  files.addEventListener('drop', function (e) {
    e.preventDefault();
    depth = 0;
    files.classList.remove('dragover');
    readDropped(e.dataTransfer).then(enqueue);
  });

  root.appendChild(h('div.transfer-layout', files, queueCard()));
  root.style.minHeight = '';
  main.scrollTop = scroll;
}

function queueCard() {
  var q = T.queue;
  var totalBytes = 0, sentBytes = 0, counts = { queued: 0, uploading: 0, done: 0, failed: 0, skipped: 0 };
  q.forEach(function (it) {
    counts[it.state]++;
    if (it.state === 'skipped' && it.error === 'Cancelled') return;
    totalBytes += it.file.size;
    sentBytes += it.state === 'done' || it.state === 'skipped' ? it.file.size : it.sent;
  });
  var pending = counts.queued + counts.uploading;
  var summary = !q.length ? 'Files you add appear here.'
    : pending ? plural(pending, 'file', 'files') + ' left · ' + fmtBytes(sentBytes) + ' of ' + fmtBytes(totalBytes)
    : plural(counts.done, 'file', 'files') + ' uploaded' + (counts.skipped ? ', ' + counts.skipped + ' skipped' : '') + (counts.failed ? ', ' + counts.failed + ' failed' : '');
  var items = h('div.queue-items');
  if (!q.length) items.appendChild(h('div.queue-empty', 'Nothing uploading yet.'));
  q.slice().reverse().slice(0, 300).forEach(function (it) {
    var stateIcon = { queued: 'clock', uploading: 'upload', done: 'checkCircle', failed: 'alert', skipped: 'skip' }[it.state];
    var stateText = it.state === 'uploading' ? Math.floor(100 * it.sent / Math.max(1, it.file.size)) + '%'
      : it.state === 'done' ? fmtBytes(it.file.size) : it.state === 'queued' ? 'Waiting' : it.error || it.state;
    items.appendChild(h('div.qitem.' + it.state, { 'data-q': '' },
      iconEl(stateIcon),
      h('div', { style: 'min-width:0' }, h('div.qname', { title: it.name }, it.name), h('div.qsub', 'SD/' + it.dest),
        it.state === 'uploading' ? h('div.bar', h('i', { style: 'width:' + (100 * it.sent / Math.max(1, it.file.size)) + '%' })) : null),
      h('span.qstate', stateText)));
  });
  return h('aside.queue-card', { 'aria-label': 'Uploads' },
    h('header', h('b', 'Uploads'), h('p', { 'data-q-summary': '' }, summary),
      h('div.progress', h('i', { 'data-q-progress': '', style: 'width:' + (totalBytes ? 100 * sentBytes / totalBytes : 0) + '%' }))),
    items,
    q.length ? h('footer',
      pending ? h('button.btn.sm.danger', { onclick: cancelUploads }, 'Cancel') : null,
      counts.failed && !pending ? h('button.btn.sm', { onclick: retryFailed }, 'Retry failed') : null,
      !pending ? h('button.btn.sm.ghost', { onclick: clearFinished }, 'Clear list') : null) : null);
}

/* Progress events repaint only the live numbers, not the whole page. */
function paintQueueProgress() {
  if (!T.ui || !T.ui.isConnected) return;
  var card = $('.queue-card', T.ui);
  if (!card) return;
  var fresh = queueCard();
  card.replaceWith(fresh);
}

window.addEventListener('beforeunload', function (e) {
  if (T.running || T.queue.some(function (q) { return q.state === 'queued'; })) { e.preventDefault(); e.returnValue = ''; }
});

/* ---------- Boot ---------- */
hydrateIcons(document);
$all('[data-play]').forEach(function (el) { el.innerHTML = icon('play'); });
var savedTheme = null;
try { savedTheme = localStorage.getItem(THEME_KEY); } catch (e) { /* storage may be blocked */ }
setTheme(savedTheme || (window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark'));
$('#themeSide').addEventListener('click', toggleTheme);
$('#themeTop').addEventListener('click', toggleTheme);
$all('[data-route]').forEach(function (el) { el.addEventListener('click', function () { go(el.getAttribute('data-route')); }); });
$all('[data-cmd]').forEach(function (el) { el.addEventListener('click', function () { command(el.getAttribute('data-cmd')); }); });
$('#modeBtn').addEventListener('click', function () { command('mode'); });
bindSeek($('.player [data-seek]'));
bindSlider($('#volume'), function () {}, function (f) { post('/api/playback/volume?percent=' + Math.round(f * 100)); }, function (v) { volumeDragging = v; });
render();
poll();
setInterval(poll, 1000);
