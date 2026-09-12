/*
 * Enforcement profiles — firmware_spec_v2.md §5.4.3. Inherited unchanged.
 *
 * Data-driven by design: the spec states that adding a profile must require
 * only a new table entry and no new control flow. Keep it that way.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_PROFILES_H_
#define IMPULSE_PROFILES_H_

#include <stdbool.h>
#include <stdint.h>

#include "../schedule/event.h"

/* A step that runs until the condition is met, rather than for a duration. */
#define IMPULSE_STEP_CONTINUOUS UINT32_MAX

#define IMPULSE_INTERVAL_DECREMENT_MS 2000U
#define IMPULSE_PROFILE_MAX_STEPS 2

struct impulse_profile_step {
	bool motor_on;
	bool buzzer_on;
	uint32_t duration_ms;
};

struct impulse_profile_def {
	uint8_t id;
	bool loops;
	uint8_t step_count;
	struct impulse_profile_step steps[IMPULSE_PROFILE_MAX_STEPS];
	/* 0 = no escalation. Otherwise the WAIT step shrinks by
	 * INTERVAL_DECREMENT_MS each cycle down to this floor. */
	uint32_t floor_interval_ms;
};

/* Live state of one running profile. */
struct impulse_profile_run {
	uint8_t profile;
	uint8_t step_index;
	uint32_t step_elapsed_ms;
	uint32_t cycles;
	bool motor_on;
	bool buzzer_on;
	bool finished;
};

const struct impulse_profile_def *impulse_profile_get(uint8_t profile);

void impulse_profile_start(struct impulse_profile_run *run, uint8_t profile);

/*
 * Advance by `dt_ms`. Updates run->motor_on / run->buzzer_on, which the caller
 * drives to hardware. Returns true if the output state changed.
 */
bool impulse_profile_tick(struct impulse_profile_run *run, uint32_t dt_ms);

/* Stop all output immediately — used the instant the condition becomes met. */
void impulse_profile_stop(struct impulse_profile_run *run);

/* Effective duration of `step_index` on cycle `cycles`, after escalation. */
uint32_t impulse_profile_step_duration(const struct impulse_profile_def *def,
				       uint8_t step_index, uint32_t cycles);

#endif /* IMPULSE_PROFILES_H_ */
