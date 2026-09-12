/* SPDX-License-Identifier: Apache-2.0 */
#include "hal.h"

#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(impulse_servo, LOG_LEVEL_INF);

/*
 * SG90 strap lock, anchor role only (v2 §4.9). P1.02 / pad A8 via PWM20.
 *
 * THE SERVO MUST NEVER ACTIVELY HOLD A POSITION. Every command attaches,
 * moves, waits, then stops driving. That is not a power optimisation: `U1`
 * pin 2 is /VBAT with no switch, fuse or current limit (v3 §7), so a stalled
 * servo is a dead short across the cell held off only by the pack's protection
 * IC. One ESP32 anchor was destroyed this way on 2026-08-10. The rail cannot
 * be cut in software, so not driving it is the only control firmware has.
 */
#define SERVO_PERIOD_NS PWM_MSEC(20) /* 50 Hz */

/* SG90: ~0.5 ms = 0 deg, ~2.5 ms = 180 deg. */
#define SERVO_MIN_PULSE_NS PWM_USEC(500)
#define SERVO_MAX_PULSE_NS PWM_USEC(2500)

#define SERVO_CLOSED_DEGREES 180
#define SERVO_OPEN_DEGREES 160

/* Long enough for the horn to actually arrive before power is removed.
 * TODO(hw): 500 ms is the ESP32 firmware's value, never measured on this
 * mechanism. Too short and the strap lock stops half-way. */
#define SERVO_MOVE_DURATION_MS 500

static const struct device *const pwm_dev = DEVICE_DT_GET(DT_NODELABEL(pwm20));
static bool servo_is_open;

static uint32_t degrees_to_pulse_ns(uint32_t deg)
{
	if (deg > 180U) {
		deg = 180U;
	}
	return SERVO_MIN_PULSE_NS +
	       ((SERVO_MAX_PULSE_NS - SERVO_MIN_PULSE_NS) * deg) / 180U;
}

int impulse_servo_init(void)
{
	if (!device_is_ready(pwm_dev)) {
		LOG_ERR("servo PWM not ready");
		return -ENODEV;
	}
	/* §4.9: boot to closed, then stop driving. */
	return impulse_servo_set(false);
}

int impulse_servo_set(bool open)
{
	uint32_t deg = open ? SERVO_OPEN_DEGREES : SERVO_CLOSED_DEGREES;
	int err;

	if (!device_is_ready(pwm_dev)) {
		return -ENODEV;
	}

	err = pwm_set(pwm_dev, 0, SERVO_PERIOD_NS, degrees_to_pulse_ns(deg),
		      PWM_POLARITY_NORMAL);
	if (err != 0) {
		LOG_ERR("servo move failed (%d)", err);
		return err;
	}

	k_msleep(SERVO_MOVE_DURATION_MS);

	/* Stop driving — see the warning at the top of this file. */
	(void)pwm_set(pwm_dev, 0, SERVO_PERIOD_NS, 0, PWM_POLARITY_NORMAL);

	servo_is_open = open;
	LOG_INF("servo -> %s", open ? "open" : "closed");
	return 0;
}

bool impulse_servo_is_open(void)
{
	return servo_is_open;
}
