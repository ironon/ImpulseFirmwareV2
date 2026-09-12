/*
 * Proximity — Bluetooth Channel Sounding. firmware_spec_v3_nrf.md §4.
 *
 * This is the ONE genuinely new subsystem in the port. The ~4,650-line
 * RF-fingerprint engine is DELETED, not deprecated: channel sounding measures
 * distance directly, so the machinery that existed to infer it from RSSI is
 * answering a question nobody asks any more.
 *
 * STATUS: interface complete, backend STUBBED. The NCS channel-sounding API is
 * the assumption in v3 most likely to be wrong (§11: "Check this first"), and
 * none of it can be validated without two boards. Everything below the
 * verdict state machine is therefore behind impulse_cs_backend_*, which is the
 * only part that should need rewriting when the real API is known.
 *
 * What is NOT stubbed, and must not be simplified away, is the §4.4 verdict
 * state machine and the §4.5 abstention rules. Those are threat-model
 * invariants carried from the deleted engine, not artefacts of it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_PROXIMITY_H_
#define IMPULSE_PROXIMITY_H_

#include <stdbool.h>
#include <stdint.h>

#include "../schedule/event.h"

/* §4.4 starting values. Every one is a STARTING VALUE, not a measurement —
 * v3 §11 names what would overturn each. */
#define IMPULSE_NEAR_ENTER_CM 200U /* "in the room, at the thing" */
#define IMPULSE_AWAY_ENTER_CM 350U /* 150 cm hysteresis band */
#define IMPULSE_NEAR_DWELL    2U
#define IMPULSE_AWAY_DWELL    4U   /* deliberately DOUBLE — see below */
#define IMPULSE_CS_ABSTAIN_MAX_CONSECUTIVE 5U

/* Bounds on a per-anchor threshold override (§4.7). Enforced on the WATCH,
 * never trusted from the app: raising a threshold is a loosening and a widened
 * threshold the user could set instantly would bypass every stayNear
 * commitment they hold. */
#define IMPULSE_THRESHOLD_MIN_CM 50U
#define IMPULSE_THRESHOLD_MAX_CM 1500U

enum impulse_prox_verdict {
	IMPULSE_PROX_NEAR = 0,
	IMPULSE_PROX_AWAY = 1,
};

enum impulse_cs_result {
	IMPULSE_CS_OK = 0,
	IMPULSE_CS_FAIL_QUALITY,  /* below the quality floor */
	IMPULSE_CS_FAIL_PROCEDURE,/* the CS procedure itself failed */
	IMPULSE_CS_FAIL_CONNECT,  /* could not establish/reuse the ACL link */
	IMPULSE_CS_FAIL_DISAGREE, /* PBR and CS-RTT disagreed beyond tolerance */
};

struct impulse_cs_measurement {
	enum impulse_cs_result result;
	uint32_t distance_cm;   /* valid only when result == OK */
	uint8_t quality;        /* backend-defined scale */
	uint32_t pbr_cm;        /* primary estimator */
	uint32_t rtt_cm;        /* sanity check; speed-of-light bounded */
};

/* Per-anchor verdict state (§4.4). */
struct impulse_prox_state {
	uint8_t anchor_id[IMPULSE_UUID_LEN];
	enum impulse_prox_verdict verdict;
	uint8_t near_run;      /* consecutive measurements under NEAR_ENTER */
	uint8_t away_run;      /* consecutive measurements over AWAY_ENTER */
	uint8_t abstain_run;   /* consecutive abstentions */
	bool have_verdict;     /* false until the first confident measurement */
	uint16_t near_enter_cm;
	uint16_t away_enter_cm;
};

void impulse_prox_state_init(struct impulse_prox_state *st,
			     const uint8_t *anchor_id,
			     uint16_t near_enter_cm, uint16_t away_enter_cm);

/*
 * Fold one measurement into the state machine.
 *
 * ABSTENTION: below the quality floor, or on a failed procedure, the engine
 * ABSTAINS. It does not return a distance and it does not guess. An abstention
 * means "no new evidence" — the verdict holds and the dwell counters do NOT
 * advance. Do not "fix" an abstention by making it return a number.
 *
 * A connect failure is ONE abstention, never an AWAY measurement. The ESP32
 * build tried treating it as evidence of distance and it produced a real
 * defect: a stationary watch just out of range re-failed its connect every
 * poll and marched itself to saturation on one observation repeated.
 */
void impulse_prox_ingest(struct impulse_prox_state *st,
			 const struct impulse_cs_measurement *m);

/*
 * Resolve the criterion. `criterion` is one of IMPULSE_CRIT_*.
 *
 * Returns true when the commitment is SATISFIED. On sustained inability to
 * measure (abstain_run past the cap) this applies the criterion-dependent
 * fail-safe from v2 §5.4.1: resolve toward NOT MET for stayNear and toward MET
 * for getAway. In both directions a sustained inability to measure is treated
 * as the user's problem rather than the device's, because the alternative
 * rewards jamming.
 */
bool impulse_prox_criterion_met(const struct impulse_prox_state *st,
				uint8_t criterion);

/* True once the abstain run has exceeded the cap — i.e. the fail-safe is in
 * force rather than a real verdict. Surfaced so the UI can say so. */
bool impulse_prox_in_failsafe(const struct impulse_prox_state *st);

/* --- backend seam --------------------------------------------------------
 *
 * Everything below is what a real channel-sounding implementation must
 * provide. The stub returns IMPULSE_CS_FAIL_PROCEDURE for every request, which
 * drives the abstention path — deliberately, so that running this firmware
 * without a backend fails CLOSED for stayNear rather than silently reporting
 * compliance.
 */
struct impulse_cs_backend {
	const char *name;
	/* Run one CS procedure against `anchor_id`. Blocking. */
	void (*measure)(const uint8_t *anchor_id,
			struct impulse_cs_measurement *out);
	/* Anchor role: begin/stop acting as a reflector. */
	int (*reflector_start)(void);
	int (*reflector_stop)(void);
};

const struct impulse_cs_backend *impulse_cs_backend(void);

/* Start the real nRF channel-sounding engine and install it as the backend.
 * Only available when CONFIG_IMPULSE_CS_BACKEND is enabled; without it the
 * stub stays in place and every measurement abstains, which fails CLOSED for
 * stayNear (§4.5). */
int impulse_cs_backend_start(void);

/*
 * Turn CS procedures on/off. Ranging at ~10 Hz is the largest avoidable draw
 * in the design, so it runs only while a commitment is actually being
 * enforced. The link stays connected either way. Safe to call every loop pass:
 * it is a no-op unless the state changes.
 */
void impulse_cs_set_ranging(bool on);
void impulse_cs_backend_set(const struct impulse_cs_backend *backend);

#endif /* IMPULSE_PROXIMITY_H_ */
