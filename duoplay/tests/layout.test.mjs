// Layout checks for every iPhone Duo screen state. Pure Node, no browser:
//   node --test tests/
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { computeLayout } from '../web/js/layout.js';

globalThis.devicePixelRatio = 3;

// Points: cover 466 x 678, inside 669 x 951 (Apple: 1398x2034 @3x and 1878x2670).
// Safari-tab sizes from WebKit's Safari 27.1 notes. Safe areas are guesses covering
// the vertical Dynamic Island on either side.
const SAFE0 = { t: 0, r: 0, b: 0, l: 0 };
const SAFE_SIDE = { t: 0, r: 44, b: 16, l: 0 };
const SAFE_TOP = { t: 44, r: 0, b: 20, l: 0 };
const CASES = [
  ['cover portrait (home screen app)', 466, 678, SAFE_SIDE, { screen: 'cover', split: false }, 'stacked'],
  ['cover portrait, island on top', 466, 678, SAFE_TOP, { screen: 'cover', split: false }, 'stacked'],
  ['cover portrait (Safari tab)', 382, 580, SAFE0, { screen: 'cover', split: false }, 'stacked'],
  ['cover landscape (home screen app)', 678, 466, { t: 0, r: 0, b: 16, l: 44 }, { screen: 'cover', split: false }, 'overlay'],
  ['cover landscape (Safari tab)', 594, 368, SAFE0, { screen: 'cover', split: false }, 'overlay'],
  ['inside portrait (home screen app)', 669, 951, SAFE_TOP, { screen: 'inner', split: false, crease: { axis: 'y', at: 475.5, half: 10 } }, 'handheld'],
  ['inside portrait (Safari tab)', 669, 867, SAFE0, { screen: 'inner', split: false, crease: { axis: 'y', at: 426.5, half: 10 } }, 'handheld'],
  ['inside landscape (home screen app)', 951, 669, { t: 0, r: 44, b: 20, l: 0 }, { screen: 'inner', split: false, crease: { axis: 'x', at: 475.5, half: 10 } }, 'overlay'],
  ['inside landscape (Safari tab)', 951, 585, SAFE0, { screen: 'inner', split: false, crease: { axis: 'x', at: 475.5, half: 10 } }, 'overlay'],
  ['Split View half', 385, 609, SAFE0, { screen: 'inner', split: true }, 'stacked'],
  ['laptop fold (posture API)', 669, 951, SAFE_TOP, { screen: 'inner', split: false, folded: true, foldAxis: 'horizontal', crease: { axis: 'y', at: 475.5, half: 12 } }, 'tabletop'],
];

const inter = (a, b) => {
  const ix = Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x);
  const iy = Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y);
  return ix > 1 && iy > 1;
};

for (const [name, w, h, safe, disp, mode] of CASES) {
  test(name, () => {
    const display = { orient: w >= h ? 'landscape' : 'portrait', folded: false, crease: null, ...disp };
    const l = computeLayout({ width: w, height: h, safe, videoAspect: 16 / 9, userScale: 1, display });
    assert.equal(l.mode, mode, 'arrangement');
    const R = { x: safe.l, y: safe.t, w: w - safe.l - safe.r, h: h - safe.t - safe.b };
    const inside = (r) => r.x >= R.x - 0.5 && r.y >= R.y - 0.5 && r.x + r.w <= R.x + R.w + 0.5 && r.y + r.h <= R.y + R.h + 0.5;
    assert.ok(inside(l.video), `picture inside the safe area ${JSON.stringify(l.video)}`);
    const names = Object.keys(l.items);
    for (const k of names) assert.ok(inside(l.items[k]), `${k} inside the safe area ${JSON.stringify(l.items[k])}`);
    for (let i = 0; i < names.length; i++)
      for (let j = i + 1; j < names.length; j++)
        assert.ok(!inter(l.items[names[i]], l.items[names[j]]), `${names[i]} overlaps ${names[j]}`);
    if (mode === 'stacked' || mode === 'handheld' || mode === 'tabletop')
      for (const k of names) assert.ok(!inter(l.items[k], l.video), `${k} covers the picture`);
    if (display.crease) {
      const c = display.crease;
      const band = c.axis === 'x' ? { x: c.at - c.half, y: 0, w: c.half * 2, h } : { x: 0, y: c.at - c.half, w, h: c.half * 2 };
      for (const k of names) assert.ok(!inter(l.items[k], band), `${k} sits on the fold`);
    }
    // Controls stay thumb-sized wherever the screen allows it.
    if (!display.split && Math.min(R.w, R.h) >= 360) assert.ok(l.scale >= 0.85, `controls shrunk to ${l.scale.toFixed(2)}`);
    // The picture gets a decent share of the screen.
    assert.ok(l.video.w >= Math.min(R.w, 300), `picture too small (${l.video.w})`);
  });
}
