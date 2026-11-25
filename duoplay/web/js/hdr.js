// HDR picture path.
//
// The console streams HDR as 10-bit HEVC with the PQ (HDR10) transfer function. Safari's
// WebRTC stack can't be relied on for HEVC, but Safari's media player renders HDR10
// natively, at full brightness on the Duo's 1600-nit panels. So in HDR mode the bridge
// wraps the console's frames into fragmented MP4 (no re-encoding) and sends them over a
// reliable data channel; here they go into a ManagedMediaSource feeding the <video>.
//
// A media player buffers by design. The latency controller below keeps playback pinned
// to the newest frame: it speeds up slightly when a few frames pile up and jumps ahead
// if the backlog ever grows past a quarter of a second.

export function hdrDisplay() {
  try { return matchMedia('(dynamic-range: high)').matches || matchMedia('(video-dynamic-range: high)').matches; } catch { return false; }
}

function mediaSourceClass() { return self.ManagedMediaSource || self.MediaSource || null; }

export function hevcHdrPlayable(codec = 'hvc1.2.4.L153.B0') {
  const MS = mediaSourceClass();
  try { return !!MS && MS.isTypeSupported(`video/mp4; codecs="${codec}"`); } catch { return false; }
}

export class LiveMediaPlayer {
  constructor(video) {
    this.video = video;
    this.queue = [];
    this.sb = null;
    this.ms = null;
    this.mime = '';
    this.timer = 0;
    this.appended = 0;
    this.lagMs = 0;
  }

  async open(mime) {
    this.close();
    const MS = mediaSourceClass();
    if (!MS) throw new Error('This browser has no media source support');
    this.mime = mime;
    const ms = new MS();
    this.ms = ms;
    const v = this.video;
    v.disableRemotePlayback = true; // required for ManagedMediaSource without an AirPlay fallback
    if ('srcObject' in v && self.ManagedMediaSource && ms instanceof self.ManagedMediaSource) {
      try { v.srcObject = ms; } catch { v.src = URL.createObjectURL(ms); }
    } else {
      v.src = URL.createObjectURL(ms);
    }
    await new Promise((resolve) => ms.addEventListener('sourceopen', resolve, { once: true }));
    const sb = ms.addSourceBuffer(mime);
    sb.mode = 'sequence'; // frames play back to back; arrival jitter can't open gaps
    sb.addEventListener('updateend', () => this.pump());
    this.sb = sb;
    // The managed source may ask us to pause appending to save power; for a live game
    // stream we keep going, so these are only noted.
    ms.addEventListener?.('endstreaming', () => { this.streamingPaused = true; });
    ms.addEventListener?.('startstreaming', () => { this.streamingPaused = false; });
    this.timer = setInterval(() => this.chase(), 50);
    this.pump();
  }

  /** Append an MP4 init segment or fragment. */
  push(buf) {
    this.queue.push(buf instanceof ArrayBuffer ? new Uint8Array(buf) : buf);
    this.pump();
  }

  pump() {
    const sb = this.sb;
    if (!sb || sb.updating || !this.queue.length) return;
    // Everything waiting goes in one append: fewer round trips through the media stack.
    let total = 0;
    for (const b of this.queue) total += b.byteLength;
    const out = new Uint8Array(total);
    let off = 0;
    for (const b of this.queue) { out.set(b, off); off += b.byteLength; }
    this.queue = [];
    try {
      sb.appendBuffer(out);
      this.appended++;
    } catch (e) {
      if (e.name === 'QuotaExceededError') this.trim(true);
      else console.warn('[duoplay] append failed', e);
    }
    const v = this.video;
    if (v.paused) v.play().catch(() => {});
  }

  chase() {
    const v = this.video, sb = this.sb;
    if (!sb || !v.buffered.length) return;
    const end = v.buffered.end(v.buffered.length - 1);
    const lag = end - v.currentTime;
    this.lagMs = lag * 1000;
    if (lag > 0.25) {
      v.currentTime = end - 0.03;
      v.playbackRate = 1;
    } else if (lag > 0.07) {
      v.playbackRate = 1.25;
    } else if (v.playbackRate !== 1) {
      v.playbackRate = 1;
    }
    if (v.paused && lag > 0.02) v.play().catch(() => {});
    this.trim(false);
  }

  trim(force) {
    const v = this.video, sb = this.sb;
    if (!sb || sb.updating || !v.buffered.length) return;
    const start = v.buffered.start(0);
    if (force || v.currentTime - start > 20) {
      try { sb.remove(start, Math.max(start, v.currentTime - 2)); } catch { /* busy */ }
    }
  }

  close() {
    clearInterval(this.timer);
    this.queue = [];
    this.sb = null;
    if (this.ms && this.ms.readyState === 'open') { try { this.ms.endOfStream(); } catch { /* */ } }
    this.ms = null;
    const v = this.video;
    if (v.src?.startsWith('blob:')) URL.revokeObjectURL(v.src);
    v.removeAttribute('src');
    if (v.srcObject && !(v.srcObject instanceof MediaStream)) v.srcObject = null;
  }
}

/**
 * Reassembles the bridge's media messages. Wire format (binary):
 *   [0] type: 1 = init segment, 2 = fragment
 *   [1] flags: bit0 = last chunk of this message
 *   [2..5] message id (uint32 LE)
 *   [6..] payload chunk
 * Init messages start with a uint16 LE length + UTF-8 JSON header ({"mime":...}).
 */
export class MediaReassembler {
  constructor({ onInit, onFragment }) {
    this.onInit = onInit;
    this.onFragment = onFragment;
    this.parts = [];
    this.id = -1;
  }

  feed(data) {
    const u8 = new Uint8Array(data);
    if (u8.length < 6) return;
    const type = u8[0], last = u8[1] & 1;
    const id = u8[2] | (u8[3] << 8) | (u8[4] << 16) | (u8[5] << 24);
    if (id !== this.id) { this.parts = []; this.id = id; }
    this.parts.push(u8.subarray(6));
    if (!last) return;
    let total = 0;
    for (const p of this.parts) total += p.byteLength;
    const msg = new Uint8Array(total);
    let off = 0;
    for (const p of this.parts) { msg.set(p, off); off += p.byteLength; }
    this.parts = [];
    if (type === 1) {
      const n = msg[0] | (msg[1] << 8);
      const header = JSON.parse(new TextDecoder().decode(msg.subarray(2, 2 + n)));
      this.onInit?.(header, msg.subarray(2 + n));
    } else if (type === 2) {
      this.onFragment?.(msg);
    }
  }
}
