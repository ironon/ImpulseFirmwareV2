/* SPDX-License-Identifier: Apache-2.0 */
#include "hal.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(impulse_outputs, LOG_LEVEL_INF);

/*
 * [verified on Board V1, 2026-08-29]
 *   buzzer P3.07 — an ACTIVE buzzer: a DC level sounds it, no tone generation
 *                  needed. Confirmed with a 150 ms DC pulse repeated every 10 s.
 *   motor  P3.03 — drives a BSS138 gate through R5, so it sources no real
 *                  current and a plain GPIO is sufficient.
 */
static const struct gpio_dt_spec buzzer =
	GPIO_DT_SPEC_GET(DT_NODELABEL(buzzer), gpios);
#if defined(CONFIG_IMPULSE_BUZZER_DISABLED)
/*
 * Buzzer output is compiled out (CONFIG_IMPULSE_BUZZER_DISABLED). The pin is
 * deliberately left as a high-impedance INPUT rather than an output driven
 * low: an output can be driven by a stray write, an input cannot drive the pad
 * at all. R17 (10k to GND) holds the net low on its own.
 */
#endif
static const struct gpio_dt_spec motor =
	GPIO_DT_SPEC_GET(DT_NODELABEL(motor), gpios);
static const struct gpio_dt_spec ir_emit =
	GPIO_DT_SPEC_GET(DT_NODELABEL(ir_emit), gpios);
static const struct gpio_dt_spec wifi_vddio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(wifi_vddio_en), gpios);
static const struct gpio_dt_spec wifi_buck =
	GPIO_DT_SPEC_GET(DT_NODELABEL(wifi_buck_en), gpios);

static int init_one(const struct gpio_dt_spec *spec, const char *what)
{
	int err;

	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("%s GPIO not ready", what);
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		LOG_ERR("%s configure failed (%d)", what, err);
	}
	return err;
}

int impulse_outputs_init(void)
{
	int err = 0;

#if defined(CONFIG_IMPULSE_BUZZER_DISABLED)
	if (gpio_is_ready_dt(&buzzer)) {
		/* INPUT, never OUTPUT — see the note above. */
		err |= gpio_pin_configure_dt(&buzzer, GPIO_INPUT);
	}
	LOG_WRN("=========================================================");
	LOG_WRN(" BUZZER OUTPUT IS COMPILED OUT — this build cannot beep.");
	LOG_WRN(" Enforcement alarms and anchor beeps are SILENT.");
	LOG_WRN(" Clear CONFIG_IMPULSE_BUZZER_DISABLED to restore sound.");
	LOG_WRN("=========================================================");
#else
	err |= init_one(&buzzer, "buzzer");
#endif
	err |= init_one(&motor, "motor");
	err |= init_one(&ir_emit, "ir_emit");
#if defined(CONFIG_WIFI_NRF70)
	/*
	 * HANDS OFF when the nRF70 driver is in the build: it owns BUCK_EN and
	 * IOVDD through its own bucken-gpios / iovdd-ctrl-gpios, and it powers
	 * the module up during device init — which runs BEFORE main(), and so
	 * before this function. Configuring them here as inactive outputs would
	 * therefore drive the rails LOW again and switch the module off
	 * moments after the driver brought it up, presenting as a WM02C that
	 * never answers on SPI.
	 */
#else
	err |= init_one(&wifi_vddio, "wifi_vddio_en");
	err |= init_one(&wifi_buck, "wifi_buck_en");
#endif

	return err;
}

void impulse_buzzer_set(bool on)
{
#if defined(CONFIG_IMPULSE_BUZZER_DISABLED)
	/* Compiled out. The pin is not even an output — see impulse_outputs_init(). */
	ARG_UNUSED(on);
#else
	(void)gpio_pin_set_dt(&buzzer, on ? 1 : 0);
#endif
}

void impulse_motor_set(bool on)
{
	(void)gpio_pin_set_dt(&motor, on ? 1 : 0);
}

void impulse_ir_emitter_set(bool on)
{
	(void)gpio_pin_set_dt(&ir_emit, on ? 1 : 0);
}

void impulse_wifi_power_set(bool on)
{
#if defined(CONFIG_WIFI_NRF70)
	/* The driver sequences these itself. Driving them from here fights it. */
	ARG_UNUSED(on);
#else
	/* TODO(hw): sequencing between the buck and the VDDIO load switch is
	 * UNVERIFIED. The WM02C's datasheet almost certainly requires an order
	 * and a settle delay; powering a QSPI peripheral in the wrong order is
	 * a classic way to latch it up. Do not enable WiFi on hardware until
	 * this is checked against the module datasheet. */
	(void)gpio_pin_set_dt(&wifi_buck, on ? 1 : 0);
	k_msleep(2);
	(void)gpio_pin_set_dt(&wifi_vddio, on ? 1 : 0);
#endif
}
