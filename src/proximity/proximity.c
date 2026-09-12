/* SPDX-License-Identifier: Apache-2.0 */
#include "proximity.h"

#include <string.h>

void impulse_prox_state_init(struct impulse_prox_state *st,
			     const uint8_t *anchor_id,
			     uint16_t near_enter_cm, uint16_t away_enter_cm)
{
	memset(st, 0, sizeof(*st));
	if (anchor_id != NULL) {
		memcpy(st->anchor_id, anchor_id, IMPULSE_UUID_LEN);
	}

	st->near_enter_cm = (near_enter_cm != 0U) ? near_enter_cm
						  : IMPULSE_NEAR_ENTER_CM;
	st->away_enter_cm = (away_enter_cm != 0U) ? away_enter_cm
						  : IMPULSE_AWAY_ENTER_CM;

	/* Clamp on the watch, never trusting the app (§4.7). A degenerate pair
	 * with no hysteresis band would oscillate; an inverted pair would make
	 * the state machine meaningless. */
	if (st->near_enter_cm < IMPULSE_THRESHOLD_MIN_CM) {
		st->near_enter_cm = IMPULSE_THRESHOLD_MIN_CM;
	}
	if (st->away_enter_cm > IMPULSE_THRESHOLD_MAX_CM) {
		st->away_enter_cm = IMPULSE_THRESHOLD_MAX_CM;
	}
	if (st->away_enter_cm <= st->near_enter_cm) {
		st->away_enter_cm = st->near_enter_cm + 50U;
	}

	/* No verdict until something is measured. Starting at NEAR would be a
	 * free pass on a stayNear commitment; starting at AWAY would fire a
	 * false alarm on one. have_verdict keeps both from happening. */
	st->verdict = IMPULSE_PROX_AWAY;
	st->have_verdict = false;
}

void impulse_prox_ingest(struct impulse_prox_state *st,
			 const struct impulse_cs_measurement *m)
{
	if (st == NULL || m == NULL) {
		return;
	}

	if (m->result != IMPULSE_CS_OK) {
		/* Abstention: no new evidence. The verdict holds and the dwell
		 * counters do NOT advance — but the abstain run does, because
		 * abstention cannot be free or "block the radio" becomes a
		 * bypass (§4.5). */
		if (st->abstain_run < UINT8_MAX) {
			st->abstain_run++;
		}
		return;
	}

	st->abstain_run = 0;

	if (m->distance_cm < st->near_enter_cm) {
		st->away_run = 0;
		if (st->near_run < UINT8_MAX) {
			st->near_run++;
		}
		if (st->near_run >= IMPULSE_NEAR_DWELL) {
			st->verdict = IMPULSE_PROX_NEAR;
			st->have_verdict = true;
		}
	} else if (m->distance_cm > st->away_enter_cm) {
		st->near_run = 0;
		if (st->away_run < UINT8_MAX) {
			st->away_run++;
		}
		/*
		 * AWAY_DWELL is double NEAR_DWELL and this is a THREAT-MODEL
		 * INVARIANT, not tuning. Physical attacks on a radio are
		 * overwhelmingly subtractive — shielding, a hand over the
		 * antenna, a wall. Subtraction can fabricate "far" and can
		 * fabricate "no measurement"; it cannot fabricate "near",
		 * because you cannot make a device appear CLOSER by attenuating
		 * anything. The cheap conclusion must be the harder one to
		 * reach. Raise both together if the corpus demands it; never
		 * invert the ratio.
		 */
		if (st->away_run >= IMPULSE_AWAY_DWELL) {
			st->verdict = IMPULSE_PROX_AWAY;
			st->have_verdict = true;
		}
	} else {
		/* Inside the hysteresis band: a real measurement, but not one
		 * that argues for a transition. Both runs reset so a person
		 * shifting in a chair cannot accumulate a flip. */
		st->near_run = 0;
		st->away_run = 0;
	}
}

bool impulse_prox_in_failsafe(const struct impulse_prox_state *st)
{
	return st != NULL &&
	       st->abstain_run > IMPULSE_CS_ABSTAIN_MAX_CONSECUTIVE;
}

bool impulse_prox_criterion_met(const struct impulse_prox_state *st,
				uint8_t criterion)
{
	if (st == NULL) {
		return false;
	}

	bool near;

	if (impulse_prox_in_failsafe(st) || !st->have_verdict) {
		/*
		 * Criterion-dependent fail-safe (§4.5, inherited from v2
		 * §5.4.1). Resolve toward NOT MET for stayNear and toward MET
		 * for getAway. Both directions treat a sustained inability to
		 * measure as the user's problem, because the alternative
		 * rewards jamming: if blocking the radio produced compliance,
		 * blocking the radio would be the bypass.
		 *
		 * phoneAway is the deliberate exception and is handled by the
		 * caller: v2 §5.4.1 step 3 requires it to FAIL OPEN, because
		 * the product must never fire a phone-distance alarm on an
		 * uncertain link.
		 */
		switch (criterion) {
		case IMPULSE_CRIT_STAY_NEAR:
			return false;
		case IMPULSE_CRIT_GET_AWAY:
			return true;
		case IMPULSE_CRIT_PHONE_AWAY:
			return true; /* fail open */
		default:
			return true;
		}
	}

	near = (st->verdict == IMPULSE_PROX_NEAR);

	switch (criterion) {
	case IMPULSE_CRIT_STAY_NEAR:
		return near;
	case IMPULSE_CRIT_GET_AWAY:
		return !near;
	case IMPULSE_CRIT_PHONE_AWAY:
		/* Only the ranging half. The caller fuses this with dock status
		 * and applies PHONE_AWAY_TOLERANCE_S (§5.4.1 Mode B). */
		return !near;
	default:
		return true;
	}
}

/* --- stub backend -------------------------------------------------------- */

static void stub_measure(const uint8_t *anchor_id,
			 struct impulse_cs_measurement *out)
{
	(void)anchor_id;
	memset(out, 0, sizeof(*out));
	/*
	 * TODO(api): replace with a real NCS channel-sounding procedure.
	 * v3 §4.3: prefer phase-based ranging (PBR) as the primary estimator
	 * with CS-RTT as the sanity check — PBR is finer, CS-RTT is far harder
	 * to spoof because it is bounded by the speed of light. Where they
	 * disagree beyond tolerance, report IMPULSE_CS_FAIL_DISAGREE rather
	 * than picking one.
	 *
	 * Failing (rather than returning a plausible distance) is deliberate:
	 * it drives the abstention path, so an unimplemented backend fails
	 * CLOSED for stayNear instead of silently reporting compliance.
	 */
	out->result = IMPULSE_CS_FAIL_PROCEDURE;
}

static int stub_reflector_start(void)
{
	return -1; /* TODO(api): anchor-role CS reflector. */
}

static int stub_reflector_stop(void)
{
	return -1;
}

static const struct impulse_cs_backend s_stub = {
	.name = "stub",
	.measure = stub_measure,
	.reflector_start = stub_reflector_start,
	.reflector_stop = stub_reflector_stop,
};

static const struct impulse_cs_backend *s_backend = &s_stub;

const struct impulse_cs_backend *impulse_cs_backend(void)
{
	return s_backend;
}

void impulse_cs_backend_set(const struct impulse_cs_backend *backend)
{
	s_backend = (backend != NULL) ? backend : &s_stub;
}
