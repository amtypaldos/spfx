// Protocol client: request/response over any byte transport. No DOM dependencies.
// Transport contract: { write(Uint8Array): void|Promise, onData: (Uint8Array)=>void, onClose: ()=>void, close(): void|Promise }
import { T, FrameDecoder, buildFrame, parseFrame } from './protocol.js';

const RESPONSE_TYPE = { [T.HELLO]: T.HELLO_RSP, [T.GET_FX]: T.FX_DESC, [T.GET_STATE]: T.STATE };

export class RequestTimeout extends Error {
  constructor(msg) { super(msg); this.name = 'RequestTimeout'; }
}

export class Client {
  constructor(transport, opts = {}) {
    this.t = transport;
    this.timeoutMs = opts.timeoutMs ?? 500;   // PROTOCOL.md: retry after 500 ms
    this.retries = opts.retries ?? 3;         // ... max 3
    this.pending = new Map();                 // seq -> {msg, resolve, reject, tries, timer}
    this.nextSeq = 1;
    this.decoder = new FrameDecoder();
    this.onEvent = null;                      // (msg) => void for 0xA0/0xA1/0xA2
    this.onClose = null;
    this.dropped = 0;
    this.closed = false;
    transport.onData = (bytes) => this._rx(bytes);
    transport.onClose = () => this._closed();
  }

  _rx(bytes) {
    for (const f of this.decoder.push(bytes)) {
      const r = parseFrame(f);
      if (!r.ok) { this.dropped++; continue; }   // corrupt frames are dropped silently; request timers retry
      const { seq, msg } = r;
      if (msg.type === T.EVENT || msg.type === T.ADMISSION_ERROR || msg.type === T.METERS || msg.type === T.PRESET_LOADED) { this.onEvent?.(msg); continue; }
      const p = this.pending.get(seq);
      if (!p) continue;
      const want = RESPONSE_TYPE[p.msg.type] ?? T.RESULT;
      // A 0x90 RESULT that names this request is always accepted (error reply to GET_*; see PROTOCOL_NOTES.md)
      const isErr = msg.type === T.RESULT && msg.requestType === p.msg.type;
      if (msg.type !== want && !isErr) continue;
      clearTimeout(p.timer);
      this.pending.delete(seq);
      p.resolve(msg);
    }
  }

  _allocSeq() {
    for (let i = 0; i < 255; i++) {
      const s = this.nextSeq;
      this.nextSeq = this.nextSeq >= 255 ? 1 : this.nextSeq + 1;   // seq 0 is reserved for device events
      if (!this.pending.has(s)) return s;
    }
    throw new Error('too many requests in flight');
  }

  request(msg) {
    if (this.closed) return Promise.reject(new Error('not connected'));
    return new Promise((resolve, reject) => {
      const seq = this._allocSeq();
      const p = { msg, resolve, reject, tries: 0, timer: null, frame: buildFrame(msg, seq) };
      this.pending.set(seq, p);
      const send = () => {
        p.tries++;
        Promise.resolve(this.t.write(p.frame)).catch((e) => { this.pending.delete(seq); clearTimeout(p.timer); reject(e); });
        p.timer = setTimeout(() => {
          if (p.tries > this.retries) { this.pending.delete(seq); reject(new RequestTimeout('no response from device')); }
          else send();
        }, this.timeoutMs);
      };
      send();
    });
  }

  _closed() {
    if (this.closed) return;
    this.closed = true;
    for (const [, p] of this.pending) { clearTimeout(p.timer); p.reject(new Error('connection closed')); }
    this.pending.clear();
    this.onClose?.();
  }

  async close() { try { await this.t.close(); } finally { this._closed(); } }
}

/**
 * Rate-limited, coalescing sender. Latest message per key wins; at most one message per `intervalMs`
 * (default 34 ms ~ 29 msgs/s). `send(msg)` performs the transmission.
 */
export class Coalescer {
  constructor(send, intervalMs = 34, now = () => Date.now()) {
    this.send = send; this.interval = intervalMs; this.now = now;
    this.pending = new Map(); this.timer = null; this.lastSent = -Infinity;
  }
  set(key, msg) {
    this.pending.delete(key);   // re-insert so the most recently touched key goes last
    this.pending.set(key, msg);
    this._schedule();
  }
  _schedule() {
    if (this.timer || !this.pending.size) return;
    const wait = Math.max(0, this.lastSent + this.interval - this.now());
    this.timer = setTimeout(() => {
      this.timer = null;
      const [key, msg] = this.pending.entries().next().value;
      this.pending.delete(key);
      this.lastSent = this.now();
      this.send(msg);
      this._schedule();
    }, wait);
  }
  cancel() { clearTimeout(this.timer); this.timer = null; this.pending.clear(); }
}
