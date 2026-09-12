/*
 * Bring-up shell — exercises each Board V1 peripheral from the RTT console.
 * SPDX-License-Identifier: Apache-2.0
 *
 * This is the Zephyr counterpart of ~/Impulse/.bringup/bringup-test.sh, which
 * did the same job for the bare-metal image. It exists because there is no
 * UART on this board and no other way to poke a driver interactively.
 *
 * Build-gated by CONFIG_IMPULSE_BRINGUP_SHELL so it cannot ship: it can drive
 * the motor and buzzer directly, which on a commitment device must never be
 * reachable at runtime in a release image.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "hal.h"
#include "../app_api.h"
#include "../schedule/event.h"

static int cmd_buzz(const struct shell *sh, size_t argc, char **argv)
{
#if defined(CONFIG_IMPULSE_BUZZER_DISABLED)
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "buzzer is COMPILED OUT of this build "
			"(CONFIG_IMPULSE_BUZZER_DISABLED). Nothing was driven.");
	return 0;
#else
	uint32_t ms = (argc > 1) ? (uint32_t)atoi(argv[1]) : 200U;

	shell_print(sh, "buzzer on for %u ms (P3.07, pad C9)", ms);
	impulse_buzzer_set(true);
	k_msleep(ms);
	impulse_buzzer_set(false);
	return 0;
#endif
}

static int cmd_motor(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = (argc > 1) ? (uint32_t)atoi(argv[1]) : 400U;

	shell_print(sh, "motor on for %u ms (P3.03, pad A7)", ms);
	impulse_motor_set(true);
	k_msleep(ms);
	impulse_motor_set(false);
	return 0;
}

static int cmd_led(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "off") == 0) {
		shell_print(sh, "ring off");
		return impulse_led_ring_clear();
	}

	uint8_t r = (argc > 1) ? (uint8_t)atoi(argv[1]) : 0;
	uint8_t g = (argc > 2) ? (uint8_t)atoi(argv[2]) : 0;
	uint8_t b = (argc > 3) ? (uint8_t)atoi(argv[3]) : 0;

	shell_print(sh, "ring r=%u g=%u b=%u", r, g, b);
	return impulse_led_ring_set_all(r, g, b);
}

static int cmd_clock(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t minute = (argc > 1) ? (uint16_t)atoi(argv[1]) : 0;

	shell_print(sh, "clock face at minute-of-day %u", minute);
	return impulse_led_ring_show_clock(minute, 40);
}

/*
 * Walks the ring one pixel at a time. This is the test that finds a wiring or
 * ordering problem the "all on" test cannot: it shows the chain length, the
 * colour order, and which physical LED is index 0 — the last of which the
 * clock face assumes and nothing has ever confirmed.
 */
static int cmd_chase(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t laps = (argc > 1) ? (uint32_t)atoi(argv[1]) : 2U;

	shell_print(sh, "chasing %u lap(s); watch for the first-lit position", laps);
	for (uint32_t l = 0; l < laps; l++) {
		for (int i = 0; i < 12; i++) {
			(void)impulse_led_ring_set_one(i, 60, 0, 0);
			k_msleep(120);
		}
	}
	return impulse_led_ring_clear();
}

/*
 * Prove the WIRE moved, not just that the driver returned 0.
 *
 * This is the lesson from the 2026-08-29 bare-metal session, where PWM
 * reported a full DMA transfer and a completion event on every frame while
 * driving the pad with zero highs out of 199 samples. DMA completion proves
 * the engine ran; it never proves the waveform reached the pin. So: drive the
 * strip repeatedly from a work item and sample P0.02's input buffer from here.
 *
 * P0 base 0x5010A000: IN at +0x00C, PIN_CNF[n] at +0x080+4n. The pin is owned
 * by SPIM30 via PSEL, but the GPIO input buffer can still be connected so the
 * pad is readable while the peripheral drives it.
 */
#define P0_IN_REG    (*(volatile uint32_t *)0x5010A00CUL)
#define P0_PINCNF2   (*(volatile uint32_t *)0x5010A088UL)
#define LED_PIN_MASK (1UL << 2)

static volatile bool ledpin_busy;

static void ledpin_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	for (int i = 0; i < 40; i++) {
		(void)impulse_led_ring_set_all(60, 60, 60);
	}
	ledpin_busy = false;
}

static K_WORK_DEFINE(ledpin_work, ledpin_work_handler);

static int cmd_ledpin(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t highs = 0, samples = 0;
	uint32_t cnf = P0_PINCNF2;

	/* Connect the input buffer (bit 1 == 0) without disturbing anything
	 * else, so the pad can be read while SPIM30 drives it. */
	P0_PINCNF2 = cnf & ~(1UL << 1);

	ledpin_busy = true;
	k_work_submit(&ledpin_work);

	int64_t deadline = k_uptime_get() + 400;

	while (ledpin_busy && k_uptime_get() < deadline) {
		samples++;
		if (P0_IN_REG & LED_PIN_MASK) {
			highs++;
		}
	}

	P0_PINCNF2 = cnf;

	shell_print(sh, "P0.02 highs=%u / samples=%u", highs, samples);
	if (highs == 0U) {
		shell_print(sh, "  ZERO highs: SPIM30 is not reaching the pad.");
	} else {
		shell_print(sh, "  pad is being driven (%u%% duty)",
			    (unsigned)(100U * highs / (samples ? samples : 1U)));
	}
	return 0;
}

static int cmd_time(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		int64_t utc = (int64_t)strtoll(argv[1], NULL, 10);
		int16_t tz = (argc > 2) ? (int16_t)atoi(argv[2]) : 0;

		impulse_app_set_time(utc, tz);
		shell_print(sh, "time set: utc=%lld tz=%d", utc, tz);
	}
	shell_print(sh, "now_utc=%lld", impulse_app_now_utc());
	return 0;
}

/*
 * End-to-end enforcement test with no BLE and no anchor.
 *
 * Installs a one-event schedule whose window already covers the current
 * minute. With the CS backend stubbed, every measurement abstains, so:
 *   stayNear (1) -> fails CLOSED -> condition NOT met -> the profile runs and
 *                   drives the motor/buzzer. This is the interesting case.
 *   getAway  (0) -> fails OPEN   -> condition met -> silence.
 * That asymmetry is §4.5's criterion-dependent fail-safe, and watching it
 * actually move hardware is the point of this command.
 */
static int cmd_demo(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t crit = (argc > 1) ? (uint8_t)atoi(argv[1])
				  : IMPULSE_CRIT_STAY_NEAR;
	uint8_t prof = (argc > 2) ? (uint8_t)atoi(argv[2])
				  : IMPULSE_PROFILE_NORMAL_BOTH;

	(void)impulse_app_install_demo(crit, prof);
	shell_print(sh, "demo schedule installed: criteria=%u profile=%u", crit,
		    prof);
	shell_print(sh, "  stayNear=1 fails closed (expect output); "
			"getAway=0 fails open (expect silence)");
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	impulse_app_report();
	shell_print(sh, "reported to the log");
	return 0;
}

static int cmd_save(const struct shell *sh, size_t argc, char **argv)
{
	int err = impulse_app_save_all();

	shell_print(sh, "saved to NVS (err=%d). Reset and run 'impulse status' "
			"to verify the round trip.", err);
	return 0;
}

/*
 * Sweep every SAADC input. The AIN-to-pin mapping for the nRF54LM20A is in no
 * document we have, so it is identified from live signals instead:
 *   - the battery divider (P1.00) sits at Vbat/2, so it reads roughly
 *     1800-2100 mV on a charged cell while unconnected inputs float near 0;
 *   - the IR receiver (P1.29) is the channel whose reading MOVES when the
 *     emitter is toggled, which the second pass below measures directly.
 */
static int cmd_adc(const struct shell *sh, size_t argc, char **argv)
{
	int32_t dark[14] = {0};
	int32_t lit[14] = {0};

	shell_print(sh, "ch   emitter-off   emitter-on    delta");
	impulse_ir_emitter_set(false);
	k_msleep(5);
	for (uint8_t ch = 0; ch < 14U; ch++) {
		if (impulse_adc_read_mv(ch, &dark[ch]) != 0) {
			dark[ch] = INT32_MIN;
		}
	}

	impulse_ir_emitter_set(true);
	k_msleep(5);
	for (uint8_t ch = 0; ch < 14U; ch++) {
		if (impulse_adc_read_mv(ch, &lit[ch]) != 0) {
			lit[ch] = INT32_MIN;
		}
	}
	impulse_ir_emitter_set(false);

	for (uint8_t ch = 0; ch < 14U; ch++) {
		if (dark[ch] == INT32_MIN || lit[ch] == INT32_MIN) {
			shell_print(sh, "%2u        (unreadable)", ch);
			continue;
		}
		shell_print(sh, "%2u   %8d mV   %8d mV   %+6d", ch, dark[ch],
			    lit[ch], lit[ch] - dark[ch]);
	}
	shell_print(sh, "");
	shell_print(sh, "Battery divider = the channel near Vbat/2 "
			"(~1800-2100 mV on a charged cell).");
	shell_print(sh, "IR receiver     = the channel with the largest |delta|.");
	return 0;
}

static int cmd_wornset(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "usage: impulse wornset <0|1|auto>");
		return 0;
	}
	if (strcmp(argv[1], "auto") == 0) {
		impulse_app_force_worn(-1);
		shell_print(sh, "worn: using the IR sensor");
	} else {
		int8_t v = (int8_t)(atoi(argv[1]) != 0);

		impulse_app_force_worn(v);
		shell_print(sh, "worn: FORCED %s (bench override; "
				"'wornset auto' returns to the sensor)",
			    v ? "ON (worn)" : "OFF (not worn)");
	}
	return 0;
}

static int cmd_worn(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "IR emitter on for 1 s (P0.07). No ADC driver exists "
			"yet — the receiver on P1.29 cannot be read until the "
			"SAADC AIN mapping is known. See HARDWARE_TEST_PLAN item 1.");
	impulse_ir_emitter_set(true);
	k_msleep(1000);
	impulse_ir_emitter_set(false);
	return 0;
}

/*
 * DELIBERATE FAILURES. A fatal handler or a watchdog that has never fired is a
 * claim, not a feature — these exist to prove both on real hardware. Each
 * drives the motor first, because the failure being guarded against is a
 * crashed device with the motor latched on (2026-09-12, SDC assert 23/587).
 */
static int cmd_crash(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "motor ON, then kernel panic — expect a reboot and a "
			"PREVIOUS BOOT CRASHED report");
	impulse_motor_set(true);
	k_sleep(K_MSEC(300));
	k_panic();
	return 0;
}

static int cmd_hang(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
#if defined(CONFIG_IMPULSE_HW_WATCHDOG)
	shell_print(sh, "motor ON, interrupts locked forever — only the hardware "
			"watchdog can recover, in ~%d s",
		    CONFIG_IMPULSE_HW_WATCHDOG_TIMEOUT_S);
#else
	shell_print(sh, "motor ON, interrupts locked forever — NO hardware "
			"watchdog in this build, so only reset recovers");
#endif
	k_sleep(K_MSEC(300));
	impulse_motor_set(true);
	(void)irq_lock();
	for (;;) {
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_impulse,
	SHELL_CMD_ARG(buzz, NULL, "buzz [ms]", cmd_buzz, 1, 1),
	SHELL_CMD_ARG(motor, NULL, "motor [ms]", cmd_motor, 1, 1),
	SHELL_CMD_ARG(led, NULL, "led <r> <g> <b> | led off", cmd_led, 1, 3),
	SHELL_CMD_ARG(clock, NULL, "clock [minute_of_day]", cmd_clock, 1, 1),
	SHELL_CMD_ARG(chase, NULL, "chase [laps]", cmd_chase, 1, 1),
	SHELL_CMD_ARG(ledpin, NULL, "sample P0.02 while driving the ring", cmd_ledpin, 1, 0),
	SHELL_CMD_ARG(time, NULL, "time [utc_seconds] [tz_min]", cmd_time, 1, 2),
	SHELL_CMD_ARG(demo, NULL, "demo [criteria] [profile]", cmd_demo, 1, 2),
	SHELL_CMD_ARG(status, NULL, "print app state", cmd_status, 1, 0),
	SHELL_CMD_ARG(save, NULL, "persist schedule+integrity to NVS", cmd_save, 1, 0),
	SHELL_CMD_ARG(adc, NULL, "sweep every SAADC input to find the mapping", cmd_adc, 1, 0),
	SHELL_CMD_ARG(worn, NULL, "pulse the IR emitter", cmd_worn, 1, 0),
	SHELL_CMD_ARG(wornset, NULL, "wornset <0|1|auto> — override the worn sensor",
		      cmd_wornset, 1, 1),
	SHELL_CMD_ARG(crash, NULL,
		      "BENCH: motor on + kernel panic (proves the fatal handler)",
		      cmd_crash, 1, 0),
	SHELL_CMD_ARG(hang, NULL,
		      "BENCH: motor on + irqs locked (proves the hardware watchdog)",
		      cmd_hang, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(impulse, &sub_impulse, "Board V1 bring-up", NULL);
