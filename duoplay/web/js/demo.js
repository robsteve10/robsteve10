// Demo stream: a small arena game drawn where the console's picture would be, driven by
// the same touch controls and Bluetooth controllers. It exists so the screen switching,
// controls and haptics can be tried on the phone with no console attached.
//
//   left stick / d-pad  move        right stick  aim        R2  fire
//   Cross  pulse ring   Circle  shield   Square  dash   Triangle  change colours
//   L1 / R1  zoom out / in   touchpad  draw light trails

import { BTN } from './layout.js';

const W = 1600, H = 900; // logical 16:9 canvas
const PALETTES = [
  { bg0: '#061314', bg1: '#0b2a2e', grid: 'rgba(31,184,205,0.10)', hero: '#1fb8cd', beam: '#bfefff', mote: '#ffc44d', ring: '#3ddc97' },
  { bg0: '#120914', bg1: '#2a0f33', grid: 'rgba(214,120,255,0.10)', hero: '#d678ff', beam: '#ffe0ff', mote: '#4dd2ff', ring: '#ff7ab6' },
  { bg0: '#0d0f06', bg1: '#25300c', grid: 'rgba(190,230,80,0.10)', hero: '#bee650', beam: '#f4ffd0', mote: '#ff8a4d', ring: '#4dffc4' },
];

export class DemoGame {
  constructor(canvas, input) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d', { alpha: false });
    this.input = input;
    this.running = false;
    this.pal = 0;
    this.reset();
    this.ro = new ResizeObserver(() => this.resize());
    this.ro.observe(canvas);
  }

  reset() {
    this.hero = { x: W / 2, y: H / 2, vx: 0, vy: 0, aim: 0, shield: 0, dash: 0 };
    this.beams = [];
    this.rings = [];
    this.trails = [];
    this.motes = Array.from({ length: 18 }, () => this.newMote());
    this.score = 0;
    this.zoom = 1;
    this.prevButtons = 0;
    this.fireCooldown = 0;
    this.frameTimes = [];
  }

  newMote() {
    return { x: Math.random() * W, y: Math.random() * H, r: 10 + Math.random() * 16, vx: (Math.random() - 0.5) * 60, vy: (Math.random() - 0.5) * 60, hue: Math.random() };
  }

  resize() {
    const r = this.canvas.getBoundingClientRect();
    const dpr = Math.min(window.devicePixelRatio || 1, 3);
    const w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) { this.canvas.width = w; this.canvas.height = h; }
  }

  start() {
    if (this.running) return;
    this.running = true;
    this.resize();
    let last = performance.now();
    const frame = (t) => {
      if (!this.running) return;
      const dt = Math.min(0.05, (t - last) / 1000);
      this.frameTimes.push(t - last);
      if (this.frameTimes.length > 60) this.frameTimes.shift();
      last = t;
      this.update(dt);
      this.draw();
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }

  stop() { this.running = false; }

  update(dt) {
    const s = this.input.snapshot();
    const pressed = s.buttons & ~this.prevButtons;
    this.prevButtons = s.buttons;
    const h = this.hero;

    let mx = s.lx, my = s.ly;
    if (s.buttons & BTN.LEFT) mx = -1;
    if (s.buttons & BTN.RIGHT) mx = 1;
    if (s.buttons & BTN.UP) my = -1;
    if (s.buttons & BTN.DOWN) my = 1;
    const speed = 620 * (h.dash > 0 ? 2.6 : 1);
    h.vx += (mx * speed - h.vx) * Math.min(1, dt * 10);
    h.vy += (my * speed - h.vy) * Math.min(1, dt * 10);
    h.x = Math.min(W - 30, Math.max(30, h.x + h.vx * dt));
    h.y = Math.min(H - 30, Math.max(30, h.y + h.vy * dt));
    if (Math.hypot(s.rx, s.ry) > 0.25) h.aim = Math.atan2(s.ry, s.rx);
    else if (Math.hypot(mx, my) > 0.25) h.aim = Math.atan2(my, mx);

    if (pressed & BTN.CROSS) this.rings.push({ x: h.x, y: h.y, r: 20, life: 1 });
    if (pressed & BTN.CIRCLE) h.shield = 1.6;
    if (pressed & BTN.SQUARE) h.dash = 0.25;
    if (pressed & BTN.TRIANGLE) this.pal = (this.pal + 1) % PALETTES.length;
    if (s.buttons & BTN.L1) this.zoom = Math.max(0.75, this.zoom - dt);
    if (s.buttons & BTN.R1) this.zoom = Math.min(1.35, this.zoom + dt);
    h.shield = Math.max(0, h.shield - dt);
    h.dash = Math.max(0, h.dash - dt);

    this.fireCooldown -= dt;
    if (s.r2 > 40 && this.fireCooldown <= 0) {
      this.fireCooldown = 0.11 - (s.r2 / 255) * 0.06;
      this.beams.push({ x: h.x, y: h.y, a: h.aim, life: 0.45 });
    }
    for (const [, pos] of s.contacts) {
      this.trails.push({ x: (pos.x / 1919) * W, y: (pos.y / 1079) * H, life: 1.2 });
    }

    for (const b of this.beams) { b.x += Math.cos(b.a) * 1900 * dt; b.y += Math.sin(b.a) * 1900 * dt; b.life -= dt; }
    for (const r of this.rings) { r.r += 900 * dt; r.life -= dt * 1.4; }
    for (const t of this.trails) t.life -= dt;
    for (const m of this.motes) {
      m.x += m.vx * dt; m.y += m.vy * dt;
      if (m.x < -40) m.x = W + 40; if (m.x > W + 40) m.x = -40;
      if (m.y < -40) m.y = H + 40; if (m.y > H + 40) m.y = -40;
      for (const b of this.beams) if (b.life > 0 && Math.hypot(b.x - m.x, b.y - m.y) < m.r + 8) { m.dead = true; b.life = 0; }
      for (const r of this.rings) if (r.life > 0 && Math.abs(Math.hypot(r.x - m.x, r.y - m.y) - r.r) < m.r) m.dead = true;
    }
    for (let i = 0; i < this.motes.length; i++) {
      if (this.motes[i].dead) { this.score++; this.rings.push({ x: this.motes[i].x, y: this.motes[i].y, r: 4, life: 0.5, small: true }); this.motes[i] = this.newMote(); }
    }
    this.beams = this.beams.filter((b) => b.life > 0);
    this.rings = this.rings.filter((r) => r.life > 0);
    this.trails = this.trails.filter((t) => t.life > 0).slice(-400);
    this.snap = s;
  }

  draw() {
    const c = this.ctx, p = PALETTES[this.pal];
    const cw = this.canvas.width, ch = this.canvas.height;
    const k = Math.min(cw / W, ch / H);
    c.setTransform(1, 0, 0, 1, 0, 0);
    const g = c.createLinearGradient(0, 0, cw, ch);
    g.addColorStop(0, p.bg0); g.addColorStop(1, p.bg1);
    c.fillStyle = g;
    c.fillRect(0, 0, cw, ch);

    c.setTransform(k * this.zoom, 0, 0, k * this.zoom, (cw - W * k * this.zoom) / 2, (ch - H * k * this.zoom) / 2);
    const h = this.hero;
    // grid that drifts with the player for a sense of speed
    c.strokeStyle = p.grid; c.lineWidth = 2;
    const ox = -(h.x * 0.15) % 80, oy = -(h.y * 0.15) % 80;
    c.beginPath();
    for (let x = ox - 80; x < W + 80; x += 80) { c.moveTo(x, -80); c.lineTo(x, H + 80); }
    for (let y = oy - 80; y < H + 80; y += 80) { c.moveTo(-80, y); c.lineTo(W + 80, y); }
    c.stroke();

    c.globalCompositeOperation = 'lighter';
    for (const t of this.trails) { c.fillStyle = withAlpha(p.ring, t.life * 0.5); c.beginPath(); c.arc(t.x, t.y, 10, 0, Math.PI * 2); c.fill(); }
    for (const m of this.motes) {
      c.fillStyle = withAlpha(p.mote, 0.25); c.beginPath(); c.arc(m.x, m.y, m.r * 1.8, 0, Math.PI * 2); c.fill();
      c.fillStyle = p.mote; c.beginPath(); c.arc(m.x, m.y, m.r * 0.6, 0, Math.PI * 2); c.fill();
    }
    c.lineCap = 'round';
    for (const b of this.beams) {
      c.strokeStyle = withAlpha(p.beam, Math.min(1, b.life * 3)); c.lineWidth = 7;
      c.beginPath(); c.moveTo(b.x, b.y); c.lineTo(b.x - Math.cos(b.a) * 90, b.y - Math.sin(b.a) * 90); c.stroke();
    }
    for (const r of this.rings) {
      c.strokeStyle = withAlpha(r.small ? p.mote : p.ring, r.life); c.lineWidth = r.small ? 4 : 10;
      c.beginPath(); c.arc(r.x, r.y, r.r, 0, Math.PI * 2); c.stroke();
    }
    c.fillStyle = withAlpha(p.hero, 0.22); c.beginPath(); c.arc(h.x, h.y, 70, 0, Math.PI * 2); c.fill();
    c.fillStyle = p.hero; c.beginPath(); c.arc(h.x, h.y, 26, 0, Math.PI * 2); c.fill();
    c.fillStyle = '#ffffff'; c.beginPath(); c.arc(h.x + Math.cos(h.aim) * 44, h.y + Math.sin(h.aim) * 44, 7, 0, Math.PI * 2); c.fill();
    if (h.shield > 0) { c.strokeStyle = withAlpha(p.beam, Math.min(1, h.shield)); c.lineWidth = 6; c.beginPath(); c.arc(h.x, h.y, 52, 0, Math.PI * 2); c.stroke(); }
    c.globalCompositeOperation = 'source-over';

    // HUD: score, frame pacing and a live view of every input
    c.setTransform(k, 0, 0, k, (cw - W * k) / 2, (ch - H * k) / 2);
    c.fillStyle = 'rgba(0,0,0,0.35)';
    roundRect(c, 24, 24, 330, 78, 18); c.fill();
    c.fillStyle = '#e9f3f2';
    c.font = '600 34px -apple-system, system-ui, sans-serif';
    c.fillText(`${this.score}`, 48, 76);
    c.font = '500 22px -apple-system, system-ui, sans-serif';
    c.fillStyle = 'rgba(233,243,242,0.7)';
    const ft = this.frameTimes.length ? this.frameTimes.reduce((a, b) => a + b, 0) / this.frameTimes.length : 16.7;
    c.fillText(`demo · ${Math.round(1000 / ft)} fps`, 140, 74);
    this.drawPad(c, W - 300, H - 150);
  }

  drawPad(c, x, y) {
    const s = this.snap || { buttons: 0, lx: 0, ly: 0, rx: 0, ry: 0, l2: 0, r2: 0 };
    c.fillStyle = 'rgba(0,0,0,0.35)';
    roundRect(c, x - 20, y - 40, 296, 166, 22); c.fill();
    const stick = (cx, cy, vx, vy) => {
      c.strokeStyle = 'rgba(233,243,242,0.5)'; c.lineWidth = 3; c.beginPath(); c.arc(cx, cy, 34, 0, Math.PI * 2); c.stroke();
      c.fillStyle = '#e9f3f2'; c.beginPath(); c.arc(cx + vx * 22, cy + vy * 22, 12, 0, Math.PI * 2); c.fill();
    };
    stick(x + 40, y + 70, s.lx, s.ly);
    stick(x + 216, y + 70, s.rx, s.ry);
    const dot = (cx, cy, on) => { c.fillStyle = on ? '#1fb8cd' : 'rgba(233,243,242,0.25)'; c.beginPath(); c.arc(cx, cy, 9, 0, Math.PI * 2); c.fill(); };
    dot(x + 128, y + 24, s.buttons & BTN.TRIANGLE); dot(x + 128, y + 76, s.buttons & BTN.CROSS);
    dot(x + 102, y + 50, s.buttons & BTN.SQUARE); dot(x + 154, y + 50, s.buttons & BTN.CIRCLE);
    c.fillStyle = 'rgba(233,243,242,0.25)'; c.fillRect(x, y - 24, 100, 10); c.fillRect(x + 156, y - 24, 100, 10);
    c.fillStyle = '#1fb8cd'; c.fillRect(x, y - 24, (s.l2 / 255) * 100, 10); c.fillRect(x + 156, y - 24, (s.r2 / 255) * 100, 10);
  }
}

function withAlpha(hex, a) {
  const n = parseInt(hex.slice(1), 16);
  return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${Math.max(0, Math.min(1, a)).toFixed(3)})`;
}

function roundRect(c, x, y, w, h, r) {
  c.beginPath();
  c.moveTo(x + r, y); c.arcTo(x + w, y, x + w, y + h, r); c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r); c.arcTo(x, y, x + w, y, r); c.closePath();
}
