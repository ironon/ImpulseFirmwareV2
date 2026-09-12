/*
 * Channel Sounding estimator fusion — turning three raw estimators into one
 * distance plus a quality verdict.
 *
 * Pure logic, no Zephyr, no radio: host-testable, which matters because this
 * is where the judgement calls live. Every constant below came from a real
 * capture (nrf_cs_test/RESULTS.md, 2026-08-30) rather than from reasoning, but
 * NONE of them is calibrated against ground truth yet — see the warning on
 * IMPULSE_CS_*_SLOPE_Q8.
 *
 * What the measurements established, and why the design looks like this:
 *
 *   ifft         noise sd 0.25-0.29 m, best of the three, and least disturbed
 *                by occlusion. It inverse-transforms the channel response to a
 *                delay profile and can pick the EARLIEST arrival, so a
 *                reflection adds a separate peak instead of moving the answer.
 *                => PRIMARY distance estimate.
 *
 *   phase_slope  noise sd 0.34-0.68 m. Fits a slope across the whole frequency
 *                response, so every multipath component drags it, and
 *                reflections are always LONGER than the direct path. It
 *                therefore reads high exactly when the direct path is
 *                obstructed. => not a distance source; its DIVERGENCE from
 *                ifft is a multipath/NLOS quality signal.
 *
 *   rtt          noise sd 0.29-0.81 m, worst, and rotation-sensitive. But it
 *                is bounded by the speed of light, which no amount of phase
 *                trickery is. => SECURITY cross-check for relay attacks, the
 *                one thing PBR alone cannot detect (spec v3 §4.6).
 *
 * Fusing all three as an average was measured and rejected: it buys only
 * 0-26% noise reduction because the three share one channel estimate and their
 * residuals are 0.29-0.53 correlated. Averaging ifft over time is also weaker
 * than it looks — the noise is time-correlated multipath drift, so 40 samples
 * cut it by 46% where white noise would give 84%.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMPULSE_CS_FUSE_H_
#define IMPULSE_CS_FUSE_H_

#include <stdbool.h>
#include <stdint.h>

#include "proximity.h"

/* One CS procedure's raw output, centimetres. A field is only meaningful if
 * its `has_` flag is set — the SDK may not produce all three every time. */
struct impulse_cs_raw {
	uint32_t ifft_cm;
	uint32_t phase_slope_cm;
	uint32_t rtt_cm;
	bool has_ifft;
	bool has_phase_slope;
	bool has_rtt;
};

/*
 * DISTANCE CALIBRATION — from the line-of-sight ground-truth sweep,
 * 2026-08-30 (`nrf_cs_test/distance_test_30s_LOS.csv`), 11 stations from
 * touching to 7.0 m, 30 s each, board rotated continuously.
 *
 *     ifft = 0.993 * true + 0.97 m      R² = 0.9881, rmse 0.20 m
 *
 * **The slope is 1.0 to within 0.7%.** `ifft` is correctly scaled and needs
 * only a CONSTANT offset removed — the outcome that was hoped for and could
 * not be assumed. After calibration the residual is 0.20 m sd with a worst
 * case of 0.39 m across the whole range.
 *
 * The earlier "slope 1.45 between estimators" figure came from walk captures
 * that contained occlusion, which inflates phase_slope and rtt relative to
 * ifft. It described occluded conditions, not the instrument. Superseded.
 */
#define IMPULSE_CS_IFFT_SLOPE_Q8  254 /* 0.993 */
/*
 * PROVISIONAL, SINGLE POINT, BM20C<->BM20C. Measured 2026-09-10: two Board V1s
 * 2 ft (61 cm) apart line-of-sight read ifft 1.32-1.36 m over 150+ samples
 * (sd 0.02-0.04 m), giving 0.993 * 134 - 61 ~= 72.
 *
 * WAS 97, which came from Board V1 against a DEV KIT. Antenna delay dominates
 * this constant, so a DK-derived offset does not transfer to a BM20C pair —
 * and 97 errs in the UNSAFE direction: it reports the watch ~25 cm CLOSER than
 * it is, so a stayNear commitment reads as satisfied slightly too far out.
 *
 * THE SLOPE IS STILL THE DK'S, AND IS UNVERIFIED FOR THIS PAIR. One point
 * cannot separate slope from offset. A proper multi-station sweep is still
 * owed — see docs/hardware-notes/cs_tuning_plan.md.
 */
#define IMPULSE_CS_IFFT_OFFSET_CM 72

/*
 * Below this the boards are inside the near field — a 2.4 GHz wavelength is
 * 12.5 cm — and phase ranging stops being meaningful. Measured at contact:
 * ifft read 0.55 m where the fitted line predicts 0.97 m, with an
 * implausibly tiny 0.01 m sd, which looks like a floor rather than a
 * measurement. Readings here are clamped to zero distance rather than
 * rejected: "closer than the model can express" is still NEAR, and rejecting
 * it would abstain at exactly the moment the answer is least in doubt.
 */
#define IMPULSE_CS_NEAR_FIELD_CM 55

/*
 * Cross-estimator relationships in LINE OF SIGHT, Q8 fixed point.
 * Derived from the same sweep:
 *     phase_slope ~ 0.92 * ifft + 1.97 m
 *     rtt         ~ 1.04 * ifft + 2.78 m
 *
 * These are what the quality and relay checks compare against. Occlusion
 * pushes both estimators ABOVE these lines — that is precisely what makes the
 * divergence a usable blocked-path detector.
 */
#define IMPULSE_CS_PS_SLOPE_Q8   234 /* 0.916 */
#define IMPULSE_CS_PS_OFFSET_CM  197
#define IMPULSE_CS_RTT_SLOPE_Q8  266 /* 1.037 */
#define IMPULSE_CS_RTT_OFFSET_CM 278

/* How far phase_slope may sit from its expected value before the measurement
 * is treated as too multipath-corrupted to trust. Generous: the point is to
 * catch a blocked direct path, not to reject ordinary noise. */
#define IMPULSE_CS_PS_DIVERGENCE_MAX_CM 250

/*
 * Relay detection. A relay/repeater cannot make light faster, so it can only
 * ADD delay: rtt inflates while PBR can be made to look near. An rtt far above
 * its expected value is therefore the signature of the one attack PBR cannot
 * see. Deliberately loose — rtt's own noise reaches 0.8 m sd — because a false
 * positive here abstains, and abstention is not free (spec §4.5).
 */
#define IMPULSE_CS_RELAY_MARGIN_CM 400

/* An ifft of exactly zero appeared twice in ~10k samples; it is not a real
 * distance. Anything below this is discarded rather than believed. */
#define IMPULSE_CS_MIN_VALID_CM 5
#define IMPULSE_CS_MAX_VALID_CM 3000

/*
 * A burst of procedures, reduced to one measurement.
 *
 * The enforcement poll runs every 60-180 s (§4.3) while CS itself samples at
 * ~9 Hz, so a poll can afford a short burst and take the MEDIAN. Median rather
 * than mean on purpose: the captures contained occasional wild values (ifft up
 * to 8.12 m, and zeros) that a mean would smear into the answer.
 */
#define IMPULSE_CS_BURST_MAX 24

struct impulse_cs_burst {
	uint32_t ifft_cm[IMPULSE_CS_BURST_MAX];
	uint8_t count;
	uint8_t rejected_range;   /* outside MIN/MAX_VALID */
	uint8_t rejected_quality; /* phase_slope divergence */
	uint8_t suspect_relay;    /* rtt implausibly long vs ifft */
	uint8_t missing_ifft;     /* procedure produced no primary estimate */
};

void impulse_cs_burst_init(struct impulse_cs_burst *b);

/* Fold one procedure's raw estimators into the burst. Returns true if the
 * sample was accepted as a distance. */
bool impulse_cs_burst_add(struct impulse_cs_burst *b,
			  const struct impulse_cs_raw *raw);

/*
 * Reduce the burst to a measurement the §4.4 state machine can ingest.
 *
 * Fails (abstains) rather than guessing when too few samples survived, or when
 * a relay is suspected. Abstention is the correct outcome for both — see §4.5,
 * and note that a relay suspicion must NEVER resolve to a near verdict.
 */
void impulse_cs_burst_finish(const struct impulse_cs_burst *b,
			     struct impulse_cs_measurement *out);

/* Minimum accepted samples before a burst can yield a distance. */
/*
 * Sized from the sweep. Error sd of a burst median, sampled from the real
 * per-station data:
 *     N=5   0.28-0.82 m
 *     N=10  0.19-0.57 m
 *     N=20  0.07-0.33 m
 * The NEAR/AWAY band is 1.5 m wide (200 to 350 cm), so N=10 is comfortably
 * inside it while N=5 is not. Cost is time, because the measurement RATE
 * falls with distance: ~9/s at 1 m, ~5.7/s at 2 m, ~2.5/s at 3.4 m, ~1.7/s at
 * 7 m. N=10 therefore takes about 2 s near the NEAR threshold and about 6 s
 * out at the edge of usable range.
 */
#define IMPULSE_CS_BURST_MIN_ACCEPTED 10

#endif /* IMPULSE_CS_FUSE_H_ */
