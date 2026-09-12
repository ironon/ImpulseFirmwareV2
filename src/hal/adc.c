/* SPDX-License-Identifier: Apache-2.0 */
#include "hal.h"

#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(impulse_adc, LOG_LEVEL_INF);

#define ADC_NODE DT_NODELABEL(adc)

/*
 * Every declared SAADC channel. The AIN-to-pin mapping for this part is not in
 * any document available here, so the channels are discovered on hardware
 * rather than assumed — see the comment on &adc in the board devicetree.
 */
static const struct device *const adc_dev = DEVICE_DT_GET(ADC_NODE);

int impulse_adc_init(void)
{
	if (!device_is_ready(adc_dev)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}
	return 0;
}

/*
 * This part has EIGHT SAADC inputs, AIN0..AIN7 — not the fourteen the
 * dt-bindings header defines. Channels 8 and above are rejected by the driver
 * with "Invalid channel ID". Measured on hardware, 2026-08-30.
 */
#define IMPULSE_ADC_CHANNELS 8

/*
 * Read one SAADC input by channel id, in millivolts. Returns a negative errno
 * on failure. Gain 1/4 against VDD/4 makes full scale VDD (~3.3 V), which
 * covers the battery divider's Vbat/2 and the IR receiver's swing.
 */
int impulse_adc_read_mv(uint8_t channel_id, int32_t *mv_out)
{
	int16_t sample = 0;
	int err;
	struct adc_channel_cfg cfg = {
		.gain = ADC_GAIN_1_4,
		/* ADC_REF_VDD_1_4 is NOT supported on this part — the driver
		 * rejects it with "Selected ADC reference is not valid".
		 * Internal is the one available. */
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 20),
		.channel_id = channel_id,
		.input_positive = channel_id, /* AINn == n */
	};
	struct adc_sequence seq = {
		.channels = BIT(channel_id),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = 12,
		.oversampling = 4,
	};

	if (!device_is_ready(adc_dev)) {
		return -ENODEV;
	}
	if (channel_id >= IMPULSE_ADC_CHANNELS) {
		return -EINVAL;
	}

	err = adc_channel_setup(adc_dev, &cfg);
	if (err != 0) {
		return err;
	}

	err = adc_read(adc_dev, &seq);
	if (err != 0) {
		return err;
	}

	int32_t val = sample;

	/* Ask the driver for its own reference rather than hard-coding one —
	 * the internal reference differs between nRF families. */
	err = adc_raw_to_millivolts(adc_ref_internal(adc_dev), ADC_GAIN_1_4,
				    seq.resolution, &val);
	if (err != 0) {
		return err;
	}

	*mv_out = val;
	return 0;
}

/*
 * Battery. /BAT_DIV on P1.00 is Vbat through a 1M-1M divider, so the pin sees
 * half the cell voltage (spec v3 §2).
 *
 * TODO(hw): the AIN index is set from the sweep, and the divider ratio is
 * taken from the schematic rather than measured. v3 §6.3 warns explicitly that
 * the divider values changed from v2 and says to recheck the scaling constant
 * against the schematic rather than porting the number.
 */
int impulse_battery_mv(int32_t *mv_out)
{
#if defined(CONFIG_IMPULSE_ADC_CH_BATTERY) && CONFIG_IMPULSE_ADC_CH_BATTERY >= 0
	int32_t pin_mv;
	int err = impulse_adc_read_mv(CONFIG_IMPULSE_ADC_CH_BATTERY, &pin_mv);

	if (err != 0) {
		return err;
	}
	*mv_out = pin_mv * 2; /* 1M-1M divider */
	return 0;
#else
	ARG_UNUSED(mv_out);
	return -ENOSYS;
#endif
}

/*
 * Worn detection, §5.2: sample the IR receiver with the emitter OFF and then
 * ON, and use the DIFFERENCE. The lit-minus-ambient method is the whole point
 * — an absolute threshold tracks room lighting, not skin.
 */
int impulse_worn_sample(int32_t *delta_mv_out)
{
#if defined(CONFIG_IMPULSE_ADC_CH_IR) && CONFIG_IMPULSE_ADC_CH_IR >= 0
	int32_t dark = 0, lit = 0;
	int err;

	impulse_ir_emitter_set(false);
	k_msleep(2);
	err = impulse_adc_read_mv(CONFIG_IMPULSE_ADC_CH_IR, &dark);
	if (err != 0) {
		return err;
	}

	impulse_ir_emitter_set(true);
	k_msleep(2); /* TODO(hw): settle time is a guess; measure it */
	err = impulse_adc_read_mv(CONFIG_IMPULSE_ADC_CH_IR, &lit);
	impulse_ir_emitter_set(false);
	if (err != 0) {
		return err;
	}

	/*
	 * Returned as dark-minus-lit so that MORE REFLECTION IS A LARGER
	 * POSITIVE NUMBER, which is the intuitive direction for a "worn"
	 * signal. The raw swing is negative: the ITR8307's phototransistor
	 * conducts harder under illumination and pulls the node down.
	 */
	*delta_mv_out = dark - lit;
	return 0;
#else
	ARG_UNUSED(delta_mv_out);
	return -ENOSYS;
#endif
}
