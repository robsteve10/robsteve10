// What the iPhone Duo is doing right now: which screen DuoPlay is on, whether it
// shares that screen in Split View, and (where Safari exposes it) how far it's folded.
//
// The two screens are the same shape (about 1.43:1: the 5.4" cover is 466 x 678 pt,
// the 7.6" inside screen 669 x 951 pt), so shape can't tell them apart. Size can:
// the cover's short side is under 500 pt, the inside screen's is over 650 pt.
//
// Safari 27.1 also has the Device Posture and Viewport Segments APIs on the Duo,
// as a developer preview behind Settings > Apps > Safari > Advanced > Feature Flags.
// When they're on we use them to tell a half-folded "laptop" posture from a flat
// screen; when they're off everything still works from size and orientation alone.

const COVER_MAX_SHORT_SIDE = 560; // pt; cover 466, inside 669
const SPLIT_FRACTION = 0.8;       // window narrower than 80% of the screen = sharing it

let last = null;
const listeners = new Set();

export function onDisplayChange(fn) { listeners.add(fn); return () => listeners.delete(fn); }

function screenSize() {
  // iOS reports the current display's size in points, in portrait, regardless of rotation.
  const w = Math.round(window.screen?.width || 0), h = Math.round(window.screen?.height || 0);
  return w > 0 && h > 0 ? { short: Math.min(w, h), long: Math.max(w, h) } : null;
}

export function isStandalone() {
  try {
    return navigator.standalone === true || matchMedia('(display-mode: standalone), (display-mode: fullscreen)').matches;
  } catch { return false; }
}

function segments() {
  try {
    const s = window.viewport?.segments;
    if (!s || s.length < 2) return null;
    return [...s].map((r) => ({ x: r.x ?? r.left, y: r.y ?? r.top, w: r.width, h: r.height }));
  } catch { return null; }
}

function posture() {
  try { return navigator.devicePosture?.type || null; } catch { return null; }
}

/**
 * @param {number} vw, vh  the page's viewport (CSS px)
 * @returns {{screen:'cover'|'inner', orient:'portrait'|'landscape', split:boolean,
 *   folded:boolean, foldAxis:'horizontal'|'vertical'|null, segments:Array|null,
 *   crease:{axis:'x'|'y', at:number, half:number}|null, postureKnown:boolean, dpr:number}}
 */
export function readDisplay(vw = innerWidth, vh = innerHeight) {
  let sc = screenSize();
  const winShort = Math.min(vw, vh), winLong = Math.max(vw, vh);
  // A window can't be bigger than its screen: if it is, the reported screen size is stale
  // (left over from the other display) and the window is the better guide.
  if (sc && (winShort > sc.short + 4 || winLong > sc.long + 4)) sc = null;
  const short = sc && sc.short >= winShort * 0.5 ? sc.short : winShort;
  const screen = short < COVER_MAX_SHORT_SIDE ? 'cover' : 'inner';
  const orient = vw >= vh ? 'landscape' : 'portrait';

  // Split View: the window covers clearly less of the screen than the screen offers.
  let split = false;
  if (sc && screen === 'inner') {
    const along = orient === 'landscape' ? sc.long : sc.short;
    split = vw < along * SPLIT_FRACTION;
  }

  const p = posture();
  const segs = segments();
  const folded = p === 'folded' || (!!segs && segs.length === 2);
  let foldAxis = null;
  if (segs && segs.length === 2) foldAxis = Math.abs(segs[0].x - segs[1].x) < 2 ? 'horizontal' : 'vertical';

  // Where the fold runs across our window when the inside screen is flat. It always
  // halves the long side: a vertical line in landscape, a horizontal one in portrait.
  // In Split View each app gets one half, so the fold is the divider, not in our window.
  let crease = null;
  if (screen === 'inner' && !split) {
    if (segs && segs.length === 2) {
      crease = foldAxis === 'horizontal'
        ? { axis: 'y', at: (segs[0].y + segs[0].h + segs[1].y) / 2, half: Math.max(8, (segs[1].y - segs[0].y - segs[0].h) / 2) }
        : { axis: 'x', at: (segs[0].x + segs[0].w + segs[1].x) / 2, half: Math.max(8, (segs[1].x - segs[0].x - segs[0].w) / 2) };
    } else if (orient === 'landscape') {
      crease = { axis: 'x', at: vw / 2, half: 10 };
    } else {
      // The fold is at the middle of the *screen*. In a Safari tab the inside screen's
      // tab bar sits above the page, so shift by whatever the page doesn't get.
      let at = vh / 2;
      if (sc && !isStandalone() && vh < sc.long - 4) at = sc.long / 2 - (sc.long - vh);
      crease = { axis: 'y', at, half: 10 };
    }
  }

  return {
    screen, orient, split, folded, foldAxis, segments: segs, crease,
    postureKnown: p !== null || segs !== null,
    dpr: window.devicePixelRatio || 1,
    winShort, winLong,
  };
}

/** Re-read and notify listeners when something meaningful changed. */
export function refreshDisplay(vw, vh) {
  const d = readDisplay(vw, vh);
  const changed = !last || last.screen !== d.screen || last.orient !== d.orient || last.split !== d.split ||
    last.folded !== d.folded || last.foldAxis !== d.foldAxis;
  const prev = last;
  last = d;
  if (changed) for (const fn of listeners) fn(d, prev);
  return d;
}

export function currentDisplay() { return last || readDisplay(); }

// Posture changes don't always come with a resize (folding to 90° keeps the viewport).
try { navigator.devicePosture?.addEventListener?.('change', () => refreshDisplay()); } catch { /* not available */ }
