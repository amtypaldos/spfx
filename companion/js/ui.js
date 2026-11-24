// DOM layer. All user/preset/device strings go through textContent / setAttribute, never innerHTML.
import { normToValue, valueToNorm, formatValue, defaultNorm, stepLabels, POOL_BYTES, CPU_BUDGET } from './fx.js';
import { presetAdmission, cleanName } from './presets.js';

const $ = (id) => document.getElementById(id);
const clamp = (x, lo, hi) => Math.min(hi, Math.max(lo, x));

function h(tag, props = {}, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v === false || v == null) continue;
    if (k === 'class') el.className = v;
    else if (k === 'text') el.textContent = v;
    else el.setAttribute(k, v === true ? '' : String(v));
  }
  for (const k of kids) if (k != null) el.append(k);
  return el;
}

const fmtBytes = (b) => (b >= 1024 ? `${(b / 1024).toFixed(b >= 10240 ? 0 : 1)} KiB` : `${b} B`);
const fmtInt = (n) => n.toLocaleString('en-US');
const toDb = (peak) => (peak <= 0 ? -Infinity : 20 * Math.log10(peak / 32768));
const TOUCH_MS = 450;
const RANGE_MAX = 200;

export function mountUI({ store, ctl, library, env }) {
  const touched = new WeakMap();      // range element -> time of last user edit (don't fight an active drag)
  const markTouched = (el) => touched.set(el, performance.now());
  const isBusy = (el) => document.activeElement === el && performance.now() - (touched.get(el) ?? -1e9) < TOUCH_MS;

  function setRange(el, norm) {
    const v = Math.round(clamp(norm, 0, 1) * Number(el.max));
    if (isBusy(el)) return;
    if (Number(el.value) !== v) el.value = String(v);
    el.style.setProperty('--fill', `${(v / Number(el.max)) * 100}%`);
  }
  const fillOf = (el) => el.style.setProperty('--fill', `${((Number(el.value) - Number(el.min)) / (Number(el.max) - Number(el.min))) * 100}%`);

  // ---------- tabs ----------
  const tabs = [$('tabLive'), $('tabPresets')];
  const panels = { tabLive: $('liveView'), tabPresets: $('presetsView') };
  function selectTab(tab, focus) {
    for (const t of tabs) {
      const on = t === tab;
      t.setAttribute('aria-selected', String(on)); t.tabIndex = on ? 0 : -1; panels[t.id].hidden = !on;
    }
    if (focus) tab.focus();
    try { sessionStorage.setItem('sp1fx.tab', tab.id); } catch { /* optional */ }
    if (tab.id === 'tabPresets') renderPresets();
  }
  tabs.forEach((t, i) => {
    t.addEventListener('click', () => selectTab(t));
    t.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') { e.preventDefault(); selectTab(tabs[(i + (e.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length], true); }
      if (e.key === 'Home') { e.preventDefault(); selectTab(tabs[0], true); }
      if (e.key === 'End') { e.preventDefault(); selectTab(tabs[tabs.length - 1], true); }
    });
  });
  try { const t = sessionStorage.getItem('sp1fx.tab'); if (t && panels[t]) selectTab($(t)); } catch { /* optional */ }

  // ---------- notices ----------
  let noticeTimer = null;
  ctl.onNotice(({ level, text }) => showNotice(level, text));
  function showNotice(level, text) {
    const n = $('notice');
    n.className = `notice${level === 'error' ? ' error' : ''}`;
    $('noticeText').textContent = text;
    n.hidden = false;
    clearTimeout(noticeTimer);
    if (level !== 'error') noticeTimer = setTimeout(() => { n.hidden = true; }, 6000);
  }
  $('noticeClose').addEventListener('click', () => { $('notice').hidden = true; });

  if (!env.serialSupported) {
    const s = $('support');
    s.textContent = 'This browser cannot talk to the SP-1: connecting needs Web Serial, which is available in desktop Chrome or Edge (not Safari, Firefox or iOS). You can still try the demo device and manage presets here.';
    s.hidden = false;
    $('connect').disabled = true;
    $('connect').title = 'Needs Web Serial (desktop Chrome or Edge)';
  }

  // ---------- header buttons ----------
  $('connect').addEventListener('click', () => env.connectSerial());
  $('demo').addEventListener('click', () => env.startDemo());
  $('disconnect').addEventListener('click', () => env.disconnect());

  // ---------- global controls ----------
  const bind = (el, fn) => el.addEventListener('input', () => { markTouched(el); fillOf(el); fn(); });
  bind($('master'), () => ctl.setMaster(Number($('master').value) / RANGE_MAX));
  bind($('gain'), () => ctl.setInputGainDb(Number($('gain').value) / 10));
  $('gBypass').addEventListener('click', () => ctl.setGlobalBypass(!store.model.globalBypass));
  $('gSync').addEventListener('click', () => ctl.setSync(!store.model.syncOn));
  $('bpm').addEventListener('change', () => {
    const v = Number($('bpm').value);
    if (Number.isFinite(v) && $('bpm').value !== '') ctl.setBpm(v); else renderGlobal(store.model);
  });

  // ---------- slots ----------
  const slotUI = [];
  function buildSlot(i) {
    const n = i + 1;
    const title = h('h2', { id: `slot${n}Title`, class: 'lane-number', text: `Slot ${n}` });
    const badge = h('span', { class: 'badge' });
    const prev = h('button', { type: 'button', 'aria-label': `Previous effect, slot ${n}`, text: '‹' });
    const next = h('button', { type: 'button', 'aria-label': `Next effect, slot ${n}`, text: '›' });
    const sel = h('select', { 'aria-label': `Effect, slot ${n}` });
    const wetName = h('label', { class: 'name', for: `slot${n}Wet`, text: 'Wet / dry' });
    const wetVal = h('span', { class: 'val' });
    const wet = h('input', { type: 'range', id: `slot${n}Wet`, min: 0, max: RANGE_MAX, step: 1, value: RANGE_MAX });
    const byp = h('button', { type: 'button', class: 'toggle', 'aria-pressed': 'false' }, h('span', { class: 'sw', 'aria-hidden': 'true' }), h('span', { text: 'Bypass' }));
    byp.setAttribute('aria-label', `Bypass slot ${n}`);
    const params = h('div', { class: 'params' });
    const rows = [0, 1, 2, 3].map((p) => {
      const id = `slot${n}P${p}`;
      const name = h('label', { class: 'name', for: id });
      const val = h('span', { class: 'val' });
      const range = h('input', { type: 'range', id, min: 0, max: RANGE_MAX, step: 1, value: 0 });
      const row = h('div', { class: 'ctl' }, h('div', { class: 'top' }, name, val), range);
      params.append(row);
      return { row, name, val, range, fxId: -1 };
    });
    const none = h('p', { class: 'none', text: 'No effect in this slot.' });
    params.append(none);
    const info = h('span');
    const reset = h('button', { type: 'button', class: 'btn small', text: 'Reset parameters' });
    reset.setAttribute('aria-label', `Reset slot ${n} parameters to defaults`);
    const card = h('section', { class: 'card slot', 'aria-labelledby': `slot${n}Title` },
      h('div', { class: 'head' }, title, badge),
      h('div', { class: 'picker' }, prev, sel, next),
      h('div', { class: 'ctl' }, h('div', { class: 'top' }, wetName, wetVal), wet),
      h('div', { class: 'row' }, byp),
      params,
      h('div', { class: 'foot' }, info, reset));
    $('slots').append(card);

    const ui = { card, badge, sel, prev, next, wet, wetName, wetVal, byp, rows, none, info, reset, params, tableRef: null };
    sel.addEventListener('change', () => ctl.setSlotFx(i, Number(sel.value)));
    const cycle = (d) => { const len = store.model.fxTable.length; ctl.setSlotFx(i, (store.model.slots[i].fx + d + len) % len); };
    prev.addEventListener('click', () => cycle(-1));
    next.addEventListener('click', () => cycle(1));
    bind(wet, () => ctl.setWet(i, Number(wet.value) / RANGE_MAX));
    byp.addEventListener('click', () => ctl.setBypass(i, !store.model.slots[i].bypass));
    reset.addEventListener('click', () => ctl.resetSlotParams(i));
    rows.forEach((r, p) => {
      bind(r.range, () => {
        const m = store.model; const fx = m.fxTable[m.slots[i].fx]; const par = fx.params[p];
        ctl.setParam(i, p, Number(r.range.value) / Number(r.range.max));
        // optimistic value text for the control being dragged
        r.val.textContent = formatValue(par, Number(r.range.value) / Number(r.range.max));
        r.range.setAttribute('aria-valuetext', r.val.textContent);
      });
      r.range.addEventListener('dblclick', () => { ctl.setParam(i, p, defaultNorm(store.model.fxTable[store.model.slots[i].fx].params[p])); });
    });
    slotUI.push(ui);
  }
  for (let i = 0; i < 4; i++) buildSlot(i);

  function renderSlot(i, m) {
    const ui = slotUI[i]; const s = m.slots[i]; const table = m.fxTable; const fx = table[s.fx] ?? table[0];
    if (ui.tableRef !== table) {   // effect list changed (device table vs fallback): rebuild options
      ui.sel.replaceChildren(...table.map((f, idx) => h('option', { value: idx, text: f.name })));
      ui.tableRef = table;
    }
    if (ui.sel.value !== String(s.fx)) ui.sel.value = String(s.fx);
    ui.badge.textContent = fx.mixMode === 'send' ? 'Send effect' : 'Crossfade effect';
    ui.wetName.textContent = fx.mixMode === 'send' ? 'Send level' : 'Wet / dry';
    ui.wetVal.textContent = `${Math.round(s.wet * 100)} %`;
    setRange(ui.wet, s.wet); ui.wet.setAttribute('aria-valuetext', ui.wetVal.textContent);
    ui.byp.setAttribute('aria-pressed', String(s.bypass));
    ui.byp.querySelector('span:last-child').textContent = s.bypass ? 'Bypassed' : 'Bypass';
    ui.card.dataset.bypass = String(s.bypass);
    const vals = s.params[s.fx] ?? [0, 0, 0, 0];
    ui.rows.forEach((r, p) => {
      const par = fx.params[p];
      r.row.hidden = !par;
      if (!par) return;
      if (r.fxId !== s.fx || r.tableRef !== table) {
        r.name.textContent = par.name;
        const stepped = par.scale === 'step';
        r.range.max = stepped ? String(par.max - par.min) : String(RANGE_MAX);
        r.range.step = '1';
        r.fxId = s.fx; r.tableRef = table;
      }
      r.val.textContent = formatValue(par, vals[p]);
      setRange(r.range, vals[p]);
      r.range.setAttribute('aria-valuetext', r.val.textContent);
    });
    ui.none.hidden = fx.params.length > 0;
    ui.reset.disabled = fx.params.length === 0 || !m.synced;
    const tail = fx.tailMs ? ` · tail ${fx.tailMs >= 1000 ? `${(fx.tailMs / 1000).toFixed(fx.tailMs % 1000 ? 1 : 0)} s` : `${fx.tailMs} ms`}` : '';
    ui.info.textContent = `${fmtBytes(fx.memBytes)} · ${fx.cpuCycles} cycles${tail}`;
  }

  // ---------- chain ----------
  function renderChain(m) {
    const list = $('chain');
    const key = m.order.join() + '|' + m.slots.map((s) => s.fx).join();
    if (list.dataset.key === key && list.dataset.tbl === String(m.fxTable.length)) return;
    list.dataset.key = key; list.dataset.tbl = String(m.fxTable.length);
    list.replaceChildren(...m.order.map((slot, pos) => {
      const name = m.fxTable[m.slots[slot].fx]?.name ?? '?';
      const move = (to) => { const o = m.order.slice(); [o[pos], o[to]] = [o[to], o[pos]]; ctl.setOrder(o); };
      const left = h('button', { type: 'button', text: '←', 'aria-label': `Move slot ${slot + 1} (${name}) earlier in the chain` });
      const right = h('button', { type: 'button', text: '→', 'aria-label': `Move slot ${slot + 1} (${name}) later in the chain` });
      left.disabled = pos === 0 || !m.synced; right.disabled = pos === 3 || !m.synced;
      left.addEventListener('click', () => move(pos - 1)); right.addEventListener('click', () => move(pos + 1));
      return h('li', {}, h('span', { class: 'pos', text: `POSITION ${pos + 1}` }), h('span', { class: 'nm', text: `${slot + 1}. ${name}` }), h('div', { class: 'mv' }, left, right));
    }));
  }

  // ---------- global / tiles ----------
  function renderGlobal(m) {
    $('gBypass').setAttribute('aria-pressed', String(m.globalBypass));
    $('gSync').setAttribute('aria-pressed', String(m.syncOn));
    setRange($('master'), m.master); $('masterVal').textContent = `${Math.round(m.master * 100)} %`;
    $('master').setAttribute('aria-valuetext', $('masterVal').textContent);
    const g = $('gain');
    if (!isBusy(g)) { g.value = String(Math.round(m.inGainDb * 10)); fillOf(g); }
    $('gainVal').textContent = `${m.inGainDb > 0 ? '+' : ''}${m.inGainDb.toFixed(1)} dB`;
    g.setAttribute('aria-valuetext', $('gainVal').textContent);
    if (document.activeElement !== $('bpm')) $('bpm').value = String(Math.round(m.bpm * 10) / 10);
  }
  function setBar(bar, frac, cls) {
    bar.firstElementChild.style.width = `${clamp(frac, 0, 1) * 100}%`;
    bar.classList.toggle('hot', cls === 'hot'); bar.classList.toggle('clip', cls === 'clip');
  }
  function renderTiles(m) {
    const live = m.synced;
    const dash = '—';
    $('tFw').textContent = live ? (m.fw || dash) : dash;
    $('tFwSub').textContent = live ? `protocol v${m.proto}${m.mode === 'demo' ? ' · simulated' : ''}` : 'protocol —';
    const q = m.meters;
    $('tCpu').textContent = live ? `${q.cpuPct} %` : dash;
    $('tCpuSub').textContent = live ? `peak ${q.cpuPeakPct} % · budget ${m.cpuBudget} cycles/frame` : 'peak —';
    setBar($('cpuBar'), q.cpuPct / 100, q.cpuPct > 90 ? 'clip' : q.cpuPct > 75 ? 'hot' : '');
    const pool = m.poolBytes || POOL_BYTES;
    $('tMem').textContent = live ? fmtBytes(q.memUsed) : dash;
    $('tMemSub').textContent = live ? `of ${fmtBytes(pool)} (${Math.round((q.memUsed / pool) * 100)} %)` : 'of —';
    setBar($('memBar'), q.memUsed / pool, q.memUsed / pool > 0.9 ? 'clip' : q.memUsed / pool > 0.75 ? 'hot' : '');
    $('tRuns').textContent = live ? `${fmtInt(q.underruns)} / ${fmtInt(q.overruns)}` : dash;
    $('tClips').textContent = live ? `${fmtInt(q.inClips)} / ${fmtInt(q.outClips)}` : dash;
    $('tNan').textContent = live ? `NaN events ${fmtInt(q.nanEvents)}` : 'NaN events —';
    for (const [id, valId, peak, clips] of [['inBar', 'inVal', q.inPeak, q.inClips], ['outBar', 'outVal', q.outPeak, q.outClips]]) {
      const db = toDb(peak);
      const bar = $(id);
      setBar(bar, live ? (db + 60) / 60 : 0, db >= -0.1 ? 'clip' : db > -6 ? 'hot' : '');
      bar.setAttribute('aria-valuenow', String(Number.isFinite(db) ? Math.round(db) : -60));
      $(valId).textContent = live ? `${Number.isFinite(db) ? db.toFixed(1) : '−∞'} dBFS · ${fmtInt(clips)} clips` : dash;
    }
  }

  function renderConn(m) {
    const c = $('conn'); c.dataset.state = m.conn;
    $('connText').textContent = m.conn === 'connected'
      ? (m.mode === 'demo' ? 'Demo device (simulated, in this page)' : `Connected to SP-1 · firmware ${m.fw || '?'}`)
      : m.conn === 'connecting' ? 'Connecting…' : 'Not connected';
    const on = m.conn !== 'disconnected';
    $('connect').hidden = on; $('demo').hidden = on; $('disconnect').hidden = !on;
    $('disconnect').textContent = m.mode === 'demo' ? 'Stop demo' : 'Disconnect';
    $('demoPanel').hidden = m.mode !== 'demo';
    const wrap = $('liveWrap'); wrap.classList.toggle('stale', !m.synced);
    for (const el of wrap.querySelectorAll('input,select,button')) {
      if (el.closest('#demoPanel')) continue;
      el.disabled = !m.synced;
    }
    if (!m.synced) for (const ui of slotUI) ui.reset.disabled = true;
  }

  function render(m) {
    renderConn(m); renderGlobal(m);
    for (let i = 0; i < 4; i++) renderSlot(i, m);
    renderChain(m); renderTiles(m);
    if (!$('presetsView').hidden) renderPresetControls();
  }

  // ---------- presets ----------
  let selected = library.list()[0]?.id ?? null;
  let armed = null;
  function arm(btn, label, action) {
    // two-step destructive action: first click arms, second confirms
    if (armed && armed.btn === btn) { clearTimeout(armed.t); const a = armed; armed = null; btn.textContent = a.orig; action(); return; }
    if (armed) { clearTimeout(armed.t); armed.btn.textContent = armed.orig; }
    const orig = btn.textContent; btn.textContent = label;
    armed = { btn, orig, t: setTimeout(() => { btn.textContent = orig; armed = null; }, 4000) };
  }
  const msg = (el, text, kind = '') => { el.textContent = text; el.className = `msg ${kind}`; };

  function renderPresets() {
    const list = $('plist');
    if (!library.get(selected)) selected = library.list()[0]?.id ?? null;
    list.replaceChildren(...library.list().map((it) => {
      const b = h('button', { type: 'button', 'aria-pressed': String(it.id === selected) }, h('span', { text: it.preset.name }), h('span', { class: 'tag', text: it.factory ? 'Factory' : 'Yours' }));
      b.addEventListener('click', () => { selected = it.id; msg($('pMsg'), ''); $('exportWrap').hidden = true; renderPresets(); });
      return h('li', {}, b);
    }));
    $('storeNotice').hidden = library.persistent;
    renderPresetControls();
  }
  function renderPresetControls() {
    const m = store.model; const it = library.get(selected);
    const connected = m.synced;
    $('pName').textContent = it ? it.preset.name : 'No preset selected';
    $('pTag').textContent = it ? (it.factory ? 'Factory preset' : 'Your preset') : '';
    const table = m.fxTable;
    $('pSlots').replaceChildren(...(it ? it.preset.slots : []).map((s, i) => {
      const fx = table.find((f) => f.name.toLowerCase().replace(/[^a-z0-9]+/g, '_') === s.fx);
      return h('li', {}, h('small', { text: `SLOT ${i + 1}${s.bypass ? ' · BYPASS' : ''}` }), h('span', { text: fx?.name ?? s.fx }));
    }));
    if (it) {
      const a = presetAdmission(it.preset, table, m.poolBytes || POOL_BYTES, m.cpuBudget || CPU_BUDGET);
      const pool = m.poolBytes || POOL_BYTES, bud = m.cpuBudget || CPU_BUDGET;
      const info = $('pInfo');
      info.textContent = `${a.ok ? 'Fits the engine budget' : (a.code === 2 ? 'Would be rejected: not enough DSP memory' : 'Would be rejected: CPU budget exceeded')} — ${fmtInt(a.mem)} of ${fmtInt(pool)} B memory, ${a.cpu} of ${bud} cycles. BPM ${it.preset.bpm}, master ${Math.round(it.preset.master * 100)} %, input ${it.preset.inputGainDb} dB.`;
      info.className = `kv ${a.ok ? '' : 'bad'}`;
    } else { $('pInfo').textContent = ''; }
    $('pLoad').disabled = !it || !connected;
    $('pLoad').title = connected ? '' : 'Connect the device or start the demo to load a preset';
    $('pUpdate').disabled = !it || !connected;
    for (const id of ['pDup', 'pExport', 'pDelete']) $(id).disabled = !it;
    $('pRename').disabled = !it;
    if (document.activeElement !== $('pRename')) $('pRename').value = it ? it.preset.name : '';
    $('saveBtn').disabled = !connected;
    $('saveHint').textContent = connected ? 'Captures the four slots, wet levels, parameters, chain order, master, input gain and BPM as they are now.' : 'Connect the device or start the demo to capture its current settings.';
  }

  $('pLoad').addEventListener('click', async () => {
    const it = library.get(selected); if (!it) return;
    $('pLoad').disabled = true;
    msg($('pMsg'), `Loading “${it.preset.name}”…`);
    const { failures } = await ctl.applyPreset(it.preset);
    if (failures.length) {
      msg($('pMsg'), `Loaded with ${failures.length} problem${failures.length > 1 ? 's' : ''}: ${failures.join(' ')}`, 'error');
      showNotice('error', failures.join(' '));
    } else msg($('pMsg'), `Loaded “${it.preset.name}”.`, 'ok');
    renderPresetControls();
  });
  $('pDup').addEventListener('click', () => {
    const d = library.duplicate(selected);
    if (d) { selected = d.id; msg($('pMsg'), 'Duplicated.', 'ok'); renderPresets(); } else msg($('pMsg'), 'Library is full.', 'error');
  });
  $('pDelete').addEventListener('click', (e) => arm(e.currentTarget, 'Click again to delete', () => {
    library.delete(selected); msg($('pMsg'), 'Deleted.', 'ok'); renderPresets();
  }));
  $('pUpdate').addEventListener('click', (e) => arm(e.currentTarget, 'Click again to overwrite', () => {
    const { presetFromModel } = env.presets;
    library.overwrite(selected, presetFromModel(store.model, 'x')); msg($('pMsg'), 'Preset updated from the current device settings.', 'ok'); renderPresets();
  }));
  $('renameForm').addEventListener('submit', (e) => {
    e.preventDefault();
    if (library.rename(selected, $('pRename').value)) { msg($('pMsg'), 'Renamed.', 'ok'); renderPresets(); } else msg($('pMsg'), 'Enter a name.', 'error');
  });
  $('pExport').addEventListener('click', () => {
    const json = library.exportJSON(selected); const it = library.get(selected);
    if (!json) return;
    $('pExportText').value = json; $('exportWrap').hidden = false;
    try {
      const url = URL.createObjectURL(new Blob([json], { type: 'application/json' }));
      const a = h('a', { href: url, download: `${cleanName(it.preset.name).replace(/[^\w.-]+/g, '_') || 'preset'}.sp1fx.json` });
      document.body.append(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(url), 2000);
      msg($('pMsg'), 'Exported. The JSON is also shown below.', 'ok');
    } catch { msg($('pMsg'), 'Could not start a download; copy the JSON below instead.', 'error'); }
  });
  $('saveForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const name = cleanName($('saveName').value) || 'My preset';
    const it = library.save(store.model, name);
    if (!it) { msg($('saveHint'), 'Library is full.', 'error'); return; }
    selected = it.id; $('saveName').value = ''; msg($('pMsg'), `Saved “${it.preset.name}”.`, 'ok'); renderPresets();
  });
  function doImport(text) {
    const r = library.importText(text);
    if (!r.ok) { msg($('importMsg'), `Import failed: ${r.errors.join(' ')}`, 'error'); return; }
    selected = r.item.id;
    const a = presetAdmission(r.item.preset, store.model.fxTable, store.model.poolBytes || POOL_BYTES, store.model.cpuBudget || CPU_BUDGET);
    msg($('importMsg'), `Imported “${r.item.preset.name}”.${r.warnings.length ? ` Adjusted: ${r.warnings.slice(0, 4).join(' ')}${r.warnings.length > 4 ? ` (+${r.warnings.length - 4} more)` : ''}` : ''}${a.ok ? '' : ' Note: this preset exceeds the device budget and would be rejected when loaded.'}`, r.warnings.length || !a.ok ? 'error' : 'ok');
    renderPresets();
  }
  $('pImport').addEventListener('click', () => doImport($('pPaste').value));
  $('pFile').addEventListener('change', async (e) => {
    const f = e.target.files[0]; e.target.value = '';
    if (!f) return;
    if (f.size > 64 * 1024) { msg($('importMsg'), 'Import failed: file is too large to be a preset (limit 64 KB).', 'error'); return; }
    try { doImport(await f.text()); } catch { msg($('importMsg'), 'Import failed: could not read the file.', 'error'); }
  });
  $('pRestore').addEventListener('click', (e) => arm(e.currentTarget, 'Click again to restore', () => {
    const n = library.restoreFactory(); selected = library.list()[0]?.id ?? null; msg($('pMsg'), `Restored ${n} factory presets; your own presets were kept.`, 'ok'); renderPresets();
  }));

  // ---------- demo panel ----------
  let autoTimer = null;
  const demo = () => env.demo?.device;
  $('dFader').addEventListener('click', () => {
    const d = demo(); if (!d) return;
    const slot = Math.floor(Math.random() * 4), fx = d.fx[d.s.fxId[slot]];
    if (!fx.params.length) { $('dInfo').textContent = `Slot ${slot + 1} has no effect; try again.`; return; }
    const idx = Math.floor(Math.random() * fx.params.length), v = Math.random();
    d.simulateFader(slot, idx, v);
    $('dInfo').textContent = `Fader: slot ${slot + 1}, ${fx.name} · ${fx.params[idx].name} → ${formatValue(fx.params[idx], v)} (sent as a 0xA0 event)`;
  });
  $('dCycle').addEventListener('click', () => {
    const d = demo(); if (!d) return;
    const slot = Number($('dSlot').value);
    const ok = d.simulateCycleFx(slot);
    $('dInfo').textContent = ok ? `Button cycle: slot ${slot + 1} → ${d.fx[d.s.fxId[slot]].name}` : `Button cycle on slot ${slot + 1} was rejected by admission control (0xA1 event).`;
  });
  $('dOverload').addEventListener('click', async () => {
    const delay = store.model.fxTable.findIndex((f) => f.name === 'Delay');
    for (let i = 0; i < 4; i++) if (store.model.slots[i].fx !== delay) { await ctl.setSlotFx(i, delay); }
    $('dInfo').textContent = 'Asked for Delay (75 KiB) in every slot: only two fit in the 150 KiB pool.';
  });
  $('dAuto').addEventListener('change', (e) => {
    clearInterval(autoTimer);
    if (e.target.checked) autoTimer = setInterval(() => $('dFader').click(), 700);
  });
  store.subscribe((m) => { if (m.mode !== 'demo' && $('dAuto').checked) { $('dAuto').checked = false; clearInterval(autoTimer); } });

  store.subscribe(render);
  render(store.model);
  renderPresets();
  return { render, renderPresets };
}
