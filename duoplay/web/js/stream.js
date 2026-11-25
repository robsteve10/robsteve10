// Connection to the bridge, built to ride out Wi-Fi.
//
// The console session lives on the bridge, independent of this page: folding or
// unfolding never touches the connection, and if Wi-Fi drops it (or iOS suspends the
// app) reattaching is a WebRTC handshake plus one keyframe, a fraction of a second,
// while the game keeps running.
//
// Picture paths:
//   standard  H.264 over WebRTC RTP: lowest latency, NACK retransmission, playout-delay
//             hint and Safari 27's jitterBufferTarget for the buffering mode you choose.
//   HDR       HEVC Main10/PQ wrapped as MP4 fragments over a reliable data channel into
//             Safari's own media player (see hdr.js), which renders HDR10 natively.
// Audio is always Opus over WebRTC, played through its own <audio> element and not
// lip-synced to the picture: for a game, sound arriving first is better than late video.

import { api } from './api.js';
import { LiveMediaPlayer, MediaReassembler } from './hdr.js';

const CLIENT_ID = (() => {
  try {
    let id = sessionStorage.getItem('duoplay.client');
    if (!id) { id = crypto.getRandomValues(new Uint32Array(2)).join('-'); sessionStorage.setItem('duoplay.client', id); }
    return id;
  } catch { return String(Math.random()).slice(2); }
})();

// Safari 27: jitterBufferTarget (ms) per buffering mode; null leaves Safari's adaptive default.
const JITTER_TARGET = [0, null, 120];

export class StreamConnection {
  constructor({ video, audio, sender, onSession, onControl, onStatus, onStats }) {
    this.video = video;
    this.audio = audio;
    this.sender = sender;
    this.onSession = onSession;
    this.onControl = onControl;
    this.onStatus = onStatus;   // ('connecting'|'reconnecting'|'needs-tap', detail)
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
    this.player = null;
    this.mediaBytes = 0;
    this.transport = 'rtp';
    this.bridgeStats = null;
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
    this.player?.close();
    this.player = null;
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

    pc.ontrack = (e) => {
      if (gen !== this.gen) return;
      this.tuneReceiver(e.receiver, e.track.kind);
      const el = e.track.kind === 'audio' ? this.audio : this.video;
      if (e.track.kind === 'video' && this.transport !== 'rtp') return; // HDR picture comes over the media channel
      el.srcObject = new MediaStream([e.track]);
      this.play(el);
    };
    pc.ondatachannel = (e) => {
      const ch = e.channel;
      ch.binaryType = 'arraybuffer';
      if (ch.label === 'input') {
        const go = () => { if (gen === this.gen) this.sender.attach(ch); };
        if (ch.readyState === 'open') go(); else ch.onopen = go;
      } else if (ch.label === 'control') {
        ch.onopen = () => {
          if (gen !== this.gen) return;
          this.control = ch;
          this.sendControl({ t: 'settings', adaptive: !!this.settings.adaptive });
        };
        ch.onmessage = (m) => { if (gen === this.gen) this.handleControl(m.data); };
      } else if (ch.label === 'media') {
        this.attachMedia(ch, gen);
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
        }, 1500);
      }
    };

    const offer = await api.post('/api/peer', {
      client_id: CLIENT_ID,
      adaptive: !!this.settings.adaptive,
      smooth_bursts: !!this.settings.smooth,
      latency: Number(this.settings.latency ?? 1),
      expected_kbps: Number(this.settings.bitrate) || (Number(this.settings.height) >= 1080 ? 15000 : 10000),
      fps: Number(this.settings.fps) || 60,
      media_player: !!this.settings.mediaPlayer,
    });
    if (gen !== this.gen) { api.post('/api/peer/close', { id: offer.id }).catch(() => {}); return; }
    this.peerId = offer.id;
    this.transport = offer.transport || 'rtp';
    await pc.setRemoteDescription({ type: 'offer', sdp: offer.sdp });
    const answer = await pc.createAnswer();
    answer.sdp = preferStereoOpus(answer.sdp);
    await pc.setLocalDescription(answer);
    await gathered(pc, 500);
    if (gen !== this.gen) return;
    await api.post('/api/peer/answer', { id: offer.id, sdp: pc.localDescription.sdp });
  }

  attachMedia(ch, gen) {
    const player = new LiveMediaPlayer(this.video);
    this.player = player;
    let opened = null;
    const re = new MediaReassembler({
      onInit: (header, init) => {
        if (gen !== this.gen) return;
        const go = async () => {
          if (!opened || player.mime !== header.mime) await player.open(header.mime);
          player.push(init);
        };
        opened = (opened || Promise.resolve()).then(go).catch((e) => this.onStatus?.('reconnecting', e.message));
      },
      onFragment: (frag) => {
        if (gen !== this.gen || !opened) return;
        opened.then(() => player.push(frag));
      },
    });
    ch.onmessage = (m) => { this.mediaBytes += m.data.byteLength; re.feed(m.data); };
  }

  tuneReceiver(receiver, kind) {
    const mode = Number(this.settings.latency ?? 1);
    try {
      if ('jitterBufferTarget' in receiver) receiver.jitterBufferTarget = kind === 'audio' && mode === 0 ? 0 : JITTER_TARGET[mode];
    } catch { /* older Safari */ }
  }

  play(el) {
    el.muted = false;
    const p = el.play();
    if (p?.catch) {
      p.catch(() => {
        // Sound needs a touch on this device: keep the picture going muted meanwhile.
        if (el === this.audio) this.onStatus?.('needs-tap');
        else { el.muted = true; el.play().catch(() => {}); }
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
    else if (m.t === 'link') this.bridgeStats = m;
    else this.onControl?.(m);
  }

  onVisibility() {
    if (document.visibilityState !== 'visible' || !this.active) return;
    // Back on screen: tell the bridge at once (it paused our stream while we were away)
    // and rebuild the connection if it didn't survive.
    this.sender.flush();
    const st = this.pc?.connectionState;
    if (!this.pc || st === 'failed' || st === 'closed' || st === 'disconnected') this.reattach('back in foreground');
    else this.sendControl({ t: 'keyframe' });
  }

  watchFrames() {
    const v = this.video;
    const cb = () => { this.lastFrameAt = performance.now(); this.framesSeen++; if (this.active) v.requestVideoFrameCallback(cb); };
    v.requestVideoFrameCallback(cb);
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
      if (s.type === 'inbound-rtp' && s.kind === 'video') v = s;
      if (s.type === 'candidate-pair' && s.state === 'succeeded' && s.nominated) {
        if (!pair || (s.bytesReceived || 0) > (pair.bytesReceived || 0)) pair = s;
      }
    });
    const now = performance.now();
    const p = this.prevStats;
    const out = {
      transport: this.transport,
      width: this.video.videoWidth, height: this.video.videoHeight,
      rttMs: pair?.currentRoundTripTime != null ? pair.currentRoundTripTime * 1000 : (this.dcRtt ?? null),
      queueMs: this.bridgeStats?.queue_ms ?? null,
      bufferMs: this.transport === 'mse' ? this.player?.lagMs ?? null : null,
    };
    const frames = this.framesSeen;
    if (p) {
      const dt = (now - p.t) / 1000;
      out.fps = (frames - p.frames) / dt;
      if (this.transport === 'mse') {
        out.mbps = ((this.mediaBytes - p.media) * 8) / 1e6 / dt;
        out.lossPct = this.bridgeStats?.loss_pct ?? 0;
      } else if (v) {
        out.mbps = ((v.bytesReceived - p.bytes) * 8) / 1e6 / dt;
        const recv = v.packetsReceived - p.recv, lost = Math.max(0, v.packetsLost - p.lost);
        out.lossPct = recv + lost > 0 ? (lost / (recv + lost)) * 100 : 0;
        const emitted = (v.jitterBufferEmittedCount ?? 0) - (p.jbCount ?? 0);
        out.bufferMs = emitted > 0 ? (((v.jitterBufferDelay ?? 0) - (p.jbDelay ?? 0)) / emitted) * 1000 : null;
        const dec = (v.framesDecoded ?? 0) - (p.decoded ?? 0);
        out.decodeMs = dec > 0 ? (((v.totalDecodeTime ?? 0) - (p.decodeTime ?? 0)) / dec) * 1000 : null;
      }
    }
    this.prevStats = {
      t: now, frames, media: this.mediaBytes, bytes: v?.bytesReceived ?? 0, recv: v?.packetsReceived ?? 0,
      lost: v?.packetsLost ?? 0, jbCount: v?.jitterBufferEmittedCount, jbDelay: v?.jitterBufferDelay,
      decoded: v?.framesDecoded, decodeTime: v?.totalDecodeTime,
    };
    out.quality = rate(out, Number(this.settings.fps) || 60);
    this.onStats?.(out);
  }
}

function rate(s, targetFps) {
  if (s.fps == null) return 'unknown';
  const rtt = s.rttMs ?? 0, loss = s.lossPct ?? 0, q = s.queueMs ?? 0;
  if (loss < 1 && rtt < 20 && q < 15 && s.fps >= targetFps * 0.9) return 'good';
  if (loss < 4 && rtt < 45 && q < 40 && s.fps >= targetFps * 0.7) return 'fair';
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

// The console sends stereo; make sure Safari decodes it as stereo.
function preferStereoOpus(sdp) {
  const m = sdp.match(/a=rtpmap:(\d+) opus\/48000\/2/i);
  if (!m) return sdp;
  const pt = m[1];
  return sdp.replace(new RegExp(`a=fmtp:${pt} ([^\\r\\n]*)`), (line, params) =>
    /stereo=1/.test(params) ? line : `a=fmtp:${pt} ${params};stereo=1;sprop-stereo=1`);
}
