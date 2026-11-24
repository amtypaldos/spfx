/* SP-1 FX crash dump (Zephyr only): make the next crash explain itself without a debugger.
 *
 *   - A fatal error (HardFault/MemManage/BusFault/UsageFault, MPU stack-guard overflow,
 *     k_panic/k_oops, kernel assert) goes through k_sys_fatal_error_handler(), which this
 *     module overrides: it records reason, PC/LR/xPSR, CFSR/HFSR/MMFAR/BFAR (snapshotted
 *     before Zephyr clears them, via the linker wrap of z_arm_fault), the current thread's
 *     name and stack high-water mark into a retained (__noinit) record with magic + CRC-32,
 *     and resets the chip at once (NVIC_SystemReset) instead of halting until the watchdog.
 *   - A hang that trips the hardware watchdog: crashdump_wdt_capture(), called from the
 *     watchdog's TIMEOUT interrupt (the nRF52 WDT gives ~61 us before it resets), records
 *     the interrupted thread and its PC/LR.
 *   - At the next boot crashdump_init() validates the record; crashdump_poll() prints a
 *     one-line "CRASH: ..." report every 10 s on an open console and clears the record once
 *     it has been printed CRASHDUMP_PRINTS times (one minute of an open console).
 *
 * Needs: CONFIG_THREAD_NAME, CONFIG_THREAD_STACK_INFO, CONFIG_INIT_STACKS (stack high-water)
 * and the link option -Wl,--wrap=z_arm_fault (see firmware/app/zephyr_usbaudio/sources.cmake).
 * RAM survives a soft reset on the nRF52840; whether it survives a watchdog reset and the
 * SP-1 bootloader is UNVERIFIED on hardware: the magic + CRC make a lost record show as
 * "no report", never as a false one. crashdump_boots() (retained boot counter) tells. */
#ifndef SP1FX_CRASHDUMP_H
#define SP1FX_CRASHDUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRASHDUMP_PRINTS     6       /* clear after this many delivered prints */
#define CRASHDUMP_PERIOD_MS  10000

/* Call first thing in main(). Returns true if a valid crash record is pending. */
bool crashdump_init(void);

/* Retained boot counter: 1 after a power-on (or when RAM was lost), +1 per reset that kept RAM. */
uint32_t crashdump_boots(void);

/* Format the pending report ("CRASH: ...\r\n"); returns its length, 0 if none. */
int crashdump_format(char *buf, size_t n);

/* Call from the main loop. When a report is pending and the console is open, emits it via
 * out() every CRASHDUMP_PERIOD_MS (first time at once) and clears it after CRASHDUMP_PRINTS. */
void crashdump_poll(int64_t now_ms, bool console_open, void (*out)(const char *s, int len));

/* From the watchdog TIMEOUT callback (interrupt context, must be fast). */
void crashdump_wdt_capture(void);

/* Breadcrumbs: the app points this at CRASHDUMP_CRUMBS words it keeps current (zephyr_sp1fx:
 * control-loop stage, uptime when the loop last ran, uptime when the stage was entered,
 * SAADC state; compare with up= on the CRASH line). They are copied into the record by the watchdog capture and the fatal
 * handler and printed as crumbs=... on the CRASH line. */
#define CRASHDUMP_CRUMBS 4
void crashdump_set_crumbs(const volatile uint32_t *words);

/* Crash-loop breaker: number of consecutive boots that followed a crash (watchdog or fatal)
 * which happened less than CRASHDUMP_EARLY_MS after its own boot. 0 after a clean run;
 * crashdump_mark_stable() (call once the run has lasted CRASHDUMP_EARLY_MS) resets it. */
#define CRASHDUMP_EARLY_MS 60000u
uint32_t crashdump_early_crashes(void);
void crashdump_mark_stable(void);

/* Retained "powered off while USB was plugged in" flag: the app sets it and soft-resets into
 * its charge standby (System OFF with VBUS already present would have no wake edge). Taking
 * it clears it. */
void crashdump_set_poweroff_flag(void);
int crashdump_take_poweroff_flag(void);
/* 1 if crashdump_init found a valid crash record from the previous run */
int crashdump_pending(void);

/* Stall snapshots (zephyr_sp1fx): a supervisor (k_timer, interrupt context) that sees the
 * control thread stop making progress formats up to CRASHDUMP_STALL_SLOTS one-line
 * descriptions (control-loop stage, its state/PC/backtrace, per-thread CPU shares, USB/CDC
 * counters) into retained RAM with a CRC. They are printed as "STALL[n]: ..." lines after
 * the next boot (with the CRASH line) and cleared with it. Interrupt-safe. */
#define CRASHDUMP_STALL_SLOTS 2
#define CRASHDUMP_STALL_LEN   300
void crashdump_stall_save(int slot, const char *line);
void crashdump_stall_clear(void);
/* the text of a slot from the previous run ("" if none) */
const char *crashdump_stall_prev(int slot);

#endif
