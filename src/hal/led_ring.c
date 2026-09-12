/* SPDX-License-Identifier: Apache-2.0 */
#include "hal.h"

#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(impulse_led_ring, LOG_LEVEL_INF);

#define RING_LEN 12

static const struct device *const strip = DEVICE_DT_GET(DT_NODELABEL(led_ring));
static struct led_rgb pixels[RING_LEN];

int impulse_led_ring_init(void)
{
	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip not ready");
		return -ENODEV;
	}
	return impulse_led_ring_clear();
}

int impulse_led_ring_clear(void)
{
	memset(pixels, 0, sizeof(pixels));
	return led_strip_update_rgb(strip, pixels, RING_LEN);
}

int impulse_led_ring_set_one(int index, uint8_t r, uint8_t g, uint8_t b)
{
	if (index < 0 || index >= RING_LEN) {
		return -EINVAL;
	}
	memset(pixels, 0, sizeof(pixels));
	pixels[index].r = r;
	pixels[index].g = g;
	pixels[index].b = b;
	return led_strip_update_rgb(strip, pixels, RING_LEN);
}

int impulse_led_ring_set_all(uint8_t r, uint8_t g, uint8_t b)
{
	for (int i = 0; i < RING_LEN; i++) {
		pixels[i].r = r;
		pixels[i].g = g;
		pixels[i].b = b;
	}
	return led_strip_update_rgb(strip, pixels, RING_LEN);
}

/*
 * Clock position -> physical pixel index.
 *
 * The chain runs COUNTER-CLOCKWISE around the face while every caller thinks
 * clockwise, so the mapping is a mirror about the 12-6 axis, not a rotation.
 *
 * Measured 2026-09-10: rendering 09:30 put the hour marker at 3 o'clock while
 * the minute marker STAYED at 6 o'clock. A rotation would have moved both and
 * shown 3:00; only a mirror leaves a marker sitting on the 12-6 axis fixed.
 * That is what distinguishes the two, and it is why this is a subtraction
 * rather than an offset constant.
 *
 * Pixel 0 itself is at 12 o'clock, so no additive term is needed. If a later
 * case rotates the ring in its housing, add the offset here — not in callers.
 */
static inline uint16_t ring_pos(uint16_t clock_index)
{
	return (uint16_t)((RING_LEN - (clock_index % RING_LEN)) % RING_LEN);
}

int impulse_led_ring_show_clock(uint16_t minute_of_day, uint8_t brightness)
{
	/*
	 * 12 LEDs = a 12-hour face, one LED per hour. v2 §5.7.4 kept this lit
	 * continuously; v3 §6.2 makes it a 5 s glance on a button press, which
	 * is the larger battery saving of the two designs.
	 *
	 * Index 0 IS at 12 o'clock, but the chain runs COUNTER-CLOCKWISE —
	 * see ring_pos(). [verified 2026-09-10]
	 */
	uint16_t hour12 = (uint16_t)((minute_of_day / 60U) % 12U);
	uint16_t minute = minute_of_day % 60U;
	uint16_t minute_led = (uint16_t)((minute * RING_LEN) / 60U);

	memset(pixels, 0, sizeof(pixels));

	pixels[ring_pos(minute_led)].r = brightness / 4U;
	pixels[ring_pos(minute_led)].g = brightness / 4U;
	pixels[ring_pos(minute_led)].b = brightness / 4U;

	pixels[ring_pos(hour12)].r = brightness;
	pixels[ring_pos(hour12)].g = brightness / 3U;
	pixels[ring_pos(hour12)].b = 0U;

	return led_strip_update_rgb(strip, pixels, RING_LEN);
}
