/*
 * Shared data structures — firmware_spec_v2.md §3.1, §3.2, §3.3.
 * Inherited by the nRF build unchanged (v3 §0.2): pure data, no platform
 * surface. These enum values are ON THE WIRE (§6.2) and are mirrored in
 * impulse_app/lib/utils/ble_constants.dart and phone_sim/constants.py.
 * Changing a number here breaks all four declarations of the contract.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_SCHEDULE_EVENT_H_
#define IMPULSE_SCHEDULE_EVENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMPULSE_UUID_LEN 16
#define IMPULSE_MAX_SSID_LEN 32
#define IMPULSE_MAX_BEEP_ANCHORS 8

/* §3.1 — wire values, do not renumber. */
enum impulse_recurrence {
	IMPULSE_RECUR_ONCE = 0,
	IMPULSE_RECUR_DAILY = 1,
	IMPULSE_RECUR_WEEKLY = 2,
	IMPULSE_RECUR_MONTHLY = 3,
};

enum impulse_criteria {
	IMPULSE_CRIT_GET_AWAY = 0,    /* user must be AWAY from anchor_id */
	IMPULSE_CRIT_STAY_NEAR = 1,   /* user must be NEAR anchor_id */
	IMPULSE_CRIT_GET_OFF_WIFI = 2,
	IMPULSE_CRIT_GET_ON_WIFI = 3,
	IMPULSE_CRIT_PHONE_AWAY = 4,  /* Mode B: phone docked at anchor_id */
};

enum impulse_profile {
	IMPULSE_PROFILE_STRICT_SILENT = 0,
	IMPULSE_PROFILE_NORMAL_SILENT = 1,
	IMPULSE_PROFILE_LOOSE_SILENT = 2,
	IMPULSE_PROFILE_STRICT_BOTH = 3,
	IMPULSE_PROFILE_NORMAL_BOTH = 4,
	IMPULSE_PROFILE_LOOSE_BOTH = 5,
	IMPULSE_PROFILE_STRICT_BUZZ = 6,
	IMPULSE_PROFILE_NORMAL_BUZZ = 7,
	IMPULSE_PROFILE_LOOSE_BUZZ = 8,
	IMPULSE_PROFILE_COUNT = 9,
};

enum impulse_anchor_profile {
	IMPULSE_ANCHOR_PROFILE_LIGHT = 0,  /* beep 3s, wait 60s, repeat */
	IMPULSE_ANCHOR_PROFILE_MEDIUM = 1, /* beep 3s, wait 30s, repeat */
	IMPULSE_ANCHOR_PROFILE_HARD = 2,   /* beep 4s, wait 10s, repeat */
	IMPULSE_ANCHOR_PROFILE_NONE = 0xFF,
};

/* §3.2. Field order mirrors the spec so the two can be diffed by eye. */
struct impulse_event {
	uint8_t id[IMPULSE_UUID_LEN];
	int64_t reference_date;   /* Unix seconds, UTC */
	uint16_t start_time;      /* minutes since local midnight, 0..1439 */
	uint16_t end_time;        /* minutes since local midnight, > start_time */
	uint8_t recurrence;       /* enum impulse_recurrence */
	uint8_t day_of_week;      /* 1..7 Mon..Sun if weekly, else 0 */
	uint8_t day_of_month;     /* 1..31 if monthly, else 0 */
	uint8_t criteria;         /* enum impulse_criteria */
	uint8_t profile;          /* enum impulse_profile */
	uint8_t anchor_profile;   /* enum impulse_anchor_profile, 0xFF if none */
	bool negate;              /* cancels a recurring event for one day */
	uint16_t donning_grace_s; /* 0..1800, §5.4.4 */

	bool has_anchor_id;
	uint8_t anchor_id[IMPULSE_UUID_LEN];

	uint8_t ssid_len;
	char wifi_ssid[IMPULSE_MAX_SSID_LEN + 1];

	uint8_t beep_anchor_count;
	uint8_t beep_anchors[IMPULSE_MAX_BEEP_ANCHORS][IMPULSE_UUID_LEN];
};

/* §3.3 — watch-side record of a known anchor. */
struct impulse_anchor_record {
	uint8_t uuid[IMPULSE_UUID_LEN];
	char name[32];
	uint8_t ble_addr[6];
	int8_t last_rssi;
	int64_t last_seen;
	uint32_t ipv4;          /* 0 = unknown */
	int64_t ip_last_updated;

	/* Per-anchor CS threshold override (v3 §4.7). 0 = use the defaults.
	 * RAISING these is a LOOSENING and is quarantined like any other
	 * (v3 §4.7) — a threshold the user can widen instantly would be a
	 * one-tap bypass of every stayNear commitment. */
	uint16_t near_enter_cm;
	uint16_t away_enter_cm;
};

static inline bool impulse_uuid_eq(const uint8_t *a, const uint8_t *b)
{
	for (size_t i = 0; i < IMPULSE_UUID_LEN; i++) {
		if (a[i] != b[i]) {
			return false;
		}
	}
	return true;
}

static inline bool impulse_uuid_is_zero(const uint8_t *a)
{
	for (size_t i = 0; i < IMPULSE_UUID_LEN; i++) {
		if (a[i] != 0U) {
			return false;
		}
	}
	return true;
}

/* True when this criterion is answered by ranging to an anchor rather than by
 * WiFi association. phoneAway is included: it ranges to the dock anchor and
 * fuses the result with dock status (§5.4.1 Mode B). */
static inline bool impulse_criteria_is_anchor_based(uint8_t c)
{
	return c == IMPULSE_CRIT_GET_AWAY || c == IMPULSE_CRIT_STAY_NEAR ||
	       c == IMPULSE_CRIT_PHONE_AWAY;
}

static inline bool impulse_criteria_is_wifi_based(uint8_t c)
{
	return c == IMPULSE_CRIT_GET_OFF_WIFI || c == IMPULSE_CRIT_GET_ON_WIFI;
}

bool impulse_event_equal(const struct impulse_event *a,
			 const struct impulse_event *b);

#endif /* IMPULSE_SCHEDULE_EVENT_H_ */
