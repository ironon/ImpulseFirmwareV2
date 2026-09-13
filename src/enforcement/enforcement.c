/* SPDX-License-Identifier: Apache-2.0 */
#include "enforcement.h"

#include <string.h>

static void link_reset(struct impulse_enforcement_ctx *ctx)
{
	ctx->link_armed = false;
	ctx->link_window_start_ms = 0;
	ctx->link_loss_activity_ms = 0;
	ctx->link_loss_away_applied = false;
	ctx->link_grace = false;
	ctx->link_grace_after_ms = 0;
	ctx->link_grace_deadline_ms = 0;
	ctx->link_grace_cooldown_until_ms = 0;
	ctx->burst_last_ms = 0;
	ctx->bursts_since_poll = 0;
}

void impulse_enforcement_init(struct impulse_enforcement_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state = IMPULSE_STATE_DORMANT;
	ctx->condition_met = true;
	impulse_profile_stop(&ctx->profile_run);
}

void impulse_enforcement_enter(struct impulse_enforcement_ctx *ctx,
			       const struct impulse_event *e, int64_t now_utc,
			       bool worn_now)
{
	ctx->state = IMPULSE_STATE_ENFORCEMENT;
	ctx->active = e;
	ctx->worn = worn_now;
	ctx->phone_near_since_utc = 0;
	ctx->phone_undock_latched = false;
	link_reset(ctx);

	/* Fresh ranging state per window — no verdict carried in from before
	 * the commitment started. */
	if (impulse_criteria_is_anchor_based(e->criteria)) {
		impulse_prox_state_init(&ctx->prox,
					e->has_anchor_id ? e->anchor_id : NULL,
					0, 0);
	}

	/*
	 * §5.4.4 donning grace. donned_at is the most recent not-worn -> worn
	 * transition, OR the window start if the watch is already on — donning
	 * the watch just before a window should not be punished.
	 */
	if (e->donning_grace_s > 0U && worn_now) {
		ctx->donned_at_utc = now_utc;
		ctx->grace_deadline_utc =
			now_utc + (int64_t)e->donning_grace_s;
	} else {
		/*
		 * NOT worn at the window start => NO grace. The `worn_now`
		 * test is load-bearing and was missing: the grace opened on
		 * every window regardless, so a watch sitting on the nightstand
		 * — which is exactly the Sunrise Lock case, §5.4.4 — entered
		 * its window already inside a grace it had not earned and
		 * stayed silent for donning_grace_s. At the spec maximum of
		 * 1800 s that is the entire alarm.
		 *
		 * Donning later still opens a fresh grace via set_worn(),
		 * which is the path the feature is actually for.
		 */
		ctx->donned_at_utc = 0;
		ctx->grace_deadline_utc = 0;
	}

	/* Start compliant: the first poll decides. Starting non-compliant would
	 * fire output before anything had been measured. */
	ctx->condition_met = true;
	impulse_profile_stop(&ctx->profile_run);
}

void impulse_enforcement_exit(struct impulse_enforcement_ctx *ctx)
{
	ctx->state = IMPULSE_STATE_DORMANT;
	ctx->active = NULL;
	ctx->condition_met = true;
	link_reset(ctx);
	impulse_profile_stop(&ctx->profile_run);
}

void impulse_enforcement_set_worn(struct impulse_enforcement_ctx *ctx,
				  bool worn, int64_t now_utc)
{
	bool was = ctx->worn;

	ctx->worn = worn;

	if (!was && worn && ctx->active != NULL &&
	    ctx->active->donning_grace_s > 0U) {
		/* A fresh grace period on every donning (§5.4.4). */
		ctx->donned_at_utc = now_utc;
		ctx->grace_deadline_utc =
			now_utc + (int64_t)ctx->active->donning_grace_s;
	}
	if (was && !worn) {
		/* Taking the watch off cancels the grace. The anchor beeping
		 * side of this is WATCH_REMOVED, which needs WiFi (v3 §1.5) and
		 * is a chunk-I concern. */
		ctx->grace_deadline_utc = 0;
	}
}

static bool ssid_matches(const struct impulse_event *e, const char *ssid)
{
	if (ssid == NULL || e->ssid_len == 0U) {
		return false;
	}
	return strncmp(e->wifi_ssid, ssid, IMPULSE_MAX_SSID_LEN) == 0;
}

/*
 * Single exit for check_condition. The profile transition (start on not-met,
 * stop on met) MUST run for every path, including the short-circuits.
 *
 * It used to live only at the bottom of check_condition, so the three early
 * returns above it — no active event, donning grace, and the phoneAway
 * fail-open — set condition_met = true and returned WITHOUT ever calling
 * impulse_profile_stop(). profile_run therefore kept motor_on = 1, and since
 * main() drives the pads from profile_run every pass, putting the watch on
 * during an alarm reported cond_met=1 while the vibration motor kept running
 * until the window closed. Observed on hardware 2026-09-12: worn=1,
 * cond_met=1, P3 OUT = 0x08 for the whole grace period.
 */
static bool settle(struct impulse_enforcement_ctx *ctx,
		   const struct impulse_event *e, bool met)
{
	if (met != ctx->condition_met) {
		if (met) {
			/* All output stops IMMEDIATELY on the transition to
			 * met (§5.4.1). */
			impulse_profile_stop(&ctx->profile_run);
		} else {
			/* Not-met restarts the profile from its beginning. */
			impulse_profile_start(&ctx->profile_run, e->profile);
		}
	}

	ctx->condition_met = met;
	return met;
}

bool impulse_enforcement_check_condition(struct impulse_enforcement_ctx *ctx,
					 int64_t now_utc, bool wifi_connected,
					 const char *wifi_ssid, bool docked)
{
	const struct impulse_event *e = ctx->active;
	bool met;

	if (e == NULL) {
		return settle(ctx, e, true);
	}

	/* Donning grace short-circuits to MET, the same shape as the Mode B
	 * tolerance below. No output, and the caller may sleep — but it must
	 * cap that sleep at the deadline so expiry is not slept through. */
	if (ctx->grace_deadline_utc != 0 && now_utc < ctx->grace_deadline_utc) {
		return settle(ctx, e, true);
	}

	switch (e->criteria) {
	case IMPULSE_CRIT_GET_ON_WIFI:
		met = wifi_connected && ssid_matches(e, wifi_ssid);
		break;
	case IMPULSE_CRIT_GET_OFF_WIFI:
		met = !wifi_connected || !ssid_matches(e, wifi_ssid);
		break;
	case IMPULSE_CRIT_STAY_NEAR:
	case IMPULSE_CRIT_GET_AWAY:
		met = impulse_prox_criterion_met(&ctx->prox, e->criteria);
		break;
	case IMPULSE_CRIT_PHONE_AWAY: {
		/*
		 * Mode B (§5.4.1). near_phone = undocked OR prox==NEAR: the
		 * phone is "with the user" whether the user went to the dock or
		 * the phone left it.
		 *
		 * FAIL OPEN, unlike every other criterion: if the link is
		 * degraded the result resolves to compliant. The product must
		 * never fire a phone-distance alarm on an uncertain connection.
		 */
		if (impulse_prox_in_failsafe(&ctx->prox) ||
		    !ctx->prox.have_verdict) {
			ctx->phone_near_since_utc = 0;
			return settle(ctx, e, true);
		}

		bool undocked = !docked || ctx->phone_undock_latched;
		bool near_phone =
			undocked || (ctx->prox.verdict == IMPULSE_PROX_NEAR);

		if (!near_phone) {
			ctx->phone_near_since_utc = 0;
			met = true;
			break;
		}

		/* Tolerance: a quick check is fine. Only a CONTINUOUS
		 * near_phone past the tolerance is actionable. */
		if (ctx->phone_near_since_utc == 0) {
			ctx->phone_near_since_utc = now_utc;
		}
		met = (now_utc - ctx->phone_near_since_utc) <
		      (int64_t)IMPULSE_PHONE_AWAY_TOLERANCE_S;
		break;
	}
	default:
		/* Unknown criterion: fail open rather than enforce something
		 * this build does not understand. */
		met = true;
		break;
	}

	return settle(ctx, e, met);
}

bool impulse_enforcement_tick(struct impulse_enforcement_ctx *ctx,
			      uint32_t dt_ms)
{
	if (ctx->state != IMPULSE_STATE_ENFORCEMENT || ctx->condition_met) {
		return false;
	}
	return impulse_profile_tick(&ctx->profile_run, dt_ms);
}

uint32_t impulse_enforcement_poll_interval_s(
	const struct impulse_enforcement_ctx *ctx)
{
	/*
	 * ABSTENTION MUST NOT BUY THE SLOW CADENCE.
	 *
	 * The criterion-dependent fail-safe makes getAway report "met" when
	 * there is no measurement — which is right for enforcement, but wrong
	 * for scheduling: it meant the watch polled every 180 s precisely when
	 * it knew nothing, so it stayed ignorant longer and kept failing open.
	 * A watch with no verdict is not a watch in a settled state; poll it at
	 * the attentive cadence until it actually knows something.
	 */
	if (!ctx->prox.have_verdict) {
		return IMPULSE_ENFORCEMENT_POLL_NOT_MET_S;
	}

	/*
	 * Mid-abstention with output running: the fastest cadence. This is the
	 * user walking out of the room while the alarm sounds — the link drops,
	 * every poll abstains, and only abstentions can release the criterion.
	 * Polling that at 60 s made leaving take six minutes to register.
	 */
	if (!ctx->condition_met && ctx->prox.abstain_run > 0U) {
		return IMPULSE_ENFORCEMENT_POLL_ABSTAIN_S;
	}

	return ctx->condition_met ? IMPULSE_ENFORCEMENT_POLL_MET_S
				  : IMPULSE_ENFORCEMENT_POLL_NOT_MET_S;
}

static void link_arm(struct impulse_enforcement_ctx *ctx, int64_t now_ms)
{
	if (!ctx->link_armed) {
		ctx->link_armed = true;
		ctx->link_window_start_ms = now_ms;
	}
}

static void conclude_away(struct impulse_enforcement_ctx *ctx, int64_t now_utc)
{
	ctx->link_grace = false;
	impulse_prox_force_away(&ctx->prox);
	/* Settle now rather than on the next poll, so output stops (getAway) or
	 * starts (stayNear) on this pass. The WiFi and dock arguments are the
	 * ones main() passes; every anchor-based criterion ignores the WiFi pair. */
	(void)impulse_enforcement_check_condition(ctx, now_utc, false, NULL,
						  true);
}

void impulse_enforcement_link_update(struct impulse_enforcement_ctx *ctx,
				     const struct impulse_cs_link_obs *obs,
				     int64_t now_ms, int64_t now_utc)
{
	const struct impulse_event *e = ctx->active;

	if (ctx->state != IMPULSE_STATE_ENFORCEMENT || e == NULL ||
	    !impulse_criteria_is_anchor_based(e->criteria) || obs == NULL ||
	    !obs->known) {
		ctx->link_grace = false;
		return;
	}

	link_arm(ctx, now_ms);

	/* Activity from before this window — the link the previous window was
	 * releasing — is not a link this window ever had, so it cannot be lost.
	 * A watch that never connects stays on the abstention path (§4.5). */
	bool seen = obs->activity_seen &&
		    obs->last_activity_ms >= ctx->link_window_start_ms;
	bool lost = seen && (now_ms - obs->last_activity_ms) >=
				    IMPULSE_CS_LINK_LOST_DETECT_MS;

	/* Met by any other route (donning grace, a real AWAY): nothing left to
	 * silence. */
	if (ctx->condition_met) {
		ctx->link_grace = false;
	}

	if (lost) {
		if (ctx->link_loss_activity_ms != obs->last_activity_ms) {
			/* A new loss. */
			ctx->link_loss_activity_ms = obs->last_activity_ms;
			ctx->link_loss_away_applied = false;

			if (e->criteria == IMPULSE_CRIT_GET_AWAY &&
			    !ctx->condition_met && !ctx->link_grace &&
			    now_ms >= ctx->link_grace_cooldown_until_ms) {
				ctx->link_grace = true;
				ctx->link_grace_after_ms = obs->last_activity_ms;
				ctx->link_grace_deadline_ms =
					obs->last_activity_ms +
					IMPULSE_LINK_LOST_AWAY_MS;
				ctx->link_grace_cooldown_until_ms =
					now_ms + IMPULSE_LINK_GRACE_COOLDOWN_MS;
			}
		}

		if (!ctx->link_loss_away_applied &&
		    (now_ms - ctx->link_loss_activity_ms) >=
			    IMPULSE_LINK_LOST_AWAY_MS) {
			ctx->link_loss_away_applied = true;
			conclude_away(ctx, now_utc);
			return;
		}
	} else {
		ctx->link_loss_activity_ms = 0;
	}

	if (!ctx->link_grace) {
		return;
	}

	/*
	 * A measurement completed AFTER the loss that is not far enough to be
	 * AWAY proves the user is still noncompliant: punish again, from the
	 * start of the profile. A far one does not end the grace — it agrees
	 * with the conclusion the grace is waiting for.
	 */
	if (obs->have_result && obs->result_ms > ctx->link_grace_after_ms &&
	    obs->result_cm <= ctx->prox.away_enter_cm) {
		ctx->link_grace = false;
		impulse_profile_start(&ctx->profile_run, e->profile);
		return;
	}

	/* The window closed without a noncompliant measurement — including the
	 * case where the link came back but produced none, or only far ones. */
	if (now_ms >= ctx->link_grace_deadline_ms) {
		conclude_away(ctx, now_utc);
	}
}

bool impulse_enforcement_output_silenced(
	const struct impulse_enforcement_ctx *ctx)
{
	return ctx->state == IMPULSE_STATE_ENFORCEMENT && ctx->link_grace;
}

bool impulse_enforcement_burst_update(struct impulse_enforcement_ctx *ctx,
				      const struct impulse_cs_link_obs *obs,
				      int64_t now_ms, int64_t now_utc)
{
	const struct impulse_event *e = ctx->active;

	if (ctx->state != IMPULSE_STATE_ENFORCEMENT || e == NULL ||
	    !impulse_criteria_is_anchor_based(e->criteria) || obs == NULL ||
	    !obs->known) {
		return false;
	}

	link_arm(ctx, now_ms);

	/* Only bursts that completed during this window, each exactly once. A
	 * result left over from the previous window is not a measurement of
	 * this commitment. */
	if (!obs->have_result || obs->result_ms < ctx->link_window_start_ms ||
	    obs->result_ms <= ctx->burst_last_ms) {
		return false;
	}

	struct impulse_cs_measurement m;

	memset(&m, 0, sizeof(m));
	m.result = IMPULSE_CS_OK;
	m.distance_cm = obs->result_cm;

	ctx->burst_last_ms = obs->result_ms;
	if (ctx->bursts_since_poll < UINT16_MAX) {
		ctx->bursts_since_poll++;
	}
	impulse_prox_ingest(&ctx->prox, &m);
	(void)impulse_enforcement_check_condition(ctx, now_utc, false, NULL,
						  true);
	return true;
}

void impulse_enforcement_poll_abstain_if_idle(
	struct impulse_enforcement_ctx *ctx)
{
	if (ctx->bursts_since_poll == 0U) {
		struct impulse_cs_measurement m;

		memset(&m, 0, sizeof(m));
		m.result = IMPULSE_CS_FAIL_PROCEDURE;
		impulse_prox_ingest(&ctx->prox, &m);
	}
	ctx->bursts_since_poll = 0;
}
