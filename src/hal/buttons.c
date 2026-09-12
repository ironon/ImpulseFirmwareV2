/* SPDX-License-Identifier: Apache-2.0 */
#include "hal.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(impulse_buttons, LOG_LEVEL_INF);

/* [verified 2026-08-29] Wake button on P0.05, S2 to GND, no external pull. */
static const struct gpio_dt_spec wake =
	GPIO_DT_SPEC_GET(DT_NODELABEL(wake_button), gpios);

static struct gpio_callback wake_cb_data;
static impulse_button_cb_t user_cb;
static int64_t press_started_ms;
/* Set once VERY_LONG has been delivered for the press still in progress, so
 * the release path does not deliver it a SECOND time. Observed on hardware
 * 2026-09-10: one 6 s hold produced two VERY_LONG events 1.45 s apart — once
 * from the while-held check below, once again on release. The hard override
 * (chunk J) is the consumer, and a duplicate there is not harmless: anything
 * with toggle semantics would immediately undo itself. */
static bool very_long_fired;

/* Thresholds. Short is anything under LONG_MS. */
#define BTN_LONG_MS      1500
#define BTN_VERY_LONG_MS 5000
#define BTN_DEBOUNCE_MS  30

static void classify_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(classify_work, classify_work_handler);

static void classify_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int level = gpio_pin_get_dt(&wake);

	if (level > 0) {
		/* Still held — check again later. The hard override needs a
		 * very-long hold to be detectable while the button is down,
		 * not only on release. */
		int64_t held = k_uptime_get() - press_started_ms;

		if (held >= BTN_VERY_LONG_MS) {
			if (!very_long_fired) {
				very_long_fired = true;
				if (user_cb != NULL) {
					user_cb(IMPULSE_BTN_VERY_LONG);
				}
			}
			return;
		}
		(void)k_work_reschedule(&classify_work, K_MSEC(100));
		return;
	}

	int64_t held = k_uptime_get() - press_started_ms;
	bool already = very_long_fired;

	/* Released: the next press starts clean regardless of what happens
	 * below. */
	very_long_fired = false;

	if (held < BTN_DEBOUNCE_MS) {
		return;
	}

	if (user_cb == NULL) {
		return;
	}

	if (already) {
		/* Already delivered while the button was still down. */
		return;
	}

	if (held >= BTN_VERY_LONG_MS) {
		user_cb(IMPULSE_BTN_VERY_LONG);
	} else if (held >= BTN_LONG_MS) {
		user_cb(IMPULSE_BTN_LONG);
	} else {
		user_cb(IMPULSE_BTN_SHORT);
	}
}

static void wake_isr(const struct device *port, struct gpio_callback *cb,
		     gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (gpio_pin_get_dt(&wake) > 0) {
		press_started_ms = k_uptime_get();
		very_long_fired = false;
	}
	/* Never do real work in the ISR — classify on the workqueue. */
	(void)k_work_reschedule(&classify_work, K_MSEC(BTN_DEBOUNCE_MS));
}

int impulse_buttons_init(impulse_button_cb_t cb)
{
	int err;

	user_cb = cb;

	if (!gpio_is_ready_dt(&wake)) {
		LOG_ERR("wake button not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&wake, GPIO_INPUT);
	if (err != 0) {
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&wake, GPIO_INT_EDGE_BOTH);
	if (err != 0) {
		return err;
	}

	gpio_init_callback(&wake_cb_data, wake_isr, BIT(wake.pin));
	return gpio_add_callback(wake.port, &wake_cb_data);
}
