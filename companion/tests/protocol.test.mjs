import test from 'node:test';
import assert from 'node:assert/strict';
import {
  cobsEncode, cobsDecode, crc16, encodeFrame, decodeFrame, FrameDecoder, buildFrame, parseFrame,
  encodePayload, decodePayload, T, n2u16, u162n, ProtocolError,
} from '../js/protocol.js';
import { descFromFx } from '../js/fx.js';
import { fxTable } from './helpers.mjs';

const bytes = (...a) => Uint8Array.from(a);

test('COBS: known vectors', () => {
  assert.deepEqual([...cobsEncode(bytes())], [1]);
  assert.deepEqual([...cobsEncode(bytes(0))], [1, 1]);
  assert.deepEqual([...cobsEncode(bytes(0, 0))], [1, 1, 1]);
  assert.deepEqual([...cobsEncode(bytes(0x11, 0x22, 0x00, 0x33))], [3, 0x11, 0x22, 2, 0x33]);
  assert.deepEqual([...cobsEncode(bytes(0x11, 0x22, 0x33, 0x44))], [5, 0x11, 0x22, 0x33, 0x44]);
});

test('COBS: round trips incl. zeros and 254-byte runs', () => {
  const cases = [[], [0], [0, 0, 0], [1], [1, 0, 2, 0, 0, 3]];
  for (const n of [253, 254, 255, 256, 507, 508, 509, 600]) {
    cases.push(Array.from({ length: n }, (_, i) => (i % 255) + 1));              // no zeros, runs of 254+
    cases.push(Array.from({ length: n }, (_, i) => (i % 7 === 0 ? 0 : 5)));      // sprinkled zeros
    cases.push([...Array.from({ length: n }, () => 9), 0]);                     // run then trailing zero
    cases.push([0, ...Array.from({ length: n }, () => 9)]);
  }
  for (const c of cases) {
    const enc = cobsEncode(Uint8Array.from(c));
    assert.ok(!enc.includes(0), 'encoded data contains no zero');
    assert.deepEqual([...cobsDecode(enc)], c, `len ${c.length}`);
  }
});

test('COBS: random round trip', () => {
  let s = 12345;
  const rnd = () => (s = (s * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  for (let i = 0; i < 500; i++) {
    const d = Uint8Array.from({ length: Math.floor(rnd() * 700) }, () => (rnd() < 0.2 ? 0 : Math.floor(rnd() * 256)));
    assert.deepEqual([...cobsDecode(cobsEncode(d))], [...d]);
  }
});

test('COBS: malformed input is rejected', () => {
  assert.throws(() => cobsDecode(bytes(0)), ProtocolError);          // zero code
  assert.throws(() => cobsDecode(bytes(5, 1, 2)), ProtocolError);    // block overruns
  assert.throws(() => cobsDecode(bytes(3, 1, 0, 2)), ProtocolError); // zero inside block
});

test('CRC-16/CCITT-FALSE known vector', () => {
  assert.equal(crc16(new TextEncoder().encode('123456789')), 0x29B1);
  assert.equal(crc16(bytes()), 0xFFFF);
});

test('frame: encode/decode and layout', () => {
  const f = encodeFrame({ type: T.SET_MASTER, seq: 7, payload: bytes(0x34, 0x12) });
  assert.equal(f[f.length - 1], 0);
  assert.ok(!f.subarray(0, f.length - 1).includes(0));
  const body = cobsDecode(f.subarray(0, f.length - 1));
  assert.deepEqual([...body.subarray(0, 5)], [1, T.SET_MASTER, 7, 0x34, 0x12]);
  const crc = crc16(body.subarray(0, 5));
  assert.equal(body[5], crc & 0xff); assert.equal(body[6], crc >> 8);   // little-endian CRC
  const d = decodeFrame(f.subarray(0, f.length - 1));
  assert.ok(d.ok); assert.equal(d.type, T.SET_MASTER); assert.equal(d.seq, 7); assert.deepEqual([...d.payload], [0x34, 0x12]);
});

test('frame: payload limits', () => {
  assert.doesNotThrow(() => encodeFrame({ type: 1, seq: 1, payload: new Uint8Array(500) }));
  assert.throws(() => encodeFrame({ type: 1, seq: 1, payload: new Uint8Array(501) }), ProtocolError); // spec: payload 0..500
});

test('frame: corrupt frames are rejected (every single-bit flip)', () => {
  const f = encodeFrame({ type: T.SET_PARAM, seq: 3, payload: bytes(1, 2, 0x10, 0x20) });
  const inner = f.subarray(0, f.length - 1);
  let accepted = 0;
  for (let i = 0; i < inner.length; i++) for (let b = 0; b < 8; b++) {
    const c = Uint8Array.from(inner); c[i] ^= 1 << b;
    if (c[i] === 0) continue;   // would become a delimiter; framing handles separately
    const d = decodeFrame(c);
    const same = d.ok && d.type === T.SET_PARAM && d.seq === 3;
    if (d.ok) accepted++;
    assert.ok(!same || false, 'corrupted frame must not decode to the original');
  }
  assert.equal(accepted, 0);
  assert.equal(decodeFrame(bytes(1, 1)).ok, false);           // too short
  assert.equal(decodeFrame(bytes()).ok, false);
});

test('frame: wrong protocol version is rejected', () => {
  const body = bytes(2, T.HELLO, 1); const crc = crc16(body);
  const full = Uint8Array.from([...body, crc & 255, crc >> 8]);
  assert.equal(decodeFrame(cobsEncode(full)).ok, false);
});

test('FrameDecoder: chunked, back-to-back, garbage between frames', () => {
  const a = buildFrame({ type: T.HELLO }, 1), b = buildFrame({ type: T.SET_MASTER, v: 0.5 }, 2);
  const stream = Uint8Array.from([...a, 9, 9, 9, 0, ...b]);
  const dec = new FrameDecoder(); const out = [];
  for (let i = 0; i < stream.length; i++) out.push(...dec.push(stream.subarray(i, i + 1)));
  assert.equal(out.length, 3);
  assert.deepEqual(out.map((x) => x.ok), [true, false, true]);
  assert.equal(out[2].seq, 2);
});

test('FrameDecoder: runaway data without delimiter does not grow unbounded', () => {
  const dec = new FrameDecoder();
  dec.push(new Uint8Array(100000).fill(7));
  assert.ok(dec.buf.length < 2000);
  const r = dec.push(bytes(0));
  assert.equal(r[0].ok, false);
  assert.ok(dec.push(buildFrame({ type: T.HELLO }, 1))[0].ok);   // recovers
});

test('u16n quantisation', () => {
  assert.equal(n2u16(0), 0); assert.equal(n2u16(1), 65535); assert.equal(n2u16(-3), 0); assert.equal(n2u16(9), 65535);
  assert.equal(n2u16(NaN), 0);
  assert.ok(Math.abs(u162n(n2u16(0.3)) - 0.3) < 1 / 65535);
});

const near = (a, b) => Math.abs(a - b) <= 1 / 65535;

test('every host->device message round trips', () => {
  const msgs = [
    { type: T.HELLO }, { type: T.GET_FX, idx: 10 }, { type: T.GET_STATE },
    { type: T.SET_SLOT_FX, slot: 3, fx: 10 }, { type: T.SET_PARAM, slot: 2, idx: 3, v: 0.25 },
    { type: T.SET_WET, slot: 1, v: 1 }, { type: T.SET_BYPASS, slot: 0, on: true }, { type: T.SET_BYPASS, slot: 0, on: false },
    { type: T.SET_GLOBAL_BYPASS, on: true }, { type: T.SET_MASTER, v: 0.75 },
    { type: T.SET_INPUT_GAIN, cdB: -1200 }, { type: T.SET_INPUT_GAIN, cdB: 1200 }, { type: T.SET_INPUT_GAIN, cdB: -1 },
    { type: T.SET_ORDER, order: [3, 1, 0, 2] }, { type: T.SET_BPM, bpm10: 300 }, { type: T.SET_BPM, bpm10: 3000 },
    { type: T.SAVE_NOW }, { type: T.FACTORY_RESET, confirm: 0xA5 }, { type: T.SUBSCRIBE, hz: 20 },
  ];
  for (const m of msgs) {
    const f = buildFrame(m, 9);
    const r = parseFrame(decodeFrame(f.subarray(0, f.length - 1)));
    assert.ok(r.ok, `${m.type}: ${r.error}`);
    assert.equal(r.seq, 9);
    for (const k of Object.keys(m)) {
      if (k === 'v') assert.ok(near(r.msg.v, m.v)); else assert.deepEqual(r.msg[k], m[k], k);
    }
  }
});

test('host messages: argument validation', () => {
  assert.throws(() => encodePayload({ type: T.SET_SLOT_FX, slot: 4, fx: 0 }), ProtocolError);
  assert.throws(() => encodePayload({ type: T.SET_PARAM, slot: 0, idx: 4, v: 0 }), ProtocolError);
  assert.throws(() => encodePayload({ type: T.SET_INPUT_GAIN, cdB: 1201 }), ProtocolError);
  assert.throws(() => encodePayload({ type: T.SET_ORDER, order: [0, 0, 1, 2] }), ProtocolError);
  assert.throws(() => encodePayload({ type: T.SET_BPM, bpm10: 299 }), ProtocolError);
  assert.throws(() => encodePayload({ type: T.SUBSCRIBE, hz: 21 }), ProtocolError);
  assert.throws(() => decodePayload(T.SET_MASTER, bytes(1)), ProtocolError);          // short
  assert.throws(() => decodePayload(T.SET_MASTER, bytes(1, 2, 3)), ProtocolError);    // trailing
  assert.throws(() => decodePayload(0x55, bytes()), ProtocolError);                   // unknown type
});

test('HELLO_RSP round trip', () => {
  const m = { type: T.HELLO_RSP, proto: 1, fw: 'sp1fx-0.1.0', fxCount: 11, sampleRate: 48000, block: 48, poolBytes: 153600, cpuBudget: 850 };
  assert.deepEqual(decodePayload(T.HELLO_RSP, encodePayload(m)), m);
});

test('FX_DESC round trip for every effect in the table, and fits in 250 bytes', () => {
  for (const fx of fxTable) {
    const m = descFromFx(fx);
    const p = encodePayload(m);
    assert.ok(p.length <= 250, `${fx.name} payload ${p.length}`);
    const d = decodePayload(T.FX_DESC, p);
    assert.equal(d.id, fx.id); assert.equal(d.name, fx.name); assert.equal(d.cpuCycles, fx.cpuCycles);
    assert.equal(d.memBytes, fx.memBytes); assert.equal(d.tailMs, fx.tailMs); assert.equal(d.mixMode, fx.mixMode);
    assert.equal(d.params.length, fx.params.length);
    d.params.forEach((q, i) => {
      const o = fx.params[i];
      assert.equal(q.name, o.name); assert.equal(q.unit, o.unit); assert.equal(q.scale, o.scale);
      assert.ok(Math.abs(q.min - o.min) < 1e-5 * Math.max(1, Math.abs(o.min)));
      assert.ok(Math.abs(q.max - o.max) < 1e-5 * Math.max(1, Math.abs(o.max)));
      assert.ok(Math.abs(q.def - o.def) < 1e-5 * Math.max(1, Math.abs(o.def)));
      assert.equal(q.labels, o.labels || '');
    });
  }
});

function sampleState() {
  const n = fxTable.length;
  return {
    type: T.STATE, fxId: [5, 1, 3, 4], bypass: [true, false, false, true], order: [2, 0, 3, 1], globalBypass: true, fxCount: n,
    inGainCdB: -350, master: 0.5, bpm10: 1234, wet: [0.1, 0.2, 0.3, 1],
    params: Array.from({ length: 4 }, (_, s) => Array.from({ length: n }, (_, f) => [0, 1, 2, 3].map((i) => ((s * 31 + f * 7 + i * 3) % 101) / 100))),
    uiMode: 1, editSlot: 2,
  };
}

test('STATE round trip (exact layout size)', () => {
  const s = sampleState();
  const p = encodePayload(s);
  const n = fxTable.length;
  assert.equal(p.length, 4 + 4 + 4 + 1 + 1 + 1 + 2 + 2 + 2 + 8 + 4 * n * 4 * 2 + 2);
  const d = decodePayload(T.STATE, p);
  for (const k of ['fxId', 'bypass', 'order', 'globalBypass', 'fxCount', 'inGainCdB', 'bpm10', 'uiMode', 'editSlot']) assert.deepEqual(d[k], s[k], k);
  assert.ok(near(d.master, s.master));
  d.wet.forEach((w, i) => assert.ok(near(w, s.wet[i])));
  for (let a = 0; a < 4; a++) for (let f = 0; f < n; f++) for (let i = 0; i < 4; i++) assert.ok(near(d.params[a][f][i], s.params[a][f][i]));
  // spec gap: payload exceeds the 250-byte payload limit, so it needs the larger receive limit
  assert.ok(p.length > 250);
  const f = buildFrame(s, 4);
  assert.ok(parseFrame(decodeFrame(f.subarray(0, f.length - 1))).ok);
});

test('STATE rejects bad order / truncated', () => {
  const s = sampleState();
  const p = encodePayload(s); p[8] = 0; p[9] = 0;
  assert.throws(() => decodePayload(T.STATE, p), ProtocolError);
  assert.throws(() => decodePayload(T.STATE, encodePayload(s).subarray(0, 100)), ProtocolError);
});

test('RESULT, EVENT, ADMISSION_ERROR, METERS round trip', () => {
  assert.deepEqual(decodePayload(T.RESULT, encodePayload({ type: T.RESULT, requestType: 0x10, code: 2 })), { type: T.RESULT, requestType: 0x10, code: 2 });
  assert.deepEqual(decodePayload(T.ADMISSION_ERROR, encodePayload({ type: T.ADMISSION_ERROR, slot: 2, fx: 3, code: 2 })), { type: T.ADMISSION_ERROR, slot: 2, fx: 3, code: 2 });
  const setters = [
    { type: T.SET_PARAM, slot: 1, idx: 2, v: 0.5 }, { type: T.SET_SLOT_FX, slot: 0, fx: 4 }, { type: T.SET_WET, slot: 3, v: 0.2 },
    { type: T.SET_BYPASS, slot: 2, on: true }, { type: T.SET_GLOBAL_BYPASS, on: false }, { type: T.SET_MASTER, v: 1 },
    { type: T.SET_INPUT_GAIN, cdB: 500 }, { type: T.SET_ORDER, order: [1, 0, 2, 3] }, { type: T.SET_BPM, bpm10: 900 },
  ];
  for (const g of setters) {
    const e = decodePayload(T.EVENT, encodePayload({ type: T.EVENT, setter: g }));
    assert.equal(e.setter.type, g.type);
    for (const k of Object.keys(g)) { if (k === 'v') assert.ok(near(e.setter.v, g.v)); else assert.deepEqual(e.setter[k], g[k]); }
  }
  assert.throws(() => decodePayload(T.EVENT, bytes(T.HELLO)), ProtocolError);   // events carry setters only
  const mt = { type: T.METERS, inPeak: 32768, outPeak: 1000, cpuPct: 63, cpuPeakPct: 80, memUsed: 119920, inClips: 1, outClips: 2, underruns: 3, overruns: 4, nanEvents: 5, uiMode: 1, editSlot: 3, globalBypass: true };
  assert.deepEqual(decodePayload(T.METERS, encodePayload(mt)), mt);
  assert.equal(decodePayload(T.METERS, encodePayload({ ...mt, globalBypass: false })).globalBypass, false);
});

test('PRESET_LOADED (0xA3) round trip', () => {
  const f = buildFrame({ type: T.PRESET_LOADED, idx: 5, bpm10: 1234 }, 0);
  const r = parseFrame(decodeFrame(f.subarray(0, f.length - 1)));
  assert.ok(r.ok);
  assert.deepEqual(r.msg, { type: T.PRESET_LOADED, idx: 5, bpm10: 1234 });
});

test('SET_SYNC round trip and validation', () => {
  for (const on of [true, false]) {
    const f = buildFrame({ type: T.SET_SYNC, on }, 4);
    const r = parseFrame(decodeFrame(f.subarray(0, f.length - 1)));
    assert.ok(r.ok); assert.deepEqual(r.msg, { type: T.SET_SYNC, on });
  }
  const bad = encodeFrame({ type: T.SET_SYNC, seq: 1, payload: Uint8Array.of(2) });
  assert.equal(parseFrame(decodeFrame(bad.subarray(0, bad.length - 1))).ok, false);
});
