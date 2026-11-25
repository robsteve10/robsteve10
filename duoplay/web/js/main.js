// DuoPlay app controller: home screen, settings, and the play stage.

import { api } from './api.js';
import { computeLayout } from './layout.js';
import { refreshDisplay, onDisplayChange, currentDisplay, isStandalone } from './display.js';
import { TouchControls } from './controls.js';
import { InputState, InputSender, GamepadInput } from './input.js';
import { StreamConnection } from './stream.js';
import { DemoGame } from './demo.js';
import { setHapticsEnabled } from './haptics.js';
import { hdrDisplay, hevcHdrPlayable } from './hdr.js';

const $ = (id) => document.getElementById(id);
const launchedAt = performance.now();
// The hosted preview build (no bridge reachable from it) sets this before loading the app.
const PREVIEW = !!window.DUOPLAY_PREVIEW;
document.body.classList.add('mode-home');

// ------------------------------------------------------------------ settings
const DEFAULTS = {
  picture: 'standard', fill: 'false', height: 1080, fps: 60, bitrate: 0, latency: 1, adaptive: true, smooth: true,
  haptics: true, hideWithPad: true, showStats: false,
  ctl: { cover: { scale: 1, opacity: 0.6 }, inner: { scale: 1, opacity: 0.55 } },
};
const settings = loadSettings();
function loadSettings() {
  try {
    const s = JSON.parse(localStorage.getItem('duoplay.settings') || '{}');
    return { ...DEFAULTS, ...s, ctl: { cover: { ...DEFAULTS.ctl.cover, ...s.ctl?.cover }, inner: { ...DEFAULTS.ctl.inner, ...s.ctl?.inner } } };
  } catch { return structuredClone(DEFAULTS); }
}
function saveSettings() { try { localStorage.setItem('duoplay.settings', JSON.stringify(settings)); } catch { /* private mode */ } }
setHapticsEnabled(settings.haptics);

const HDR_OK = hdrDisplay() && hevcHdrPlayable();
// ?player=media sends the standard picture through the media player as well (fallback for
// networks where WebRTC video misbehaves); ?player=rtp switches back.
try {
  const pm = new URLSearchParams(location.search).get('player');
  if (pm) { settings.mediaPlayer = pm === 'media'; saveSettings(); }
} catch { /* */ }
if (!HDR_OK && settings.picture === 'hdr') settings.picture = 'standard';

// ------------------------------------------------------------------ stage wiring
const stage = $('stage');
const videoBox = $('video-box');
const video = $('video');
const audio = $('audio');
const canvas = $('demo-canvas');
const input = new InputState();
const sender = new InputSender(input);
const controls = new TouchControls($('controls'), input, { onMenu: () => openMenu() });
new GamepadInput(input, {
  onActive: () => { if (settings.hideWithPad) $('controls').classList.add('hidden-by-pad'); },
  onConnect: (gp) => toast(`${gp.id.replace(/\(.*\)/, '').trim() || 'Controller'} connected`, 2200),
});
const foldLine = document.createElement('div');
foldLine.className = 'fold-line';
foldLine.hidden = true;
stage.appendChild(foldLine);

let videoAspect = 16 / 9;
let current = null;
let stageKind = null; // 'demo' | 'stream' | null
let demo = null;

// Safe-area insets differ between the two screens (the Dynamic Island runs down the side).
const probe = document.createElement('div');
probe.style.cssText = 'position:fixed;inset:0;visibility:hidden;pointer-events:none;' +
  'padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)';
document.body.appendChild(probe);
function safeArea() {
  const cs = getComputedStyle(probe);
  return { t: parseFloat(cs.paddingTop) || 0, r: parseFloat(cs.paddingRight) || 0, b: parseFloat(cs.paddingBottom) || 0, l: parseFloat(cs.paddingLeft) || 0 };
}

function relayout() {
  const w = stage.hidden ? innerWidth : stage.clientWidth;
  const h = stage.hidden ? innerHeight : stage.clientHeight;
  if (!w || !h) return;
  const d = refreshDisplay(w, h);
  updateScreenChip(d);
  if (stage.hidden) return;
  const prefs = settings.ctl[d.screen];
  const l = computeLayout({ width: w, height: h, safe: safeArea(), videoAspect, userScale: prefs.scale, display: d,
    fill: String(settings.fill) === 'true' });
  const v = l.video;
  if (video.style.objectFit !== l.fit) video.style.objectFit = l.fit;
  videoBox.style.transform = `translate3d(${v.x}px, ${v.y}px, 0)`;
  videoBox.style.width = `${v.w}px`;
  videoBox.style.height = `${v.h}px`;
  stage.dataset.mode = l.mode;
  stage.dataset.screen = l.screen;
  stage.dataset.orient = l.orient;
  stage.style.setProperty('--ctl-opacity', String(prefs.opacity));
  if ((l.mode === 'handheld' || l.mode === 'tabletop') && d.crease) {
    foldLine.hidden = false;
    foldLine.style.top = `${d.crease.at}px`;
  } else foldLine.hidden = true;
  controls.apply(l);
  if (current && (current.screen !== l.screen || current.orient !== l.orient || current.mode !== l.mode)) {
    sender.neutralNow(); // anything released by controls.apply goes out right away
  }
  current = l;
}

// ResizeObserver callbacks run after layout and before paint, so a fold, unfold or
// rotation is laid out in the same frame the new screen first draws.
// The window resize event comes earlier still (before animation frames), so the new
// arrangement is in place before anything else in that frame runs.
new ResizeObserver(relayout).observe(stage);
new ResizeObserver(relayout).observe(document.documentElement);
window.addEventListener('resize', relayout);
window.visualViewport?.addEventListener('resize', relayout);
window.addEventListener('orientationchange', relayout);
onDisplayChange((d, prev) => {
  if (prev && prev.screen !== d.screen) console.info(`[duoplay] now on the ${d.screen === 'inner' ? 'inside' : 'cover'} screen`);
});
video.addEventListener('resize', () => {
  if (video.videoWidth && video.videoHeight) {
    const a = video.videoWidth / video.videoHeight;
    if (Math.abs(a - videoAspect) > 0.01) { videoAspect = a; relayout(); }
  }
});

function updateScreenChip(d) {
  const chip = $('screen-chip');
  chip.textContent = d.split ? 'Split View' : d.folded ? 'Half folded' : d.screen === 'inner' ? 'Inside screen' : 'Cover screen';
}

let soundBlocked = false;
stage.addEventListener('pointerdown', () => {
  $('controls').classList.remove('hidden-by-pad');
  if (soundBlocked) {
    soundBlocked = false;
    audio.muted = false;
    audio.play().catch(() => { soundBlocked = true; });
  }
}, { capture: true });

// ------------------------------------------------------------------ stage
let wakeLock = null;
async function requestWakeLock() {
  try { if ('wakeLock' in navigator && document.visibilityState === 'visible') wakeLock = await navigator.wakeLock.request('screen'); } catch { /* needs HTTPS */ }
}
document.addEventListener('visibilitychange', () => { if (stageKind && document.visibilityState === 'visible') requestWakeLock(); });

function enterStage(kind) {
  stageKind = kind;
  document.body.classList.replace('mode-home', 'mode-stream');
  $('home').hidden = true;
  stage.hidden = false;
  canvas.hidden = kind !== 'demo';
  video.hidden = kind === 'demo';
  current = null;
  relayout();
  requestWakeLock();
}

function exitStage() {
  stageKind = null;
  stage.hidden = true;
  $('home').hidden = false;
  document.body.classList.replace('mode-stream', 'mode-home');
  controls.releaseAll();
  video.srcObject = null;
  audio.srcObject = null;
  demo?.stop();
  wakeLock?.release?.().catch(() => {});
  wakeLock = null;
  hideOverlay();
  closeSheet();
  relayout();
  refreshState(true);
}

function overlay(title, text = '', { spinner = true, retry = false, pin = false } = {}) {
  $('overlay').hidden = false;
  $('overlay-title').textContent = title;
  $('overlay-text').textContent = text;
  $('overlay-spinner').hidden = !spinner;
  $('overlay-retry').hidden = !retry;
  $('overlay-pin').hidden = !pin;
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

// ------------------------------------------------------------------ demo
function playDemo() {
  enterStage('demo');
  demo = demo || new DemoGame(canvas, input);
  demo.reset();
  demo.start();
  toast(current?.mode === 'handheld' ? 'Half-fold the phone like a laptop: the game sits on top, the pad below.' : 'Fold or unfold the phone any time: the game keeps going.', 4000);
}

// ------------------------------------------------------------------ console stream
let conn = null;
let session = { state: 'idle' };
let pollTimer = null;
let firstFrame = false;
let reconnectToastTimer = null;
let poorSince = 0, lastAdvice = 0, lastStats = null;
let playing = null;

async function play(c) {
  playing = c;
  // Inside the tap: lets Safari start sound later without asking for another touch.
  audio.muted = false;
  audio.play().catch(() => {});
  enterStage('stream');
  firstFrame = false;
  overlay(c.state === 'standby' ? `Waking ${c.nickname}…` : `Connecting to ${c.nickname}…`,
    c.state === 'standby' ? 'This can take up to half a minute.' : '');
  try {
    await api.post('/api/session/start', {
      console_id: c.id, height: Number(settings.height), fps: Number(settings.fps),
      bitrate_kbps: Number(settings.bitrate), hdr: settings.picture === 'hdr' && HDR_OK,
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
    if (stageKind !== 'stream') return;
    try {
      const st = await api.get('/api/state');
      onSession(st.session);
    } catch (e) {
      if (!firstFrame) overlay('Bridge unreachable', e.message, { spinner: true });
    }
    pollTimer = setTimeout(tick, firstFrame ? 3000 : 600);
  };
  tick();
}

function onSession(s) {
  if (!s || stageKind !== 'stream') return;
  session = s;
  const name = playing?.nickname || s.console_name || 'console';
  if (conn) conn.streaming = s.state === 'streaming';
  switch (s.state) {
    case 'waking': overlay(`Waking ${name}…`, 'This can take up to half a minute.'); break;
    case 'connecting': if (!firstFrame) overlay(`Connecting to ${name}…`); startConnection(); break;
    case 'pin':
      overlay('Console passcode', s.message || 'Enter the passcode of the user signed in on the console.', { spinner: false, pin: true });
      startConnection();
      break;
    case 'streaming':
      startConnection();
      if (firstFrame) hideOverlay();
      else overlay(`Connecting to ${name}…`, 'Waiting for the first picture…');
      break;
    case 'error': case 'ended':
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
    video, audio, sender,
    onSession,
    onStatus: (st) => {
      if (st === 'needs-tap') { soundBlocked = true; toast('Touch the screen to turn on sound', 4000); return; }
      clearTimeout(reconnectToastTimer);
      if (st === 'reconnecting' && firstFrame) reconnectToastTimer = setTimeout(() => toast('Reconnecting…', 0), 700);
    },
    onStats: (s) => onStats(s),
  });
  conn.start(settings);
}

video.addEventListener('playing', () => {
  if (stageKind !== 'stream') return;
  firstFrame = true;
  clearTimeout(reconnectToastTimer);
  if ($('toast').textContent === 'Reconnecting…') $('toast').hidden = true;
  if (session.state === 'streaming' || session.state === 'connecting') hideOverlay();
});

function onStats(s) {
  lastStats = s;
  if (s.fps > 0 && !firstFrame) { firstFrame = true; hideOverlay(); }
  if (s.fps > 0 && $('toast').textContent === 'Reconnecting…') $('toast').hidden = true;
  const dot = $('quality-dot');
  if (dot) dot.className = `quality-dot ${s.quality}`;
  const hud = $('hud');
  hud.hidden = !settings.showStats;
  if (settings.showStats) {
    const f = (v, d = 0) => (v == null || Number.isNaN(v) ? '–' : v.toFixed(d));
    hud.textContent = `${s.transport === 'mse' ? 'HDR ' : ''}${s.height || '?'}p ${f(s.fps)}fps · ${f(s.mbps, 1)} Mbps · rtt ${f(s.rttMs)} ms · ` +
      `Wi-Fi queue ${f(s.queueMs)} ms · loss ${f(s.lossPct, 1)}% · buffer ${f(s.bufferMs)} ms`;
  }
  const now = performance.now();
  if (s.quality === 'poor') {
    if (!poorSince) poorSince = now;
    if (now - poorSince > 6000 && now - lastAdvice > 90000) {
      lastAdvice = now;
      toast(`Wi-Fi is struggling (${(s.lossPct ?? 0).toFixed(1)}% loss, ${Math.round(s.queueMs ?? s.rttMs ?? 0)} ms delay). ` +
        'Use the router\'s 6 GHz or 5 GHz network and stay in range, or pick Smooth buffering in Settings.', 8000);
    }
  } else poorSince = 0;
}

async function leave(stopSession) {
  clearTimeout(pollTimer);
  conn?.stop();
  conn = null;
  if (stopSession && stageKind === 'stream') api.post('/api/session/stop').catch(() => {});
  exitStage();
}

$('overlay-leave').onclick = () => leave(true);
$('overlay-retry').onclick = () => { conn?.stop(); conn = null; if (playing) { exitStage(); play(playing); } };
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
function closeSheet() {
  $('sheet').hidden = true;
  const v = $('sheet-body').querySelector('video');
  if (v) { v.pause(); v.removeAttribute('src'); v.load(); }
}
$('sheet').addEventListener('click', (e) => { if (e.target.closest('[data-close-sheet]')) closeSheet(); });

function openMenu() {
  openSheet('tpl-menu', (body) => {
    const s = lastStats;
    body.querySelector('#menu-net').textContent = stageKind === 'stream' && s
      ? `Wi-Fi: ${s.quality} · ${s.mbps?.toFixed(1) ?? '–'} Mbps · ${Math.round(s.rttMs ?? 0)} ms round trip · ${(s.lossPct ?? 0).toFixed(1)}% loss`
      : stageKind === 'demo' ? 'Demo: everything runs on this phone, no network involved.' : '';
    if (stageKind !== 'stream') body.querySelectorAll('[data-console-only]').forEach((el) => { el.hidden = true; });
    body.querySelector('.menu-grid').addEventListener('click', async (e) => {
      const act = e.target.closest('[data-act]')?.dataset.act;
      if (!act) return;
      if (act === 'ps') { input.pulseButton(1 << 15, 150); closeSheet(); }
      if (act === 'stats') { settings.showStats = !settings.showStats; saveSettings(); $('hud').hidden = !settings.showStats || stageKind !== 'stream'; closeSheet(); }
      if (act === 'controls') { $('controls').classList.toggle('hidden-by-pad'); closeSheet(); }
      if (act === 'settings') openSettings();
      if (act === 'rest') { closeSheet(); try { await api.post('/api/session/standby'); } catch (err) { toast(err.message); } }
      if (act === 'leave') { closeSheet(); leave(true); }
    });
  });
}

function openSettings() {
  openSheet('tpl-settings', (body) => {
    const screen = currentDisplay().screen;
    body.querySelector('[data-screen-label]').textContent = screen === 'inner' ? 'inside screen' : 'cover screen';
    if (!HDR_OK) {
      const opt = body.querySelector('[data-set=picture] option[value=hdr]');
      opt.disabled = true;
      opt.textContent = 'HDR: not available on this screen';
    }
    const hint = bridgeHint(lastState?.device);
    body.querySelector('#set-device-hint').textContent = hint;
    body.querySelector('#set-device-hint').hidden = !hint;
    body.querySelector('#set-log').hidden = bridgeless;
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
        if (key.startsWith('ctl') || key === 'fill') relayout();
        if (key === 'haptics') setHapticsEnabled(val);
        if (key === 'showStats') $('hud').hidden = !val || stageKind !== 'stream';
        if (key === 'adaptive') conn?.sendControl({ t: 'settings', adaptive: !!val });
      });
    }
  });
}
$('btn-settings').onclick = openSettings;

function openHdrCheck() {
  openSheet('tpl-hdr', (body) => {
    const v = body.querySelector('#hdr-video');
    const verdict = body.querySelector('#hdr-verdict');
    verdict.textContent = HDR_OK
      ? 'This screen reports HDR, and Safari can play the console\'s HDR format.'
      : hdrDisplay() ? 'This screen reports HDR, but Safari here can\'t play the console\'s HDR format.' : 'This screen doesn\'t report HDR support.';
    v.addEventListener('error', () => {
      v.hidden = true;
      verdict.textContent = 'This browser can\'t play the HDR test clip (10-bit HEVC). Safari on iPhone Duo can.';
    }, { once: true });
    v.play().catch(() => {});
  });
}
$('btn-hdr-check').onclick = openHdrCheck;
$('hdr-status').textContent = HDR_OK
  ? 'Available: pick HDR under Settings → Picture. The console sends HDR when the game and the PS5\'s HDR setting allow it.'
  : hdrDisplay() ? 'This screen supports HDR, but this browser can\'t play the console\'s HDR stream.' : 'Not available on this screen.';

// ------------------------------------------------------------------ home
let stateTimer = null;
let lastState = null;
let bridgeless = false;

async function refreshState(discover = false) {
  clearTimeout(stateTimer);
  if (stageKind) return;
  if (PREVIEW) { // there is no bridge behind the preview's host
    bridgeless = true;
    renderHome(null);
    $('footer-note').textContent = 'Preview build';
    return;
  }
  try {
    const st = await api.get(`/api/state${discover ? '?discover=1' : ''}`);
    lastState = st;
    bridgeless = false;
    tuneForBridge(st.device);
    renderHome(st);
    $('footer-note').textContent = `Bridge ${st.version} · ${st.secure ? 'secure connection' : 'not secure (HTTP)'}`;
  } catch {
    bridgeless = true;
    renderHome(null);
    $('footer-note').textContent = PREVIEW ? 'Preview build' : 'Running without a bridge';
  }
  if (!bridgeless) stateTimer = setTimeout(() => refreshState(), 2000);
}

// A small bridge (a Raspberry Pi Zero W: one core, one 2.4 GHz radio carrying the picture
// twice) streams best at 720p. Applied once, so a choice made in Settings afterwards sticks.
function tuneForBridge(device) {
  if (!device?.recommended_height || settings.deviceTuned === device.model) return;
  if (Number(settings.height) > device.recommended_height) settings.height = String(device.recommended_height);
  settings.deviceTuned = device.model;
  saveSettings();
}

function bridgeHint(device) {
  if (!device?.max_kbps) return '';
  const name = (device.model || 'This bridge').replace(/ Rev [\d.]+$/, '');
  return `${name}: streams best at ${device.recommended_height}p, up to ${Math.round(device.max_kbps / 1000)} Mbps. ` +
    'Higher settings are capped to that.';
}

function renderHome(st) {
  $('card-bridgeless').hidden = !bridgeless;
  $('card-consoles').hidden = bridgeless;
  $('step-psn').hidden = bridgeless || !!st?.psn_account_set;
  $('banner-install').hidden = PREVIEW || isStandalone() || dismissed('banner-install');
  const https = st && !window.isSecureContext && st.https_port;
  $('banner-https').hidden = !https || dismissed('banner-https');
  if (https) $('link-https').href = `https://${location.hostname}:${st.https_port}/`;
  if (!st) return;

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
    const stateText = { ready: 'Ready', standby: 'Rest mode: DuoPlay wakes it', unknown: 'Not found on the network' }[c.state] || c.state;
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
    `<p class="hint">For networks that hide the console from discovery. Find its address on the PS5 under
     <b>Settings → Network → Connection Status → View Connection Status</b>.</p>
     <input id="add-ip" inputmode="decimal" placeholder="192.168.1.50" autocomplete="off">`,
    async (body) => {
      const host = body.querySelector('#add-ip').value.trim();
      if (!host) throw new Error('Enter an address.');
      await api.post('/api/discover', { host });
      toast('Looking for it… it appears in the list once it answers.');
    });
  setTimeout(() => b.querySelector('#add-ip').focus(), 60);
};
$('btn-demo').onclick = playDemo;

async function savePsn(payload) {
  const err = $('psn-error');
  err.hidden = true;
  try { await api.post('/api/psn/account', payload, 30000); toast('Account linked.'); refreshState(true); }
  catch (e) { err.textContent = e.message; err.hidden = false; }
}
$('psn-save').onclick = () => savePsn({ redirect_url: $('psn-redirect').value.trim() });
$('psn-id-save').onclick = () => savePsn({ account_id: $('psn-id').value.trim() });

// Diagnostics handle (handy in Web Inspector, and used by the automated layout checks).
window.duoplay = { get layout() { return current; }, get stats() { return lastStats; }, get conn() { return conn; }, display: currentDisplay, input, controls, relayout, settings };

if (PREVIEW) {
  $('card-bridgeless').querySelector('p').textContent =
    'This is a preview. Safari can\'t open the kind of network connection a PS5 uses, so the full app comes from a small ' +
    'bridge at home, and that copy is the one for your Home Screen. A Raspberry Pi Zero W does the job, set up from this phone:';
  $('card-bridgeless').querySelector('.hint').textContent =
    'Here you can try the demo, the controls on both screens, folding, the half-folded handheld mode, haptics and the HDR check.';
}

// ------------------------------------------------------------------ launch
(async () => {
  relayout();
  await refreshState(true);
  if (!bridgeless) api.get('/api/psn/login-url').then((r) => { $('psn-login').href = r.url; }).catch(() => {});
  // Let the unfold animation finish, then open the app like a native launch.
  const wait = Math.max(0, 650 - (performance.now() - launchedAt));
  setTimeout(() => {
    const l = $('launch');
    l.classList.add('gone');
    setTimeout(() => l.remove(), 500);
  }, wait);
})();
