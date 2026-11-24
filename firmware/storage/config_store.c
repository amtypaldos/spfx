#include "config_store.h"
#include <math.h>
#include <string.h>

#define MAGIC 0x58463153u /* "S1FX" little-endian */
#define HDR_BYTES 12u     /* magic, version, len, seq */

/* ---------------------------------------------------------------- crc32 */
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1u));
	}
	return ~c;
}
uint32_t sp1fx_crc32(const uint8_t *p, uint32_t n) { return crc32(p, n); }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ----------------------------------------------------------- payload v1 */
static uint16_t q16(float x)
{
	if (!(x >= 0.0f)) return 0;
	if (x > 1.0f) x = 1.0f;
	return (uint16_t)(x * 65535.0f + 0.5f);
}
static float uq16(uint16_t v) { return (float)v / 65535.0f; }

void store_data_default(store_data_t *d)
{
	engine_config_default(&d->cfg);
	d->bpm = 120.0f;
	d->sync_on = 0;
}

/* layout: fx_id[4] bypass[4] order[4] global sync_on fx_count | in_gain_cdB(i16) master bpm_x10 | wet[4] | params[4][fx_count][4] */
uint16_t store_encode(const store_data_t *d, uint8_t *p, uint16_t cap)
{
	uint16_t need = (uint16_t)(15 + 2 + 2 + 2 + 8 + ENGINE_SLOTS * FX_COUNT * FX_MAX_PARAMS * 2);
	if (cap < need) return 0;
	uint8_t *w = p;
	for (int s = 0; s < ENGINE_SLOTS; s++) *w++ = d->cfg.fx_id[s];
	for (int s = 0; s < ENGINE_SLOTS; s++) *w++ = d->cfg.bypass[s];
	for (int s = 0; s < ENGINE_SLOTS; s++) *w++ = d->cfg.order[s];
	*w++ = d->cfg.global_bypass;
	*w++ = d->sync_on ? 1 : 0; /* was reserved(0): old records read as sync off */
	*w++ = FX_COUNT;
	float g = d->cfg.in_gain_db;
	if (!(g >= -12.0f)) g = -12.0f;
	if (g > 12.0f) g = 12.0f;
	put16(w, (uint16_t)(int16_t)lrintf(g * 100.0f)); w += 2;
	put16(w, q16(d->cfg.master)); w += 2;
	float bpm = d->bpm;
	if (!(bpm >= 30.0f)) bpm = 120.0f;
	if (bpm > 300.0f) bpm = 300.0f;
	put16(w, (uint16_t)lrintf(bpm * 10.0f)); w += 2;
	for (int s = 0; s < ENGINE_SLOTS; s++) { put16(w, q16(d->cfg.wet[s])); w += 2; }
	for (int s = 0; s < ENGINE_SLOTS; s++)
		for (int f = 0; f < FX_COUNT; f++)
			for (int i = 0; i < FX_MAX_PARAMS; i++) { put16(w, q16(d->cfg.params[s][f][i])); w += 2; }
	return (uint16_t)(w - p);
}

int store_decode(const uint8_t *p, uint16_t len, uint16_t layout_version, store_data_t *out)
{
	if (layout_version != STORE_LAYOUT_VERSION) return -1; /* unknown layout: caller falls back */
	if (len < 15 + 6 + 8) return -1;
	store_data_default(out);
	const uint8_t *r = p;
	for (int s = 0; s < ENGINE_SLOTS; s++) out->cfg.fx_id[s] = *r++;
	for (int s = 0; s < ENGINE_SLOTS; s++) out->cfg.bypass[s] = *r++;
	for (int s = 0; s < ENGINE_SLOTS; s++) out->cfg.order[s] = *r++;
	out->cfg.global_bypass = *r++;
	out->sync_on = *r++ == 1 ? 1 : 0;
	int stored_fx = *r++;
	out->cfg.in_gain_db = (float)(int16_t)get16(r) / 100.0f; r += 2;
	out->cfg.master = uq16(get16(r)); r += 2;
	out->bpm = (float)get16(r) / 10.0f; r += 2;
	for (int s = 0; s < ENGINE_SLOTS; s++) { out->cfg.wet[s] = uq16(get16(r)); r += 2; }
	uint32_t need = (uint32_t)(r - p) + (uint32_t)ENGINE_SLOTS * (uint32_t)stored_fx * FX_MAX_PARAMS * 2u;
	if (stored_fx < 1 || need > len) return -1;
	for (int s = 0; s < ENGINE_SLOTS; s++)
		for (int f = 0; f < stored_fx; f++)
			for (int i = 0; i < FX_MAX_PARAMS; i++) {
				float v = uq16(get16(r)); r += 2;
				if (f < FX_COUNT) out->cfg.params[s][f][i] = v; /* effects added later keep defaults */
			}
	engine_config_sanitize(&out->cfg);
	if (!(out->bpm >= 30.0f && out->bpm <= 300.0f)) out->bpm = 120.0f;
	return 0;
}

/* --------------------------------------------------------------- journal */

/* 1 = valid record. */
static int read_record(store_t *s, int page, int slot, uint32_t *seq_out, store_data_t *data)
{
	uint8_t rec[STORE_RECORD_BYTES];
	if (s->f.read(s->f.ctx, (uint32_t)page, (uint32_t)slot * STORE_RECORD_BYTES, rec, STORE_RECORD_BYTES) != 0) return 0;
	if (get32(rec) != MAGIC) return 0;
	uint16_t ver = get16(rec + 4), len = get16(rec + 6);
	if (len > STORE_RECORD_BYTES - HDR_BYTES - 4) return 0;
	if (crc32(rec, HDR_BYTES + len) != get32(rec + HDR_BYTES + len)) return 0;
	uint32_t seq = get32(rec + 8);
	if (seq == 0 || seq == 0xFFFFFFFFu) return 0;
	if (data && store_decode(rec + HDR_BYTES, len, ver, data) != 0) return 0;
	if (seq_out) *seq_out = seq;
	return 1;
}

static int slot_blank(store_t *s, int page, int slot)
{
	uint8_t rec[STORE_RECORD_BYTES];
	if (s->f.read(s->f.ctx, (uint32_t)page, (uint32_t)slot * STORE_RECORD_BYTES, rec, STORE_RECORD_BYTES) != 0) return 0;
	for (uint32_t i = 0; i < STORE_RECORD_BYTES; i++)
		if (rec[i] != 0xFF) return 0;
	return 1;
}

static void scan(store_t *s)
{
	uint32_t best = 0;
	int best_page = -1, best_slot = -1;
	for (int p = 0; p < 2; p++)
		for (int k = 0; k < (int)STORE_RECORDS_PER_PAGE; k++) {
			uint32_t q;
			if (read_record(s, p, k, &q, 0) && q > best) { best = q; best_page = p; best_slot = k; }
		}
	s->seq = best;
	s->active_page = best_page;
	s->need_rescan = 0;
	if (best_page < 0) { s->next_slot = 0; return; }
	/* next free slot: after the last slot that is not blank (a torn record counts as used) */
	int last_used = best_slot;
	for (int k = best_slot + 1; k < (int)STORE_RECORDS_PER_PAGE; k++)
		if (!slot_blank(s, best_page, k)) last_used = k;
	s->next_slot = last_used + 1;
}

int store_init(store_t *s, flash_ops_t ops)
{
	memset(s, 0, sizeof *s);
	s->f = ops;
	scan(s);
	return 0;
}

int store_load(store_t *s, store_data_t *out)
{
	if (s->active_page < 0) { store_data_default(out); return -1; }
	/* the newest record was validated (including decode) during scan; read it again */
	for (int k = (int)STORE_RECORDS_PER_PAGE - 1; k >= 0; k--) {
		uint32_t q;
		if (read_record(s, s->active_page, k, &q, out) && q == s->seq) return 0;
	}
	store_data_default(out);
	return -1;
}

int store_save(store_t *s, const store_data_t *d)
{
	if (s->need_rescan) scan(s);

	uint8_t rec[STORE_RECORD_BYTES];
	memset(rec, 0xFF, sizeof rec);
	uint8_t payload[STORE_RECORD_BYTES - HDR_BYTES - 4];
	uint16_t len = store_encode(d, payload, sizeof payload);
	if (!len) return -2;

	int page = s->active_page, slot = s->next_slot;
	if (page < 0) {
		/* nothing valid anywhere. Use page 0; if it is not blank, erase it first. */
		page = 0; slot = 0;
		for (int k = 0; k < (int)STORE_RECORDS_PER_PAGE; k++)
			if (!slot_blank(s, 0, k)) { if (s->f.erase(s->f.ctx, 0) != 0) { s->need_rescan = 1; return -1; } break; }
	} else if (slot >= (int)STORE_RECORDS_PER_PAGE) {
		/* page full: the other page only holds older records, so it is safe to erase */
		page = 1 - page; slot = 0;
		if (s->f.erase(s->f.ctx, (uint32_t)page) != 0) { s->need_rescan = 1; return -1; }
	}

	uint32_t seq = s->seq + 1;
	put32(rec, MAGIC);
	put16(rec + 4, STORE_LAYOUT_VERSION);
	put16(rec + 6, len);
	put32(rec + 8, seq);
	memcpy(rec + HDR_BYTES, payload, len);
	put32(rec + HDR_BYTES + len, crc32(rec, HDR_BYTES + len));

	if (s->f.write(s->f.ctx, (uint32_t)page, (uint32_t)slot * STORE_RECORD_BYTES, rec, STORE_RECORD_BYTES) != 0) {
		s->need_rescan = 1; /* may be torn; the next save re-reads flash before trusting cached state */
		return -1;
	}
	s->seq = seq;
	s->active_page = page;
	s->next_slot = slot + 1;
	return 0;
}

/* ------------------------------------------------------------ debounce */

void store_debounce_init(store_debounce_t *d) { memset(d, 0, sizeof *d); }

void store_debounce_mark(store_debounce_t *d, uint32_t now)
{
	d->dirty = 1;
	d->last_change_ms = now;
}

int store_debounce_due(store_debounce_t *d, uint32_t now)
{
	if (!d->dirty) return 0;
	if (now - d->last_change_ms < STORE_QUIET_MS) return 0;
	if (d->ever_written && now - d->last_write_ms < STORE_MIN_INTERVAL_MS) return 0;
	d->dirty = 0;
	d->last_write_ms = now;
	d->ever_written = 1;
	return 1;
}
