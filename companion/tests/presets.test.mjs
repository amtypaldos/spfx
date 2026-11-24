import test from 'node:test';
import assert from 'node:assert/strict';
import {
  validatePreset, parsePresetText, FACTORY_PRESETS, presetAdmission, PresetLibrary, memoryStorage, LIBRARY_KEY,
  presetToJSON, presetFromModel, cleanName, MAX_IMPORT_BYTES,
} from '../js/presets.js';
import { POOL_BYTES, CPU_BUDGET } from '../js/fx.js';
import { fxTable } from './helpers.mjs';

const good = () => structuredClone(FACTORY_PRESETS[0]);

test('factory presets: names, shape, validity', () => {
  assert.deepEqual(FACTORY_PRESETS.map((p) => p.name), ['Dub Station', 'Boom Bap', 'House', 'Lo-Fi Tape', 'Ambient', 'Clean']);
  const fx = (n) => FACTORY_PRESETS.find((p) => p.name === n).slots.map((s) => s.fx);
  assert.deepEqual(fx('Dub Station'), ['saturation', 'filter', 'delay', 'reverb']);
  assert.deepEqual(fx('Boom Bap'), ['saturation', 'bitcrusher', 'compressor', 'reverb']);
  assert.deepEqual(fx('House'), ['filter', 'chorus', 'delay', 'reverb']);
  assert.deepEqual(fx('Lo-Fi Tape'), ['saturation', 'tape_wobble', 'bitcrusher', 'filter']);
  assert.deepEqual(fx('Ambient'), ['chorus', 'delay', 'reverb', 'tremolo']);
  assert.ok(FACTORY_PRESETS.at(-1).slots.every((s) => s.bypass));
  for (const p of FACTORY_PRESETS) {
    const v = validatePreset(p, fxTable);
    assert.ok(v.ok, `${p.name}: ${v.errors}`);
    assert.deepEqual(v.warnings, [], `${p.name} should need no clamping`);
    assert.deepEqual(v.preset, p);
  }
});

test('factory presets: pass admission with the table mem/cpu numbers', () => {
  for (const p of FACTORY_PRESETS) {
    const a = presetAdmission(p, fxTable, POOL_BYTES, CPU_BUDGET);
    assert.ok(a.ok, `${p.name} rejected: code ${a.code} mem ${a.mem} cpu ${a.cpu}`);
  }
  const mem = Object.fromEntries(FACTORY_PRESETS.map((p) => [p.name, presetAdmission(p, fxTable).mem]));
  assert.equal(mem['Dub Station'], 76800 + 43120);
  assert.equal(mem['House'], 6144 + 76800 + 43120);
});

test('validate: rejects wrong schema / version / shape', () => {
  for (const bad of [null, undefined, 5, 'x', [], {}, { schema: 'other', version: 1 }, { schema: 'sp1fx.preset' },
    { ...good(), version: 2 }, { ...good(), version: '1' }, { ...good(), version: 0 },
    { ...good(), slots: [] }, { ...good(), slots: good().slots.slice(0, 3) }, { ...good(), slots: [...good().slots, good().slots[0]] },
    { ...good(), slots: 'abc' }]) {
    assert.equal(validatePreset(bad, fxTable).ok, false, JSON.stringify(bad)?.slice(0, 60));
  }
  const p = good(); p.slots[2].fx = 'dub_delay';
  assert.equal(validatePreset(p, fxTable).ok, false);
  const q = good(); q.slots[1] = null;
  assert.equal(validatePreset(q, fxTable).ok, false);
});

test('validate: clamps numbers, defaults bad fields, ignores unknown keys', () => {
  const p = good();
  Object.assign(p, { master: 7, inputGainDb: -99, bpm: 1e9, evil: 'x', order: [0, 0, 1, 2], globalBypass: 'yes' });
  p.slots[0] = { fx: 'saturation', wet: -4, bypass: 'no', params: [5, -1, 'a', null, 0.5, 0.5], extra: { a: 1 } };
  const v = validatePreset(p, fxTable);
  assert.ok(v.ok);
  assert.equal(v.preset.master, 1); assert.equal(v.preset.inputGainDb, -12); assert.equal(v.preset.bpm, 300);
  assert.deepEqual(v.preset.order, [0, 1, 2, 3]); assert.equal(v.preset.globalBypass, false);
  assert.equal(v.preset.slots[0].wet, 0); assert.equal(v.preset.slots[0].bypass, false);
  assert.equal(v.preset.slots[0].params.length, 4);
  assert.equal(v.preset.slots[0].params[0], 1); assert.equal(v.preset.slots[0].params[1], 0);
  assert.equal(v.preset.slots[0].params[2], 0.5);        // non-number -> default (Bias default 0 is mid-range)
  assert.ok(!('evil' in v.preset)); assert.ok(!('extra' in v.preset.slots[0]));
  assert.ok(v.warnings.length >= 5);
});

test('validate: hostile input', () => {
  const evil = JSON.parse('{"__proto__":{"polluted":1},"constructor":{"x":1},"schema":"sp1fx.preset","version":1,"name":"<img src=x onerror=alert(1)>\\u0000\\n' + 'A'.repeat(500) + '","slots":[' +
    '{"fx":"__proto__"},{"fx":"constructor"},{"fx":"filter"},{"fx":"none"}]}');
  assert.equal(validatePreset(evil, fxTable).ok, false);
  const ok = JSON.parse('{"__proto__":{"polluted":1},"schema":"sp1fx.preset","version":1,"name":"<b>hi</b>\\u0007\\n   x","slots":[' +
    '{"fx":"filter","params":{"length":4}},{"fx":"filter","params":"abc"},{"fx":"none","wet":1e999},{"fx":"none"}]}');
  const v = validatePreset(ok, fxTable);
  assert.ok(v.ok);
  assert.equal({}.polluted, undefined);
  assert.equal(v.preset.name, '<b>hi</b> x');               // kept as inert text; UI renders with textContent
  assert.ok(Object.getPrototypeOf(v.preset) === Object.prototype);
  assert.deepEqual(v.preset.slots[0].params, [fxTable[1].params[0], 0, 0, 0].map((_, i) => v.preset.slots[0].params[i]));
  assert.ok(v.preset.slots[0].params.every((x) => x >= 0 && x <= 1));
  const long = validatePreset({ schema: 'sp1fx.preset', version: 1, name: 'N'.repeat(1000), slots: Array(4).fill({ fx: 'none' }) }, fxTable);
  assert.equal(long.preset.name.length, 40);
  const num = validatePreset({ schema: 'sp1fx.preset', version: 1, name: 5, master: '0.5', slots: Array(4).fill({ fx: 'none', wet: NaN }) }, fxTable);
  assert.equal(num.preset.name, 'Untitled'); assert.equal(num.preset.master, 0.75);
});

test('parsePresetText: size/JSON checks and round trip', () => {
  assert.equal(parsePresetText('{not json', fxTable).ok, false);
  assert.equal(parsePresetText('x'.repeat(MAX_IMPORT_BYTES + 1), fxTable).ok, false);
  assert.equal(parsePresetText(undefined, fxTable).ok, false);
  const r = parsePresetText(presetToJSON(good()), fxTable);
  assert.ok(r.ok); assert.deepEqual(r.preset, good());
});

test('library: seeds factory presets, persists, survives reload', () => {
  const st = memoryStorage();
  const lib = new PresetLibrary(st, fxTable);
  assert.equal(lib.list().length, 6); assert.ok(lib.list().every((i) => i.factory));
  assert.ok(st.getItem(LIBRARY_KEY));
  const id = lib.list()[1].id;
  lib.rename(id, '  My   Boom  ');
  const dup = lib.duplicate(id);
  assert.equal(dup.preset.name, 'My Boom copy'); assert.equal(dup.factory, false);
  const lib2 = new PresetLibrary(st, fxTable);
  assert.equal(lib2.list().length, 7);
  assert.equal(lib2.get(id).preset.name, 'My Boom');
  assert.ok(lib2.delete(dup.id)); assert.equal(lib2.list().length, 6);
  assert.equal(lib2.delete('nope'), false);
});

test('library: save from model, unique names, export/import round trip', () => {
  const lib = new PresetLibrary(memoryStorage(), fxTable);
  const model = {
    fxTable, inGainDb: 3, master: 0.5, bpm: 99.5, order: [1, 0, 2, 3], globalBypass: false,
    slots: [{ fx: 3, bypass: false, wet: 0.4, params: fxTable.map(() => [0.1, 0.2, 0.3, 0.4]) }, { fx: 0, bypass: true, wet: 1, params: [] },
      { fx: 10, bypass: false, wet: 1, params: fxTable.map(() => [0, 0, 0, 0]) }, { fx: 0, bypass: false, wet: 1, params: [] }],
  };
  const a = lib.save(model, 'Mine'), b = lib.save(model, 'Mine');
  assert.equal(a.preset.name, 'Mine'); assert.equal(b.preset.name, 'Mine (2)');
  assert.deepEqual(a.preset.slots[0], { fx: 'delay', bypass: false, wet: 0.4, params: [0.1, 0.2, 0.3, 0.4] });
  assert.equal(a.preset.slots[2].fx, 'tape_wobble');
  const json = lib.exportJSON(a.id);
  const r = lib.importText(json);
  assert.ok(r.ok); assert.equal(r.item.preset.name, 'Mine (3)');
  assert.deepEqual({ ...r.item.preset, name: 0 }, { ...a.preset, name: 0 });
  assert.equal(lib.importText('{"schema":"sp1fx.preset","version":9}').ok, false);
  assert.equal(presetFromModel(model, 'x\u0000y').name, 'x y');
});

test('library: restore factory defaults keeps user presets', () => {
  const lib = new PresetLibrary(memoryStorage(), fxTable);
  lib.delete(lib.list()[0].id);
  lib.rename(lib.list()[0].id, 'Changed');
  const mine = lib.save({ ...{ fxTable, inGainDb: 0, master: 0.5, bpm: 120, order: [0, 1, 2, 3], globalBypass: false }, slots: Array(4).fill({ fx: 0, bypass: true, wet: 1, params: [] }) }, 'Dub Station');
  assert.equal(mine.preset.name, 'Dub Station');   // the factory one was deleted above
  assert.equal(lib.restoreFactory(), 6);
  assert.deepEqual(lib.list().filter((i) => i.factory).map((i) => i.preset.name), FACTORY_PRESETS.map((p) => p.name));
  assert.ok(lib.get(mine.id)); assert.equal(lib.get(mine.id).preset.name, 'Dub Station (user)');
});

test('library: works without storage / with throwing storage / with corrupt data', () => {
  const l1 = new PresetLibrary(null, fxTable);
  assert.equal(l1.list().length, 6); assert.equal(l1.persistent, false);
  assert.ok(l1.save({ fxTable, inGainDb: 0, master: 0.5, bpm: 120, order: [0, 1, 2, 3], globalBypass: false, slots: Array(4).fill({ fx: 0, bypass: true, wet: 1, params: [] }) }, 'A'));
  const thrower = { getItem() { throw new Error('denied'); }, setItem() { throw new Error('quota'); } };
  const l2 = new PresetLibrary(thrower, fxTable);
  assert.equal(l2.list().length, 6); assert.equal(l2.persistent, false);
  const st = memoryStorage(); st.setItem(LIBRARY_KEY, '{garbage');
  assert.equal(new PresetLibrary(st, fxTable).list().length, 6);
  st.setItem(LIBRARY_KEY, JSON.stringify({ version: 1, items: [{ id: 'a', preset: { schema: 'x' } }, { id: 'b', factory: false, preset: good() }, 7] }));
  const l3 = new PresetLibrary(st, fxTable);
  assert.deepEqual(l3.list().map((i) => i.id), ['b']);
});

test('cleanName', () => {
  assert.equal(cleanName('a\tb\nc'), 'a b c');
  assert.equal(cleanName('   '), '');
});
