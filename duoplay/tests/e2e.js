// End-to-end test against a running bridge started with --test-pattern.
//
//   duoplay-bridge --test-pattern pattern.h264 &
//   node tests/e2e.js [http://localhost:8080] [screenshot-dir]
//
// Plays the test stream in a phone-sized headless Chromium, then folds, unfolds and
// rotates the "phone" mid-stream and checks that:
//   - each screen gets the expected arrangement, laid out in the same frame,
//   - the stream never reconnects or stalls because of a screen switch,
//   - controller input reaches the bridge, and a button held during a fold is released,
//   - a dropped connection is reattached without restarting the console session.

const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

const BASE = process.argv[2] || 'http://localhost:8080';
const SHOTS = process.argv[3] || null;

// Representative viewports (CSS px). The layout engine classifies by shape, not by
// these exact numbers, so any cover/inner sizes behave the same way.
const SCREENS = {
  coverPortrait: { width: 393, height: 852 },
  coverLandscape: { width: 852, height: 393 },
  innerPortrait: { width: 744, height: 940 },
  innerLandscape: { width: 940, height: 744 },
};
const EXPECT = {
  coverPortrait: { screen: 'cover', mode: 'stacked' },
  coverLandscape: { screen: 'cover', mode: 'overlay' },
  innerPortrait: { screen: 'inner', mode: 'stacked' },
  innerLandscape: { screen: 'inner', mode: 'overlay' },
};

let failures = 0;
function check(ok, msg) {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${msg}`);
  if (!ok) failures++;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function bridgeState() {
  const r = await fetch(`${BASE}/api/state`);
  return r.json();
}

(async () => {
  const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
  const context = await browser.newContext({
    viewport: SCREENS.coverPortrait, hasTouch: true, isMobile: true, deviceScaleFactor: SHOTS ? 2 : 1,
  });
  const page = await context.newPage();
  const peerRequests = [];
  const sessionStarts = [];
  page.on('request', (req) => {
    if (req.url().endsWith('/api/peer') && req.method() === 'POST') peerRequests.push(Date.now());
    if (req.url().endsWith('/api/session/start')) sessionStarts.push(Date.now());
  });
  page.on('console', (m) => { if (m.type() === 'error') console.log('  [page]', m.text()); });

  await page.goto(BASE);
  await page.getByText('Test pattern (no console)').waitFor({ timeout: 10000 });
  if (SHOTS) await page.screenshot({ path: path.join(SHOTS, 'setup.png') });
  const t0 = Date.now();
  await page.locator('.console', { hasText: 'Test pattern' }).getByRole('button', { name: 'Play' }).click();

  // --- first picture ---
  await page.waitForFunction(() => {
    const v = document.getElementById('video');
    return v.videoWidth > 0 && v.currentTime > 0.2;
  }, null, { timeout: 15000 });
  check(true, `first picture after ${Date.now() - t0} ms`);
  await page.waitForFunction(() => document.getElementById('overlay').hidden, null, { timeout: 5000 }).catch(() => {});
  check(await page.evaluate(() => document.getElementById('overlay').hidden), 'connecting overlay is gone once the picture arrives');

  const peersBefore = peerRequests.length;
  const startsBefore = sessionStarts.length;

  // --- screen switching mid-stream ---
  const order = ['coverPortrait', 'innerPortrait', 'innerLandscape', 'coverLandscape', 'coverPortrait', 'innerLandscape'];
  for (const name of order) {
    const before = await page.evaluate(() => document.getElementById('video').currentTime);
    await page.setViewportSize(SCREENS[name]);
    // Same-frame check: by the next animation frame the stage must already be laid out
    // for the new screen (ResizeObserver runs before paint).
    const laid = await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(() => {
      const st = document.getElementById('stage');
      const box = document.getElementById('video-box').getBoundingClientRect();
      resolve({ screen: st.dataset.screen, mode: st.dataset.mode, w: innerWidth, h: innerHeight,
        box: { x: box.x, y: box.y, w: box.width, h: box.height } });
    })));
    const exp = EXPECT[name];
    check(laid.screen === exp.screen && laid.mode === exp.mode,
      `${name}: ${laid.w}x${laid.h} -> ${laid.screen}/${laid.mode} in first frame (want ${exp.screen}/${exp.mode})`);
    check(laid.box.x >= -1 && laid.box.y >= -1 && laid.box.x + laid.box.w <= laid.w + 1 && laid.box.y + laid.box.h <= laid.h + 1,
      `${name}: picture fits the screen (${Math.round(laid.box.w)}x${Math.round(laid.box.h)} at ${Math.round(laid.box.x)},${Math.round(laid.box.y)})`);

    // Controls must stay on screen and must not overlap each other.
    const items = await page.evaluate(() => window.duoplay.layout.items);
    const W = laid.w, H = laid.h;
    const off = Object.entries(items).filter(([, r]) => r.x < -1 || r.y < -1 || r.x + r.w > W + 1 || r.y + r.h > H + 1).map(([k]) => k);
    check(!off.length, `${name}: all controls on screen${off.length ? ` (off: ${off.join(', ')})` : ''}`);
    const names = Object.keys(items);
    const overlaps = [];
    for (let i = 0; i < names.length; i++) for (let j = i + 1; j < names.length; j++) {
      const a = items[names[i]], b = items[names[j]];
      const ix = Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x);
      const iy = Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y);
      if (ix > 2 && iy > 2) overlaps.push(`${names[i]}/${names[j]}`);
    }
    check(!overlaps.length, `${name}: no controls overlap${overlaps.length ? ` (${overlaps.join(', ')})` : ''}`);
    if (exp.mode === 'stacked') {
      const vb = laid.box;
      const covered = Object.entries(items).filter(([, r]) => r.y < vb.y + vb.h - 1).map(([k]) => k);
      check(!covered.length, `${name}: no control covers the picture${covered.length ? ` (${covered.join(', ')})` : ''}`);
    }

    await sleep(700);
    const after = await page.evaluate(() => document.getElementById('video').currentTime);
    check(after - before > 0.4, `${name}: video kept playing through the switch (+${(after - before).toFixed(2)} s)`);
    if (SHOTS) await page.screenshot({ path: path.join(SHOTS, `${name}.png`) });
  }
  check(peerRequests.length === peersBefore, `no reconnection caused by screen switches (${peerRequests.length - peersBefore} new connections)`);
  check(sessionStarts.length === startsBefore, 'console session never restarted by screen switches');

  // --- input reaches the bridge ---
  const cdp = await context.newCDPSession(page);
  const centre = async (id, sub) => page.evaluate(([id, sub]) => {
    const r = window.duoplay.layout.items[id];
    if (sub === 'bottom') return { x: r.x + r.w / 2, y: r.y + r.h * 0.82 };
    return { x: r.x + r.w / 2, y: r.y + r.h / 2 };
  }, [id, sub]);
  const touch = (type, pts) => cdp.send('Input.dispatchTouchEvent', { type, touchPoints: pts });

  await page.setViewportSize(SCREENS.coverLandscape);
  await sleep(300);
  const cross = await centre('faces', 'bottom');
  await touch('touchStart', [{ x: cross.x, y: cross.y, id: 1 }]);
  await sleep(250);
  let st = await bridgeState();
  check((st.session.source?.input?.buttons & 1) === 1, `holding Cross reaches the bridge (buttons=${st.session.source?.input?.buttons})`);

  // left stick pushed right while Cross is held
  const ls = await centre('lstick');
  await touch('touchStart', [{ x: cross.x, y: cross.y, id: 1 }, { x: ls.x, y: ls.y, id: 2 }]);
  await touch('touchMove', [{ x: cross.x, y: cross.y, id: 1 }, { x: ls.x + 80, y: ls.y, id: 2 }]);
  await sleep(250);
  st = await bridgeState();
  const inp = st.session.source?.input || {};
  check(inp.lx > 20000 && Math.abs(inp.ly) < 4000 && (inp.buttons & 1) === 1,
    `multi-touch: Cross + left stick right (lx=${inp.lx}, ly=${inp.ly}, buttons=${inp.buttons})`);

  // --- the phone is unfolded while both are held: nothing may stay stuck ---
  await page.setViewportSize(SCREENS.innerLandscape);
  await sleep(250);
  st = await bridgeState();
  const after = st.session.source?.input || {};
  check(after.buttons === 0 && after.lx === 0 && after.ly === 0, `everything released when the screen switched under the thumbs (buttons=${after.buttons}, lx=${after.lx})`);
  await touch('touchEnd', []);

  // d-pad diagonal
  const dp = await page.evaluate(() => { const r = window.duoplay.layout.items.dpad; return { x: r.x + r.w * 0.85, y: r.y + r.h * 0.15 }; });
  await touch('touchStart', [{ x: dp.x, y: dp.y, id: 3 }]);
  await sleep(200);
  st = await bridgeState();
  const UP = 1 << 6, RIGHT = 1 << 5;
  check((st.session.source.input.buttons & (UP | RIGHT)) === (UP | RIGHT), `d-pad up-right diagonal (buttons=${st.session.source.input.buttons})`);
  await touch('touchEnd', []);
  await sleep(200);
  st = await bridgeState();
  check(st.session.source.input.buttons === 0, 'release reaches the bridge');

  // --- connection drop: reattach without restarting the console session ---
  const tDrop = Date.now();
  await page.evaluate(() => window.duoplay.conn.pc.close()); // simulate the link dying
  await page.evaluate(() => { window.__framesAtDrop = window.duoplay.conn.framesSeen; window.duoplay.conn.reattach('test: link dropped'); });
  // a new MediaStream restarts currentTime, so count rendered frames instead
  await page.waitForFunction(() => window.duoplay.conn.framesSeen > window.__framesAtDrop + 5, null, { timeout: 10000, polling: 16 });
  check(true, `picture back ${Date.now() - tDrop} ms after the connection dropped`);
  check(sessionStarts.length === startsBefore, 'console session survived the reconnect');
  await sleep(1500);
  st = await bridgeState();
  check(st.session.media.viewers === 1, `old connection cleaned up on the bridge (viewers=${st.session.media.viewers})`);

  // --- stats ---
  await sleep(1500);
  const stats = await page.evaluate(() => window.duoplay.stats);
  check(stats && stats.fps > 40, `stats: ${stats?.fps?.toFixed(0)} fps, ${stats?.mbps?.toFixed(1)} Mbps, rtt ${stats?.rttMs?.toFixed(1)} ms, loss ${stats?.lossPct?.toFixed(2)}%, quality ${stats?.quality}`);

  if (SHOTS) {
    await page.evaluate(() => { window.duoplay.settings.showStats = true; });
    await sleep(1200);
    await page.screenshot({ path: path.join(SHOTS, 'stats.png') });
  }

  await page.evaluate(() => fetch('/api/session/stop', { method: 'POST' }));
  await browser.close();
  console.log(failures ? `\n${failures} check(s) failed` : '\nAll checks passed');
  process.exit(failures ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });
