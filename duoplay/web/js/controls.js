// On-screen controller.
//
// All touches are tracked by one handler on the controls layer and hit-tested against
// the geometry the layout engine computed (no DOM reads while playing). That makes
// multi-touch dependable and lets a thumb roll across face buttons or the d-pad the
// way it would on a real pad.

import { BTN } from './layout.js';

const GLYPH = {
  triangle: '<svg viewBox="0 0 24 24"><path d="M12 4.5 20.5 19h-17Z"/></svg>',
  circle: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="7.5"/></svg>',
  cross: '<svg viewBox="0 0 24 24"><path d="m5.5 5.5 13 13m0-13-13 13"/></svg>',
  square: '<svg viewBox="0 0 24 24"><rect x="5" y="5" width="14" height="14" rx="1"/></svg>',
  menu: '<svg viewBox="0 0 24 24"><path d="M5 12h.01M12 12h.01M19 12h.01" stroke-width="4" stroke-linecap="round"/></svg>',
  options: '<svg viewBox="0 0 24 24"><path d="M5 7h14M5 12h14M5 17h14"/></svg>',
  create: '<svg viewBox="0 0 24 24"><path d="M12 4v10M8 10l4 4 4-4M5 19h14"/></svg>',
};

const FACE_BITS = { top: BTN.TRIANGLE, right: BTN.CIRCLE, bottom: BTN.CROSS, left: BTN.SQUARE };
const SIMPLE = {
  l1: { mask: BTN.L1, label: 'L1', cls: 'ctl-shoulder' },
  r1: { mask: BTN.R1, label: 'R1', cls: 'ctl-shoulder' },
  l2: { trigger: 'l2', label: 'L2', cls: 'ctl-shoulder' },
  r2: { trigger: 'r2', label: 'R2', cls: 'ctl-shoulder' },
  create: { mask: BTN.CREATE, glyph: 'create', cls: 'ctl-pill' },
  options: { mask: BTN.OPTIONS, glyph: 'options', cls: 'ctl-pill' },
  ps: { mask: BTN.PS, label: 'PS', cls: 'ctl-pill' },
  menu: { app: 'menu', glyph: 'menu', cls: 'ctl-pill ctl-menu' },
};

const TOUCHPAD_MAX_X = 1919, TOUCHPAD_MAX_Y = 1079;

export class TouchControls {
  constructor(container, input, { onMenu } = {}) {
    this.el = container;
    this.input = input;
    this.onMenu = onMenu;
    this.geom = null;
    this.els = {};
    this.pointers = new Map();
    this.lastStickTap = { l: 0, r: 0 };
    this.touchIdNext = 0;
    this.build();
    this.bind();
  }

  build() {
    const add = (id, cls, html) => {
      const d = document.createElement('div');
      d.className = `ctl ${cls}`;
      d.dataset.id = id;
      d.innerHTML = html;
      this.el.appendChild(d);
      this.els[id] = d;
      return d;
    };
    for (const [id, def] of Object.entries(SIMPLE)) {
      const inner = def.glyph ? GLYPH[def.glyph] : `<span class="lbl">${def.label}</span>`;
      add(id, def.cls, `<div class="face">${inner}${id === 'menu' ? '<span class="quality-dot" id="quality-dot"></span>' : ''}</div>`);
    }
    add('touchpad', 'ctl-touchpad', '<div class="face"></div>');
    add('dpad', 'ctl-dpad', '<div class="arm up"></div><div class="arm down"></div><div class="arm left"></div><div class="arm right"></div>');
    add('faces', 'ctl-faces',
      `<div class="btn-face f-top"><div class="face">${GLYPH.triangle}</div></div>` +
      `<div class="btn-face f-right"><div class="face">${GLYPH.circle}</div></div>` +
      `<div class="btn-face f-bottom"><div class="face">${GLYPH.cross}</div></div>` +
      `<div class="btn-face f-left"><div class="face">${GLYPH.square}</div></div>`);
    for (const side of ['l', 'r']) add(`${side}stick`, 'ctl-stick', '<div class="base"></div><div class="knob"></div>');
  }

  /** Position everything for a new layout. Called inside ResizeObserver, before paint. */
  apply(layout) {
    const structural = !this.geom || this.geom.mode !== layout.mode || this.geom.screen !== layout.screen ||
      this.geom.orient !== layout.orient;
    const moved = !this.geom || Object.keys(layout.items).some((k) => {
      const a = layout.items[k], b = this.geom.items[k];
      return !b || Math.abs(a.x - b.x) > 2 || Math.abs(a.y - b.y) > 2 || Math.abs(a.w - b.w) > 2;
    });
    // Fingers that were down when the phone was folded/unfolded/rotated now point at
    // different controls (or the screen went dark under them): let go of everything
    // rather than leave a button stuck.
    if (structural || moved) this.releaseAll();
    this.geom = layout;
    for (const [id, r] of Object.entries(layout.items)) {
      const el = this.els[id];
      if (!el) continue;
      el.style.transform = `translate3d(${r.x}px, ${r.y}px, 0)`;
      el.style.width = `${r.w}px`;
      el.style.height = `${r.h}px`;
    }
  }

  setVisible(on) { this.el.style.display = on ? '' : 'none'; if (!on) this.releaseAll(); }

  bind() {
    const opts = { passive: false };
    this.el.addEventListener('pointerdown', (e) => this.down(e), opts);
    this.el.addEventListener('pointermove', (e) => this.move(e), opts);
    this.el.addEventListener('pointerup', (e) => this.up(e), opts);
    this.el.addEventListener('pointercancel', (e) => this.up(e, true), opts);
    this.el.addEventListener('lostpointercapture', (e) => this.up(e, true), opts);
    // Stop iOS from scrolling, zooming, selecting or showing callouts under the thumbs.
    for (const t of ['touchstart', 'touchmove', 'touchend', 'gesturestart', 'gesturechange', 'contextmenu', 'dblclick'])
      this.el.addEventListener(t, (e) => { if (e.cancelable) e.preventDefault(); }, opts);
  }

  hit(x, y) {
    const it = this.geom?.items;
    if (!it) return null;
    const inRect = (r, pad = 0) => x >= r.x - pad && x <= r.x + r.w + pad && y >= r.y - pad && y <= r.y + r.h + pad;
    for (const id of ['menu', 'ps', 'create', 'options', 'l1', 'l2', 'r1', 'r2']) {
      if (it[id] && inRect(it[id], 6)) return { kind: 'btn', id };
    }
    if (inRect(it.touchpad, 4)) return { kind: 'touchpad' };
    if (inRect(it.dpad, it.dpad.w * 0.12)) return { kind: 'dpad' };
    if (inRect(it.faces, it.faces.w * 0.12)) return { kind: 'faces' };
    for (const side of ['l', 'r']) {
      const r = it[`${side}stick`];
      const cx = r.x + r.w / 2, cy = r.y + r.h / 2;
      const d = Math.hypot(x - cx, y - cy);
      if (d <= r.w * 0.85) return { kind: 'stick', side, floating: d > r.w * 0.5 };
    }
    return null;
  }

  down(e) {
    if (e.cancelable) e.preventDefault();
    const target = this.hit(e.clientX, e.clientY);
    if (!target) return;
    try { this.el.setPointerCapture(e.pointerId); } catch { /* ignore */ }
    const p = { ...target, x: e.clientX, y: e.clientY, t0: performance.now(), x0: e.clientX, y0: e.clientY };
    this.pointers.set(e.pointerId, p);

    if (p.kind === 'btn') {
      const def = SIMPLE[p.id];
      this.els[p.id].classList.add('on');
      if (def.mask) this.input.touchButton(def.mask, true);
      if (def.trigger) this.input.touchTrigger(def.trigger, 255);
    } else if (p.kind === 'stick') {
      const r = this.geom.items[`${p.side}stick`];
      p.ox = p.floating ? e.clientX : r.x + r.w / 2;
      p.oy = p.floating ? e.clientY : r.y + r.h / 2;
      p.radius = r.w * 0.42;
      const el = this.els[`${p.side}stick`];
      el.classList.add('on');
      const base = el.querySelector('.base');
      base.style.transform = `translate(${p.ox - (r.x + r.w / 2)}px, ${p.oy - (r.y + r.h / 2)}px)`;
      // double tap = click the stick (L3/R3) for as long as this touch is held
      if (performance.now() - this.lastStickTap[p.side] < 320) {
        p.click = p.side === 'l' ? BTN.L3 : BTN.R3;
        this.input.touchButton(p.click, true);
      }
      this.updateStick(p, e.clientX, e.clientY);
    } else if (p.kind === 'touchpad') {
      p.touchId = this.touchIdNext;
      this.touchIdNext = (this.touchIdNext + 1) & 0x7f;
      this.els.touchpad.classList.add('on');
      this.updateTouchpad(p, e.clientX, e.clientY);
    } else {
      this.updateCluster(p, e.clientX, e.clientY);
    }
  }

  move(e) {
    const p = this.pointers.get(e.pointerId);
    if (!p) return;
    if (e.cancelable) e.preventDefault();
    // Coalesced events give every intermediate position the screen sampled (120 Hz panels).
    const evs = e.getCoalescedEvents ? e.getCoalescedEvents() : null;
    const last = evs && evs.length ? evs[evs.length - 1] : e;
    p.x = last.clientX; p.y = last.clientY;
    if (p.kind === 'stick') this.updateStick(p, p.x, p.y);
    else if (p.kind === 'dpad' || p.kind === 'faces') this.updateCluster(p, p.x, p.y);
    else if (p.kind === 'touchpad') this.updateTouchpad(p, p.x, p.y);
    else if (p.kind === 'btn') {
      // sliding off a shoulder button releases it, sliding back presses it again
      const r = this.geom.items[p.id];
      const inside = p.x >= r.x - 16 && p.x <= r.x + r.w + 16 && p.y >= r.y - 16 && p.y <= r.y + r.h + 16;
      if (inside !== !p.off) {
        p.off = !inside;
        const def = SIMPLE[p.id];
        this.els[p.id].classList.toggle('on', inside);
        if (def.mask) this.input.touchButton(def.mask, inside);
        if (def.trigger) this.input.touchTrigger(def.trigger, inside ? 255 : 0);
      }
    }
  }

  up(e, cancelled = false) {
    const p = this.pointers.get(e.pointerId);
    if (!p) return;
    this.pointers.delete(e.pointerId);
    this.release(p, cancelled);
  }

  release(p, cancelled) {
    const quick = performance.now() - p.t0 < 220 && Math.hypot(p.x - p.x0, p.y - p.y0) < 12;
    if (p.kind === 'btn') {
      const def = SIMPLE[p.id];
      this.els[p.id].classList.remove('on');
      if (def.mask) this.input.touchButton(def.mask, false);
      if (def.trigger) this.input.touchTrigger(def.trigger, 0);
      if (def.app === 'menu' && !cancelled && !p.off) this.onMenu?.();
    } else if (p.kind === 'stick') {
      if (p.click) this.input.touchButton(p.click, false);
      this.lastStickTap[p.side] = quick && !cancelled ? performance.now() : 0;
      this.input.touchStick(p.side, 0, 0);
      const el = this.els[`${p.side}stick`];
      el.classList.remove('on');
      el.querySelector('.knob').style.transform = '';
      el.querySelector('.base').style.transform = '';
    } else if (p.kind === 'touchpad') {
      this.input.touchpadContact(p.touchId, null);
      this.els.touchpad.classList.remove('on');
      if (quick && !cancelled) this.input.pulseButton(BTN.TOUCHPAD, 90); // a tap clicks the pad
    } else {
      this.setCluster(p, 0);
    }
  }

  releaseAll() {
    for (const p of this.pointers.values()) this.release(p, true);
    this.pointers.clear();
    this.input.touchReset();
    for (const el of Object.values(this.els)) {
      el.classList.remove('on');
      el.querySelectorAll('.on').forEach((x) => x.classList.remove('on'));
    }
  }

  updateStick(p, x, y) {
    let dx = (x - p.ox) / p.radius, dy = (y - p.oy) / p.radius;
    const mag = Math.hypot(dx, dy);
    if (mag > 1) { dx /= mag; dy /= mag; }
    // small radial dead zone, rescaled so the stick still reaches full range
    const m2 = Math.min(1, mag);
    const dz = 0.07;
    const k = m2 < dz ? 0 : (m2 - dz) / (1 - dz) / (m2 || 1);
    this.input.touchStick(p.side, dx * k, dy * k);
    const knob = this.els[`${p.side}stick`].querySelector('.knob');
    const r = this.geom.items[`${p.side}stick`];
    const offX = p.ox - (r.x + r.w / 2), offY = p.oy - (r.y + r.h / 2);
    knob.style.transform = `translate(${offX + dx * p.radius}px, ${offY + dy * p.radius}px)`;
  }

  updateTouchpad(p, x, y) {
    const r = this.geom.items.touchpad;
    const tx = Math.round(Math.min(1, Math.max(0, (x - r.x) / r.w)) * TOUCHPAD_MAX_X);
    const ty = Math.round(Math.min(1, Math.max(0, (y - r.y) / r.h)) * TOUCHPAD_MAX_Y);
    this.input.touchpadContact(p.touchId, { x: tx, y: ty });
  }

  updateCluster(p, x, y) {
    const r = this.geom.items[p.kind];
    const cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    const dx = (x - cx) / (r.w / 2), dy = (y - cy) / (r.h / 2);
    let mask = 0;
    if (p.kind === 'dpad') {
      const mag = Math.hypot(dx, dy);
      if (mag > 0.18) {
        // 8-way: diagonals get a 45° wedge each, like a real d-pad rocking on its pivot
        const a = Math.atan2(dy, dx) * 180 / Math.PI; // 0 = right, 90 = down
        const sector = Math.round(((a + 360) % 360) / 45) % 8;
        mask = [BTN.RIGHT, BTN.RIGHT | BTN.DOWN, BTN.DOWN, BTN.DOWN | BTN.LEFT, BTN.LEFT, BTN.LEFT | BTN.UP, BTN.UP, BTN.UP | BTN.RIGHT][sector];
      }
    } else {
      // Face buttons: press whichever are under the thumb; landing between two presses both.
      const pos = { top: [0, -0.64], right: [0.64, 0], bottom: [0, 0.64], left: [-0.64, 0] };
      let best = null, bestD = 9;
      for (const [k, [bx, by]] of Object.entries(pos)) {
        const d = Math.hypot(dx - bx, dy - by);
        if (d < 0.42) mask |= FACE_BITS[k];
        if (d < bestD) { bestD = d; best = k; }
      }
      if (!mask && bestD < 0.75) mask = FACE_BITS[best];
    }
    this.setCluster(p, mask);
  }

  setCluster(p, mask) {
    const prev = p.mask || 0;
    if (prev === mask) return;
    p.mask = mask;
    const off = prev & ~mask, on = mask & ~prev;
    if (off) this.input.touchButton(off, false);
    if (on) this.input.touchButton(on, true);
    const el = this.els[p.kind];
    if (p.kind === 'dpad') {
      el.querySelector('.up').classList.toggle('on', !!(mask & BTN.UP));
      el.querySelector('.down').classList.toggle('on', !!(mask & BTN.DOWN));
      el.querySelector('.left').classList.toggle('on', !!(mask & BTN.LEFT));
      el.querySelector('.right').classList.toggle('on', !!(mask & BTN.RIGHT));
    } else {
      el.querySelector('.f-top .face').classList.toggle('on', !!(mask & BTN.TRIANGLE));
      el.querySelector('.f-right .face').classList.toggle('on', !!(mask & BTN.CIRCLE));
      el.querySelector('.f-bottom .face').classList.toggle('on', !!(mask & BTN.CROSS));
      el.querySelector('.f-left .face').classList.toggle('on', !!(mask & BTN.SQUARE));
    }
  }
}
