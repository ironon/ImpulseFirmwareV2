/*
 * Host tests for the pure-logic core.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

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
	CHECK(m.distance_cm >= 195 && m.distance_cm <= 210,
	      "raw 297 cm calibrates to ~200 cm true");

	/* The calibration is what makes the §4.4 thresholds mean anything: an
	 * uncalibrated 297 would read as beyond AWAY_ENTER_CM (350)? no — but
	 * it would read 297 where the truth is 201, which is most of the way
	 * across the 150 cm hysteresis band. */
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

	printf("\n%d checks, %d failed\n", g_run, g_fail);
	return g_fail == 0 ? 0 : 1;
}
