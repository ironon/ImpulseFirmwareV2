/* SPDX-License-Identifier: Apache-2.0 */
#include "net_wifi.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>

#include "../storage/storage.h"

LOG_MODULE_REGISTER(impulse_wifi, LOG_LEVEL_INF);

/* Retry cadence. Deliberately slow: a watch that cannot see its AP should not
 * spend its battery re-associating every second, and WATCH_REMOVED is
 * re-asserted per enforcement poll anyway (§5.5.1), so a missed window of a few
 * seconds costs nothing. */
#define WIFI_RETRY_INTERVAL_MS 15000

static struct impulse_wifi_cred g_cred;
static bool g_have_cred;
static bool g_want_link;
static bool g_connected;
static bool g_connect_pending;
static int64_t g_last_attempt_ms;
static bool g_dhcp_started;
static int64_t g_last_dhcp_ms;

static struct net_mgmt_event_callback g_cb;

static void wifi_evt(struct net_mgmt_event_callback *cb, uint64_t evt,
		     struct net_if *iface)
{
	ARG_UNUSED(iface);

	switch (evt) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *st = (const struct wifi_status *)cb->info;

		g_connect_pending = false;
		if (st != NULL && st->status != 0) {
			LOG_WRN("wifi: association failed (status %d)", st->status);
			g_connected = false;
			break;
		}
		g_connected = true;
		LOG_INF("wifi: connected to \"%s\"", g_cred.ssid);

		/* DHCP is started from the tick, not here: see start_dhcp(). */
		g_dhcp_started = false;
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		if (g_connected) {
			LOG_WRN("wifi: link lost");
		}
		{
			struct net_if *wifi_iface = net_if_get_first_wifi();

			if (wifi_iface != NULL) {
				net_dhcpv4_stop(wifi_iface);
			}
		}
		g_connected = false;
		g_connect_pending = false;
		break;
	default:
		break;
	}
}

void impulse_wifi_init(void)
{
	net_mgmt_init_event_callback(&g_cb, wifi_evt,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&g_cb);

	if (impulse_storage_load_wifi(&g_cred) == 0 && g_cred.ssid[0] != '\0') {
		g_have_cred = true;
		LOG_INF("wifi: credentials for \"%s\" restored", g_cred.ssid);
	} else {
		LOG_INF("wifi: no stored credentials");
	}
}

int impulse_wifi_set_credentials(const char *ssid, const char *psk)
{
	if (ssid == NULL || ssid[0] == '\0') {
		return -EINVAL;
	}

	memset(&g_cred, 0, sizeof(g_cred));
	strncpy(g_cred.ssid, ssid, IMPULSE_WIFI_SSID_MAX);
	if (psk != NULL) {
		strncpy(g_cred.psk, psk, IMPULSE_WIFI_PSK_MAX);
	}
	g_have_cred = true;

	int err = impulse_storage_save_wifi(&g_cred);

	if (err != 0) {
		LOG_ERR("wifi: credential save failed (%d)", err);
	}

	/*
	 * Fresh-offer preemption (§4.5): a new SSID restarts the attempt cycle
	 * immediately rather than waiting out the retry interval. Someone
	 * standing in front of a stranded device should see it recover in
	 * seconds, not after the backoff.
	 */
	g_last_attempt_ms = 0;
	g_connect_pending = false;
	LOG_INF("wifi: credentials set for \"%s\"", g_cred.ssid);
	return err;
}

enum impulse_wifi_state impulse_wifi_state(void)
{
	if (!g_have_cred) {
		return IMPULSE_WIFI_UNPROVISIONED;
	}
	return g_connected ? IMPULSE_WIFI_CONNECTED : IMPULSE_WIFI_PROVISIONED;
}

bool impulse_wifi_connected(void)
{
	return g_connected;
}

const char *impulse_wifi_ssid(void)
{
	return g_cred.ssid;
}

void impulse_wifi_ipv4(uint8_t out[4])
{
	struct net_if *iface = net_if_get_first_wifi();

	memset(out, 0, 4);
	if (iface == NULL) {
		return;
	}

	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

	if (ipv4 == NULL) {
		return;
	}
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (ipv4->unicast[i].ipv4.is_used &&
		    ipv4->unicast[i].ipv4.addr_state == NET_ADDR_PREFERRED) {
			memcpy(out, &ipv4->unicast[i].ipv4.address.in_addr, 4);
			return;
		}
	}
}

void impulse_wifi_request(bool want)
{
	if (want == g_want_link) {
		return;
	}
	g_want_link = want;
	LOG_INF("wifi: link %s", want ? "REQUESTED" : "released");

	if (!want && g_connected) {
		struct net_if *iface = net_if_get_first_wifi();

		if (iface != NULL) {
			(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface,
				       NULL, 0);
		}
	} else if (want) {
		/* Attempt immediately rather than at the next retry tick. */
		g_last_attempt_ms = 0;
	}
}

void impulse_wifi_tick(int64_t now_ms)
{
	/*
	 * DHCP must be STARTED explicitly: CONFIG_NET_DHCPV4 only compiles the
	 * client in, and without the connection manager nothing kicks it off —
	 * the interface associates happily and then sits at 0.0.0.0 forever,
	 * which looks like a working link right up until the first datagram
	 * goes nowhere.
	 *
	 * Driven from here rather than from the connect callback because
	 * starting it there is too early: the L2 has reported association but
	 * the carrier is not necessarily up, and the request is simply lost.
	 * Retried until an address appears.
	 */
	if (g_connected && !g_dhcp_started &&
	    (g_last_dhcp_ms == 0 || (now_ms - g_last_dhcp_ms) > 8000)) {
		struct net_if *wifi_iface = net_if_get_first_wifi();
		uint8_t ip[4];

		impulse_wifi_ipv4(ip);
		if (ip[0] != 0U) {
			g_dhcp_started = true;
		} else if (wifi_iface != NULL) {
			g_last_dhcp_ms = now_ms;
			LOG_INF("wifi: starting DHCP");
			net_dhcpv4_restart(wifi_iface);
		}
	}

	/* Report the address once, when it first appears. The app needs it for
	 * the anchor IP table, and on a board with no UART it is otherwise
	 * invisible without enabling the whole net shell. */
	static bool ip_logged;

	if (g_connected && !ip_logged) {
		uint8_t ip[4];

		impulse_wifi_ipv4(ip);
		if (ip[0] != 0U) {
			LOG_INF("wifi: address %u.%u.%u.%u", ip[0], ip[1],
				ip[2], ip[3]);
			ip_logged = true;
		}
	} else if (!g_connected) {
		ip_logged = false;
	}

	if (!g_want_link || !g_have_cred || g_connected || g_connect_pending) {
		return;
	}
	if (g_last_attempt_ms != 0 &&
	    (now_ms - g_last_attempt_ms) < WIFI_RETRY_INTERVAL_MS) {
		return;
	}
	g_last_attempt_ms = now_ms;

	struct net_if *iface = net_if_get_first_wifi();

	if (iface == NULL) {
		LOG_ERR("wifi: no interface — is the WM02C fitted and the "
			"driver built in? (SB_CONFIG_WIFI_NRF70)");
		return;
	}

	static struct wifi_connect_req_params params;

	memset(&params, 0, sizeof(params));
	params.ssid = (const uint8_t *)g_cred.ssid;
	params.ssid_length = strlen(g_cred.ssid);
	params.psk = (const uint8_t *)g_cred.psk;
	params.psk_length = strlen(g_cred.psk);
	params.security = (params.psk_length > 0) ? WIFI_SECURITY_TYPE_PSK
						  : WIFI_SECURITY_TYPE_NONE;
	params.channel = WIFI_CHANNEL_ANY;
	/* 2.4 GHz: WATCH_REMOVED has to cross a house, and 5 GHz does not. */
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.mfp = WIFI_MFP_OPTIONAL;

	int err = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params,
			   sizeof(params));

	if (err != 0) {
		LOG_WRN("wifi: connect request failed (%d)", err);
		return;
	}
	g_connect_pending = true;
	LOG_INF("wifi: associating with \"%s\"...", g_cred.ssid);
}
