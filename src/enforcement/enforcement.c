/* SPDX-License-Identifier: Apache-2.0 */
#include "enforcement.h"

#include <string.h>

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
	if (e->donning_grace_s > 0U) {
		ctx->donned_at_utc = now_utc;
		ctx->grace_deadline_utc =
			now_utc + (int64_t)e->donning_grace_s;
	} else {
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

bool impulse_enforcement_check_condition(struct impulse_enforcement_ctx *ctx,
					 int64_t now_utc, bool wifi_connected,
					 const char *wifi_ssid, bool docked)
{
	const struct impulse_event *e = ctx->active;
	bool met;

	if (e == NULL) {
		ctx->condition_met = true;
		return true;
	}

	/* Donning grace short-circuits to MET, the same shape as the Mode B
	 * tolerance below. No output, and the caller may sleep — but it must
	 * cap that sleep at the deadline so expiry is not slept through. */
	if (ctx->grace_deadline_utc != 0 && now_utc < ctx->grace_deadline_utc) {
		ctx->condition_met = true;
		return true;
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
			ctx->condition_met = true;
			return true;
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
	return ctx->condition_met ? IMPULSE_ENFORCEMENT_POLL_MET_S
				  : IMPULSE_ENFORCEMENT_POLL_NOT_MET_S;
}
