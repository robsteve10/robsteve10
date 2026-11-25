// Controller state from touch and Bluetooth controllers, and its trip to the bridge.
//
// Wi-Fi strategy: every packet carries the *complete* pad state and goes over an
// unordered, never-retransmitted data channel, so a lost packet is simply replaced by
// the next one instead of holding input back while it is re-sent. Each change is sent
// twice, 8 ms apart (a single Wi-Fi drop then costs nothing), and while anything is held
// the state repeats every 45 ms. The bridge drops any packet older than one it applied.

import { BTN } from './layout.js';

const clamp16 = (v) => Math.max(-32768, Math.min(32767, Math.round(v * 32767)));

export class InputState {
  constructor() {
    this.listeners = new Set();
    this.touchReset(true);
    this.pad = { buttons: 0, l2: 0, r2: 0, lx: 0, ly: 0, rx: 0, ry: 0 };
    this.pulses = 0;
  }

  onChange(fn) { this.listeners.add(fn); return () => this.listeners.delete(fn); }
  changed() { for (const fn of this.listeners) fn(); }

  // ---- touch layer ----
  touchReset(silent = false) {
    this.touch = { buttons: 0, l2: 0, r2: 0, lx: 0, ly: 0, rx: 0, ry: 0 };
    this.contacts = new Map(); // touchpad: id -> {x, y}
    if (!silent) this.changed();
  }
  touchButton(mask, on) {
    const b = on ? this.touch.buttons | mask : this.touch.buttons & ~mask;
    if (b !== this.touch.buttons) { this.touch.buttons = b; this.changed(); }
  }
  touchTrigger(which, v) { if (this.touch[which] !== v) { this.touch[which] = v; this.changed(); } }
  touchStick(side, x, y) {
    this.touch[`${side}x`] = x; this.touch[`${side}y`] = y;
    this.changed();
  }
  touchpadContact(id, pos) {
    if (pos) this.contacts.set(id, pos); else this.contacts.delete(id);
    this.changed();
  }
  pulseButton(mask, ms) {
    this.pulses |= mask; this.changed();
    setTimeout(() => { this.pulses &= ~mask; this.changed(); }, ms);
  }

  // ---- physical controller layer ----
  setPad(p) {
    const o = this.pad;
    if (o.buttons === p.buttons && o.l2 === p.l2 && o.r2 === p.r2 && o.lx === p.lx && o.ly === p.ly && o.rx === p.rx && o.ry === p.ry) return;
    this.pad = p;
    this.changed();
  }

  /** Merged state: buttons OR'ed, triggers max'ed, each stick from whichever source is pushed further. */
  snapshot() {
    const t = this.touch, p = this.pad;
    const pick = (ax, ay, bx, by) => (Math.hypot(ax, ay) >= Math.hypot(bx, by) ? [ax, ay] : [bx, by]);
    const [lx, ly] = pick(t.lx, t.ly, p.lx, p.ly);
    const [rx, ry] = pick(t.rx, t.ry, p.rx, p.ry);
    const contacts = [...this.contacts.entries()].slice(0, 2);
    return {
      buttons: t.buttons | p.buttons | this.pulses,
      l2: Math.max(t.l2, p.l2), r2: Math.max(t.r2, p.r2),
      lx, ly, rx, ry, contacts,
    };
  }

  isNeutral(s = this.snapshot()) {
    return !s.buttons && !s.l2 && !s.r2 && !s.lx && !s.ly && !s.rx && !s.ry && !s.contacts.length;
  }
}

export function encodeState(s, seq) {
  const buf = new ArrayBuffer(30);
  const v = new DataView(buf);
  v.setUint8(0, 1);
  v.setUint8(1, 0);
  v.setUint16(2, seq & 0xffff, true);
  v.setUint32(4, s.buttons >>> 0, true);
  v.setUint8(8, s.l2);
  v.setUint8(9, s.r2);
  v.setInt16(10, clamp16(s.lx), true);
  v.setInt16(12, clamp16(s.ly), true);
  v.setInt16(14, clamp16(s.rx), true);
  v.setInt16(16, clamp16(s.ry), true);
  for (let i = 0; i < 2; i++) {
    const off = 18 + i * 6;
    const c = s.contacts[i];
    v.setInt8(off, c ? c[0] : -1);
    v.setUint16(off + 2, c ? c[1].x : 0, true);
    v.setUint16(off + 4, c ? c[1].y : 0, true);
  }
  return buf;
}

export class InputSender {
  constructor(state) {
    this.state = state;
    this.channel = null;
    this.seq = 0;
    this.pending = false;
    this.lastSend = 0;
    this.repeatTimer = 0;
    this.sent = 0;
    state.onChange(() => this.schedule());
    // Repeats heal lost packets while something is held; the slow heartbeat tells the
    // bridge this page is alive (it pauses the stream to a phone that went quiet).
    this.timer = setInterval(() => {
      const gap = performance.now() - this.lastSend;
      if ((!this.state.isNeutral() && gap >= 45) || gap >= 400) this.flush();
    }, 25);
  }

  attach(channel) { this.channel = channel; this.flush(); }
  detach() { this.channel = null; }

  schedule() {
    // Coalesce everything that changes within one event (e.g. two thumbs) into one packet.
    if (this.pending) return;
    this.pending = true;
    queueMicrotask(() => {
      this.pending = false;
      this.flush();
      // The same snapshot again shortly after: one lost Wi-Fi frame then costs nothing.
      clearTimeout(this.repeatTimer);
      this.repeatTimer = setTimeout(() => { if (performance.now() - this.lastSend >= 6) this.flush(); }, 8);
    });
  }

  flush() {
    const ch = this.channel;
    if (!ch || ch.readyState !== 'open') return;
    // Never queue input behind a congested link: stale input is worse than none.
    if (ch.bufferedAmount > 2048) return;
    try {
      ch.send(encodeState(this.state.snapshot(), this.seq++));
      this.lastSend = performance.now();
      this.sent++;
    } catch { /* channel closing */ }
  }

  /** Send the current state right now, e.g. "nothing pressed" after a screen change. */
  neutralNow() { this.flush(); }
}

// ---- Bluetooth controllers (DualSense, DualShock 4, Xbox, MFi) ----------------------
// Safari exposes them through the Gamepad API on a secure (HTTPS) page once a button
// has been pressed. (Safari on iPhone has no controller rumble API, so console rumble
// can't be passed through.)

const STD = [ // "standard" mapping index -> PS button
  BTN.CROSS, BTN.CIRCLE, BTN.SQUARE, BTN.TRIANGLE, BTN.L1, BTN.R1, 0, 0,
  BTN.CREATE, BTN.OPTIONS, BTN.L3, BTN.R3, BTN.UP, BTN.DOWN, BTN.LEFT, BTN.RIGHT, BTN.PS, BTN.TOUCHPAD,
];

export class GamepadInput {
  constructor(state, { onActive, onConnect } = {}) {
    this.state = state;
    this.onActive = onActive;
    this.onConnect = onConnect;
    this.running = false;
    this.lastActive = 0;
    window.addEventListener('gamepadconnected', (e) => { this.start(); this.onConnect?.(e.gamepad); });
    window.addEventListener('gamepaddisconnected', () => { if (!this.pads().length) this.stop(); });
    if (this.pads().length) this.start();
  }

  pads() {
    try { return [...(navigator.getGamepads?.() || [])].filter(Boolean); } catch { return []; }
  }

  start() {
    if (this.running) return;
    this.running = true;
    const tick = () => {
      if (!this.running) return;
      this.poll();
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }

  stop() {
    this.running = false;
    this.state.setPad({ buttons: 0, l2: 0, r2: 0, lx: 0, ly: 0, rx: 0, ry: 0 });
  }

  poll() {
    let buttons = 0, l2 = 0, r2 = 0, lx = 0, ly = 0, rx = 0, ry = 0;
    const dz = (x, y) => {
      const m = Math.hypot(x, y);
      if (m < 0.1) return [0, 0];
      const k = Math.min(1, (m - 0.1) / 0.9) / m;
      return [x * k, y * k];
    };
    for (const gp of this.pads()) {
      const b = gp.buttons;
      for (let i = 0; i < STD.length && i < b.length; i++) if (STD[i] && b[i].pressed) buttons |= STD[i];
      if (b[6]) l2 = Math.max(l2, Math.round(b[6].value * 255));
      if (b[7]) r2 = Math.max(r2, Math.round(b[7].value * 255));
      const a = gp.axes;
      if (a.length >= 4) {
        const [x1, y1] = dz(a[0], a[1]);
        const [x2, y2] = dz(a[2], a[3]);
        if (Math.hypot(x1, y1) > Math.hypot(lx, ly)) { lx = x1; ly = y1; }
        if (Math.hypot(x2, y2) > Math.hypot(rx, ry)) { rx = x2; ry = y2; }
      }
    }
    if (buttons || l2 || r2 || lx || ly || rx || ry) {
      const now = performance.now();
      if (now - this.lastActive > 1000) this.onActive?.();
      this.lastActive = now;
    }
    this.state.setPad({ buttons, l2, r2, lx, ly, rx, ry });
  }
}
