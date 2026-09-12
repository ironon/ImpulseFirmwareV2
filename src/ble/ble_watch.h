/*
 * Watch GATT service — firmware_spec_v2.md §5.6, minus the deletions in
 * firmware_spec_v3_nrf.md §4.8.
 *
 * The contract is FROZEN: same service UUID, same characteristic UUIDs, same
 * payload layouts, same SCHEDULE_FORMAT_VERSION. It is declared in four places
 * (CLAUDE.md) — this file, both firmwares' main, the Flutter app's
 * ble_constants.dart, and phone_sim's constants.py. A change here without the
 * others is a broken product, not a refactor.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_BLE_WATCH_H_
#define IMPULSE_BLE_WATCH_H_

#include <stdbool.h>
#include <stdint.h>

int impulse_ble_start(void);

/* Push a Watch Status notification (§5.6). Safe to call from anywhere; the
 * work is deferred to the system workqueue. */
void impulse_ble_notify_status(void);

/* Push a Pending Changes notification (§9.5) — the queue changed. */
void impulse_ble_notify_pending(void);

bool impulse_ble_connected(void);

#endif /* IMPULSE_BLE_WATCH_H_ */
