/* SPDX-License-Identifier: Apache-2.0 */
#include "anchor.h"

#include <string.h>

#include "../app_api.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../hal/hal.h"
#include "../schedule/event.h"

LOG_MODULE_REGISTER(impulse_anchor, LOG_LEVEL_INF);

#define IDENTIFY_BEEP_DURATION_MS 800

static uint8_t anchor_uuid[IMPULSE_UUID_LEN];
static struct impulse_anchor_beep beep_state;

/* §4.8 patterns. on_ms / off_ms per profile. */
static const struct {
	uint32_t on_ms;
	uint32_t off_ms;
} beep_patterns[] = {
	[IMPULSE_ANCHOR_PROFILE_LIGHT] = {3000, 60000},
	[IMPULSE_ANCHOR_PROFILE_MEDIUM] = {3000, 30000},
	[IMPULSE_ANCHOR_PROFILE_HARD] = {4000, 10000},
};

const uint8_t *impulse_anchor_uuid(void)
{
	return anchor_uuid;
}

void impulse_anchor_init(void)
{
	memset(&beep_state, 0, sizeof(beep_state));

	/*
	 * §4.2 identity. Derived from FICR DEVICEID so it is stable across
	 * reboots without needing storage to have loaded yet, and unique per
	 * physical board. storage.c overwrites this with the persisted value if
	 * one exists, so a factory reset genuinely re-identifies the anchor.
	 *
	 * FICR base 0x00FFC000, INFO at +0x300, DEVICEID at +0x304 (2 words).
	 */
	const volatile uint32_t *deviceid =
		(const volatile uint32_t *)0x00FFC304UL;
	uint32_t a = deviceid[0];
	uint32_t b = deviceid[1];

	for (int i = 0; i < IMPULSE_UUID_LEN; i++) {
		uint32_t src = (i < 8) ? a : b;

		anchor_uuid[i] = (uint8_t)((src >> ((i % 4) * 8)) & 0xFFU) ^
				 (uint8_t)(0x4A + i);
	}
	/* RFC 4122 version/variant bits, so it is a well-formed UUIDv4-shaped
	 * value rather than raw silicon id. */
	anchor_uuid[6] = (uint8_t)((anchor_uuid[6] & 0x0FU) | 0x40U);
	anchor_uuid[8] = (uint8_t)((anchor_uuid[8] & 0x3FU) | 0x80U);

	LOG_INF("anchor identity %02x%02x%02x%02x-...", anchor_uuid[0],
		anchor_uuid[1], anchor_uuid[2], anchor_uuid[3]);
}

void impulse_anchor_beep_start(uint8_t anchor_profile)
{
	if (anchor_profile > IMPULSE_ANCHOR_PROFILE_HARD) {
		return;
	}
	if (beep_state.active && beep_state.profile == anchor_profile) {
		return; /* already running this pattern */
	}
	memset(&beep_state, 0, sizeof(beep_state));
	beep_state.active = true;
	beep_state.profile = anchor_profile;
	beep_state.buzzer_on = true;
	impulse_buzzer_set(true);
	LOG_INF("beep start (profile %u)", anchor_profile);
}

void impulse_anchor_beep_stop(void)
{
	if (!beep_state.active && !beep_state.buzzer_on) {
		return;
	}
	memset(&beep_state, 0, sizeof(beep_state));
	impulse_buzzer_set(false);
	LOG_INF("beep stop");
}

struct impulse_anchor_beep *impulse_anchor_beep_state(void)
{
	return &beep_state;
}

bool impulse_anchor_beep_tick(struct impulse_anchor_beep *b, uint32_t dt_ms,
			      uint16_t max_beep_minutes)
{
	bool was_on;

	if (b == NULL || !b->active) {
		return false;
	}

	was_on = b->buzzer_on;
	b->phase_elapsed_ms += dt_ms;
	b->total_elapsed_ms += dt_ms;

	/*
	 * §4.4 max_beep_minutes is a device-level cap, independent of the
	 * event's end time. Without it a watch that goes out of range mid-event
	 * leaves an anchor beeping until someone unplugs it.
	 */
	if (max_beep_minutes > 0U &&
	    b->total_elapsed_ms >= (uint32_t)max_beep_minutes * 60000U) {
		LOG_WRN("beep cap reached (%u min)", max_beep_minutes);
		b->active = false;
		b->buzzer_on = false;
		impulse_buzzer_set(false);
		return was_on;
	}

	uint32_t limit = b->buzzer_on ? beep_patterns[b->profile].on_ms
				      : beep_patterns[b->profile].off_ms;

	if (b->phase_elapsed_ms >= limit) {
		b->phase_elapsed_ms = 0;
		b->buzzer_on = !b->buzzer_on;
		impulse_buzzer_set(b->buzzer_on);
	}

	return b->buzzer_on != was_on;
}

bool impulse_anchor_in_active_event(const struct impulse_schedule *sched,
				    const struct impulse_date *today,
				    uint16_t minute_of_day,
				    int16_t tz_offset_minutes)
{
	static struct impulse_day_plan plan;

	if (sched == NULL || today == NULL) {
		return false;
	}

	impulse_recalculate_day(sched, today, tz_offset_minutes, &plan);

	for (uint8_t i = 0; i < plan.count; i++) {
		const struct impulse_event *e = plan.events[i];

		if (minute_of_day < e->start_time ||
		    minute_of_day >= e->end_time) {
			continue;
		}

		/* Involves this anchor as the target... */
		if (e->has_anchor_id &&
		    impulse_uuid_eq(e->anchor_id, anchor_uuid)) {
			return true;
		}
		/* ...or as one of the anchors asked to beep. */
		for (uint8_t b = 0; b < e->beep_anchor_count; b++) {
			if (impulse_uuid_eq(e->beep_anchors[b], anchor_uuid)) {
				return true;
			}
		}
	}
	return false;
}

uint8_t impulse_anchor_toggle(uint8_t value, bool in_active_event)
{
	if (value == 0x00U) {
		/* Close is ALWAYS accepted — the strap may always be locked. */
		(void)impulse_servo_set(false);
		return 0x01U;
	}

	if (value == 0x01U) {
		if (in_active_event) {
			/* Refusing to unlock during a commitment is the whole
			 * point of the strap lock. */
			LOG_INF("open refused: enforcement event involves this anchor");
			return 0x02U;
		}
		(void)impulse_servo_set(true);
		return 0x01U;
	}

	return 0x00U;
}

void impulse_anchor_on_window_start(void)
{
	if (impulse_servo_is_open()) {
		/* §4.9 auto-close BYPASSES the enforcement check by design:
		 * a window opening on an unlocked strap must lock it. */
		LOG_INF("window start: auto-closing strap");
		(void)impulse_servo_set(false);
	}
}

void impulse_anchor_identify(void)
{
	impulse_buzzer_set(true);
	k_msleep(IDENTIFY_BEEP_DURATION_MS);
	impulse_buzzer_set(false);
}

void impulse_anchor_handle_command(const uint8_t *pkt, size_t len)
{
	if (len != IMPULSE_CMD_PACKET_LEN) {
		LOG_WRN("cmd: ignoring %u-byte datagram (expected %d)",
			(unsigned)len, IMPULSE_CMD_PACKET_LEN);
		return;
	}

	uint8_t command = pkt[0];
	const uint8_t *event_uuid = &pkt[1 + IMPULSE_UUID_LEN];

	if (command == IMPULSE_CMD_WATCH_WORN) {
		/* §4.6: stop all beeping immediately. Unconditional — a watch
		 * saying "I am back on" must never be second-guessed. */
		LOG_INF("cmd: WATCH_WORN — stopping alarm");
		impulse_anchor_beep_stop();
		return;
	}

	if (command != IMPULSE_CMD_WATCH_REMOVED) {
		return;
	}

	const struct impulse_event *ev = impulse_app_find_active_event(event_uuid);

	if (ev == NULL) {
		LOG_DBG("cmd: WATCH_REMOVED for an event that is not active here");
		return;
	}

	/* Is THIS anchor in the event's beepAnchors? If not, ignore (§4.6.5). */
	const uint8_t *me = impulse_anchor_uuid();
	bool mine = false;

	for (uint8_t i = 0; i < ev->beep_anchor_count; i++) {
		if (memcmp(ev->beep_anchors[i], me, IMPULSE_UUID_LEN) == 0) {
			mine = true;
			break;
		}
	}
	if (!mine) {
		LOG_DBG("cmd: WATCH_REMOVED but this anchor is not in beepAnchors");
		return;
	}

	uint8_t profile = ev->anchor_profile;

	if (profile > IMPULSE_ANCHOR_PROFILE_HARD) {
		/* §4.11.1 precedent: a missing anchorProfile defaults to MEDIUM
		 * rather than silently not alarming. A commitment whose alarm
		 * is dead because a field was omitted is the worst outcome. */
		profile = IMPULSE_ANCHOR_PROFILE_MEDIUM;
	}

	impulse_anchor_beep_start(profile);
}

