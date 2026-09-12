/* SPDX-License-Identifier: Apache-2.0 */
#include "net_udp.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include "../identity.h"
#include "../storage/storage.h"
#include "net_wifi.h"

LOG_MODULE_REGISTER(impulse_udp, LOG_LEVEL_INF);

#define ANCHOR_IP_MAX 8

static void encode_packet(uint8_t *out, uint8_t command,
			  const uint8_t *watch_uuid, const uint8_t *event_uuid)
{
	out[0] = command;
	memcpy(&out[1], watch_uuid, IMPULSE_UUID_LEN);
	memcpy(&out[1 + IMPULSE_UUID_LEN], event_uuid, IMPULSE_UUID_LEN);
}

static int send_one(int sock, const uint8_t ip[4], const uint8_t *pkt)
{
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port = htons(IMPULSE_ANCHOR_UDP_PORT),
	};

	memcpy(&dst.sin_addr, ip, 4);

	int rc = zsock_sendto(sock, pkt, IMPULSE_UDP_PACKET_LEN, 0,
			      (struct sockaddr *)&dst, sizeof(dst));

	if (rc != IMPULSE_UDP_PACKET_LEN) {
		/* Logged, always. An escalation that fails silently is exactly
		 * the §5.5.1 failure this path exists to prevent, and the first
		 * version of this function only logged on SUCCESS — so a
		 * refused sendto looked identical to no window being open. */
		LOG_WRN("udp: sendto %u.%u.%u.%u failed (rc=%d errno=%d)",
			ip[0], ip[1], ip[2], ip[3], rc, errno);
		return -EIO;
	}
	return 0;
}

#if defined(CONFIG_IMPULSE_ROLE_WATCH)

int impulse_udp_send_to_anchors(uint8_t command,
				const struct impulse_event *event)
{
	if (event == NULL || event->beep_anchor_count == 0U) {
		return 0;
	}
	if (!impulse_wifi_connected()) {
		/* §5.5.1 re-asserts on the next poll, which is the whole reason
		 * it re-asserts — but say so at INF. "Nothing happened and
		 * nothing was logged" is how the original anchor-escalation bug
		 * survived in the field for weeks. */
		LOG_INF("udp: cmd 0x%02x deferred — no association yet", command);
		return 0;
	}

	static struct impulse_anchor_ip tbl[ANCHOR_IP_MAX];
	uint8_t count = 0;

	(void)impulse_storage_load_anchor_ips(tbl, ANCHOR_IP_MAX, &count);

	uint8_t pkt[IMPULSE_UDP_PACKET_LEN];

	encode_packet(pkt, command, impulse_watch_uuid(), event->id);

	int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock < 0) {
		LOG_ERR("udp: socket failed (%d)", errno);
		return 0;
	}

	/*
	 * SO_BROADCAST is REQUIRED for the 255.255.255.255 fallback below.
	 * Without it sendto() refuses the datagram outright, and because the
	 * first version of this code logged only successes, the whole
	 * escalation path failed completely silently: window open, watch
	 * unworn, WiFi associated, polls running, and not one packet.
	 */
	int on = 1;

	if (zsock_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on,
			     sizeof(on)) < 0) {
		/*
		 * Zephyr answers ENOPROTOOPT (109) here and SENDS THE BROADCAST
		 * ANYWAY — measured 2026-09-12. So this is not a failure, and
		 * it must not be logged as one: at WRN it fired on every send,
		 * including unicasts that had nothing to do with broadcast,
		 * which is exactly the kind of standing false alarm that trains
		 * people to ignore the log.
		 */
		LOG_DBG("udp: SO_BROADCAST unsupported (%d); harmless here",
			errno);
	}

	int sent = 0;

	for (uint8_t b = 0; b < event->beep_anchor_count; b++) {
		const uint8_t *want = event->beep_anchors[b];
		bool found = false;

		for (uint8_t i = 0; i < count; i++) {
			if (memcmp(tbl[i].uuid, want, IMPULSE_UUID_LEN) != 0) {
				continue;
			}
			found = true;
			if (send_one(sock, tbl[i].ip, pkt) == 0) {
				sent++;
				LOG_INF("udp: cmd 0x%02x -> %u.%u.%u.%u",
					command, tbl[i].ip[0], tbl[i].ip[1],
					tbl[i].ip[2], tbl[i].ip[3]);
			}
			break;
		}

		if (!found) {
			/*
			 * Attempt 3 of §5.5.1: broadcast. The app has not told
			 * us this anchor's IP (or the table is stale), and an
			 * anchor that never hears WATCH_REMOVED is a silent
			 * hole in the commitment. The anchor validates the
			 * event UUID and its own membership in beepAnchors on
			 * every packet, so a broadcast reaches exactly the
			 * anchors that should act and is ignored by the rest.
			 *
			 * mDNS (attempt 2) is deliberately skipped: it costs a
			 * resolver and multicast group membership to save one
			 * broadcast datagram on a home LAN.
			 */
			static const uint8_t bcast[4] = {255, 255, 255, 255};

			if (send_one(sock, bcast, pkt) == 0) {
				sent++;
				LOG_INF("udp: cmd 0x%02x -> broadcast "
					"(no IP known for this anchor)",
					command);
			}
		}
	}

	(void)zsock_close(sock);
	return sent;
}

#endif /* CONFIG_IMPULSE_ROLE_WATCH */

#if defined(CONFIG_IMPULSE_ROLE_ANCHOR)

#include "../anchor/anchor.h"
#include "../app_api.h"

#define UDP_RX_STACK 2048
#define UDP_RX_PRIO  6

K_THREAD_STACK_DEFINE(udp_rx_stack, UDP_RX_STACK);
static struct k_thread udp_rx_thread;

/*
 * §4.6. Every guard is re-evaluated PER PACKET, deliberately: the watch
 * re-asserts WATCH_REMOVED on every enforcement poll, so this runs constantly
 * during a window, and it must be safe to run constantly. It is also what makes
 * re-assertion able to restart an alarm this anchor lost to its own reboot or
 * max_beep_minutes timeout, without ever starting one that should not run.
 */
static void handle_packet(const uint8_t *pkt, size_t len)
{
	if (len != IMPULSE_UDP_PACKET_LEN) {
		LOG_WRN("udp: ignoring %u-byte datagram (expected %d)",
			(unsigned)len, IMPULSE_UDP_PACKET_LEN);
		return;
	}

	uint8_t command = pkt[0];
	const uint8_t *event_uuid = &pkt[1 + IMPULSE_UUID_LEN];

	if (command == IMPULSE_UDP_WATCH_WORN) {
		/* §4.6: stop all beeping immediately. Unconditional — a watch
		 * saying "I am back on" must never be second-guessed. */
		LOG_INF("udp: WATCH_WORN — stopping alarm");
		impulse_anchor_beep_stop();
		return;
	}

	if (command != IMPULSE_UDP_WATCH_REMOVED) {
		return;
	}

	const struct impulse_event *ev = impulse_app_find_active_event(event_uuid);

	if (ev == NULL) {
		LOG_DBG("udp: WATCH_REMOVED for an event that is not active here");
		return;
	}

	/* Is THIS anchor in the event's beepAnchors? If not, ignore (§4.6.5). */
	const uint8_t *me = impulse_anchor_uuid();
	bool mine = false;

	for (uint8_t i = 0; i < ev->beep_anchor_count; i++) {
		if (memcmp(ev->beep_anchors[i], me, IMPULSE_UUID_LEN) == 0) {
			mine = true;
			break;
		}
	}
	if (!mine) {
		LOG_DBG("udp: WATCH_REMOVED but this anchor is not in beepAnchors");
		return;
	}

	uint8_t profile = ev->anchor_profile;

	if (profile > IMPULSE_ANCHOR_PROFILE_HARD) {
		/* §4.11.1 precedent: a missing anchorProfile defaults to MEDIUM
		 * rather than silently not alarming. A commitment whose alarm
		 * is dead because a field was omitted is the worst outcome. */
		profile = IMPULSE_ANCHOR_PROFILE_MEDIUM;
	}

	impulse_anchor_beep_start(profile);
}

static void udp_rx_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

		if (sock < 0) {
			k_sleep(K_SECONDS(5));
			continue;
		}

		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(IMPULSE_ANCHOR_UDP_PORT),
			.sin_addr.s_addr = htonl(INADDR_ANY),
		};

		if (zsock_bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			LOG_ERR("udp: bind to %d failed (%d)",
				IMPULSE_ANCHOR_UDP_PORT, errno);
			(void)zsock_close(sock);
			k_sleep(K_SECONDS(5));
			continue;
		}

		LOG_INF("udp: listening on %d", IMPULSE_ANCHOR_UDP_PORT);

		for (;;) {
			uint8_t buf[64];
			struct sockaddr_in from;
			socklen_t fromlen = sizeof(from);

			int n = zsock_recvfrom(sock, buf, sizeof(buf), 0,
					       (struct sockaddr *)&from,
					       &fromlen);

			if (n < 0) {
				LOG_WRN("udp: recv failed (%d), re-opening", errno);
				break;
			}
			handle_packet(buf, (size_t)n);
		}

		(void)zsock_close(sock);
		k_sleep(K_SECONDS(2));
	}
}

int impulse_udp_listener_start(void)
{
	(void)k_thread_create(&udp_rx_thread, udp_rx_stack, UDP_RX_STACK,
			      udp_rx_entry, NULL, NULL, NULL, UDP_RX_PRIO, 0,
			      K_NO_WAIT);
	k_thread_name_set(&udp_rx_thread, "impulse_udp");
	return 0;
}

#endif /* CONFIG_IMPULSE_ROLE_ANCHOR */
