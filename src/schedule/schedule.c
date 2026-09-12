/* SPDX-License-Identifier: Apache-2.0 */
#include "schedule.h"

#include <string.h>

/*
 * Howard Hinnant's civil-date algorithms. Chosen over any libc time API on
 * purpose: these are branch-free, allocation-free, have no locale or timezone
 * database behind them, and are trivially testable on a host. Valid for any
 * year the int32 range covers, which is well past anything this product sees.
 */
int64_t impulse_days_from_civil(int32_t y, uint8_t m, uint8_t d)
{
	int64_t yy = y;

	yy -= (m <= 2U) ? 1 : 0;

	const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
	const int64_t yoe = yy - era * 400;                       /* 0..399 */
	const int64_t doy =
		(153 * (m + (m > 2U ? -3 : 9)) + 2) / 5 + (int64_t)d - 1;
	const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + doe - 719468;
}

void impulse_civil_from_days(int64_t z, struct impulse_date *out)
{
	z += 719468;

	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;                     /* 0..146096 */
	const int64_t yoe =
		(doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t y = yoe + era * 400;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const int64_t d = doy - (153 * mp + 2) / 5 + 1;
	const int64_t m = mp + (mp < 10 ? 3 : -9);

	out->year = (int32_t)(y + (m <= 2 ? 1 : 0));
	out->month = (uint8_t)m;
	out->day = (uint8_t)d;
}

uint8_t impulse_day_of_week(const struct impulse_date *d)
{
	/* 1970-01-01 was a Thursday. Shift so Monday == 1, Sunday == 7 to
	 * match Event.day_of_week (§3.2). */
	int64_t days = impulse_days_from_civil(d->year, d->month, d->day);
	int64_t dow = (days + 3) % 7; /* 0 = Monday */

	if (dow < 0) {
		dow += 7;
	}
	return (uint8_t)(dow + 1);
}

void impulse_local_from_utc(int64_t utc_seconds, int16_t tz_offset_minutes,
			    struct impulse_date *date_out,
			    uint16_t *minute_of_day_out)
{
	int64_t local = utc_seconds + (int64_t)tz_offset_minutes * 60;
	int64_t days = local / 86400;
	int64_t rem = local % 86400;

	if (rem < 0) {
		rem += 86400;
		days -= 1;
	}
	if (date_out != NULL) {
		impulse_civil_from_days(days, date_out);
	}
	if (minute_of_day_out != NULL) {
		*minute_of_day_out = (uint16_t)(rem / 60);
	}
}

int64_t impulse_utc_from_local_midnight(const struct impulse_date *date,
					int16_t tz_offset_minutes)
{
	int64_t days = impulse_days_from_civil(date->year, date->month,
					       date->day);

	return days * 86400 - (int64_t)tz_offset_minutes * 60;
}

bool impulse_event_occurs_on(const struct impulse_event *e,
			     const struct impulse_date *date,
			     int16_t tz_offset_minutes)
{
	switch (e->recurrence) {
	case IMPULSE_RECUR_DAILY:
		return true;
	case IMPULSE_RECUR_WEEKLY:
		return impulse_day_of_week(date) == e->day_of_week;
	case IMPULSE_RECUR_MONTHLY:
		return date->day == e->day_of_month;
	case IMPULSE_RECUR_ONCE: {
		/* reference_date is a UTC instant; the occurrence is the LOCAL
		 * date it falls on. Comparing UTC dates here would move the
		 * event by a day for anyone far enough from UTC, which is a
		 * genuinely nasty bug in a product whose windows are matched on
		 * an exact local minute. */
		struct impulse_date ref;

		impulse_local_from_utc(e->reference_date, tz_offset_minutes,
				       &ref, NULL);
		return ref.year == date->year && ref.month == date->month &&
		       ref.day == date->day;
	}
	default:
		return false;
	}
}

void impulse_recalculate_day(const struct impulse_schedule *sched,
			     const struct impulse_date *date,
			     int16_t tz_offset_minutes,
			     struct impulse_day_plan *out)
{
	memset(out, 0, sizeof(*out));

	if (sched == NULL || date == NULL) {
		return;
	}

	/*
	 * §5.3.2 steps 1-3 merged into one pass. The spec builds a map keyed by
	 * UUID where specific events overwrite recurring ones; with a small
	 * bounded array a linear scan is simpler and has no allocation.
	 *
	 * Ordering matters: a "once" event must win over a recurring event with
	 * the same UUID regardless of the order they appear in the blob, so
	 * recurring entries are only inserted if no once-entry already claimed
	 * that UUID, and a once-entry replaces any recurring entry it finds.
	 */
	for (uint16_t i = 0; i < sched->count; i++) {
		const struct impulse_event *e = &sched->events[i];

		if (!impulse_event_occurs_on(e, date, tz_offset_minutes)) {
			continue;
		}

		bool is_specific = (e->recurrence == IMPULSE_RECUR_ONCE);
		int existing = -1;

		for (uint8_t j = 0; j < out->count; j++) {
			if (impulse_uuid_eq(out->events[j]->id, e->id)) {
				existing = (int)j;
				break;
			}
		}

		if (existing >= 0) {
			if (is_specific) {
				/* Specific overrides recurring (step 3). */
				out->events[existing] = e;
			}
			continue;
		}

		if (out->count >= CONFIG_IMPULSE_MAX_EVENTS_PER_DAY) {
			/* TODO(review): the ESP32 build silently dropped the
			 * overflow. Dropping is wrong in the loosening
			 * direction — a dropped event is an unenforced
			 * commitment — but so is failing the whole day. Left as
			 * a drop to match shipped behaviour; raise the cap
			 * instead, which this MCU can easily afford. */
			break;
		}
		out->events[out->count++] = e;
	}

	/* Step 4: drop negated entries. A negating one-time event removes the
	 * recurring occurrence it shadowed AND itself, so after the override
	 * pass above the negate flag is simply a delete. */
	uint8_t w = 0;

	for (uint8_t i = 0; i < out->count; i++) {
		if (!out->events[i]->negate) {
			out->events[w++] = out->events[i];
		}
	}
	out->count = w;

	/* Step 5: sort by start_time ascending. Insertion sort — the array is
	 * tiny and bounded, and it is stable, which keeps same-start events in
	 * blob order rather than an arbitrary one. */
	for (uint8_t i = 1; i < out->count; i++) {
		const struct impulse_event *key = out->events[i];
		int j = (int)i - 1;

		while (j >= 0 && out->events[j]->start_time > key->start_time) {
			out->events[j + 1] = out->events[j];
			j--;
		}
		out->events[j + 1] = key;
	}
}

const struct impulse_event *impulse_active_event(
	const struct impulse_day_plan *plan, uint16_t minute_of_day)
{
	if (plan == NULL) {
		return NULL;
	}
	for (uint8_t i = 0; i < plan->count; i++) {
		const struct impulse_event *e = plan->events[i];

		/* Half-open [start, end): an event ending at 09:00 is not
		 * active at 09:00, so a back-to-back pair does not overlap. */
		if (minute_of_day >= e->start_time &&
		    minute_of_day < e->end_time) {
			return e;
		}
	}
	return NULL;
}

int64_t impulse_seconds_until_next_start(const struct impulse_event *e,
					 int64_t now_utc,
					 int16_t tz_offset_minutes)
{
	struct impulse_date today;
	uint16_t now_minute;

	if (e == NULL) {
		return INT64_MAX;
	}

	impulse_local_from_utc(now_utc, tz_offset_minutes, &today, &now_minute);

	int64_t base_days = impulse_days_from_civil(today.year, today.month,
						    today.day);

	/* Search a bounded horizon. 400 days covers every recurrence this
	 * format can express, including a monthly event on the 31st and a
	 * yearly-in-practice "once". */
	for (int32_t offset = 0; offset < 400; offset++) {
		struct impulse_date d;

		impulse_civil_from_days(base_days + offset, &d);
		if (!impulse_event_occurs_on(e, &d, tz_offset_minutes)) {
			continue;
		}
		if (offset == 0 && now_minute >= e->start_time) {
			/* Already begun today — a window whose minute has
			 * passed is never entered (§5.3), so look further. */
			continue;
		}

		int64_t midnight =
			impulse_utc_from_local_midnight(&d, tz_offset_minutes);
		int64_t start = midnight + (int64_t)e->start_time * 60;

		return start - now_utc;
	}

	return INT64_MAX;
}
