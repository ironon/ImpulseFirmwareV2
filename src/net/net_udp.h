/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IMPULSE_NET_UDP_H_
#define IMPULSE_NET_UDP_H_

#include <stdbool.h>
#include <stdint.h>

#include "../schedule/schedule.h"

/* §7 constant. */
#define IMPULSE_ANCHOR_UDP_PORT 5555

/* §6.1 commands. */
#define IMPULSE_UDP_WATCH_REMOVED 0x01
#define IMPULSE_UDP_WATCH_WORN    0x02

/* §6.1: [1 cmd][16 watch uuid][16 event uuid] */
#define IMPULSE_UDP_PACKET_LEN 33

/* ---- watch side ---- */

/*
 * Send a command to every anchor in event->beep_anchors.
 *
 * IDEMPOTENT BY CONSTRUCTION, and relied upon to be: §5.5.1 re-asserts
 * WATCH_REMOVED on EVERY enforcement poll rather than once on the worn edge,
 * because the single announcement was lost three different ways in the field —
 * no association at the instant of the edge, a dropped datagram (this is UDP
 * with no acknowledgement), and an anchor that rebooted or timed out its own
 * alarm. The anchor re-evaluates all of its guards per packet, so a repeat can
 * only ever restart an alarm that should already be running.
 *
 * Returns the number of anchors the datagram was handed to the stack for.
 */
int impulse_udp_send_to_anchors(uint8_t command,
				const struct impulse_event *event);

/* ---- anchor side ---- */

/* Start listening on IMPULSE_ANCHOR_UDP_PORT. */
int impulse_udp_listener_start(void);

#endif /* IMPULSE_NET_UDP_H_ */
