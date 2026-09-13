/*
 * Fatal-error handling, crash records and the hardware watchdog.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_FATAL_H_
#define IMPULSE_FATAL_H_

/* Log why the previous boot ended: the hardware reset cause, plus the crash
 * record left by the fatal handler if there is one. Call once, early. */
void impulse_fatal_report_boot(void);

/* No-ops unless CONFIG_IMPULSE_HW_WATCHDOG. Start after init; feed every
 * main-loop pass. */
void impulse_hw_watchdog_start(void);
void impulse_hw_watchdog_feed(void);

/*
 * Record WHY this firmware is about to reboot itself, in RAM that survives the
 * reset, and log it at the next boot. Call immediately before sys_reboot().
 *
 * The RTT line a deliberate reboot prints is almost always lost: on 2026-09-13
 * the anchor rebooted mid-window with no crash record and nothing readable
 * over SWD afterwards, leaving the sysworkq watchdog, the RRSP stuck-free
 * timer, the hardware watchdog and a brownout all equally plausible.
 * `why` must be a string literal (only the pointer's text is copied).
 */
void impulse_note_reboot(const char *why);

#endif /* IMPULSE_FATAL_H_ */
