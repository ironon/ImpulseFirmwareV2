/*
 * Enforcement — firmware_spec_v2.md §5.1, §5.4. Watch role.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_ENFORCEMENT_H_
#define IMPULSE_ENFORCEMENT_H_

#include <stdbool.h>
#include <stdint.h>

#include "../proximity/proximity.h"
#include "../schedule/schedule.h"
#include "profiles.h"

/* §5.1.2 activity states. DORMANT_SLEEP is a Zephyr power state on this
 * platform, not an explicit sleep call in the loop (v3 §6.1 delta 1). */
enum impulse_state {
	IMPULSE_STATE_UNPAIRED = 0,
	IMPULSE_STATE_DORMANT,
	IMPULSE_STATE_DORMANT_SLEEP,
	IMPULSE_STATE_ENFORCEMENT,
};

/* §5.4.1 polling. The v2 600 s STILL tier is DELETED along with the motion
 * channel that justified it — it existed because a motionless wrist could not
 * produce new fading draws, which does not constrain a time-of-flight
 * measurement (v3 §4.3). */
#define IMPULSE_ENFORCEMENT_POLL_NOT_MET_S 60U
#define IMPULSE_ENFORCEMENT_POLL_MET_S     180U

/*
 * ABSTAINING WHILE THE ALARM IS RUNNING IS THE URGENT CASE, NOT THE IDLE ONE.
 *
 * getAway can essentially never resolve by the normal route: channel sounding
 * needs the BLE link, so walking out of the room DESTROYS the measurement
 * rather than producing a large one. AWAY_DWELL is unreachable in practice and
 * the criterion can only release through the abstention fail-safe — which
 * needs CS_ABSTAIN_MAX_CONSECUTIVE + 1 = 6 polls.
 *
 * At the NOT_MET cadence that is 6 x 60 s = SIX MINUTES of alarm after the
 * user has already done the thing the alarm was demanding — a feedback loop
 * nobody can perceive is not enforcing anything, it is just noise.
 *
 * CORRECTION, 2026-09-12. This was first justified by a hardware test in which
 * the alarm "did not stop when the user left the room, only on reset". That
 * observation was NOT this timeout: read over SWD, the watch had halted on a
 * SoftDevice Controller assert (23, 587) with the motor pad latched on, which
 * is exactly the "only reset stops it" signature. The arithmetic above still
 * stands on its own and the faster cadence is still right, but no hardware
 * measurement has yet shown this path being slow. See agent-notes.
 *
 * At 10 s the same six polls resolve in about a minute. The trade-off is
 * deliberate and worth stating: ABSTAIN_MAX exists so that jamming the radio
 * is not a free bypass (§4.5), and this makes that bypass cost ~60 s rather
 * than ~6 min. It is still a cost, and it is bounded by the same dwell count;
 * only the clock changed. Do not raise it back without re-reading §4.5 and
 * asking what a user is supposed to learn from an alarm that ignores them.
 */
#define IMPULSE_ENFORCEMENT_POLL_ABSTAIN_S 10U
#define IMPULSE_PHONE_AWAY_TOLERANCE_S     60U
/* §3.2: wake this far before a boundary and poll into it. Not drift
 * compensation — the LFXO makes drift negligible — but insurance against
 * scheduling jitter, because a boundary missed by any margin is missed
 * permanently. */
#define IMPULSE_CLOCK_GUARD_S 5U

struct impulse_enforcement_ctx {
	enum impulse_state state;
	const struct impulse_event *active;
	bool condition_met;
	bool worn;

	struct impulse_profile_run profile_run;
	struct impulse_prox_state prox;

	/* §5.4.4 donning grace. Wall-clock, deliberately: it must survive
	 * enforcement light sleep, and it is a comfort feature rather than an
	 * integrity timer, so the elapsed-time rule of §9.2 does not apply. */
	int64_t donned_at_utc;
	int64_t grace_deadline_utc;

	/* Mode B tolerance — first moment the phone read as near. */
	int64_t phone_near_since_utc;
	bool phone_undock_latched;
};

void impulse_enforcement_init(struct impulse_enforcement_ctx *ctx);

/* Entering the window for `e`. Resets per-event state. */
void impulse_enforcement_enter(struct impulse_enforcement_ctx *ctx,
			       const struct impulse_event *e, int64_t now_utc,
			       bool worn_now);

void impulse_enforcement_exit(struct impulse_enforcement_ctx *ctx);

/* Evaluate the criterion. `wifi_connected` / `wifi_ssid` describe the current
 * association; `docked` is the anchor's dock status for Mode B. */
bool impulse_enforcement_check_condition(struct impulse_enforcement_ctx *ctx,
					 int64_t now_utc, bool wifi_connected,
					 const char *wifi_ssid, bool docked);

/* Drive the output profile. `dt_ms` since the last call. Returns true if the
 * motor/buzzer state changed and the caller should push it to hardware. */
bool impulse_enforcement_tick(struct impulse_enforcement_ctx *ctx,
			      uint32_t dt_ms);

/* Seconds until the next condition poll, honouring the tiered interval. */
uint32_t impulse_enforcement_poll_interval_s(
	const struct impulse_enforcement_ctx *ctx);

/* §5.2 worn transitions drive anchor beeping and the donning grace. */
void impulse_enforcement_set_worn(struct impulse_enforcement_ctx *ctx,
				  bool worn, int64_t now_utc);

#endif /* IMPULSE_ENFORCEMENT_H_ */
