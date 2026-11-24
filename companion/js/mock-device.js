// In-browser mock of the SP-1 FX device side of PROTOCOL.md. No DOM dependencies.
// It speaks the real wire format (COBS + CRC framing) so the host runs its real client code path.
// This is a simulation written from the spec, NOT firmware and NOT evidence about real hardware.
import {
  T, RESULT, FrameDecoder, buildFrame, parseFrame, FACTORY_RESET_CONFIRM, SETTER_TYPES,
} from './protocol.js';
import { descFromFx, checkAdmission, defaultNorm, POOL_BYTES, CPU_BUDGET } from './fx.js';

export function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6D2B79F5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export class MockDevice {
  /** @param fxTable canonical table (see fx.js). */
  constructor(fxTable, { rng = Math.random, fw = 'mock-0.1 (simulated)', poolBytes = POOL_BYTES, cpuBudget = CPU_BUDGET } = {}) {
    this.fx = fxTable; this.rng = rng; this.fw = fw; this.pool = poolBytes; this.cpuBudget = cpuBudget;
    this.decoder = new FrameDecoder();
    this.onOutput = null;            // (Uint8Array) => void
    this.sampleRate = 48000; this.block = 48;
    this.hz = 0; this.meterAcc = 0;
    this.log = [];                   // received setter messages, for tests
    this.counters = { inClips: 0, outClips: 0, underruns: 0, overruns: 0, nanEvents: 0 };
    this.cpuPeak = 0; this.level = 0.4; this.peakIn = 0; this.peakOut = 0;
    this.factoryReset();
  }

  factoryReset() {
    const fx = this.fx;
    // The firmware default is "all slots bypassed, master 75%" (docs/STATUS.md). Assignments here are a demo choice.
    const initial = ['saturation', 'filter', 'delay', 'reverb'];
    const idOf = (name) => Math.max(0, fx.findIndex((f) => f.name.toLowerCase().replace(/ /g, '_') === name));
    this.s = {
      fxId: initial.map(idOf), bypass: [true, true, true, true], order: [0, 1, 2, 3], globalBypass: false,
      inGainCdB: 0, master: 0.75, bpm10: 1200, wet: [1, 1, 1, 1],
      params: Array.from({ length: 4 }, () => fx.map((f) => [0, 1, 2, 3].map((i) => (f.params[i] ? defaultNorm(f.params[i]) : 0)))),
      uiMode: 0, editSlot: 0,
    };
    for (let i = 0; i < 4; i++) this.s.wet[i] = fx[this.s.fxId[i]].mixMode === 'send' ? 0.3 : 1;
  }

  // ----- wire I/O -----
  receive(bytes) {
    for (const f of this.decoder.push(bytes)) {
      const r = parseFrame(f);
      if (!r.ok) continue;                       // dropped silently, as specified
      this._handle(r.msg, r.seq);
    }
  }
  _emit(msg, seq = 0) { this.onOutput?.(buildFrame(msg, seq)); }
  _result(req, code, seq) { this._emit({ type: T.RESULT, requestType: req.type, code }, seq); }

  // ----- admission -----
  mem() { return this.s.fxId.reduce((a, id) => a + this.fx[id].memBytes, 0); }
  cpu() { return this.s.fxId.reduce((a, id) => a + this.fx[id].cpuCycles, 0); }
  admit(slot, fxId) {
    const list = this.s.fxId.map((id, i) => (i === slot ? fxId : id)).map((id) => this.fx[id]);
    return checkAdmission(list, this.pool, this.cpuBudget);
  }

  /** Apply a setter to device state. Returns a RESULT code; state is unchanged on non-zero. */
  _apply(g) {
    const s = this.s;
    switch (g.type) {
      case T.SET_SLOT_FX: {
        if (g.fx >= this.fx.length) return RESULT.BAD_ARG;
        const a = this.admit(g.slot, g.fx);
        if (!a.ok) return a.code;
        s.fxId[g.slot] = g.fx; return RESULT.OK;
      }
      case T.SET_PARAM: s.params[g.slot][s.fxId[g.slot]][g.idx] = g.v; return RESULT.OK;
      case T.SET_WET: s.wet[g.slot] = g.v; return RESULT.OK;
      case T.SET_BYPASS: s.bypass[g.slot] = g.on; return RESULT.OK;
      case T.SET_GLOBAL_BYPASS: s.globalBypass = g.on; return RESULT.OK;
      case T.SET_MASTER: s.master = g.v; return RESULT.OK;
      case T.SET_INPUT_GAIN: s.inGainCdB = g.cdB; return RESULT.OK;
      case T.SET_ORDER: s.order = g.order.slice(); return RESULT.OK;
      case T.SET_BPM: s.bpm10 = g.bpm10; return RESULT.OK;
      default: return RESULT.BAD_ARG;
    }
  }

  _handle(m, seq) {
    if (SETTER_TYPES.includes(m.type)) {
      this.log.push(m);
      // Host-originated changes are acknowledged with RESULT and never echoed as events.
      this._result(m, this._apply(m), seq);
      return;
    }
    switch (m.type) {
      case T.HELLO:
        this._emit({ type: T.HELLO_RSP, proto: 1, fw: this.fw, fxCount: this.fx.length, sampleRate: this.sampleRate,
          block: this.block, poolBytes: this.pool, cpuBudget: this.cpuBudget }, seq);
        break;
      case T.GET_FX:
        if (m.idx >= this.fx.length) this._result(m, RESULT.BAD_ARG, seq);
        else this._emit(descFromFx(this.fx[m.idx]), seq);
        break;
      case T.GET_STATE: this._emit({ type: T.STATE, ...this.s, fxCount: this.fx.length }, seq); break;
      case T.SAVE_NOW: this._result(m, RESULT.OK, seq); break;
      case T.FACTORY_RESET:
        if (m.confirm !== FACTORY_RESET_CONFIRM) { this._result(m, RESULT.BAD_ARG, seq); break; }
        this.factoryReset(); this._result(m, RESULT.OK, seq); break;
      case T.SUBSCRIBE: this.hz = m.hz; this.meterAcc = 0; this._result(m, RESULT.OK, seq); break;
      default: break;
    }
  }

  // ----- simulated physical controls (device-originated changes -> 0xA0 events) -----
  /** A device-side change, as if from a fader/button/other host. Emits 0xA0 on success, 0xA1 when admission fails. */
  deviceChange(setter) {
    const code = this._apply(setter);
    if (code === RESULT.OK) { this._emit({ type: T.EVENT, setter }); return true; }
    if (setter.type === T.SET_SLOT_FX) this._emit({ type: T.ADMISSION_ERROR, slot: setter.slot, fx: setter.fx, code });
    return false;
  }
  simulateFader(slot, idx, v) { return this.deviceChange({ type: T.SET_PARAM, slot, idx, v }); }
  /** Physical effect-cycle on a slot: next effect, wrapping through None. */
  simulateCycleFx(slot) {
    const next = (this.s.fxId[slot] + 1) % this.fx.length;
    return this.deviceChange({ type: T.SET_SLOT_FX, slot, fx: next });
  }

  // ----- simulated meters -----
  tick(dtMs) {
    const s = this.s;
    // slow random walk of the "programme level"
    this.level = Math.min(1.3, Math.max(0.05, this.level + (this.rng() - 0.5) * 0.12));
    const gain = Math.pow(10, s.inGainCdB / 2000);
    const active = s.fxId.reduce((a, id, i) => a + (s.bypass[i] || s.globalBypass ? 0 : this.fx[id].cpuCycles), 0);
    const cpuPct = Math.min(100, ((active + 60) / this.cpuBudget) * 100 + this.rng() * 2);
    this.cpuPeak = Math.max(this.cpuPeak, cpuPct);
    const inPk = Math.min(40000, 32768 * this.level * gain * (0.7 + 0.3 * this.rng()));
    const outPk = Math.min(40000, inPk * s.master * (s.globalBypass ? 1 : 1.1));
    this.peakIn = Math.max(this.peakIn, inPk); this.peakOut = Math.max(this.peakOut, outPk);
    if (this.hz <= 0) return;
    this.meterAcc += dtMs;
    const period = 1000 / this.hz;
    if (this.meterAcc < period) return;
    this.meterAcc = 0;
    if (this.peakIn >= 32768) this.counters.inClips++;
    if (this.peakOut >= 32768) this.counters.outClips++;
    this._emit({
      type: T.METERS, inPeak: Math.round(this.peakIn), outPeak: Math.round(this.peakOut), cpuPct: Math.round(cpuPct),
      cpuPeakPct: Math.round(this.cpuPeak), memUsed: this.mem(), ...this.counters,
      uiMode: s.uiMode, editSlot: s.editSlot, globalBypass: s.globalBypass,
    });
    this.peakIn = 0; this.peakOut = 0;
  }
}

/**
 * In-memory loopback transport: host bytes -> device.receive(), device bytes -> transport.onData.
 * Delivery is asynchronous (microtask), like a real serial port. Fault injection for tests/demo:
 * dropNext(n) discards the next n host->device writes.
 */
export function createLoopback(device) {
  const t = {
    onData: null, onClose: null, closed: false, _drop: 0,
    write(bytes) { if (t.closed) throw new Error('closed'); if (t._drop > 0) { t._drop--; return; } const c = bytes.slice(); queueMicrotask(() => device.receive(c)); },
    close() { if (t.closed) return; t.closed = true; t.onClose?.(); },
    dropNext(n = 1) { t._drop = n; },
  };
  device.onOutput = (bytes) => { const c = bytes.slice(); queueMicrotask(() => { if (!t.closed) t.onData?.(c); }); };
  return t;
}
