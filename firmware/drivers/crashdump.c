/* SP-1 FX crash dump (Zephyr only). See crashdump.h. */
#include "crashdump.h"

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/printk.h>
#include <cmsis_core.h>
#include <string.h>

#define REC_MAGIC  0x43524153u /* "CRAS" */
#define BOOT_MAGIC 0x424f4f54u /* "BOOT" */

enum { KIND_FATAL = 1, KIND_WDT = 2 };

struct crash_rec {
	uint32_t magic;
	uint32_t count;            /* crashes since the record was last cleared */
	uint32_t kind, reason;
	uint32_t pc, lr, xpsr;     /* stacked by the exception (xPSR[8:0] = active exception, 0 = thread) */
	uint32_t cfsr, hfsr, mmfar, bfar;
	uint32_t uptime_ms;
	uint32_t tid, stk_size, stk_unused;
	uint32_t in_isr;           /* watchdog: 1 = an interrupt was running, PC/LR unknown */
	uint32_t crumb[CRASHDUMP_CRUMBS];
	char thread[16];
	uint32_t crc;              /* CRC-32 of everything above */
};

/* Retained across resets that keep RAM: .noinit is NOLOAD and outside .bss, so neither the
 * C runtime nor the kernel clears it (checked in the linker map, docs/BRINGUP_USBAUDIO.md). */
struct stall_rec {
	uint32_t magic;
	char line[CRASHDUMP_STALL_LEN];
	uint32_t crc;
};

static __attribute__((section(".noinit.sp1fx_crashdump"))) struct {
	struct crash_rec rec;
	uint32_t boot_magic, boots, boot_check;
	struct stall_rec stall[CRASHDUMP_STALL_SLOTS];
	uint32_t early, early_check, last_count;
} ret;

static const volatile uint32_t *crumbs;

void crashdump_set_crumbs(const volatile uint32_t *words)
{
	crumbs = words;
}

static void rec_crumbs(void)
{
	for (int i = 0; i < CRASHDUMP_CRUMBS; i++) {
		ret.rec.crumb[i] = crumbs ? crumbs[i] : 0u;
	}
}

/* previous run's stall lines, copied at init (the retained ones are reused by this run) */
static char stall_prev[CRASHDUMP_STALL_SLOTS][CRASHDUMP_STALL_LEN];
static bool stall_any;

static uint32_t crc_tab[256];
static bool crc_ready;
static bool pending;

static uint32_t crc32(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint32_t c = 0xFFFFFFFFu;

	while (n--) {
		if (crc_ready) {
			c = crc_tab[(c ^ *b++) & 0xFFu] ^ (c >> 8);
		} else {
			c ^= *b++;
			for (int k = 0; k < 8; k++) {
				c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
			}
		}
	}
	return ~c;
}

static bool rec_valid(void)
{
	return ret.rec.magic == REC_MAGIC &&
	       ret.rec.crc == crc32(&ret.rec, offsetof(struct crash_rec, crc));
}

static void rec_seal(void)
{
	/* a reset before the CRC is stored leaves a record that fails the check */
	ret.rec.magic = REC_MAGIC;
	ret.rec.crc = crc32(&ret.rec, offsetof(struct crash_rec, crc));
	__DSB();
}

static uint32_t next_count(void)
{
	/* informative only: no CRC check here (the watchdog path has ~61 us) */
	uint32_t c = (ret.rec.magic == REC_MAGIC) ? ret.rec.count + 1u : 1u;

	return (c == 0u || c > 9999u) ? 1u : c;
}

static void rec_thread(k_tid_t t)
{
	const char *n = NULL;

	ret.rec.tid = (uint32_t)(uintptr_t)t;
#if defined(CONFIG_THREAD_NAME)
	if (t != NULL) {
		n = k_thread_name_get(t);
	}
#endif
	if (n == NULL || n[0] == '\0') {
		n = "?";
	}
	size_t i = 0;

	for (; i < sizeof(ret.rec.thread) - 1u && n[i] != '\0'; i++) {
		ret.rec.thread[i] = n[i];
	}
	for (; i < sizeof(ret.rec.thread); i++) {
		ret.rec.thread[i] = '\0';
	}
}

/* ---- fault registers, snapshotted before Zephyr's fault decoder clears CFSR */
static volatile uint32_t snap_valid, snap_cfsr, snap_hfsr, snap_mmfar, snap_bfar;

void __real_z_arm_fault(uint32_t msp, uint32_t psp, uint32_t exc_return, void *callee_regs);
void __wrap_z_arm_fault(uint32_t msp, uint32_t psp, uint32_t exc_return, void *callee_regs);

void __wrap_z_arm_fault(uint32_t msp, uint32_t psp, uint32_t exc_return, void *callee_regs)
{
	snap_cfsr = SCB->CFSR;
	snap_hfsr = SCB->HFSR;
	snap_mmfar = SCB->MMFAR;
	snap_bfar = SCB->BFAR;
	snap_valid = 1u;
	__real_z_arm_fault(msp, psp, exc_return, callee_regs);
}

/* Overrides the kernel's weak handler: record, then reset at once (no halt, no 4 s of noise
 * until the watchdog). Runs with interrupts locked. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	(void)arch_irq_lock();
	const uint32_t count = next_count();
	k_tid_t t = k_current_get();

	ret.rec.magic = 0u;
	ret.rec.count = count;
	ret.rec.kind = KIND_FATAL;
	ret.rec.reason = reason;
	ret.rec.pc = esf ? esf->basic.pc : 0u;
	ret.rec.lr = esf ? esf->basic.lr : 0u;
	ret.rec.xpsr = esf ? esf->basic.xpsr : 0u;
	ret.rec.cfsr = snap_valid ? snap_cfsr : SCB->CFSR;
	ret.rec.hfsr = snap_valid ? snap_hfsr : SCB->HFSR;
	ret.rec.mmfar = snap_valid ? snap_mmfar : SCB->MMFAR;
	ret.rec.bfar = snap_valid ? snap_bfar : SCB->BFAR;
	ret.rec.uptime_ms = k_uptime_get_32();
	ret.rec.in_isr = 0u;
	rec_crumbs();
	ret.rec.stk_size = 0u;
	ret.rec.stk_unused = 0u;
	rec_thread(t);
#if defined(CONFIG_THREAD_STACK_INFO)
	if (t != NULL) {
		ret.rec.stk_size = (uint32_t)t->stack_info.size;
#if defined(CONFIG_INIT_STACKS)
		size_t unused = 0;

		if (reason != K_ERR_STACK_CHK_FAIL && k_thread_stack_space_get(t, &unused) == 0) {
			ret.rec.stk_unused = (uint32_t)unused;
		}
#endif
	}
#endif
	rec_seal();
	NVIC_SystemReset();
	CODE_UNREACHABLE;
}

void crashdump_wdt_capture(void)
{
	/* SCB->ICSR.RETTOBASE: no other exception active -> the WDT interrupt pre-empted a
	 * thread, whose basic exception frame is at PSP (r0 r1 r2 r3 r12 lr pc xpsr). */
	const bool from_thread = (SCB->ICSR & SCB_ICSR_RETTOBASE_Msk) != 0u;
	const uint32_t count = next_count();

	ret.rec.magic = 0u;
	ret.rec.count = count;
	ret.rec.kind = KIND_WDT;
	ret.rec.reason = 0u;
	if (from_thread) {
		const uint32_t *f = (const uint32_t *)__get_PSP();

		ret.rec.lr = f[5];
		ret.rec.pc = f[6];
		ret.rec.xpsr = f[7];
	} else {
		ret.rec.lr = ret.rec.pc = ret.rec.xpsr = 0u;
	}
	ret.rec.in_isr = from_thread ? 0u : 1u;
	ret.rec.cfsr = ret.rec.hfsr = ret.rec.mmfar = ret.rec.bfar = 0u;
	ret.rec.uptime_ms = k_uptime_get_32();
	ret.rec.stk_size = ret.rec.stk_unused = 0u; /* no time to scan a stack here */
	rec_crumbs();
	rec_thread(k_current_get());
	rec_seal();
}

bool crashdump_init(void)
{
	for (uint32_t i = 0; i < 256u; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
		}
		crc_tab[i] = c;
	}
	crc_ready = true;

	if (ret.boot_magic == BOOT_MAGIC && ret.boot_check == ~ret.boots) {
		ret.boots++;
	} else {
		ret.boot_magic = BOOT_MAGIC;
		ret.boots = 1u;
	}
	ret.boot_check = ~ret.boots;

	pending = rec_valid();
	if (!pending) {
		ret.rec.magic = 0u;
	}
	/* crash-loop breaker: count boots that follow an early crash (a new record) */
	if (ret.early_check != ~ret.early) {
		ret.early = 0u;
		ret.last_count = 0u;
	}
	if (pending && ret.rec.count != ret.last_count) {
		ret.early = (ret.rec.uptime_ms < CRASHDUMP_EARLY_MS) ? ret.early + 1u : 0u;
		ret.last_count = ret.rec.count;
	} else if (!pending) {
		ret.early = 0u;
	}
	ret.early_check = ~ret.early;
	for (int i = 0; i < CRASHDUMP_STALL_SLOTS; i++) {
		struct stall_rec *st = &ret.stall[i];

		stall_prev[i][0] = '\0';
		if (st->magic == REC_MAGIC &&
		    st->crc == crc32(st, offsetof(struct stall_rec, crc))) {
			memcpy(stall_prev[i], st->line, CRASHDUMP_STALL_LEN);
			stall_prev[i][CRASHDUMP_STALL_LEN - 1] = '\0';
			stall_any = true;
		}
		st->magic = 0u;
	}
	return pending;
}

void crashdump_stall_save(int slot, const char *line)
{
	if (slot < 0 || slot >= CRASHDUMP_STALL_SLOTS) {
		return;
	}
	struct stall_rec *st = &ret.stall[slot];
	size_t i = 0;

	st->magic = 0u;
	for (; i < CRASHDUMP_STALL_LEN - 1u && line[i] != '\0'; i++) {
		st->line[i] = line[i];
	}
	for (; i < CRASHDUMP_STALL_LEN; i++) {
		st->line[i] = '\0';
	}
	st->magic = REC_MAGIC;
	st->crc = crc32(st, offsetof(struct stall_rec, crc));
	__DSB();
}

void crashdump_stall_clear(void)
{
	for (int i = 0; i < CRASHDUMP_STALL_SLOTS; i++) {
		ret.stall[i].magic = 0u;
	}
}

const char *crashdump_stall_prev(int slot)
{
	return (slot >= 0 && slot < CRASHDUMP_STALL_SLOTS) ? stall_prev[slot] : "";
}

uint32_t crashdump_early_crashes(void)
{
	return ret.early;
}

void crashdump_mark_stable(void)
{
	ret.early = 0u;
	ret.early_check = ~0u;
}

uint32_t crashdump_boots(void)
{
	return ret.boots;
}

static const char *reason_name(const struct crash_rec *r)
{
	if (r->kind == KIND_WDT) {
		return "watchdog_hang";
	}
	switch (r->reason) {
	case K_ERR_CPU_EXCEPTION:   return "cpu_exception";
	case K_ERR_SPURIOUS_IRQ:    return "spurious_irq";
	case K_ERR_STACK_CHK_FAIL:  return "stack_overflow";
	case K_ERR_KERNEL_OOPS:     return "oops";
	case K_ERR_KERNEL_PANIC:    return "panic";
	default:                    return "arch";
	}
}

int crashdump_format(char *buf, size_t n)
{
	if (!pending || n == 0u) {
		return 0;
	}
	const struct crash_rec *r = &ret.rec;
	int len = snprintk(buf, n,
			   "CRASH: #%u %s reason=%u thread=%s(%08x)%s pc=%08x lr=%08x xpsr=%08x "
			   "cfsr=%08x hfsr=%08x mmfar=%08x bfar=%08x stack_unused=%u/%u "
			   "up=%u ms boots=%u crumbs=stage:%u loop_ms:%u stage_ms:%u adc:0x%x\r\n",
			   (unsigned int)r->count, reason_name(r), (unsigned int)r->reason,
			   r->thread, (unsigned int)r->tid, r->in_isr ? " in_isr" : "",
			   (unsigned int)r->pc, (unsigned int)r->lr, (unsigned int)r->xpsr,
			   (unsigned int)r->cfsr, (unsigned int)r->hfsr, (unsigned int)r->mmfar,
			   (unsigned int)r->bfar, (unsigned int)r->stk_unused,
			   (unsigned int)r->stk_size, (unsigned int)r->uptime_ms,
			   (unsigned int)ret.boots, (unsigned int)r->crumb[0], (unsigned int)r->crumb[1],
			   (unsigned int)r->crumb[2], (unsigned int)r->crumb[3]);

	if (len < 0) {
		return 0;
	}
	return (len >= (int)n) ? (int)n - 1 : len;
}

void crashdump_poll(int64_t now_ms, bool console_open, void (*out)(const char *s, int len))
{
	static int64_t next_ms;
	static int prints;
	char b[360];

	if ((!pending && !stall_any) || !console_open || (prints > 0 && now_ms < next_ms)) {
		return;
	}
	const int len = crashdump_format(b, sizeof(b));

	if (len > 0) {
		out(b, len);
	}
	for (int i = 0; i < CRASHDUMP_STALL_SLOTS; i++) {
		if (stall_prev[i][0] != '\0') {
			int n = snprintk(b, sizeof(b), "STALL[%d] prev run: %s\r\n", i, stall_prev[i]);

			if (n > 0) {
				out(b, n >= (int)sizeof(b) ? (int)sizeof(b) - 1 : n);
			}
		}
	}
	next_ms = now_ms + CRASHDUMP_PERIOD_MS;
	if (++prints >= CRASHDUMP_PRINTS) {
		ret.rec.magic = 0u; /* seen for a minute: clear */
		pending = false;
		stall_any = false;
	}
}
