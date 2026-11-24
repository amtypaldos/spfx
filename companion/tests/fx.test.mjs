import test from 'node:test';
import assert from 'node:assert/strict';
import { stepLabels, normToValue, valueToNorm, formatValue, defaultNorm, fxKey, checkAdmission, POOL_BYTES, CPU_BUDGET, normalizeFxTable } from '../js/fx.js';
import { fxTable } from './helpers.mjs';

test('scale mapping: endpoints for every parameter in the table', () => {
  for (const fx of fxTable) for (const p of fx.params) {
    assert.ok(Math.abs(normToValue(p, 0) - p.min) < 1e-9, `${fx.name}/${p.name} min`);
    assert.ok(Math.abs(normToValue(p, 1) - p.max) < 1e-9 * Math.max(1, p.max), `${fx.name}/${p.name} max`);
  }
});

test('scale mapping: lin/log round trip', () => {
  for (const fx of fxTable) for (const p of fx.params) {
    if (p.scale === 'step') continue;
    for (const n of [0, 0.1, 0.25, 0.5, 0.77, 1]) {
      assert.ok(Math.abs(valueToNorm(p, normToValue(p, n)) - n) < 1e-9, `${fx.name}/${p.name} @${n}`);
    }
    const d = defaultNorm(p);
    assert.ok(Math.abs(normToValue(p, d) - p.def) < 1e-9 * Math.max(1, Math.abs(p.def)));
  }
});

test('scale mapping: log is geometric, lin is linear', () => {
  const cutoff = fxTable[1].params[0];
  assert.equal(cutoff.scale, 'log');
  assert.ok(Math.abs(normToValue(cutoff, 0.5) - Math.sqrt(20 * 20000)) < 1e-6);
  const drive = fxTable[1].params[2];
  assert.equal(normToValue(drive, 0.5), 50);
});

test('scale mapping: step params snap to integers and round trip through u16n', () => {
  const type = fxTable[1].params[3];
  assert.equal(type.scale, 'step');
  for (let v = 0; v <= 3; v++) {
    const n = valueToNorm(type, v);
    assert.equal(normToValue(type, n), v);
    const q = Math.round(n * 65535) / 65535;            // wire quantisation
    assert.equal(normToValue(type, q), v);
    assert.equal(formatValue(type, n), type.labels.split(',')[v]);
  }
  assert.equal(normToValue(type, 0.4), 1);              // snaps to nearest step
});

test('formatValue: real units', () => {
  const cutoff = fxTable[1].params[0];
  assert.equal(formatValue(cutoff, valueToNorm(cutoff, 1500)), '1.50 kHz');
  assert.equal(formatValue(cutoff, valueToNorm(cutoff, 440)), '440 Hz');
  const thr = fxTable[9].params[0];
  assert.equal(formatValue(thr, valueToNorm(thr, -18)), '-18.0 dB');
  const gain = fxTable[5].params[3];
  assert.equal(formatValue(gain, valueToNorm(gain, 3)), '+3.0 dB');
  assert.equal(formatValue(fxTable[3].params[1], 0.4), '38 %');
  assert.equal(formatValue(fxTable[9].params[1], valueToNorm(fxTable[9].params[1], 4)), '4.00:1');
  assert.equal(formatValue(fxTable[5].params[2], 0.5), '0.00');
});

test('fxKey and table normalisation', () => {
  assert.equal(fxKey('Tape wobble'), 'tape_wobble');
  assert.equal(fxTable.length, 11);
  assert.equal(fxTable[0].name, 'None');
  assert.throws(() => normalizeFxTable([{ nope: 1 }]));
  assert.throws(() => normalizeFxTable('x'));
});

test('admission arithmetic', () => {
  const delay = fxTable[3], reverb = fxTable[4];
  assert.equal(POOL_BYTES, 153600);
  assert.equal(checkAdmission([delay, reverb]).ok, true);
  assert.equal(checkAdmission([delay, delay, reverb]).code, 2);          // 196720 B > 153600
  const chorus = fxTable[2], tape = fxTable[10];
  assert.equal(checkAdmission([delay, reverb, tape, chorus]).code, 3);  // 210+240+255+200 = 905 cycles > 850, memory fits
  assert.equal(checkAdmission([reverb, reverb, reverb, reverb]).code, 3);   // CPU is checked first (as in firmware engine_assign): 960 > 850, memory would also fail
  assert.equal(CPU_BUDGET, 850);
});

test('step parameters map exactly like the firmware (floor of n*count, bucket-centred inverse)', () => {
  const type = fxTable[1].params[3]; // Filter "Type": 4 steps, Low-pass..Notch
  assert.equal(type.scale, 'step');
  assert.deepEqual([0, 0.24, 0.25, 0.49, 0.5, 0.74, 0.75, 1].map((n) => normToValue(type, n)), [0, 0, 1, 1, 2, 2, 3, 3]);
  for (let v = type.min; v <= type.max; v++) assert.equal(normToValue(type, valueToNorm(type, v)), v);
  assert.equal(stepLabels(type)[normToValue(type, 0.5)], 'Band-pass');
});
