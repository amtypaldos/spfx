/* SP-1 FX — device side of the companion protocol v1 (docs/PROTOCOL.md).
 *
 * Pure C99: no I/O, no allocation, no globals. The caller owns a proto_t, feeds received
 * bytes into proto_rx_byte(), calls proto_tick() from the control thread (e.g. every 8 ms,
 * right after controller_tick()) and supplies a tx callback that writes bytes to the
 * CDC-ACM port. Everything runs on the control thread; the audio thread is never touched
 * except for reading/resetting the diagnostic peaks in engine_t.diag. */
#ifndef SP1FX_PROTOCOL_H
#define SP1FX_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "../dsp/engine.h"
#include "../controls/controller.h"
#include "../storage/config_store.h"
#include "../app/sync_out.h"

#define PROTO_VERSION 1u
#define PROTO_MAX_PAYLOAD 500u
#define PROTO_MAX_BODY 512u
/* Worst-case COBS expansion of a max body: one code byte per 254 data bytes, plus one. */
#define PROTO_COBS_MAX(n) ((n) + (n) / 254u + 1u)
#define PROTO_MAX_FRAME (PROTO_COBS_MAX(PROTO_MAX_BODY) + 1u) /* + 0x00 delimiter */

/* message types */
enum {
	PT_HELLO = 0x01, PT_GET_FX = 0x02, PT_GET_STATE = 0x03,
	PT_SET_SLOT_FX = 0x10, PT_SET_PARAM = 0x11, PT_SET_WET = 0x12, PT_SET_BYPASS = 0x13,
	PT_SET_GLOBAL_BYPASS = 0x14, PT_SET_MASTER = 0x15, PT_SET_INPUT_GAIN = 0x16,
	PT_SET_ORDER = 0x17, PT_SET_BPM = 0x18, PT_SET_SYNC = 0x19,
	PT_SAVE_NOW = 0x20, PT_FACTORY_RESET = 0x21, PT_SUBSCRIBE = 0x30,
	PT_HELLO_RSP = 0x81, PT_FX_DESC = 0x82, PT_STATE = 0x83, PT_RESULT = 0x90,
	PT_EVENT = 0xA0, PT_ADMISSION_ERROR = 0xA1, PT_METERS = 0xA2, PT_PRESET_LOADED = 0xA3
};

/* RESULT codes */
enum {
	PR_OK = 0, PR_BAD_ARG = 1, PR_NO_MEMORY = 2, PR_NO_CPU = 3, PR_BUSY = 4, PR_STORAGE = 5
};

#define PROTO_FACTORY_RESET_CONFIRM 0xA5u
#define PROTO_EVENT_MIN_INTERVAL_MS 50u /* 20 events/s per fader-type control */

/* ---- primitives (exposed for tests) ---- */
uint16_t proto_crc16(const uint8_t *p, size_t n); /* CRC-16/CCITT-FALSE */
/* Returns encoded length (no delimiter), or 0 if `cap` is too small. Byte-identical to
 * companion/js/protocol.js cobsEncode(). */
size_t proto_cobs_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
/* Returns 0 and the decoded length in *out_len, or -1 on malformed input / overflow. */
int proto_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *out_len);

/* Build `COBS(ver type seq payload crc16le) 0x00` into out. Returns total bytes incl. the
 * delimiter, or 0 if the payload is too long or out is too small. */
size_t proto_build_frame(uint8_t type, uint8_t seq, const uint8_t *payload, size_t n,
                         uint8_t *out, size_t cap);

typedef struct {
	uint8_t type, seq;
	const uint8_t *payload; /* points into the caller's body buffer */
	uint16_t len;
} proto_frame_t;

/* Parse one encoded frame (without its 0x00). body must hold PROTO_MAX_BODY bytes.
 * Returns 0 on success, -1 on COBS/length/CRC/version error. */
int proto_parse_frame(const uint8_t *enc, size_t n, uint8_t *body, proto_frame_t *f);

/* ---- device instance ---- */
typedef void (*proto_tx_fn)(void *ctx, const uint8_t *bytes, size_t n);
typedef int (*proto_save_fn)(void *ctx); /* SAVE_NOW: 0 ok, nonzero = storage error */

typedef struct {
	engine_t *eng;           /* required */
	controller_t *ctl;       /* optional: soft-takeover resync, device events, ui_mode */
	sync_out_t *sync;        /* optional: SET_BPM */
	store_debounce_t *deb;   /* optional: marked dirty after every host-originated change */
	proto_tx_fn tx;          /* required */
	proto_save_fn save;      /* optional: SAVE_NOW (absent => code 5) */
	void *ctx;               /* passed to tx and save */
	const char *fw;          /* firmware version string for HELLO_RSP (<= 64 chars) */
	uint16_t block;          /* audio block size reported in HELLO_RSP */
	float bpm;               /* initial tempo (30..300) */
	uint8_t sync_on;         /* initial sync-clock state (restored from the saved config) */
} proto_config_t;

typedef struct {
	uint8_t pending;
	float value;
	uint32_t last_sent_ms;
	uint8_t ever_sent;
} proto_fader_ev_t;

typedef struct {
	proto_config_t c;
	float bpm;          /* current tempo; persist {eng->cfg, bpm, sync_on} as store_data_t */
	uint8_t sync_on;    /* sync-jack clock running */
	int dirty;          /* set by every host-originated change; caller may clear */
	uint32_t now_ms;    /* clock from the last proto_tick() */

	/* receiver */
	uint8_t rx[PROTO_MAX_FRAME];
	uint16_t rx_len;
	uint8_t rx_overflow;
	uint32_t rx_frames, rx_dropped;

	/* meters */
	uint8_t sub_hz;
	uint32_t next_meter_ms;

	/* device event rate limiting: wet[slot] and param[slot][idx] */
	proto_fader_ev_t ev_wet[ENGINE_SLOTS];
	proto_fader_ev_t ev_param[ENGINE_SLOTS][FX_MAX_PARAMS];

	/* scratch buffers (no stack-heavy work, no globals) */
	uint8_t body[PROTO_MAX_BODY];
	uint8_t txbuf[PROTO_MAX_FRAME];
	uint8_t pay[PROTO_MAX_PAYLOAD];
} proto_t;

void proto_init(proto_t *p, const proto_config_t *cfg);
/* Feed one received byte. Complete valid frames are executed immediately. */
void proto_rx_byte(proto_t *p, uint8_t b);
void proto_rx(proto_t *p, const uint8_t *bytes, size_t n);
/* Control-thread tick: drains device events from the controller (rate-limited) and emits
 * METERS at the subscribed rate. */
void proto_tick(proto_t *p, uint32_t now_ms);

#endif
