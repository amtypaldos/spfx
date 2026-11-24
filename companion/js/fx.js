// Effect table helpers and parameter scale mapping. No DOM dependencies.
// Parameters travel as normalised 0..1 (u16n). The device maps them to real units; this mirrors that
// mapping for display only (PROTOCOL_NOTES.md #5: the exact mapping is not specified in PROTOCOL.md).

export const POOL_BYTES = 150 * 1024;   // engine DSP memory pool
export const CPU_BUDGET = 850;          // cycles per frame budget
export const SLOT_COUNT = 4;

const clamp01 = (x) => (x < 0 ? 0 : x > 1 ? 1 : x);

/** 'Tape wobble' -> 'tape_wobble' (the preset fx key). */
export const fxKey = (name) => String(name).toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '');

/** Normalised 0..1 -> real value. */
export function normToValue(p, n) {
  n = clamp01(Number.isFinite(n) ? n : 0);
  const span = p.max - p.min;
  // Same as firmware fx_param_map(): floor(n*(count)), capped at max. (Was round(), which disagreed.)
  if (p.scale === 'step') return p.min + Math.min(Math.floor(n * (span + 1)), span);
  if (p.scale === 'log' && p.min > 0 && p.max > p.min) return p.min * Math.pow(p.max / p.min, n);
  return p.min + n * span;
}

/** Real value -> normalised 0..1. */
export function valueToNorm(p, v) {
  const span = p.max - p.min;
  if (!(span > 0)) return 0;
  let n;
  if (p.scale === 'step') n = (v - p.min + 0.5) / (span + 1); // inverse of the above, centred in its bucket
  else if (p.scale === 'log' && p.min > 0 && p.max > p.min) n = Math.log(Math.max(v, p.min) / p.min) / Math.log(p.max / p.min);
  else n = (v - p.min) / span;
  return clamp01(n);
}

export const defaultNorm = (p) => valueToNorm(p, p.def);

export const stepLabels = (p) => (p.scale === 'step' && p.labels ? p.labels.split(',') : []);

function sig(v, digits) {
  if (v === 0) return '0';
  const a = Math.abs(v);
  const dec = Math.max(0, digits - 1 - Math.floor(Math.log10(a)));
  return v.toFixed(Math.min(dec, 3));
}

/** Human readable value with unit, e.g. "1.50 kHz", "-18.0 dB", "Low-pass". */
export function formatValue(p, n) {
  const v = normToValue(p, n);
  if (p.scale === 'step') {
    const labels = stepLabels(p);
    const i = Math.round(v - p.min);
    return labels[i] ?? String(v);
  }
  const unit = p.unit || '';
  if (unit === 'Hz' && Math.abs(v) >= 1000) return `${sig(v / 1000, 3)} kHz`;
  if (unit === '%') return `${Math.round(v)} %`;
  if (unit === 'dB') return `${v > 0 ? '+' : ''}${v.toFixed(1)} dB`;
  if (unit === ':1') return `${sig(v, 3)}:1`;
  if (unit === 'deg') return `${Math.round(v)}°`;
  if (unit === 'ms' && Math.abs(v) >= 1000) return `${sig(v / 1000, 3)} s`;
  const num = unit === '' && Math.abs(p.max - p.min) <= 2 ? v.toFixed(2) : sig(v, 3);
  return unit ? `${num} ${unit}` : num;
}

/** Build the canonical effect-table entry from a FX_DESC protocol message. */
export function fxFromDesc(d) {
  return {
    id: d.id, name: d.name, cpuCycles: d.cpuCycles, memBytes: d.memBytes, tailMs: d.tailMs, mixMode: d.mixMode,
    params: d.params.map((p) => {
      const o = { name: p.name, unit: p.unit, min: p.min, max: p.max, def: p.def, scale: p.scale };
      if (p.labels) o.labels = p.labels;
      return o;
    }),
  };
}

/** Table entry -> FX_DESC message (used by the mock device). */
export function descFromFx(fx) {
  return {
    type: 0x82, id: fx.id, name: fx.name, cpuCycles: fx.cpuCycles, memBytes: fx.memBytes, tailMs: fx.tailMs, mixMode: fx.mixMode,
    params: fx.params.map((p) => ({ ...p, labels: p.labels || '' })),
  };
}

/** Validate/clean a fetched table. Throws on garbage. */
export function normalizeFxTable(arr) {
  if (!Array.isArray(arr) || arr.length < 1 || arr.length > 64) throw new Error('bad fx table');
  return arr.map((e, i) => {
    if (!e || typeof e.name !== 'string') throw new Error('bad fx entry');
    const num = (x, d = 0) => (typeof x === 'number' && Number.isFinite(x) ? x : d);
    const params = (Array.isArray(e.params) ? e.params : []).slice(0, 4).map((p) => ({
      name: String(p.name ?? ''), unit: String(p.unit ?? ''), min: num(p.min), max: num(p.max, 1), def: num(p.def),
      scale: ['lin', 'log', 'step'].includes(p.scale) ? p.scale : 'lin', ...(p.labels ? { labels: String(p.labels) } : {}),
    }));
    return {
      id: i, name: e.name, cpuCycles: num(e.cpuCycles), memBytes: num(e.memBytes), tailMs: num(e.tailMs),
      mixMode: e.mixMode === 'send' ? 'send' : 'crossfade', params,
    };
  });
}

/** Admission check mirroring the engine budgets: summed memory vs pool, summed CPU vs budget. */
export function checkAdmission(fxEntries, pool = POOL_BYTES, cpuBudget = CPU_BUDGET) {
  let mem = 0, cpu = 0;
  for (const f of fxEntries) { mem += f.memBytes; cpu += f.cpuCycles; }
  // Same order as firmware engine_assign(): CPU budget first, then memory. The device is
  // authoritative (it also counts per-effect state bytes and pool fragmentation).
  if (cpu > cpuBudget) return { ok: false, code: 3, mem, cpu };
  if (mem > pool) return { ok: false, code: 2, mem, cpu };
  return { ok: true, code: 0, mem, cpu };
}
