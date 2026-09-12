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

#endif /* IMPULSE_FATAL_H_ */
