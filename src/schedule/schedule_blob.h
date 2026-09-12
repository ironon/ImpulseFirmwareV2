/*
 * Schedule blob wire format — firmware_spec_v2.md §6.2.
 * FROZEN by v3 §0.2: byte-identical to the ESP32 build, including
 * SCHEDULE_FORMAT_VERSION = 0x02. This format is declared in four places
 * (see CLAUDE.md); this file is one of them. Do not "improve" it here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_SCHEDULE_BLOB_H_
#define IMPULSE_SCHEDULE_BLOB_H_

#include <stdint.h>
#include <stddef.h>

#include "event.h"

#define IMPULSE_SCHEDULE_FORMAT_VERSION 0x02

/* Transfer opcodes on the control characteristic (§6.2). */
#define IMPULSE_XFER_BEGIN 0x01
#define IMPULSE_XFER_END   0x02
#define IMPULSE_XFER_ABORT 0x03

/* END verdicts written back to the app (§6.2, §9.3). */
#define IMPULSE_END_FAILED      0x00 /* CRC or parse failure — app retries */
#define IMPULSE_END_ACCEPTED    0x01
#define IMPULSE_END_QUARANTINED 0x03 /* accepted; >=1 loosening quarantined */
#define IMPULSE_END_REJECTED    0x04 /* would loosen the ACTIVE event */

/*
 * Hard cap on a declared transfer length. The ESP32 build once malloc'd an
 * untrusted 4-byte length directly; this bound is that defect's fix and v3 §9
 * explicitly says to port it. It is input validation, not a NimBLE quirk.
 */
#define IMPULSE_SCHEDULE_MAX_BLOB_BYTES (64U * 1024U)

struct impulse_schedule {
	uint16_t count;
	struct impulse_event events[CONFIG_IMPULSE_MAX_EVENTS];
	uint32_t crc32; /* of the blob this was parsed from; 0 = none */
};

enum impulse_blob_result {
	IMPULSE_BLOB_OK = 0,
	IMPULSE_BLOB_ERR_VERSION,   /* unknown leading version byte */
	IMPULSE_BLOB_ERR_TRUNCATED, /* ran off the end of the buffer */
	IMPULSE_BLOB_ERR_TOO_MANY,  /* more events than CONFIG_IMPULSE_MAX_EVENTS */
	IMPULSE_BLOB_ERR_FIELD,     /* a field failed validation */
};

/*
 * Parse a schedule blob. `len` is the blob WITHOUT any trailing CRC (BLE
 * carries the CRC in the END frame; the HTTP body appends it and the caller
 * must strip it first).
 *
 * Rejects rather than truncates when the blob declares more events than fit:
 * a silently shortened schedule is a silently weakened commitment.
 */
enum impulse_blob_result impulse_blob_parse(const uint8_t *buf, size_t len,
					    struct impulse_schedule *out);

/* Serialise back to the same wire format. Returns bytes written, or 0 if the
 * buffer is too small. Used for the anchor's HTTP mirror and for round-trip
 * tests. */
size_t impulse_blob_serialize(const struct impulse_schedule *sched,
			      uint8_t *buf, size_t cap);

uint32_t impulse_crc32(const uint8_t *data, size_t len);

#endif /* IMPULSE_SCHEDULE_BLOB_H_ */
