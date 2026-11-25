// Controller: connects store <-> protocol client. No DOM dependencies (testable in Node).
import { T, RESULT, FACTORY_RESET_CONFIRM, CAP } from './protocol.js';
import { Client, Coalescer, RequestTimeout } from './client.js';
import { applyState, applySetter, applyMeters, createModel, defaultParams, hasCap } from './state.js';
import { fxFromDesc, fxKey, SLOT_COUNT } from './fx.js';

const METER_HZ = 10;

export const RESULT_TEXT = {
  [RESULT.BAD_ARG]: 'the device rejected the value (bad argument)',
  [RESULT.NO_MEMORY]: 'not enough DSP memory',
  [RESULT.NO_CPU]: 'not enough CPU headroom',
  [RESULT.BUSY]: 'the device is busy, try again',
  [RESULT.STORAGE]: 'the device could not save its settings (storage error)',
};

export class Controller {
  /**
   * @param store Store from state.js
   * @param fallbackTable canonical fx table used before/without a device
   */
  constructor(store, fallbackTable, { paceMs = 34, rateMs = 34 } = {}) {
    this.store = store; this.fallback = fallbackTable; this.paceMs = paceMs;
    this.client = null; this.noticeFns = new Set(); this.timeouts = 0;
    this.coalescer = new Coalescer((msg) => { this._send(msg).then((c) => this._finish(msg, c)); }, rateMs);
    this.sent = 0;
    this.store.update((m) => { m.fxTable = fallbackTable; });
  }

  onNotice(fn) { this.noticeFns.add(fn); return () => this.noticeFns.delete(fn); }
  notice(level, text) { for (const f of this.noticeFns) f({ level, text }); }

  get model() { return this.store.model; }
  fxName(id) { return this.model.fxTable[id]?.name ?? `effect ${id}`; }

  // ---------- connection ----------
  async connect(transport, mode) {
    this.store.update((m) => { m.conn = 'connecting'; m.mode = mode; m.synced = false; });
    const client = new Client(transport);
    this.client = client;
    client.onEvent = (msg) => this._event(msg);
    client.onClose = () => this._closed(client);
    try {
      const hello = await client.request({ type: T.HELLO });
      if (hello.proto !== 1) throw new Error(`Device speaks protocol v${hello.proto}; this app speaks v1.`);
      const table = [];
      for (let i = 0; i < hello.fxCount; i++) {
        const d = await client.request({ type: T.GET_FX, idx: i });
        if (d.type !== T.FX_DESC) throw new Error(`Device refused effect descriptor ${i}.`);
        table.push(fxFromDesc(d));
      }
      this.store.update((m) => {
        m.fw = hello.fw; m.proto = hello.proto; m.sampleRate = hello.sampleRate; m.block = hello.block; m.caps = hello.caps;
        m.poolBytes = hello.poolBytes; m.cpuBudget = hello.cpuBudget; m.fxTable = table;
      });
      await this.refreshState();
      const r = await client.request({ type: T.SUBSCRIBE, hz: METER_HZ });
      if (r.code !== RESULT.OK) this.notice('error', 'Device refused the meter subscription; meters will stay at zero.');
      this.store.update((m) => { m.conn = 'connected'; });
    } catch (e) {
      this.notice('error', `Could not connect: ${e.message}`);
      await this.disconnect();
      throw e;
    }
  }

  async disconnect() {
    const c = this.client;
    this.client = null;
    this.coalescer.cancel();
    if (c) { try { await c.close(); } catch { /* ignore */ } }
    this.store.update((m) => {
      m.conn = 'disconnected'; m.mode = null; m.synced = false; m.fxTable = this.fallback;
      m.meters = createModel().meters;
    });
  }

  _closed(client) {
    if (this.client !== client) return;
    this.client = null; this.coalescer.cancel();
    this.notice('error', 'The device disconnected.');
    this.store.update((m) => { m.conn = 'disconnected'; m.mode = null; m.synced = false; m.fxTable = this.fallback; });
  }

  async refreshState() {
    if (!this.client) return;
    const s = await this.client.request({ type: T.GET_STATE });
    if (s.type !== T.STATE) throw new Error('Device did not return its state.');
    if (s.fxCount !== this.model.fxTable.length) throw new Error('Device state does not match its effect table.');
    this.store.update((m) => applyState(m, s));
  }

  // ---------- device -> host ----------
  _event(msg) {
    if (msg.type === T.EVENT) this.store.update((m) => { applySetter(m, msg.setter); });   // never echoed back to the device
    else if (msg.type === T.METERS) this.store.update((m) => applyMeters(m, msg));
    else if (msg.type === T.PRESET_LOADED) {
      // the device switched preset by itself (FN + rocker): everything changed, so re-read it all
      this.notice('info', `Device loaded preset ${msg.idx + 1}.`);
      this.refreshState().catch(() => {});
    }
    else if (msg.type === T.ADMISSION_ERROR) {
      this.notice('error', `The device could not switch slot ${msg.slot + 1} to ${this.fxName(msg.fx)}: ${RESULT_TEXT[msg.code] ?? 'rejected'}. Slot unchanged.`);
    }
  }

  // ---------- host -> device ----------
  describeFailure(msg, code) {
    if (msg.type === T.SET_SLOT_FX) {
      const base = `${this.fxName(msg.fx)} in slot ${msg.slot + 1}`;
      if (code === RESULT.NO_MEMORY) return `Not enough DSP memory for ${base}; slot unchanged.`;
      if (code === RESULT.NO_CPU) return `Not enough CPU headroom for ${base}; slot unchanged.`;
      return `Could not set ${base}: ${RESULT_TEXT[code] ?? `error ${code}`}; slot unchanged.`;
    }
    return `The device did not accept that change: ${RESULT_TEXT[code] ?? `error ${code}`}.`;
  }

  /** Send one setter and resolve with the RESULT code (or -1 on no response). Does not touch the model. */
  async _send(msg) {
    if (!this.client) return -1;
    try {
      this.sent++;
      const r = await this.client.request(msg);
      this.timeouts = 0;
      return r.code;
    } catch (e) {
      if (e instanceof RequestTimeout && ++this.timeouts === 2) this.notice('error', 'The device is not responding.');
      return -1;
    }
  }

  /** Optimistic edit: apply to the model now, send, and on rejection tell the user and re-sync from the device. */
  async _edit(msg, { coalesceKey = null } = {}) {
    this.store.update((m) => { applySetter(m, msg); });
    if (!this.client) return RESULT.OK;
    if (coalesceKey) {
      // coalesced sends report failures through the same path
      this.coalescer.set(coalesceKey, msg);
      return RESULT.OK;
    }
    return this._finish(msg, await this._send(msg));
  }
  async _finish(msg, code) {
    if (code === RESULT.OK) return code;
    if (code !== -1) this.notice('error', this.describeFailure(msg, code));
    try { await this.refreshState(); } catch { /* connection problems are reported elsewhere */ }
    return code;
  }

  setSlotFx(slot, fx) { return this._edit({ type: T.SET_SLOT_FX, slot, fx }); }
  setParam(slot, idx, v) { return this._edit({ type: T.SET_PARAM, slot, idx, v }, { coalesceKey: `p${slot}.${idx}` }); }
  setWet(slot, v) { return this._edit({ type: T.SET_WET, slot, v }, { coalesceKey: `w${slot}` }); }
  setBypass(slot, on) { return this._edit({ type: T.SET_BYPASS, slot, on }); }
  setGlobalBypass(on) { return this._edit({ type: T.SET_GLOBAL_BYPASS, on }); }
  setMaster(v) { return this._edit({ type: T.SET_MASTER, v }, { coalesceKey: 'master' }); }
  setInputGainDb(db) { return this._edit({ type: T.SET_INPUT_GAIN, cdB: Math.round(Math.min(12, Math.max(-12, db)) * 100) }, { coalesceKey: 'gain' }); }
  setOrder(order) { return this._edit({ type: T.SET_ORDER, order }); }
  setSync(on) { return this._edit({ type: T.SET_SYNC, on: !!on }); }
  /** Headphone jack: 0 headphones, 2 loud (+15 dB), 1 line out (+36 dB). The UI asks before going louder. */
  setOutputMode(mode) { return this._edit({ type: T.SET_OUTPUT_MODE, mode: [0, 1, 2].includes(mode) ? mode : 0 }); }
  setBpm(bpm) { return this._edit({ type: T.SET_BPM, bpm10: Math.round(Math.min(300, Math.max(30, bpm)) * 10) }); }

  /** Reset the active effect's four parameters in a slot to their defaults. */
  resetSlotParams(slot) {
    const fx = this.model.fxTable[this.model.slots[slot].fx];
    const d = defaultParams([fx])[0][0];
    return Promise.all(fx.params.map((_, i) => this.setParam(slot, i, d[i])));
  }

  async saveNow() {
    if (!this.client) return;
    if (!hasCap(this.model, CAP.SAVE)) { this.notice('error', `Saving on the device needs newer firmware (this device: ${this.model.fw || '?'}).`); return; }
    const c = await this._send({ type: T.SAVE_NOW });
    this.notice(c === RESULT.OK ? 'info' : 'error', c === RESULT.OK ? 'Saved on the device.' : 'The device did not confirm the save.');
  }
  async factoryReset() {
    if (!this.client) return;
    const c = await this._send({ type: T.FACTORY_RESET, confirm: FACTORY_RESET_CONFIRM });
    if (c !== RESULT.OK) { this.notice('error', 'The device refused the factory reset.'); return; }
    await this.refreshState();
    this.notice('info', 'Device reset to factory settings.');
  }

  async _pace() { if (this.paceMs > 0) await new Promise((r) => setTimeout(r, this.paceMs)); }

  /**
   * Apply a validated preset through individual setters. Slot assignments go first, ordered so that
   * memory/CPU are freed before they are claimed. Returns { failures: string[] }.
   */
  async applyPreset(preset) {
    this.coalescer.cancel();
    const failures = [];
    if (!this.client) { return { failures: ['Not connected to a device.'] }; }
    const table = this.model.fxTable;
    const idx = new Map(table.map((f, i) => [fxKey(f.name), i]));
    const pool = this.model.poolBytes || 1, budget = this.model.cpuBudget || 1;
    const cost = (fxId) => table[fxId].memBytes / pool + table[fxId].cpuCycles / budget;
    const slotOrder = [0, 1, 2, 3].sort((a, b) =>
      (cost(idx.get(preset.slots[a].fx)) - cost(this.model.slots[a].fx)) - (cost(idx.get(preset.slots[b].fx)) - cost(this.model.slots[b].fx)));
    const send = async (msg, label) => {
      const code = await this._send(msg);
      if (code !== RESULT.OK) failures.push(code === -1 ? `${label}: no response from the device.` : this.describeFailure(msg, code).replace(/; slot unchanged\./, '; slot left unchanged.'));
      await this._pace();
      return code;
    };
    for (const slot of slotOrder) {
      const want = preset.slots[slot];
      const fx = idx.get(want.fx);
      if (this.model.slots[slot].fx !== fx) {
        if (await send({ type: T.SET_SLOT_FX, slot, fx }, `Slot ${slot + 1}`) !== RESULT.OK) continue;
        this.store.update((m) => { m.slots[slot].fx = fx; });
      }
      const f = table[fx];
      for (let i = 0; i < f.params.length; i++) {
        await send({ type: T.SET_PARAM, slot, idx: i, v: want.params[i] }, `Slot ${slot + 1}`);
        this.store.update((m) => { (m.slots[slot].params[fx] ||= [0, 0, 0, 0])[i] = want.params[i]; });
      }
      await send({ type: T.SET_WET, slot, v: want.wet }, `Slot ${slot + 1}`);
      await send({ type: T.SET_BYPASS, slot, on: want.bypass }, `Slot ${slot + 1}`);
    }
    await send({ type: T.SET_MASTER, v: preset.master }, 'Master');
    await send({ type: T.SET_INPUT_GAIN, cdB: Math.round(preset.inputGainDb * 100) }, 'Input gain');
    await send({ type: T.SET_BPM, bpm10: Math.round(preset.bpm * 10) }, 'BPM');
    await send({ type: T.SET_ORDER, order: preset.order }, 'Order');
    await send({ type: T.SET_GLOBAL_BYPASS, on: preset.globalBypass }, 'Global bypass');
    try { await this.refreshState(); } catch { /* reported elsewhere */ }
    return { failures };
  }
}

export { SLOT_COUNT };
