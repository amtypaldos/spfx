import test from 'node:test';
import assert from 'node:assert/strict';
import { MockDevice, createLoopback, mulberry32 } from '../js/mock-device.js';
import { Client, Coalescer, RequestTimeout } from '../js/client.js';
import { Controller } from '../js/controller.js';
import { Store } from '../js/state.js';
import { T, RESULT } from '../js/protocol.js';
import { FACTORY_PRESETS } from '../js/presets.js';
import { fxTable, tick } from './helpers.mjs';

const ID = Object.fromEntries(fxTable.map((f) => [f.name.toLowerCase().replace(/ /g, '_'), f.id]));

function rig(opts = {}) {
  const dev = new MockDevice(fxTable, { rng: mulberry32(1) });
  const tr = createLoopback(dev);
  const store = new Store();
  const ctl = new Controller(store, fxTable, { paceMs: 0, rateMs: 1, ...opts });
  const notices = []; ctl.onNotice((n) => notices.push(n));
  return { dev, tr, store, ctl, notices };
}
const settle = async (n = 5) => { for (let i = 0; i < n; i++) await tick(); };

test('client: handshake HELLO -> GET_FX x11 -> GET_STATE -> SUBSCRIBE through real framing', async () => {
  const { dev, tr, store, ctl } = rig();
  await ctl.connect(tr, 'demo');
  const m = store.model;
  assert.equal(m.conn, 'connected'); assert.equal(m.fw, dev.fw); assert.equal(m.poolBytes, 153600); assert.equal(m.cpuBudget, 850);
  assert.equal(m.fxTable.length, 11);
  assert.equal(m.fxTable[3].name, 'Delay'); assert.equal(m.fxTable[3].memBytes, 76800);
  assert.equal(m.fxTable[1].params[3].labels, 'Low-pass,High-pass,Band-pass,Notch');
  assert.deepEqual(m.slots.map((s) => s.fx), dev.s.fxId);
  assert.equal(dev.hz, 10);
  assert.ok(m.synced);
  await ctl.disconnect();
  assert.equal(store.model.conn, 'disconnected');
});

test('mock: admission failure memory (code 2) keeps previous assignment and reports a clear message', async () => {
  const { dev, tr, store, ctl, notices } = rig();
  await ctl.connect(tr, 'demo');
  // slots: saturation, filter, delay, reverb. Put a second delay into slot 2 (idx 1): 76800*2+43120 = 196720 > 153600
  const code = await ctl.setSlotFx(1, ID.delay);
  assert.equal(code, RESULT.NO_MEMORY);
  assert.equal(dev.s.fxId[1], ID.filter, 'device kept previous assignment');
  assert.equal(store.model.slots[1].fx, ID.filter, 'UI reverted');
  assert.equal(notices.at(-1).text, 'Not enough DSP memory for Delay in slot 2; slot unchanged.');
  await ctl.disconnect();
});

test('mock: admission failure CPU (code 3)', async () => {
  const { dev, tr, store, ctl, notices } = rig();
  await ctl.connect(tr, 'demo');
  await ctl.setSlotFx(0, ID.chorus);               // free memory first: chorus 120 cpu, 6144 B
  await ctl.setSlotFx(1, ID.chorus);               // 120
  await ctl.setSlotFx(2, ID.chorus);               // 120 + reverb 320 + 120 + 120 = 680 OK
  assert.equal(await ctl.setSlotFx(3, ID.reverb), RESULT.OK);
  // cpu: chorus*3 (360) + reverb (320) = 680. Replace a chorus with reverb -> 880 > 850, memory 43120*2+12288 fits
  const code = await ctl.setSlotFx(0, ID.reverb);
  assert.equal(code, RESULT.NO_CPU);
  assert.equal(dev.s.fxId[0], ID.chorus);
  assert.equal(notices.at(-1).text, 'Not enough CPU headroom for Reverb in slot 1; slot unchanged.');
  await ctl.disconnect();
});

test('mock: no echo of host-originated changes; device events do arrive', async () => {
  const { dev, tr, store, ctl } = rig();
  await ctl.connect(tr, 'demo');
  const events = [];
  const orig = ctl.client.onEvent;
  ctl.client.onEvent = (m) => { events.push(m); orig(m); };
  await ctl.setMaster(0.4); await ctl.setBypass(2, false); await ctl.setGlobalBypass(true);
  await ctl.setSlotFx(0, ID.compressor); await ctl.setBpm(133.3); await ctl.setOrder([3, 2, 1, 0]); await ctl.setInputGainDb(-6);
  ctl.setParam(0, 1, 0.8); ctl.setWet(0, 0.2);
  await settle(20); await new Promise((r) => setTimeout(r, 20));
  assert.deepEqual(events.filter((e) => e.type === T.EVENT), [], 'device must not echo host changes');
  assert.ok(Math.abs(dev.s.master - 0.4) < 1e-4);
  assert.equal(dev.s.bypass[2], false); assert.equal(dev.s.globalBypass, true);
  assert.equal(dev.s.fxId[0], ID.compressor); assert.equal(dev.s.bpm10, 1333); assert.deepEqual(dev.s.order, [3, 2, 1, 0]);
  assert.equal(dev.s.inGainCdB, -600);
  assert.ok(Math.abs(dev.s.params[0][ID.compressor][1] - 0.8) < 1e-4);
  assert.ok(Math.abs(dev.s.wet[0] - 0.2) < 1e-4);
  // now a physical fader move
  dev.simulateFader(3, 2, 0.9); dev.deviceChange({ type: T.SET_MASTER, v: 0.2 });
  await settle();
  assert.equal(events.filter((e) => e.type === T.EVENT).length, 2);
  assert.ok(Math.abs(store.model.slots[3].params[ID.reverb][2] - 0.9) < 1e-4);
  assert.ok(Math.abs(store.model.master - 0.2) < 1e-4);
  await ctl.disconnect();
});

test('mock: device-originated effect cycle + admission error event (0xA1)', async () => {
  const { dev, tr, store, ctl, notices } = rig();
  await ctl.connect(tr, 'demo');
  // slot 1 (filter, id 1) cycles to chorus (2), then delay (3): rejected (two delays + reverb)
  assert.ok(dev.simulateCycleFx(1)); await settle();
  assert.equal(store.model.slots[1].fx, ID.chorus);
  assert.equal(dev.simulateCycleFx(1), false); await settle();
  assert.equal(dev.s.fxId[1], ID.chorus);
  assert.equal(store.model.slots[1].fx, ID.chorus);
  assert.match(notices.at(-1).text, /slot 2 to Delay: not enough DSP memory\. Slot unchanged\./);
  await ctl.disconnect();
});

test('mock: bad arguments and factory reset confirmation', async () => {
  const { dev, tr, ctl } = rig();
  await ctl.connect(tr, 'demo');
  const c = ctl.client;
  assert.equal((await c.request({ type: T.SET_SLOT_FX, slot: 0, fx: 99 })).code, RESULT.BAD_ARG);
  assert.equal((await c.request({ type: T.GET_FX, idx: 99 })).code, RESULT.BAD_ARG);
  assert.equal((await c.request({ type: T.FACTORY_RESET, confirm: 0 })).code, RESULT.BAD_ARG);
  await ctl.setMaster(0.1); await settle(8);
  await ctl.factoryReset();
  assert.ok(Math.abs(dev.s.master - 0.75) < 1e-9);
  assert.ok(Math.abs(ctl.model.master - 0.75) < 1e-4);
  assert.equal((await c.request({ type: T.SAVE_NOW })).code, RESULT.OK);
  await ctl.disconnect();
});

test('mock: meters only while subscribed, with budget-based CPU and memory', async () => {
  const { dev, tr, store, ctl } = rig();
  await ctl.connect(tr, 'demo');
  dev.s.bypass = [false, false, false, false];
  for (let i = 0; i < 10; i++) { dev.tick(100); await settle(1); }
  const mt = store.model.meters;
  assert.equal(mt.memUsed, 76800 + 43120);
  // active cycles 90+70+80+320 = 560 (+60 base) / 850 ~ 73%
  assert.ok(mt.cpuPct >= 72 && mt.cpuPct <= 76, `cpu ${mt.cpuPct}`);
  assert.ok(mt.inPeak > 0 && mt.outPeak > 0);
  await ctl.client.request({ type: T.SUBSCRIBE, hz: 0 });
  const before = JSON.stringify(store.model.meters);
  for (let i = 0; i < 10; i++) { dev.tick(100); await settle(1); }
  assert.equal(JSON.stringify(store.model.meters), before);
  await ctl.disconnect();
});

test('client: lost frames are retried (same seq), then time out', async () => {
  const dev = new MockDevice(fxTable); const tr = createLoopback(dev);
  const c = new Client(tr, { timeoutMs: 20, retries: 3 });
  tr.dropNext(2);
  const r = await c.request({ type: T.HELLO });
  assert.equal(r.type, T.HELLO_RSP);
  tr.dropNext(99);
  await assert.rejects(c.request({ type: T.HELLO }), RequestTimeout);
  tr.dropNext(0);
  await c.close();
  await assert.rejects(c.request({ type: T.HELLO }), /not connected/);
});

test('client: corrupted response frames are dropped and recovered by retry', async () => {
  const dev = new MockDevice(fxTable); const tr = createLoopback(dev);
  const c = new Client(tr, { timeoutMs: 20, retries: 3 });
  const send = dev.onOutput; let first = true;
  dev.onOutput = (b) => { if (first) { first = false; const x = b.slice(); x[4] ^= 0xFF; send(x); } else send(b); };
  const r = await c.request({ type: T.HELLO });
  assert.equal(r.type, T.HELLO_RSP); assert.equal(c.dropped, 1);
  await c.close();
});

test('coalescer: <= ~30 msgs/s, latest value wins, trailing value always sent', async () => {
  const sent = [];
  let t = 0;
  const co = new Coalescer((m) => sent.push(m), 34, () => t);
  // simulate a 200-event drag in 1 s of real time using the real timers
  const start = Date.now();
  await new Promise((resolve) => {
    let i = 0;
    const iv = setInterval(() => {
      t = Date.now() - start;
      co.set('p0.0', { v: i / 200 }); if (++i === 200) { clearInterval(iv); setTimeout(resolve, 80); }
    }, 5);
  });
  assert.ok(sent.length >= 10, `sent ${sent.length}`);
  const elapsed = (Date.now() - start) / 1000;
  assert.ok(sent.length <= Math.ceil(30 * elapsed) + 2, `${sent.length} msgs in ${elapsed}s`);
  assert.equal(sent.at(-1).v, 199 / 200, 'final value delivered');
  assert.equal(co.pending.size, 0);
});

test('controller: dragging a param produces few messages and ends at the final value', async () => {
  const { dev, tr, ctl } = rig({ rateMs: 34 });
  await ctl.connect(tr, 'demo');
  const before = dev.log.length;
  for (let i = 1; i <= 100; i++) { ctl.setParam(0, 0, i / 100); await new Promise((r) => setTimeout(r, 2)); }
  await new Promise((r) => setTimeout(r, 120));
  const n = dev.log.filter((m) => m.type === T.SET_PARAM).length;
  assert.ok(n < 20, `sent ${n}`);
  assert.ok(Math.abs(dev.s.params[0][ID.saturation][0] - 1) < 1e-4);
  await ctl.disconnect();
});

test('controller: factory presets apply cleanly through real setters; failures are reported', async () => {
  const { dev, tr, store, ctl } = rig();
  await ctl.connect(tr, 'demo');
  for (const p of FACTORY_PRESETS) {
    const res = await ctl.applyPreset(p);
    assert.deepEqual(res.failures, [], p.name);
    assert.deepEqual(dev.s.fxId, p.slots.map((s) => ID[s.fx]), p.name);
    p.slots.forEach((s, i) => {
      assert.equal(dev.s.bypass[i], s.bypass);
      assert.ok(Math.abs(dev.s.wet[i] - s.wet) < 1e-4);
      s.params.slice(0, fxTable[ID[s.fx]].params.length).forEach((v, k) => assert.ok(Math.abs(dev.s.params[i][ID[s.fx]][k] - v) < 1e-4));
    });
    assert.deepEqual(store.model.slots.map((s) => s.fx), dev.s.fxId);
  }
  // a hostile-but-valid preset that cannot fit: 3 delays
  const bad = structuredClone(FACTORY_PRESETS[2]);
  bad.slots[0].fx = 'delay'; bad.slots[1].fx = 'delay'; bad.slots[2].fx = 'delay'; bad.slots[3].fx = 'none';
  const before = [...dev.s.fxId];
  const res = await ctl.applyPreset(bad);
  assert.ok(res.failures.length >= 1);
  assert.match(res.failures.join(' '), /Not enough DSP memory for Delay/);
  assert.equal(dev.s.fxId.filter((x) => x === ID.delay).length, 2, 'two delays fit? 153600 = exactly 2 delays');
  assert.notDeepEqual(dev.s.fxId, before);
  await ctl.disconnect();
});

test('controller: slot-order-dependent presets succeed by freeing memory first', async () => {
  const { dev, tr, ctl } = rig();
  await ctl.connect(tr, 'demo');
  await ctl.applyPreset(FACTORY_PRESETS[0]);                 // Dub: delay in slot 3
  const p = structuredClone(FACTORY_PRESETS[4]);             // Ambient: delay in slot 2, reverb slot 3
  assert.deepEqual((await ctl.applyPreset(p)).failures, []);
  assert.deepEqual(dev.s.fxId, [ID.chorus, ID.delay, ID.reverb, ID.tremolo]);
  await ctl.disconnect();
});

test('device-loaded preset (0xA3): companion re-reads the whole state', async () => {
  const { dev, tr, store, ctl, notices } = rig();
  await ctl.connect(tr, 'demo');
  dev.simulatePresetLoad(3, (st) => { st.fxId[0] = ID.reverb; st.master = 0.5; st.bpm10 = 987; st.bypass[1] = false; });
  await settle(30); await new Promise((r) => setTimeout(r, 30));
  assert.equal(store.model.slots[0].fx, ID.reverb);
  assert.ok(Math.abs(store.model.master - 0.5) < 1e-3);
  assert.equal(Math.round(store.model.bpm * 10), 987);
  assert.ok(notices.some((n) => /preset 4/i.test(n.text)));
});

test('sync clock switch: host sets it, device state reports it, no echo', async () => {
  const { dev, tr, store, ctl } = rig();
  await ctl.connect(tr, 'demo');
  assert.equal(store.model.syncOn, false);
  const events = [];
  const orig = ctl.client.onEvent;
  ctl.client.onEvent = (m) => { events.push(m); orig(m); };
  await ctl.setSync(true);
  await settle(10);
  assert.equal(dev.s.syncOn, true);
  assert.equal(store.model.syncOn, true);
  assert.deepEqual(events.filter((e) => e.type === T.EVENT), []);
  await ctl.refreshState();
  assert.equal(store.model.syncOn, true, 'STATE carries the saved sync flag');
  await ctl.setSync(false);
  assert.equal(dev.s.syncOn, false);
});
