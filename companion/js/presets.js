// Presets: schema v1 validation, factory presets, and a browser-stored library. No DOM dependencies
// (storage is injected so it can be tested and so a missing localStorage degrades to memory).
import { fxKey, defaultNorm, checkAdmission, POOL_BYTES, CPU_BUDGET } from './fx.js';

export const PRESET_SCHEMA = 'sp1fx.preset';
export const PRESET_VERSION = 1;
export const LIBRARY_KEY = 'sp1fx.presetLibrary.v1';
export const MAX_IMPORT_BYTES = 64 * 1024;
export const MAX_NAME = 40;
export const MAX_PRESETS = 200;

const isObj = (x) => x !== null && typeof x === 'object' && !Array.isArray(x);
const isNum = (x) => typeof x === 'number' && Number.isFinite(x);
const clamp = (x, lo, hi) => Math.min(hi, Math.max(lo, x));

export function cleanName(s) {
  // strip control characters and collapse whitespace; rendering uses textContent so no HTML escaping is needed
  return String(s).replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, MAX_NAME);
}

/**
 * Validate untrusted input (parsed JSON object) against schema v1.
 * Returns { ok:true, preset, warnings } or { ok:false, errors }. Never throws, never mutates the input,
 * and builds a fresh object from known keys only (unknown keys are ignored).
 */
export function validatePreset(input, fxTable) {
  const errors = [], warnings = [];
  const fail = (e) => ({ ok: false, errors: [e] });
  if (!isObj(input)) return fail('Preset must be a JSON object.');
  if (input.schema !== PRESET_SCHEMA) return fail('Unknown schema (expected "sp1fx.preset").');
  if (input.version !== PRESET_VERSION) return fail(`Unsupported preset version ${typeof input.version === 'number' ? input.version : '(missing)'}; this app reads version 1.`);

  const keys = new Map(fxTable.map((f, i) => [fxKey(f.name), i]));

  let name = typeof input.name === 'string' ? cleanName(input.name) : '';
  if (!name) { name = 'Untitled'; warnings.push('Missing or empty name; using "Untitled".'); }

  const num = (v, lo, hi, def, label) => {
    if (v === undefined) return def;
    if (!isNum(v)) { warnings.push(`${label} is not a number; using ${def}.`); return def; }
    if (v < lo || v > hi) warnings.push(`${label} ${v} clamped to ${lo}..${hi}.`);
    return clamp(v, lo, hi);
  };

  const inputGainDb = num(input.inputGainDb, -12, 12, 0, 'inputGainDb');
  const master = num(input.master, 0, 1, 0.75, 'master');
  const bpm = num(input.bpm, 30, 300, 120, 'bpm');

  let order = [0, 1, 2, 3];
  if (input.order !== undefined) {
    const o = input.order;
    if (Array.isArray(o) && o.length === 4 && [0, 1, 2, 3].every((v) => o.includes(v))) order = o.slice();
    else warnings.push('order is not a permutation of 0..3; using 0,1,2,3.');
  }
  let globalBypass = false;
  if (input.globalBypass !== undefined) {
    if (typeof input.globalBypass === 'boolean') globalBypass = input.globalBypass;
    else warnings.push('globalBypass is not a boolean; using false.');
  }

  if (!Array.isArray(input.slots) || input.slots.length !== 4) return fail('slots must be an array of exactly 4 entries.');
  const slots = [];
  for (let i = 0; i < 4; i++) {
    const s = input.slots[i];
    if (!isObj(s)) return fail(`Slot ${i + 1} must be an object.`);
    if (typeof s.fx !== 'string' || !keys.has(s.fx)) return fail(`Slot ${i + 1}: unknown effect${typeof s.fx === 'string' ? ` "${cleanName(s.fx).slice(0, 24)}"` : ''}.`);
    const fxEntry = fxTable[keys.get(s.fx)];
    const wet = num(s.wet, 0, 1, 1, `Slot ${i + 1} wet`);
    let bypass = false;
    if (s.bypass !== undefined) {
      if (typeof s.bypass === 'boolean') bypass = s.bypass;
      else warnings.push(`Slot ${i + 1} bypass is not a boolean; using false.`);
    }
    const params = [0, 1, 2, 3].map((p) => {
      const def = fxEntry.params[p] ? defaultNorm(fxEntry.params[p]) : 0;
      const raw = Array.isArray(s.params) ? s.params[p] : undefined;
      return num(raw, 0, 1, def, `Slot ${i + 1} param ${p + 1}`);
    });
    slots.push({ fx: s.fx, bypass, wet, params });
  }
  return { ok: true, preset: { schema: PRESET_SCHEMA, version: PRESET_VERSION, name, inputGainDb, master, bpm, order, globalBypass, slots }, warnings, errors };
}

/** Parse + validate JSON text from an untrusted source. */
export function parsePresetText(text, fxTable) {
  if (typeof text !== 'string') return { ok: false, errors: ['No text to import.'] };
  if (text.length > MAX_IMPORT_BYTES) return { ok: false, errors: ['File is too large to be a preset (limit 64 KB).'] };
  let obj;
  try { obj = JSON.parse(text); } catch { return { ok: false, errors: ['Not valid JSON.'] }; }
  return validatePreset(obj, fxTable);
}

export function presetToJSON(p) { return JSON.stringify(p, null, 2) + '\n'; }

/** Admission check of a preset against pool/budget using the effect table's mem/cpu numbers. */
export function presetAdmission(preset, fxTable, pool = POOL_BYTES, budget = CPU_BUDGET) {
  const keys = new Map(fxTable.map((f, i) => [fxKey(f.name), i]));
  return checkAdmission(preset.slots.map((s) => fxTable[keys.get(s.fx)]), pool, budget);
}

/** Snapshot the current model into a preset. */
export function presetFromModel(model, name) {
  return {
    schema: PRESET_SCHEMA, version: PRESET_VERSION, name: cleanName(name) || 'Untitled',
    inputGainDb: model.inGainDb, master: model.master, bpm: model.bpm, order: model.order.slice(), globalBypass: model.globalBypass,
    slots: model.slots.map((s) => ({
      fx: fxKey(model.fxTable[s.fx]?.name ?? 'none'), bypass: s.bypass, wet: s.wet,
      params: [0, 1, 2, 3].map((i) => (s.params[s.fx]?.[i] ?? 0)),
    })),
  };
}

// ---------- factory presets ----------
const S = (fx, wet, params, bypass = false) => ({ fx, bypass, wet, params });
const P = (name, slots, extra = {}) => ({
  schema: PRESET_SCHEMA, version: PRESET_VERSION, name, inputGainDb: 0, master: 0.75, bpm: 120, order: [0, 1, 2, 3], globalBypass: false, slots, ...extra,
});
// Parameter order per effect is the fx_table order. Values are normalised 0..1 (log params are log-normalised).
export const FACTORY_PRESETS = [
  // "Dub delay" does not exist yet: plain delay stands in.
  P('Dub Station', [
    S('saturation', 0.6, [0.4, 0.7, 0.5, 0.5]),
    S('filter', 1.0, [0.5, 0.4, 0.0, 0.0]),
    S('delay', 0.4, [0.6, 0.5, 0.4, 0.5]),
    S('reverb', 0.3, [0.5, 0.5, 0.5, 0.15]),
  ], { bpm: 140 }),
  P('Boom Bap', [
    S('saturation', 0.7, [0.55, 0.6, 0.5, 0.45]),
    S('bitcrusher', 0.5, [0.55, 0.62, 0.65, 0.5]),
    S('compressor', 1.0, [0.5, 0.45, 0.35, 0.5]),
    S('reverb', 0.2, [0.15, 0.3, 0.6, 0.05]),    // short decay (~0.5 s)
  ], { bpm: 90 }),
  P('House', [
    S('filter', 1.0, [0.7, 0.35, 0.1, 0.0]),
    S('chorus', 0.5, [0.35, 0.5, 0.1, 0.7]),
    S('delay', 0.3, [0.55, 0.4, 0.5, 0.6]),
    S('reverb', 0.2, [0.4, 0.5, 0.5, 0.1]),
  ], { bpm: 124 }),
  P('Lo-Fi Tape', [
    S('saturation', 0.7, [0.5, 0.5, 0.5, 0.5]),
    S('tape_wobble', 1.0, [0.35, 0.25, 0.45, 0.2]),
    S('bitcrusher', 0.45, [0.6, 0.6, 0.55, 0.5]),
    S('filter', 1.0, [0.55, 0.2, 0.0, 0.0]),
  ], { bpm: 80 }),
  P('Ambient', [
    S('chorus', 0.6, [0.2, 0.6, 0.2, 0.9]),
    S('delay', 0.4, [0.8, 0.6, 0.6, 0.8]),
    S('reverb', 0.5, [0.85, 0.8, 0.5, 0.3]),
    S('tremolo', 1.0, [0.3, 0.3, 0.0, 0.5]),
  ], { bpm: 70 }),
  P('Clean', [
    S('none', 1, [0, 0, 0, 0], true), S('none', 1, [0, 0, 0, 0], true),
    S('none', 1, [0, 0, 0, 0], true), S('none', 1, [0, 0, 0, 0], true),
  ]),
];

// ---------- library ----------
let idCounter = 0;
const newId = () => `u${Date.now().toString(36)}${(idCounter++).toString(36)}${Math.floor(Math.random() * 1e6).toString(36)}`;

/** A storage-like object that keeps everything in memory (used when localStorage is unavailable). */
export function memoryStorage() {
  const m = new Map();
  return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => { m.set(k, String(v)); }, removeItem: (k) => { m.delete(k); } };
}

/**
 * Library of presets. Items: { id, factory: bool, preset }. Persisted as one JSON blob in `storage`
 * (localStorage-like). Every storage call is wrapped; failure only sets `persistent=false`.
 */
export class PresetLibrary {
  constructor(storage, fxTable) {
    this.fxTable = fxTable; this.persistent = true; this.items = [];
    this.storage = storage;
    this._load();
  }
  _safe(fn) { try { return fn(); } catch { this.persistent = false; return null; } }
  _load() {
    const raw = this._safe(() => this.storage?.getItem(LIBRARY_KEY));
    if (this.storage == null) this.persistent = false;
    let parsed = null;
    if (raw) { try { parsed = JSON.parse(raw); } catch { parsed = null; } }
    if (isObj(parsed) && parsed.version === 1 && Array.isArray(parsed.items)) {
      for (const it of parsed.items.slice(0, MAX_PRESETS)) {
        if (!isObj(it)) continue;
        const v = validatePreset(it.preset, this.fxTable);
        if (v.ok) this.items.push({ id: typeof it.id === 'string' ? it.id.slice(0, 40) : newId(), factory: it.factory === true, preset: v.preset });
      }
    } else {
      this.items = FACTORY_PRESETS.map((p) => ({ id: newId(), factory: true, preset: structuredClone(p) }));
      this._persist();
    }
  }
  _persist() {
    this._safe(() => this.storage.setItem(LIBRARY_KEY, JSON.stringify({ version: 1, items: this.items })));
  }
  list() { return this.items; }
  get(id) { return this.items.find((i) => i.id === id) ?? null; }
  _uniqueName(name) {
    const base = cleanName(name) || 'Untitled';
    const taken = new Set(this.items.map((i) => i.preset.name));
    if (!taken.has(base)) return base;
    for (let n = 2; n < 1000; n++) {
      const suffix = ` (${n})`;
      const c = base.slice(0, MAX_NAME - suffix.length) + suffix;
      if (!taken.has(c)) return c;
    }
    return base;
  }
  /** Add a (validated) preset; names are made unique. Returns the item or null if the library is full. */
  add(preset, { factory = false } = {}) {
    if (this.items.length >= MAX_PRESETS) return null;
    const item = { id: newId(), factory, preset: { ...structuredClone(preset), name: this._uniqueName(preset.name) } };
    this.items.push(item); this._persist();
    return item;
  }
  save(model, name) { return this.add(presetFromModel(model, name)); }
  overwrite(id, preset) {
    const it = this.get(id); if (!it) return false;
    it.preset = { ...structuredClone(preset), name: it.preset.name }; this._persist(); return true;
  }
  rename(id, name) {
    const it = this.get(id); const n = cleanName(name);
    if (!it || !n) return false;
    it.preset.name = n; this._persist(); return true;
  }
  duplicate(id) { const it = this.get(id); return it ? this.add({ ...it.preset, name: `${it.preset.name} copy` }) : null; }
  delete(id) {
    const i = this.items.findIndex((x) => x.id === id);
    if (i < 0) return false;
    this.items.splice(i, 1); this._persist(); return true;
  }
  exportJSON(id) { const it = this.get(id); return it ? presetToJSON(it.preset) : null; }
  /** Import untrusted JSON text. Returns {ok, item?, errors, warnings}. */
  importText(text) {
    const v = parsePresetText(text, this.fxTable);
    if (!v.ok) return v;
    const item = this.add(v.preset);
    if (!item) return { ok: false, errors: ['Preset library is full.'] };
    return { ok: true, item, warnings: v.warnings, errors: [] };
  }
  /** Re-add / reset the factory presets (user presets are kept). Returns the number restored. */
  restoreFactory() {
    this.items = this.items.filter((i) => !i.factory);
    const factoryItems = FACTORY_PRESETS.map((p) => ({ id: newId(), factory: true, preset: structuredClone(p) }));
    // factory names win; rename colliding user presets rather than lose them
    const names = new Set(FACTORY_PRESETS.map((p) => p.name));
    for (const u of this.items) if (names.has(u.preset.name)) u.preset.name = `${u.preset.name} (user)`.slice(0, MAX_NAME);
    this.items = [...factoryItems, ...this.items];
    this._persist();
    return factoryItems.length;
  }
}
