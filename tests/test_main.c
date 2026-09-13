/*
 * Host tests for the pure-logic core.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "enforcement/enforcement.h"
#include "enforcement/profiles.h"
#include "integrity/classify.h"
#include "integrity/integrity.h"
#include "proximity/cs_fuse.h"
#include "proximity/proximity.h"
#include "schedule/schedule.h"
#include "schedule/schedule_blob.h"

static int g_fail;
static int g_run;

#define CHECK(cond, msg)                                                       \
	do {                                                                   \
		g_run++;                                                       \
		if (!(cond)) {                                                 \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); \
			g_fail++;                                              \
		}                                                              \
	} while (0)

static void uuid_fill(uint8_t *u, uint8_t seed)
{
	for (int i = 0; i < IMPULSE_UUID_LEN; i++) {
		u[i] = (uint8_t)(seed + i);
	}
}

static struct impulse_event mk_event(uint8_t seed, uint16_t start,
				     uint16_t end, uint8_t recur)
{
	struct impulse_event e;

	memset(&e, 0, sizeof(e));
	uuid_fill(e.id, seed);
	e.start_time = start;
	e.end_time = end;
	e.recurrence = recur;
	e.criteria = IMPULSE_CRIT_STAY_NEAR;
	e.profile = IMPULSE_PROFILE_NORMAL_BOTH;
	e.anchor_profile = IMPULSE_ANCHOR_PROFILE_NONE;
	e.has_anchor_id = true;
	uuid_fill(e.anchor_id, 0x40);
	/* 2026-08-30 12:00Z — a Sunday, which several recurrence tests use. */
	e.reference_date = 1788091200LL;
	return e;
}

/* ---- civil date ---------------------------------------------------------- */
static void test_dates(void)
{
	printf("civil date\n");

	struct impulse_date d;

	impulse_civil_from_days(0, &d);
	CHECK(d.year == 1970 && d.month == 1 && d.day == 1, "epoch is 1970-01-01");

	/* 1970-01-01 was a Thursday => 4 with Monday==1. */
	CHECK(impulse_day_of_week(&d) == 4, "epoch is a Thursday");

	d.year = 2026; d.month = 8; d.day = 30;
	CHECK(impulse_day_of_week(&d) == 7, "2026-08-30 is a Sunday");

	d.year = 2024; d.month = 2; d.day = 29;
	int64_t leap = impulse_days_from_civil(2024, 2, 29);
	impulse_civil_from_days(leap, &d);
	CHECK(d.year == 2024 && d.month == 2 && d.day == 29, "leap day round-trips");

	/* A negative UTC offset must not move the local date backwards past
	 * midnight incorrectly — this is the bug that shifts an event by a day
	 * for anyone west of UTC. */
	uint16_t minute;
	impulse_local_from_utc(1788091200LL, -300, &d, &minute);
	CHECK(d.year == 2026 && d.month == 8 && d.day == 30, "UTC-5 keeps the date");
	CHECK(minute == 7 * 60, "UTC-5 of 12:00Z is 07:00 local");
}

/* ---- blob round-trip ----------------------------------------------------- */
static void test_blob(void)
{
	printf("schedule blob\n");

	struct impulse_schedule s;
	uint8_t buf[2048];

	memset(&s, 0, sizeof(s));
	s.count = 2;
	s.events[0] = mk_event(1, 360, 420, IMPULSE_RECUR_DAILY);
	s.events[1] = mk_event(2, 1020, 1080, IMPULSE_RECUR_WEEKLY);
	s.events[1].day_of_week = 3;
	s.events[1].criteria = IMPULSE_CRIT_GET_ON_WIFI;
	s.events[1].has_anchor_id = false;
	memset(s.events[1].anchor_id, 0, IMPULSE_UUID_LEN);
	s.events[1].ssid_len = 4;
	memcpy(s.events[1].wifi_ssid, "gym\0", 4);
	s.events[1].beep_anchor_count = 1;
	uuid_fill(s.events[1].beep_anchors[0], 0x70);
	s.events[1].anchor_profile = IMPULSE_ANCHOR_PROFILE_MEDIUM;

	size_t n = impulse_blob_serialize(&s, buf, sizeof(buf));

	CHECK(n > 0, "serialize succeeds");
	CHECK(buf[0] == IMPULSE_SCHEDULE_FORMAT_VERSION, "version byte is 0x02");

	struct impulse_schedule back;
	enum impulse_blob_result r = impulse_blob_parse(buf, n, &back);

	CHECK(r == IMPULSE_BLOB_OK, "parse succeeds");
	CHECK(back.count == 2, "event count round-trips");
	CHECK(impulse_event_equal(&s.events[0], &back.events[0]), "event 0 round-trips");
	CHECK(impulse_event_equal(&s.events[1], &back.events[1]), "event 1 round-trips");

	/* Wire-layout guard: 3-byte header, then a fixed 55-byte prefix per
	 * event before the variable SSID/beep tail. If this changes, all four
	 * declarations of the contract have to move together. */
	CHECK(n == 3 + (16+8+2+2+7+2+1+16+1+0+1) + (16+8+2+2+7+2+1+16+1+4+1+16),
	      "blob length matches the documented field layout");

	/* An unknown version must be rejected, not reinterpreted. */
	buf[0] = 0x03;
	CHECK(impulse_blob_parse(buf, n, &back) == IMPULSE_BLOB_ERR_VERSION,
	      "unknown version rejected");
	buf[0] = IMPULSE_SCHEDULE_FORMAT_VERSION;

	/* Truncation must fail cleanly rather than read past the end. */
	CHECK(impulse_blob_parse(buf, n - 5, &back) == IMPULSE_BLOB_ERR_TRUNCATED,
	      "truncated blob rejected");

	/* A blob claiming more events than fit is rejected, never truncated —
	 * a silently shortened schedule is a silently weakened commitment. */
	uint8_t hdr[3] = {IMPULSE_SCHEDULE_FORMAT_VERSION, 0xFF, 0xFF};
	CHECK(impulse_blob_parse(hdr, sizeof(hdr), &back) == IMPULSE_BLOB_ERR_TOO_MANY,
	      "over-long event count rejected");

	/* CRC32 must agree with zlib.crc32, which the app and phone_sim use. */
	CHECK(impulse_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926U,
	      "CRC32 matches the standard check vector");
}

/* ---- recalculate_day ----------------------------------------------------- */
static void test_recalc(void)
{
	printf("recalculate_day\n");

	struct impulse_schedule s;
	struct impulse_day_plan plan;
	struct impulse_date sunday = {2026, 8, 30};

	memset(&s, 0, sizeof(s));

	/* daily 06:00-07:00, weekly-Sunday 17:00-18:00, weekly-Monday (skipped) */
	s.events[0] = mk_event(1, 360, 420, IMPULSE_RECUR_DAILY);
	s.events[1] = mk_event(2, 1020, 1080, IMPULSE_RECUR_WEEKLY);
	s.events[1].day_of_week = 7;
	s.events[2] = mk_event(3, 600, 660, IMPULSE_RECUR_WEEKLY);
	s.events[2].day_of_week = 1;
	s.count = 3;

	impulse_recalculate_day(&s, &sunday, 0, &plan);
	CHECK(plan.count == 2, "only the daily and the Sunday weekly apply");
	CHECK(plan.events[0]->start_time == 360, "sorted by start_time");
	CHECK(plan.events[1]->start_time == 1020, "sorted by start_time (2)");

	/* Active-event lookup is half-open [start, end). */
	CHECK(impulse_active_event(&plan, 360) == plan.events[0], "start minute is inside");
	CHECK(impulse_active_event(&plan, 419) == plan.events[0], "last minute is inside");
	CHECK(impulse_active_event(&plan, 420) == NULL, "end minute is outside");

	/* §5.3.4 negate: a one-time negate with a matching UUID cancels the
	 * recurring event for that day, and removes itself too. */
	struct impulse_event neg = mk_event(1, 360, 420, IMPULSE_RECUR_ONCE);
	neg.negate = true;
	neg.reference_date = 1788091200LL; /* 2026-08-30 */
	s.events[3] = neg;
	s.count = 4;

	impulse_recalculate_day(&s, &sunday, 0, &plan);
	CHECK(plan.count == 1, "negate removes both the recurrence and itself");
	CHECK(plan.events[0]->start_time == 1020, "the surviving event is the weekly");

	/* The negate is scoped to its one day. */
	struct impulse_date monday = {2026, 8, 31};
	impulse_recalculate_day(&s, &monday, 0, &plan);
	CHECK(plan.count == 2, "next day is unaffected by the negate");
}

/* ---- §9.1 classification ------------------------------------------------- */
static void test_classify(void)
{
	printf("tighten/loosen classification\n");

	struct impulse_event a = mk_event(1, 540, 600, IMPULSE_RECUR_DAILY);
	struct impulse_event b = a;

	CHECK(impulse_classify_modified(&a, &b, 0) == IMPULSE_CHANGE_NONE,
	      "identical events are no change");

	/* Window: a longer window binds harder. */
	b = a; b.start_time = 480; /* starts earlier */
	CHECK(impulse_classify_modified(&a, &b, 0) == IMPULSE_CHANGE_TIGHTEN,
	      "widening the window is a tightening");

	b = a; b.start_time = 550; /* starts later => shorter */
	CHECK(impulse_classify_modified(&a, &b, 0) == IMPULSE_CHANGE_LOOSEN,
	      "shortening the window is a loosening");

	b = a; b.start_time = 550; b.end_time = 610; /* both shift later */
	CHECK(impulse_classify_modified(&a, &b, 0) == IMPULSE_CHANGE_LOOSEN,
	      "a shifted window is non-comparable and therefore not instant");

	/* Criteria and target changes are never instant. */
	b = a; b.criteria = IMPULSE_CRIT_GET_AWAY;
	CHECK(impulse_classify_modified(&a, &b, 0) == IMPULSE_CHANGE_LOOSEN,
	      "a criteria change is non-comparable");

	b = a; uuid_fill(b.anchor_id, 0x99);
	CHECK(impulse_classify_modified(&a, &b, 0) == IMPULSE_CHANGE_LOOSEN,
	      "an anchor change is non-comparable");

	/* Profile partial order. */
	CHECK(impulse_rel_profile(IMPULSE_PROFILE_NORMAL_BOTH,
				  IMPULSE_PROFILE_STRICT_BOTH) == IMPULSE_REL_SUPERSET,
	      "normal->strict at same output is tighter");
	CHECK(impulse_rel_profile(IMPULSE_PROFILE_STRICT_BOTH,
				  IMPULSE_PROFILE_STRICT_BUZZ) == IMPULSE_REL_SUBSET,
	      "both->buzz is weaker output");
	CHECK(impulse_rel_profile(IMPULSE_PROFILE_STRICT_BUZZ,
				  IMPULSE_PROFILE_STRICT_SILENT) == IMPULSE_REL_INCOMPARABLE,
	      "buzz vs silent is non-comparable");
	CHECK(impulse_rel_profile(IMPULSE_PROFILE_LOOSE_BUZZ,
				  IMPULSE_PROFILE_STRICT_SILENT) == IMPULSE_REL_INCOMPARABLE,
	      "tighter strictness but sideways output is non-comparable");

	/* Recurrence sets. */
	struct impulse_event w = mk_event(1, 540, 600, IMPULSE_RECUR_WEEKLY);
	w.day_of_week = 2;
	struct impulse_event dly = w; dly.recurrence = IMPULSE_RECUR_DAILY; dly.day_of_week = 0;
	CHECK(impulse_rel_recurrence(&w, &dly, 0) == IMPULSE_REL_SUPERSET,
	      "weekly->daily covers strictly more days");
	CHECK(impulse_rel_recurrence(&dly, &w, 0) == IMPULSE_REL_SUBSET,
	      "daily->weekly covers fewer");
	struct impulse_event w2 = w; w2.day_of_week = 4;
	CHECK(impulse_rel_recurrence(&w, &w2, 0) == IMPULSE_REL_INCOMPARABLE,
	      "weekly Tue->Thu is a different set, not a bigger one");

	/* donningGrace: shorter binds harder. */
	b = a; b.donning_grace_s = 60; a.donning_grace_s = 300;
	CHECK(impulse_classify_modified(&a, &b, 0) == IMPULSE_CHANGE_TIGHTEN,
	      "shortening donning grace is a tightening");
	a.donning_grace_s = 0;

	/* Adding an event is a tightening; adding a NEGATE event is not. */
	struct impulse_event add = mk_event(9, 540, 600, IMPULSE_RECUR_DAILY);
	CHECK(impulse_classify_added(&add) == IMPULSE_CHANGE_TIGHTEN,
	      "adding an event is a tightening");
	add.negate = true;
	CHECK(impulse_classify_added(&add) == IMPULSE_CHANGE_LOOSEN,
	      "adding a negate cancels a day and is a loosening");

	CHECK(impulse_classify_deleted(&a) == IMPULSE_CHANGE_LOOSEN,
	      "deleting an event is a loosening");

	/* Multi-field: one loosening field poisons an otherwise tightening edit. */
	b = a;
	b.start_time = 480;                    /* tightening */
	b.criteria = IMPULSE_CRIT_GET_AWAY;    /* non-comparable */
	CHECK(impulse_classify_modified(&a, &b, 0) == IMPULSE_CHANGE_LOOSEN,
	      "a single non-comparable field makes the whole edit not instant");
}

/* ---- §9.3 / §9.4 gate and promotion -------------------------------------- */
static void test_integrity(void)
{
	printf("commitment integrity gate\n");

	struct impulse_integrity ig;
	struct impulse_schedule cur, prop;
	struct impulse_diff_report rep;
	const int64_t now = 1788091200LL; /* 2026-08-30 12:00Z */

	impulse_integrity_init(&ig);
	impulse_integrity_set_elapsed_base(1000);

	memset(&cur, 0, sizeof(cur));
	cur.events[0] = mk_event(1, 540, 600, IMPULSE_RECUR_DAILY);
	cur.count = 1;

	/* An event already in the schedule with no settle record is SEALED
	 * rather than granted a free edit window — see seal_unknown_events().
	 * That is what makes the loosening below quarantine rather than apply. */
	/* A loosening of an event happening TOMORROW is quarantined, not applied.
	 * (Same-day, so inside the 24 h free horizon.) */
	prop = cur;
	prop.events[0].start_time = 570; /* shorter window => loosening */
	uint8_t verdict = impulse_integrity_apply_push(&ig, &cur, &prop, NULL,
						       now, 0, &rep);

	CHECK(verdict == IMPULSE_END_QUARANTINED, "loosening returns 0x03");
	CHECK(rep.quarantined == 1, "one entry quarantined");
	CHECK(cur.events[0].start_time == 540, "current schedule is unchanged");

	/* It must NOT promote early... */
	CHECK(impulse_integrity_promote(&ig, &cur, now, 0) == 0,
	      "no promotion before the delay elapses");
	CHECK(cur.events[0].start_time == 540, "still unchanged before promotion");

	/* ...and must promote once 24 h of ELAPSED time has passed. */
	impulse_integrity_set_elapsed_base(1000 + 24 * 3600 + 1);
	CHECK(impulse_integrity_promote(&ig, &cur, now, 0) == 1,
	      "promotes after 24 h elapsed");
	CHECK(cur.events[0].start_time == 570, "promotion applied the change");

	/* A tightening applies immediately. */
	prop = cur;
	prop.events[0].start_time = 480;
	verdict = impulse_integrity_apply_push(&ig, &cur, &prop, NULL, now, 0, &rep);
	CHECK(verdict == IMPULSE_END_ACCEPTED, "tightening returns 0x01");
	CHECK(cur.events[0].start_time == 480, "tightening applied at once");

	/* The active-event guard: a push that loosens the CURRENTLY ACTIVE
	 * event is rejected outright. This is the check that closes the
	 * "push an empty schedule at 6am" bypass. */
	struct impulse_event active = cur.events[0];
	struct impulse_schedule empty;

	memset(&empty, 0, sizeof(empty));
	verdict = impulse_integrity_apply_push(&ig, &cur, &empty, &active, now, 0, &rep);
	CHECK(verdict == IMPULSE_END_REJECTED, "empty push against an active event is 0x04");
	CHECK(rep.rejected_active_event, "report says why");
	CHECK(cur.count == 1, "schedule untouched by a rejected push");
}

/* ---- §5.4.3 profiles ----------------------------------------------------- */
static void test_profiles(void)
{
	printf("enforcement profiles\n");

	const struct impulse_profile_def *def =
		impulse_profile_get(IMPULSE_PROFILE_NORMAL_SILENT);

	CHECK(def != NULL, "normal_silent exists");
	CHECK(def->loops, "normal_silent loops");
	CHECK(def->floor_interval_ms == 5000U, "normal floors at 5 s");

	/* The wait step shrinks 2 s per cycle and clamps at the floor; the
	 * output step keeps its length. */
	CHECK(impulse_profile_step_duration(def, 1, 0) == 30000U, "cycle 1 waits 30 s");
	CHECK(impulse_profile_step_duration(def, 1, 1) == 28000U, "cycle 2 waits 28 s");
	CHECK(impulse_profile_step_duration(def, 1, 13) == 5000U, "clamps to the floor");
	CHECK(impulse_profile_step_duration(def, 1, 99) == 5000U, "stays at the floor");
	CHECK(impulse_profile_step_duration(def, 0, 50) == 2000U, "the buzz step never shrinks");

	const struct impulse_profile_def *loose =
		impulse_profile_get(IMPULSE_PROFILE_LOOSE_BUZZ);
	CHECK(loose->floor_interval_ms == 10000U, "loose floors at 10 s");

	/* Continuous profiles never stop on their own. */
	struct impulse_profile_run run;

	impulse_profile_start(&run, IMPULSE_PROFILE_STRICT_BOTH);
	CHECK(run.motor_on && run.buzzer_on, "strict_both starts with both on");
	for (int i = 0; i < 1000; i++) {
		(void)impulse_profile_tick(&run, 100);
	}
	CHECK(run.motor_on && run.buzzer_on && !run.finished,
	      "a continuous profile runs until the condition is met");

	/* A looping profile alternates. */
	impulse_profile_start(&run, IMPULSE_PROFILE_NORMAL_BOTH);
	CHECK(run.motor_on, "starts in the output step");
	(void)impulse_profile_tick(&run, 2000);
	CHECK(!run.motor_on && !run.buzzer_on, "moves into the wait step");
}

/* ---- §4.4 / §4.5 proximity ----------------------------------------------- */
static void test_proximity(void)
{
	printf("proximity verdict machine\n");

	struct impulse_prox_state st;
	struct impulse_cs_measurement m = {0};

	impulse_prox_state_init(&st, NULL, 0, 0);
	CHECK(!st.have_verdict, "no verdict before any measurement");
	CHECK(!impulse_prox_criterion_met(&st, IMPULSE_CRIT_STAY_NEAR),
	      "stayNear fails closed with no measurement");
	CHECK(impulse_prox_criterion_met(&st, IMPULSE_CRIT_GET_AWAY),
	      "getAway fails open with no measurement");

	/* NEAR needs NEAR_DWELL consecutive close readings. */
	m.result = IMPULSE_CS_OK;
	m.distance_cm = 100;
	impulse_prox_ingest(&st, &m);
	CHECK(!st.have_verdict, "one close reading is not enough");
	impulse_prox_ingest(&st, &m);
	CHECK(st.verdict == IMPULSE_PROX_NEAR, "two close readings reach NEAR");
	CHECK(impulse_prox_criterion_met(&st, IMPULSE_CRIT_STAY_NEAR), "stayNear satisfied");

	/* AWAY deliberately needs DOUBLE the evidence — a threat-model
	 * invariant, because attenuation can fabricate "far" but never "near". */
	m.distance_cm = 500;
	impulse_prox_ingest(&st, &m);
	impulse_prox_ingest(&st, &m);
	CHECK(st.verdict == IMPULSE_PROX_NEAR, "two far readings do NOT flip to AWAY");
	impulse_prox_ingest(&st, &m);
	impulse_prox_ingest(&st, &m);
	CHECK(st.verdict == IMPULSE_PROX_AWAY, "four far readings do flip");
	CHECK(IMPULSE_AWAY_DWELL == 2 * IMPULSE_NEAR_DWELL,
	      "the 2:1 dwell ratio is a floor, not a tunable");

	/* Hysteresis band: a reading between the thresholds is real evidence
	 * but argues for no transition, and resets both runs. */
	impulse_prox_state_init(&st, NULL, 0, 0);
	m.distance_cm = 100;
	impulse_prox_ingest(&st, &m);
	m.distance_cm = 275; /* inside 200..350 */
	impulse_prox_ingest(&st, &m);
	m.distance_cm = 100;
	impulse_prox_ingest(&st, &m);
	CHECK(!st.have_verdict, "the band interrupts a run rather than extending it");

	/* Abstention holds the verdict and does NOT advance the dwell counters. */
	impulse_prox_state_init(&st, NULL, 0, 0);
	m.result = IMPULSE_CS_OK;
	m.distance_cm = 100;
	impulse_prox_ingest(&st, &m);
	impulse_prox_ingest(&st, &m);
	CHECK(st.verdict == IMPULSE_PROX_NEAR, "NEAR established");

	m.result = IMPULSE_CS_FAIL_CONNECT;
	for (int i = 0; i < 3; i++) {
		impulse_prox_ingest(&st, &m);
	}
	CHECK(st.verdict == IMPULSE_PROX_NEAR, "abstention holds the verdict");
	CHECK(!impulse_prox_in_failsafe(&st), "3 abstentions is under the cap");
	CHECK(impulse_prox_criterion_met(&st, IMPULSE_CRIT_STAY_NEAR),
	      "held verdict still satisfies stayNear");

	/* But abstention is not free: past the cap the fail-safe takes over,
	 * because otherwise jamming the radio would be a bypass. */
	for (int i = 0; i < 5; i++) {
		impulse_prox_ingest(&st, &m);
	}
	CHECK(impulse_prox_in_failsafe(&st), "sustained abstention trips the fail-safe");
	CHECK(!impulse_prox_criterion_met(&st, IMPULSE_CRIT_STAY_NEAR),
	      "fail-safe resolves stayNear to NOT met");
	CHECK(impulse_prox_criterion_met(&st, IMPULSE_CRIT_GET_AWAY),
	      "fail-safe resolves getAway to met");

	/* A per-anchor override must be clamped on the watch. */
	impulse_prox_state_init(&st, NULL, 5, 60000);
	CHECK(st.near_enter_cm >= IMPULSE_THRESHOLD_MIN_CM, "near threshold clamped up");
	CHECK(st.away_enter_cm <= IMPULSE_THRESHOLD_MAX_CM, "away threshold clamped down");
	CHECK(st.away_enter_cm > st.near_enter_cm, "hysteresis band is never inverted");
}

/* ---- CS estimator fusion ------------------------------------------------ */
static struct impulse_cs_raw mk_raw(uint32_t ifft_cm)
{
	/* A "clean" sample: phase_slope and rtt exactly where the measured
	 * cross-estimator relationships say they should be. */
	struct impulse_cs_raw r = {0};

	r.has_ifft = true;
	r.ifft_cm = ifft_cm;
	r.has_phase_slope = true;
	r.phase_slope_cm = ((ifft_cm * IMPULSE_CS_PS_SLOPE_Q8) >> 8) +
			   IMPULSE_CS_PS_OFFSET_CM;
	r.has_rtt = true;
	r.rtt_cm = ((ifft_cm * IMPULSE_CS_RTT_SLOPE_Q8) >> 8) +
		   IMPULSE_CS_RTT_OFFSET_CM;
	return r;
}

static void test_cs_fuse(void)
{
	printf("CS estimator fusion\n");

	struct impulse_cs_burst b;
	struct impulse_cs_measurement m;

	/* A clean burst yields a CALIBRATED distance, not the raw median.
	 * Raw ifft of 297 cm maps to about 2 m of true distance:
	 * (297 - 97) / 0.993 = 201. */
	impulse_cs_burst_init(&b);
	for (int i = 0; i < 12; i++) {
		struct impulse_cs_raw r = mk_raw(297);
		CHECK(impulse_cs_burst_add(&b, &r), "clean sample accepted");
	}
	impulse_cs_burst_finish(&b, &m);
	CHECK(m.result == IMPULSE_CS_OK, "clean burst succeeds");
	CHECK(m.pbr_cm == 297, "raw estimator preserved");
	/* Stated against the constant, not a number. Hard-coding the expected
	 * output meant re-deriving the offset on real hardware (97 -> 72 cm,
	 * BM20C<->BM20C, 2026-09-12) broke this check for a reason that had
	 * nothing to do with the estimator. */
	CHECK(m.distance_cm == ((297 - IMPULSE_CS_IFFT_OFFSET_CM) << 8) /
				      IMPULSE_CS_IFFT_SLOPE_Q8,
	      "raw ifft is calibrated by the offset and slope");

	/* The calibration is what makes the §4.4 thresholds mean anything: an
	 * uncalibrated reading is a whole offset too far, which is a large
	 * fraction of the NEAR/AWAY hysteresis band. */
	CHECK(IMPULSE_CS_IFFT_OFFSET_CM > 0, "offset calibration present");

	/* Contact readings sit inside the near field and clamp to zero rather
	 * than going negative or abstaining. */
	impulse_cs_burst_init(&b);
	for (int i = 0; i < 12; i++) {
		struct impulse_cs_raw r = mk_raw(55);
		(void)impulse_cs_burst_add(&b, &r);
	}
	impulse_cs_burst_finish(&b, &m);
	CHECK(m.result == IMPULSE_CS_OK, "near-field burst still succeeds");
	CHECK(m.distance_cm == 0, "near field clamps to zero, never negative");

	/* The median must reject an outlier that a mean would smear in — the
	 * captures contained ifft values up to 8.12 m among 1 m readings. */
	impulse_cs_burst_init(&b);
	for (int i = 0; i < 12; i++) {
		struct impulse_cs_raw r = mk_raw(297);
		(void)impulse_cs_burst_add(&b, &r);
	}
	{
		struct impulse_cs_raw wild = mk_raw(812);
		(void)impulse_cs_burst_add(&b, &wild);
	}
	impulse_cs_burst_finish(&b, &m);
	CHECK(m.result == IMPULSE_CS_OK, "burst with one outlier still succeeds");
	CHECK(m.pbr_cm == 297, "median ignores the outlier entirely");

	/* Zero and absurd readings are discarded, not believed. */
	impulse_cs_burst_init(&b);
	{
		struct impulse_cs_raw z = mk_raw(500);
		z.ifft_cm = 0;
		CHECK(!impulse_cs_burst_add(&b, &z), "ifft==0 rejected");
		z.ifft_cm = 99999;
		CHECK(!impulse_cs_burst_add(&b, &z), "absurd distance rejected");
		CHECK(b.rejected_range == 2, "both counted as range rejects");
	}

	/* phase_slope far from its expected value means the direct path is
	 * probably blocked: reject as poor quality rather than trusting it. */
	impulse_cs_burst_init(&b);
	for (int i = 0; i < 12; i++) {
		struct impulse_cs_raw r = mk_raw(200);

		r.phase_slope_cm += IMPULSE_CS_PS_DIVERGENCE_MAX_CM + 100;
		CHECK(!impulse_cs_burst_add(&b, &r), "high multipath rejected");
	}
	impulse_cs_burst_finish(&b, &m);
	CHECK(m.result == IMPULSE_CS_FAIL_QUALITY, "quality failure reported");
	CHECK(m.distance_cm == 0, "no distance is produced on abstention");

	/* Relay: rtt inflated well beyond expectation while ifft looks near.
	 * This must NOT resolve to a near verdict — it is the one attack PBR
	 * cannot see, and reporting the distance would report the attacker's
	 * chosen answer. */
	impulse_cs_burst_init(&b);
	for (int i = 0; i < 12; i++) {
		struct impulse_cs_raw r = mk_raw(80);

		r.rtt_cm += IMPULSE_CS_RELAY_MARGIN_CM + 200;
		CHECK(!impulse_cs_burst_add(&b, &r), "relay-shaped sample rejected");
	}
	impulse_cs_burst_finish(&b, &m);
	CHECK(m.result == IMPULSE_CS_FAIL_DISAGREE, "relay suspicion reported");
	CHECK(m.distance_cm == 0, "relay suspicion yields no distance");

	/* rtt SHORTER than expected is only noise — nothing outruns light, so
	 * a short reading cannot fabricate nearness and must not abstain. */
	impulse_cs_burst_init(&b);
	for (int i = 0; i < 12; i++) {
		struct impulse_cs_raw r = mk_raw(200);

		r.rtt_cm = (r.rtt_cm > 300U) ? (r.rtt_cm - 300U) : 0U;
		CHECK(impulse_cs_burst_add(&b, &r), "short rtt still accepted");
	}
	impulse_cs_burst_finish(&b, &m);
	CHECK(m.result == IMPULSE_CS_OK, "short rtt does not abstain");

	/* Too few surviving samples abstains rather than reporting a thin
	 * answer with false confidence. */
	impulse_cs_burst_init(&b);
	for (int i = 0; i < IMPULSE_CS_BURST_MIN_ACCEPTED - 1; i++) {
		struct impulse_cs_raw r = mk_raw(200);
		(void)impulse_cs_burst_add(&b, &r);
	}
	impulse_cs_burst_finish(&b, &m);
	CHECK(m.result != IMPULSE_CS_OK, "a thin burst abstains");

	/* A missing primary estimate is not covered for by phase_slope. */
	impulse_cs_burst_init(&b);
	{
		struct impulse_cs_raw r = mk_raw(200);

		r.has_ifft = false;
		CHECK(!impulse_cs_burst_add(&b, &r), "no ifft means no sample");
		CHECK(b.missing_ifft == 1, "missing ifft counted");
	}

	/* End to end: a good burst must drive the §4.4 machine to NEAR. */
	{
		struct impulse_prox_state st;

		impulse_prox_state_init(&st, NULL, 0, 0);
		for (int poll = 0; poll < 2; poll++) {
			impulse_cs_burst_init(&b);
			for (int i = 0; i < 12; i++) {
				struct impulse_cs_raw r = mk_raw(180);
				(void)impulse_cs_burst_add(&b, &r);
			}
			impulse_cs_burst_finish(&b, &m);
			impulse_prox_ingest(&st, &m);
		}
		CHECK(st.verdict == IMPULSE_PROX_NEAR, "clean bursts reach NEAR");
		CHECK(impulse_prox_criterion_met(&st, IMPULSE_CRIT_STAY_NEAR),
		      "stayNear satisfied by real burst pipeline");
	}
}

/* ---- enforcement: every exit must settle the output profile -------------- */
static void test_enforcement_outputs(void)
{
	struct impulse_enforcement_ctx ctx;
	struct impulse_event e = mk_event(1, 0, 1440, 0);

	e.criteria = IMPULSE_CRIT_GET_AWAY;
	e.profile = IMPULSE_PROFILE_STRICT_BOTH;
	e.donning_grace_s = 300;

	/*
	 * Regression, observed on hardware 2026-09-12. The donning grace
	 * short-circuited check_condition with `condition_met = true; return`,
	 * which skipped the profile transition at the bottom of the function.
	 * profile_run kept motor_on = 1, and main() drives the pads from
	 * profile_run every pass, so putting the watch on mid-alarm reported
	 * cond_met=1 while the motor kept running for the rest of the window.
	 */
	impulse_enforcement_init(&ctx);
	impulse_enforcement_enter(&ctx, &e, 1000, false);
	CHECK(ctx.grace_deadline_utc == 0,
	      "entering a window UNWORN opens no donning grace");

	/* Unworn and near the anchor => getAway unmet => profile running. */
	ctx.prox.have_verdict = true;
	ctx.prox.verdict = IMPULSE_PROX_NEAR;
	CHECK(impulse_enforcement_check_condition(&ctx, 1000, false, NULL,
						  true) == false,
	      "getAway while near is not met");
	(void)impulse_enforcement_tick(&ctx, 100);
	CHECK(ctx.profile_run.motor_on, "strict profile drives the motor");

	/* Donning opens the grace. Output must stop on the same call. */
	impulse_enforcement_set_worn(&ctx, true, 1010);
	CHECK(impulse_enforcement_check_condition(&ctx, 1010, false, NULL,
						  true) == true,
	      "donning grace short-circuits to met");
	CHECK(!ctx.profile_run.motor_on,
	      "donning grace stops the motor, not just the verdict");
	CHECK(!ctx.profile_run.buzzer_on, "donning grace stops the buzzer");
	(void)impulse_enforcement_tick(&ctx, 100);
	CHECK(!ctx.profile_run.motor_on, "a stopped profile stays stopped");

	/* Grace expiry with the watch still near => alarm resumes. */
	CHECK(impulse_enforcement_check_condition(&ctx, 1010 + 301, false,
						  NULL, true) == false,
	      "grace expiry re-evaluates the criterion");
	(void)impulse_enforcement_tick(&ctx, 100);
	CHECK(ctx.profile_run.motor_on, "alarm resumes after the grace");

	/* Entering a window already wearing the watch DOES earn the grace. */
	{
		struct impulse_enforcement_ctx w;

		impulse_enforcement_init(&w);
		impulse_enforcement_enter(&w, &e, 1000, true);
		CHECK(w.grace_deadline_utc == 1000 + 300,
		      "entering a window WORN opens the donning grace");
	}

	/*
	 * Cadence while abstaining mid-alarm. getAway can only release through
	 * the abstention fail-safe (the link is gone, so there are no AWAY
	 * measurements to dwell on), and at the 60 s NOT_MET cadence that took
	 * CS_ABSTAIN_MAX_CONSECUTIVE + 1 = 6 polls = six minutes.
	 */
	{
		struct impulse_enforcement_ctx c2;

		impulse_enforcement_init(&c2);
		impulse_enforcement_enter(&c2, &e, 1000, false);
		c2.prox.have_verdict = true;
		c2.prox.verdict = IMPULSE_PROX_NEAR;
		(void)impulse_enforcement_check_condition(&c2, 1000, false,
							  NULL, true);
		CHECK(c2.condition_met == false, "alarm is running");
		CHECK(impulse_enforcement_poll_interval_s(&c2) ==
			      IMPULSE_ENFORCEMENT_POLL_NOT_MET_S,
		      "a settled not-met verdict keeps the normal cadence");

		/* The user walks out: the link drops and polls start abstaining. */
		c2.prox.abstain_run = 1;
		CHECK(impulse_enforcement_poll_interval_s(&c2) ==
			      IMPULSE_ENFORCEMENT_POLL_ABSTAIN_S,
		      "abstaining mid-alarm polls at the fast cadence");

		/* Releasing must still cost the full dwell, just not the wall
		 * clock — abstention is not free (§4.5). */
		CHECK(IMPULSE_ENFORCEMENT_POLL_ABSTAIN_S *
			      (IMPULSE_CS_ABSTAIN_MAX_CONSECUTIVE + 1) <= 90U,
		      "leaving the room releases within ~a minute");

		c2.condition_met = true;
		CHECK(impulse_enforcement_poll_interval_s(&c2) ==
			      IMPULSE_ENFORCEMENT_POLL_MET_S,
		      "a met condition is never put on the fast cadence");
	}

	/* A no-active-event exit must settle too. */
	ctx.active = NULL;
	CHECK(impulse_enforcement_check_condition(&ctx, 2000, false, NULL,
						  true) == true,
	      "no active event is met");
	CHECK(!ctx.profile_run.motor_on, "no active event stops the motor");

	/* phoneAway fail-open on a degraded link must settle too. */
	e.criteria = IMPULSE_CRIT_PHONE_AWAY;
	e.donning_grace_s = 0;
	impulse_enforcement_init(&ctx);
	impulse_enforcement_enter(&ctx, &e, 3000, true);
	ctx.prox.have_verdict = true;
	ctx.prox.verdict = IMPULSE_PROX_NEAR;
	ctx.phone_near_since_utc = 1;
	CHECK(impulse_enforcement_check_condition(
		      &ctx, 3000 + IMPULSE_PHONE_AWAY_TOLERANCE_S + 1, false,
		      NULL, true) == false,
	      "phoneAway fires once the tolerance is exhausted");
	(void)impulse_enforcement_tick(&ctx, 100);
	CHECK(ctx.profile_run.motor_on, "phoneAway drives the motor");

	ctx.prox.have_verdict = false; /* link degrades => fail open */
	CHECK(impulse_enforcement_check_condition(&ctx, 3000, false, NULL,
						  true) == true,
	      "phoneAway fails open on a degraded link");
	CHECK(!ctx.profile_run.motor_on,
	      "phoneAway fail-open stops the motor");
}

/* ---- lost link = AWAY, and the lost-link grace (v3 §4.5, v0.16) ---------- */

static struct impulse_cs_link_obs link_obs(int64_t activity_ms)
{
	struct impulse_cs_link_obs o;

	memset(&o, 0, sizeof(o));
	o.known = true;
	o.activity_seen = true;
	o.last_activity_ms = activity_ms;
	return o;
}

/* A getAway window with the alarm running, link healthy at t = 1000 ms. */
static void alarm_running(struct impulse_enforcement_ctx *c,
			  const struct impulse_event *e,
			  struct impulse_cs_link_obs *o)
{
	impulse_enforcement_init(c);
	impulse_enforcement_enter(c, e, 100, false);
	*o = link_obs(1000);
	impulse_enforcement_link_update(c, o, 1000, 100);
	c->prox.have_verdict = true;
	c->prox.verdict = IMPULSE_PROX_NEAR;
	(void)impulse_enforcement_check_condition(c, 100, false, NULL, true);
	(void)impulse_enforcement_tick(c, 100);
}

static void test_link_loss(void)
{
	struct impulse_enforcement_ctx c;
	struct impulse_cs_link_obs o;
	struct impulse_event e = mk_event(2, 0, 1440, 0);

	e.criteria = IMPULSE_CRIT_GET_AWAY;
	e.profile = IMPULSE_PROFILE_STRICT_BOTH;

	/* The whole path: silence inside 2 s, ring stays red, AWAY at 10 s. */
	alarm_running(&c, &e, &o);
	CHECK(!c.condition_met && c.profile_run.motor_on,
	      "link: alarm running before the loss");
	impulse_enforcement_link_update(&c, &o, 2199, 101);
	CHECK(!impulse_enforcement_output_silenced(&c),
	      "link: 1.199 s of silence is not yet a lost link");
	impulse_enforcement_link_update(&c, &o, 2200, 101);
	CHECK(impulse_enforcement_output_silenced(&c),
	      "link: a lost link silences output inside 2 s");
	CHECK(!c.condition_met,
	      "link: silenced but still unmet, so the ring stays red");
	impulse_enforcement_link_update(&c, &o, 10999, 110);
	CHECK(impulse_enforcement_output_silenced(&c) && !c.condition_met,
	      "link: 9.999 s lost is still grace, not AWAY");
	impulse_enforcement_link_update(&c, &o, 11000, 110);
	CHECK(c.condition_met, "link: 10 s lost concludes AWAY, getAway met");
	CHECK(c.prox.have_verdict && c.prox.verdict == IMPULSE_PROX_AWAY &&
	      c.prox.abstain_run == 0,
	      "link: the verdict is a real AWAY, not the fail-safe");
	CHECK(!impulse_enforcement_output_silenced(&c) &&
	      !c.profile_run.motor_on && !c.profile_run.buzzer_on,
	      "link: output stopped for real once AWAY is concluded");

	/* Cancelled by a noncompliant measurement; stale and far ones don't. */
	alarm_running(&c, &e, &o);
	impulse_enforcement_link_update(&c, &o, 2500, 101);
	CHECK(impulse_enforcement_output_silenced(&c), "grace: opens on loss");
	o.have_result = true;
	o.result_ms = 900;
	o.result_cm = 40;
	impulse_enforcement_link_update(&c, &o, 2600, 101);
	CHECK(impulse_enforcement_output_silenced(&c),
	      "grace: a burst from BEFORE the loss does not end it");
	o.last_activity_ms = 4000;
	o.result_ms = 4000;
	o.result_cm = 500;
	impulse_enforcement_link_update(&c, &o, 4000, 103);
	CHECK(impulse_enforcement_output_silenced(&c),
	      "grace: a FAR measurement does not end it");
	o.last_activity_ms = 5000;
	o.result_ms = 5000;
	o.result_cm = 80;
	impulse_enforcement_link_update(&c, &o, 5000, 104);
	CHECK(!impulse_enforcement_output_silenced(&c),
	      "grace: a noncompliant measurement ends it");
	(void)impulse_enforcement_tick(&c, 100);
	CHECK(!c.condition_met && c.profile_run.motor_on,
	      "grace: and the alarm resumes at once");

	/* Cooldown: no second grace for 2 min, but the 10 s rule still holds. */
	impulse_enforcement_link_update(&c, &o, 6500, 106);
	CHECK(!impulse_enforcement_output_silenced(&c),
	      "cooldown: no second grace inside 2 min of the first");
	impulse_enforcement_link_update(&c, &o, 14999, 115);
	CHECK(!c.condition_met, "cooldown: not AWAY before 10 s");
	impulse_enforcement_link_update(&c, &o, 15000, 115);
	CHECK(c.condition_met,
	      "cooldown: 10 s lost is still AWAY during the cooldown");

	c.prox.verdict = IMPULSE_PROX_NEAR; /* walked back in */
	(void)impulse_enforcement_check_condition(&c, 200, false, NULL, true);
	o = link_obs(2500 + 120000);
	impulse_enforcement_link_update(&c, &o, 2500 + 120000, 222);
	impulse_enforcement_link_update(&c, &o, 2500 + 120000 + 1200, 223);
	CHECK(impulse_enforcement_output_silenced(&c),
	      "cooldown: a grace is available again 2 min after the last");

	/* The link came back but produced no measurement in time: AWAY. */
	alarm_running(&c, &e, &o);
	impulse_enforcement_link_update(&c, &o, 2500, 101);
	o.last_activity_ms = 10500; /* activity resumed, no burst yet */
	impulse_enforcement_link_update(&c, &o, 10999, 110);
	CHECK(impulse_enforcement_output_silenced(&c) && !c.condition_met,
	      "grace: link back without a measurement holds the grace");
	impulse_enforcement_link_update(&c, &o, 11000, 110);
	CHECK(c.condition_met,
	      "grace: no noncompliant measurement by 10 s concludes AWAY");

	/* stayNear: no grace, and a lost link FAILS the commitment. */
	{
		struct impulse_event s = mk_event(3, 0, 1440, 0);

		s.profile = IMPULSE_PROFILE_STRICT_BOTH;
		impulse_enforcement_init(&c);
		impulse_enforcement_enter(&c, &s, 100, false);
		o = link_obs(1000);
		impulse_enforcement_link_update(&c, &o, 1000, 100);
		c.prox.have_verdict = true;
		c.prox.verdict = IMPULSE_PROX_NEAR;
		CHECK(impulse_enforcement_check_condition(&c, 100, false, NULL,
							  true),
		      "stayNear: near is met");
		impulse_enforcement_link_update(&c, &o, 2500, 101);
		CHECK(!impulse_enforcement_output_silenced(&c),
		      "stayNear: a lost link never opens a grace");
		impulse_enforcement_link_update(&c, &o, 11000, 110);
		CHECK(!c.condition_met,
		      "stayNear: 10 s lost is AWAY, which fails the commitment");
		(void)impulse_enforcement_tick(&c, 100);
		CHECK(c.profile_run.motor_on, "stayNear: and the alarm starts");
	}

	/* Activity from before the window, or none at all, is never a loss. */
	impulse_enforcement_init(&c);
	impulse_enforcement_enter(&c, &e, 100, false);
	o = link_obs(500);
	impulse_enforcement_link_update(&c, &o, 1000, 100);
	impulse_enforcement_link_update(&c, &o, 30000, 130);
	CHECK(!impulse_enforcement_output_silenced(&c) && !c.prox.have_verdict,
	      "window: activity from before the window is not a lost link");
	o.activity_seen = false;
	impulse_enforcement_link_update(&c, &o, 60000, 160);
	CHECK(!c.prox.have_verdict,
	      "window: a link that never existed is abstention, not AWAY");

	/* Telemetry-less backends (stub, anchor) are untouched. */
	alarm_running(&c, &e, &o);
	o.known = false;
	impulse_enforcement_link_update(&c, &o, 50000, 150);
	CHECK(!impulse_enforcement_output_silenced(&c) && !c.condition_met,
	      "backend: no telemetry, no lost-link rule");
}

int main(void)
{
	printf("impulse host tests\n\n");

	test_dates();
	test_blob();
	test_recalc();
	test_classify();
	test_integrity();
	test_profiles();
	test_proximity();
	test_cs_fuse();
	test_enforcement_outputs();
	test_link_loss();

	printf("\n%d checks, %d failed\n", g_run, g_fail);
	return g_fail == 0 ? 0 : 1;
}
