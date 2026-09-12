/*
 * Hooks the bring-up shell uses to drive the application state that main.c
 * owns. Bring-up only — see CONFIG_IMPULSE_BRINGUP_SHELL.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_APP_API_H_
#define IMPULSE_APP_API_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Snapshot for the Watch Status characteristic (§5.6). */
struct impulse_app_status {
	uint8_t activity_state; /* 0 dormant, 1 enforcement, 2 dormant_sleep */
	uint8_t bt_connected;
	uint8_t wifi_connected;
	uint8_t worn;
	uint8_t battery_pct; /* 0xFF = not available */
	uint8_t active_event_id[16];
	uint8_t condition_met;
	uint32_t schedule_crc;
};

void impulse_app_get_status(struct impulse_app_status *out);

/*
 * Apply a received schedule blob through the §9.3 commitment-integrity gate.
 * Returns the §6.2 END verdict byte: 0x01 accepted, 0x03 partially
 * quarantined, 0x04 rejected (would loosen the ACTIVE event), 0x00 parse/CRC
 * failure. NEVER bypasses the gate — that gate is the product.
 */
uint8_t impulse_app_apply_schedule(const uint8_t *blob, size_t len);

/* Settings characteristic (§5.6, gated by §9.8). Returns the response byte. */
uint8_t impulse_app_apply_settings(uint8_t disconnected_is_dormant,
				   uint8_t away_is_dormant,
				   int16_t tz_offset_minutes,
				   uint16_t settle_window_min);

/* Time write (§5.6), guarded by §9.7. Returns the response byte. */
uint8_t impulse_app_apply_time(int64_t utc_seconds, int16_t tz_offset_minutes);

/* Watch: WiFi credentials (JSON {"ssid":...,"password":...}) and the anchor
 * UUID->IP table ([n] then [uuid 16][ipv4 4][ts u32] per entry), both pushed by
 * the app. These used to be accepted at the ATT layer and dropped. */
void impulse_app_apply_wifi_credentials(const uint8_t *json, size_t len);
void impulse_app_apply_anchor_ips(const uint8_t *buf, size_t len);

/* Emergency pass (§9.6). */
uint8_t impulse_app_pass_spend(const uint8_t *event_id, uint32_t date_yyyymmdd,
			       uint8_t *remaining_out);
uint8_t impulse_app_pass_set_allowance(uint8_t allowance);
void impulse_app_pass_read(uint8_t *allowance, uint8_t *remaining);

/* Pending Changes queue (§9.5). Fills `buf` with the wire payload, returns
 * the number of bytes written. */
size_t impulse_app_pending_payload(uint8_t *buf, size_t cap);

/* --- anchor role (§4.7, §4.9) --------------------------------------------
 * The anchor stores and recomputes its own schedule but does NOT run the §9
 * integrity gate: the watch is the root of trust and an anchor that
 * quarantined its own schedule would just be out of sync with the device that
 * actually decides.
 */
uint8_t impulse_app_anchor_apply_schedule(const uint8_t *blob, size_t len);
void impulse_app_anchor_set_settings(uint16_t max_beep_minutes,
				     int16_t tz_offset_minutes);
bool impulse_app_anchor_in_active_event(void);

/* CRC32 of the schedule blob this anchor currently holds (§4.7 sync check). */
uint32_t impulse_app_anchor_schedule_crc(void);

/* Anchor: resolve an event UUID from a UDP command against the anchor's OWN
 * schedule and clock, returning it only if a window is open right now. */
const struct impulse_event *impulse_app_find_active_event(const uint8_t *event_uuid);

void impulse_app_set_time(int64_t utc_seconds, int16_t tz_offset_minutes);
int64_t impulse_app_now_utc(void);

/* Install a synthetic single-event schedule whose window covers the current
 * minute, so the enforcement path can be driven end to end without BLE. */
int impulse_app_install_demo(uint8_t criteria, uint8_t profile);

/* Bench override for the worn sensor: -1 = sensor, 0 = force off, 1 = force on.
 * Shell only — never exposed over BLE. See the comment at g_worn_override. */
void impulse_app_force_worn(int8_t mode);

/* Persist / reload, for the storage round-trip test. */
int impulse_app_save_all(void);
void impulse_app_report(void);

#endif /* IMPULSE_APP_API_H_ */
