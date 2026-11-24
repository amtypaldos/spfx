/*
 * SP-1 FX integrated firmware (v1). NOT TESTED ON HARDWARE as a whole; see
 * docs/BRINGUP_SP1FX.md for the staged owner test and docs/STATUS.md for what each part
 * inherited from the hardware-verified bring-up images.
 *
 * Threads (single core, Cortex-M4F at 64 MHz):
 *   audio  (prio 0)  USB OUT ring -> int16->float -> engine_process (64-frame blocks) ->
 *                    fades / trim -> I2S (slave; CS42L42 headphones or TAS2505 speaker).
 *                    From zephyr_usbaudio image A v3 (hardware-verified), unchanged except:
 *                    block peaks for the quiet detector, audio frame counter for the sync
 *                    clock, per-frame cpu counters for the protocol, windowed overload guard.
 *   sync   (prio 1)  sync-jack clock (MIDI 24 PPQN + PO pulses) counted in rendered audio
 *                    frames. Idle (20 ms polls, jack pins and bit timer untouched) while off.
 *   main   (prio 7)  control thread, 8 ms tick: SAADC ladders + faders -> controller
 *                    (gestures, soft takeover, presets, volume) -> PWM LEDs; companion
 *                    protocol on the CDC ACM port; headphone detect; settings journal;
 *                    diagnostic text; watchdog feed.
 *   USB stack threads (cooperative) run the UAC2 / CDC callbacks.
 *
 * Console scheme (firmware/app/app_logic.h): the CDC ACM port starts in TEXT mode when a
 * host opens it (DTR): rate-limited diagnostic lines. The first valid protocol frame
 * (docs/PROTOCOL.md) switches it to BINARY mode: one 0x00 separator, then only protocol
 * frames; no text at all until the host closes the port.
 *
 * Settings (docs/ADR-003-storage.md): journal of 512 B records in the two 4 KB pages
 * 0xFD000..0xFEFFF (devicetree partition sp1fx_config_partition, carved out of the top of
 * slot0; the image may not reach it: check_image.py --reserve). Writes are chunked so the
 * NVMC stalls stay short; a page erase (CPU stalled up to ~85 ms) only runs while the audio
 * has been quiet for 1.5 s, at power-off, or on an explicit SAVE_NOW, always under a duck.
 *
 * HEARING SAFETY: codec analog settings as the tone/usbaudio images (sp1_codecs.c, shared),
 * engine soft limiter, then CONFIG_SP1FX_OUT_TRIM_DB (-6 dB) digital ceiling; master volume
 * 2 dB per VOL press on a -60..0 dB taper (controls/volume.h).
 *
 * Provenance: zephyr_usbaudio (audio path, USB, watchdog, power-off, crash dump),
 * zephyr_controls (controls loop), zephyr_sync (sync driver use). Upstream notices:
 * firmware/app/zephyr_usbaudio/NOTICES.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/flash.h>
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
#include <stdarg.h>
#include <string.h>

#include "sp1_codecs.h"
#include "audio/usb_audio_core.h"
#include "dsp/engine.h"
#include "app/factory_presets.h"
#include "app/app_logic.h"
#include "app/sync_out.h"
#include "controls/controller.h"
#include "controls/volume.h"
#include "drivers/controls_hw.h"
#include "drivers/crashdump.h"
#include "drivers/ladder.h"
#include "drivers/midi_out.h"
#include "drivers/sync_hw.h"
#include "protocol/protocol.h"
#include "storage/config_store.h"

#define FW_VERSION       CONFIG_SP1FX_FW_VERSION
#define TICK_MS          8
#define FN_RELEASE_MS    60
#define HP_POLL_MS       250
#define DETAIL_EVERY     5        /* detail lines every 5th diag period */
#define ADC_TIMEOUT_MS   100      /* a SAADC scan normally takes < 1 ms */
#define SAVE_GAP_MS      30000u   /* rc2: at most one settings save per 30 s, none in the
				   * first 30 s after boot (power off / SAVE_NOW excepted) */

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
#define AUDIO_STACK_SIZE 6144
#define ENGINE_INIT_BUDGET (48u * 1024u)
#define STARVE_IDLE_CYC  (CPU_HZ / 20000u)   /* 50 us */
#define STARVE_RUN       32
/* Overload guard limit: 85 % of the 64-frame period (85,333 cycles at 64 MHz), judged on
 * the MEAN over 32-block windows, 3 windows in a row (~128 ms; dsp/engine.h). */
#define OVERLOAD_LIMIT_CYC ((uint32_t)((uint64_t)BLK_FRAMES * CPU_HZ / FS_HZ * 85u / 100u))
/* Quiet = 1.5 s of blocks with input and output below -66 dBFS (app_logic.h). */
#define QUIET_BLOCKS     1125u

#define UAC2_NODE        DT_NODELABEL(uac2_speaker)
#define OUT_TERMINAL_ID  UAC2_ENTITY_ID(DT_NODELABEL(in_terminal))

BUILD_ASSERT((CONFIG_SP1FX_UA_RING_FRAMES & (CONFIG_SP1FX_UA_RING_FRAMES - 1)) == 0,
	     "ring size must be a power of two");
BUILD_ASSERT(CONFIG_SP1FX_UA_LOW_FRAMES >= BLK_FRAMES &&
	     CONFIG_SP1FX_UA_LOW_FRAMES < CONFIG_SP1FX_UA_TARGET_FRAMES &&
	     CONFIG_SP1FX_UA_TARGET_FRAMES < CONFIG_SP1FX_UA_HIGH_FRAMES &&
	     CONFIG_SP1FX_UA_HIGH_FRAMES + 2 * USB_MAX_FRAMES <= CONFIG_SP1FX_UA_RING_FRAMES,
	     "ring limits");

/* --------------------- settings flash area (ADR-003) --------------------- */
/* The two journal pages. Every flash access of this image goes through cfg_flash_addr(),
 * which refuses anything outside them. The devicetree overlay carves them out of the top of
 * slot0; these asserts pin the layout so a dts edit cannot silently move them onto the
 * bootloader (0x0..0x1FFFF), the image, or the board's 'storage' page (0xFF000). */
#define CFG_NODE   DT_NODELABEL(sp1fx_config_partition)
#define CFG_BASE   ((uint32_t)DT_REG_ADDR(CFG_NODE))
#define CFG_SIZE   ((uint32_t)DT_REG_SIZE(CFG_NODE))
#define SLOT0_END  ((uint32_t)(DT_REG_ADDR(DT_NODELABEL(slot0_partition)) + \
			       DT_REG_SIZE(DT_NODELABEL(slot0_partition))))
BUILD_ASSERT(DT_REG_ADDR(CFG_NODE) == 0xFD000 && DT_REG_SIZE(CFG_NODE) == 0x2000,
	     "settings pages must be 0xFD000..0xFEFFF (ADR-003)");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(slot0_partition)) == 0x20000 &&
	     DT_REG_ADDR(DT_NODELABEL(slot0_partition)) + DT_REG_SIZE(DT_NODELABEL(slot0_partition))
		     <= 0xFD000, "the app image must end below the settings pages");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(storage_partition)) == 0xFF000,
	     "the board storage page must stay at 0xFF000 (never touched)");
BUILD_ASSERT(CFG_SIZE == 2u * STORE_PAGE_BYTES, "two journal pages");
#define FLASH_CHUNK      8u      /* bytes per flash_write: 2 words, <= ~82 us of NVMC stall */
#define FLASH_GAP_US     400     /* sleep between chunks: audio + USB run in between */

K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLK_BYTES, SLAB_BLOCKS, 4);
K_MEM_SLAB_DEFINE_STATIC(usb_rx_slab, ROUND_UP(USB_PKT_BYTES, UDC_BUF_GRANULARITY), 4,
			 UDC_BUF_ALIGN);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static const struct device *const uac2_dev = DEVICE_DT_GET(UAC2_NODE);
static const struct device *const flash_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

#define ZU DT_PATH(zephyr_user)
static const struct gpio_dt_spec osc_en     = GPIO_DT_SPEC_GET(ZU, osc_en_gpios);
static const struct gpio_dt_spec cs42_rst   = GPIO_DT_SPEC_GET(ZU, cs42_rst_gpios);
static const struct gpio_dt_spec tas_rst    = GPIO_DT_SPEC_GET(ZU, tas_rst_gpios);

volatile uint32_t sp1fx_boot_resetreas;

/* --------------------------- engine (static pool) ------------------------ */
static uint8_t engine_pool[CONFIG_SP1FX_ENGINE_POOL_KIB * 1024] __aligned(8);
static engine_t eng;

/* ------------------------------ rings / state ---------------------------- */
static int16_t ring_store[CONFIG_SP1FX_UA_RING_FRAMES * 2];
static ua_ring_t out_ring;                 /* USB thread -> audio thread */
static ua_cons_t out_cons;                 /* audio thread */
static ua_fb_t fb;                         /* USB thread */
static volatile int32_t g_fb_value = 48 << UA_FB_FRAC;
static volatile bool g_out_stream;         /* host enabled the OUT terminal */
static volatile bool g_out_restart_req;    /* USB -> audio: flush, prebuffer */
static volatile uint32_t g_usb_overrun;    /* frames lost: ring full */
static volatile uint32_t g_usb_zero_pkts;
static volatile uint32_t g_usb_nobuf;
static volatile uint32_t g_usb_frames_in;
static volatile uint32_t g_sof_count;
static volatile uint32_t g_stream_starts;

/* --------------------- per-second snapshots (seqlock) -------------------- */
struct a_snap {
	uint32_t cyc_min, cyc_max, cyc_avg;       /* engine_process per 64-frame block */
	uint32_t blk_max, blk_avg;                /* whole block work incl. conversions */
	uint32_t ovh_max;                         /* max (block - engine): conversions + preemption */
	uint32_t spikes;                          /* blocks over the guard limit in the window */
	uint32_t fill_min, fill_max, fill_avg;
	float in_pk, out_pk;                      /* peaks in the window (every 2nd sample) */
	uint32_t nblk;
};
static struct a_snap a_pub;
static volatile uint32_t a_seq;

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
static volatile uint32_t g_frames;         /* audio frames rendered (sync clock time base) */
static volatile uint32_t g_out_sat;
static volatile bool     g_playing;
static volatile uint32_t g_apply_max, g_apply_blocks;
static volatile uint32_t g_blk_max_all;
static volatile uint32_t g_starve;
static volatile uint32_t g_i2s_recover_sleep;
static quiet_t g_quiet;                    /* audio thread writes, main reads one word */

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

/* peak of |l|,|r| over every 2nd frame (cheap: quiet detection and the diag line only) */
static float peak2(const float *l, const float *r)
{
	float p = 0.0f;

	for (int i = 0; i < BLK_FRAMES; i += 2) {
		float a = fabsf(l[i]), b = fabsf(r[i]);

		p = a > p ? a : p;
		p = b > p ? b : p;
	}
	return p;
}

/* One 64-frame block: USB ring -> engine -> int16 (into the I2S block). */
static void render_block(int16_t *dst, uint32_t *eng_cycles, float *ipk, float *opk)
{
	static float last_l, last_r;
	bool decay = false;

	if (g_out_restart_req) {
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
		(void)ua_ramp_apply(&in_ramp, g_out_stream ? 1.0f : 0.0f, in_l, in_r, BLK_FRAMES);
	}
	*ipk = peak2(in_l, in_r);

	const uint32_t t0 = DWT->CYCCNT;

	engine_process(&eng, in_l, in_r, out_l, out_r, BLK_FRAMES);
	*eng_cycles = DWT->CYCCNT - t0;
	*opk = peak2(out_l, out_r);

	const float g = ua_ramp_apply(&out_ramp, g_out_enable ? 1.0f : 0.0f, out_l, out_r,
				      BLK_FRAMES);

	g_gain_zero = (g <= 0.0f);
	g_out_sat += ua_f32_to_s16(out_l, out_r, dst, BLK_FRAMES, trim_gain);
}

static void sup_audio_tick(void);   /* stall watcher, called from the audio thread */

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

	uint32_t w_min = UINT32_MAX, w_max = 0, w_blk_max = 0, w_n = 0, w_ovh = 0, w_spk = 0;
	uint64_t w_sum = 0, w_bsum = 0;
	float w_ipk = 0.0f, w_opk = 0.0f;
	uint32_t busy_run = 0;
	uint32_t t_iter = DWT->CYCCNT, bc = 0;

	for (;;) {
		void *blk;

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
		float ipk, opk;

		render_block(blk, &ec, &ipk, &opk);

		bc = DWT->CYCCNT - tb;
		(void)engine_overload_feed(&eng, bc, OVERLOAD_LIMIT_CYC);
		quiet_feed(&g_quiet, ipk, opk);
		g_frames += BLK_FRAMES;
		sup_audio_tick();

		if (eng.diag.cfg_applies != applies0 || eng.diag.slot_inits != inits0) {
			g_apply_blocks++;
			if (ec > g_apply_max) {
				g_apply_max = ec;
			}
		}
		if (bc > g_blk_max_all) {
			g_blk_max_all = bc;
		}
		/* protocol METERS: cycles per stereo frame, like the declared budget */
		const uint32_t ecf = ec / BLK_FRAMES;

		eng.diag.cpu_last_cycles = ecf;
		if (ecf > eng.diag.cpu_peak_cycles) {
			eng.diag.cpu_peak_cycles = ecf;
		}
		w_min = ec < w_min ? ec : w_min;
		w_max = ec > w_max ? ec : w_max;
		w_blk_max = bc > w_blk_max ? bc : w_blk_max;
		if (bc > ec && bc - ec > w_ovh) {
			w_ovh = bc - ec;
		}
		if (bc > OVERLOAD_LIMIT_CYC) {
			w_spk++;
		}
		w_ipk = ipk > w_ipk ? ipk : w_ipk;
		w_opk = opk > w_opk ? opk : w_opk;
		w_sum += ec;
		w_bsum += bc;
		if (++w_n >= BLOCKS_PER_S) {
			struct a_snap s = {
				.cyc_min = w_min, .cyc_max = w_max,
				.cyc_avg = (uint32_t)(w_sum / w_n), .blk_max = w_blk_max,
				.blk_avg = (uint32_t)(w_bsum / w_n), .ovh_max = w_ovh, .spikes = w_spk,
				.fill_min = out_cons.fill_n ? out_cons.fill_min : 0,
				.fill_max = out_cons.fill_max,
				.fill_avg = out_cons.fill_n ?
					(uint32_t)(out_cons.fill_sum / out_cons.fill_n) : 0,
				.in_pk = w_ipk, .out_pk = w_opk,
				.nblk = w_n,
			};
			SNAP_PUBLISH(a_seq, a_pub, s);
			ua_cons_stats_window_reset(&out_cons);
			w_min = UINT32_MAX;
			w_max = w_blk_max = w_n = w_ovh = w_spk = 0;
			w_sum = w_bsum = 0;
			w_ipk = w_opk = 0.0f;
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
		g_i2s_recover_sleep++;
		k_msleep(1);
		t_iter = DWT->CYCCNT;
		bc = 0;
	}
}

/* ------------------------------ UAC2 callbacks --------------------------- */
/* USB stack cooperative thread; never block. As zephyr_usbaudio image A. */

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
	ARG_UNUSED(dev); ARG_UNUSED(terminal); ARG_UNUSED(user_data); ARG_UNUSED(buf);
}

static uint32_t uac2_feedback_cb(const struct device *dev, uint8_t terminal,
				 void *user_data)
{
	ARG_UNUSED(dev); ARG_UNUSED(terminal); ARG_UNUSED(user_data);
	return (uint32_t)g_fb_value;   /* Q10.14 (Full-Speed, 3 bytes on the wire) */
}

static void uac2_sof_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev); ARG_UNUSED(user_data);

	g_sof_count++;
	if (g_out_stream) {
		g_fb_value = ua_fb_update(&fb, ua_ring_fill(&out_ring));
	}
}

static const struct uac2_ops uac2_ops = {
	.sof_cb = uac2_sof_cb,
	.terminal_update_cb = uac2_terminal_update_cb,
	.get_recv_buf = uac2_get_recv_buf,
	.data_recv_cb = uac2_data_recv_cb,
	.buf_release_cb = uac2_buf_release_cb,
	.feedback_cb = uac2_feedback_cb,
};

/* --------------------------- USB CDC ACM port ---------------------------- */
/* One port, two uses (see the console scheme at the top). TX: our ring, drained by the CDC
 * callback; a line or frame that does not fit is dropped whole. RX: bytes into rx_rb, parsed
 * by the protocol on the control thread. */
static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
/* TX: app_logic.h contx_t (whole-message queue, never waits, 0x00 before the first binary
 * frame; host-tested with a stalled host). Producer: control thread; consumer: cdc_cb. */
#define CON_TX_BYTES 4096u
#define RX_DRAIN_MAX 512u      /* protocol bytes parsed per 8 ms tick (rest: next tick) */
static uint8_t con_buf[CON_TX_BYTES];
static contx_t ctx;
RING_BUF_DECLARE(rx_rb, 1024);
static struct k_spinlock con_lock, rx_lock;
static bool usb_ok;
static int usb_rc = 1;
static uint32_t rx_drops, cdc_tx_bytes, cdc_rx_bytes;
#define conm (ctx.m)
static volatile uint32_t g_binary_since_ms;    /* console switched to binary at */
static ratelimit_t ev_rl;
static proto_t proto;

static void cdc_cb(const struct device *dev, void *user)
{
	ARG_UNUSED(user);

	if (!uart_irq_update(dev)) {
		return;
	}
	if (uart_irq_rx_ready(dev)) {
		uint8_t b[64];
		int n;

		while ((n = uart_fifo_read(dev, b, sizeof(b))) > 0) {
			k_spinlock_key_t key = k_spin_lock(&rx_lock);
			uint32_t put = ring_buf_put(&rx_rb, b, (uint32_t)n);

			k_spin_unlock(&rx_lock, key);
			rx_drops += (uint32_t)n - put;
			cdc_rx_bytes += (uint32_t)n;
		}
	}
	if (uart_irq_tx_ready(dev)) {
		k_spinlock_key_t key = k_spin_lock(&con_lock);
		const uint8_t *p;
		uint32_t n = txq_peek(&ctx.q, &p, 64);
		int w = (n > 0u) ? uart_fifo_fill(dev, p, (int)n) : 0;

		if (w < 0) {
			w = 0;
		}
		txq_consume(&ctx.q, (uint32_t)w);
		cdc_tx_bytes += (uint32_t)w;
		bool empty = txq_used(&ctx.q) == 0u;

		k_spin_unlock(&con_lock, key);
		if (empty) {
			uart_irq_tx_disable(dev);
		}
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
	uart_irq_rx_enable(cdc);
	usb_ok = true;
}

static bool port_dtr(void)
{
	uint32_t dtr = 0;

	if (!usb_ok) {
		return false;
	}
	(void)uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
	return dtr != 0u;
}

/* Re-evaluate the port mode (contx sends the 0x00 separator on the switch to binary). */
static void conm_poll(void)
{
	const int dtr = port_dtr() ? 1 : 0;
	k_spinlock_key_t key = k_spin_lock(&con_lock);

	contx_poll(&ctx, dtr, proto.rx_frames);
	const bool kick = txq_used(&ctx.q) != 0u;

	if (!conmode_binary_ok(&ctx.m)) {
		g_binary_since_ms = 0u;
	} else if (g_binary_since_ms == 0u) {
		g_binary_since_ms = k_uptime_get_32() | 1u;
	}

	k_spin_unlock(&con_lock, key);
	if (kick && usb_ok) {
		uart_irq_tx_enable(cdc);
	}
}

/* text: only in text mode; whole line or dropped (never waits) */
static void con_write(const char *s, int len)
{
	if (len <= 0 || !usb_ok) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&con_lock);
	const int ok = contx_text(&ctx, (const uint8_t *)s, (uint32_t)len);

	k_spin_unlock(&con_lock, key);
	if (ok) {
		uart_irq_tx_enable(cdc);
	}
}

static void con_printf(const char *fmt, ...)
{
	char buf[384];
	va_list ap;

	if (!conmode_text_ok(&conm)) {
		return;
	}
	va_start(ap, fmt);
	int n = vsnprintk(buf, sizeof(buf), fmt, ap);

	va_end(ap);
	if (n <= 0) {
		return;
	}
	if (n >= (int)sizeof(buf)) {
		n = (int)sizeof(buf) - 1;
		buf[n - 2] = '\r';
		buf[n - 1] = '\n';
	}
	con_write(buf, n);
}

/* event lines (things that happened): rate-limited so a burst cannot flood the port */
#define EV_PRINTF(...) do {						\
		if (conmode_text_ok(&conm) &&				\
		    ratelimit_take(&ev_rl, k_uptime_get_32())) {	\
			con_printf(__VA_ARGS__);			\
		}							\
	} while (0)

/* protocol tx: only in binary mode (a frame before the switch would corrupt text) */
static void proto_tx(void *c, const uint8_t *bytes, size_t n)
{
	ARG_UNUSED(c);
	conm_poll();   /* the request that caused this answer may be the very first frame */
	if (!usb_ok) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&con_lock);
	const int ok = contx_frame(&ctx, bytes, (uint32_t)n);

	k_spin_unlock(&con_lock, key);
	if (ok) {
		uart_irq_tx_enable(cdc);
	}
}

static void rx_drain(void)
{
	uint8_t b[64];
	uint32_t total = 0;

	while (total < RX_DRAIN_MAX) {
		k_spinlock_key_t key = k_spin_lock(&rx_lock);
		uint32_t n = ring_buf_get(&rx_rb, b, sizeof(b));

		k_spin_unlock(&rx_lock, key);
		if (n == 0u) {
			break;
		}
		total += n;
		proto_rx(&proto, b, n);
	}
}

/* ------------------------------ breadcrumbs ------------------------------ */
/* Where the control thread (the only watchdog feeder) is. Read by the stall supervisor. */
enum {
	ST_BOOT = 0, ST_TOP, ST_CONSOLE, ST_ADC, ST_CONTROLLER, ST_PROTO_RX, ST_PROTO_TICK,
	ST_STORE, ST_LEDS, ST_HP_I2C, ST_DIAG, ST_SLEEP, ST_FLASH_W, ST_FLASH_E, ST_WAIT_OUT,
	ST_POWEROFF, ST_COUNT
};
static const char *const stage_name[ST_COUNT] = {
	"boot", "top", "console", "adc", "controller", "proto_rx", "proto_tick", "store", "leds",
	"hp_i2c", "diag", "sleep", "flash_w", "flash_e", "wait_out", "poweroff"
};
static volatile uint8_t g_stage;
static volatile uint32_t g_stage_ms;           /* when g_stage was entered */
static volatile uint32_t g_loop_ms;            /* last pass through the top of the loop */
static volatile uint32_t g_flash_end_ms;       /* end of the last flash write/erase */
static volatile uint32_t g_flash_ops;
static k_tid_t g_main_tid;
/* copied into the crash record by the watchdog capture (crashdump_set_crumbs):
 * [0] stage, [1] uptime of the last loop pass, [2] uptime the stage was entered,
 * [3] SAADC: bit0 scan running, bit1 scan stuck, bits 8.. timeouts */
static volatile uint32_t g_crumbs[CRASHDUMP_CRUMBS];

static inline void stage(uint8_t s)
{
	g_stage = s;
	g_stage_ms = k_uptime_get_32();
	g_crumbs[0] = s;
	g_crumbs[2] = g_stage_ms;
}

/* ------------------------- watchdog / power ------------------------------ */
static void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++) {
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
	}
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
	const uint8_t prev = g_stage;

	stage(ST_WAIT_OUT);
	for (int t = 0; t < 10 && !g_gain_zero; t++) {
		wait_fed(10);
	}
	wait_fed(15);
	stage(prev);
}

static bool cs42_ok, tas_ok, cs42_cfg_ok, tas_cfg_ok;

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

/* ------------------------------ settings --------------------------------- */
static store_t store;
static store_debounce_t deb;
static save_sched_t ssched;
static bool store_ok;
static uint32_t st_saves, st_erases, st_errors, st_erase_ms_max, st_write_ms_max;
static int st_load_rc = -2;

/* Absolute flash address of (page, off, len) in the journal, or 0 if out of range. */
static uint32_t cfg_flash_addr(uint32_t page, uint32_t off, uint32_t len)
{
	if (page > 1u || off > STORE_PAGE_BYTES || len > STORE_PAGE_BYTES - off) {
		return 0u;
	}
	const uint32_t a = CFG_BASE + page * STORE_PAGE_BYTES + off;

	if (a < CFG_BASE || a + len > CFG_BASE + CFG_SIZE || a < SLOT0_END) {
		return 0u;
	}
	return a;
}

static int fl_read(void *uctx, uint32_t page, uint32_t off, void *buf, uint32_t len)
{
	ARG_UNUSED(uctx);
	const uint32_t a = cfg_flash_addr(page, off, len);

	if (a == 0u) {
		return -1;
	}
	return flash_read(flash_dev, (off_t)a, buf, len);
}

/* Chunked: each flash_write stalls the CPU for <= 2 x 41 us (nRF52840 tWRITE max per word);
 * the sleep between chunks lets the audio thread, I2S and USB interrupts run. */
static int fl_write(void *uctx, uint32_t page, uint32_t off, const void *buf, uint32_t len)
{
	ARG_UNUSED(uctx);
	const uint32_t a = cfg_flash_addr(page, off, len);

	if (a == 0u || (a & 3u) || (len & 3u)) {
		return -1;
	}
	const int64_t t0 = k_uptime_get();
	const uint8_t *p = buf;
	const uint8_t prev = g_stage;

	stage(ST_FLASH_W);
	g_flash_ops++;

	for (uint32_t i = 0; i < len; i += FLASH_CHUNK) {
		const uint32_t n = MIN(FLASH_CHUNK, len - i);
		int rc = flash_write(flash_dev, (off_t)(a + i), p + i, n);

		if (rc != 0) {
			g_flash_end_ms = k_uptime_get_32();
			stage(prev);
			return rc;
		}
		feed_wdt();
		k_usleep(FLASH_GAP_US);
	}
	const uint32_t ms = (uint32_t)(k_uptime_get() - t0);

	g_flash_end_ms = k_uptime_get_32();
	stage(prev);

	st_write_ms_max = MAX(st_write_ms_max, ms);
	return 0;
}

/* A page erase stalls the CPU (and with it USB and I2S servicing) for up to ~85 ms
 * (nRF52840 tERASEPAGE; Zephyr's nRF flash driver without radio sync erases in one blocking
 * NVMC operation). Only reached when the scheduler allows it (quiet audio), at power-off or
 * on SAVE_NOW; the output is ducked to silence first in every case. */
static int fl_erase(void *uctx, uint32_t page)
{
	ARG_UNUSED(uctx);
	const uint32_t a = cfg_flash_addr(page, 0u, STORE_PAGE_BYTES);

	if (a == 0u || (a % STORE_PAGE_BYTES) != 0u) {
		return -1;
	}
	const uint8_t prev = g_stage;

	stage(ST_FLASH_E);
	g_flash_ops++;
	engine_set_duck(&eng, 1);
	wait_fed(30);                      /* engine duck: ~20 ms to silence */
	const uint32_t err0 = g_err, und0 = out_cons.underruns;
	const int64_t t0 = k_uptime_get();

	feed_wdt();
	int rc = flash_erase(flash_dev, (off_t)a, STORE_PAGE_BYTES);

	feed_wdt();
	const uint32_t ms = (uint32_t)(k_uptime_get() - t0);

	g_flash_end_ms = k_uptime_get_32();
	wait_fed(10);
	engine_set_duck(&eng, 0);
	stage(prev);
	st_erases++;
	st_erase_ms_max = MAX(st_erase_ms_max, ms);
	EV_PRINTF("FLASH erase page 0x%05x rc=%d took %u ms (ducked) i2s_err+%u usb_under+%u\r\n",
		  (unsigned int)a, rc, (unsigned int)ms, (unsigned int)(g_err - err0),
		  (unsigned int)(out_cons.underruns - und0));
	return rc;
}

/* USB cable present (VBUS). rc2: no flash page erase while it is: the ~85 ms CPU stall of an
 * erase is the one thing rc1 did with USB running that the stable usbaudio images never did,
 * and is the leading (unproven) suspect for the rc1 watchdog resets with the companion. */
static bool usb_vbus(void)
{
	return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0u;
}
static uint32_t st_busy;   /* SAVE_NOW answered "busy" */
static uint32_t st_dedup;  /* due saves skipped: nothing differs from the last saved record */
static uint32_t st_last_save_ms;          /* boot time, then the last write */
static uint8_t st_last_rec[STORE_RECORD_BYTES];
static uint16_t st_last_len;
static bool safe_boot;                    /* crash-loop breaker: journal not used this run */

static void cur_data(store_data_t *d)
{
	d->cfg = eng.cfg;
	d->bpm = proto.bpm;
	d->sync_on = proto.sync_on;
}

static int do_save(const char *why)
{
	static store_data_t d;   /* control thread only; keeps 1 KB off the stack */

	if (!store_ok) {
		return -1;
	}
	if (safe_boot) {
		return -1;   /* never overwrite the journal from a crash-loop safe boot */
	}
	cur_data(&d);
	static uint8_t enc[STORE_RECORD_BYTES];
	const uint16_t n = store_encode(&d, enc, sizeof(enc));

	if (n != 0u && n == st_last_len && memcmp(enc, st_last_rec, n) == 0) {
		st_dedup++;   /* identical to what is on flash: no write */
		return 0;
	}
	const int64_t t0 = k_uptime_get();
	int rc = store_save(&store, &d);

	if (rc == 0) {
		st_saves++;
		memcpy(st_last_rec, enc, n);
		st_last_len = n;
	} else {
		st_errors++;
	}
	st_last_save_ms = k_uptime_get_32();
	EV_PRINTF("SAVE (%s) rc=%d seq=%u page=%d slot=%d took %u ms\r\n", why, rc,
		  (unsigned int)store.seq, store.active_page, store.next_slot - 1,
		  (unsigned int)(k_uptime_get() - t0));
	return rc;
}

static int proto_save_now(void *uctx)
{
	ARG_UNUSED(uctx);
	if (store_ok && usb_vbus() && store_save_needs_erase(&store)) {
		/* would erase with USB running: keep it pending (power off saves it) */
		ssched.pending = 1;
		st_busy++;
		return PR_BUSY;
	}
	int rc = do_save("SAVE_NOW");

	if (rc == 0) {
		deb.dirty = 0;
		ssched.pending = 0;
	}
	return rc;
}

static void storage_tick(uint32_t now)
{
	if (!store_ok || safe_boot) {
		return;
	}
	/* rc2: nothing in the first 30 s after boot and at most one save per 30 s (the debounce
	 * keeps the change marked dirty meanwhile) */
	const int due = (now - st_last_save_ms >= SAVE_GAP_MS) ? store_debounce_due(&deb, now) : 0;
	/* "quiet" for the scheduler = an erase is allowed: silent audio AND no USB cable (rc2) */
	const int quiet = quiet_ok(&g_quiet, QUIET_BLOCKS) && engine_config_settled(&eng) &&
			  !usb_vbus();

	/* store_*_needs_erase read flash (cheap reads, cached for the spare page) */
	const save_action_t act = save_sched_next(&ssched, now, due,
						  (due || ssched.pending) ? store_save_needs_erase(&store) : 0,
						  quiet ? store_spare_needs_erase(&store) : 0, quiet);

	switch (act) {
	case SAVE_WRITE:
		(void)do_save("debounced");
		break;
	case SAVE_WRITE_WITH_ERASE:
		save_sched_erased(&ssched, now);
		(void)do_save("debounced, page erase while quiet");
		break;
	case SAVE_ERASE_SPARE: {
		save_sched_erased(&ssched, now);
		int rc = store_erase_spare(&store);

		if (rc != 0) {
			st_errors++;
		}
		break;
	}
	default:
		break;
	}
}

/* ------------------------------ sync thread ------------------------------ */
static volatile uint16_t g_sync_bpm10 = 1200;
static volatile uint8_t g_sync_want;
static volatile uint8_t g_sync_halt;
static volatile uint32_t g_sync_ticks, g_sync_pulses, g_sync_starts;
static volatile uint8_t g_sync_hw_init;

#if CONFIG_SP1FX_SYNC
#define SYNC_STACK_SIZE 1536
#define SYNC_MAX_ADVANCE 4800u   /* frames per poll after a stall (100 ms) */
static K_THREAD_STACK_DEFINE(sync_stack, SYNC_STACK_SIZE);
static struct k_thread sync_tcb;

static void sync_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	static sync_out_t so;
	uint16_t bpm10 = 0;
	bool on = false, stopping = false;
	uint32_t last = g_frames;

	sync_out_init(&so, (float)FS_HZ);
	for (;;) {
		if (g_sync_halt) {
			k_sleep(K_FOREVER);
		}
		const uint16_t want_bpm = g_sync_bpm10;

		if (want_bpm != bpm10) {
			bpm10 = want_bpm;
			sync_out_set_bpm(&so, (float)bpm10 / 10.0f);
		}
		if (g_sync_want && !on) {
			if (!g_sync_hw_init) {
				/* first use: configure the jack pins (idle levels) and the timer */
				const sync_hw_cfg_t hw = {
					.midi_invert = IS_ENABLED(CONFIG_SP1FX_SYNC_MIDI_INVERT),
					.swap_tip_ring = IS_ENABLED(CONFIG_SP1FX_SYNC_SWAP_TIP_RING),
					.po_active_high = !IS_ENABLED(CONFIG_SP1FX_SYNC_PO_ACTIVE_LOW),
					.po_pulse_ms = CONFIG_SP1FX_SYNC_PO_PULSE_MS,
					.midi_enable = 1,
					.po_enable = 1,
					.start_stopped = 1,
				};
				(void)sync_hw_init(&hw);
				g_sync_hw_init = 1;
			}
			sync_hw_run(1);
			sync_out_start(&so);
			last = g_frames;
			on = true;
			stopping = false;
			g_sync_starts++;
		} else if (!g_sync_want && on) {
			sync_out_stop(&so);    /* next advance yields MIDI Stop */
			on = false;
			stopping = true;
		}
		const uint32_t fr = g_frames;
		uint32_t n = fr - last;

		last = fr;
		if (n > SYNC_MAX_ADVANCE) {
			n = SYNC_MAX_ADVANCE;
		}
		if (on || stopping) {
			sync_out_events_t ev = sync_out_advance(&so, n);

			if (ev.start) {
				(void)sync_hw_midi(MIDI_START);
			}
			for (uint32_t i = 0; i < ev.midi_ticks; i++) {
				(void)sync_hw_midi(MIDI_CLOCK);
			}
			for (uint32_t i = 0; i < ev.po_pulses; i++) {
				(void)sync_hw_po_pulse();
			}
			if (ev.stop) {
				(void)sync_hw_midi(MIDI_STOP);
			}
			g_sync_ticks += ev.midi_ticks;
			g_sync_pulses += ev.po_pulses;
			if (stopping && !ev.stop && sync_hw_drained()) {
				sync_hw_run(0);   /* no interrupts while the clock is off */
				stopping = false;
			}
		}
		k_msleep((on || stopping) ? 1 : 20);
	}
}
#endif

/* ------------------------------ controls --------------------------------- */
static controller_t ui;
static ladder_t lad1, lad2;
static fader_t fad[4];
static int hw_rc = 1;

static int fill_factory(void *uctx, int idx, store_data_t *out)
{
	ARG_UNUSED(uctx);
	if (idx < 0 || idx >= FACTORY_PRESET_COUNT) {
		return -1;
	}
	factory_preset_fill(&factory_presets[idx], out);
	return 0;
}

static const char *fx_name(int id)
{
	const fx_desc *d = fx_get(id);

	return (d && d->name) ? d->name : "?";
}

static const char *preset_name(int idx)
{
	return (idx >= 0 && idx < FACTORY_PRESET_COUNT) ? factory_presets[idx].name : "-";
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

static int idb(float db)
{
	return (int)lrintf(db);
}

/* cycles per 64-frame block -> % of the block period x10 */
static unsigned int pct10(uint32_t blk)
{
	return (unsigned int)(((uint64_t)blk * FS_HZ * 1000u + (uint64_t)BLK_FRAMES * CPU_HZ / 2u) /
			      ((uint64_t)BLK_FRAMES * CPU_HZ));
}

static void wdt_timeout_cb(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	crashdump_wdt_capture();
}

static unsigned int stack_used(k_tid_t t, unsigned int *size)
{
	size_t unused = 0;

	*size = (unsigned int)t->stack_info.size;
	if (k_thread_stack_space_get(t, &unused) != 0) {
		return 0u;
	}
	return *size - (unsigned int)unused;
}

/* ---------------------------- stall supervisor ---------------------------- */
/* The audio thread (clocked by I2S blocks, not by the kernel timer) checks every 100 ms that the control loop is making
 * progress. After 1.5 s and again after 3.5 s without progress (the watchdog resets at ~4 s)
 * it writes a one-line snapshot into retained RAM (crashdump_stall_save): loop stage and how
 * long it has been there, the control thread's scheduler state, saved PC/LR and return
 * addresses found on its stack (addr2line them), CPU share of every thread over the last
 * 0.5 s, USB SOF/audio/USB-frame rates, console mode and queue, last protocol request, time
 * since the last flash operation. It never feeds the watchdog. */
#define SUP_MS        100u
#define SUP_WIN_MS    500u
#define STALL1_MS     1500u
#define STALL2_MS     3500u
#define SUP_THREADS   10

static struct {
	k_tid_t tid;
	const char *name;
	uint64_t prev, cur;
} sup_thr[SUP_THREADS];
static int sup_n;
static uint64_t sup_tot_prev, sup_tot_cur, sup_idle_prev, sup_idle_cur;
static uint32_t sup_sof_prev, sup_sof_cur, sup_blk_prev, sup_blk_cur, sup_fin_prev, sup_fin_cur;
static uint32_t sup_ticks;
static uint8_t sup_snaps;
static volatile uint8_t g_stall_resolved;
static volatile uint32_t g_stall_max_ms, g_stall_count;
static char sup_line[CRASHDUMP_STALL_SLOTS][CRASHDUMP_STALL_LEN];

static void sup_add_thread(const struct k_thread *t, void *user)
{
	ARG_UNUSED(user);
	if (sup_n < SUP_THREADS) {
		sup_thr[sup_n].tid = (k_tid_t)t;
		sup_thr[sup_n].name = k_thread_name_get((k_tid_t)t);
		sup_n++;
	}
}

static void sup_sample(void)
{
	k_thread_runtime_stats_t st;

	for (int i = 0; i < sup_n; i++) {
		sup_thr[i].prev = sup_thr[i].cur;
		if (k_thread_runtime_stats_get(sup_thr[i].tid, &st) == 0) {
			sup_thr[i].cur = st.execution_cycles;
		}
	}
	sup_tot_prev = sup_tot_cur;
	sup_idle_prev = sup_idle_cur;
	if (k_thread_runtime_stats_all_get(&st) == 0) {
		sup_tot_cur = st.execution_cycles;
		sup_idle_cur = st.idle_cycles;
	}
	sup_sof_prev = sup_sof_cur;
	sup_sof_cur = g_sof_count;
	sup_blk_prev = sup_blk_cur;
	sup_blk_cur = g_blocks;
	sup_fin_prev = sup_fin_cur;
	sup_fin_cur = g_usb_frames_in;
}

static int bt_scan(const uint32_t *sp, uint32_t *out, int max)
{
	const uintptr_t lo = g_main_tid->stack_info.start;
	const uintptr_t hi = lo + g_main_tid->stack_info.size;
	int n = 0;

	if ((uintptr_t)sp < lo || (uintptr_t)sp >= hi) {
		return 0;
	}
	for (const uint32_t *w = sp + 8; (uintptr_t)w < hi && n < max; w++) {
		const uint32_t v = *w;

		if ((v & 1u) && v >= 0x20000u && v < 0xFD000u) {
			out[n++] = v;
		}
	}
	return n;
}

static void sup_snapshot(int slot, uint32_t now, uint32_t age)
{
	char *b = sup_line[slot];
	const size_t cap = CRASHDUMP_STALL_LEN;
	const k_tid_t cur = k_current_get();
	const bool main_cur = (cur == g_main_tid);
	const uint32_t *frame = NULL;
	uint32_t bt[6] = { 0 };

	if (main_cur && (SCB->ICSR & SCB_ICSR_RETTOBASE_Msk) != 0u) {
		frame = (const uint32_t *)__get_PSP();
	} else if (!main_cur) {
		frame = (const uint32_t *)g_main_tid->callee_saved.psp;
	}
	const int nbt = frame ? bt_scan(frame, bt, 6) : 0;
	const char *cn = k_thread_name_get(cur);
	int len = snprintk(b, cap, "up=%u loop_age=%u stage=%s(%u ms) main:%s st=0x%02x pc=%08x lr=%08x "
			   "bt=%x,%x,%x,%x,%x,%x cur=%s cpu%%o:",
			   now, age, stage_name[g_stage < ST_COUNT ? g_stage : 0],
			   now - g_stage_ms, main_cur ? "RUN" : "off",
			   (unsigned int)g_main_tid->base.thread_state,
			   frame ? (unsigned int)frame[6] : 0u, frame ? (unsigned int)frame[5] : 0u,
			   nbt > 0 ? bt[0] : 0u, nbt > 1 ? bt[1] : 0u, nbt > 2 ? bt[2] : 0u,
			   nbt > 3 ? bt[3] : 0u, nbt > 4 ? bt[4] : 0u, nbt > 5 ? bt[5] : 0u,
			   cn ? cn : "?");
	const uint64_t dt = sup_tot_cur - sup_tot_prev;

	for (int i = 0; i < sup_n && len > 0 && (size_t)len < cap; i++) {
		const uint64_t d = sup_thr[i].cur - sup_thr[i].prev;
		const char *nm = sup_thr[i].name ? sup_thr[i].name : "?";

		len += snprintk(b + len, cap - (size_t)len, " %.6s=%u", nm,
				dt ? (unsigned int)(d * 1000u / dt) : 0u);
	}
	if (len > 0 && (size_t)len < cap) {
		len += snprintk(b + len, cap - (size_t)len,
				" idle=%u sof/s=%u blk/s=%u usbf/s=%u con=%s txq=%u fdrop=%u rx=%u last=%02x "
				"flash_ops=%u since_flash=%u bin_for=%u vbus=%d",
				dt ? (unsigned int)((sup_idle_cur - sup_idle_prev) * 1000u / dt) : 0u,
				(sup_sof_cur - sup_sof_prev) * 1000u / SUP_WIN_MS,
				(sup_blk_cur - sup_blk_prev) * 1000u / SUP_WIN_MS,
				(sup_fin_cur - sup_fin_prev) * 1000u / SUP_WIN_MS,
				conmode_binary_ok(&ctx.m) ? "bin" : (conmode_text_ok(&ctx.m) ? "text" : "closed"),
				(unsigned int)txq_used(&ctx.q), (unsigned int)ctx.frame_drops,
				(unsigned int)proto.rx_frames, proto.last_type, (unsigned int)g_flash_ops,
				g_flash_ops ? now - g_flash_end_ms : 0u,
				g_binary_since_ms ? now - g_binary_since_ms : 0u, usb_vbus() ? 1 : 0);
	}
	crashdump_stall_save(slot, b);
}

static volatile uint8_t g_sup_on;
static uint32_t sup_blocks;

/* From the audio thread once per block (I2S-clocked: keeps working even if the kernel's
 * timer or every timed wait were the thing that broke); acts every 75 blocks = 100 ms. */
static void sup_audio_tick(void)
{
	if (!g_sup_on || ++sup_blocks < SUP_MS * BLOCKS_PER_S / 1000u) {
		return;
	}
	sup_blocks = 0;
	const uint32_t now = k_uptime_get_32();
	const uint32_t age = now - g_loop_ms;

	/* keep the last complete 0.5 s window while stalled (the snapshot describes the stall) */
	if (++sup_ticks >= SUP_WIN_MS / SUP_MS) {
		sup_ticks = 0;
		sup_sample();
	}
	if (age > g_stall_max_ms) {
		g_stall_max_ms = age;
	}
	if (age >= STALL1_MS && sup_snaps == 0u) {
		sup_snapshot(0, now, age);
		sup_snaps = 1;
		g_stall_count++;
	} else if (age >= STALL2_MS && sup_snaps == 1u) {
		sup_snapshot(1, now, age);
		sup_snaps = 2;
	} else if (age < STALL1_MS && sup_snaps != 0u) {
		g_stall_resolved = sup_snaps;   /* near miss: main prints the lines, clears them */
		sup_snaps = 0;
	}
}

/* ------------------------------ power off -------------------------------- */
static FUNC_NORETURN void power_off(void)
{
	g_sup_on = 0;               /* the FN-release wait below is not a stall */
	crashdump_stall_clear();
	stage(ST_POWEROFF);
	controls_hw_leds_off();
	/* save unsaved settings first (audio is going away, an erase is fine now) */
	if (store_ok && (deb.dirty || ssched.pending)) {
		(void)do_save("power off");
	}
	con_printf("POWER OFF: release FUNCTION; next FUNCTION press wakes into the bootloader\r\n");
	g_out_enable = false;
	wait_silent();
#if CONFIG_SP1FX_SYNC
	if (g_sync_hw_init) {
		g_sync_want = 0;          /* MIDI Stop if it was running */
		for (int t = 0; t < 10 && sync_hw_running(); t++) {
			wait_fed(5);
		}
		g_sync_halt = 1;
		wait_fed(25);
		sync_hw_shutdown();
	}
#endif
	if (tas_ok || cs42_ok) {
		sp1_codecs_shutdown();
	}
	g_audio_stop = true;
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	(void)gpio_pin_set_dt(&tas_rst, 1);
	(void)gpio_pin_set_dt(&cs42_rst, 1);
	(void)gpio_pin_set_dt(&osc_en, 0);
	wait_fed(30);   /* let the console line go out (best effort) */

	while (controls_hw_fn_pressed()) {
		feed_wdt();
		k_msleep(20);
	}
	wait_fed(FN_RELEASE_MS);
	controls_hw_leds_off();
	controls_hw_rail(false);
	controls_hw_arm_fn_wake();

	(void)irq_lock();
	feed_wdt();
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) {
	}
}

/* ------------------------------ diagnostics ------------------------------ */
struct shadow {
	uint8_t fx[4], byp[4], gb, mode, edit;
	float master;
	uint32_t preset_flash;
	int8_t need[4];
	uint8_t shed, forced;
	uint16_t bpm10;
	uint8_t sync_on;
};

static void log_changes(struct shadow *sh, uint32_t now)
{
	const engine_config_t *c = &eng.cfg;

	if (ui.preset_flash_until != sh->preset_flash) {
		sh->preset_flash = ui.preset_flash_until;
		uint32_t mem = 0, cpu = 0;

		engine_resources(&eng, &mem, &cpu);
		EV_PRINTF("C %u PRESET %d/%d %s: %s %s %s %s mem=%u cpu_decl=%u bpm=%u\r\n", now,
			  ui.preset_idx + 1, FACTORY_PRESET_COUNT, preset_name(ui.preset_idx),
			  fx_name(c->fx_id[0]), fx_name(c->fx_id[1]), fx_name(c->fx_id[2]),
			  fx_name(c->fx_id[3]), (unsigned int)mem, (unsigned int)cpu,
			  (unsigned int)lrintf(ui.preset_bpm));
	}
	for (int s = 0; s < 4; s++) {
		if (c->fx_id[s] != sh->fx[s]) {
			EV_PRINTF("C %u slot %d effect %s\r\n", now, s + 1, fx_name(c->fx_id[s]));
			sh->fx[s] = c->fx_id[s];
		}
		if (c->bypass[s] != sh->byp[s]) {
			EV_PRINTF("C %u slot %d bypass=%d\r\n", now, s + 1, c->bypass[s]);
			sh->byp[s] = c->bypass[s];
		}
		/* soft-takeover state per fader (T3/T4 "blink on first touch" investigation) */
		const int8_t need = ui.to[s].picked ? 0 : ui.to[s].need;

		if (need != sh->need[s]) {
			EV_PRINTF("K %u fader %d %s\r\n", now, s + 1,
				  need > 0 ? "pickup: move UP" : need < 0 ? "pickup: move DOWN" :
				  "picked up");
			sh->need[s] = need;
		}
	}
	if (c->global_bypass != sh->gb) {
		EV_PRINTF("C %u global bypass=%d\r\n", now, c->global_bypass);
		sh->gb = c->global_bypass;
	}
	if (c->master != sh->master) {
		EV_PRINTF("C %u VOLUME master=%u/1000 = %d dB, codec input max %d dBFS\r\n", now,
			  milli(c->master), idb(vol_master_db(c->master)),
			  idb(vol_out_dbfs(c->master, (float)CONFIG_SP1FX_OUT_TRIM_DB)));
		sh->master = c->master;
	}
	if ((uint8_t)ui.mode != sh->mode || (uint8_t)ui.edit_slot != sh->edit) {
		EV_PRINTF("C %u mode %s%d\r\n", now, ui.mode == UI_EDIT ? "EDIT slot " : "PERFORM",
			  ui.mode == UI_EDIT ? ui.edit_slot + 1 : 0);
		sh->mode = (uint8_t)ui.mode;
		sh->edit = (uint8_t)ui.edit_slot;
	}
	const uint8_t sm = eng.diag.shed_mask, fb_on = eng.diag.forced_bypass;

	if (sm != sh->shed || fb_on != sh->forced) {
		if (sm & ~sh->shed) {
			ui.err_until = now + 700u;   /* refused pattern on the PLAY-side LEDs */
		}
		EV_PRINTF("OVERLOAD guard: shed_mask=0x%x forced_bypass=%u sheds=%u windows_over=%u "
			  "last_window=%u.%u%% limit=85%% preset=%d:%s\r\n", sm, fb_on,
			  (unsigned int)eng.diag.overload_sheds,
			  (unsigned int)eng.diag.overload_windows,
			  pct10(eng.diag.ovl_last_mean) / 10, pct10(eng.diag.ovl_last_mean) % 10,
			  ui.preset_idx + 1, preset_name(ui.preset_idx));
		sh->shed = sm;
		sh->forced = fb_on;
	}
	const uint16_t b10 = (uint16_t)lrintf(proto.bpm * 10.0f);

	if (b10 != sh->bpm10 || proto.sync_on != sh->sync_on) {
		EV_PRINTF("C %u sync clock %s bpm=%u.%u (companion)\r\n", now,
			  proto.sync_on ? "ON" : "off", b10 / 10u, b10 % 10u);
		sh->bpm10 = b10;
		sh->sync_on = proto.sync_on;
	}
}

struct usage {
	uint64_t audio, main, sync, total, idle;
};

static void usage_get(struct usage *u)
{
	k_thread_runtime_stats_t st;

	memset(u, 0, sizeof(*u));
	if (k_thread_runtime_stats_get(&audio_tcb, &st) == 0) {
		u->audio = st.execution_cycles;
	}
	if (k_thread_runtime_stats_get(k_current_get(), &st) == 0) {
		u->main = st.execution_cycles;
	}
#if CONFIG_SP1FX_SYNC
	if (k_thread_runtime_stats_get(&sync_tcb, &st) == 0) {
		u->sync = st.execution_cycles;
	}
#endif
	if (k_thread_runtime_stats_all_get(&st) == 0) {
		u->total = st.execution_cycles;
		u->idle = st.idle_cycles;
	}
}

static unsigned int share10(uint64_t part, uint64_t whole)
{
	return whole ? (unsigned int)((part * 1000u + whole / 2u) / whole) : 0u;
}

static void log_diag(uint32_t now, bool detail)
{
	static uint32_t prev_frames_in, prev_sof, prev_blocks;
	static struct usage pu;
	struct a_snap a = { 0 };
	const bool a_ok = snap_read(&a_seq, &a_pub, &a, sizeof(a));
	const uint32_t fin = g_usb_frames_in, sof = g_sof_count, b = g_blocks;

	con_printf("D t=%u preset=%d:%s cpu%%_avg/max=%u.%u/%u.%u blk%%_avg/max=%u.%u/%u.%u%s "
		   "ovh_max=%u spikes>85%%=%u ovl win=%u.%u%% max=%u.%u%% shed=0x%x forced=%u "
		   "vol=%d dB\r\n",
		   now / 1000u, ui.preset_idx + 1, preset_name(ui.preset_idx),
		   pct10(a.cyc_avg) / 10, pct10(a.cyc_avg) % 10, pct10(a.cyc_max) / 10,
		   pct10(a.cyc_max) % 10, pct10(a.blk_avg) / 10, pct10(a.blk_avg) % 10,
		   pct10(a.blk_max) / 10, pct10(a.blk_max) % 10, a_ok ? "" : "?",
		   (unsigned int)a.ovh_max, (unsigned int)a.spikes,
		   pct10(eng.diag.ovl_last_mean) / 10, pct10(eng.diag.ovl_last_mean) % 10,
		   pct10(eng.diag.ovl_max_mean) / 10, pct10(eng.diag.ovl_max_mean) % 10,
		   eng.diag.shed_mask, eng.diag.forced_bypass, idb(vol_master_db(eng.cfg.master)));
	con_printf("  usb out=%s play=%d fill_min/avg/max=%u/%u/%u rx_f/s=%u sof/s=%u under=%u "
		   "ovr=%u dup=%u drop=%u | in_pk=%u out_pk=%u (x1e-3) in_clip=%u out_clip=%u "
		   "sat16=%u | i2s blk/s=%u err=%u rec=%u | out=%s hp=%d quiet=%u\r\n",
		   g_out_stream ? "on" : "off", g_playing ? 1 : 0, (unsigned int)a.fill_min,
		   (unsigned int)a.fill_avg, (unsigned int)a.fill_max,
		   (unsigned int)((fin - prev_frames_in) * 1000u / CONFIG_SP1FX_DIAG_PERIOD_MS),
		   (unsigned int)((sof - prev_sof) * 1000u / CONFIG_SP1FX_DIAG_PERIOD_MS),
		   (unsigned int)out_cons.underruns, (unsigned int)g_usb_overrun,
		   (unsigned int)out_cons.dups, (unsigned int)out_cons.drops,
		   milli(a.in_pk), milli(a.out_pk), (unsigned int)eng.diag.in_clips,
		   (unsigned int)eng.diag.out_clips, (unsigned int)g_out_sat,
		   (unsigned int)((b - prev_blocks) * 1000u / CONFIG_SP1FX_DIAG_PERIOD_MS),
		   (unsigned int)g_err, (unsigned int)g_recover, out_name[cur_out], hp_raw,
		   quiet_ok(&g_quiet, QUIET_BLOCKS) ? 1u : 0u);
	prev_frames_in = fin;
	prev_sof = sof;
	prev_blocks = b;

	struct usage u;

	usage_get(&u);
	const uint64_t dt = u.total - pu.total;

	con_printf("  threads%% audio=%u.%u main=%u.%u sync=%u.%u other=%u.%u idle=%u.%u "
		   "(ISR time counts to the thread it interrupted)\r\n",
		   share10(u.audio - pu.audio, dt) / 10, share10(u.audio - pu.audio, dt) % 10,
		   share10(u.main - pu.main, dt) / 10, share10(u.main - pu.main, dt) % 10,
		   share10(u.sync - pu.sync, dt) / 10, share10(u.sync - pu.sync, dt) % 10,
		   share10(dt - (u.audio - pu.audio) - (u.main - pu.main) - (u.sync - pu.sync) -
			   (u.idle - pu.idle), dt) / 10,
		   share10(dt - (u.audio - pu.audio) - (u.main - pu.main) - (u.sync - pu.sync) -
			   (u.idle - pu.idle), dt) % 10,
		   share10(u.idle - pu.idle, dt) / 10, share10(u.idle - pu.idle, dt) % 10);
	pu = u;
	if (!detail) {
		return;
	}
	unsigned int a_sz = 0, m_sz = 0, s_sz = 0, s_used = 0;
	const unsigned int a_used = stack_used(&audio_tcb, &a_sz);
	const unsigned int m_used = stack_used(k_current_get(), &m_sz);
#if CONFIG_SP1FX_SYNC
	s_used = stack_used(&sync_tcb, &s_sz);
#endif
	con_printf("  info fw=%s stk audio=%u/%u main=%u/%u sync=%u/%u apply_cyc_max=%u "
		   "blk_cyc_max=%u starve=%u torn=%u lay_err=%u boots=%u rr=%x hw=%d adc=%d "
		   "pwm_rel=0x%x cs42=%s tas=%s usb=%d trim=%d dB stalls=%u stall_max_ms=%u "
		   "vbus=%d save_busy=%u cdc_tx=%u cdc_rx=%u txq_max=%u\r\n",
		   FW_VERSION, a_used, a_sz, m_used, m_sz, s_used, s_sz,
		   (unsigned int)g_apply_max, (unsigned int)g_blk_max_all, (unsigned int)g_starve,
		   (unsigned int)eng.diag.torn_retries, (unsigned int)eng.diag.layout_errors,
		   (unsigned int)crashdump_boots(), (unsigned int)sp1fx_boot_resetreas, hw_rc,
		   controls_hw_adc_status(), (unsigned int)controls_hw_released_pwms(),
		   cs42_cfg_ok ? "ok" : (cs42_ok ? "cfg-fail" : "absent"),
		   tas_cfg_ok ? "ok" : (tas_ok ? "cfg-fail" : "absent"), usb_rc,
		   CONFIG_SP1FX_OUT_TRIM_DB, (unsigned int)g_stall_count,
		   (unsigned int)g_stall_max_ms, usb_vbus() ? 1 : 0, (unsigned int)st_busy,
		   (unsigned int)cdc_tx_bytes, (unsigned int)cdc_rx_bytes,
		   (unsigned int)ctx.q.max_used);
	con_printf("  store ok=%d load=%d seq=%u page=%d saves=%u erases=%u errors=%u "
		   "write_ms_max=%u erase_ms_max=%u deferred=%u dirty=%d pending=%d dedup=%u safe=%d "
		   "adc_to=%u adc_max_ms=%u | "
		   "sync on=%u hw=%u ticks=%u pulses=%u starts=%u | con drops=%u frames_drop=%u "
		   "rx_drop=%u ev_drop=%u rx_frames=%u bad=%u\r\n",
		   store_ok ? 1 : 0, st_load_rc, (unsigned int)store.seq, store.active_page,
		   (unsigned int)st_saves, (unsigned int)st_erases, (unsigned int)st_errors,
		   (unsigned int)st_write_ms_max, (unsigned int)st_erase_ms_max,
		   (unsigned int)ssched.deferred, deb.dirty, ssched.pending, (unsigned int)st_dedup,
		   safe_boot ? 1 : 0, (unsigned int)controls_hw_adc_timeouts(),
		   (unsigned int)controls_hw_adc_max_ms(), proto.sync_on,
		   g_sync_hw_init, (unsigned int)g_sync_ticks, (unsigned int)g_sync_pulses,
		   (unsigned int)g_sync_starts, (unsigned int)ctx.text_drops,
		   (unsigned int)ctx.frame_drops, (unsigned int)rx_drops, (unsigned int)ev_rl.dropped,
		   (unsigned int)proto.rx_frames, (unsigned int)proto.rx_dropped);
}

/* --------------------------------- main ---------------------------------- */
int main(void)
{
	sp1fx_boot_resetreas = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	feed_wdt();
	(void)crashdump_init();
	crashdump_set_crumbs(g_crumbs);

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

	/* controls: rail on, LEDs dark (PWM0/PWM1), SAADC channels */
	hw_rc = controls_hw_init();
	feed_wdt();

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
	ua_ramp_init(&in_ramp, (float)FS_HZ, CONFIG_SP1FX_UA_FADE_IN_MS / 1000.0f,
		     CONFIG_SP1FX_UA_FADE_OUT_MS / 1000.0f);
	ua_ramp_init(&out_ramp, (float)FS_HZ, OUT_FADE_IN_S, OUT_FADE_OUT_S);
	trim_gain = powf(10.0f, (float)CONFIG_SP1FX_OUT_TRIM_DB / 20.0f);
	engine_init(&eng, engine_pool, sizeof(engine_pool), (float)FS_HZ,
		    ENGINE_DEFAULT_CPU_BUDGET);
	eng.init_budget = ENGINE_INIT_BUDGET;   /* before the audio thread exists */

	/* saved settings (or defaults: all slots bypassed = clean pass-through, master 75 %) */
	static store_data_t boot_d;
	const flash_ops_t ops = { NULL, fl_read, fl_write, fl_erase };

	store_ok = device_is_ready(flash_dev);
	safe_boot = crashdump_early_crashes() >= 2u;
	if (store_ok && !safe_boot) {
		(void)store_init(&store, ops);
		st_load_rc = store_load(&store, &boot_d);
	} else {
		store_data_default(&boot_d);   /* safe boot: factory defaults, journal untouched */
	}
	/* the loaded state counts as saved: no write at boot unless something really changes */
	st_last_len = store_encode(&boot_d, st_last_rec, sizeof(st_last_rec));
	st_last_save_ms = k_uptime_get_32();
	feed_wdt();
	const int boot_rejected = engine_apply_config(&eng, &boot_d.cfg);

	store_debounce_init(&deb);
	save_sched_init(&ssched);

	controller_init(&ui, &eng);
	controller_set_presets(&ui, FACTORY_PRESET_COUNT, fill_factory, NULL);
	ui.preset_keep_master = 1;   /* FN + rocker never changes the volume */

	ladder_cfg_t c1, c2;

	ladder_cfg_ladder1(&c1, LADDER1_DECODE_PLAY_CHORDS);
	ladder_cfg_ladder2(&c2);
	ladder_init(&lad1, &c1);
	ladder_init(&lad2, &c2);
	for (int i = 0; i < 4; i++) {
		fader_init(&fad[i], FADER_RAW_LO_DEFAULT, FADER_RAW_HI_DEFAULT);
	}

	const proto_config_t pc = {
		.eng = &eng, .ctl = &ui, .sync = NULL, .deb = &deb, .tx = proto_tx,
		.save = proto_save_now, .ctx = NULL, .fw = FW_VERSION, .block = BLK_FRAMES,
		.bpm = boot_d.bpm, .sync_on = boot_d.sync_on,
	};

	proto_init(&proto, &pc);
	contx_init(&ctx, con_buf, sizeof(con_buf));
	ratelimit_init(&ev_rl, 10, 20, k_uptime_get_32());
	g_sync_bpm10 = (uint16_t)lrintf(proto.bpm * 10.0f);
	g_sync_want = proto.sync_on;

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
#if CONFIG_SP1FX_SYNC
	k_thread_create(&sync_tcb, sync_stack, K_THREAD_STACK_SIZEOF(sync_stack),
			sync_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(1), K_FP_REGS,
			K_NO_WAIT);
	k_thread_name_set(&sync_tcb, "sync");
#endif
	g_main_tid = k_current_get();
	g_loop_ms = k_uptime_get_32();
	k_thread_foreach(sup_add_thread, NULL);
	g_sup_on = 1;
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

	/* If FUNCTION is still held from the wake-up press, hide it from the controller until
	 * it has been released once, so that press cannot become a 1.5 s power-off hold. */
	bool fn_armed = !controls_hw_fn_pressed();
	int64_t next = k_uptime_get();
	uint32_t now = k_uptime_get_32();
	uint32_t next_hp = now + HP_POLL_MS;
	uint32_t next_diag = now + CONFIG_SP1FX_DIAG_PERIOD_MS;
	uint32_t diag_n = 0;
	uint32_t seen_change = ui.change_seq;
	bool was_text = false;
	int hp_cand = -1, hp_cnt = 0;
	controls_raw_t raw;
	float fv[4] = { 0 };
	uint16_t lad_mask = 0;
	bool stable_marked = false;
	struct shadow sh;

	memset(&sh, 0, sizeof(sh));
	for (int s = 0; s < 4; s++) {
		sh.fx[s] = eng.cfg.fx_id[s];
		sh.byp[s] = eng.cfg.bypass[s];
	}
	sh.gb = eng.cfg.global_bypass;
	sh.master = eng.cfg.master;
	sh.bpm10 = (uint16_t)lrintf(proto.bpm * 10.0f);
	sh.sync_on = proto.sync_on;
	sh.preset_flash = ui.preset_flash_until;

	for (;;) {
		feed_wdt();
		now = k_uptime_get_32();
		g_loop_ms = now;
		g_crumbs[1] = now;
		if (!stable_marked && now >= CRASHDUMP_EARLY_MS) {
			crashdump_mark_stable();   /* this run did not crash early */
			stable_marked = true;
		}
		stage(ST_CONSOLE);
		if (g_stall_resolved) {
			EV_PRINTF("STALL near miss: control loop stopped for %u ms (watchdog at ~4000)\r\n",
				  (unsigned int)g_stall_max_ms);
			for (int i = 0; i < (int)g_stall_resolved && i < CRASHDUMP_STALL_SLOTS; i++) {
				EV_PRINTF("STALL[%d]: %s\r\n", i, sup_line[i]);
			}
			g_stall_resolved = 0;
			crashdump_stall_clear();
		}

		/* ---- port mode, banner when a terminal opens the port ---- */
		conm_poll();
		if (conmode_text_ok(&conm) && !was_text) {
			con_printf("I SP-1 FX %s: USB audio + effects + controls + companion protocol. "
				   "Text until the companion sends its first frame. resetreas=0x%x "
				   "boot_rejected=%d store_load=%d (0 = saved settings) sync=%s bpm=%u "
				   "trim=%d dB%s\r\n", FW_VERSION, (unsigned int)sp1fx_boot_resetreas,
				   boot_rejected, st_load_rc, proto.sync_on ? "ON" : "off",
				   (unsigned int)lrintf(proto.bpm), CONFIG_SP1FX_OUT_TRIM_DB,
				   safe_boot ? " SAFE BOOT: 2+ early crashes in a row -> factory defaults, "
					       "saved settings not loaded and not written this run" : "");
		}
		was_text = conmode_text_ok(&conm);
		crashdump_poll(now, conmode_text_ok(&conm), con_write);

		/* ---- controls ---- */
		stage(ST_ADC);
		/* never waits for the SAADC (rc1 used the blocking adc_read: K_FOREVER) */
		if (controls_hw_read_nb(&raw, now, ADC_TIMEOUT_MS) == 0) {
			lad_mask = (uint16_t)(ladder_update(&lad1, raw.ladder1) |
					      ladder_update(&lad2, raw.ladder2));
			for (int i = 0; i < 4; i++) {
				fv[i] = fader_update(&fad[i], raw.fader[i]);
			}
		}
		g_crumbs[3] = (controls_hw_adc_stuck() ? 2u : 0u) |
			      (controls_hw_adc_timeouts() << 8);
		stage(ST_CONTROLLER);
		uint16_t buttons = lad_mask;
		const bool fnp = controls_hw_fn_pressed();

		if (!fnp) {
			fn_armed = true;
		}
		if (fnp && fn_armed) {
			buttons |= BTN_BIT(BTN_FN);
		}
		controller_tick(&ui, now, buttons, fv);
		if (ui.power_off_request) {
			power_off();
		}
		if (ui.preset_flash_until != sh.preset_flash) {
			/* a preset was loaded: the guard gives it a fresh chance */
			engine_overload_clear(&eng);
			eng.diag.cpu_peak_cycles = 0;
		}

		/* ---- companion protocol (also drains the controller's events) ---- */
		stage(ST_PROTO_RX);
		rx_drain();
		conm_poll();
		stage(ST_PROTO_TICK);
		proto_tick(&proto, now);
		g_sync_bpm10 = (uint16_t)lrintf(proto.bpm * 10.0f);
		g_sync_want = proto.sync_on;

		/* ---- persistence: device-side changes (host-side ones mark deb in proto) ---- */
		if (ui.change_seq != seen_change) {
			seen_change = ui.change_seq;
			store_debounce_mark(&deb, now);
		}
		stage(ST_STORE);
		storage_tick(now);
		stage(ST_LEDS);

		/* ---- LEDs ---- */
		led_frame_t f;

		controller_leds(&ui, now, &f);
		controls_hw_leds(f.track, f.play);
		/* output-level LEDs read diag.out_peak: let it decay (half-life ~110 ms) */
		eng.diag.out_peak *= 0.95f;
		eng.diag.in_peak *= 0.95f;

		/* ---- headphone detect / output switch ---- */
		if ((int32_t)(now - next_hp) >= 0) {
			next_hp = now + HP_POLL_MS;
			stage(ST_HP_I2C);
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
				EV_PRINTF("C %u output -> %s\r\n", now, out_name[cur_out]);
			}
		}

		/* ---- text diagnostics ---- */
		stage(ST_DIAG);
		log_changes(&sh, now);
		if ((int32_t)(now - next_diag) >= 0) {
			next_diag = now + CONFIG_SP1FX_DIAG_PERIOD_MS;
			if (conmode_text_ok(&conm)) {
				log_diag(now, (diag_n % DETAIL_EVERY) == 0u);
			}
			diag_n++;
		}

		stage(ST_SLEEP);
		next += TICK_MS;
		const int64_t t = k_uptime_get();

		if (next <= t) {
			next = t + 1;   /* overran: do not try to catch up */
		}
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
	return 0;
}
