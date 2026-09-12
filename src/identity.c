/* SPDX-License-Identifier: Apache-2.0 */
#include "identity.h"

#include <string.h>

void impulse_derive_device_uuid(uint8_t out[IMPULSE_UUID_LEN])
{
	/* FICR base 0x00FFC000, INFO at +0x300, DEVICEID at +0x304 (2 words). */
	const volatile uint32_t *deviceid =
		(const volatile uint32_t *)0x00FFC304UL;
	uint32_t a = deviceid[0];
	uint32_t b = deviceid[1];

	for (int i = 0; i < IMPULSE_UUID_LEN; i++) {
		uint32_t src = (i < 8) ? a : b;

		out[i] = (uint8_t)((src >> ((i % 4) * 8)) & 0xFFU) ^
			 (uint8_t)(0x4A + i);
	}
	/* RFC 4122 version/variant bits, so it is a well-formed UUIDv4-shaped
	 * value rather than a raw silicon id. */
	out[6] = (uint8_t)((out[6] & 0x0FU) | 0x40U);
	out[8] = (uint8_t)((out[8] & 0x3FU) | 0x80U);
}

const uint8_t *impulse_watch_uuid(void)
{
	static uint8_t uuid[IMPULSE_UUID_LEN];
	static bool derived;

	if (!derived) {
		impulse_derive_device_uuid(uuid);
		derived = true;
	}
	return uuid;
}
