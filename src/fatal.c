/*
 * Fatal-error handling, crash records and the hardware watchdog.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fatal.h"

#include <zephyr/arch/cpu.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/printk.h>

#include "hal/hal.h"

LOG_MODULE_REGISTER(impulse_fatal, LOG_LEVEL_INF);

#if defined(CONFIG_RESET_ON_FATAL_ERROR)
/* NCS's lib/fatal_error defines k_sys_fatal_error_handler too, as a bare
 * reboot with no way to switch outputs off first. The two cannot coexist. */
#error "CONFIG_RESET_ON_FATAL_ERROR replaces this handler; leave it off"
#endif

extern void sys_arch_reboot(int type);

/* ---- crash record ------------------------------------------------------ */

/*
 * Retained across a warm reset (.noinit is never zeroed at startup), lost on
 * power loss. A cold boot leaves random RAM, which the magic rejects.
 */
#define CRASH_MAGIC 0x494D5043U /* "IMPC" */

struct crash_record {
	uint32_t magic;
	uint32_t count;   /* crashes since the last cold boot */
	uint32_t pending; /* not yet reported */
	uint32_t reason;
	uint32_t pc;
	uint32_t lr;
	uint32_t uptime_ms;
};

static struct crash_record crash __noinit;

/*
 * Replaces Zephyr's weak default, which halts forever.
 *
 * Observed 2026-09-12: the watch halted on a SoftDevice Controller assert
 * (23, 587) mid channel sounding. Interrupts locked, tick stopped, every
 * thread frozen — and the vibration motor latched ON, because a halted CPU
 * simply leaves the last value on the pad. It ran until a human pressed reset,
 * and in the product that means until the battery died.
 */
void k_sys_fatal_error_handler(unsigned int reason,
			       const struct arch_esf *esf)
{
	/* OUTPUTS FIRST, before anything that could fault again. A GPIO write
	 * here is a register write with no locks; nothing below is as safe. */
	impulse_motor_set(false);
	impulse_buzzer_set(false);

	crash.count = (crash.magic == CRASH_MAGIC) ? crash.count + 1U : 1U;
	crash.magic = CRASH_MAGIC;
	crash.pending = 1U;
	crash.reason = reason;
	crash.pc = (esf != NULL) ? esf->basic.pc : 0U;
	crash.lr = (esf != NULL) ? esf->basic.lr : 0U;
	crash.uptime_ms = (uint32_t)k_uptime_get();

	/* Deferred logging never flushed on 2026-09-12 — the assert text was
	 * only recovered by reading the RTT buffer out of RAM. Flush now. */
	LOG_PANIC();

#if defined(CONFIG_IMPULSE_HALT_ON_FATAL)
	LOG_ERR("fatal error %u (pc 0x%08x lr 0x%08x) — HALTING, bench build",
		reason, crash.pc, crash.lr);
	k_fatal_halt(reason);
#else
	LOG_ERR("fatal error %u (pc 0x%08x lr 0x%08x) — rebooting", reason,
		crash.pc, crash.lr);
	sys_arch_reboot(0);
#endif
	CODE_UNREACHABLE;
}

/* ---- boot report ------------------------------------------------------- */

static const char *const reset_cause_names[] = {
	"pin", "software", "brownout", "power-on", "watchdog", "debug",
	"security", "low-power-wake", "cpu-lockup", "parity", "pll", "clock",
	"hardware", "user", "temperature", "bootloader", "flash",
};

void impulse_fatal_report_boot(void)
{
	uint32_t cause = 0U;
	int err = hwinfo_get_reset_cause(&cause);

	if (err == 0) {
		char names[128];
		size_t used = 0U;

		names[0] = '\0';
		for (size_t i = 0U; i < ARRAY_SIZE(reset_cause_names); i++) {
			if ((cause & BIT(i)) == 0U) {
				continue;
			}
			int n = snprintk(names + used, sizeof(names) - used,
					 "%s%s", used ? "+" : "",
					 reset_cause_names[i]);
			if (n < 0 || (size_t)n >= sizeof(names) - used) {
				break;
			}
			used += (size_t)n;
		}
		LOG_INF("reset cause: 0x%08x (%s)", cause,
			used ? names : "none reported");
		(void)hwinfo_clear_reset_cause();
	} else {
		LOG_WRN("reset cause unavailable (%d)", err);
	}

	if (crash.magic == CRASH_MAGIC && crash.pending != 0U) {
		LOG_ERR("PREVIOUS BOOT CRASHED: reason %u, pc 0x%08x, lr 0x%08x, "
			"at uptime %u ms (crash #%u since cold boot)",
			crash.reason, crash.pc, crash.lr, crash.uptime_ms,
			crash.count);
		crash.pending = 0U;
	}
}

/* ---- hardware watchdog ------------------------------------------------- */

#if defined(CONFIG_IMPULSE_HW_WATCHDOG)
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

void impulse_hw_watchdog_start(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window = {
			.min = 0U,
			.max = CONFIG_IMPULSE_HW_WATCHDOG_TIMEOUT_S * 1000U,
		},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int err;

	if (!device_is_ready(wdt)) {
		LOG_ERR("hardware watchdog not ready");
		return;
	}

	err = wdt_install_timeout(wdt, &cfg);
	if (err < 0) {
		LOG_ERR("watchdog timeout install failed (%d)", err);
		return;
	}
	wdt_channel = err;

	/* Paused only while a DEBUGGER holds the CPU, so SWD inspection cannot
	 * trip it. A firmware halt is not paused — that is the point. */
	err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err != 0) {
		LOG_ERR("watchdog setup failed (%d)", err);
		wdt_channel = -1;
		return;
	}

	LOG_INF("hardware watchdog armed (%d s)",
		CONFIG_IMPULSE_HW_WATCHDOG_TIMEOUT_S);
}

void impulse_hw_watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		(void)wdt_feed(wdt, wdt_channel);
	}
}
#else
void impulse_hw_watchdog_start(void)
{
}

void impulse_hw_watchdog_feed(void)
{
}
#endif /* CONFIG_IMPULSE_HW_WATCHDOG */
