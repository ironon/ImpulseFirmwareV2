/* SPDX-License-Identifier: Apache-2.0 */
#include "storage.h"

#include "../net/net_wifi.h"

#include <string.h>

#include <zephyr/logging/log.h>
#include <stdlib.h>

#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(impulse_storage, LOG_LEVEL_INF);

#define KEY_ROOT      "impulse"
#define KEY_SCHED_COUNT KEY_ROOT "/schedn"
#define KEY_SCHED_CRC   KEY_ROOT "/schedc"
#define KEY_SCHED_EV    KEY_ROOT "/ev"
/* Integrity is split the same way and for the same reason as the schedule:
 * struct impulse_integrity is ~21 kB (64 settle records + 16 pending, each
 * carrying a full event) against a 4096-byte NVS sector. Scalars go in one
 * small record; settle and pending entries get one record each. */
#define KEY_INTEG_HDR KEY_ROOT "/ig"
#define KEY_INTEG_ST  KEY_ROOT "/st"
#define KEY_INTEG_PD  KEY_ROOT "/pd"

struct integ_hdr {
	uint16_t settle_window_min;
	uint8_t pass_allowance;
	uint8_t pass_spend_count;
	uint64_t pass_spend_elapsed_s[8];
};
#define KEY_ELAPSED   KEY_ROOT "/elapsed"
/* Wall clock. Separate from KEY_ELAPSED on purpose: elapsed is a MONOTONIC
 * basis that §9.2 requires for integrity timers precisely so that writing the
 * wall clock cannot accelerate a quarantine. Persisting the wall clock must
 * not be allowed to contaminate that. */
#define KEY_WALL      KEY_ROOT "/wall"
#define KEY_BOOTS     KEY_ROOT "/boots"
#define KEY_WIFI      KEY_ROOT "/wifi"
#define KEY_ANCHORIP  KEY_ROOT "/aip"
#define KEY_ANCHORIPN KEY_ROOT "/aipn"
#define KEY_TZ        KEY_ROOT "/tz"

/*
 * Loaded copies. settings_load_subtree() delivers values through a callback, so
 * a landing area is needed; these also serve as the in-RAM working copies.
 */
static struct impulse_schedule *s_sched_target;
static struct impulse_integrity *s_integ_target;
static uint64_t *s_elapsed_target;
static int64_t *s_wall_target;
static uint32_t *s_boots_target;
static struct impulse_wifi_cred *s_wifi_target;
static struct impulse_anchor_ip *s_aip_target;
static uint8_t s_aip_max;
static uint8_t *s_aip_count;
static int16_t *s_tz_target;

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	ARG_UNUSED(param);
	const char *next = NULL;

	if (settings_name_steq(key, "boots", NULL) && s_boots_target != NULL) {
		if (len != sizeof(*s_boots_target)) {
			return 0;
		}
		return read_cb(cb_arg, s_boots_target, len) > 0 ? 0 : -EINVAL;
	}
	if (settings_name_steq(key, "wifi", NULL) && s_wifi_target != NULL) {
		if (len != sizeof(*s_wifi_target)) {
			return 0;
		}
		return read_cb(cb_arg, s_wifi_target, len) > 0 ? 0 : -EINVAL;
	}
	if (settings_name_steq(key, "aipn", NULL) && s_aip_count != NULL) {
		uint8_t n = 0;

		if (read_cb(cb_arg, &n, sizeof(n)) <= 0) {
			return -EINVAL;
		}
		*s_aip_count = (n > s_aip_max) ? s_aip_max : n;
		return 0;
	}
	if (settings_name_steq(key, "aip", &next) && s_aip_target != NULL) {
		unsigned long idx;

		if (next == NULL) {
			return 0;
		}
		idx = strtoul(next, NULL, 10);
		if (idx >= s_aip_max || len != sizeof(s_aip_target[0])) {
			return 0;
		}
		return read_cb(cb_arg, &s_aip_target[idx], len) > 0 ? 0 : -EINVAL;
	}
	if (settings_name_steq(key, "wall", NULL) && s_wall_target != NULL) {
		if (len != sizeof(*s_wall_target)) {
			return 0;
		}
		return read_cb(cb_arg, s_wall_target, len) > 0 ? 0 : -EINVAL;
	}
	if (settings_name_steq(key, "schedn", NULL) && s_sched_target != NULL) {
		uint16_t n = 0;

		if (read_cb(cb_arg, &n, sizeof(n)) <= 0) {
			return -EINVAL;
		}
		s_sched_target->count =
			(n > CONFIG_IMPULSE_MAX_EVENTS)
				? (uint16_t)CONFIG_IMPULSE_MAX_EVENTS
				: n;
		return 0;
	}
	if (settings_name_steq(key, "schedc", NULL) && s_sched_target != NULL) {
		return read_cb(cb_arg, &s_sched_target->crc32,
			       sizeof(s_sched_target->crc32)) > 0
			       ? 0
			       : -EINVAL;
	}
	if (settings_name_steq(key, "ev", &next) && s_sched_target != NULL) {
		unsigned long idx;

		if (next == NULL) {
			return 0;
		}
		idx = strtoul(next, NULL, 10);
		if (idx >= CONFIG_IMPULSE_MAX_EVENTS) {
			return 0;
		}
		/* A struct-layout change between builds must not be read as a
		 * valid event: reject rather than reinterpret. */
		if (len != sizeof(s_sched_target->events[0])) {
			LOG_WRN("stored event size mismatch, ignoring");
			return 0;
		}
		return read_cb(cb_arg, &s_sched_target->events[idx], len) > 0
			       ? 0
			       : -EINVAL;
	}
	if (settings_name_steq(key, "ig", NULL) && s_integ_target != NULL) {
		struct integ_hdr hdr;

		if (len != sizeof(hdr) ||
		    read_cb(cb_arg, &hdr, sizeof(hdr)) <= 0) {
			LOG_WRN("integrity header mismatch, ignoring");
			return 0;
		}
		s_integ_target->settle_window_min = hdr.settle_window_min;
		s_integ_target->pass_allowance = hdr.pass_allowance;
		s_integ_target->pass_spend_count = hdr.pass_spend_count;
		memcpy(s_integ_target->pass_spend_elapsed_s,
		       hdr.pass_spend_elapsed_s,
		       sizeof(hdr.pass_spend_elapsed_s));
		return 0;
	}
	if (settings_name_steq(key, "st", &next) && s_integ_target != NULL) {
		unsigned long idx = (next != NULL) ? strtoul(next, NULL, 10)
						   : CONFIG_IMPULSE_MAX_EVENTS;

		if (idx >= CONFIG_IMPULSE_MAX_EVENTS ||
		    len != sizeof(s_integ_target->settle[0])) {
			return 0;
		}
		return read_cb(cb_arg, &s_integ_target->settle[idx], len) > 0
			       ? 0
			       : -EINVAL;
	}
	if (settings_name_steq(key, "pd", &next) && s_integ_target != NULL) {
		unsigned long idx =
			(next != NULL) ? strtoul(next, NULL, 10)
				       : CONFIG_IMPULSE_PENDING_QUEUE_MAX;

		if (idx >= CONFIG_IMPULSE_PENDING_QUEUE_MAX ||
		    len != sizeof(s_integ_target->pending[0])) {
			return 0;
		}
		return read_cb(cb_arg, &s_integ_target->pending[idx], len) > 0
			       ? 0
			       : -EINVAL;
	}
	if (settings_name_steq(key, "elapsed", NULL) &&
	    s_elapsed_target != NULL) {
		return read_cb(cb_arg, s_elapsed_target,
			       sizeof(*s_elapsed_target)) > 0
			       ? 0
			       : -EINVAL;
	}
	if (settings_name_steq(key, "tz", NULL) && s_tz_target != NULL) {
		return read_cb(cb_arg, s_tz_target, sizeof(*s_tz_target)) > 0
			       ? 0
			       : -EINVAL;
	}
	return 0;
}

int impulse_storage_init(void)
{
	int err = settings_subsys_init();

	if (err != 0) {
		LOG_ERR("settings_subsys_init failed (%d)", err);
	}
	return err;
}

/*
 * The schedule is stored as ONE RECORD PER EVENT, not as one blob.
 *
 * Writing the whole `struct impulse_schedule` in a single settings entry does
 * not merely waste flash — it CANNOT WORK. The struct is around 14 kB (64
 * events at ~220 bytes) while an NVS entry is bounded by the sector size,
 * 4096 bytes here, so `settings_save_one()` rejects it with -EINVAL and the
 * schedule is silently never persisted. Found on hardware, 2026-08-30; the
 * earlier code returned the error but nothing checked it.
 *
 * Per-event records also remove the cliff: a full 64-event schedule with SSIDs
 * and beep anchors would overflow a single record even in the compact wire
 * format.
 */
int impulse_storage_save_schedule(const struct impulse_schedule *s)
{
	char key[40];
	uint16_t count = s->count;
	int err;

	/* The CRC travels with the count. It is what the app compares against
	 * to confirm-rather-than-infer that the watch holds the schedule it
	 * thinks it does (MOBILE_APP_SPEC §8.16); losing it across a reboot
	 * makes a synced watch report "no schedule" and triggers a re-push. */
	err = settings_save_one(KEY_SCHED_CRC, &s->crc32, sizeof(s->crc32));
	if (err != 0) {
		LOG_ERR("schedule crc save failed (%d)", err);
		return err;
	}

	err = settings_save_one(KEY_SCHED_COUNT, &count, sizeof(count));
	if (err != 0) {
		LOG_ERR("schedule count save failed (%d)", err);
		return err;
	}

	for (uint16_t i = 0; i < count; i++) {
		(void)snprintk(key, sizeof(key), KEY_SCHED_EV "/%u", i);
		err = settings_save_one(key, &s->events[i],
					sizeof(s->events[i]));
		if (err != 0) {
			LOG_ERR("event %u save failed (%d)", i, err);
			return err;
		}
	}

	/* Delete any stale records left by a longer previous schedule, so a
	 * shortened schedule cannot be resurrected on the next boot. */
	for (uint16_t i = count; i < CONFIG_IMPULSE_MAX_EVENTS; i++) {
		(void)snprintk(key, sizeof(key), KEY_SCHED_EV "/%u", i);
		(void)settings_delete(key);
	}

	return 0;
}

int impulse_storage_load_schedule(struct impulse_schedule *s)
{
	int err;

	s_sched_target = s;
	err = settings_load_subtree_direct(KEY_ROOT, load_cb, NULL);
	s_sched_target = NULL;
	return err;
}

int impulse_storage_save_integrity(const struct impulse_integrity *ig)
{
	struct integ_hdr hdr = {
		.settle_window_min = ig->settle_window_min,
		.pass_allowance = ig->pass_allowance,
		.pass_spend_count = ig->pass_spend_count,
	};
	char key[40];
	int err;

	memcpy(hdr.pass_spend_elapsed_s, ig->pass_spend_elapsed_s,
	       sizeof(hdr.pass_spend_elapsed_s));

	err = settings_save_one(KEY_INTEG_HDR, &hdr, sizeof(hdr));
	if (err != 0) {
		LOG_ERR("integrity header save failed (%d)", err);
		return err;
	}

	for (size_t i = 0; i < CONFIG_IMPULSE_MAX_EVENTS; i++) {
		(void)snprintk(key, sizeof(key), KEY_INTEG_ST "/%u",
			       (unsigned)i);
		if (ig->settle[i].in_use) {
			err = settings_save_one(key, &ig->settle[i],
						sizeof(ig->settle[i]));
			if (err != 0) {
				LOG_ERR("settle %u save failed (%d)",
					(unsigned)i, err);
				return err;
			}
		} else {
			(void)settings_delete(key);
		}
	}

	for (size_t i = 0; i < CONFIG_IMPULSE_PENDING_QUEUE_MAX; i++) {
		(void)snprintk(key, sizeof(key), KEY_INTEG_PD "/%u",
			       (unsigned)i);
		if (ig->pending[i].in_use) {
			err = settings_save_one(key, &ig->pending[i],
						sizeof(ig->pending[i]));
			if (err != 0) {
				LOG_ERR("pending %u save failed (%d)",
					(unsigned)i, err);
				return err;
			}
		} else {
			(void)settings_delete(key);
		}
	}

	return 0;
}

int impulse_storage_load_integrity(struct impulse_integrity *ig)
{
	int err;

	s_integ_target = ig;
	err = settings_load_subtree_direct(KEY_ROOT, load_cb, NULL);
	s_integ_target = NULL;
	return err;
}

int impulse_storage_save_boot_count(uint32_t n)
{
	return settings_save_one(KEY_BOOTS, &n, sizeof(n));
}

int impulse_storage_load_boot_count(uint32_t *n)
{
	int err;

	*n = 0;
	s_boots_target = n;
	err = settings_load_subtree_direct(KEY_ROOT, load_cb, NULL);
	s_boots_target = NULL;
	return err;
}

int impulse_storage_save_wifi(const struct impulse_wifi_cred *cred)
{
	return settings_save_one(KEY_WIFI, cred, sizeof(*cred));
}

int impulse_storage_load_wifi(struct impulse_wifi_cred *cred)
{
	int err;

	memset(cred, 0, sizeof(*cred));
	s_wifi_target = cred;
	err = settings_load_subtree_direct(KEY_ROOT, load_cb, NULL);
	s_wifi_target = NULL;
	return err;
}

int impulse_storage_save_anchor_ips(const struct impulse_anchor_ip *tbl,
				    uint8_t count)
{
	int first_err = 0;
	int err;

	/* One record per entry, like events: the whole table as a single blob
	 * would be fine at today's sizes, but per-record is what this file
	 * already does everywhere else and it keeps a bad entry from taking the
	 * rest down. */
	err = settings_save_one(KEY_ANCHORIPN, &count, sizeof(count));
	if (err != 0) {
		first_err = err;
	}
	for (uint8_t i = 0; i < count; i++) {
		char key[40];

		(void)snprintk(key, sizeof(key), KEY_ANCHORIP "/%u", i);
		err = settings_save_one(key, &tbl[i], sizeof(tbl[i]));
		if (err != 0 && first_err == 0) {
			first_err = err;
		}
	}
	return first_err;
}

int impulse_storage_load_anchor_ips(struct impulse_anchor_ip *tbl,
				    uint8_t max, uint8_t *count_out)
{
	int err;

	memset(tbl, 0, sizeof(*tbl) * max);
	*count_out = 0;
	s_aip_target = tbl;
	s_aip_max = max;
	s_aip_count = count_out;
	err = settings_load_subtree_direct(KEY_ROOT, load_cb, NULL);
	s_aip_target = NULL;
	s_aip_count = NULL;
	return err;
}

int impulse_storage_save_wall_clock(int64_t utc_s)
{
	return settings_save_one(KEY_WALL, &utc_s, sizeof(utc_s));
}

int impulse_storage_load_wall_clock(int64_t *utc_s)
{
	int err;

	*utc_s = 0;
	s_wall_target = utc_s;
	err = settings_load_subtree_direct(KEY_ROOT, load_cb, NULL);
	s_wall_target = NULL;
	return err;
}

int impulse_storage_save_elapsed_base(uint64_t base_s)
{
	return settings_save_one(KEY_ELAPSED, &base_s, sizeof(base_s));
}

int impulse_storage_load_elapsed_base(uint64_t *base_s)
{
	int err;

	*base_s = 0;
	s_elapsed_target = base_s;
	err = settings_load_subtree_direct(KEY_ROOT, load_cb, NULL);
	s_elapsed_target = NULL;
	return err;
}

int impulse_storage_save_settings(int16_t tz_offset_minutes)
{
	return settings_save_one(KEY_TZ, &tz_offset_minutes,
				 sizeof(tz_offset_minutes));
}

int impulse_storage_load_settings(int16_t *tz_offset_minutes)
{
	int err;

	*tz_offset_minutes = 0;
	s_tz_target = tz_offset_minutes;
	err = settings_load_subtree_direct(KEY_ROOT, load_cb, NULL);
	s_tz_target = NULL;
	return err;
}

int impulse_storage_factory_reset(void)
{
	int err = 0;

	err |= settings_delete(KEY_SCHED_COUNT);
	err |= settings_delete(KEY_SCHED_CRC);
	for (uint16_t i = 0; i < CONFIG_IMPULSE_MAX_EVENTS; i++) {
		char key[40];

		(void)snprintk(key, sizeof(key), KEY_SCHED_EV "/%u", i);
		(void)settings_delete(key);
	}
	err |= settings_delete(KEY_INTEG_HDR);
	for (size_t i = 0; i < CONFIG_IMPULSE_MAX_EVENTS; i++) {
		char key[40];

		(void)snprintk(key, sizeof(key), KEY_INTEG_ST "/%u",
			       (unsigned)i);
		(void)settings_delete(key);
	}
	for (size_t i = 0; i < CONFIG_IMPULSE_PENDING_QUEUE_MAX; i++) {
		char key[40];

		(void)snprintk(key, sizeof(key), KEY_INTEG_PD "/%u",
			       (unsigned)i);
		(void)settings_delete(key);
	}
	err |= settings_delete(KEY_ELAPSED);
	err |= settings_delete(KEY_TZ);

	LOG_WRN("factory reset performed");
	return err;
}
