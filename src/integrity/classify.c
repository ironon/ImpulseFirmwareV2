/* SPDX-License-Identifier: Apache-2.0 */
#include "classify.h"

#include <string.h>

#include "../schedule/schedule.h"

/* --- profile partial order ------------------------------------------------
 *
 * Two INDEPENDENT dimensions (§9.1):
 *   strictness: strict > normal > loose
 *   output:     both > buzz, both > silent; buzz vs silent NON-COMPARABLE
 *
 * The enum is laid out as output-major, strictness-minor:
 *   0,1,2 = strict/normal/loose SILENT
 *   3,4,5 = strict/normal/loose BOTH
 *   6,7,8 = strict/normal/loose BUZZ
 * so both dimensions fall out of divmod 3. A change is tightening only if it
 * is >= in BOTH dimensions and > in at least one.
 */
static int profile_strictness(uint8_t p)
{
	/* 0 = strict -> rank 2 (hardest). */
	return 2 - (int)(p % 3U);
}

static int profile_output(uint8_t p)
{
	return (int)(p / 3U); /* 0 silent, 1 both, 2 buzz */
}

enum impulse_rel impulse_rel_profile(uint8_t old_p, uint8_t new_p)
{
	if (old_p == new_p) {
		return IMPULSE_REL_EQUAL;
	}
	if (old_p >= IMPULSE_PROFILE_COUNT || new_p >= IMPULSE_PROFILE_COUNT) {
		return IMPULSE_REL_INCOMPARABLE;
	}

	int so = profile_strictness(old_p);
	int sn = profile_strictness(new_p);
	int oo = profile_output(old_p);
	int on = profile_output(new_p);

	/* Output dimension: 'both' dominates each of the others; 'buzz' and
	 * 'silent' are siblings with no order between them. */
	int out_cmp;

	if (oo == on) {
		out_cmp = 0;
	} else if (on == 1) {
		out_cmp = 1; /* -> both: strictly more output */
	} else if (oo == 1) {
		out_cmp = -1; /* both -> something narrower */
	} else {
		return IMPULSE_REL_INCOMPARABLE; /* buzz <-> silent */
	}

	int str_cmp = (sn > so) ? 1 : ((sn < so) ? -1 : 0);

	if (str_cmp >= 0 && out_cmp >= 0) {
		return IMPULSE_REL_SUPERSET; /* tighter or equal in both */
	}
	if (str_cmp <= 0 && out_cmp <= 0) {
		return IMPULSE_REL_SUBSET;
	}
	return IMPULSE_REL_INCOMPARABLE;
}

/* --- window --------------------------------------------------------------
 * A LONGER enforcement window binds harder, so new ⊇ old is a tightening.
 */
enum impulse_rel impulse_rel_window(const struct impulse_event *old_e,
				    const struct impulse_event *new_e)
{
	if (old_e->start_time == new_e->start_time &&
	    old_e->end_time == new_e->end_time) {
		return IMPULSE_REL_EQUAL;
	}
	if (new_e->start_time <= old_e->start_time &&
	    new_e->end_time >= old_e->end_time) {
		return IMPULSE_REL_SUPERSET;
	}
	if (new_e->start_time >= old_e->start_time &&
	    new_e->end_time <= old_e->end_time) {
		return IMPULSE_REL_SUBSET;
	}
	/* Both edges moved the same way — a shift, not a resize. */
	return IMPULSE_REL_INCOMPARABLE;
}

/* --- recurrence ----------------------------------------------------------
 *
 * Comparing occurrence SETS exactly would mean enumerating the calendar. This
 * only claims SUPERSET (the tightening, and therefore dangerous, direction)
 * where it is provable from the rule alone; everything else falls to
 * INCOMPARABLE, which the policy layer treats as a loosening. Claiming SUBSET
 * too eagerly is safe — it only ever quarantines.
 */
enum impulse_rel impulse_rel_recurrence(const struct impulse_event *old_e,
					const struct impulse_event *new_e,
					int16_t tz_offset_minutes)
{
	uint8_t o = old_e->recurrence;
	uint8_t n = new_e->recurrence;

	if (o == n) {
		switch (o) {
		case IMPULSE_RECUR_DAILY:
			return IMPULSE_REL_EQUAL;
		case IMPULSE_RECUR_WEEKLY:
			return (old_e->day_of_week == new_e->day_of_week)
				       ? IMPULSE_REL_EQUAL
				       : IMPULSE_REL_INCOMPARABLE;
		case IMPULSE_RECUR_MONTHLY:
			return (old_e->day_of_month == new_e->day_of_month)
				       ? IMPULSE_REL_EQUAL
				       : IMPULSE_REL_INCOMPARABLE;
		case IMPULSE_RECUR_ONCE: {
			struct impulse_date od, nd;

			impulse_local_from_utc(old_e->reference_date,
					       tz_offset_minutes, &od, NULL);
			impulse_local_from_utc(new_e->reference_date,
					       tz_offset_minutes, &nd, NULL);
			return (od.year == nd.year && od.month == nd.month &&
				od.day == nd.day)
				       ? IMPULSE_REL_EQUAL
				       : IMPULSE_REL_INCOMPARABLE;
		}
		default:
			return IMPULSE_REL_INCOMPARABLE;
		}
	}

	/* daily occurs on every day, so it is a superset of every other rule.
	 * This is the ONLY superset claim made across differing types. */
	if (n == IMPULSE_RECUR_DAILY) {
		return IMPULSE_REL_SUPERSET;
	}
	if (o == IMPULSE_RECUR_DAILY) {
		return IMPULSE_REL_SUBSET;
	}

	/* A single day is a subset of any repeating rule. Safe to claim: subset
	 * means loosening, which only ever quarantines. */
	if (n == IMPULSE_RECUR_ONCE) {
		return IMPULSE_REL_SUBSET;
	}

	/* once -> weekly/monthly could be a superset, but only if the original
	 * date is actually covered by the new rule. Not proven here. */
	return IMPULSE_REL_INCOMPARABLE;
}

/* --- beep anchors (set comparison) --------------------------------------- */
static bool beep_contains(const struct impulse_event *e, const uint8_t *uuid)
{
	for (uint8_t i = 0; i < e->beep_anchor_count; i++) {
		if (impulse_uuid_eq(e->beep_anchors[i], uuid)) {
			return true;
		}
	}
	return false;
}

enum impulse_rel impulse_rel_beep_anchors(const struct impulse_event *old_e,
					  const struct impulse_event *new_e)
{
	bool new_has_all_old = true;
	bool old_has_all_new = true;

	for (uint8_t i = 0; i < old_e->beep_anchor_count; i++) {
		if (!beep_contains(new_e, old_e->beep_anchors[i])) {
			new_has_all_old = false;
			break;
		}
	}
	for (uint8_t i = 0; i < new_e->beep_anchor_count; i++) {
		if (!beep_contains(old_e, new_e->beep_anchors[i])) {
			old_has_all_new = false;
			break;
		}
	}

	if (new_has_all_old && old_has_all_new) {
		return IMPULSE_REL_EQUAL;
	}
	if (new_has_all_old) {
		return IMPULSE_REL_SUPERSET;
	}
	if (old_has_all_new) {
		return IMPULSE_REL_SUBSET;
	}
	return IMPULSE_REL_INCOMPARABLE;
}

/* --- whole-event classification ------------------------------------------ */

enum impulse_change_class impulse_classify_added(const struct impulse_event *e)
{
	/* §9.1 last row: a one-time negate added against a recurring event
	 * cancels a day. It arrives as an ADDED event but is a loosening, and
	 * treating "added" as unconditionally tightening would make cancelling
	 * any day a one-tap instant escape. */
	if (e->negate) {
		return IMPULSE_CHANGE_LOOSEN;
	}
	return IMPULSE_CHANGE_TIGHTEN;
}

enum impulse_change_class impulse_classify_deleted(
	const struct impulse_event *e)
{
	(void)e;
	return IMPULSE_CHANGE_LOOSEN;
}

/* Fold one field's relation into the running verdict. */
static void fold(enum impulse_rel rel, bool *any_tighten, bool *not_instant)
{
	switch (rel) {
	case IMPULSE_REL_EQUAL:
		break;
	case IMPULSE_REL_SUPERSET:
		*any_tighten = true;
		break;
	case IMPULSE_REL_SUBSET:
	case IMPULSE_REL_INCOMPARABLE:
	default:
		*not_instant = true;
		break;
	}
}

enum impulse_change_class impulse_classify_modified(
	const struct impulse_event *old_e, const struct impulse_event *new_e,
	int16_t tz_offset_minutes)
{
	bool any_tighten = false;
	bool not_instant = false;

	if (impulse_event_equal(old_e, new_e)) {
		return IMPULSE_CHANGE_NONE;
	}

	fold(impulse_rel_window(old_e, new_e), &any_tighten, &not_instant);
	fold(impulse_rel_recurrence(old_e, new_e, tz_offset_minutes),
	     &any_tighten, &not_instant);
	fold(impulse_rel_profile(old_e->profile, new_e->profile), &any_tighten,
	     &not_instant);
	fold(impulse_rel_beep_anchors(old_e, new_e), &any_tighten,
	     &not_instant);

	/* criteria and target are NON-COMPARABLE on any change (§9.1): a
	 * different criterion or a different anchor is a different commitment,
	 * not a stronger or weaker one. */
	if (old_e->criteria != new_e->criteria) {
		not_instant = true;
	}
	if (old_e->has_anchor_id != new_e->has_anchor_id ||
	    (new_e->has_anchor_id &&
	     !impulse_uuid_eq(old_e->anchor_id, new_e->anchor_id))) {
		not_instant = true;
	}
	if (old_e->ssid_len != new_e->ssid_len ||
	    (new_e->ssid_len != 0U &&
	     memcmp(old_e->wifi_ssid, new_e->wifi_ssid, new_e->ssid_len) != 0)) {
		not_instant = true;
	}

	/* anchorProfile: hard > medium > light. NONE is "no beeping", the
	 * weakest state, so it orders below light. */
	if (old_e->anchor_profile != new_e->anchor_profile) {
		int o = (old_e->anchor_profile == IMPULSE_ANCHOR_PROFILE_NONE)
				? -1
				: (int)old_e->anchor_profile;
		int n = (new_e->anchor_profile == IMPULSE_ANCHOR_PROFILE_NONE)
				? -1
				: (int)new_e->anchor_profile;

		if (n > o) {
			any_tighten = true;
		} else {
			not_instant = true;
		}
	}

	/* donningGraceS: a SHORTER grace binds harder (§5.4.4 classification). */
	if (old_e->donning_grace_s != new_e->donning_grace_s) {
		if (new_e->donning_grace_s < old_e->donning_grace_s) {
			any_tighten = true;
		} else {
			not_instant = true;
		}
	}

	/* Turning negate on cancels the day. */
	if (!old_e->negate && new_e->negate) {
		not_instant = true;
	} else if (old_e->negate && !new_e->negate) {
		any_tighten = true;
	}

	if (not_instant) {
		return IMPULSE_CHANGE_LOOSEN;
	}
	return any_tighten ? IMPULSE_CHANGE_TIGHTEN : IMPULSE_CHANGE_NONE;
}
