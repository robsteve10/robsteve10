// Layout engine for the iPhone Duo.
//
// Arrangements (all computed from the live window, so Split View or a Safari tab with
// toolbars works too):
//   cover · portrait     picture on top, the whole controller below it
//   cover · landscape    picture across the top, controls at the sides (overlaid)
//   inside · portrait    "handheld": picture on the top half, controller on the bottom
//                        half, nothing on the fold. Half-fold it like a laptop and it
//                        becomes a pocket console.
//   inside · landscape   picture across the top, controls in the corners; the small
//                        centre buttons step aside so nothing sits on the crease
//   Split View           compact version of the cover layout
//
// Controls keep one physical size on both screens (points are the same physical size
// on both panels) and only shrink when a window is too small to fit them.
//
// Everything here is pure arithmetic so it can run inside a ResizeObserver callback,
// before the browser paints the first frame on the new screen.

export const BTN = {
  CROSS: 1 << 0, CIRCLE: 1 << 1, SQUARE: 1 << 2, TRIANGLE: 1 << 3,
  LEFT: 1 << 4, RIGHT: 1 << 5, UP: 1 << 6, DOWN: 1 << 7,
  L1: 1 << 8, R1: 1 << 9, L3: 1 << 10, R3: 1 << 11,
  OPTIONS: 1 << 12, CREATE: 1 << 13, TOUCHPAD: 1 << 14, PS: 1 << 15,
};

// Base sizes in points at scale 1.
const S = { stick: 128, dpad: 126, faces: 140, shW: 60, shH: 38, small: 40, tpW: 104, tpH: 36, gap: 14, margin: 14 };
const CREASE_CLEARANCE = 14; // extra points kept free on each side of the fold

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

/**
 * @param {object} p
 * @param {number} p.width, p.height   window size (CSS px = points)
 * @param {{t,r,b,l}} p.safe            safe-area insets
 * @param {number} p.videoAspect        stream width / height
 * @param {number} p.userScale          size preference for this screen
 * @param {object} p.display            from display.js
 */
export function computeLayout(p) {
  const { width: W, height: H, safe, videoAspect: va = 16 / 9, userScale = 1, display: d, fill = false } = p;
  // Two rectangles: the whole screen, which the picture may use edge to edge (the
  // status bar and Dynamic Island just draw over it, as in any full-screen video app),
  // and the safe area, which the controls stay inside so no thumb lands under them.
  const F = { x: 0, y: 0, w: W, h: H };
  const R = { x: safe.l, y: safe.t, w: Math.max(1, W - safe.l - safe.r), h: Math.max(1, H - safe.t - safe.b) };
  const base = { screen: d.screen, orient: d.orient, split: d.split, crease: d.crease, fit: 'contain' };

  // Handheld: inside screen in portrait (flat or half-folded like a laptop).
  const stackedFold = d.crease && d.crease.axis === 'y';
  if (stackedFold) {
    const c = d.crease;
    const gapHalf = c.half + CREASE_CLEARANCE;
    const top = { x: F.x, y: F.y, w: F.w, h: c.at - gapHalf };
    const bottom = { x: R.x, y: c.at + gapHalf, w: R.w, h: R.y + R.h - (c.at + gapHalf) };
    if (top.h > 120 && bottom.h > 240) {
      const video = fit(top, va, 'center');
      const items = layoutPanel(bottom, userScale);
      return finish({ ...base, mode: d.folded ? 'tabletop' : 'handheld', video, items, scale: items._s });
    }
  }

  if (d.orient === 'portrait') {
    const video = { x: F.x, y: F.y, w: F.w, h: F.w / va };
    const panelTop = Math.max(R.y, video.h);
    const panel = { x: R.x, y: panelTop, w: R.w, h: R.y + R.h - panelTop };
    if (panel.h >= 230) {
      const items = layoutPanel(panel, userScale);
      // Spare height goes mostly between picture and pad, so the picture sits close
      // to where the eyes already are instead of stranded at the top edge.
      const top = Math.min(...Object.values(items).filter((r) => r && r.w).map((r) => r.y));
      const spare = top - (video.y + video.h) - S.gap * items._s;
      if (spare > 0) video.y += spare * 0.45;
      return finish({ ...base, mode: 'stacked', video, items, scale: items._s });
    }
  }

  // Landscape (or a portrait window too short to stack).
  let s = userScale;
  const needH = S.shH + Math.max(S.dpad, S.faces) + S.stick + S.gap * 2.2;
  s = Math.max(0.55, Math.min(s, (R.h - 2 * S.margin) / needH));
  const vw = Math.min(F.w, F.h * va);
  const gutter = (F.w - vw) / 2;
  const clusterW = Math.max(S.dpad, S.stick, S.faces) * s + 2 * S.margin;
  if (gutter - Math.max(safe.l, safe.r) >= clusterW && !fill) {
    const video = { x: gutter, y: (F.h - vw / va) / 2, w: vw, h: vw / va };
    const left = { x: R.x, y: R.y, w: gutter - R.x, h: R.h };
    const right = { x: W - gutter, y: R.y, w: R.x + R.w - (W - gutter), h: R.h };
    return finish({ ...base, mode: 'split', video, items: layoutSides(R, left, right, s, true, d.crease), scale: s });
  }
  const sideW = Math.min(R.w * 0.36, (S.dpad / 2 + S.stick * 0.78 + S.stick / 2) * s + S.margin * 2);
  const left = { x: R.x, y: R.y, w: sideW, h: R.h };
  const right = { x: R.x + R.w - sideW, y: R.y, w: sideW, h: R.h };
  const items = layoutSides(R, left, right, s, false, d.crease);
  // "Fill screen": the picture covers every pixel, cropping its edges to fit the shape.
  if (fill) return finish({ ...base, mode: 'overlay', fit: 'cover', video: { ...F }, items, scale: s });
  // Taller than 16:9 (both Duo screens are): picture at the top, sticks in the band below.
  const tall = F.w / F.h < va;
  const video = fit(F, va, tall ? 'top' : 'center');
  return finish({ ...base, mode: 'overlay', video, items, scale: s });
}

function fit(rect, va, anchor) {
  let w = rect.w, h = rect.w / va;
  if (h > rect.h) { h = rect.h; w = h * va; }
  const x = rect.x + (rect.w - w) / 2;
  const y = anchor === 'top' ? rect.y : rect.y + (rect.h - h) / 2;
  return { x, y, w, h };
}

const sq = (cx, cy, d) => ({ x: cx - d / 2, y: cy - d / 2, w: d, h: d });
const box = (cx, cy, w, h) => ({ x: cx - w / 2, y: cy - h / 2, w, h });

// The upper cluster may tuck down beside the stick only once they are far enough
// apart sideways that their touch areas can't collide.
function diagonalOverlap(dx, upper, stick, gap) {
  const clear = (upper + stick) / 2 + gap * 0.5;
  return dx >= clear ? Math.min(stick * 0.35, dx - clear + stick * 0.2) : 0;
}

// Landscape: one cluster per side, sticks low where thumbs rest, d-pad / face buttons
// above and outside them, shoulders above those.
function layoutSides(R, left, right, s, vertical, crease) {
  const m = S.margin, g = S.gap * s;
  const st = S.stick * s, dp = S.dpad * s, fc = S.faces * s;
  const items = {};

  const stickCy = R.y + R.h - m - st / 2;
  let lStickCx, rStickCx, dpadCx, facesCx, upperCy;
  if (vertical) {
    lStickCx = dpadCx = left.x + left.w / 2;
    rStickCx = facesCx = right.x + right.w / 2;
    upperCy = stickCy - st / 2 - g - Math.max(dp, fc) / 2;
  } else {
    dpadCx = left.x + m + dp / 2;
    facesCx = right.x + right.w - m - fc / 2;
    const dx = Math.min(st * 0.78, Math.max(0, left.w - m * 2 - dp / 2 - st / 2));
    lStickCx = dpadCx + dx;
    rStickCx = facesCx - dx;
    upperCy = stickCy - st / 2 - g - Math.max(dp, fc) / 2 + diagonalOverlap(dx, Math.max(dp, fc), st, g);
  }
  upperCy = Math.max(upperCy, R.y + m + S.shH * s + g + Math.max(dp, fc) / 2);

  items.lstick = sq(lStickCx, stickCy, st);
  items.rstick = sq(rStickCx, stickCy, st);
  items.dpad = sq(dpadCx, upperCy, dp);
  items.faces = sq(facesCx, upperCy, fc);

  const shY = Math.max(R.y + m, upperCy - Math.max(dp, fc) / 2 - g - S.shH * s);
  const shw = S.shW * s, shh = S.shH * s;
  const lOuter = Math.min(left.x + m, dpadCx - dp / 2);
  items.l2 = { x: lOuter, y: shY, w: shw, h: shh };
  items.l1 = { x: lOuter + shw + 6 * s, y: shY, w: shw, h: shh };
  const rOuter = Math.max(right.x + right.w - m, facesCx + fc / 2);
  items.r2 = { x: rOuter - shw, y: shY, w: shw, h: shh };
  items.r1 = { x: rOuter - 2 * shw - 6 * s, y: shY, w: shw, h: shh };

  // Rarely used buttons along the top, small and out of the action.
  const sm = S.small * s * 0.9;
  const tpW = S.tpW * s, tpH = S.tpH * s * 0.85;
  const rowCy = R.y + m * 0.5 + S.tpH * s * 0.5;
  const row2Cy = rowCy + S.tpH * s * 0.5 + 6 * s + sm / 2;
  if (crease && crease.axis === 'x') {
    // The fold runs down the middle: build the group on either side of it.
    const lEdge = crease.at - crease.half - CREASE_CLEARANCE;
    const rEdge = crease.at + crease.half + CREASE_CLEARANCE;
    items.touchpad = box(lEdge - tpW / 2, rowCy, tpW, tpH);
    items.create = sq(lEdge - tpW - 8 * s - sm / 2, rowCy, sm);
    items.options = sq(rEdge + sm / 2, rowCy, sm);
    items.ps = sq(rEdge + sm * 1.5 + 8 * s, rowCy, sm);
    items.menu = sq(rEdge + sm * 2.5 + 16 * s, rowCy, sm);
  } else {
    const cx = R.x + R.w / 2;
    items.touchpad = box(cx, rowCy, tpW, tpH);
    items.create = sq(cx - tpW / 2 - 8 * s - sm / 2, rowCy, sm);
    items.options = sq(cx + tpW / 2 + 8 * s + sm / 2, rowCy, sm);
    items.ps = sq(cx - sm * 0.65, row2Cy, sm);
    items.menu = sq(cx + sm * 0.65, row2Cy, sm);
  }
  return items;
}

// A handheld controller inside a rectangle (below the picture, or the lower half of
// the inside screen): anchored to the bottom edge, spreading sideways as width allows.
function layoutPanel(P, userScale) {
  const m = S.margin;
  const upperMax = Math.max(S.dpad, S.faces);
  const needRow = S.small + S.shH + upperMax + S.stick + S.gap * 3.2; // centre buttons on their own row
  const needInline = S.shH + upperMax + S.stick + S.gap * 2.6;        // centre buttons between the shoulders
  const fitW = (P.w - m * 2) / (S.dpad + S.faces + S.small + 24);
  // Room between L1 and R1 for a slim touchpad flanked by Create / Options?
  const inlineFits = (k) => P.w - 2 * (m + 2 * S.shW * k + 16 * k) >= 64 * k + 2 * (S.small * 0.85 * k + 6 * k);
  let s = Math.min(userScale, (P.h - m * 2) / needRow, fitW);
  const sInline = Math.min(userScale, (P.h - m * 2) / needInline, fitW);
  if (inlineFits(sInline)) s = sInline;
  s = Math.max(s, 0.5);
  const inline = inlineFits(s);
  const g = S.gap * s, st = S.stick * s, dp = S.dpad * s, fc = S.faces * s;
  const shw = S.shW * s, shh = S.shH * s, sm = S.small * s;
  const items = {};

  const half = P.w / 2;
  const sideAvail = half - m - sm / 2 - 6 * s; // keep a centre lane for PS / menu
  const dx = clamp(sideAvail - dp / 2 - st / 2, 0, st * 0.8);
  const dpadCx = P.x + m + dp / 2;
  const facesCx = P.x + P.w - m - fc / 2;
  const stickCy = P.y + P.h - m - st / 2;
  const upperCy = stickCy - st / 2 - g - Math.max(dp, fc) / 2 + diagonalOverlap(dx, Math.max(dp, fc), st, g);

  items.lstick = sq(dpadCx + dx, stickCy, st);
  items.rstick = sq(facesCx - dx, stickCy, st);
  items.dpad = sq(dpadCx, upperCy, dp);
  items.faces = sq(facesCx, upperCy, fc);

  const shY = upperCy - Math.max(dp, fc) / 2 - g - shh;
  items.l2 = { x: P.x + m, y: shY, w: shw, h: shh };
  items.l1 = { x: P.x + m + shw + 6 * s, y: shY, w: shw, h: shh };
  items.r2 = { x: P.x + P.w - m - shw, y: shY, w: shw, h: shh };
  items.r1 = { x: P.x + P.w - m - 2 * shw - 6 * s, y: shY, w: shw, h: shh };

  // Create / touchpad / options between the shoulder pairs when they fit (narrowing
  // the touchpad if need be), else on a row of their own above.
  const cx = P.x + P.w / 2;
  const between = (items.r1.x - 10 * s) - (items.l1.x + shw + 10 * s);
  if (inline) {
    const csm = Math.min(sm, S.small * 0.85 * s + (between > S.tpW * s + 2 * (sm + 8 * s) ? sm : 0));
    const gap = 6 * s;
    const tpW = Math.min(S.tpW * s, between - 2 * (csm + gap));
    const rowCy = shY + shh / 2;
    items.touchpad = box(cx, rowCy, tpW, Math.min(S.tpH * s, shh));
    items.create = sq(cx - tpW / 2 - gap - csm / 2, rowCy, csm);
    items.options = sq(cx + tpW / 2 + gap + csm / 2, rowCy, csm);
  } else {
    const rowCy = shY - g * 0.6 - S.tpH * s / 2;
    items.touchpad = box(cx, rowCy, S.tpW * s, S.tpH * s);
    items.create = sq(cx - S.tpW * s / 2 - 8 * s - sm / 2, rowCy, sm);
    items.options = sq(cx + S.tpW * s / 2 + 8 * s + sm / 2, rowCy, sm);
  }
  // PS between the sticks like on the pad; the app menu just above it.
  items.ps = sq(cx, stickCy + st * 0.12, sm);
  items.menu = sq(cx, stickCy - st * 0.32, sm * 0.9);
  Object.defineProperty(items, '_s', { value: s, enumerable: false });
  return items;
}

function finish(l) {
  const dpr = (typeof devicePixelRatio === 'number' && devicePixelRatio) || 1;
  const r = (v) => Math.round(v * dpr) / dpr;
  const rr = (o) => ({ x: r(o.x), y: r(o.y), w: r(o.w), h: r(o.h) });
  l.video = rr(l.video);
  const items = {};
  for (const k of Object.keys(l.items)) items[k] = rr(l.items[k]);
  l.items = items;
  return l;
}
