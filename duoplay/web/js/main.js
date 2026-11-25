// DuoPlay app controller: setup screens, settings, and the streaming stage.

import { api } from './api.js';
import { classify, computeLayout } from './layout.js';
import { TouchControls } from './controls.js';
import { InputState, InputSender, GamepadInput } from './input.js';
import { StreamConnection } from './stream.js';

const $ = (id) => document.getElementById(id);

// ------------------------------------------------------------------ settings
const DEFAULTS = {
  height: 1080, fps: 60, bitrate: 0, codec: 'h264', latency: 1, adaptive: true, smooth: true,
  hideWithPad: true, showStats: false,
  ctl: { cover: { scale: 1, opacity: 0.55 }, inner: { scale: 1, opacity: 0.5 } },
};
const settings = loadSettings();
function loadSettings() {
  try {
    const s = JSON.parse(localStorage.getItem('duoplay.settings') || '{}');
    return { ...DEFAULTS, ...s, ctl: { cover: { ...DEFAULTS.ctl.cover, ...s.ctl?.cover }, inner: { ...DEFAULTS.ctl.inner, ...s.ctl?.inner } } };
  } catch { return structuredClone(DEFAULTS); }
}
function saveSettings() { try { localStorage.setItem('duoplay.settings', JSON.stringify(settings)); } catch { /* private mode */ } }
const hevcSupported = (() => {
  try { return (RTCRtpReceiver.getCapabilities?.('video')?.codecs || []).some((c) => /h265|hevc/i.test(c.mimeType)); } catch { return false; }
})();
if (!hevcSupported) settings.codec = 'h264';

// ------------------------------------------------------------------ stage wiring
const stage = $('stage');
const videoBox = $('video-box');
const video = $('video');
const input = new InputState();
const sender = new InputSender(input);
const controls = new TouchControls($('controls'), input, { onMenu: () => openMenu() });
const gamepads = new GamepadInput(input, {
  onActive: () => { if (settings.hideWithPad) $('controls').classList.add('hidden-by-pad'); },
});
let videoAspect = 16 / 9;
let current = null; // last layout
let streamingUi = false;

// Safe-area insets differ between the two screens (camera cut-outs, rounded corners).
// A probe padded with env(safe-area-inset-*) reports the live values.
const probe = document.createElement('div');
probe.style.cssText = 'position:fixed;inset:0;visibility:hidden;pointer-events:none;' +
  'padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)';
document.body.appendChild(probe);
function safeArea() {
  const cs = getComputedStyle(probe);
  return { t: parseFloat(cs.paddingTop) || 0, r: parseFloat(cs.paddingRight) || 0, b: parseFloat(cs.paddingBottom) || 0, l: parseFloat(cs.paddingLeft) || 0 };
}
function segments() {
  try {
    const s = window.viewport?.segments || window.getWindowSegments?.();
    return s && s.length > 1 ? [...s] : null;
  } catch { return null; }
}

function relayout() {
  if (stage.hidden) return;
  const w = stage.clientWidth, h = stage.clientHeight;
  if (!w || !h) return;
  const cls = classify(w, h);
  const prefs = settings.ctl[cls.screen];
  const l = computeLayout({ width: w, height: h, safe: safeArea(), videoAspect, userScale: prefs.scale, segments: segments() });
  const v = l.video;
  videoBox.style.transform = `translate3d(${v.x}px, ${v.y}px, 0)`;
  videoBox.style.width = `${v.w}px`;
  videoBox.style.height = `${v.h}px`;
  stage.dataset.mode = l.mode;
  stage.dataset.screen = l.screen;
  stage.dataset.orient = l.orient;
  stage.style.setProperty('--ctl-opacity', String(prefs.opacity));
  const screenChanged = current && (current.screen !== l.screen || current.orient !== l.orient);
  controls.apply(l);
  if (screenChanged) {
    sender.neutralNow(); // the release from controls.apply goes out immediately
    console.info(`[duoplay] screen: ${l.screen} ${l.orient} -> ${l.mode} (${w}x${h})`);
  }
  current = l;
  document.documentElement.dataset.screen = l.screen;
}

// ResizeObserver callbacks run after layout and before paint, so a fold/unfold or
// rotation is laid out in the same frame the new screen first draws.
new ResizeObserver(relayout).observe(stage);
window.visualViewport?.addEventListener('resize', relayout);
window.addEventListener('orientationchange', relayout);
video.addEventListener('resize', () => {
  if (video.videoWidth && video.videoHeight) {
    const a = video.videoWidth / video.videoHeight;
    if (Math.abs(a - videoAspect) > 0.01) { videoAspect = a; relayout(); }
  }
});
let soundBlocked = false;
stage.addEventListener('pointerdown', () => {
  $('controls').classList.remove('hidden-by-pad');
  if (soundBlocked) {
    soundBlocked = false;
    video.muted = false;
    video.play().catch(() => { soundBlocked = true; });
  }
}, { capture: true });

// ------------------------------------------------------------------ streaming
let conn = null;
let session = { state: 'idle' };
let pollTimer = null;
let firstFrame = false;
let wakeLock = null;
let reconnectToastTimer = null;
let poorSince = 0, lastAdvice = 0;

function enterStage() {
  document.body.classList.replace('mode-setup', 'mode-stream');
  $('setup').hidden = true;
  stage.hidden = false;
  streamingUi = true;
  current = null;
  relayout();
  requestWakeLock();
}

function exitStage() {
  streamingUi = false;
  stage.hidden = true;
  $('setup').hidden = false;
  document.body.classList.replace('mode-stream', 'mode-setup');
  controls.releaseAll();
  video.srcObject = null;
  wakeLock?.release?.().catch(() => {});
  wakeLock = null;
  closeSheet();
  refreshState(true);
}

async function requestWakeLock() {
  try { if ('wakeLock' in navigator && document.visibilityState === 'visible') wakeLock = await navigator.wakeLock.request('screen'); } catch { /* needs HTTPS */ }
}
document.addEventListener('visibilitychange', () => { if (streamingUi && document.visibilityState === 'visible') requestWakeLock(); });

function overlay(title, text = '', { spinner = true, retry = false, pin = false, sound = false } = {}) {
  $('overlay').hidden = false;
  $('overlay-title').textContent = title;
  $('overlay-text').textContent = text;
  $('overlay-spinner').hidden = !spinner;
  $('overlay-retry').hidden = !retry;
  $('overlay-pin').hidden = !pin;
  $('overlay-sound').hidden = !sound;
  if (pin) setTimeout(() => $('login-pin').focus(), 50);
}
function hideOverlay() { $('overlay').hidden = true; }

function toast(msg, ms = 3500) {
  const t = $('toast');
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toast.timer);
  if (ms) toast.timer = setTimeout(() => { t.hidden = true; }, ms);
}

let playing = null;
async function play(c) {
  playing = c;
  // Inside the tap: lets Safari start sound later without another tap.
  video.muted = false;
  video.play().catch(() => {});
  enterStage();
  firstFrame = false;
  overlay(c.state === 'standby' ? `Waking ${c.nickname}…` : `Connecting to ${c.nickname}…`,
    c.state === 'standby' ? 'This can take up to half a minute.' : '');
  try {
    await api.post('/api/session/start', {
      console_id: c.id, height: Number(settings.height), fps: Number(settings.fps),
      bitrate_kbps: Number(settings.bitrate), hevc: settings.codec === 'hevc' && hevcSupported,
    }, 20000);
  } catch (e) {
    overlay('Could not start', e.message, { spinner: false, retry: true });
    return;
  }
  pollSession();
}

function pollSession() {
  clearTimeout(pollTimer);
  const tick = async () => {
    if (!streamingUi) return;
    try {
      const st = await api.get('/api/state');
      onSession(st.session);
    } catch (e) {
      if (!firstFrame) overlay('Bridge unreachable', e.message, { spinner: true });
    }
    // fast while setting up, slow once the data channel carries updates
    pollTimer = setTimeout(tick, firstFrame ? 3000 : 600);
  };
  tick();
}

function onSession(s) {
  if (!s || !streamingUi) return;
  session = s;
  const name = playing?.nickname || s.console_name || 'console';
  if (conn) conn.streaming = s.state === 'streaming';
  switch (s.state) {
    case 'waking':
      overlay(`Waking ${name}…`, 'This can take up to half a minute.');
      break;
    case 'connecting':
      if (!firstFrame) overlay(`Connecting to ${name}…`);
      startConnection();
      break;
    case 'pin':
      overlay('Console passcode', s.message || 'Enter the passcode for the user signed in on the console.', { spinner: false, pin: true });
      startConnection();
      break;
    case 'streaming':
      startConnection();
      if (firstFrame) hideOverlay();
      else if ($('overlay').hidden || $('overlay-title').textContent.startsWith('Connecting'))
        overlay(`Connecting to ${name}…`, 'Waiting for the first picture…');
      break;
    case 'error':
    case 'ended':
      overlay(s.state === 'error' ? 'Stream stopped' : 'Stream ended', s.message || '', { spinner: false, retry: true });
      break;
    case 'idle':
      if (conn && firstFrame) overlay('Stream stopped', 'The session was closed on the bridge.', { spinner: false, retry: true });
      break;
  }
}

function startConnection() {
  if (conn) return;
  conn = new StreamConnection({
    video, sender,
    onSession,
    onControl: (m) => { if (m.t === 'rumble') gamepads.rumble(m.l, m.r); },
    onStatus: (st, detail) => {
      if (st === 'needs-tap') {
        // Safari wants a touch before it plays sound. The picture keeps going muted and
        // the first touch anywhere (you'll be touching the controls anyway) unmutes it.
        soundBlocked = true;
        toast('Touch the screen to turn on sound', 4000);
        return;
      }
      clearTimeout(reconnectToastTimer);
      if (st === 'reconnecting' && firstFrame) {
        // Only mention it if it isn't fixed almost instantly.
        reconnectToastTimer = setTimeout(() => toast('Reconnecting…', 0), 700);
      }
    },
    onStats: (s) => onStats(s),
  });
  conn.start(settings);
}

video.addEventListener('playing', () => {
  if (!streamingUi) return;
  firstFrame = true;
  clearTimeout(reconnectToastTimer);
  $('toast').hidden = true;
  if (session.state === 'streaming' || session.state === 'connecting') hideOverlay();
});
video.addEventListener('loadeddata', () => video.dispatchEvent(new Event('playing')));

function onStats(s) {
  if (s.fps > 0 && !firstFrame) { firstFrame = true; hideOverlay(); }
  if (!$('toast').hidden && $('toast').textContent === 'Reconnecting…') $('toast').hidden = true;
  const dot = $('quality-dot');
  if (dot) dot.className = `quality-dot ${s.quality}`;
  const hud = $('hud');
  hud.hidden = !settings.showStats;
  if (settings.showStats) {
    const f = (v, d = 0) => (v == null || Number.isNaN(v) ? '–' : v.toFixed(d));
    hud.textContent = `${s.height || '?'}p ${f(s.fps)}fps · ${f(s.mbps, 1)} Mbps · rtt ${f(s.rttMs)} ms · loss ${f(s.lossPct, 1)}% · buf ${f(s.bufferMs)} ms · dec ${f(s.decodeMs, 1)} ms`;
  }
  lastStats = s;
  // Persistent trouble: tell the user what would actually help.
  const now = performance.now();
  if (s.quality === 'poor') {
    if (!poorSince) poorSince = now;
    if (now - poorSince > 6000 && now - lastAdvice > 90000) {
      lastAdvice = now;
      toast(`Wi-Fi is struggling (${(s.lossPct ?? 0).toFixed(1)}% loss, ${Math.round(s.rttMs ?? 0)} ms). ` +
        'Move closer to the router, use its 5/6 GHz network, wire the console and bridge, or lower the bitrate.', 8000);
    }
  } else poorSince = 0;
}
let lastStats = null;

async function leave(stopSession) {
  clearTimeout(pollTimer);
  conn?.stop();
  conn = null;
  if (stopSession) api.post('/api/session/stop').catch(() => {});
  hideOverlay();
  exitStage();
}

$('overlay-leave').onclick = () => leave(true);
$('overlay-retry').onclick = () => {
  conn?.stop(); conn = null;
  if (playing) { exitStage(); play(playing); }
};
$('overlay-sound').onclick = () => {
  video.muted = false;
  video.play().catch(() => {});
  hideOverlay();
};
$('login-pin-send').onclick = async () => {
  const pin = $('login-pin').value.trim();
  if (!pin) return;
  try {
    await api.post('/api/session/pin', { pin });
    $('login-pin').value = '';
    overlay('Connecting…');
  } catch (e) { $('overlay-text').textContent = e.message; }
};

// ------------------------------------------------------------------ sheets
function openSheet(tplId, setup) {
  const body = $('sheet-body');
  body.replaceChildren($(tplId).content.cloneNode(true));
  $('sheet').hidden = false;
  controls.releaseAll();
  setup?.(body);
}
function closeSheet() { $('sheet').hidden = true; }
$('sheet').addEventListener('click', (e) => { if (e.target.closest('[data-close-sheet]')) closeSheet(); });

function openMenu() {
  openSheet('tpl-menu', (body) => {
    const s = lastStats;
    body.querySelector('#menu-net').textContent = s
      ? `Wi-Fi: ${s.quality} · ${s.mbps?.toFixed(1) ?? '–'} Mbps · ${Math.round(s.rttMs ?? 0)} ms round trip · ${(s.lossPct ?? 0).toFixed(1)}% loss`
      : '';
    body.querySelector('.menu-grid').addEventListener('click', async (e) => {
      const act = e.target.closest('[data-act]')?.dataset.act;
      if (!act) return;
      if (act === 'ps') { input.pulseButton(1 << 15, 150); closeSheet(); }
      if (act === 'stats') { settings.showStats = !settings.showStats; saveSettings(); $('hud').hidden = !settings.showStats; closeSheet(); }
      if (act === 'controls') { const c = $('controls'); c.classList.toggle('hidden-by-pad'); closeSheet(); }
      if (act === 'settings') openSettings();
      if (act === 'rest') { closeSheet(); try { await api.post('/api/session/standby'); } catch (err) { toast(err.message); } }
      if (act === 'leave') { closeSheet(); leave(true); }
    });
  });
}

function openSettings() {
  openSheet('tpl-settings', (body) => {
    const screen = classify(innerWidth, innerHeight).screen;
    body.querySelector('[data-screen-label]').textContent = screen === 'inner' ? 'inside screen' : 'cover screen';
    if (!hevcSupported) {
      const opt = body.querySelector('[data-set=codec] option[value=hevc]');
      opt.disabled = true;
      opt.textContent += ' — not supported here';
    }
    for (const el of body.querySelectorAll('[data-set]')) {
      const key = el.dataset.set;
      const get = () => (key.startsWith('ctl') ? settings.ctl[screen][key === 'ctlScale' ? 'scale' : 'opacity'] : settings[key]);
      if (el.type === 'checkbox') el.checked = !!get(); else el.value = String(get());
      el.addEventListener('input', () => {
        const val = el.type === 'checkbox' ? el.checked : el.type === 'range' ? Number(el.value) : el.value;
        if (key === 'ctlScale') settings.ctl[screen].scale = val;
        else if (key === 'ctlOpacity') settings.ctl[screen].opacity = val;
        else settings[key] = val;
        saveSettings();
        if (key.startsWith('ctl')) relayout();
        if (key === 'showStats') $('hud').hidden = !val;
        if (key === 'adaptive') conn?.sendControl({ t: 'settings', adaptive: !!val });
      });
    }
  });
}
$('btn-settings').onclick = openSettings;

// ------------------------------------------------------------------ setup screen
let stateTimer = null;
let lastState = null;

async function refreshState(discover = false) {
  clearTimeout(stateTimer);
  if (streamingUi) return;
  try {
    const st = await api.get(`/api/state${discover ? '?discover=1' : ''}`);
    lastState = st;
    renderSetup(st);
    $('footer-note').textContent = `Bridge ${st.version} · ${st.secure ? 'secure connection' : 'not secure (HTTP)'}`;
  } catch (e) {
    $('footer-note').textContent = e.message;
  }
  stateTimer = setTimeout(() => refreshState(), 2000);
}

function renderSetup(st) {
  $('step-psn').hidden = st.psn_account_set;
  const list = $('console-list');
  list.replaceChildren();
  const consoles = st.consoles || [];
  $('console-empty').hidden = consoles.some((c) => !c.test);
  for (const c of consoles) {
    const row = document.createElement('div');
    row.className = 'console';
    const name = document.createElement('div');
    name.className = 'console-name';
    const dot = document.createElement('span');
    dot.className = `dot ${c.state}`;
    name.append(dot, document.createTextNode(c.nickname || c.host));
    if (!c.ps5) { const b = document.createElement('span'); b.className = 'badge'; b.textContent = 'PS4'; name.append(b); }
    const meta = document.createElement('div');
    meta.className = 'console-meta';
    const stateText = { ready: 'Ready', standby: 'Rest mode — will be woken up', unknown: 'Not found on the network' }[c.state] || c.state;
    meta.textContent = `${stateText}${c.running_app ? ` · ${c.running_app}` : ''} · ${c.host}`;
    const actions = document.createElement('div');
    actions.className = 'console-actions';
    if (c.paired) {
      const playBtn = document.createElement('button');
      playBtn.className = 'btn primary';
      playBtn.textContent = 'Play';
      playBtn.onclick = () => play(c);
      actions.append(playBtn);
      if (!c.test) {
        const more = document.createElement('button');
        more.className = 'btn ghost';
        more.textContent = '⋯';
        more.setAttribute('aria-label', `More for ${c.nickname}`);
        more.onclick = () => consoleMenu(c);
        actions.append(more);
      }
    } else {
      const pairBtn = document.createElement('button');
      pairBtn.className = 'btn';
      pairBtn.textContent = 'Pair';
      pairBtn.onclick = () => pairSheet(c);
      actions.append(pairBtn);
    }
    row.append(name, actions, meta);
    list.append(row);
  }
  const https = !window.isSecureContext && st.https_port;
  $('banner-https').hidden = !https || dismissed('banner-https');
  if (https) {
    $('link-https').href = `https://${location.hostname}:${st.https_port}/`;
  }
  const standalone = navigator.standalone || matchMedia('(display-mode: standalone), (display-mode: fullscreen)').matches;
  $('banner-home').hidden = standalone || dismissed('banner-home') || !$('banner-https').hidden;
}

function dismissed(id) { try { return localStorage.getItem(`duoplay.dismiss.${id}`) === '1'; } catch { return false; } }
document.addEventListener('click', (e) => {
  const id = e.target.closest('[data-dismiss]')?.dataset.dismiss;
  if (!id) return;
  try { localStorage.setItem(`duoplay.dismiss.${id}`, '1'); } catch { /* */ }
  $(id).hidden = true;
});

function sheetForm(title, html, onSubmit) {
  const body = $('sheet-body');
  body.innerHTML = `<h2></h2>${html}<p class="error" hidden></p><button class="btn primary" data-submit>Continue</button><button class="btn ghost" data-close-sheet>Cancel</button>`;
  body.querySelector('h2').textContent = title;
  $('sheet').hidden = false;
  const err = body.querySelector('.error');
  const submit = body.querySelector('[data-submit]');
  submit.onclick = async () => {
    err.hidden = true;
    submit.disabled = true;
    const label = submit.textContent;
    submit.textContent = 'Working…';
    try { await onSubmit(body); closeSheet(); refreshState(true); } catch (e) { err.textContent = e.message; err.hidden = false; }
    submit.disabled = false;
    submit.textContent = label;
  };
  return body;
}

function pairSheet(c) {
  if (!lastState?.psn_account_set) { toast('Link your PlayStation account first (step 1).'); $('step-psn').scrollIntoView({ behavior: 'smooth' }); return; }
  const b = sheetForm(`Pair ${c.nickname || 'console'}`,
    `<p class="hint">On the console open <b>Settings → System → Remote Play → Pair Device</b>. Enter the 8-digit number it shows.</p>
     <input class="pin-big" id="pair-pin" inputmode="numeric" pattern="[0-9]*" maxlength="9" placeholder="0000 0000" autocomplete="off">`,
    async (body) => {
      const pin = body.querySelector('#pair-pin').value.replace(/\D/g, '');
      if (pin.length !== 8) throw new Error('The pairing number has 8 digits.');
      await api.post('/api/consoles/register', { host: c.host, pin, ps5: c.ps5 !== false }, 45000);
      toast('Paired. Tap Play to start.');
    });
  setTimeout(() => b.querySelector('#pair-pin').focus(), 60);
}

function consoleMenu(c) {
  const body = $('sheet-body');
  body.innerHTML = `<h2></h2><div class="menu-grid">
    <button class="tile" data-a="wake"><b>Wake up</b><span>Turn on from rest mode</span></button>
    <button class="tile" data-a="repair"><b>Pair again</b><span>If streaming stopped working</span></button>
    <button class="tile danger" data-a="remove"><b>Forget</b><span>Remove pairing</span></button></div>
    <button class="btn ghost" data-close-sheet>Close</button>`;
  body.querySelector('h2').textContent = c.nickname;
  $('sheet').hidden = false;
  body.querySelector('.menu-grid').onclick = async (e) => {
    const a = e.target.closest('[data-a]')?.dataset.a;
    if (!a) return;
    try {
      if (a === 'wake') { await api.post('/api/consoles/wake', { id: c.id }); toast('Wake-up sent.'); closeSheet(); }
      if (a === 'repair') { closeSheet(); pairSheet({ ...c, paired: false }); }
      if (a === 'remove') { await api.post('/api/consoles/remove', { id: c.id }); closeSheet(); refreshState(); }
    } catch (err) { toast(err.message); }
  };
}

$('btn-add-ip').onclick = () => {
  const b = sheetForm('Add a console by address',
    `<p class="hint">Use this if your console doesn't show up by itself (some routers block discovery). Find its address on the console under
     <b>Settings → Network → Connection Status → View Connection Status</b>.</p>
     <input id="add-ip" inputmode="decimal" placeholder="192.168.1.50" autocomplete="off">
     <label class="switch"><input type="checkbox" id="add-ps4"><span>It's a PS4</span></label>`,
    async (body) => {
      const host = body.querySelector('#add-ip').value.trim();
      if (!host) throw new Error('Enter an address.');
      await api.post('/api/discover', { host });
      toast('Looking for it… it appears in the list once it answers.');
    });
  setTimeout(() => b.querySelector('#add-ip').focus(), 60);
};

// PSN account
api.get('/api/psn/login-url').then((r) => { $('psn-login').href = r.url; }).catch(() => {});
async function savePsn(payload) {
  const err = $('psn-error');
  err.hidden = true;
  try { await api.post('/api/psn/account', payload, 30000); toast('Account linked.'); refreshState(true); }
  catch (e) { err.textContent = e.message; err.hidden = false; }
}
$('psn-save').onclick = () => savePsn({ redirect_url: $('psn-redirect').value.trim() });
$('psn-id-save').onclick = () => savePsn({ account_id: $('psn-id').value.trim() });

// Diagnostics handle (used by the automated tests; handy in Web Inspector too).
window.duoplay = {
  get conn() { return conn; }, get layout() { return current; }, get stats() { return lastStats; },
  input, controls, relayout, settings,
};

// ------------------------------------------------------------------ go
refreshState(true);
// Keep layout correct even if the page is opened while already folded/rotated.
relayout();
