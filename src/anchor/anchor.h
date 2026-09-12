/*
 * Anchor role — firmware_spec_v2.md §4, inherited by v3 §0.2 with the
 * proximity chapters deleted.
 *
 * The anchor is the same PCB as the watch (v3 §1.3); only the Kconfig role and
 * the case differ. It does NOT enforce and it does not decide anything about
 * proximity — under channel sounding it is a passive reflector participating
 * in a measurement the watch interprets (v3 §4.2).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_ANCHOR_H_
#define IMPULSE_ANCHOR_H_

#include <stdbool.h>
#include <stdint.h>

#include "../schedule/schedule.h"

/* §4.8 beeping patterns, driven by the active event's anchorProfile. */
struct impulse_anchor_beep {
	bool active;
	uint8_t profile; /* enum impulse_anchor_profile */
	bool buzzer_on;
	uint32_t phase_elapsed_ms;
	uint32_t total_elapsed_ms;
};

void impulse_anchor_init(void);

/* The anchor's own identity (§4.2). Generated once on first boot from the
 * device's unique FICR id and persisted; permanent until factory reset. */
const uint8_t *impulse_anchor_uuid(void);

/* Begin/stop the removal alarm. Started by a WATCH_REMOVED command, stopped by
 * WATCH_WORN, by the window ending, or by max_beep_minutes. */
void impulse_anchor_beep_start(uint8_t anchor_profile);
void impulse_anchor_beep_stop(void);

/* Drive the beep state machine. Returns true if the buzzer state changed. */
bool impulse_anchor_beep_tick(struct impulse_anchor_beep *b, uint32_t dt_ms,
			      uint16_t max_beep_minutes);

/*
 * §4.9: is an enforcement event active AND does it involve THIS anchor —
 * either as event.anchorId or in event.beepAnchors? Open commands are refused
 * while this is true; close commands never are.
 */
bool impulse_anchor_in_active_event(const struct impulse_schedule *sched,
				    const struct impulse_date *today,
				    uint16_t minute_of_day,
				    int16_t tz_offset_minutes);

/* Handle a Toggle write (§4.9). Returns the response byte: 0x01 accepted,
 * 0x02 rejected because an enforcement event involving this anchor is live. */
uint8_t impulse_anchor_toggle(uint8_t value, bool in_active_event);

/* Called when the schedule or the clock says a window involving this anchor
 * has just begun. Auto-closes the strap, bypassing the enforcement check. */
void impulse_anchor_on_window_start(void);

struct impulse_anchor_beep *impulse_anchor_beep_state(void);

/* §4.4 Identify: beep briefly so a human can tell which physical anchor this
 * UUID belongs to. Blocking; call from a workqueue, never a BLE callback. */
void impulse_anchor_identify(void);

/* §6.1 command packet: [1 cmd][16 watch uuid][16 event uuid]. */
#define IMPULSE_CMD_WATCH_REMOVED 0x01
#define IMPULSE_CMD_WATCH_WORN    0x02
#define IMPULSE_CMD_PACKET_LEN    33

/*
 * Handle a watch->anchor command, whatever carried it.
 *
 * Lives here rather than in the UDP layer because the BLE transport must work
 * on an anchor built with NO WiFi at all — and because having one validator
 * means the two carriers cannot drift apart in what they accept.
 */
void impulse_anchor_handle_command(const uint8_t *pkt, size_t len);

#endif /* IMPULSE_ANCHOR_H_ */
