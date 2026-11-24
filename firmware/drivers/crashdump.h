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

#endif
