/* SP-1 FX — persistent configuration: versioned record + two-page journal.
 *
 * Design (see docs/ADR-003-storage.md):
 *  - A record is 512 bytes: magic, layout version, payload length, sequence number,
 *    payload, CRC32. A page (4 KB) holds 8 records written append-only.
 *  - Two pages alternate. The newest valid record (highest sequence) wins.
 *  - A page is only erased when the OTHER page holds the newest record, so a power cut at any
 *    point leaves at least one valid, complete record.
 *  - Flash access goes through flash_ops_t so everything is testable on the host.
 *  - Never called from the audio thread. */
#ifndef SP1FX_CONFIG_STORE_H
#define SP1FX_CONFIG_STORE_H

#include "../dsp/engine.h"
#include <stdint.h>

#define STORE_PAGE_BYTES 4096u
#define STORE_RECORD_BYTES 512u
#define STORE_RECORDS_PER_PAGE (STORE_PAGE_BYTES / STORE_RECORD_BYTES)
#define STORE_LAYOUT_VERSION 1u

typedef struct {
	engine_config_t cfg;
	float bpm; /* 30..300 */
	uint8_t sync_on; /* sync-jack clock running; saved and restored at power-up */
} store_data_t;

/* page_index is 0 or 1 (the two journal pages). Offsets are relative to the page. */
typedef struct {
	void *ctx;
	int (*read)(void *ctx, uint32_t page_index, uint32_t off, void *buf, uint32_t len);
	/* off and len are multiples of 4; flash can only clear bits (NOR semantics) */
	int (*write)(void *ctx, uint32_t page_index, uint32_t off, const void *buf, uint32_t len);
	int (*erase)(void *ctx, uint32_t page_index);
} flash_ops_t;

typedef struct {
	flash_ops_t f;
	uint32_t seq;      /* sequence of the newest valid record, 0 if none */
	int active_page;   /* page holding the newest record, -1 if none */
	int next_slot;     /* next free record slot in active_page */
	int need_rescan;
} store_t;

uint32_t sp1fx_crc32(const uint8_t *p, uint32_t n); /* standard CRC-32 (zlib) */
void store_data_default(store_data_t *d);

/* Serialisation (exposed for tests and for the companion protocol). */
uint16_t store_encode(const store_data_t *d, uint8_t *payload, uint16_t cap);
int store_decode(const uint8_t *payload, uint16_t len, uint16_t layout_version, store_data_t *out); /* 0 ok */

int store_init(store_t *s, flash_ops_t ops);
/* 0 = loaded valid data; -1 = nothing valid (out gets defaults). Always sanitised. */
int store_load(store_t *s, store_data_t *out);
/* 0 ok, <0 flash error (state stays recoverable; the next save rescans) */
int store_save(store_t *s, const store_data_t *d);

/* ---- write debouncing: wear control, called from the control thread ---- */
#define STORE_QUIET_MS 1500u       /* wait this long after the last change */
#define STORE_MIN_INTERVAL_MS 5000u /* never write more often than this */

typedef struct {
	int dirty;
	uint32_t last_change_ms, last_write_ms;
	int ever_written;
} store_debounce_t;

void store_debounce_init(store_debounce_t *d);
void store_debounce_mark(store_debounce_t *d, uint32_t now_ms);
/* returns 1 when a write should happen now (and records it) */
int store_debounce_due(store_debounce_t *d, uint32_t now_ms);

#endif
