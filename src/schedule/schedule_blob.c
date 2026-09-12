/* SPDX-License-Identifier: Apache-2.0 */
#include "schedule_blob.h"

#include <string.h>

/*
 * A cursor over untrusted bytes. Every read is bounds-checked and sets an
 * `overrun` flag rather than trapping, so a malformed blob produces a clean
 * parse failure instead of a fault. The caller checks the flag once at the end
 * as well as at each loop head.
 */
struct cursor {
	const uint8_t *buf;
	size_t len;
	size_t pos;
	bool overrun;
};

static bool cur_take(struct cursor *c, void *dst, size_t n)
{
	if (c->overrun || (c->len - c->pos) < n) {
		c->overrun = true;
		return false;
	}
	if (dst != NULL) {
		memcpy(dst, &c->buf[c->pos], n);
	}
	c->pos += n;
	return true;
}

static uint8_t cur_u8(struct cursor *c)
{
	uint8_t v = 0;

	(void)cur_take(c, &v, sizeof(v));
	return v;
}

static uint16_t cur_u16(struct cursor *c)
{
	uint8_t b[2] = {0};

	(void)cur_take(c, b, sizeof(b));
	return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static int64_t cur_i64(struct cursor *c)
{
	uint8_t b[8] = {0};
	uint64_t v = 0;

	(void)cur_take(c, b, sizeof(b));
	for (int i = 7; i >= 0; i--) {
		v = (v << 8) | b[i];
	}
	return (int64_t)v;
}

/* CRC-32 (IEEE 802.3, reflected) — must match Python's zlib.crc32, which is
 * what the app and phone_sim use. Bitwise rather than table-driven: a schedule
 * blob is a few kB and this runs once per transfer, so 256 words of flash for
 * a table nobody will notice is not worth it. */
uint32_t impulse_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFFU;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));

			crc = (crc >> 1) ^ (0xEDB88320U & mask);
		}
	}
	return ~crc;
}

/*
 * Field validation. §3.2 says the app enforces these before transmission and
 * "the firmware may assert but not correct". We reject rather than correct:
 * a silently corrected event is a commitment the user did not author, and the
 * app is explicitly untrusted (§9 — it is deletable and replaceable by a
 * generic BLE tool).
 */
static bool event_fields_valid(const struct impulse_event *e)
{
	if (e->recurrence > IMPULSE_RECUR_MONTHLY) {
		return false;
	}
	if (e->criteria > IMPULSE_CRIT_PHONE_AWAY) {
		return false;
	}
	if (e->profile >= IMPULSE_PROFILE_COUNT) {
		return false;
	}
	if (e->start_time > 1439U || e->end_time > 1439U) {
		return false;
	}
	/* Windows may not span midnight, and entry is matched on the exact
	 * local minute (§5.3) — an inverted window would simply never fire. */
	if (e->end_time <= e->start_time) {
		return false;
	}
	if (e->recurrence == IMPULSE_RECUR_WEEKLY &&
	    (e->day_of_week < 1U || e->day_of_week > 7U)) {
		return false;
	}
	if (e->recurrence == IMPULSE_RECUR_MONTHLY &&
	    (e->day_of_month < 1U || e->day_of_month > 31U)) {
		return false;
	}
	if (e->donning_grace_s > 1800U) {
		return false;
	}
	if (e->beep_anchor_count > IMPULSE_MAX_BEEP_ANCHORS) {
		return false;
	}
	if (e->beep_anchor_count > 0U &&
	    e->anchor_profile > IMPULSE_ANCHOR_PROFILE_HARD) {
		return false;
	}
	return true;
}

enum impulse_blob_result impulse_blob_parse(const uint8_t *buf, size_t len,
					    struct impulse_schedule *out)
{
	struct cursor c = {.buf = buf, .len = len, .pos = 0, .overrun = false};

	if (buf == NULL || out == NULL || len < 3U) {
		return IMPULSE_BLOB_ERR_TRUNCATED;
	}

	/* The version byte was added in spec v0.5. A v1 parser misreads it as
	 * the low byte of the event count, which is why every declaration of
	 * this format has to move together. */
	if (cur_u8(&c) != IMPULSE_SCHEDULE_FORMAT_VERSION) {
		return IMPULSE_BLOB_ERR_VERSION;
	}

	uint16_t count = cur_u16(&c);

	if (count > CONFIG_IMPULSE_MAX_EVENTS) {
		return IMPULSE_BLOB_ERR_TOO_MANY;
	}

	memset(out, 0, sizeof(*out));

	for (uint16_t i = 0; i < count; i++) {
		struct impulse_event *e = &out->events[i];

		if (!cur_take(&c, e->id, IMPULSE_UUID_LEN)) {
			return IMPULSE_BLOB_ERR_TRUNCATED;
		}
		e->reference_date = cur_i64(&c);
		e->start_time = cur_u16(&c);
		e->end_time = cur_u16(&c);
		e->recurrence = cur_u8(&c);
		e->day_of_week = cur_u8(&c);
		e->day_of_month = cur_u8(&c);
		e->criteria = cur_u8(&c);
		e->profile = cur_u8(&c);
		e->anchor_profile = cur_u8(&c);
		e->negate = (cur_u8(&c) != 0U);
		e->donning_grace_s = cur_u16(&c);

		e->has_anchor_id = (cur_u8(&c) != 0U);
		/* The 16 anchor bytes are ALWAYS on the wire, zero-filled when
		 * absent — the presence flag does not remove them. */
		if (!cur_take(&c, e->anchor_id, IMPULSE_UUID_LEN)) {
			return IMPULSE_BLOB_ERR_TRUNCATED;
		}

		e->ssid_len = cur_u8(&c);
		if (e->ssid_len > IMPULSE_MAX_SSID_LEN) {
			return IMPULSE_BLOB_ERR_FIELD;
		}
		if (e->ssid_len > 0U) {
			if (!cur_take(&c, e->wifi_ssid, e->ssid_len)) {
				return IMPULSE_BLOB_ERR_TRUNCATED;
			}
		}
		e->wifi_ssid[e->ssid_len] = '\0';

		e->beep_anchor_count = cur_u8(&c);
		if (e->beep_anchor_count > IMPULSE_MAX_BEEP_ANCHORS) {
			return IMPULSE_BLOB_ERR_FIELD;
		}
		for (uint8_t b = 0; b < e->beep_anchor_count; b++) {
			if (!cur_take(&c, e->beep_anchors[b],
				      IMPULSE_UUID_LEN)) {
				return IMPULSE_BLOB_ERR_TRUNCATED;
			}
		}

		if (c.overrun) {
			return IMPULSE_BLOB_ERR_TRUNCATED;
		}
		if (!event_fields_valid(e)) {
			return IMPULSE_BLOB_ERR_FIELD;
		}
	}

	if (c.overrun) {
		return IMPULSE_BLOB_ERR_TRUNCATED;
	}

	out->count = count;
	out->crc32 = impulse_crc32(buf, len);
	return IMPULSE_BLOB_OK;
}

/* --- serialise ----------------------------------------------------------- */

struct writer {
	uint8_t *buf;
	size_t cap;
	size_t pos;
	bool full;
};

static void w_bytes(struct writer *w, const void *src, size_t n)
{
	if (w->full || (w->cap - w->pos) < n) {
		w->full = true;
		return;
	}
	memcpy(&w->buf[w->pos], src, n);
	w->pos += n;
}

static void w_u8(struct writer *w, uint8_t v)
{
	w_bytes(w, &v, 1);
}

static void w_u16(struct writer *w, uint16_t v)
{
	uint8_t b[2] = {(uint8_t)(v & 0xFFU), (uint8_t)((v >> 8) & 0xFFU)};

	w_bytes(w, b, sizeof(b));
}

static void w_i64(struct writer *w, int64_t v)
{
	uint8_t b[8];
	uint64_t u = (uint64_t)v;

	for (int i = 0; i < 8; i++) {
		b[i] = (uint8_t)((u >> (8 * i)) & 0xFFU);
	}
	w_bytes(w, b, sizeof(b));
}

size_t impulse_blob_serialize(const struct impulse_schedule *sched,
			      uint8_t *buf, size_t cap)
{
	struct writer w = {.buf = buf, .cap = cap, .pos = 0, .full = false};

	if (sched == NULL || buf == NULL) {
		return 0;
	}

	w_u8(&w, IMPULSE_SCHEDULE_FORMAT_VERSION);
	w_u16(&w, sched->count);

	for (uint16_t i = 0; i < sched->count; i++) {
		const struct impulse_event *e = &sched->events[i];

		w_bytes(&w, e->id, IMPULSE_UUID_LEN);
		w_i64(&w, e->reference_date);
		w_u16(&w, e->start_time);
		w_u16(&w, e->end_time);
		w_u8(&w, e->recurrence);
		w_u8(&w, e->day_of_week);
		w_u8(&w, e->day_of_month);
		w_u8(&w, e->criteria);
		w_u8(&w, e->profile);
		w_u8(&w, e->anchor_profile);
		w_u8(&w, e->negate ? 1U : 0U);
		w_u16(&w, e->donning_grace_s);
		w_u8(&w, e->has_anchor_id ? 1U : 0U);
		w_bytes(&w, e->anchor_id, IMPULSE_UUID_LEN);
		w_u8(&w, e->ssid_len);
		if (e->ssid_len > 0U) {
			w_bytes(&w, e->wifi_ssid, e->ssid_len);
		}
		w_u8(&w, e->beep_anchor_count);
		for (uint8_t b = 0; b < e->beep_anchor_count; b++) {
			w_bytes(&w, e->beep_anchors[b], IMPULSE_UUID_LEN);
		}
	}

	return w.full ? 0U : w.pos;
}
