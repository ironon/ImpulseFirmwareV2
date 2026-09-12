/*
 * Schedule system — firmware_spec_v2.md §5.3. Inherited unchanged by v3.
 * recalculate_day() is the single source of truth for what is active on a day.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_SCHEDULE_H_
#define IMPULSE_SCHEDULE_H_

#include <stdbool.h>
#include <stdint.h>

#include "event.h"
#include "schedule_blob.h"

/* A local calendar date. All Event start/end times are minutes since LOCAL
 * midnight; all stored timestamps are UTC (§5.3.5). */
struct impulse_date {
	int32_t year;
	uint8_t month; /* 1..12 */
	uint8_t day;   /* 1..31 */
};

struct impulse_day_plan {
	uint8_t count;
	/* Pointers into the caller's schedule — no copying, and therefore only
	 * valid while that schedule is unmodified. */
	const struct impulse_event *events[CONFIG_IMPULSE_MAX_EVENTS_PER_DAY];
};

/* Civil-date helpers (proleptic Gregorian). Exposed because the integrity
 * code needs them for the LOOSEN_FREE_HORIZON_H calculation. */
int64_t impulse_days_from_civil(int32_t y, uint8_t m, uint8_t d);
void impulse_civil_from_days(int64_t z, struct impulse_date *out);
/* 1 = Monday .. 7 = Sunday, matching Event.day_of_week (§3.2). */
uint8_t impulse_day_of_week(const struct impulse_date *d);

/* Convert a UTC instant to the local date and minute-of-day. */
void impulse_local_from_utc(int64_t utc_seconds, int16_t tz_offset_minutes,
			    struct impulse_date *date_out,
			    uint16_t *minute_of_day_out);

/* UTC instant of local midnight on `date`. */
int64_t impulse_utc_from_local_midnight(const struct impulse_date *date,
					int16_t tz_offset_minutes);

/*
 * §5.3.2. Merge recurring + specific events for `date`, apply the negate
 * mechanic, and sort by start_time ascending.
 *
 * Specific (once) events override recurring events with the SAME UUID, then
 * anything still flagged negate is dropped — so a negating one-time event
 * removes both the recurring occurrence and itself (§5.3.4).
 */
void impulse_recalculate_day(const struct impulse_schedule *sched,
			     const struct impulse_date *date,
			     int16_t tz_offset_minutes,
			     struct impulse_day_plan *out);

/* True if `e` occurs on `date` by its recurrence rule alone (§5.3.3). */
bool impulse_event_occurs_on(const struct impulse_event *e,
			     const struct impulse_date *date,
			     int16_t tz_offset_minutes);

/* The event active at `minute_of_day`, or NULL. Ties break toward the
 * earliest-starting event, which is the order recalculate_day produced. */
const struct impulse_event *impulse_active_event(
	const struct impulse_day_plan *plan, uint16_t minute_of_day);

/*
 * Seconds from `now_utc` until the next occurrence of `e` begins, or INT64_MAX
 * if it has none within the search horizon. Used by the §9.3 free-horizon rule
 * and by wake scheduling.
 */
int64_t impulse_seconds_until_next_start(const struct impulse_event *e,
					 int64_t now_utc,
					 int16_t tz_offset_minutes);

#endif /* IMPULSE_SCHEDULE_H_ */
