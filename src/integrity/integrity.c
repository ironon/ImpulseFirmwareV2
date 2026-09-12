/* SPDX-License-Identifier: Apache-2.0 */
#include "integrity.h"

#include <string.h>

#ifdef __ZEPHYR__
#include <zephyr/kernel.h>
#endif

/*
 * Persisted elapsed-time base. Zephyr's uptime resets on reboot, so a base is
 * added and re-persisted to give a counter that spans reboots — the same
 * pattern the ESP32 build used for usage instrumentation (§9.2).
 * TODO(storage): storage.c must load this at boot and persist it periodically
 * and on clean shutdown. Until it does, a reboot rewinds every integrity timer
 * to zero, which is a LOOSENING bug: a pending change would restart its 24 h,
 * but a settle window would also restart, making edits free again. Wire it up
 * before this is trusted on real hardware.
 */
static uint64_t s_elapsed_base_s;

void impulse_integrity_set_elapsed_base(uint64_t base_s)
{
	s_elapsed_base_s = base_s;
}

uint64_t impulse_elapsed_now_s(void)
{
#ifdef __ZEPHYR__
	return s_elapsed_base_s + (uint64_t)(k_uptime_get() / 1000);
#else
	/* Host tests inject time through impulse_integrity_set_elapsed_base(). */
	return s_elapsed_base_s;
#endif
}

void impulse_integrity_init(struct impulse_integrity *ig)
{
	memset(ig, 0, sizeof(*ig));
	ig->settle_window_min = IMPULSE_SETTLE_WINDOW_DEFAULT_MIN;
	ig->pass_allowance = IMPULSE_PASS_BUDGET_DEFAULT;
}

/* --- helpers ------------------------------------------------------------- */

static const struct impulse_event *find_event(
	const struct impulse_schedule *s, const uint8_t *id)
{
	for (uint16_t i = 0; i < s->count; i++) {
		if (impulse_uuid_eq(s->events[i].id, id)) {
			return &s->events[i];
		}
	}
	return NULL;
}

static struct impulse_settle *settle_for(struct impulse_integrity *ig,
					 const uint8_t *id, bool create)
{
	struct impulse_settle *free_slot = NULL;

	for (size_t i = 0; i < CONFIG_IMPULSE_MAX_EVENTS; i++) {
		if (ig->settle[i].in_use &&
		    impulse_uuid_eq(ig->settle[i].event_id, id)) {
			return &ig->settle[i];
		}
		if (!ig->settle[i].in_use && free_slot == NULL) {
			free_slot = &ig->settle[i];
		}
	}
	if (create && free_slot != NULL) {
		memset(free_slot, 0, sizeof(*free_slot));
		free_slot->in_use = true;
		memcpy(free_slot->event_id, id, IMPULSE_UUID_LEN);
		return free_slot;
	}
	return NULL;
}

/*
 * §9.2: an event settles when settle_window_min elapsed minutes pass with no
 * accepted change; at that instant its current state becomes the baseline.
 * Called lazily rather than on a timer — the result is identical and it needs
 * no wakeups.
 */
static void settle_tick(struct impulse_integrity *ig,
			const struct impulse_schedule *current, uint64_t now_s)
{
	uint64_t window_s = (uint64_t)ig->settle_window_min * 60U;

	for (size_t i = 0; i < CONFIG_IMPULSE_MAX_EVENTS; i++) {
		struct impulse_settle *st = &ig->settle[i];

		if (!st->in_use || st->has_baseline) {
			continue;
		}
		if (now_s < st->last_edit_elapsed_s + window_s) {
			continue;
		}

		const struct impulse_event *e = find_event(current,
							   st->event_id);

		if (e != NULL) {
			st->settled_baseline = *e;
			st->has_baseline = true;
		}
	}
}

/*
 * Any event in the schedule that has no settle entry is SEALED: an entry is
 * created for it with its current state as the baseline, i.e. treated as
 * already settled.
 *
 * This closes a fail-open. §9.3 grants a free edit window to an event with no
 * baseline yet — that is the genuine first-setup case, and it is reached by
 * ADDING an event, which records an edit and starts its settle window. But an
 * event that is in the schedule while its integrity state is missing (storage
 * desync, an integrity record that failed to load, a downgrade) would take the
 * same path and be freely loosenable forever. Sealing it instead means the
 * worst case is an event that cannot be loosened instantly, which is the safe
 * direction for a commitment device.
 */
static void seal_unknown_events(struct impulse_integrity *ig,
				const struct impulse_schedule *current,
				uint64_t now_s)
{
	for (uint16_t i = 0; i < current->count; i++) {
		const struct impulse_event *e = &current->events[i];

		if (settle_for(ig, e->id, false) != NULL) {
			continue;
		}

		struct impulse_settle *st = settle_for(ig, e->id, true);

		if (st != NULL) {
			st->last_edit_elapsed_s = now_s;
			st->settled_baseline = *e;
			st->has_baseline = true;
		}
	}
}

static void note_edit(struct impulse_integrity *ig, const uint8_t *id,
		      uint64_t now_s)
{
	struct impulse_settle *st = settle_for(ig, id, true);

	if (st != NULL) {
		st->last_edit_elapsed_s = now_s;
		/* An accepted change re-opens the settle window: the event is
		 * no longer settled, and its old baseline no longer describes
		 * it. */
		st->has_baseline = false;
	}
}

/* Cancel any pending entries targeting `id` — the newer intent wins (§9.3).
 * Only ever binds harder, so it opens no escape. */
static uint16_t cancel_pending_for(struct impulse_integrity *ig,
				   const uint8_t *id)
{
	uint16_t n = 0;

	for (size_t i = 0; i < CONFIG_IMPULSE_PENDING_QUEUE_MAX; i++) {
		if (ig->pending[i].in_use &&
		    impulse_uuid_eq(ig->pending[i].target_id, id)) {
			ig->pending[i].in_use = false;
			n++;
		}
	}
	return n;
}

static bool queue_push(struct impulse_integrity *ig,
		       const struct impulse_pending *p)
{
	for (size_t i = 0; i < CONFIG_IMPULSE_PENDING_QUEUE_MAX; i++) {
		if (!ig->pending[i].in_use) {
			ig->pending[i] = *p;
			ig->pending[i].in_use = true;
			return true;
		}
	}
	/* Queue full: the loosening is DROPPED, not applied (§9.3 step 5).
	 * The app sees the truth through the Pending Changes characteristic. */
	return false;
}

static bool sched_upsert(struct impulse_schedule *s,
			 const struct impulse_event *e)
{
	for (uint16_t i = 0; i < s->count; i++) {
		if (impulse_uuid_eq(s->events[i].id, e->id)) {
			s->events[i] = *e;
			return true;
		}
	}
	if (s->count >= CONFIG_IMPULSE_MAX_EVENTS) {
		return false;
	}
	s->events[s->count++] = *e;
	return true;
}

static void sched_delete(struct impulse_schedule *s, const uint8_t *id)
{
	for (uint16_t i = 0; i < s->count; i++) {
		if (impulse_uuid_eq(s->events[i].id, id)) {
			for (uint16_t j = i + 1; j < s->count; j++) {
				s->events[j - 1] = s->events[j];
			}
			s->count--;
			return;
		}
	}
}

/*
 * §9.3 "apply immediately" third bullet: a change to an UNSETTLED event whose
 * resulting state is still at least as binding as its settled_baseline (or
 * which has no baseline at all — the free first-setup window).
 */
static bool within_free_edit_window(struct impulse_integrity *ig,
				    const struct impulse_event *proposed,
				    int16_t tz_offset_minutes)
{
	struct impulse_settle *st = settle_for(ig, proposed->id, false);

	if (st == NULL || !st->has_baseline) {
		return true; /* never settled — first-setup window */
	}

	enum impulse_change_class c = impulse_classify_modified(
		&st->settled_baseline, proposed, tz_offset_minutes);

	return impulse_change_is_instant(c);
}

/* §9.3 no-escape exception: loosening an event whose next occurrence is more
 * than LOOSEN_FREE_HORIZON_H away grants no in-the-moment escape. */
static bool beyond_free_horizon(const struct impulse_event *e,
				const struct impulse_event *active,
				int64_t now_utc, int16_t tz_offset_minutes)
{
	if (active != NULL && impulse_uuid_eq(active->id, e->id)) {
		return false; /* currently active is never "far future" */
	}

	int64_t secs = impulse_seconds_until_next_start(e, now_utc,
							tz_offset_minutes);

	return secs > (int64_t)IMPULSE_LOOSEN_FREE_HORIZON_H * 3600;
}

void impulse_integrity_seal_unknown(struct impulse_integrity *ig,
				    const struct impulse_schedule *current)
{
	seal_unknown_events(ig, current, impulse_elapsed_now_s());
}

uint8_t impulse_integrity_apply_push(struct impulse_integrity *ig,
				     struct impulse_schedule *current,
				     const struct impulse_schedule *proposed,
				     const struct impulse_event *active,
				     int64_t now_utc, int16_t tz_offset_minutes,
				     struct impulse_diff_report *report)
{
	uint64_t now_s = impulse_elapsed_now_s();
	struct impulse_diff_report rep = {0};

	seal_unknown_events(ig, current, now_s);
	settle_tick(ig, current, now_s);

	/*
	 * §9.3 "Phase-1 minimum": reject outright any push that would loosen
	 * the CURRENTLY ACTIVE event. This is the check that closes the "push
	 * an empty schedule at 6am" bypass, and it is deliberately a whole-push
	 * rejection rather than a per-event quarantine.
	 *
	 * NOTE this is kept even though the full diff gate below would
	 * quarantine such a change anyway: the spec defines 0x04 as a distinct
	 * verdict the app must be able to see, and rejecting the whole push is
	 * a stronger guarantee than quarantining one event out of it.
	 */
	if (active != NULL) {
		const struct impulse_event *pa = find_event(proposed,
							    active->id);
		enum impulse_change_class c;

		if (pa == NULL) {
			c = impulse_classify_deleted(active);
		} else {
			c = impulse_classify_modified(active, pa,
						      tz_offset_minutes);
		}
		if (!impulse_change_is_instant(c)) {
			rep.rejected_active_event = true;
			if (report != NULL) {
				*report = rep;
			}
			return IMPULSE_END_REJECTED;
		}
	}

	/* Deletions: present in current, absent from proposed. Walk backwards
	 * so in-place removal does not skip entries. */
	for (int32_t i = (int32_t)current->count - 1; i >= 0; i--) {
		const struct impulse_event *cur = &current->events[i];

		if (find_event(proposed, cur->id) != NULL) {
			continue;
		}

		uint8_t id[IMPULSE_UUID_LEN];

		memcpy(id, cur->id, IMPULSE_UUID_LEN);

		struct impulse_event snapshot = *cur;

		if (beyond_free_horizon(&snapshot, active, now_utc,
					tz_offset_minutes)) {
			sched_delete(current, id);
			note_edit(ig, id, now_s);
			(void)cancel_pending_for(ig, id);
			rep.applied++;
			continue;
		}

		struct impulse_pending p = {
			.kind = IMPULSE_PENDING_DELETE,
			.apply_after_elapsed_s =
				now_s + (uint64_t)IMPULSE_LOOSEN_DELAY_H * 3600U,
		};

		memcpy(p.target_id, id, IMPULSE_UUID_LEN);
		(void)cancel_pending_for(ig, id);
		if (queue_push(ig, &p)) {
			rep.quarantined++;
		} else {
			rep.dropped_queue_full++;
		}
	}

	/* Additions and modifications. */
	for (uint16_t i = 0; i < proposed->count; i++) {
		const struct impulse_event *pe = &proposed->events[i];
		const struct impulse_event *ce = find_event(current, pe->id);
		enum impulse_change_class c;

		if (ce == NULL) {
			c = impulse_classify_added(pe);
		} else {
			c = impulse_classify_modified(ce, pe,
						      tz_offset_minutes);
		}

		if (c == IMPULSE_CHANGE_NONE) {
			continue;
		}

		bool instant = impulse_change_is_instant(c) ||
			       within_free_edit_window(ig, pe,
						       tz_offset_minutes) ||
			       beyond_free_horizon(pe, active, now_utc,
						   tz_offset_minutes);

		if (instant) {
			if (sched_upsert(current, pe)) {
				note_edit(ig, pe->id, now_s);
				(void)cancel_pending_for(ig, pe->id);
				rep.applied++;
			} else {
				rep.dropped_queue_full++;
			}
			continue;
		}

		struct impulse_pending p = {
			.kind = IMPULSE_PENDING_UPSERT,
			.apply_after_elapsed_s =
				now_s + (uint64_t)IMPULSE_LOOSEN_DELAY_H * 3600U,
			.proposed = *pe,
		};

		memcpy(p.target_id, pe->id, IMPULSE_UUID_LEN);
		(void)cancel_pending_for(ig, pe->id);
		if (queue_push(ig, &p)) {
			rep.quarantined++;
		} else {
			rep.dropped_queue_full++;
		}
	}

	if (report != NULL) {
		*report = rep;
	}

	if (rep.quarantined > 0U || rep.dropped_queue_full > 0U) {
		return IMPULSE_END_QUARANTINED;
	}
	return IMPULSE_END_ACCEPTED;
}

uint16_t impulse_integrity_promote(struct impulse_integrity *ig,
				   struct impulse_schedule *current,
				   int64_t now_utc, int16_t tz_offset_minutes)
{
	uint64_t now_s = impulse_elapsed_now_s();
	uint16_t promoted = 0;

	for (size_t i = 0; i < CONFIG_IMPULSE_PENDING_QUEUE_MAX; i++) {
		struct impulse_pending *p = &ig->pending[i];

		if (!p->in_use || now_s < p->apply_after_elapsed_s) {
			continue;
		}

		switch (p->kind) {
		case IMPULSE_PENDING_DELETE:
			sched_delete(current, p->target_id);
			promoted++;
			break;
		case IMPULSE_PENDING_UPSERT:
			/* §9.4: entries whose effect has already expired are
			 * dropped, not applied — e.g. a negate for a date that
			 * has passed. Applying it would be harmless but would
			 * leave dead state in the schedule. */
			if (p->proposed.recurrence == IMPULSE_RECUR_ONCE) {
				int64_t until =
					impulse_seconds_until_next_start(
						&p->proposed, now_utc,
						tz_offset_minutes);

				if (until == INT64_MAX) {
					p->in_use = false;
					continue;
				}
			}
			(void)sched_upsert(current, &p->proposed);
			promoted++;
			break;
		case IMPULSE_PENDING_SETTING:
			/* TODO(settings): §9.8 gated settings (dormancy flags,
			 * settle window, pass allowance) are not wired to
			 * storage yet. The queue carries them correctly; the
			 * apply step is missing. */
			promoted++;
			break;
		default:
			break;
		}

		p->in_use = false;
		note_edit(ig, p->target_id, now_s);
	}

	return promoted;
}

bool impulse_integrity_spend_pass(struct impulse_integrity *ig,
				  const uint8_t *event_id,
				  uint32_t date_yyyymmdd,
				  struct impulse_schedule *current,
				  uint8_t *remaining_out)
{
	uint64_t now_s = impulse_elapsed_now_s();
	uint64_t window_s = (uint64_t)IMPULSE_PASS_WINDOW_DAYS * 86400U;
	uint8_t in_window = 0;

	/* Age out spends older than the rolling window, compacting as we go. */
	uint8_t w = 0;

	for (uint8_t i = 0; i < ig->pass_spend_count; i++) {
		if (ig->pass_spend_elapsed_s[i] + window_s > now_s) {
			ig->pass_spend_elapsed_s[w++] =
				ig->pass_spend_elapsed_s[i];
		}
	}
	ig->pass_spend_count = w;
	in_window = w;

	if (in_window >= ig->pass_allowance) {
		if (remaining_out != NULL) {
			*remaining_out = 0;
		}
		return false;
	}

	/* Build the one-day negate and apply it IMMEDIATELY, bypassing §9.3.
	 * The pass is the sanctioned escape valve; gating it would defeat its
	 * entire purpose. */
	const struct impulse_event *target = find_event(current, event_id);

	if (target == NULL) {
		if (remaining_out != NULL) {
			*remaining_out = ig->pass_allowance - in_window;
		}
		return false;
	}

	struct impulse_event neg = *target;

	neg.recurrence = IMPULSE_RECUR_ONCE;
	neg.negate = true;
	neg.day_of_week = 0;
	neg.day_of_month = 0;

	struct impulse_date d = {
		.year = (int32_t)(date_yyyymmdd / 10000U),
		.month = (uint8_t)((date_yyyymmdd / 100U) % 100U),
		.day = (uint8_t)(date_yyyymmdd % 100U),
	};

	/* Noon local, so the instant is unambiguously inside the target day
	 * regardless of the timezone offset in force when it is read back. */
	neg.reference_date = impulse_utc_from_local_midnight(&d, 0) + 12 * 3600;

	if (!sched_upsert(current, &neg)) {
		return false;
	}

	if (ig->pass_spend_count <
	    (uint8_t)(sizeof(ig->pass_spend_elapsed_s) /
		      sizeof(ig->pass_spend_elapsed_s[0]))) {
		ig->pass_spend_elapsed_s[ig->pass_spend_count++] = now_s;
	}

	if (remaining_out != NULL) {
		uint8_t used = ig->pass_spend_count;

		*remaining_out = (used >= ig->pass_allowance)
					 ? 0U
					 : (uint8_t)(ig->pass_allowance - used);
	}
	return true;
}

bool impulse_integrity_time_change_allowed(const struct impulse_event *active,
					   int64_t old_utc, int16_t old_tz,
					   int64_t new_utc, int16_t new_tz)
{
	uint16_t old_min, new_min;

	if (active == NULL) {
		return true;
	}

	impulse_local_from_utc(old_utc, old_tz, NULL, &old_min);
	impulse_local_from_utc(new_utc, new_tz, NULL, &new_min);

	bool was_inside = (old_min >= active->start_time &&
			   old_min < active->end_time);
	bool now_inside = (new_min >= active->start_time &&
			   new_min < active->end_time);

	/* §9.7: reject a change that would END or SKIP the active window.
	 * Moving further INTO the window is fine. */
	return !(was_inside && !now_inside);
}
