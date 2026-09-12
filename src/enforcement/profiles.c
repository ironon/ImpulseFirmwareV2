/* SPDX-License-Identifier: Apache-2.0 */
#include "profiles.h"

#include <string.h>

/* Local so this file compiles on a host as well as under Zephyr. */
#define IMPULSE_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/*
 * §5.4.3. Three strictness levels x three output modes. The looping profiles
 * all start at a 30 s wait and escalate by 2 s per cycle; only the floor
 * differs, which is what separates "normal" from "loose".
 */
static const struct impulse_profile_def s_profiles[] = {
	{
		.id = IMPULSE_PROFILE_STRICT_SILENT,
		.loops = false,
		.step_count = 1,
		.steps = {{true, false, IMPULSE_STEP_CONTINUOUS}},
		.floor_interval_ms = 0,
	},
	{
		.id = IMPULSE_PROFILE_NORMAL_SILENT,
		.loops = true,
		.step_count = 2,
		.steps = {{true, false, 2000U}, {false, false, 30000U}},
		.floor_interval_ms = 5000U,
	},
	{
		.id = IMPULSE_PROFILE_LOOSE_SILENT,
		.loops = true,
		.step_count = 2,
		.steps = {{true, false, 2000U}, {false, false, 30000U}},
		.floor_interval_ms = 10000U,
	},
	{
		.id = IMPULSE_PROFILE_STRICT_BOTH,
		.loops = false,
		.step_count = 1,
		.steps = {{true, true, IMPULSE_STEP_CONTINUOUS}},
		.floor_interval_ms = 0,
	},
	{
		.id = IMPULSE_PROFILE_NORMAL_BOTH,
		.loops = true,
		.step_count = 2,
		.steps = {{true, true, 2000U}, {false, false, 30000U}},
		.floor_interval_ms = 5000U,
	},
	{
		.id = IMPULSE_PROFILE_LOOSE_BOTH,
		.loops = true,
		.step_count = 2,
		.steps = {{true, true, 2000U}, {false, false, 30000U}},
		.floor_interval_ms = 10000U,
	},
	{
		.id = IMPULSE_PROFILE_STRICT_BUZZ,
		.loops = false,
		.step_count = 1,
		.steps = {{false, true, IMPULSE_STEP_CONTINUOUS}},
		.floor_interval_ms = 0,
	},
	{
		.id = IMPULSE_PROFILE_NORMAL_BUZZ,
		.loops = true,
		.step_count = 2,
		.steps = {{false, true, 2000U}, {false, false, 30000U}},
		.floor_interval_ms = 5000U,
	},
	{
		.id = IMPULSE_PROFILE_LOOSE_BUZZ,
		.loops = true,
		.step_count = 2,
		.steps = {{false, true, 2000U}, {false, false, 30000U}},
		.floor_interval_ms = 10000U,
	},
};

const struct impulse_profile_def *impulse_profile_get(uint8_t profile)
{
	for (size_t i = 0; i < IMPULSE_ARRAY_LEN(s_profiles); i++) {
		if (s_profiles[i].id == profile) {
			return &s_profiles[i];
		}
	}
	return NULL;
}

uint32_t impulse_profile_step_duration(const struct impulse_profile_def *def,
				       uint8_t step_index, uint32_t cycles)
{
	if (def == NULL || step_index >= def->step_count) {
		return 0U;
	}

	uint32_t base = def->steps[step_index].duration_ms;

	if (base == IMPULSE_STEP_CONTINUOUS) {
		return IMPULSE_STEP_CONTINUOUS;
	}

	/* Only the silent WAIT step escalates — the step with no output. The
	 * vibration/buzz step keeps its 2 s length; what shortens is the gap
	 * between them, which is what makes the nagging speed up. */
	bool is_wait = !def->steps[step_index].motor_on &&
		       !def->steps[step_index].buzzer_on;

	if (!is_wait || def->floor_interval_ms == 0U) {
		return base;
	}

	uint32_t shrink = cycles * IMPULSE_INTERVAL_DECREMENT_MS;

	if (shrink >= base) {
		return def->floor_interval_ms;
	}

	uint32_t v = base - shrink;

	return (v < def->floor_interval_ms) ? def->floor_interval_ms : v;
}

void impulse_profile_start(struct impulse_profile_run *run, uint8_t profile)
{
	const struct impulse_profile_def *def = impulse_profile_get(profile);

	memset(run, 0, sizeof(*run));
	run->profile = profile;

	if (def == NULL || def->step_count == 0U) {
		run->finished = true;
		return;
	}

	run->motor_on = def->steps[0].motor_on;
	run->buzzer_on = def->steps[0].buzzer_on;
}

void impulse_profile_stop(struct impulse_profile_run *run)
{
	run->motor_on = false;
	run->buzzer_on = false;
	run->finished = true;
}

bool impulse_profile_tick(struct impulse_profile_run *run, uint32_t dt_ms)
{
	const struct impulse_profile_def *def;
	bool was_motor = run->motor_on;
	bool was_buzzer = run->buzzer_on;

	if (run->finished) {
		return false;
	}

	def = impulse_profile_get(run->profile);
	if (def == NULL) {
		impulse_profile_stop(run);
		return was_motor || was_buzzer;
	}

	uint32_t dur = impulse_profile_step_duration(def, run->step_index,
						     run->cycles);

	if (dur == IMPULSE_STEP_CONTINUOUS) {
		/* Runs until the condition is met; the caller stops it. */
		run->motor_on = def->steps[run->step_index].motor_on;
		run->buzzer_on = def->steps[run->step_index].buzzer_on;
		return false;
	}

	run->step_elapsed_ms += dt_ms;

	while (run->step_elapsed_ms >= dur) {
		run->step_elapsed_ms -= dur;
		run->step_index++;

		if (run->step_index >= def->step_count) {
			if (!def->loops) {
				impulse_profile_stop(run);
				return was_motor || was_buzzer;
			}
			run->step_index = 0;
			run->cycles++;
		}

		dur = impulse_profile_step_duration(def, run->step_index,
						    run->cycles);
		if (dur == IMPULSE_STEP_CONTINUOUS) {
			break;
		}
	}

	run->motor_on = def->steps[run->step_index].motor_on;
	run->buzzer_on = def->steps[run->step_index].buzzer_on;

	return (run->motor_on != was_motor) || (run->buzzer_on != was_buzzer);
}
