/*
 * Frozen GATT UUIDs. Mirrored from phone_sim/impulse_phone/constants.py and
 * impulse_app/lib/utils/ble_constants.dart — if this file disagrees with those,
 * they are all wrong together and the product is broken.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_BLE_UUIDS_H_
#define IMPULSE_BLE_UUIDS_H_

#include <zephyr/bluetooth/uuid.h>

#define IMPULSE_UUID_BASE(first) \
	BT_UUID_128_ENCODE(first, 0xf8ce, 0x11ee, 0x8001, 0x020304050607)

#define IMPULSE_UUID_ANCHOR_SVC        IMPULSE_UUID_BASE(0x4a0f0001)
#define IMPULSE_UUID_ANCHOR_IDENTIFY   IMPULSE_UUID_BASE(0x4a0f0002)
#define IMPULSE_UUID_ANCHOR_WIFI_CRED  IMPULSE_UUID_BASE(0x4a0f0003)
#define IMPULSE_UUID_ANCHOR_SETTINGS   IMPULSE_UUID_BASE(0x4a0f0004)
#define IMPULSE_UUID_ANCHOR_SCHED_CTRL IMPULSE_UUID_BASE(0x4a0f0005)
#define IMPULSE_UUID_ANCHOR_SCHED_DATA IMPULSE_UUID_BASE(0x4a0f0006)
#define IMPULSE_UUID_ANCHOR_TOGGLE     IMPULSE_UUID_BASE(0x4a0f0007)
/* 0x4a0f0008..000b and 000f are the proximity and calibration
 * characteristics DELETED by spec v3 §4.8. Do not re-add. */
#define IMPULSE_UUID_ANCHOR_DOCK_REG   IMPULSE_UUID_BASE(0x4a0f000c)
#define IMPULSE_UUID_ANCHOR_DOCK_STAT  IMPULSE_UUID_BASE(0x4a0f000d)
#define IMPULSE_UUID_ANCHOR_WIFI_STAT  IMPULSE_UUID_BASE(0x4a0f000e)
/*
 * Watch -> anchor escalation over BLE (WATCH_REMOVED / WATCH_WORN).
 *
 * Carries the SAME 33-byte §6.1 payload as the UDP path, deliberately: the
 * anchor validates it with identical code, so the two transports cannot drift
 * apart in what they accept. Only the carrier differs.
 *
 * This exists because BLE and WiFi cannot both be active on Board V1 (see
 * agent-notes 2026-09-12), and because the product must not depend on
 * simultaneous radios — the FCC grant is expected to forbid it.
 */
#define IMPULSE_UUID_ANCHOR_WATCH_STATE IMPULSE_UUID_BASE(0x4a0f000f)

#define IMPULSE_UUID_WATCH_SVC       IMPULSE_UUID_BASE(0x4a0f0010)
#define IMPULSE_UUID_WIFI_CRED       IMPULSE_UUID_BASE(0x4a0f0011)
#define IMPULSE_UUID_SCHED_CTRL      IMPULSE_UUID_BASE(0x4a0f0012)
#define IMPULSE_UUID_SCHED_DATA      IMPULSE_UUID_BASE(0x4a0f0013)
#define IMPULSE_UUID_SETTINGS        IMPULSE_UUID_BASE(0x4a0f0014)
#define IMPULSE_UUID_SEEN_ANCHORS    IMPULSE_UUID_BASE(0x4a0f0015)
#define IMPULSE_UUID_STATUS          IMPULSE_UUID_BASE(0x4a0f0016)
#define IMPULSE_UUID_ANCHOR_IP       IMPULSE_UUID_BASE(0x4a0f0017)
#define IMPULSE_UUID_LED_CONFIG      IMPULSE_UUID_BASE(0x4a0f0018)
#define IMPULSE_UUID_TIME            IMPULSE_UUID_BASE(0x4a0f0019)
#define IMPULSE_UUID_PENDING         IMPULSE_UUID_BASE(0x4a0f001a)
#define IMPULSE_UUID_PASS            IMPULSE_UUID_BASE(0x4a0f001b)

/*
 * 0x4a0f001c — Calibration Control — is DELETED by spec v3 §4.8 along with
 * calibration-v2. Do not re-add it: it exists for nothing, and the app drops
 * it in the same lockstep change.
 */

#endif /* IMPULSE_BLE_UUIDS_H_ */
