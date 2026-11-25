// Fold-aware layout engine.
//
// The same page runs on the cover screen (a normal tall phone shape) and the inside
// screen (close to square). Rather than hard-coding either screen's size, every
// layout is derived from the live viewport, so it is right for any shape, either
// orientation, and the half-folded "tabletop" posture when the browser reports a hinge.
//
// Principles:
//  * Controls keep the same *physical* size on both screens: thumbs don't grow when
//    the phone unfolds. Sizes are CSS px (≈ constant physical size on iPhone), only
//    shrunk when a screen is too small to fit them.
//  * When there is room beside or below the picture, controls go there and cover
//    nothing; they only overlay the picture when there is no other choice.
//  * The computation is pure and cheap, so it runs inside a ResizeObserver callback,
//    i.e. after layout but before paint: the new arrangement appears in the very
//    first frame drawn on the new screen. Nothing is re-created; the <video> and the
//    WebRTC connection never notice.

export const BTN = {
  CROSS: 1 << 0, CIRCLE: 1 << 1, SQUARE: 1 << 2, TRIANGLE: 1 << 3,
  LEFT: 1 << 4, RIGHT: 1 << 5, UP: 1 << 6, DOWN: 1 << 7,
  L1: 1 << 8, R1: 1 << 9, L3: 1 << 10, R3: 1 << 11,
  OPTIONS: 1 << 12, CREATE: 1 << 13, TOUCHPAD: 1 << 14, PS: 1 << 15,
};

// Base sizes in CSS px at scale 1.
const S = { stick: 128, dpad: 126, faces: 140, shW: 60, shH: 38, small: 40, tpW: 104, tpH: 36, gap: 14, margin: 14 };

// Long/short side ratio below which a screen is "inner" (tablet-like). Phone-shaped
// screens are ~2.0-2.4, folding phones' inside screens ~1.1-1.45.
const INNER_MAX_ASPECT = 1.6;

export function classify(w, h) {
  const aspect = Math.max(w, h) / Math.max(1, Math.min(w, h));
  return {
    screen: aspect < INNER_MAX_ASPECT ? 'inner' : 'cover',
    orient: w >= h ? 'landscape' : 'portrait',
    aspect,
  };
}

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

/**
 * @param {object} p
 * @param {number} p.width, p.height     stage size (CSS px)
 * @param {{t,r,b,l}} p.safe             safe-area insets
 * @param {number} p.videoAspect         stream width / height (16:9 for the PS5)
 * @param {number} p.userScale           per-screen size preference
 * @param {Array<DOMRect>|null} p.segments viewport segments when the browser reports a fold
 * @returns layout: { screen, orient, mode, scale, video:{x,y,w,h}, items:{...} }
 */
export function computeLayout(p) {
  const { width: W0, height: H0, safe, videoAspect: va = 16 / 9, userScale = 1 } = p;
  const cls = classify(W0, H0);
  const R = { x: safe.l, y: safe.t, w: Math.max(1, W0 - safe.l - safe.r), h: Math.max(1, H0 - safe.t - safe.b) };

  // Half-folded with the hinge across the middle: picture on the top half, pad on the bottom half.
  const segs = p.segments;
  if (segs && segs.length === 2 && Math.abs(segs[0].left - segs[1].left) < 2 && segs[1].top > segs[0].top) {
    const top = segs[0], bottom = segs[1];
    const vrect = { x: R.x, y: Math.max(R.y, top.top), w: R.w, h: top.bottom - Math.max(R.y, top.top) };
    const video = fit(vrect, va, 'center');
    const panel = { x: R.x, y: bottom.top, w: R.w, h: R.y + R.h - bottom.top };
    const s = userScale * clamp(Math.min(panel.w, panel.h) / 380, 0.75, 1.1);
    return finish({ ...cls, mode: 'tabletop', video, items: layoutPanel(panel, s), scale: s });
  }

  if (cls.orient === 'portrait') {
    const video = { x: R.x, y: R.y, w: R.w, h: R.w / va };
    const panel = { x: R.x, y: R.y + video.h, w: R.w, h: R.h - video.h };
    if (panel.h >= 220) {
      const s = userScale * clamp(R.w / 390, 0.78, 1.12);
      const items = layoutPanel(panel, s);
      // Spare height goes above the picture (a little) and between picture and pad
      // (mostly), so the picture sits near the thumbs' line of sight instead of
      // stranded at the top with a dead gap under it.
      const top = Math.min(...Object.values(items).map((r) => r.y));
      const spare = top - (video.y + video.h) - S.gap * s;
      if (spare > 0) video.y += spare * 0.45;
      return finish({ ...cls, mode: 'stacked', video, items, scale: s });
    }
    // Very short portrait window (e.g. a split-screen sliver): fall through to overlay.
  }

  // Landscape (or a portrait window too short to stack).
  let s = userScale * clamp(Math.min(R.w, R.h) / 380, 0.78, 1.12);
  const needH = (S.shH + S.dpad + S.stick + S.gap * 2.2) + (2 * S.margin) / s;
  s = Math.min(s, R.h / needH);
  s = Math.max(s, 0.55);

  const vw = Math.min(R.w, R.h * va);
  const gutter = (R.w - vw) / 2;
  const clusterW = Math.max(S.dpad, S.stick, S.faces) * s + 2 * S.margin;
  if (gutter >= clusterW) {
    const video = { x: R.x + gutter, y: R.y + (R.h - vw / va) / 2, w: vw, h: vw / va };
    const left = { x: R.x, y: R.y, w: gutter, h: R.h };
    const right = { x: R.x + R.w - gutter, y: R.y, w: gutter, h: R.h };
    return finish({ ...cls, mode: 'split', video, items: layoutSides(R, left, right, s, true), scale: s });
  }

  // Overlay. If the screen is taller than 16:9 (the inside screen), park the picture at
  // the top so the spare band at the bottom takes the sticks and covers less of the game.
  const tall = R.w / R.h < va;
  const video = fit(R, va, tall ? 'top' : 'center');
  const sideW = Math.min(R.w * 0.36, (S.dpad / 2 + S.stick * 0.75 + S.stick / 2) * s + S.margin * 2);
  const left = { x: R.x, y: R.y, w: sideW, h: R.h };
  const right = { x: R.x + R.w - sideW, y: R.y, w: sideW, h: R.h };
  return finish({ ...cls, mode: 'overlay', video, items: layoutSides(R, left, right, s, false), scale: s });
}

// The upper button cluster may tuck down beside the stick only once the two are far
// enough apart sideways that their touch areas can't collide.
function diagonalOverlap(dx, upper, stick, gap) {
  const clear = (upper + stick) / 2 + gap * 0.5;
  return dx >= clear ? Math.min(stick * 0.35, dx - clear + stick * 0.2) : 0;
}

function fit(rect, va, anchor) {
  let w = rect.w, h = rect.w / va;
  if (h > rect.h) { h = rect.h; w = h * va; }
  const x = rect.x + (rect.w - w) / 2;
  const y = anchor === 'top' ? rect.y : rect.y + (rect.h - h) / 2;
  return { x, y, w, h };
}

function sq(cx, cy, d) { return { x: cx - d / 2, y: cy - d / 2, w: d, h: d }; }
function box(cx, cy, w, h) { return { x: cx - w / 2, y: cy - h / 2, w, h }; }

// Landscape: one cluster per side. Sticks sit at the bottom where thumbs rest, the
// d-pad / face buttons above-outside them (DualSense geometry), shoulders above those.
// Clusters are anchored to the bottom, so on the tall inside screen nothing drifts
// up out of thumb reach.
function layoutSides(R, left, right, s, vertical) {
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

  // Shoulders: one row just above the upper buttons (never above the safe top).
  const shY = Math.max(R.y + m, upperCy - Math.max(dp, fc) / 2 - g - S.shH * s);
  const shw = S.shW * s, shh = S.shH * s;
  const lOuter = Math.min(left.x + m, dpadCx - dp / 2);
  items.l2 = { x: lOuter, y: shY, w: shw, h: shh };
  items.l1 = { x: lOuter + shw + 6 * s, y: shY, w: shw, h: shh };
  const rOuter = Math.max(right.x + right.w - m, facesCx + fc / 2);
  items.r2 = { x: rOuter - shw, y: shY, w: shw, h: shh };
  items.r1 = { x: rOuter - 2 * shw - 6 * s, y: shY, w: shw, h: shh };

  // Rarely used buttons along the top centre, small, out of the action.
  const cx = R.x + R.w / 2, top = R.y + m * 0.5;
  const sm = S.small * s * 0.9;
  items.touchpad = box(cx, top + S.tpH * s * 0.5, S.tpW * s, S.tpH * s * 0.85);
  items.create = sq(cx - S.tpW * s / 2 - 8 * s - sm / 2, top + S.tpH * s * 0.5, sm);
  items.options = sq(cx + S.tpW * s / 2 + 8 * s + sm / 2, top + S.tpH * s * 0.5, sm);
  items.ps = sq(cx - sm * 0.65, top + S.tpH * s + 6 * s + sm / 2, sm);
  items.menu = sq(cx + sm * 0.65, top + S.tpH * s + 6 * s + sm / 2, sm);
  return items;
}

// Below the picture (portrait) or on the lower half (tabletop): a handheld layout
// anchored to the bottom edge, spreading out sideways as far as the width allows.
function layoutPanel(P, s0) {
  const m = S.margin;
  // Fit height: centre row + shoulders + upper buttons + sticks
  const need = S.small + S.shH + S.dpad + S.stick + S.gap * 3.2;
  let s = Math.min(s0, (P.h - m * 2) / need);
  s = Math.max(s, 0.5);
  const g = S.gap * s, st = S.stick * s, dp = S.dpad * s, fc = S.faces * s;
  const shw = S.shW * s, shh = S.shH * s, sm = S.small * s;
  const items = {};

  const half = P.w / 2;
  const sideAvail = half - m - (S.small * s) / 2 - 6 * s; // keep a centre lane for PS/menu
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

  // Create / touchpad / options: between the shoulder pairs if there is room, else a row above.
  const cx = P.x + P.w / 2;
  const centreW = S.tpW * s + 2 * (sm + 8 * s);
  const between = (items.r1.x - 10 * s) - (items.l1.x + shw + 10 * s);
  const rowCy = between >= centreW ? shY + shh / 2 : shY - g * 0.6 - S.tpH * s / 2;
  items.touchpad = box(cx, rowCy, S.tpW * s, S.tpH * s);
  items.create = sq(cx - S.tpW * s / 2 - 8 * s - sm / 2, rowCy, sm);
  items.options = sq(cx + S.tpW * s / 2 + 8 * s + sm / 2, rowCy, sm);
  // PS between the sticks like on the pad; the app menu just above it.
  items.ps = sq(cx, stickCy + st * 0.12, sm);
  items.menu = sq(cx, stickCy - st * 0.32, sm * 0.9);
  return items;
}

function finish(l) {
  // round to device pixels to keep borders crisp
  const dpr = (typeof devicePixelRatio === 'number' && devicePixelRatio) || 1;
  const r = (v) => Math.round(v * dpr) / dpr;
  const rr = (o) => ({ x: r(o.x), y: r(o.y), w: r(o.w), h: r(o.h) });
  l.video = rr(l.video);
  for (const k of Object.keys(l.items)) l.items[k] = rr(l.items[k]);
  return l;
}
