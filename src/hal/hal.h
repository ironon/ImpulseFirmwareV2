/*
 * Board V1 hardware layer.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_HAL_H_
#define IMPULSE_HAL_H_

#include <stdbool.h>
#include <stdint.h>

int impulse_outputs_init(void);
void impulse_buzzer_set(bool on);
void impulse_motor_set(bool on);
void impulse_ir_emitter_set(bool on);
void impulse_wifi_power_set(bool on);

int impulse_led_ring_init(void);
/* 12 pixels, GRB order as the strip expects. */
int impulse_led_ring_set_all(uint8_t r, uint8_t g, uint8_t b);
int impulse_led_ring_clear(void);
/* Light exactly one pixel, everything else dark. The test that reveals chain
 * order and which physical LED is index 0. */
int impulse_led_ring_set_one(int index, uint8_t r, uint8_t g, uint8_t b);
/* Analog clock face, v2 §5.7.4 semantics but shown only on demand (v3 §6.2). */
int impulse_led_ring_show_clock(uint16_t minute_of_day, uint8_t brightness);

/* Servo strap lock — ANCHOR ROLE ONLY (v2 §4.9). Never holds position. */
int impulse_servo_init(void);
int impulse_servo_set(bool open);
bool impulse_servo_is_open(void);

int impulse_adc_init(void);
int impulse_adc_read_mv(uint8_t channel_id, int32_t *mv_out);
int impulse_battery_mv(int32_t *mv_out);
/* Ambient-minus-lit IR difference (§5.2). Larger positive = more
 * reflection = more likely worn. */
int impulse_worn_sample(int32_t *delta_mv_out);

enum impulse_button_event {
	IMPULSE_BTN_SHORT = 0,
	IMPULSE_BTN_LONG = 1,
	IMPULSE_BTN_VERY_LONG = 2,
};

typedef void (*impulse_button_cb_t)(enum impulse_button_event ev);

/*
 * ALL button handling routes through this one dispatcher. v3 §6.2 is explicit:
 * leave a clean seam with short/long/very-long dispatch and do not scatter
 * press handling, because the long press is reserved for the hard override
 * whose mechanism is still undecided (chunk J, Concepts/Safety.md).
 * Do NOT implement the v2 two-button hold — this board cannot do it.
 */
int impulse_buttons_init(impulse_button_cb_t cb);

#endif /* IMPULSE_HAL_H_ */
