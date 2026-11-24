// Entry point: wires store, controller, preset library, Web Serial and the in-page demo device.
import { Store } from './state.js';
import { Controller } from './controller.js';
import { normalizeFxTable } from './fx.js';
import { PresetLibrary, memoryStorage, presetFromModel } from './presets.js';
import { MockDevice, createLoopback } from './mock-device.js';
import { WebSerialTransport, serialSupported } from './device.js';
import { mountUI } from './ui.js';

async function loadFallbackTable() {
  const res = await fetch('data/fx_table.json', { cache: 'no-cache' });   // same-origin static file
  if (!res.ok) throw new Error(`fx_table.json: HTTP ${res.status}`);
  return normalizeFxTable(await res.json());
}

function getStorage() {
  try { const s = window.localStorage; s.setItem('sp1fx.probe', '1'); s.removeItem('sp1fx.probe'); return s; } catch { return null; }
}

async function main() {
  let table;
  try { table = await loadFallbackTable(); }
  catch (e) {
    document.getElementById('support').textContent = `Could not load data/fx_table.json (${e.message}). Serve this folder over HTTP, for example: python3 -m http.server`;
    document.getElementById('support').hidden = false;
    return;
  }
  const store = new Store();
  const ctl = new Controller(store, table);
  const library = new PresetLibrary(getStorage(), table);
  const env = { serialSupported: serialSupported(), demo: null, presets: { presetFromModel } };

  let tickTimer = null;
  const stopDemo = () => { clearInterval(tickTimer); tickTimer = null; env.demo = null; };

  env.connectSerial = async () => {
    try {
      const t = await WebSerialTransport.request();
      store.update((m) => { m.conn = 'connecting'; });
      await t.open();
      await ctl.connect(t, 'serial');
    } catch (e) {
      if (e?.name === 'NotFoundError') { store.update((m) => { m.conn = 'disconnected'; }); return; }   // user closed the picker
      store.update((m) => { m.conn = 'disconnected'; });
      ctl.notice('error', `Could not open the serial port: ${e.message ?? e}`);
    }
  };
  env.startDemo = async () => {
    const device = new MockDevice(table);
    const transport = createLoopback(device);
    env.demo = { device, transport };
    tickTimer = setInterval(() => device.tick(50), 50);
    try { await ctl.connect(transport, 'demo'); } catch { stopDemo(); }
  };
  env.disconnect = async () => { stopDemo(); await ctl.disconnect(); };
  store.subscribe((m) => { if (m.conn === 'disconnected' && env.demo) stopDemo(); });

  mountUI({ store, ctl, library, env });
  // test/debug hook, read-only conveniences (no network, nothing sensitive)
  window.sp1fx = { store, ctl, library, env };
}

main();
