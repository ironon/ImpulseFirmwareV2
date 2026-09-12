/*
 * Commitment integrity — settle state, diff gate, pending queue, promotion.
 * firmware_spec_v2.md §9.2-§9.4, §9.6-§9.8. Ported behaviour-for-behaviour
 * per v3 §0.2, which calls §9 "the most important chapter in the project".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_INTEGRITY_H_
#define IMPULSE_INTEGRITY_H_

#include <stdbool.h>
#include <stdint.h>

#include "../schedule/schedule.h"
#include "classify.h"

/* §9.10 constants. */
#define IMPULSE_SETTLE_WINDOW_DEFAULT_MIN 120
#define IMPULSE_SETTLE_WINDOW_FLOOR_MIN   30
#define IMPULSE_SETTLE_WINDOW_CEIL_MIN    240
#define IMPULSE_LOOSEN_DELAY_H            24
#define IMPULSE_LOOSEN_FREE_HORIZON_H     24
#define IMPULSE_PASS_BUDGET_DEFAULT       2
#define IMPULSE_PASS_WINDOW_DAYS          7

enum impulse_pending_kind {
	IMPULSE_PENDING_UPSERT = 0, /* full proposed post-change event */
	IMPULSE_PENDING_DELETE = 1,
	IMPULSE_PENDING_SETTING = 2,
};

/*
 * A quarantined change. It stores the FULL proposed post-change state, not a
 * classification, so promotion needs no app involvement (§9.3) — the app may
 * have been uninstalled in the meantime and the promotion must still happen.
 */
struct impulse_pending {
	bool in_use;
	uint8_t kind;
	uint8_t target_id[IMPULSE_UUID_LEN];
	/* Monotonic ELAPSED seconds, never wall clock — see below. */
	uint64_t apply_after_elapsed_s;
	struct impulse_event proposed; /* valid for UPSERT */
	uint16_t setting_key;          /* valid for SETTING */
	int32_t setting_value;
};

/* Per-event settle tracking (§9.2). */
struct impulse_settle {
	bool in_use;
	uint8_t event_id[IMPULSE_UUID_LEN];
	uint64_t last_edit_elapsed_s;
	bool has_baseline;
	struct impulse_event settled_baseline;
};

struct impulse_integrity {
	uint16_t settle_window_min;
	uint8_t pass_allowance;
	uint8_t pass_spend_count;
	uint64_t pass_spend_elapsed_s[8];

	struct impulse_settle settle[CONFIG_IMPULSE_MAX_EVENTS];
	struct impulse_pending pending[CONFIG_IMPULSE_PENDING_QUEUE_MAX];
};

struct impulse_diff_report {
	uint16_t applied;
	uint16_t quarantined;
	uint16_t dropped_queue_full;
	bool rejected_active_event;
};

void impulse_integrity_init(struct impulse_integrity *ig);

/* Seal any schedule event that has no settle record, treating it as already
 * settled with its current state as the baseline. Call at boot after loading
 * both the schedule and the integrity state — see seal_unknown_events(). */
void impulse_integrity_seal_unknown(struct impulse_integrity *ig,
				    const struct impulse_schedule *current);

/*
 * ELAPSED time, not wall clock. Every integrity timer — settle windows,
 * apply_after, pass-window aging — counts monotonic elapsed seconds across
 * reboots, so that writing the clock cannot accelerate a quarantined
 * loosening (§9.2). Do not "fix" this to a timestamp.
 */
uint64_t impulse_elapsed_now_s(void);

/* Seed the persisted elapsed base at boot from storage, and inject time in
 * host tests. Integrity timers are meaningless without this: a reboot would
 * otherwise rewind every settle window and every pending apply_after. */
void impulse_integrity_set_elapsed_base(uint64_t base_s);

/*
 * §9.3 diff gate. `current` is mutated in place into the new accepted
 * schedule; anything quarantined lands in `ig->pending`.
 *
 * `active` is the event currently being enforced, or NULL. Returns the §6.2
 * END verdict byte: 0x01 accepted, 0x03 partially quarantined, 0x04 rejected.
 */
uint8_t impulse_integrity_apply_push(struct impulse_integrity *ig,
				     struct impulse_schedule *current,
				     const struct impulse_schedule *proposed,
				     const struct impulse_event *active,
				     int64_t now_utc, int16_t tz_offset_minutes,
				     struct impulse_diff_report *report);

/*
 * §9.4 autonomous promotion. Called on every wake — RTC boundary, midnight,
 * boot — with no app involvement. Returns the number of entries promoted.
 * Promotion happens no EARLIER than nominal; a sleeping watch may promote
 * late, which is the safe direction.
 */
uint16_t impulse_integrity_promote(struct impulse_integrity *ig,
				   struct impulse_schedule *current,
				   int64_t now_utc, int16_t tz_offset_minutes);

/* §9.6 emergency pass. Applies a one-day negate IMMEDIATELY, deliberately
 * bypassing the diff gate — the pass is the sanctioned escape valve and is
 * spendable even on the currently active event. Returns true if spent. */
bool impulse_integrity_spend_pass(struct impulse_integrity *ig,
				  const uint8_t *event_id, uint32_t date_yyyymmdd,
				  struct impulse_schedule *current,
				  uint8_t *remaining_out);

/* §9.7 time hardening: reject a time or timezone change that would end or skip
 * the currently active window. Returns true if the change is permitted. */
bool impulse_integrity_time_change_allowed(const struct impulse_event *active,
					   int64_t old_utc, int16_t old_tz,
					   int64_t new_utc, int16_t new_tz);

#endif /* IMPULSE_INTEGRITY_H_ */
