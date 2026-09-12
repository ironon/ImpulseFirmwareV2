/*
 * Persistence — firmware_spec_v3_nrf.md §8. ESP32 NVS becomes Zephyr NVS via
 * the settings subsystem.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_STORAGE_H_
#define IMPULSE_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>

#include "../integrity/integrity.h"
#include "../schedule/schedule.h"

int impulse_storage_init(void);

int impulse_storage_save_schedule(const struct impulse_schedule *s);
int impulse_storage_load_schedule(struct impulse_schedule *s);

/*
 * Commitment-integrity state is SECURITY STATE (§8): settle timers, settled
 * baselines, and the pending queue. Losing it silently converts every
 * quarantined loosening into a free one.
 */
int impulse_storage_save_integrity(const struct impulse_integrity *ig);
int impulse_storage_load_integrity(struct impulse_integrity *ig);

/* Monotonic elapsed base, persisted so integrity timers survive a reboot
 * (§9.2). Must be re-saved periodically — see the note in integrity.c. */
/*
 * Wall clock across a reboot.
 *
 * Without this the watch boots believing it is 1970, matches no window, and
 * enforces NOTHING until the app reconnects — silently. A reset (or a flat
 * battery) therefore cancelled every commitment with no indication, which for
 * an overnight commitment means simply not going off.
 *
 * The restored value is a LOWER BOUND, never authoritative: it is whatever was
 * last written, so it lags by up to the save interval plus however long the
 * device was off. The app re-syncing (§8.11) always supersedes it.
 */
/* WiFi credentials (§4.4 write). Persisted because §5.5.1's worst failure is
 * "no WiFi at the instant of the edge" — a watch that must re-provision after
 * every reboot cannot escalate to anchors. */
struct impulse_wifi_cred;
int impulse_storage_save_wifi(const struct impulse_wifi_cred *cred);
int impulse_storage_load_wifi(struct impulse_wifi_cred *cred);

/* Anchor UUID -> IPv4 table (§5.5.1 attempt 1). Pushed by the app. */
struct impulse_anchor_ip {
	uint8_t uuid[IMPULSE_UUID_LEN];
	uint8_t ip[4];
};
int impulse_storage_save_anchor_ips(const struct impulse_anchor_ip *tbl,
				    uint8_t count);
int impulse_storage_load_anchor_ips(struct impulse_anchor_ip *tbl,
				    uint8_t max, uint8_t *count_out);

/* Persisted boot counter. A board that silently resets is indistinguishable
 * from one whose radios died — both go quiet — and the RTT buffer is wiped by
 * the reset, so the log cannot tell you either. This can. */
int impulse_storage_save_boot_count(uint32_t n);
int impulse_storage_load_boot_count(uint32_t *n);

int impulse_storage_save_wall_clock(int64_t utc_s);
int impulse_storage_load_wall_clock(int64_t *utc_s);

int impulse_storage_save_elapsed_base(uint64_t base_s);
int impulse_storage_load_elapsed_base(uint64_t *base_s);

int impulse_storage_save_settings(int16_t tz_offset_minutes);
int impulse_storage_load_settings(int16_t *tz_offset_minutes);

/*
 * §8: factory reset must clear EVERYTHING above. The self-binding delay means
 * a plain schedule clear answers 0x03 by design, so a genuine clean start
 * needs a real wipe. Bench builds only.
 */
int impulse_storage_factory_reset(void);

#endif /* IMPULSE_STORAGE_H_ */
