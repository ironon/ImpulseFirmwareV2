/*
 * Commitment integrity — canonical tighten/loosen classification.
 * firmware_spec_v2.md §9.1, inherited by v3 §0.2 "behaviour for behaviour".
 *
 * This is the single canonical definition on the device side. The app mirrors
 * these rules so its previews match, but ONLY the watch's verdict is binding:
 * the app is deletable, reinstallable, and replaceable by a generic BLE tool,
 * so any loosening guarantee implemented app-side is bypassable in minutes.
 * The app proposes; the watch disposes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_CLASSIFY_H_
#define IMPULSE_CLASSIFY_H_

#include <stdbool.h>

#include "../schedule/event.h"

enum impulse_change_class {
	IMPULSE_CHANGE_NONE = 0,     /* nothing actually changed */
	IMPULSE_CHANGE_TIGHTEN,      /* binds at least as hard — apply now */
	IMPULSE_CHANGE_LOOSEN,       /* binds less — quarantine */
	IMPULSE_CHANGE_NONCOMPARABLE /* cannot be proven >= — treat as loosen */
};

/*
 * Non-comparable is deliberately NOT a third outcome at the policy layer: if
 * the watch cannot prove a change binds at least as hard, the change is not
 * instant. Kept distinct from LOOSEN only so the reason can be reported.
 */
static inline bool impulse_change_is_instant(enum impulse_change_class c)
{
	return c == IMPULSE_CHANGE_NONE || c == IMPULSE_CHANGE_TIGHTEN;
}

/* Relations used by the field comparisons below. */
enum impulse_rel {
	IMPULSE_REL_EQUAL = 0,
	IMPULSE_REL_SUPERSET,   /* new covers everything old did, and more */
	IMPULSE_REL_SUBSET,     /* new covers less */
	IMPULSE_REL_INCOMPARABLE
};

enum impulse_rel impulse_rel_window(const struct impulse_event *old_e,
				    const struct impulse_event *new_e);
enum impulse_rel impulse_rel_recurrence(const struct impulse_event *old_e,
					const struct impulse_event *new_e,
					int16_t tz_offset_minutes);
enum impulse_rel impulse_rel_beep_anchors(const struct impulse_event *old_e,
					  const struct impulse_event *new_e);
enum impulse_rel impulse_rel_profile(uint8_t old_p, uint8_t new_p);

/* An event appearing in the proposed schedule that was not in the current one.
 * Adding is a tightening — EXCEPT a negate event, which cancels a day and is
 * therefore always a loosening (§9.1 final row). */
enum impulse_change_class impulse_classify_added(const struct impulse_event *e);

/* An event present in the current schedule and absent from the proposal. */
enum impulse_change_class impulse_classify_deleted(
	const struct impulse_event *e);

/*
 * Multi-field edit of one event. Tightening only if EVERY changed field
 * classifies as tightening; a single loosening or non-comparable field makes
 * the whole change a loosening (§9.1 "Multi-field edits").
 */
enum impulse_change_class impulse_classify_modified(
	const struct impulse_event *old_e, const struct impulse_event *new_e,
	int16_t tz_offset_minutes);

#endif /* IMPULSE_CLASSIFY_H_ */
