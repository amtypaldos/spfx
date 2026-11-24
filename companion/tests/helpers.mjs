import { readFileSync } from 'node:fs';
import { normalizeFxTable } from '../js/fx.js';
export const fxTable = normalizeFxTable(JSON.parse(readFileSync(new URL('../data/fx_table.json', import.meta.url), 'utf8')));
export const tick = () => new Promise((r) => setTimeout(r, 0));
