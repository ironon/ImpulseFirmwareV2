/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IMPULSE_IDENTITY_H_
#define IMPULSE_IDENTITY_H_

#include <stdint.h>

#include "schedule/schedule.h"

/*
 * Derive this board's stable device UUID from FICR DEVICEID.
 *
 * Both roles need one: the anchor for §4.2 identity, and the watch because
 * every UDP command carries the watch UUID (§6.1). They share this derivation
 * so the two cannot drift — an anchor and a watch built from the same tree must
 * shape their identity identically or the app sees two different schemes.
 *
 * Stable across reboots without needing storage to have loaded, and unique per
 * physical board. Callers may overwrite with a persisted value.
 */
void impulse_derive_device_uuid(uint8_t out[IMPULSE_UUID_LEN]);

/* The watch's own UUID, derived once at first call. */
const uint8_t *impulse_watch_uuid(void);

#endif /* IMPULSE_IDENTITY_H_ */
