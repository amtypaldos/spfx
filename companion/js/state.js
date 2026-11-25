// Application model + tiny store. No DOM dependencies.
import { T, CAP, CAPS_RC6 } from './protocol.js';
import { defaultNorm, SLOT_COUNT } from './fx.js';

export function createModel() {
  return {
    conn: 'disconnected',          // disconnected | connecting | connected
    mode: null,                    // 'serial' | 'demo' | null
    fw: '', proto: 0, sampleRate: 0, block: 0, caps: CAPS_RC6,
    poolBytes: 0, cpuBudget: 0,
    fxTable: [],
    synced: false,                 // true once a STATE has been received
    slots: Array.from({ length: SLOT_COUNT }, () => ({ fx: 0, bypass: false, wet: 1, params: [] })),
    order: [0, 1, 2, 3],
    globalBypass: false, syncOn: false, outMode: 0, inGainDb: 0, master: 0.75, bpm: 120,
    uiMode: 0, editSlot: 0,
    meters: { inPeak: 0, outPeak: 0, cpuPct: 0, cpuPeakPct: 0, memUsed: 0, inClips: 0, outClips: 0, underruns: 0, overruns: 0, nanEvents: 0 },
  };
}

/** True when the connected firmware declares the feature (HELLO_RSP caps, PROTOCOL.md). */
export function hasCap(m, bit) { return ((m.caps >>> 0) & bit) !== 0; }

/** Controls the connected firmware does not support, for graying out:
 *  element ids, 'outMode:<value>' for level options, 'battery', 'save'. */
export function unsupported(m) {
  const u = [];
  if (!hasCap(m, CAP.SYNC)) u.push('gSync');
  if (!hasCap(m, CAP.OUT_LINE)) u.push('outMode:1');
  if (!hasCap(m, CAP.OUT_LOUD)) u.push('outMode:2');
  if (!hasCap(m, CAP.BATTERY)) u.push('battery');
  if (!hasCap(m, CAP.SAVE)) u.push('save');
  return u;
}

/** Make per-slot, per-effect remembered params (norm) from defaults. */
export function defaultParams(fxTable) {
  return Array.from({ length: SLOT_COUNT }, () => fxTable.map((fx) => [0, 1, 2, 3].map((i) => (fx.params[i] ? defaultNorm(fx.params[i]) : 0))));
}

export function applyState(m, s) {
  for (let i = 0; i < SLOT_COUNT; i++) {
    m.slots[i] = { fx: s.fxId[i], bypass: s.bypass[i], wet: s.wet[i], params: s.params[i] };
  }
  m.order = s.order.slice();
  m.globalBypass = s.globalBypass;
  m.syncOn = !!s.syncOn;
  m.outMode = [0, 1, 2].includes(s.outMode) ? s.outMode : 0;
  m.inGainDb = s.inGainCdB / 100;
  m.master = s.master;
  m.bpm = s.bpm10 / 10;
  m.uiMode = s.uiMode; m.editSlot = s.editSlot;
  m.synced = true;
}

/** Apply a setter message (host edit already confirmed, or a device 0xA0 event) to the model. */
export function applySetter(m, g) {
  switch (g.type) {
    case T.SET_SLOT_FX: m.slots[g.slot].fx = g.fx; break;
    case T.SET_PARAM: {
      const sl = m.slots[g.slot];
      (sl.params[sl.fx] ||= [0, 0, 0, 0])[g.idx] = g.v; break;
    }
    case T.SET_WET: m.slots[g.slot].wet = g.v; break;
    case T.SET_BYPASS: m.slots[g.slot].bypass = g.on; break;
    case T.SET_GLOBAL_BYPASS: m.globalBypass = g.on; break;
    case T.SET_MASTER: m.master = g.v; break;
    case T.SET_INPUT_GAIN: m.inGainDb = g.cdB / 100; break;
    case T.SET_ORDER: m.order = g.order.slice(); break;
    case T.SET_BPM: m.bpm = g.bpm10 / 10; break;
    case T.SET_SYNC: m.syncOn = g.on; break;
    case T.SET_OUTPUT_MODE: m.outMode = g.mode; break;
    default: return false;
  }
  return true;
}

export function applyMeters(m, x) {
  Object.assign(m.meters, {
    inPeak: x.inPeak, outPeak: x.outPeak, cpuPct: x.cpuPct, cpuPeakPct: x.cpuPeakPct, memUsed: x.memUsed,
    inClips: x.inClips, outClips: x.outClips, underruns: x.underruns, overruns: x.overruns, nanEvents: x.nanEvents,
    usbPower: !!x.usbPower, charging: !!x.charging, guardForced: !!x.guardForced, guardShed: x.guardShed ?? 0,
    battPct: Number.isInteger(x.battPct) ? x.battPct : null,
  });
  m.globalBypass = x.globalBypass;
  m.uiMode = x.uiMode; m.editSlot = x.editSlot;
}

export class Store {
  constructor(model = createModel()) { this.model = model; this.subs = new Set(); this.queued = false; }
  subscribe(fn) { this.subs.add(fn); return () => this.subs.delete(fn); }
  /** Mutate the model then notify (notifications are batched to one microtask). */
  update(fn) {
    fn(this.model);
    if (this.queued) return;
    this.queued = true;
    queueMicrotask(() => { this.queued = false; for (const f of this.subs) f(this.model); });
  }
}
