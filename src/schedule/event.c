/* SPDX-License-Identifier: Apache-2.0 */
#include "event.h"

#include <string.h>

bool impulse_event_equal(const struct impulse_event *a,
			 const struct impulse_event *b)
{
	if (a == NULL || b == NULL) {
		return a == b;
	}

	/* Compared field by field rather than with memcmp: the struct has
	 * padding, and wifi_ssid carries bytes past ssid_len that are not part
	 * of the value. A memcmp would report spurious differences and, since
	 * "changed" routes through the §9.1 classifier, spurious differences
	 * become spurious quarantines. */
	if (!impulse_uuid_eq(a->id, b->id) ||
	    a->reference_date != b->reference_date ||
	    a->start_time != b->start_time || a->end_time != b->end_time ||
	    a->recurrence != b->recurrence ||
	    a->day_of_week != b->day_of_week ||
	    a->day_of_month != b->day_of_month ||
	    a->criteria != b->criteria || a->profile != b->profile ||
	    a->anchor_profile != b->anchor_profile || a->negate != b->negate ||
	    a->donning_grace_s != b->donning_grace_s ||
	    a->has_anchor_id != b->has_anchor_id ||
	    a->ssid_len != b->ssid_len ||
	    a->beep_anchor_count != b->beep_anchor_count) {
		return false;
	}

	if (a->has_anchor_id && !impulse_uuid_eq(a->anchor_id, b->anchor_id)) {
		return false;
	}

	if (a->ssid_len != 0U &&
	    memcmp(a->wifi_ssid, b->wifi_ssid, a->ssid_len) != 0) {
		return false;
	}

	for (uint8_t i = 0; i < a->beep_anchor_count; i++) {
		if (!impulse_uuid_eq(a->beep_anchors[i], b->beep_anchors[i])) {
			return false;
		}
	}

	return true;
}
