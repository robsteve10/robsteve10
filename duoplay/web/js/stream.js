// WebRTC connection to the bridge, built to ride out Wi-Fi.
//
// The console session lives on the bridge, independent of this page. Folding or
// unfolding the phone never touches the connection at all. If the connection does
// drop (Wi-Fi blip, app suspended, roaming between access points), reattaching is
// just a WebRTC handshake on the LAN plus one keyframe, typically well under a second,
// and the game keeps running meanwhile.

import { api } from './api.js';

const CLIENT_ID = (() => {
  try {
    let id = sessionStorage.getItem('duoplay.client');
    if (!id) { id = crypto.getRandomValues(new Uint32Array(2)).join('-'); sessionStorage.setItem('duoplay.client', id); }
    return id;
  } catch { return String(Math.random()).slice(2); }
})();

export class StreamConnection {
  constructor({ video, sender, onSession, onControl, onStatus, onStats }) {
    this.video = video;
    this.sender = sender;
    this.onSession = onSession;
    this.onControl = onControl;
    this.onStatus = onStatus;   // ('connecting'|'live'|'reconnecting'|'failed', detail)
    this.onStats = onStats;
    this.pc = null;
    this.peerId = null;
    this.control = null;
    this.gen = 0;
    this.active = false;
    this.lastFrameAt = 0;
    this.framesSeen = 0;
    this.failures = 0;
    this.prevStats = null;
    this.lastKeyReq = 0;
    this.disconnectTimer = null;
    this.settings = {};
    this.boundVisibility = () => this.onVisibility();
    this.boundOnline = () => this.active && this.reattach('network changed');
    this.boundPageShow = (e) => { if (e.persisted && this.active) this.reattach('page restored'); };
  }

  start(settings) {
    this.settings = settings;
    this.active = true;
    this.failures = 0;
    document.addEventListener('visibilitychange', this.boundVisibility);
    window.addEventListener('online', this.boundOnline);
    window.addEventListener('pageshow', this.boundPageShow);
    this.watchFrames();
    this.watchdog = setInterval(() => this.checkHealth(), 500);
    this.statsTimer = setInterval(() => this.collectStats(), 1000);
    return this.reattach('start');
  }

  stop() {
    this.active = false;
    this.gen++;
    clearInterval(this.watchdog);
    clearInterval(this.statsTimer);
    clearTimeout(this.disconnectTimer);
    clearTimeout(this.retryTimer);
    document.removeEventListener('visibilitychange', this.boundVisibility);
    window.removeEventListener('online', this.boundOnline);
    window.removeEventListener('pageshow', this.boundPageShow);
    this.teardown();
  }

  teardown() {
    this.sender.detach();
    const pc = this.pc, id = this.peerId;
    this.pc = null; this.peerId = null; this.control = null;
    if (pc) { try { pc.close(); } catch { /* */ } }
    if (id) api.post('/api/peer/close', { id }).catch(() => {});
  }

  async reattach(reason) {
    if (!this.active) return;
    const gen = ++this.gen;
    clearTimeout(this.retryTimer);
    clearTimeout(this.disconnectTimer);
    console.info('[duoplay] connecting:', reason);
    this.onStatus?.(this.framesSeen ? 'reconnecting' : 'connecting', reason);
    this.teardown();
    try {
      await this.attach(gen);
    } catch (err) {
      if (gen !== this.gen || !this.active) return;
      this.failures++;
      const delay = Math.min(5000, 400 * 2 ** Math.min(this.failures, 4));
      console.warn('[duoplay] attach failed', err);
      this.onStatus?.('reconnecting', err.message || String(err));
      this.retryTimer = setTimeout(() => this.reattach('retry'), delay);
    }
  }

  async attach(gen) {
    const pc = new RTCPeerConnection({ iceServers: [], bundlePolicy: 'max-bundle', rtcpMuxPolicy: 'require' });
    this.pc = pc;
    const stream = new MediaStream();

    pc.ontrack = (e) => {
      if (gen !== this.gen) return;
      stream.addTrack(e.track);
      this.tuneReceiver(e.receiver);
      if (this.video.srcObject !== stream) this.video.srcObject = stream;
      this.play();
    };
    pc.ondatachannel = (e) => {
      const ch = e.channel;
      if (ch.label === 'input') {
        ch.binaryType = 'arraybuffer';
        const go = () => { if (gen === this.gen) this.sender.attach(ch); };
        if (ch.readyState === 'open') go(); else ch.onopen = go;
      } else if (ch.label === 'control') {
        ch.onopen = () => {
          if (gen !== this.gen) return;
          this.control = ch;
          this.sendControl({ t: 'settings', adaptive: !!this.settings.adaptive });
        };
        ch.onmessage = (m) => { if (gen === this.gen) this.handleControl(m.data); };
      }
    };
    pc.onconnectionstatechange = () => {
      if (gen !== this.gen) return;
      const st = pc.connectionState;
      if (st === 'connected') {
        this.failures = 0;
        clearTimeout(this.disconnectTimer);
      } else if (st === 'failed') {
        this.reattach('connection failed');
      } else if (st === 'disconnected') {
        // ICE often heals itself after a short Wi-Fi hiccup; give it a moment first.
        clearTimeout(this.disconnectTimer);
        this.disconnectTimer = setTimeout(() => {
          if (gen === this.gen && pc.connectionState !== 'connected') this.reattach('connection lost');
        }, 2000);
      }
    };

    const offer = await api.post('/api/peer', {
      client_id: CLIENT_ID,
      adaptive: !!this.settings.adaptive,
      smooth_bursts: !!this.settings.smooth,
      latency: Number(this.settings.latency ?? 1),
      expected_kbps: Number(this.settings.bitrate) || (Number(this.settings.height) >= 1080 ? 15000 : 10000),
    });
    if (gen !== this.gen) { api.post('/api/peer/close', { id: offer.id }).catch(() => {}); return; }
    this.peerId = offer.id;
    await pc.setRemoteDescription({ type: 'offer', sdp: offer.sdp });
    const answer = await pc.createAnswer();
    answer.sdp = preferStereoOpus(answer.sdp);
    await pc.setLocalDescription(answer);
    await gathered(pc, 700);
    if (gen !== this.gen) return;
    await api.post('/api/peer/answer', { id: offer.id, sdp: pc.localDescription.sdp });
  }

  tuneReceiver(receiver) {
    const lowest = Number(this.settings.latency) === 0;
    try {
      if ('jitterBufferTarget' in receiver) receiver.jitterBufferTarget = lowest ? 0 : null;
      if ('playoutDelayHint' in receiver) receiver.playoutDelayHint = lowest ? 0 : null;
    } catch { /* not supported */ }
  }

  play() {
    const v = this.video;
    v.muted = false;
    const p = v.play();
    if (p?.catch) {
      p.catch(() => {
        // Sound needs a tap on this device: keep the picture going muted meanwhile.
        v.muted = true;
        v.play().catch(() => {});
        this.onStatus?.('needs-tap');
      });
    }
  }

  sendControl(obj) {
    if (this.control?.readyState === 'open') {
      try { this.control.send(JSON.stringify(obj)); } catch { /* */ }
    }
  }

  handleControl(data) {
    let m;
    try { m = JSON.parse(data); } catch { return; }
    if (m.t === 'session') this.onSession?.(m);
    else if (m.t === 'renegotiate') this.reattach(m.reason || 'renegotiate');
    else if (m.t === 'pong') this.dcRtt = performance.now() - m.id;
    else this.onControl?.(m);
  }

  onVisibility() {
    if (document.visibilityState !== 'visible' || !this.active) return;
    // Back on screen: tell the bridge at once (it paused our stream while we were away),
    // and rebuild the connection if it didn't survive.
    this.sender.flush();
    const st = this.pc?.connectionState;
    if (!this.pc || st === 'failed' || st === 'closed' || st === 'disconnected') this.reattach('back in foreground');
    else this.sendControl({ t: 'keyframe' });
  }

  watchFrames() {
    const v = this.video;
    if (v.requestVideoFrameCallback) {
      const cb = () => { this.lastFrameAt = performance.now(); this.framesSeen++; if (this.active) v.requestVideoFrameCallback(cb); };
      v.requestVideoFrameCallback(cb);
    } else {
      let last = -1;
      const poll = () => {
        if (!this.active) return;
        if (v.currentTime !== last) { last = v.currentTime; this.lastFrameAt = performance.now(); this.framesSeen++; }
        requestAnimationFrame(poll);
      };
      requestAnimationFrame(poll);
    }
  }

  checkHealth() {
    if (!this.active || !this.pc || document.visibilityState !== 'visible') return;
    if (this.pc.connectionState !== 'connected' || !this.framesSeen || !this.streaming) return;
    const stale = performance.now() - this.lastFrameAt;
    if (stale > 1200 && performance.now() - this.lastKeyReq > 1500) {
      this.lastKeyReq = performance.now();
      this.sendControl({ t: 'keyframe' }); // picture froze: ask for a fresh keyframe
    }
    if (stale > 6000) this.reattach('picture frozen');
  }

  async collectStats() {
    const pc = this.pc;
    if (!pc || pc.connectionState !== 'connected') return;
    this.sendControl({ t: 'ping', id: performance.now() });
    let report;
    try { report = await pc.getStats(); } catch { return; }
    let v = null, pair = null;
    report.forEach((s) => {
      if (s.type === 'inbound-rtp' && (s.kind === 'video' || s.mediaType === 'video')) v = s;
      if (s.type === 'candidate-pair' && s.state === 'succeeded' && (s.nominated || s.selected)) {
        if (!pair || (s.bytesReceived || 0) > (pair.bytesReceived || 0)) pair = s;
      }
    });
    if (!v) return;
    const now = performance.now();
    const p = this.prevStats;
    const out = {
      width: v.frameWidth, height: v.frameHeight,
      fps: v.framesPerSecond ?? 0,
      rttMs: pair?.currentRoundTripTime != null ? pair.currentRoundTripTime * 1000 : (this.dcRtt ?? null),
    };
    if (p) {
      const dt = (now - p.t) / 1000;
      out.mbps = ((v.bytesReceived - p.bytes) * 8) / 1e6 / dt;
      const recv = v.packetsReceived - p.recv, lost = Math.max(0, v.packetsLost - p.lost);
      out.lossPct = recv + lost > 0 ? (lost / (recv + lost)) * 100 : 0;
      out.nacks = (v.nackCount ?? 0) - (p.nacks ?? 0);
      const emitted = (v.jitterBufferEmittedCount ?? 0) - (p.jbCount ?? 0);
      out.bufferMs = emitted > 0 ? (((v.jitterBufferDelay ?? 0) - (p.jbDelay ?? 0)) / emitted) * 1000 : null;
      const dec = (v.framesDecoded ?? 0) - (p.decoded ?? 0);
      out.decodeMs = dec > 0 ? (((v.totalDecodeTime ?? 0) - (p.decodeTime ?? 0)) / dec) * 1000 : null;
      out.dropped = (v.framesDropped ?? 0) - (p.dropped ?? 0);
      if (!out.fps && dt > 0) out.fps = dec / dt;
    }
    this.prevStats = {
      t: now, bytes: v.bytesReceived, recv: v.packetsReceived, lost: v.packetsLost, nacks: v.nackCount,
      jbCount: v.jitterBufferEmittedCount, jbDelay: v.jitterBufferDelay, decoded: v.framesDecoded,
      decodeTime: v.totalDecodeTime, dropped: v.framesDropped,
    };
    out.quality = rate(out, Number(this.settings.fps) || 60);
    this.onStats?.(out);
  }
}

function rate(s, targetFps) {
  if (s.lossPct == null) return 'unknown';
  const rtt = s.rttMs ?? 0;
  if (s.lossPct < 1 && rtt < 20 && s.fps >= targetFps * 0.9) return 'good';
  if (s.lossPct < 4 && rtt < 45 && s.fps >= targetFps * 0.7) return 'fair';
  return 'poor';
}

function gathered(pc, timeoutMs) {
  if (pc.iceGatheringState === 'complete') return Promise.resolve();
  return new Promise((resolve) => {
    const t = setTimeout(resolve, timeoutMs);
    pc.addEventListener('icegatheringstatechange', () => {
      if (pc.iceGatheringState === 'complete') { clearTimeout(t); resolve(); }
    });
  });
}

// The console sends stereo; make sure the phone decodes it as stereo.
function preferStereoOpus(sdp) {
  const m = sdp.match(/a=rtpmap:(\d+) opus\/48000\/2/i);
  if (!m) return sdp;
  const pt = m[1];
  return sdp.replace(new RegExp(`a=fmtp:${pt} ([^\\r\\n]*)`), (line, params) =>
    /stereo=1/.test(params) ? line : `a=fmtp:${pt} ${params};stereo=1;sprop-stereo=1`);
}
