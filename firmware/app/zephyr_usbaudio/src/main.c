/*
 * SP-1 FX USB audio bring-up: computer audio through the real FX engine.
 *
 * NOT TESTED ON HARDWARE. Expected behaviour (docs/BRINGUP_USBAUDIO.md):
 *   Image A (zephyr_usbaudio): composite USB device "SP-1 FX" = UAC2 speaker
 *     (host -> SP-1, 48 kHz 16-bit stereo, asynchronous, explicit feedback)
 *     + CDC ACM status console. Audio: USB ring -> int16->float ->
 *     engine_process() (64-frame blocks) -> fades/trim -> float->int16 ->
 *     I2S (slave, CS42L42 headphones or TAS2505 speaker, as the tone image).
 *   Image B (zephyr_usbaudio_io, CONFIG_SP1FX_USB_AUDIO_IN_STREAM): the same,
 *     plus the processed blocks returned to the host on a UAC2 IN stream;
 *     implicit feedback instead of a feedback endpoint (one ISO IN endpoint
 *     on the nRF52840; see ../zephyr_usbaudio_io/app.overlay).
 *   FUNCTION tap: next preset (Clean + 6 factory presets) with the
 *     controller's duck -> 25 ms -> engine_post_config -> unduck sequence.
 *     The config is STAGED: the audio thread takes it at the start of its
 *     next block and initialises changed effects there, at most
 *     ENGINE_INIT_BUDGET bytes of effect memory per block (dsp/engine.h).
 *   FUNCTION held 1.5 s: fade, mute, SYSTEM_OFF (as zephyr_min / tone).
 *   LED1 heartbeat, LED2 USB OUT stream playing, LED3 any underrun/overrun/
 *   dup/drop in the last second, LED4 preset index as a blink count
 *   (Clean = off, preset k = k blinks every 2 s).
 *   Console: once per second, raw DWT cycle counts of engine_process(),
 *     stack high-water marks, config-apply cost, main-loop / audio-block
 *     stall maxima; a "CRASH: ..." line every 10 s after a crash
 *     (drivers/crashdump.h, docs/BRINGUP_USBAUDIO.md "Known issue").
 *
 * HEARING SAFETY: codec analog gains as the tone image (CS42L42 mixer -30 dB,
 * TAS2505 +6 dB / -6 dB, sp1_codecs.c, shared unmodified), engine soft
 * limiter, then CONFIG_SP1FX_UA_TRIM_DB (default -12 dB) digital trim, a
 * CONFIG_SP1FX_UA_FADE_IN_MS (200 ms) fade-in whenever the stream starts, a
 * fade-out on stream stop and a decay-to-zero on underrun.
 *
 * Provenance: watchdog/power-off/codec bring-up/headphone switching are the
 * tone image's (firmware/app/zephyr_tone/src/main.c). The UAC2 callback
 * structure follows Zephyr's uac2_explicit/implicit_feedback samples
 * (Apache-2.0); the fill-based feedback law follows chattock/sp1-tape-looper
 * feedback_update() (MIT, 0359372e). See ../NOTICES.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/drivers/usb/udc_buf.h>
#include <sample_usbd.h>
#include <hal/nrf_gpio.h>
#include <nrfx.h>
#include <math.h>
#include <string.h>

#include "sp1_codecs.h"
#include "audio/usb_audio_core.h"
#include "dsp/engine.h"
#include "app/factory_presets.h"
#include "drivers/crashdump.h"

#define LOOP_MS          10
#define BLINK_HALF_MS    500
#define FN_HOLD_OFF_MS   1500
#define FN_TAP_MIN_MS    30
#define FN_RELEASE_MS    60
#define HP_POLL_MS       250
#define STATUS_MS        1000
#define PRESET_FADE_MS   25     /* = PRESET_FADE_MS of firmware/controls/controller.h */

/* ---------------------------- audio constants ---------------------------- */
#define FS_HZ            48000
#define BLK_FRAMES       64
#define BLK_BYTES        (BLK_FRAMES * 2 * (int)sizeof(int16_t))
#define SLAB_BLOCKS      8
#define PRIME_BLOCKS     4
#define BLOCKS_PER_S     750     /* 48000 / 64 */
#define CPU_HZ           64000000u
#define FRAME_BYTES      4u
#define USB_MAX_FRAMES   49u     /* 48 + 1 (asynchronous) per 1 ms frame */
#define USB_PKT_BYTES    (USB_MAX_FRAMES * FRAME_BYTES)
#define OUT_FADE_IN_S    0.2f    /* after a headphone/speaker switch */
#define OUT_FADE_OUT_S   0.03f
/* Audio thread stack: engine_process ~1.6 KB of block arrays + effect code + FP exception
 * frames; measured high-water is printed (stk audio=). */
#define AUDIO_STACK_SIZE 6144
/* Effect memory initialised per 64-frame block after a config change (the delay's 75 KB
 * still goes alone): spreads a full preset change over <= 3 blocks. */
#define ENGINE_INIT_BUDGET (48u * 1024u)
/* Starvation guard: this many blocks in a row with < STARVE_IDLE_CYC cycles of waiting
 * (the audio thread at priority 0 never blocked) -> sleep 1 ms so the main thread (and the
 * watchdog feed) still run; counted as starve=. */
#define STARVE_IDLE_CYC  (CPU_HZ / 20000u)   /* 50 us */
#define STARVE_RUN       32
/* Overload guard (dsp/engine.h engine_overload_feed): whole-block work above 85 % of the
 * 64-frame period (64 / 48 kHz = 85,333 cycles at 64 MHz), judged as dsp/engine.h describes
 * (since v1-rc1: mean over 32-block windows; the v3 image used 8 blocks in a row) -> the
 * engine switches off the most expensive slot (then global bypass). */
#define OVERLOAD_LIMIT_CYC ((uint32_t)((uint64_t)BLK_FRAMES * CPU_HZ / FS_HZ * 85u / 100u))

#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
#define UAC2_NODE        DT_NODELABEL(uac2_io)
#define OUT_TERMINAL_ID  UAC2_ENTITY_ID(DT_NODELABEL(io_usb_out))
#define IN_TERMINAL_ID   UAC2_ENTITY_ID(DT_NODELABEL(io_usb_in))
#define IMAGE_NAME       "B (usbaudio-io)"
#else
#define UAC2_NODE        DT_NODELABEL(uac2_speaker)
#define OUT_TERMINAL_ID  UAC2_ENTITY_ID(DT_NODELABEL(in_terminal))
#define IMAGE_NAME       "A (usbaudio-in)"
#endif

BUILD_ASSERT((CONFIG_SP1FX_UA_RING_FRAMES & (CONFIG_SP1FX_UA_RING_FRAMES - 1)) == 0,
	     "ring size must be a power of two");
BUILD_ASSERT(CONFIG_SP1FX_UA_LOW_FRAMES >= BLK_FRAMES &&
	     CONFIG_SP1FX_UA_LOW_FRAMES < CONFIG_SP1FX_UA_TARGET_FRAMES &&
	     CONFIG_SP1FX_UA_TARGET_FRAMES < CONFIG_SP1FX_UA_HIGH_FRAMES &&
	     CONFIG_SP1FX_UA_HIGH_FRAMES + 2 * USB_MAX_FRAMES <= CONFIG_SP1FX_UA_RING_FRAMES,
	     "ring limits");

K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLK_BYTES, SLAB_BLOCKS, 4);
/* USB OUT receive buffers (owned by the stack between get_recv_buf and
 * data_recv_cb; copied into the ring at once). */
K_MEM_SLAB_DEFINE_STATIC(usb_rx_slab, ROUND_UP(USB_PKT_BYTES, UDC_BUF_GRANULARITY), 4,
			 UDC_BUF_ALIGN);
#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
K_MEM_SLAB_DEFINE_STATIC(usb_tx_slab, ROUND_UP(USB_PKT_BYTES, UDC_BUF_GRANULARITY), 4,
			 UDC_BUF_ALIGN);
#endif

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static const struct device *const uac2_dev = DEVICE_DT_GET(UAC2_NODE);

#define ZU DT_PATH(zephyr_user)
static const struct gpio_dt_spec led_hb     = GPIO_DT_SPEC_GET(ZU, heartbeat_led_gpios);
static const struct gpio_dt_spec led_stream = GPIO_DT_SPEC_GET(ZU, stream_led_gpios);
static const struct gpio_dt_spec led_event  = GPIO_DT_SPEC_GET(ZU, event_led_gpios);
static const struct gpio_dt_spec led_preset = GPIO_DT_SPEC_GET(ZU, preset_led_gpios);
static const struct gpio_dt_spec fn         = GPIO_DT_SPEC_GET(ZU, fn_gpios);
static const struct gpio_dt_spec osc_en     = GPIO_DT_SPEC_GET(ZU, osc_en_gpios);
static const struct gpio_dt_spec cs42_rst   = GPIO_DT_SPEC_GET(ZU, cs42_rst_gpios);
static const struct gpio_dt_spec tas_rst    = GPIO_DT_SPEC_GET(ZU, tas_rst_gpios);

volatile uint32_t sp1fx_boot_resetreas;

/* --------------------------- engine (static pool) ------------------------ */
static uint8_t engine_pool[CONFIG_SP1FX_ENGINE_POOL_KIB * 1024] __aligned(8);
static engine_t eng;

#define N_PRESETS (1 + FACTORY_PRESET_COUNT)
static const char *preset_name(int i)
{
	return i == 0 ? "Clean" : factory_presets[i - 1].name;
}

/* ------------------------------ rings / state ---------------------------- */
static int16_t ring_store[CONFIG_SP1FX_UA_RING_FRAMES * 2];
static ua_ring_t out_ring;                 /* USB thread -> audio thread */
static ua_cons_t out_cons;                 /* audio thread */
static ua_fb_t fb;                         /* USB thread */
static volatile int32_t g_fb_value = 48 << UA_FB_FRAC;
static volatile bool g_out_stream;         /* host enabled the OUT terminal */
static volatile bool g_out_restart_req;    /* USB -> audio: flush, prebuffer */
static volatile uint32_t g_usb_overrun;    /* frames lost: ring full */
static volatile uint32_t g_usb_zero_pkts;  /* OUT packets with 0 bytes */
static volatile uint32_t g_usb_nobuf;      /* get_recv_buf had no buffer */
static volatile uint32_t g_usb_frames_in;  /* frames received (rate check) */
static volatile uint32_t g_sof_count;
static volatile uint32_t g_stream_starts;

#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
static int16_t ret_store[CONFIG_SP1FX_UA_RING_FRAMES * 2];
static ua_ring_t ret_ring;                 /* audio thread -> USB thread */
static ua_cons_t ret_cons;                 /* USB thread */
static uint32_t in_acc;                    /* USB thread: packet sizing */
static volatile bool g_in_stream;
static volatile uint32_t g_ret_overrun;    /* audio thread: ret ring full */
static volatile uint32_t g_in_send_fail;
static volatile uint32_t g_in_frames_out;
static volatile uint32_t g_in_silent_pkts;
#endif

/* --------------------- per-second snapshots (seqlock) -------------------- */
struct a_snap {
	uint32_t cyc_min, cyc_max, cyc_avg;       /* engine_process per 64-frame block */
	uint32_t blk_max;                         /* whole block work incl. conversions */
	uint32_t fill_min, fill_max, fill_avg;
	uint32_t nblk;
};
static struct a_snap a_pub;
static volatile uint32_t a_seq;

#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
struct r_snap {
	uint32_t fill_min, fill_max, fill_avg, n;
};
static struct r_snap r_pub;
static volatile uint32_t r_seq;
#endif

#define SNAP_PUBLISH(seq, dst, src) do { (seq)++; __DMB(); (dst) = (src); __DMB(); (seq)++; } while (0)

static bool snap_read(volatile uint32_t *seq, const void *src, void *dst, size_t n)
{
	for (int i = 0; i < 4; i++) {
		uint32_t s1 = *seq;

		__DMB();
		if (s1 & 1u) {
			k_yield();
			continue;
		}
		memcpy(dst, src, n);
		__DMB();
		if (*seq == s1) {
			return true;
		}
	}
	return false;
}

/* ------------------------ audio thread shared state ---------------------- */
static volatile bool     g_out_enable;     /* main -> audio: output ramp target */
static volatile bool     g_audio_stop;
static volatile bool     g_gain_zero = true;
static volatile int      g_i2s_cfg_rc = 1;
static volatile int      g_i2s_start_rc = 1;
static volatile uint32_t g_blocks, g_err, g_tmo, g_recover;
static volatile uint32_t g_out_sat;        /* samples saturated by float->int16 */
static volatile bool     g_playing;        /* consumer in RUN state */
/* since-boot maxima (audio thread writes, main reads; single words) */
static volatile uint32_t g_apply_max, g_apply_last; /* engine cycles in blocks that applied a config */
static volatile uint32_t g_apply_blocks;            /* blocks that applied a config / ran inits */
static volatile uint32_t g_blk_max_all;             /* whole-block work, cycles */
static volatile uint32_t g_starve;                  /* starvation-guard sleeps */
static volatile uint32_t g_i2s_recover_sleep;

static float in_l[BLK_FRAMES], in_r[BLK_FRAMES];
static float out_l[BLK_FRAMES], out_r[BLK_FRAMES];
static int16_t usb_blk[BLK_FRAMES * 2];
static ua_ramp_t in_ramp, out_ramp;
static float trim_gain;

static void prime_and_start(void)
{
	for (int i = 0; i < PRIME_BLOCKS; i++) {
		void *pb;

		if (k_mem_slab_alloc(&tx_slab, &pb, K_NO_WAIT) != 0) {
			break;
		}
		memset(pb, 0, BLK_BYTES);
		if (i2s_write(i2s_dev, pb, BLK_BYTES) != 0) {
			k_mem_slab_free(&tx_slab, pb);
		}
	}
	g_i2s_start_rc = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
}

/* One 64-frame block: USB ring -> engine -> int16 (into the I2S block). */
static void render_block(int16_t *dst, uint32_t *eng_cycles)
{
	static float last_l, last_r;
	bool decay = false;

	if (g_out_restart_req) {
		/* host re-enabled the stream: flush stale data, prebuffer again;
		 * if we were playing, this block decays instead of cutting to 0 */
		g_out_restart_req = false;
		decay = g_playing;
		ua_cons_restart(&out_cons);
	}
	const int f = ua_cons_read(&out_cons, usb_blk, BLK_FRAMES);

	if (f & UA_BLK_AUDIO) {
		ua_s16_to_f32(usb_blk, in_l, in_r, BLK_FRAMES);
		if (f & UA_BLK_STARTED) {
			in_ramp.g = 0.0f;            /* fade in from silence */
		}
		g_playing = true;
	} else if ((f & UA_BLK_UNDERRUN) || decay) {
		/* ran dry: decay the last input sample to 0, restart at 0 */
		ua_decay(last_l * in_ramp.g, last_r * in_ramp.g, in_l, in_r, BLK_FRAMES);
		in_ramp.g = 0.0f;
		g_playing = false;
	} else {
		for (int i = 0; i < BLK_FRAMES; i++) {   /* not memset: byte-wise in this libc */
			in_l[i] = 0.0f;
			in_r[i] = 0.0f;
		}
		g_playing = false;
	}
	if (f & UA_BLK_AUDIO) {
		last_l = in_l[BLK_FRAMES - 1];
		last_r = in_r[BLK_FRAMES - 1];
		/* stream stopped by the host: fade out on what is left in the ring */
		(void)ua_ramp_apply(&in_ramp, g_out_stream ? 1.0f : 0.0f, in_l, in_r, BLK_FRAMES);
	}

	const uint32_t t0 = DWT->CYCCNT;

	engine_process(&eng, in_l, in_r, out_l, out_r, BLK_FRAMES);
	*eng_cycles = DWT->CYCCNT - t0;

	const float g = ua_ramp_apply(&out_ramp, g_out_enable ? 1.0f : 0.0f, out_l, out_r,
				      BLK_FRAMES);

	g_gain_zero = (g <= 0.0f);
	g_out_sat += ua_f32_to_s16(out_l, out_r, dst, BLK_FRAMES, trim_gain);

#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
	if (g_in_stream) {
		/* the same processed samples that go to I2S */
		g_ret_overrun += BLK_FRAMES - ua_ring_write(&ret_ring, dst, BLK_FRAMES);
	}
#endif
}

static K_THREAD_STACK_DEFINE(audio_stack, AUDIO_STACK_SIZE);
static struct k_thread audio_tcb;

static void audio_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	struct i2s_config cfg = {
		.word_size      = 16,
		.channels       = 2,
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE,
		.frame_clk_freq = FS_HZ,
		.mem_slab       = &tx_slab,
		.block_size     = BLK_BYTES,
		.timeout        = 100,
	};

	if (!device_is_ready(i2s_dev)) {
		g_i2s_cfg_rc = -100;
		return;
	}
	g_i2s_cfg_rc = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
	if (g_i2s_cfg_rc != 0) {
		return;
	}
	prime_and_start();

	/* per-second window (this thread only) */
	uint32_t w_min = UINT32_MAX, w_max = 0, w_blk_max = 0, w_n = 0;
	uint64_t w_sum = 0;

	uint32_t busy_run = 0;
	uint32_t t_iter = DWT->CYCCNT, bc = 0;

	for (;;) {
		void *blk;

		/* starvation guard: did the previous iteration ever wait (for a free slab or
		 * room in the I2S queue)? If not, for STARVE_RUN blocks in a row, we are
		 * overloaded at priority 0 and would starve main + the watchdog feed. */
		{
			const uint32_t now = DWT->CYCCNT;
			const uint32_t idle = (now - t_iter) - bc;

			t_iter = now;
			bc = 0;
			busy_run = (idle < STARVE_IDLE_CYC) ? busy_run + 1u : 0u;
			if (busy_run >= STARVE_RUN) {
				busy_run = 0;
				g_starve++;
				k_msleep(1);
				t_iter = DWT->CYCCNT;
			}
		}
		if (g_audio_stop) {
			k_sleep(K_FOREVER);
		}
		if (k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(100)) != 0) {
			g_tmo++;
			continue;
		}

		const uint32_t tb = DWT->CYCCNT;
		const uint32_t applies0 = eng.diag.cfg_applies, inits0 = eng.diag.slot_inits;
		uint32_t ec;

		render_block(blk, &ec);

		bc = DWT->CYCCNT - tb;
		(void)engine_overload_feed(&eng, bc, OVERLOAD_LIMIT_CYC);

		if (eng.diag.cfg_applies != applies0 || eng.diag.slot_inits != inits0) {
			g_apply_blocks++;
			g_apply_last = ec;
			if (ec > g_apply_max) {
				g_apply_max = ec;
			}
		}
		if (bc > g_blk_max_all) {
			g_blk_max_all = bc;
		}
		eng.diag.cpu_last_cycles = ec;
		if (ec > eng.diag.cpu_peak_cycles) {
			eng.diag.cpu_peak_cycles = ec;
		}
		if (ec < w_min) {
			w_min = ec;
		}
		if (ec > w_max) {
			w_max = ec;
		}
		if (bc > w_blk_max) {
			w_blk_max = bc;
		}
		w_sum += ec;
		if (++w_n >= BLOCKS_PER_S) {
			struct a_snap s = {
				.cyc_min = w_min, .cyc_max = w_max,
				.cyc_avg = (uint32_t)(w_sum / w_n), .blk_max = w_blk_max,
				.fill_min = out_cons.fill_n ? out_cons.fill_min : 0,
				.fill_max = out_cons.fill_max,
				.fill_avg = out_cons.fill_n ?
					(uint32_t)(out_cons.fill_sum / out_cons.fill_n) : 0,
				.nblk = w_n,
			};
			SNAP_PUBLISH(a_seq, a_pub, s);
			ua_cons_stats_window_reset(&out_cons);
			w_min = UINT32_MAX;
			w_max = w_blk_max = w_n = 0;
			w_sum = 0;
		}

		const int rc = i2s_write(i2s_dev, blk, BLK_BYTES);

		if (rc == 0) {
			g_blocks++;
			continue;
		}
		k_mem_slab_free(&tx_slab, blk);
		if (rc == -EAGAIN) {
			g_tmo++;
			continue;
		}
		g_err++;
		if (g_audio_stop) {
			continue;
		}
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		prime_and_start();
		g_recover++;
		/* never retry at priority 0 without giving the main thread a chance */
		g_i2s_recover_sleep++;
		k_msleep(1);
		t_iter = DWT->CYCCNT;
		bc = 0;
	}
}

/* ------------------------------ UAC2 callbacks --------------------------- */
/* All of these run in the USB stack's cooperative thread (prio COOP(8)),
 * which pre-empts the audio thread. They never block. */

static void uac2_terminal_update_cb(const struct device *dev, uint8_t terminal,
				    bool enabled, bool microframes, void *user_data)
{
	ARG_UNUSED(dev); ARG_UNUSED(microframes); ARG_UNUSED(user_data);

	if (terminal == OUT_TERMINAL_ID) {
		if (enabled && !g_out_stream) {
			ua_fb_reset(&fb);
			g_fb_value = fb.value;
			g_out_restart_req = true;
			g_stream_starts++;
		}
		g_out_stream = enabled;
	}
#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
	if (terminal == IN_TERMINAL_ID) {
		if (enabled && !g_in_stream) {
			ua_cons_restart(&ret_cons);   /* this thread is its consumer */
			in_acc = 0;
		}
		g_in_stream = enabled;
	}
#endif
}

static void *uac2_get_recv_buf(const struct device *dev, uint8_t terminal,
			       uint16_t size, void *user_data)
{
	ARG_UNUSED(dev); ARG_UNUSED(user_data);
	void *buf = NULL;

	if (terminal != OUT_TERMINAL_ID || size > USB_PKT_BYTES) {
		return NULL;
	}
	if (k_mem_slab_alloc(&usb_rx_slab, &buf, K_NO_WAIT) != 0) {
		g_usb_nobuf++;
		return NULL;
	}
	return buf;
}

static void uac2_data_recv_cb(const struct device *dev, uint8_t terminal,
			      void *buf, uint16_t size, void *user_data)
{
	ARG_UNUSED(dev); ARG_UNUSED(terminal); ARG_UNUSED(user_data);
	const uint32_t n = size / FRAME_BYTES;

	if (n == 0u) {
		g_usb_zero_pkts++;
	} else if (g_out_stream) {
		g_usb_frames_in += n;
		g_usb_overrun += n - ua_ring_write(&out_ring, buf, n);
	}
	k_mem_slab_free(&usb_rx_slab, buf);
}

static void uac2_buf_release_cb(const struct device *dev, uint8_t terminal,
				void *buf, void *user_data)
{
	ARG_UNUSED(dev); ARG_UNUSED(terminal); ARG_UNUSED(user_data);
#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
	k_mem_slab_free(&usb_tx_slab, buf);
#else
	ARG_UNUSED(buf);
#endif
}

static uint32_t uac2_feedback_cb(const struct device *dev, uint8_t terminal,
				 void *user_data)
{
	ARG_UNUSED(dev); ARG_UNUSED(terminal); ARG_UNUSED(user_data);
	return (uint32_t)g_fb_value;   /* Q10.14 (Full-Speed, 3 bytes on the wire) */
}

#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
static void send_in_packet(void)
{
	static uint32_t r_n;
	void *buf;
	/* Implicit feedback: the IN packet size carries the device rate. While
	 * the OUT stream is closed the rate is nominal (the return ring's
	 * drop/dup safeguard then absorbs the clock difference). */
	const int32_t rate = g_out_stream ? g_fb_value : (48 << UA_FB_FRAC);
	uint32_t n = ua_pkt_frames(&in_acc, rate);

	if (n > USB_MAX_FRAMES) {
		n = USB_MAX_FRAMES;
	}
	if (k_mem_slab_alloc(&usb_tx_slab, &buf, K_NO_WAIT) != 0) {
		g_in_send_fail++;
		return;
	}
	int f = ua_cons_read(&ret_cons, buf, n);

	if (!(f & UA_BLK_AUDIO)) {
		memset(buf, 0, n * FRAME_BYTES);    /* prebuffering / underrun */
		g_in_silent_pkts++;
	}
	if (++r_n >= 1000u) {
		struct r_snap s = {
			.fill_min = ret_cons.fill_min, .fill_max = ret_cons.fill_max,
			.fill_avg = (uint32_t)(ret_cons.fill_sum / ret_cons.fill_n),
			.n = ret_cons.fill_n,
		};
		SNAP_PUBLISH(r_seq, r_pub, s);
		ua_cons_stats_window_reset(&ret_cons);
		r_n = 0;
	}
	if (usbd_uac2_send(uac2_dev, IN_TERMINAL_ID, buf, (uint16_t)(n * FRAME_BYTES)) != 0) {
		k_mem_slab_free(&usb_tx_slab, buf);
		g_in_send_fail++;
	} else {
		g_in_frames_out += n;
	}
}
#endif

static void uac2_sof_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev); ARG_UNUSED(user_data);

	g_sof_count++;
	if (g_out_stream) {
		g_fb_value = ua_fb_update(&fb, ua_ring_fill(&out_ring));
	}
#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
	if (g_in_stream) {
		send_in_packet();
	}
#endif
}

static const struct uac2_ops uac2_ops = {
	.sof_cb = uac2_sof_cb,
	.terminal_update_cb = uac2_terminal_update_cb,
	.get_recv_buf = uac2_get_recv_buf,
	.data_recv_cb = uac2_data_recv_cb,
	.buf_release_cb = uac2_buf_release_cb,
	.feedback_cb = uac2_feedback_cb,
};

/* ------------------------------ USB + console ---------------------------- */
static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
RING_BUF_DECLARE(con_rb, 2048);
static struct k_spinlock con_lock;
static bool usb_ok;
static int usb_rc = 1;
static uint32_t con_drops;

static void cdc_cb(const struct device *dev, void *user)
{
	ARG_UNUSED(user);

	if (!uart_irq_update(dev) || !uart_irq_tx_ready(dev)) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&con_lock);
	uint8_t *p;
	uint32_t n = ring_buf_get_claim(&con_rb, &p, 64);
	int w = (n > 0u) ? uart_fifo_fill(dev, p, (int)n) : 0;

	if (w < 0) {
		w = 0;
	}
	(void)ring_buf_get_finish(&con_rb, (uint32_t)w);
	bool empty = ring_buf_is_empty(&con_rb);

	k_spin_unlock(&con_lock, key);
	if (empty) {
		uart_irq_tx_disable(dev);
	}
}

static void usb_start(void)
{
	struct usbd_context *ctx;

	if (!device_is_ready(cdc) || !device_is_ready(uac2_dev)) {
		usb_rc = -100;
		return;
	}
	usbd_uac2_set_ops(uac2_dev, &uac2_ops, NULL);
	ctx = sample_usbd_init_device(NULL);
	if (ctx == NULL) {
		usb_rc = -101;
		return;
	}
	usb_rc = usbd_enable(ctx);
	if (usb_rc != 0) {
		return;
	}
	uart_irq_callback_user_data_set(cdc, cdc_cb, NULL);
	usb_ok = true;
}

static bool con_host_open(void)
{
	uint32_t dtr = 0;

	if (!usb_ok) {
		return false;
	}
	(void)uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
	return dtr != 0u;
}

static void con_write(const char *s, int len)
{
	if (len <= 0 || !con_host_open()) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&con_lock);
	bool fits = ring_buf_space_get(&con_rb) >= (uint32_t)len;

	if (fits) {
		(void)ring_buf_put(&con_rb, (const uint8_t *)s, (uint32_t)len);
	}
	k_spin_unlock(&con_lock, key);
	if (fits) {
		uart_irq_tx_enable(cdc);
	} else {
		con_drops++;
	}
}

#define CON_PRINTF(...) do {						\
		char _l[320];						\
		int _n = snprintk(_l, sizeof(_l), __VA_ARGS__);		\
		if (_n > 0) {						\
			con_write(_l, MIN(_n, (int)sizeof(_l) - 1));	\
		}							\
	} while (0)

/* ------------------------- watchdog / LEDs / power ----------------------- */
static void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++) {
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
	}
}

/* GUESS (untested), as zephyr_min/tone: release a bootloader PWM claim. */
static void release_pwm_claim(uint32_t abs_pin)
{
	NRF_PWM_Type *const pwms[] = { NRF_PWM0, NRF_PWM1, NRF_PWM2, NRF_PWM3 };

	for (size_t i = 0; i < ARRAY_SIZE(pwms); i++) {
		NRF_PWM_Type *p = pwms[i];
		bool routed = false;

		if (p->ENABLE == 0u) {
			continue;
		}
		for (int c = 0; c < 4; c++) {
			uint32_t psel = p->PSEL.OUT[c];
			bool connected = (psel & PWM_PSEL_OUT_CONNECT_Msk) == 0u;
			uint32_t pin = psel & (PWM_PSEL_OUT_PIN_Msk | PWM_PSEL_OUT_PORT_Msk);

			if (connected && pin == abs_pin) {
				routed = true;
			}
		}
		if (!routed) {
			continue;
		}
		p->EVENTS_STOPPED = 0u;
		p->TASKS_STOP = 1u;
		for (int t = 0; t < 1000 && p->EVENTS_STOPPED == 0u; t++) {
			k_busy_wait(1);
		}
		p->ENABLE = 0u;
	}
}

static uint32_t abs_pin(const struct gpio_dt_spec *g)
{
	const uint32_t port = (g->port == DEVICE_DT_GET(DT_NODELABEL(gpio1))) ? 1u : 0u;

	return NRF_GPIO_PIN_MAP(port, g->pin);
}

static bool fn_pressed(void)
{
	return gpio_pin_get_dt(&fn) > 0;
}

static void wait_fed(int ms)
{
	while (ms > 0) {
		feed_wdt();
		k_msleep(ms > 20 ? 20 : ms);
		ms -= 20;
	}
	feed_wdt();
}

static void wait_silent(void)
{
	for (int t = 0; t < 10 && !g_gain_zero; t++) {
		wait_fed(10);
	}
	wait_fed(15);
}

static bool cs42_ok, tas_ok, cs42_cfg_ok, tas_cfg_ok;

static FUNC_NORETURN void power_off(void)
{
	g_out_enable = false;
	wait_silent();
	if (tas_ok || cs42_ok) {
		sp1_codecs_shutdown();
	}
	g_audio_stop = true;
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	(void)gpio_pin_set_dt(&tas_rst, 1);
	(void)gpio_pin_set_dt(&cs42_rst, 1);
	(void)gpio_pin_set_dt(&osc_en, 0);

	(void)gpio_pin_set_dt(&led_hb, 0);
	(void)gpio_pin_set_dt(&led_stream, 0);
	(void)gpio_pin_set_dt(&led_event, 0);
	(void)gpio_pin_set_dt(&led_preset, 0);

	while (fn_pressed()) {
		feed_wdt();
		k_msleep(20);
	}
	feed_wdt();
	k_msleep(FN_RELEASE_MS);

	nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, fn.pin), NRF_GPIO_PIN_PULLUP,
				 NRF_GPIO_PIN_SENSE_LOW);

	(void)irq_lock();
	feed_wdt();
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) {
	}
}

/* ------------------------------ output select ---------------------------- */
enum out { OUT_NONE, OUT_SPK, OUT_HP };
static const char *const out_name[] = { "none", "spk", "hp" };
static enum out cur_out = OUT_NONE;
static int hp_in = -1;
static int hp_raw = -1;

static enum out wanted_out(void)
{
	if (hp_in == 1 && cs42_cfg_ok) {
		return OUT_HP;
	}
	if (tas_cfg_ok) {
		return OUT_SPK;
	}
	return OUT_NONE;
}

static void apply_out(enum out o)
{
	if (cur_out == OUT_SPK || o != OUT_SPK) {
		if (tas_cfg_ok) {
			(void)tas_spk_enable(false);
		}
	}
	if (cur_out == OUT_HP || o != OUT_HP) {
		if (cs42_cfg_ok) {
			(void)cs42_hp_enable(false);
		}
	}
	if (o == OUT_SPK) {
		(void)tas_spk_enable(true);
	} else if (o == OUT_HP) {
		(void)cs42_hp_enable(true);
	}
	cur_out = o;
	g_out_enable = (o != OUT_NONE);
}

static void dwt_enable(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0u;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ---------------------------------- presets ------------------------------ */
static store_data_t preset_buf;   /* control thread only */
static int preset_idx;
static int preset_rejected;

static int apply_preset(int idx)
{
	if (idx == 0) {
		store_data_default(&preset_buf);     /* all slots bypassed: passthrough */
	} else {
		factory_preset_fill(&factory_presets[idx - 1], &preset_buf);
	}
	/* staged: the audio thread applies it at its next block (dsp/engine.h) */
	return engine_post_config(&eng, &preset_buf.cfg);
}

/* x in [0,1] -> thousandths, for printing without float printf */
static unsigned int milli(float x)
{
	if (!(x >= 0.0f)) {
		return 0u;
	}
	if (x > 1000.0f) {
		x = 1000.0f;
	}
	return (unsigned int)(x * 1000.0f + 0.5f);
}

/* cycles per 64-frame block -> per frame x10 and % of 1333.33 x10 */
static void cyc_fmt(uint32_t blk, unsigned int *per_frame_x10, unsigned int *pct_x10)
{
	*per_frame_x10 = (unsigned int)((blk * 10u + 32u) / BLK_FRAMES);
	/* % = blk/64 / (64e6/48e3) * 100 = blk * 48e3 * 100 / (64 * 64e6) */
	*pct_x10 = (unsigned int)(((uint64_t)blk * FS_HZ * 1000u + (uint64_t)BLK_FRAMES * CPU_HZ / 2u) /
				  ((uint64_t)BLK_FRAMES * CPU_HZ));
}

/* Watchdog TIMEOUT interrupt: ~61 us before the reset, record who was running. */
static void wdt_timeout_cb(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	crashdump_wdt_capture();
}

/* stack bytes used (high-water) of a thread; 0 if unknown */
static unsigned int stack_used(k_tid_t t, unsigned int *size)
{
	size_t unused = 0;

	*size = (unsigned int)t->stack_info.size;
	if (k_thread_stack_space_get(t, &unused) != 0) {
		return 0u;
	}
	return *size - (unsigned int)unused;
}

int main(void)
{
	sp1fx_boot_resetreas = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	feed_wdt();
	(void)crashdump_init();

	if (device_is_ready(wdt)) {
		struct wdt_timeout_cfg cfg = {
			.window.min = 0U,
			.window.max = 4000U,
			.callback = wdt_timeout_cb,
			.flags = WDT_FLAG_RESET_SOC,
		};
		(void)wdt_install_timeout(wdt, &cfg);
		(void)wdt_setup(wdt, 0);
	}
	feed_wdt();

	const struct gpio_dt_spec *leds[] = { &led_hb, &led_stream, &led_event, &led_preset };

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		release_pwm_claim(abs_pin(leds[i]));
		(void)gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
	}
	(void)gpio_pin_configure_dt(&fn, GPIO_INPUT);

	(void)gpio_pin_configure_dt(&cs42_rst, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&tas_rst, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&osc_en, GPIO_OUTPUT_INACTIVE);
	dwt_enable();

	/* audio core + engine before USB can deliver anything */
	(void)ua_ring_init(&out_ring, ring_store, CONFIG_SP1FX_UA_RING_FRAMES);
	ua_cons_init(&out_cons, &out_ring, CONFIG_SP1FX_UA_TARGET_FRAMES,
		     CONFIG_SP1FX_UA_LOW_FRAMES, CONFIG_SP1FX_UA_HIGH_FRAMES);
	ua_fb_init(&fb, 48 << UA_FB_FRAC, CONFIG_SP1FX_UA_TARGET_FRAMES, CONFIG_SP1FX_UA_FB_KP,
		   1 << UA_FB_FRAC, CONFIG_SP1FX_UA_FB_EMA_SHIFT);
#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
	(void)ua_ring_init(&ret_ring, ret_store, CONFIG_SP1FX_UA_RING_FRAMES);
	ua_cons_init(&ret_cons, &ret_ring, CONFIG_SP1FX_UA_RET_TARGET_FRAMES,
		     CONFIG_SP1FX_UA_RET_LOW_FRAMES, CONFIG_SP1FX_UA_RET_HIGH_FRAMES);
#endif
	ua_ramp_init(&in_ramp, (float)FS_HZ, CONFIG_SP1FX_UA_FADE_IN_MS / 1000.0f,
		     CONFIG_SP1FX_UA_FADE_OUT_MS / 1000.0f);
	ua_ramp_init(&out_ramp, (float)FS_HZ, OUT_FADE_IN_S, OUT_FADE_OUT_S);
	trim_gain = powf(10.0f, (float)CONFIG_SP1FX_UA_TRIM_DB / 20.0f);
	engine_init(&eng, engine_pool, sizeof(engine_pool), (float)FS_HZ,
		    ENGINE_DEFAULT_CPU_BUDGET);
	eng.init_budget = ENGINE_INIT_BUDGET;   /* before the audio thread exists */
	preset_rejected = apply_preset(0);

	usb_start();
	feed_wdt();

	k_msleep(2);
	(void)gpio_pin_set_dt(&osc_en, 1);
	k_msleep(2);
	(void)gpio_pin_set_dt(&cs42_rst, 0);
	(void)gpio_pin_set_dt(&tas_rst, 0);
	wait_fed(10);

	uint8_t devid[3] = { 0 };

	if (sp1_i2c_ready()) {
		cs42_ok = cs42_probe(devid);
		tas_ok = tas_probe();
	}

	k_thread_create(&audio_tcb, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack),
			audio_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(0), K_FP_REGS,
			K_NO_WAIT);
	k_thread_name_set(&audio_tcb, "audio");
	wait_fed(5);

	if (tas_ok) {
		tas_cfg_ok = tas_configure();
	}
	feed_wdt();
	if (cs42_ok) {
		cs42_cfg_ok = cs42_configure();
	}
	feed_wdt();

	for (int t = 0; t < 50 && g_blocks < 100u; t++) {
		wait_fed(10);
	}

	if (cs42_cfg_ok) {
		int votes = 0, reads = 0;

		for (int i = 0; i < 3; i++) {
			int c = cs42_hp_present();

			if (c >= 0) {
				reads++;
				votes += c;
			}
			hp_raw = c;
			wait_fed(8);
		}
		hp_in = (reads > 0) ? ((votes * 2 > reads) ? 1 : 0) : -1;
	}
	apply_out(wanted_out());

	bool fn_armed = !fn_pressed();
	int64_t fn_down_at = -1;
	int64_t now = k_uptime_get();
	int64_t next_toggle = now + BLINK_HALF_MS;
	int64_t next_hp = now + HP_POLL_MS;
	int64_t next_status = now + STATUS_MS;
	int64_t preset_apply_at = -1;
	int preset_pending = -1;
	bool led_on = false;
	int hp_cand = -1, hp_cnt = 0;
	uint32_t prev_blocks = g_blocks, prev_events = 0;
	uint32_t prev_frames_in = 0, prev_sof = 0;
#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
	uint32_t prev_frames_out = 0;
#endif
	bool event_led = false;
	int64_t loop_prev = k_uptime_get();
	uint32_t loop_gap_max = 0, loop_gap_win = 0;    /* ms between main-loop iterations */
	int64_t preset_posted_at = -1;
	uint32_t settle_ms_last = 0;
	uint8_t ovl_mask_seen = 0, ovl_forced_seen = 0;

	CON_PRINTF("SP-1 FX usbaudio image %s v3 (optimised DSP + overload guard): pool=%u B trim=%d dB fade_in=%d ms "
		   "ring=%d target=%d low=%d high=%d kp=%d\r\n", IMAGE_NAME,
		   (unsigned int)sizeof(engine_pool), CONFIG_SP1FX_UA_TRIM_DB,
		   CONFIG_SP1FX_UA_FADE_IN_MS, CONFIG_SP1FX_UA_RING_FRAMES,
		   CONFIG_SP1FX_UA_TARGET_FRAMES, CONFIG_SP1FX_UA_LOW_FRAMES,
		   CONFIG_SP1FX_UA_HIGH_FRAMES, CONFIG_SP1FX_UA_FB_KP);

	for (;;) {
		feed_wdt();
		now = k_uptime_get();
		{
			/* watchdog near-miss indicator: the feed above is the only one */
			const uint32_t gap = (uint32_t)(now - loop_prev);

			loop_prev = now;
			if (gap > loop_gap_max) {
				loop_gap_max = gap;
			}
			if (gap > loop_gap_win) {
				loop_gap_win = gap;
			}
		}
		crashdump_poll(now, con_host_open(), con_write);
		if (preset_posted_at >= 0 && engine_config_settled(&eng)) {
			settle_ms_last = (uint32_t)(now - preset_posted_at);
			preset_posted_at = -1;
		}

		if (now >= next_toggle) {
			led_on = !led_on;
			(void)gpio_pin_set_dt(&led_hb, led_on ? 1 : 0);
			next_toggle += BLINK_HALF_MS;
		}

		/* LED4: preset index as a blink count every 2 s (Clean = off) */
		{
			const int ph = (int)(now % 2000);
			const bool on = preset_idx > 0 && ph < preset_idx * 250 && (ph % 250) < 120;

			(void)gpio_pin_set_dt(&led_preset, on ? 1 : 0);
		}
		(void)gpio_pin_set_dt(&led_stream, (g_out_stream && g_playing) ? 1 : 0);

		/* FUNCTION: tap = next preset, hold 1.5 s = power off */
		if (fn_pressed()) {
			if (fn_armed) {
				if (fn_down_at < 0) {
					fn_down_at = now;
				} else if (now - fn_down_at >= FN_HOLD_OFF_MS) {
					power_off();
				}
			}
		} else {
			if (fn_armed && fn_down_at >= 0 && now - fn_down_at >= FN_TAP_MIN_MS) {
				int base = preset_pending >= 0 ? preset_pending : preset_idx;

				preset_pending = (base + 1) % N_PRESETS;
				preset_apply_at = now + PRESET_FADE_MS;
				engine_set_duck(&eng, 1);
			}
			fn_armed = true;
			fn_down_at = -1;
		}
		if (preset_pending >= 0 && now >= preset_apply_at) {
			preset_idx = preset_pending;
			preset_pending = -1;
			engine_overload_clear(&eng);   /* a new preset gets a fresh chance */
			preset_rejected = apply_preset(preset_idx);
			preset_posted_at = now;
			engine_set_duck(&eng, 0);
			eng.diag.cpu_peak_cycles = 0;
			uint32_t mem = 0, cpu = 0;

			engine_resources(&eng, &mem, &cpu);
			CON_PRINTF("preset %d:%s rejected=%d mem=%u cpu_decl=%u\r\n", preset_idx,
				   preset_name(preset_idx), preset_rejected, (unsigned int)mem,
				   (unsigned int)cpu);
		}

		if (now >= next_hp) {
			next_hp += HP_POLL_MS;
			if (cs42_cfg_ok) {
				int c = cs42_hp_present();

				hp_raw = c;
				if (c >= 0) {
					if (c == hp_cand) {
						hp_cnt++;
					} else {
						hp_cand = c;
						hp_cnt = 1;
					}
					if (hp_cnt >= 2) {
						hp_in = c;
					}
				}
			}
			enum out w = wanted_out();

			if (w != cur_out) {
				g_out_enable = false;
				wait_silent();
				apply_out(w);
			}
		}

		{
			/* overload guard actions (audio thread) -> one console line per change */
			const uint8_t sm = eng.diag.shed_mask, fb_on = eng.diag.forced_bypass;

			if (sm != ovl_mask_seen || fb_on != ovl_forced_seen) {
				CON_PRINTF("OVERLOAD guard: shed_mask=0x%x (slots off:%s%s%s%s) forced_bypass=%u "
					   "sheds=%u over_blocks=%u limit=%u cyc/blk preset=%d:%s\r\n",
					   sm, (sm & 1u) ? " 0" : "", (sm & 2u) ? " 1" : "",
					   (sm & 4u) ? " 2" : "", (sm & 8u) ? " 3" : "", fb_on,
					   (unsigned int)eng.diag.overload_sheds,
					   (unsigned int)eng.diag.overload_blocks,
					   (unsigned int)OVERLOAD_LIMIT_CYC, preset_idx, preset_name(preset_idx));
				ovl_mask_seen = sm;
				ovl_forced_seen = fb_on;
			}
		}

		if (now >= next_status) {
			next_status += STATUS_MS;

			struct a_snap a = { 0 };
			const bool a_ok = snap_read(&a_seq, &a_pub, &a, sizeof(a));
			const uint32_t b = g_blocks, db = b - prev_blocks;
			const uint32_t fin = g_usb_frames_in, sof = g_sof_count;
			uint32_t events = out_cons.underruns + out_cons.dups + out_cons.drops +
					  g_usb_overrun + g_usb_nobuf;
#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
			events += ret_cons.underruns + ret_cons.dups + ret_cons.drops +
				  g_ret_overrun + g_in_send_fail;
#endif
			event_led = events != prev_events;
			prev_events = events;
			(void)gpio_pin_set_dt(&led_event, event_led ? 1 : 0);
			prev_blocks = b;

			unsigned int fmin, pmin, favg, pavg, fmax, pmax;

			cyc_fmt(a.cyc_min, &fmin, &pmin);
			cyc_fmt(a.cyc_avg, &favg, &pavg);
			cyc_fmt(a.cyc_max, &fmax, &pmax);
			const int32_t fbv = g_fb_value;
			/* frames per ms x 10000 */
			const unsigned int fb_x1e4 =
				(unsigned int)(((int64_t)fbv * 10000 + (1 << 13)) >> UA_FB_FRAC);

			CON_PRINTF("t=%u preset=%d:%s rej=%d cyc_blk_min/avg/max=%u/%u/%u%s "
				   "cyc_frame_avg/max=%u.%u/%u.%u cpu%%_avg/max=%u.%u/%u.%u "
				   "blk_total_max=%u\r\n",
				   (unsigned int)(now / 1000), preset_idx, preset_name(preset_idx),
				   preset_rejected, (unsigned int)a.cyc_min, (unsigned int)a.cyc_avg,
				   (unsigned int)a.cyc_max, a_ok ? "" : "?", favg / 10, favg % 10,
				   fmax / 10, fmax % 10, pavg / 10, pavg % 10, pmax / 10, pmax % 10,
				   (unsigned int)a.blk_max);
			CON_PRINTF("  usb out=%s play=%d starts=%u fill_min/avg/max=%u/%u/%u "
				   "fb=%u (%u.%04u f/ms) rx_f/s=%u sof/s=%u under=%u ovr=%u dup=%u "
				   "drop=%u zpkt=%u nobuf=%u\r\n",
				   g_out_stream ? "on" : "off", g_playing ? 1 : 0,
				   (unsigned int)g_stream_starts, (unsigned int)a.fill_min,
				   (unsigned int)a.fill_avg, (unsigned int)a.fill_max,
				   (unsigned int)fbv, fb_x1e4 / 10000, fb_x1e4 % 10000,
				   (unsigned int)(fin - prev_frames_in),
				   (unsigned int)(sof - prev_sof),
				   (unsigned int)out_cons.underruns, (unsigned int)g_usb_overrun,
				   (unsigned int)out_cons.dups, (unsigned int)out_cons.drops,
				   (unsigned int)g_usb_zero_pkts, (unsigned int)g_usb_nobuf);
			prev_frames_in = fin;
			prev_sof = sof;
#if CONFIG_SP1FX_USB_AUDIO_IN_STREAM
			{
				struct r_snap r = { 0 };

				(void)snap_read(&r_seq, &r_pub, &r, sizeof(r));
				const uint32_t fo = g_in_frames_out;

				CON_PRINTF("  ret in=%s tx_f/s=%u fill_min/avg/max=%u/%u/%u "
					   "under=%u dup=%u drop=%u ovr=%u silent_pkts=%u "
					   "send_fail=%u\r\n",
					   g_in_stream ? "on" : "off",
					   (unsigned int)(fo - prev_frames_out),
					   (unsigned int)r.fill_min, (unsigned int)r.fill_avg,
					   (unsigned int)r.fill_max,
					   (unsigned int)ret_cons.underruns,
					   (unsigned int)ret_cons.dups, (unsigned int)ret_cons.drops,
					   (unsigned int)g_ret_overrun,
					   (unsigned int)g_in_silent_pkts,
					   (unsigned int)g_in_send_fail);
				prev_frames_out = fo;
			}
#endif
			CON_PRINTF("  eng in_pk=%u out_pk=%u (x1e-3) in_clip=%u out_clip=%u nan=%u "
				   "sat16=%u | i2s blk/s=%u err=%u tmo=%u rec=%u cfg=%d/%d | "
				   "cs42=%02x%s tas=%s%s hp=%d out=%s usb=%d con_drop=%u rr=%x\r\n",
				   milli(eng.diag.in_peak), milli(eng.diag.out_peak),
				   (unsigned int)eng.diag.in_clips, (unsigned int)eng.diag.out_clips,
				   (unsigned int)eng.diag.nan_events, (unsigned int)g_out_sat,
				   (unsigned int)db, (unsigned int)g_err, (unsigned int)g_tmo,
				   (unsigned int)g_recover, g_i2s_cfg_rc, g_i2s_start_rc,
				   devid[0], cs42_cfg_ok ? "+" : (cs42_ok ? "!" : "-"),
				   tas_ok ? "ack" : "nak", tas_cfg_ok ? "+" : "-", hp_raw,
				   out_name[cur_out], usb_rc, (unsigned int)con_drops,
				   (unsigned int)sp1fx_boot_resetreas);
			{
				unsigned int a_sz = 0, m_sz = 0;
				const unsigned int a_used = stack_used(&audio_tcb, &a_sz);
				const unsigned int m_used = stack_used(k_current_get(), &m_sz);

				CON_PRINTF("  diag stk audio=%u/%u main=%u/%u | apply_cyc max/last=%u/%u "
					   "n=%u inits=%u settle_ms=%u%s | blk_cyc_max=%u | "
					   "loop_gap_ms max/1s=%u/%u starve=%u i2s_sleep=%u | "
					   "torn=%u lay_err=%u boots=%u | ovl shed=0x%x forced=%u "
					   "over_blk=%u\r\n",
					   a_used, a_sz, m_used, m_sz, (unsigned int)g_apply_max,
					   (unsigned int)g_apply_last, (unsigned int)g_apply_blocks,
					   (unsigned int)eng.diag.slot_inits, (unsigned int)settle_ms_last,
					   engine_config_settled(&eng) ? "" : " PENDING",
					   (unsigned int)g_blk_max_all, (unsigned int)loop_gap_max,
					   (unsigned int)loop_gap_win, (unsigned int)g_starve,
					   (unsigned int)g_i2s_recover_sleep,
					   (unsigned int)eng.diag.torn_retries,
					   (unsigned int)eng.diag.layout_errors,
					   (unsigned int)crashdump_boots(),
					   (unsigned int)eng.diag.shed_mask,
					   (unsigned int)eng.diag.forced_bypass,
					   (unsigned int)eng.diag.overload_blocks);
				loop_gap_win = 0;
			}
		}

		k_msleep(LOOP_MS);
	}
	return 0;
}
