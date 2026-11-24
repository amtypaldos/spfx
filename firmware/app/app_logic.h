/* SP-1 FX — small decision logic of the integrated firmware (zephyr_sp1fx), kept in pure C so
 * it is host-tested (tests/test_app_logic.c). No I/O, no globals.
 *
 *  1. Save/erase scheduling: when a debounced settings save may run, and when a flash page
 *     erase (which stalls the nRF52840 CPU, USB and I2S servicing for up to ~85 ms) may run.
 *  2. Console mode: one CDC ACM port carries both the human-readable diagnostic lines and the
 *     binary companion protocol (docs/PROTOCOL.md). Text goes out only until the first valid
 *     protocol frame arrives; from then on only binary frames, until the host closes the port
 *     (DTR low).
 *  3. Audio quiet detector: "nothing audible is playing" for long enough that an erase stall
 *     cannot be heard.
 *  4. Line rate limiter for event lines on the text console. */
#ifndef SP1FX_APP_LOGIC_H
#define SP1FX_APP_LOGIC_H

#include <stdint.h>

/* ---- 1. save/erase scheduling ---------------------------------------------------------- */
typedef enum {
	SAVE_IDLE = 0,
	SAVE_WRITE,            /* append a record: no erase, chunked writes, safe at any time */
	SAVE_ERASE_SPARE,      /* pre-erase the spare page now (audio is quiet) */
	SAVE_WRITE_WITH_ERASE  /* the save itself needs an erase; audio is quiet */
} save_action_t;

#define SAVE_ERASE_RETRY_MS 10000u /* after a failed erase, wait before trying again */

typedef struct {
	int pending;               /* a save is due but was deferred (it needs an erase, not quiet) */
	uint32_t deferred;         /* ticks a due save waited for a quiet moment (diagnostics) */
	uint32_t last_erase_try_ms;
	int erase_tried;
} save_sched_t;

void save_sched_init(save_sched_t *s);
/* Call once per control tick.
 *   due          store_debounce_due() said "write now" on this tick
 *   needs_erase  store_save_needs_erase()
 *   spare_dirty  store_spare_needs_erase()
 *   quiet        the audio has been quiet long enough (quiet_ok())
 * The caller performs the returned action and reports an erase via save_sched_erased(). */
save_action_t save_sched_next(save_sched_t *s, uint32_t now_ms, int due, int needs_erase,
			      int spare_dirty, int quiet);
void save_sched_erased(save_sched_t *s, uint32_t now_ms);

/* ---- 1b. event-based saving (owner decision, rc4) -------------------------------------
 * A settings record is written after EVENTS, not after every tweak:
 *   - a preset switch, leaving edit mode, an effect change in a slot, and the companion's
 *     setting changes (effect, order, BPM, sync, output mode, factory reset) are events; a
 *     burst of them (rocker auto-repeat) is saved once, SAVE_EVENT_SETTLE_MS after the last;
 *   - tweaks (faders, VOL, bypass taps, the companion's wet/param/master/input gain) only mark
 *     the state dirty; they go out with the next event, at power off, or by the safety timer:
 *     dirty and nothing saved for SAVE_SAFETY_MS;
 *   - nothing is saved while the load is high (the caller says so), nothing at boot;
 *   - power off and the companion's SAVE_NOW save at once (not through this module).
 * Trade-off: tweaks made since the last save are lost if the battery dies or the unit resets
 * before the next event / power off / safety save. */
#define SAVE_EVENT_SETTLE_MS 1500u
#define SAVE_SAFETY_MS 120000u
typedef struct {
	uint8_t dirty, event;
	uint32_t event_ms, last_save_ms;
	uint32_t deferred_load;     /* due but held back by high load (ticks) */
} saveev_t;
void saveev_init(saveev_t *s, uint32_t now_ms);           /* boot: nothing dirty */
void saveev_tweak(saveev_t *s, uint32_t now_ms);
void saveev_event(saveev_t *s, uint32_t now_ms);          /* also marks dirty */
int saveev_due(saveev_t *s, uint32_t now_ms, int load_ok); /* 1: start a save now */
void saveev_saved(saveev_t *s, uint32_t now_ms);           /* a save completed (any path) */

/* ---- 2. console mode ---------------------------------------------------------------- */
typedef struct {
	uint8_t open;              /* host has the port open (DTR) */
	uint8_t binary;            /* companion detected: binary frames only */
	uint32_t mark;             /* protocol rx frame counter when the port was opened */
	uint32_t switches;         /* text -> binary transitions */
} con_mode_t;

void conmode_init(con_mode_t *m);
/* Update from the DTR line and the protocol's valid-frame counter. Returns 1 exactly when the
 * mode switches to binary: the caller then sends one 0x00 first, so a host-side COBS decoder
 * drops any half-received text line as one bad frame instead of merging it with the next
 * frame. */
int conmode_update(con_mode_t *m, int dtr, uint32_t rx_frames);
int conmode_text_ok(const con_mode_t *m);    /* diagnostic text may be sent */
int conmode_binary_ok(const con_mode_t *m);  /* protocol frames may be sent */

/* ---- 2b. console TX queue + mode, as one unit (what main.c uses) -----------------------
 * Byte ring with whole-message semantics: a text line or protocol frame is queued
 * completely or not at all (never half a frame), and the producer NEVER waits: when the
 * host does not read (port open but stalled, or no reader at all), messages are dropped and
 * counted. Single producer (control thread) / single consumer (CDC callback). The 0x00 mode
 * separator is guaranteed to precede the first binary frame: while it cannot be queued,
 * frames are dropped (and the separator retried) rather than sent without it. */
typedef struct {
	uint8_t *buf;
	uint32_t cap;
	volatile uint32_t head, tail;   /* free-running; head: producer, tail: consumer */
	uint32_t max_used;
} txq_t;

void txq_init(txq_t *q, uint8_t *buf, uint32_t cap);
uint32_t txq_used(const txq_t *q);
uint32_t txq_space(const txq_t *q);
int txq_put(txq_t *q, const uint8_t *p, uint32_t n);        /* 1 queued whole, 0 dropped */
uint32_t txq_peek(const txq_t *q, const uint8_t **p, uint32_t max); /* contiguous bytes */
void txq_consume(txq_t *q, uint32_t n);

typedef struct {
	con_mode_t m;
	txq_t q;
	uint8_t sep_pending;
	uint32_t text_drops, frame_drops, frames_sent, sep_sent;
} contx_t;

void contx_init(contx_t *c, uint8_t *buf, uint32_t cap);
/* Mode update from DTR and the protocol's valid-frame counter (see conmode_update). */
void contx_poll(contx_t *c, int dtr, uint32_t rx_frames);
/* 1 queued; 0 not allowed in this mode or dropped. Never blocks. */
int contx_text(contx_t *c, const uint8_t *s, uint32_t n);
int contx_frame(contx_t *c, const uint8_t *f, uint32_t n);

/* ---- 2c. FN hold to power on / off (owner decision: 4 s, 8-LED fill) -------------------
 * The fill: every 0.5 s of holding one more LED lights and stays lit: first the four
 * PLAY-side LEDs (left to right), then the four track LEDs T1..T4; the 8th lights at 4.0 s
 * together with the action. holdfill_count() gives how many are lit (0 before 0.5 s). */
#define HOLD_MS        4000u
#define HOLD_STEP_MS   500u
int holdfill_count(uint32_t held_ms);   /* 0..8 */

/* Boot decision (owner decisions for rc3; design follows chattock/sp1-tape-looper's
 * charge-standby, MIT, which ships on this hardware):
 *   STANDBY: the device stays "off" -- no USB, no audio -- until FUNCTION is held 4 s (8-LED
 *            fill); with USB power present it shows the charge state, without it goes back to
 *            System OFF at once. Used for: a wake from System OFF by FUNCTION (RESETREAS OFF),
 *            a wake by USB being plugged in (VBUS), and our own power off while plugged in
 *            (a soft reset with the retained power-off flag, because System OFF with VBUS
 *            already present has no wake edge).
 *   NOW:     start at once: watchdog / lockup recovery, a fatal-error reset (soft reset with a
 *            crash record), power-on reset / fresh battery / unknown (0), reset pin, any other
 *            soft reset. Crash recovery and the crash-loop breaker are never blocked. */
#define RR_RESETPIN  (1u << 0)
#define RR_DOG       (1u << 1)
#define RR_SREQ      (1u << 2)
#define RR_LOCKUP    (1u << 3)
#define RR_OFF       (1u << 16)
#define RR_LPCOMP    (1u << 17)
#define RR_DIF       (1u << 18)
#define RR_NFC       (1u << 19)
#define RR_VBUS      (1u << 20)
typedef enum { BOOT_NOW = 0, BOOT_STANDBY } boot_mode_t;
boot_mode_t boot_decide(uint32_t resetreas, int crash_pending, int poweroff_flag);
typedef enum { SB_WAIT = 0, SB_BOOT, SB_OFF } sb_action_t;
/* one standby step: FN (debounced) held for held_ms, USB power present? */
sb_action_t standby_step(int fn_down, int usb_power, uint32_t held_ms);

/* ---- 2d. SAADC scan state machine (zephyr_sp1fx drivers/saadc_scan.c) ------------------
 * Non-blocking and self-healing: a scan is started in one control tick and collected in a
 * later one; a scan that has not ended after SCAN_TIMEOUT_MS is aborted and restarted (never
 * latched, never waited for). The hardware layer executes the returned action. */
#define SCAN_TIMEOUT_MS 50u
typedef enum { SCAN_ACT_NONE = 0, SCAN_ACT_START, SCAN_ACT_COLLECT_START, SCAN_ACT_ABORT_START } scan_act_t;
typedef struct {
	uint8_t running;
	uint32_t t0;          /* start of the running scan */
	uint32_t ok, timeouts, starts;
	uint32_t last_ok_ms;
	uint8_t have_ok;
} scanfsm_t;
void scanfsm_init(scanfsm_t *f);
/* ended: the hardware reports the running scan complete (EVENTS_END) */
scan_act_t scanfsm_step(scanfsm_t *f, uint32_t now_ms, int ended);
/* ms since the last good scan (0xFFFFFFFF if none yet) */
uint32_t scanfsm_age(const scanfsm_t *f, uint32_t now_ms);

/* ---- 2e. battery fuel gauge (rc6) -----------------------------------------------------
 * AIN4 (P0.28) reads the battery through an on-board divider (SP-1-dev wiki "Battery
 * charger"). Conversion with the scanner's setup (gain 1/6, internal 0.6 V reference, 12-bit):
 * V_pin = raw * 3.6 V / 4096. BATT_DIVIDER_X1000 is the ONE assumed constant: 2.0, derived
 * from Tape Looper's anchor "raw ~2380 = ~4.21 V at rest" (2380 * 3.6 / 4096 = 2.092 V at the
 * pin -> ratio 2.01, i.e. a 1:1 divider). CALCULATED, UNCALIBRATED.
 * Percent: Li-ion resting-voltage table 3.30 V = 0 % .. 4.20 V = 100 % (nonlinear, linear in
 * between table points). While charging the terminal voltage reads high, so 100 mV is taken off
 * and the level is capped at 95 % until the charger reports done; running with audio the
 * voltage sags a little under load (not corrected; smoothed). Smoothing: exponential average
 * (~2 s), shown level moves only by >= 3 % (or to/from the 95/100 cap) and at most every 2 s. */
#define BATT_DIVIDER_X1000 2000
#define BATT_CHARGE_OFFSET_MV 100
#define BATT_CHARGING_CAP 95
#define BATT_HYST_PCT 3
#define BATT_UPDATE_MS 2000u
int batt_mv_from_raw(int raw);          /* <0 raw -> -1 */
int batt_pct_from_mv(int mv);           /* 0..100 */
typedef struct {
	float mv;          /* smoothed */
	int have;
	int shown;         /* displayed percent, -1 none yet */
	uint32_t last_upd;
} batt_t;
void batt_init(batt_t *b);
/* feed one raw reading (any rate; tau ~2 s at 8-10 ms per call) */
void batt_feed(batt_t *b, int raw, int charging, int charge_done, uint32_t now_ms);
int batt_shown(const batt_t *b);        /* 0..100, -1 unknown */
/* LED bar for the charge standby: n = 1..8 LEDs (PLAY-side 1-4 then T1-T4) for the level;
 * the top one pulses slowly while charging; all steady when done. Brightness 0..255. */
void batt_leds(int pct, int charging, uint32_t now_ms, uint8_t play[4], uint8_t track[4]);

/* ---- 3. audio quiet detector -------------------------------------------------------- */
#define QUIET_PEAK 0.0005f         /* -66 dBFS: below this a block counts as silent */
typedef struct {
	uint32_t silent_blocks;    /* consecutive silent blocks */
} quiet_t;
/* Feed once per audio block with the block's input and output peaks (linear, 1.0 = FS). */
void quiet_feed(quiet_t *q, float in_peak, float out_peak);
/* 1 if at least `blocks` consecutive blocks were silent. */
int quiet_ok(const quiet_t *q, uint32_t blocks);

/* ---- 4. line rate limiter (token bucket) -------------------------------------------- */
typedef struct {
	uint32_t tokens_x1000, burst, per_s, last_ms;
	uint32_t dropped;
} ratelimit_t;
void ratelimit_init(ratelimit_t *r, uint32_t per_s, uint32_t burst, uint32_t now_ms);
int ratelimit_take(ratelimit_t *r, uint32_t now_ms); /* 1 = may send a line now */

#endif
