/* SPDX-License-Identifier: Apache-2.0 */
#include "cs_fuse.h"

#include <string.h>

void impulse_cs_burst_init(struct impulse_cs_burst *b)
{
	memset(b, 0, sizeof(*b));
}

static uint32_t expected_cm(uint32_t ifft_cm, uint32_t slope_q8,
			    uint32_t offset_cm)
{
	return ((ifft_cm * slope_q8) >> 8) + offset_cm;
}

static uint32_t abs_diff(uint32_t a, uint32_t b)
{
	return (a > b) ? (a - b) : (b - a);
}

bool impulse_cs_burst_add(struct impulse_cs_burst *b,
			  const struct impulse_cs_raw *raw)
{
	if (b == NULL || raw == NULL) {
		return false;
	}

	if (!raw->has_ifft) {
		/* No primary estimate. phase_slope alone is not a substitute:
		 * it is the estimator that multipath biases, which is exactly
		 * the condition under which ifft tends to be missing. */
		if (b->missing_ifft < UINT8_MAX) {
			b->missing_ifft++;
		}
		return false;
	}

	if (raw->ifft_cm < IMPULSE_CS_MIN_VALID_CM ||
	    raw->ifft_cm > IMPULSE_CS_MAX_VALID_CM) {
		if (b->rejected_range < UINT8_MAX) {
			b->rejected_range++;
		}
		return false;
	}

	/*
	 * Quality gate: how far is phase_slope from where it should be, given
	 * ifft? They track each other closely in clean line-of-sight and
	 * separate when multipath dominates, because reflections lengthen
	 * phase_slope while ifft can still resolve the direct path. A large
	 * divergence therefore means "the direct path is probably blocked",
	 * which is a better quality signal than any threshold on ifft alone.
	 */
	if (raw->has_phase_slope) {
		uint32_t want = expected_cm(raw->ifft_cm,
					    IMPULSE_CS_PS_SLOPE_Q8,
					    IMPULSE_CS_PS_OFFSET_CM);

		if (abs_diff(raw->phase_slope_cm, want) >
		    IMPULSE_CS_PS_DIVERGENCE_MAX_CM) {
			if (b->rejected_quality < UINT8_MAX) {
				b->rejected_quality++;
			}
			return false;
		}
	}

	/*
	 * Relay check. A repeater can only ADD propagation delay, so it
	 * inflates rtt while PBR can be manipulated to look near. rtt running
	 * far ABOVE its expected value is the signature; rtt below expectation
	 * is just noise and is ignored, because nothing can beat the speed of
	 * light and a short reading cannot fabricate nearness.
	 */
	if (raw->has_rtt) {
		uint32_t want = expected_cm(raw->ifft_cm,
					    IMPULSE_CS_RTT_SLOPE_Q8,
					    IMPULSE_CS_RTT_OFFSET_CM);

		if (raw->rtt_cm > want + IMPULSE_CS_RELAY_MARGIN_CM) {
			if (b->suspect_relay < UINT8_MAX) {
				b->suspect_relay++;
			}
			return false;
		}
	}

	if (b->count < IMPULSE_CS_BURST_MAX) {
		b->ifft_cm[b->count++] = raw->ifft_cm;
	}
	return true;
}

static uint32_t median_of(const uint32_t *src, uint8_t n)
{
	uint32_t tmp[IMPULSE_CS_BURST_MAX];

	memcpy(tmp, src, n * sizeof(tmp[0]));

	/* Insertion sort: n <= 24 and bounded, so this is both fast enough and
	 * free of the surprises a partial-selection algorithm brings. */
	for (uint8_t i = 1; i < n; i++) {
		uint32_t key = tmp[i];
		int j = (int)i - 1;

		while (j >= 0 && tmp[j] > key) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = key;
	}

	if ((n & 1U) != 0U) {
		return tmp[n / 2];
	}
	return (tmp[n / 2 - 1] + tmp[n / 2]) / 2U;
}

void impulse_cs_burst_finish(const struct impulse_cs_burst *b,
			     struct impulse_cs_measurement *out)
{
	memset(out, 0, sizeof(*out));

	if (b == NULL) {
		out->result = IMPULSE_CS_FAIL_PROCEDURE;
		return;
	}

	/*
	 * A relay suspicion outranks everything, including a burst that
	 * otherwise looks healthy. Reporting a distance here would be reporting
	 * the attacker's chosen answer.
	 */
	if (b->suspect_relay > 0U && b->suspect_relay >= b->count) {
		out->result = IMPULSE_CS_FAIL_DISAGREE;
		return;
	}

	if (b->count < IMPULSE_CS_BURST_MIN_ACCEPTED) {
		/* Distinguish "the radio could not measure" from "the
		 * measurements were rejected as poor quality": both abstain,
		 * but they mean different things in a log. */
		out->result = (b->rejected_quality > b->count)
				      ? IMPULSE_CS_FAIL_QUALITY
				      : IMPULSE_CS_FAIL_PROCEDURE;
		return;
	}

	uint32_t raw = median_of(b->ifft_cm, b->count);

	out->result = IMPULSE_CS_OK;
	out->pbr_cm = raw;

	/*
	 * Convert to TRUE distance before handing it on. The §4.4 thresholds
	 * (NEAR_ENTER_CM 200, AWAY_ENTER_CM 350) are real-world distances, so
	 * comparing a raw estimator against them would be wrong by the ~0.97 m
	 * offset — enough to turn "at the desk" into "across the room".
	 */
	if (raw <= IMPULSE_CS_NEAR_FIELD_CM) {
		out->distance_cm = 0; /* nearer than the model can express */
	} else if (raw <= IMPULSE_CS_IFFT_OFFSET_CM) {
		out->distance_cm = 0;
	} else {
		out->distance_cm = ((raw - IMPULSE_CS_IFFT_OFFSET_CM) << 8) /
				   IMPULSE_CS_IFFT_SLOPE_Q8;
	}

	out->quality = b->count; /* accepted samples == confidence, for now */
}
