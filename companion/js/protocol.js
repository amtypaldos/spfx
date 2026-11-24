// SP-1 FX device protocol v1: COBS, CRC-16/CCITT-FALSE, framing, messages.
// Pure ES module, no DOM or Node dependencies, so it runs in the browser and in `node --test`.
// Implements docs/PROTOCOL.md; where that document is silent or inconsistent see PROTOCOL_NOTES.md.

export const PROTO_VERSION = 1;
export const MAX_BODY = 256;          // PROTOCOL.md: "Max body 256 bytes"
export const MAX_PAYLOAD = 250;       // PROTOCOL.md: payload 0..250 bytes
// PROTOCOL_NOTES.md #1: the 0x83 STATE payload (383 bytes for 11 effects) cannot fit in 250.
// Receivers therefore accept larger bodies; senders of everything else stay within MAX_BODY.
export const MAX_BODY_RX = 1024;

export class ProtocolError extends Error {
  constructor(msg) { super(msg); this.name = 'ProtocolError'; }
}

// ---------- message type codes ----------
export const T = Object.freeze({
  HELLO: 0x01, GET_FX: 0x02, GET_STATE: 0x03,
  SET_SLOT_FX: 0x10, SET_PARAM: 0x11, SET_WET: 0x12, SET_BYPASS: 0x13, SET_GLOBAL_BYPASS: 0x14,
  SET_MASTER: 0x15, SET_INPUT_GAIN: 0x16, SET_ORDER: 0x17, SET_BPM: 0x18,
  SAVE_NOW: 0x20, FACTORY_RESET: 0x21, SUBSCRIBE: 0x30,
  HELLO_RSP: 0x81, FX_DESC: 0x82, STATE: 0x83, RESULT: 0x90,
  EVENT: 0xA0, ADMISSION_ERROR: 0xA1, METERS: 0xA2,
});
export const TYPE_NAMES = Object.freeze(Object.fromEntries(Object.entries(T).map(([k, v]) => [v, k])));
export const SETTER_TYPES = Object.freeze([
  T.SET_SLOT_FX, T.SET_PARAM, T.SET_WET, T.SET_BYPASS, T.SET_GLOBAL_BYPASS,
  T.SET_MASTER, T.SET_INPUT_GAIN, T.SET_ORDER, T.SET_BPM,
]);
export const RESULT = Object.freeze({ OK: 0, BAD_ARG: 1, NO_MEMORY: 2, NO_CPU: 3, BUSY: 4, STORAGE: 5 });
export const FACTORY_RESET_CONFIRM = 0xA5;

// ---------- COBS ----------
export function cobsEncode(data) {
  const out = [0];
  let codeIdx = 0, code = 1;
  for (let i = 0; i < data.length; i++) {
    const b = data[i];
    if (b === 0) {
      out[codeIdx] = code; codeIdx = out.length; out.push(0); code = 1;
    } else {
      out.push(b); code++;
      if (code === 0xFF) { out[codeIdx] = code; codeIdx = out.length; out.push(0); code = 1; }
    }
  }
  out[codeIdx] = code;
  return Uint8Array.from(out);
}

/** Returns Uint8Array, or throws ProtocolError on malformed input. Input must not contain the 0x00 delimiter. */
export function cobsDecode(data) {
  const out = [];
  let i = 0;
  while (i < data.length) {
    const code = data[i++];
    if (code === 0) throw new ProtocolError('COBS: zero byte inside frame');
    const end = i + code - 1;
    if (end > data.length) throw new ProtocolError('COBS: block overruns frame');
    for (; i < end; i++) {
      if (data[i] === 0) throw new ProtocolError('COBS: zero byte inside block');
      out.push(data[i]);
    }
    if (code !== 0xFF && i < data.length) out.push(0);
  }
  return Uint8Array.from(out);
}

// ---------- CRC-16/CCITT-FALSE ----------
export function crc16(bytes, init = 0xFFFF) {
  let crc = init;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i] << 8;
    for (let b = 0; b < 8; b++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
  }
  return crc;
}

// ---------- framing ----------
/** Encode one frame: COBS(ver type seq payload crc16le) + 0x00. */
export function encodeFrame({ type, seq = 0, payload = new Uint8Array(0) }, maxBody = MAX_BODY) {
  if (!Number.isInteger(type) || type < 0 || type > 255) throw new ProtocolError('bad type');
  if (!Number.isInteger(seq) || seq < 0 || seq > 255) throw new ProtocolError('bad seq');
  if (maxBody === MAX_BODY && payload.length > MAX_PAYLOAD) throw new ProtocolError(`payload ${payload.length} > ${MAX_PAYLOAD}`);
  const body = new Uint8Array(3 + payload.length + 2);
  body[0] = PROTO_VERSION; body[1] = type; body[2] = seq;
  body.set(payload, 3);
  if (body.length > maxBody) throw new ProtocolError(`frame body ${body.length} > ${maxBody}`);
  const crc = crc16(body.subarray(0, body.length - 2));
  body[body.length - 2] = crc & 0xFF; body[body.length - 1] = crc >> 8;
  const enc = cobsEncode(body);
  const out = new Uint8Array(enc.length + 1);
  out.set(enc); out[enc.length] = 0;
  return out;
}

/** Decode the bytes of one frame (without the trailing 0x00). Never throws; returns {ok,...}. */
export function decodeFrame(bytes, maxBody = MAX_BODY_RX) {
  let body;
  try { body = cobsDecode(bytes); } catch (e) { return { ok: false, error: e.message }; }
  if (body.length < 5) return { ok: false, error: 'frame too short' };
  if (body.length > maxBody) return { ok: false, error: 'frame too long' };
  const want = body[body.length - 2] | (body[body.length - 1] << 8);
  if (crc16(body.subarray(0, body.length - 2)) !== want) return { ok: false, error: 'CRC mismatch' };
  if (body[0] !== PROTO_VERSION) return { ok: false, error: `unsupported frame version ${body[0]}` };
  return { ok: true, type: body[1], seq: body[2], payload: body.slice(3, body.length - 2) };
}

/** Streaming splitter: feed arbitrary chunks, get decoded frame results. Bad frames come back as {ok:false} (callers drop them). */
export class FrameDecoder {
  constructor(maxBody = MAX_BODY_RX) { this.buf = []; this.maxBody = maxBody; this.overflow = false; }
  push(chunk) {
    const results = [];
    for (let i = 0; i < chunk.length; i++) {
      const b = chunk[i];
      if (b === 0) {
        if (this.overflow) { results.push({ ok: false, error: 'frame overflow' }); }
        else if (this.buf.length) { results.push(decodeFrame(Uint8Array.from(this.buf), this.maxBody)); }
        this.buf = []; this.overflow = false;
      } else if (!this.overflow) {
        this.buf.push(b);
        if (this.buf.length > Math.ceil(this.maxBody * 255 / 254) + 4) { this.overflow = true; this.buf = []; }
      }
    }
    return results;
  }
}

// ---------- scalar helpers ----------
export const n2u16 = (x) => Math.round(Math.min(1, Math.max(0, Number.isFinite(x) ? x : 0)) * 65535);
export const u162n = (u) => u / 65535;

const enc = new TextEncoder();
const dec = new TextDecoder('utf-8', { fatal: false });

class Writer {
  constructor() { this.a = []; }
  u8(v) { this.a.push(v & 0xFF); return this; }
  u16(v) { this.a.push(v & 0xFF, (v >> 8) & 0xFF); return this; }
  i16(v) { return this.u16(v & 0xFFFF); }
  u32(v) { this.a.push(v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF); return this; }
  f32(v) { const b = new Uint8Array(4); new DataView(b.buffer).setFloat32(0, v, true); this.a.push(...b); return this; }
  str(s) { const b = enc.encode(s); if (b.length > 255) throw new ProtocolError('string too long'); this.u8(b.length); this.a.push(...b); return this; }
  bytes() { return Uint8Array.from(this.a); }
}
class Reader {
  constructor(b) { this.b = b; this.dv = new DataView(b.buffer, b.byteOffset, b.byteLength); this.p = 0; }
  need(n) { if (this.p + n > this.b.length) throw new ProtocolError('payload too short'); }
  u8() { this.need(1); return this.b[this.p++]; }
  u16() { this.need(2); const v = this.dv.getUint16(this.p, true); this.p += 2; return v; }
  i16() { this.need(2); const v = this.dv.getInt16(this.p, true); this.p += 2; return v; }
  u32() { this.need(4); const v = this.dv.getUint32(this.p, true); this.p += 4; return v; }
  f32() { this.need(4); const v = this.dv.getFloat32(this.p, true); this.p += 4; return v; }
  str() { const n = this.u8(); this.need(n); const s = dec.decode(this.b.subarray(this.p, this.p + n)); this.p += n; return s; }
  done() { if (this.p !== this.b.length) throw new ProtocolError('trailing bytes in payload'); }
}

const SCALES = ['lin', 'log', 'step'];
const MIX_MODES = ['crossfade', 'send'];

function checkRange(name, v, lo, hi) {
  if (!Number.isInteger(v) || v < lo || v > hi) throw new ProtocolError(`${name} out of range`);
  return v;
}
const checkSlot = (v) => checkRange('slot', v, 0, 3);

// ---------- message codec ----------
// Message objects use normalised floats (0..1) for u16n fields; they are quantised on the wire.

export function encodePayload(msg) {
  const w = new Writer();
  switch (msg.type) {
    case T.HELLO: case T.GET_STATE: case T.SAVE_NOW: break;
    case T.GET_FX: w.u8(checkRange('idx', msg.idx, 0, 255)); break;
    case T.SET_SLOT_FX: w.u8(checkSlot(msg.slot)).u8(checkRange('fx', msg.fx, 0, 255)); break;
    case T.SET_PARAM: w.u8(checkSlot(msg.slot)).u8(checkRange('idx', msg.idx, 0, 3)).u16(n2u16(msg.v)); break;
    case T.SET_WET: w.u8(checkSlot(msg.slot)).u16(n2u16(msg.v)); break;
    case T.SET_BYPASS: w.u8(checkSlot(msg.slot)).u8(msg.on ? 1 : 0); break;
    case T.SET_GLOBAL_BYPASS: w.u8(msg.on ? 1 : 0); break;
    case T.SET_MASTER: w.u16(n2u16(msg.v)); break;
    case T.SET_INPUT_GAIN: w.i16(checkRange('cdB', msg.cdB, -1200, 1200)); break;
    case T.SET_ORDER: {
      if (!isPermutation(msg.order)) throw new ProtocolError('order must be a permutation of 0..3');
      for (const o of msg.order) w.u8(o);
      break;
    }
    case T.SET_BPM: w.u16(checkRange('bpm10', msg.bpm10, 300, 3000)); break;
    case T.FACTORY_RESET: w.u8(msg.confirm ?? FACTORY_RESET_CONFIRM); break;
    case T.SUBSCRIBE: w.u8(checkRange('hz', msg.hz, 0, 20)); break;

    case T.HELLO_RSP:
      w.u8(msg.proto).u8(enc.encode(msg.fw).length);
      w.a.push(...enc.encode(msg.fw));
      w.u8(msg.fxCount).u32(msg.sampleRate).u16(msg.block).u32(msg.poolBytes).u16(msg.cpuBudget);
      break;
    case T.FX_DESC:
      w.u8(msg.id).str(msg.name).u8(msg.params.length).u16(msg.cpuCycles).u32(msg.memBytes).u32(msg.tailMs)
        .u8(Math.max(0, MIX_MODES.indexOf(msg.mixMode)));
      for (const p of msg.params) {
        w.str(p.name).str(p.unit).f32(p.min).f32(p.max).f32(p.def).u8(Math.max(0, SCALES.indexOf(p.scale))).str(p.labels || '');
      }
      break;
    case T.STATE: encodeState(w, msg); break;
    case T.RESULT: w.u8(msg.requestType).u8(msg.code); break;
    case T.EVENT: {
      if (!SETTER_TYPES.includes(msg.setter.type)) throw new ProtocolError('event setter type');
      w.u8(msg.setter.type); w.a.push(...encodePayload(msg.setter)); break;
    }
    case T.ADMISSION_ERROR: w.u8(msg.slot).u8(msg.fx).u8(msg.code); break;
    case T.METERS:
      w.u16(msg.inPeak).u16(msg.outPeak).u8(msg.cpuPct).u8(msg.cpuPeakPct).u32(msg.memUsed)
        .u32(msg.inClips).u32(msg.outClips).u32(msg.underruns).u32(msg.overruns).u32(msg.nanEvents)
        .u8(msg.uiMode).u8(msg.editSlot).u8(msg.globalBypass ? 1 : 0);
      break;
    default: throw new ProtocolError(`cannot encode type 0x${(msg.type >>> 0).toString(16)}`);
  }
  return w.bytes();
}

export function decodePayload(type, payload) {
  const r = new Reader(payload);
  let m;
  switch (type) {
    case T.HELLO: case T.GET_STATE: case T.SAVE_NOW: m = { type }; break;
    case T.GET_FX: m = { type, idx: r.u8() }; break;
    case T.SET_SLOT_FX: m = { type, slot: checkSlot(r.u8()), fx: r.u8() }; break;
    case T.SET_PARAM: m = { type, slot: checkSlot(r.u8()), idx: checkRange('idx', r.u8(), 0, 3), v: u162n(r.u16()) }; break;
    case T.SET_WET: m = { type, slot: checkSlot(r.u8()), v: u162n(r.u16()) }; break;
    case T.SET_BYPASS: m = { type, slot: checkSlot(r.u8()), on: r.u8() !== 0 }; break;
    case T.SET_GLOBAL_BYPASS: m = { type, on: r.u8() !== 0 }; break;
    case T.SET_MASTER: m = { type, v: u162n(r.u16()) }; break;
    case T.SET_INPUT_GAIN: m = { type, cdB: checkRange('cdB', r.i16(), -1200, 1200) }; break;
    case T.SET_ORDER: {
      const order = [r.u8(), r.u8(), r.u8(), r.u8()];
      if (!isPermutation(order)) throw new ProtocolError('order must be a permutation of 0..3');
      m = { type, order }; break;
    }
    case T.SET_BPM: m = { type, bpm10: checkRange('bpm10', r.u16(), 300, 3000) }; break;
    case T.FACTORY_RESET: m = { type, confirm: r.u8() }; break;
    case T.SUBSCRIBE: m = { type, hz: checkRange('hz', r.u8(), 0, 20) }; break;

    case T.HELLO_RSP: {
      m = { type, proto: r.u8(), fw: r.str(), fxCount: r.u8(), sampleRate: r.u32(), block: r.u16(), poolBytes: r.u32(), cpuBudget: r.u16() };
      break;
    }
    case T.FX_DESC: {
      const id = r.u8(), name = r.str(), np = r.u8();
      if (np > 4) throw new ProtocolError('too many params');
      const cpuCycles = r.u16(), memBytes = r.u32(), tailMs = r.u32(), mm = r.u8();
      const params = [];
      for (let i = 0; i < np; i++) {
        const p = { name: r.str(), unit: r.str(), min: r.f32(), max: r.f32(), def: r.f32(), scale: SCALES[r.u8()], labels: '' };
        if (!p.scale) throw new ProtocolError('unknown scale');
        p.labels = r.str();
        params.push(p);
      }
      m = { type, id, name, cpuCycles, memBytes, tailMs, mixMode: MIX_MODES[mm] ?? 'crossfade', params };
      break;
    }
    case T.STATE: m = decodeState(r); break;
    case T.RESULT: m = { type, requestType: r.u8(), code: r.u8() }; break;
    case T.EVENT: {
      const st = r.u8();
      if (!SETTER_TYPES.includes(st)) throw new ProtocolError('event carries non-setter type');
      return { type, setter: decodePayload(st, payload.subarray(1)) };
    }
    case T.ADMISSION_ERROR: m = { type, slot: checkSlot(r.u8()), fx: r.u8(), code: r.u8() }; break;
    case T.METERS:
      m = { type, inPeak: r.u16(), outPeak: r.u16(), cpuPct: r.u8(), cpuPeakPct: r.u8(), memUsed: r.u32(),
        inClips: r.u32(), outClips: r.u32(), underruns: r.u32(), overruns: r.u32(), nanEvents: r.u32(),
        uiMode: r.u8(), editSlot: r.u8(), globalBypass: (r.u8() & 1) === 1 };
      break;
    default: throw new ProtocolError(`unknown message type 0x${type.toString(16)}`);
  }
  r.done();
  return m;
}

function isPermutation(o) {
  return Array.isArray(o) && o.length === 4 && [0, 1, 2, 3].every((v) => o.includes(v)) && o.every(Number.isInteger);
}

// STATE: fx_id[4] bypass[4] order[4] global u8, reserved u8, fx_count u8, in_gain_cdB i16, master u16n,
// bpm10 u16, wet[4] u16n, params[4][fx_count][4] u16n, ui_mode u8, edit_slot u8
function encodeState(w, s) {
  const fxCount = s.fxCount;
  for (let i = 0; i < 4; i++) w.u8(s.fxId[i]);
  for (let i = 0; i < 4; i++) w.u8(s.bypass[i] ? 1 : 0);
  for (let i = 0; i < 4; i++) w.u8(s.order[i]);
  w.u8(s.globalBypass ? 1 : 0).u8(0).u8(fxCount).i16(s.inGainCdB).u16(n2u16(s.master)).u16(s.bpm10);
  for (let i = 0; i < 4; i++) w.u16(n2u16(s.wet[i]));
  for (let sl = 0; sl < 4; sl++) for (let f = 0; f < fxCount; f++) for (let p = 0; p < 4; p++) w.u16(n2u16(s.params[sl][f][p]));
  w.u8(s.uiMode).u8(s.editSlot);
}
function decodeState(r) {
  const fxId = [r.u8(), r.u8(), r.u8(), r.u8()];
  const bypass = [r.u8() !== 0, r.u8() !== 0, r.u8() !== 0, r.u8() !== 0];
  const order = [r.u8(), r.u8(), r.u8(), r.u8()];
  if (!isPermutation(order)) throw new ProtocolError('state: bad order');
  const globalBypass = r.u8() !== 0; r.u8();
  const fxCount = r.u8();
  if (fxCount === 0) throw new ProtocolError('state: fx_count 0');
  const inGainCdB = r.i16(), master = u162n(r.u16()), bpm10 = r.u16();
  const wet = [u162n(r.u16()), u162n(r.u16()), u162n(r.u16()), u162n(r.u16())];
  const params = [];
  for (let sl = 0; sl < 4; sl++) {
    const per = [];
    for (let f = 0; f < fxCount; f++) per.push([u162n(r.u16()), u162n(r.u16()), u162n(r.u16()), u162n(r.u16())]);
    params.push(per);
  }
  const uiMode = r.u8(), editSlot = r.u8();
  return { type: T.STATE, fxId, bypass, order, globalBypass, fxCount, inGainCdB, master, bpm10, wet, params, uiMode, editSlot };
}

/** Build a ready-to-send frame for a message object. */
export function buildFrame(msg, seq = 0) {
  const payload = encodePayload(msg);
  return encodeFrame({ type: msg.type, seq, payload }, msg.type === T.STATE ? MAX_BODY_RX : MAX_BODY);
}

/** Parse a decodeFrame() result into {seq, msg}; returns {ok:false,error} on any problem. */
export function parseFrame(frame) {
  if (!frame.ok) return frame;
  try { return { ok: true, seq: frame.seq, msg: decodePayload(frame.type, frame.payload) }; }
  catch (e) { return { ok: false, error: e.message, seq: frame.seq, type: frame.type }; }
}
