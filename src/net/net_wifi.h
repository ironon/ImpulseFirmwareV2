/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IMPULSE_NET_WIFI_H_
#define IMPULSE_NET_WIFI_H_

#include <stdbool.h>
#include <stdint.h>

#define IMPULSE_WIFI_SSID_MAX 32
#define IMPULSE_WIFI_PSK_MAX  64

struct impulse_wifi_cred {
	char ssid[IMPULSE_WIFI_SSID_MAX + 1];
	char psk[IMPULSE_WIFI_PSK_MAX + 1];
};

/* §4.4 / §8: 0x00 never provisioned, 0x01 provisioned but not connected,
 * 0x02 connected. Matches the WiFi Status byte the app already reads. */
enum impulse_wifi_state {
	IMPULSE_WIFI_UNPROVISIONED = 0x00,
	IMPULSE_WIFI_PROVISIONED   = 0x01,
	IMPULSE_WIFI_CONNECTED     = 0x02,
};

void impulse_wifi_init(void);

/* Store credentials and start connecting. Persists them, so a reboot does not
 * lose the association — §5.5.1's "no WiFi at the instant of the edge" failure
 * is the one this has to avoid. */
int impulse_wifi_set_credentials(const char *ssid, const char *psk);

enum impulse_wifi_state impulse_wifi_state(void);
bool impulse_wifi_connected(void);
const char *impulse_wifi_ssid(void);

/* Current IPv4 address, or 0.0.0.0 if none. Written big-endian (network order)
 * into out[4], which is the order §4.4's WiFi Status payload carries. */
void impulse_wifi_ipv4(uint8_t out[4]);

/*
 * Ask for the link to be up (or let it drop).
 *
 * §5.5.1: an enforcement window whose event has beepAnchors MUST keep WiFi
 * alive and reconnect if it drops, because the whole WATCH_REMOVED path needs
 * the association. The earlier rule — "WiFi is abandoned once a window opens"
 * — was false for exactly this case and shipped broken.
 */
void impulse_wifi_request(bool want);

/* Drive reconnection. Call from the main loop; cheap and idempotent. */
void impulse_wifi_tick(int64_t now_ms);

#endif /* IMPULSE_NET_WIFI_H_ */
