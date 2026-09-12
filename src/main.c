/*
 * Impulse firmware — nRF54LM20A / Zephyr.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chunk A/B/C/F skeleton per firmware_spec_v3_nrf.md §10. What exists here is
 * the enforcement spine and the board HAL. What does NOT exist yet, and is
 * deliberately absent rather than half-written:
 *   - BLE / GATT surface (chunk E) — nothing can push a schedule yet.
 *   - Channel sounding (chunk G) — stubbed; see proximity.c.
 *   - WiFi via the WM02C (chunk I).
 *   - Hard override (chunk J) — gated on Concepts/Safety.md, not on code.
 *   - Power management / DORMANT_SLEEP (chunk D).
 */

#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/reboot.h>

#include "fatal.h"

#include "enforcement/enforcement.h"
#include "hal/hal.h"
#include "integrity/integrity.h"
#include "proximity/proximity.h"
#include "schedule/schedule.h"
#include "storage/storage.h"
#include "identity.h"
/* Shared §6.1 command constants — wire format, not role-specific. */
#include "anchor/anchor.h"
#if defined(CONFIG_IMPULSE_NET)
#include "net/net_udp.h"
#include "net/net_wifi.h"
#endif

#include "app_api.h"

#if defined(CONFIG_IMPULSE_BLE)
#include "ble/ble_watch.h"
#endif
#if defined(CONFIG_IMPULSE_ROLE_ANCHOR)
#include "anchor/anchor.h"
#endif

LOG_MODULE_REGISTER(impulse, LOG_LEVEL_INF);

/*
 * LFCLK source, read straight from the CLOCK peripheral.
 *
 * spec §3.2 requires this to be CONFIRMED AT RUNTIME rather than assumed from
 * the devicetree, because the failure is silent: Zephyr falls back to the
 * internal RC and the crystal sits unused on the module with no error
 * anywhere. An 8 h sleep on the RC can wake past the boundary minute, and a
 * window whose minute has passed is never entered.
 *
 * CLOCK_S base 0x5010E000, LFCLK block at +0x440. SRC/STAT: 0=LFRC, 1=LFXO,
 * 2=LFSYNT (synthesised from HFCLK).
 */
#define CLOCK_LFCLK_SRC   (*(volatile uint32_t *)0x5010E440UL)
#define CLOCK_LFCLK_RUN   (*(volatile uint32_t *)0x5010E448UL)
#define CLOCK_LFCLK_STAT  (*(volatile uint32_t *)0x5010E44CUL)

static const char *lfclk_src_name(uint32_t v)
{
	switch (v & 0x3U) {
	case 0: return "LFRC (internal RC)";
	case 1: return "LFXO (crystal)";
	case 2: return "LFSYNT (from HFCLK)";
	default: return "?";
	}
}

static void report_lfclk(void)
{
	uint32_t src = CLOCK_LFCLK_SRC;
	uint32_t run = CLOCK_LFCLK_RUN;
	uint32_t stat = CLOCK_LFCLK_STAT;

	LOG_INF("LFCLK: SRC=%u (%s) RUN=%u STAT=%u (%s)", src & 3U,
		lfclk_src_name(src), run & 1U, stat & 3U, lfclk_src_name(stat));

	/*
	 * On nRF54L those registers normally read ZERO and that is not a fault.
	 * The GRTC manages its own low-frequency source through
	 * CONFIG_NRF_GRTC_TIMER_CLOCK_MANAGEMENT (nrf_grtc_timer.c calls
	 * nrfx_grtc_clock_source_set(NRF_GRTC_CLKSEL_LFCLK)), rather than
	 * through the legacy CLOCK.LFCLK block that older nRF parts used. So
	 * the authoritative answer for §3.2 is the GRTC's build-time source
	 * selection below, cross-checked against measured drift — a crystal is
	 * tens of ppm, the internal RC is whole percent.
	 */
#if defined(CONFIG_NRF_GRTC_TIMER_SOURCE_LFXO)
	LOG_INF("GRTC source: LFXO (crystal) — spec v3 §3.2 satisfied");
#elif defined(CONFIG_NRF_GRTC_TIMER_SOURCE_LFRC)
	LOG_ERR("GRTC source: LFRC — the crystal is unused, see spec v3 §3.2");
#else
	LOG_WRN("GRTC source: not one of LFXO/LFRC — check the build config");
#endif

	if ((run & 1U) != 0U && (stat & 3U) != 1U) {
		LOG_ERR("legacy LFCLK block is running on a non-crystal source");
	}
}

#if defined(CONFIG_IMPULSE_ROLE_WATCH)
#define ROLE_NAME "watch"
#elif defined(CONFIG_IMPULSE_ROLE_ANCHOR)
#define ROLE_NAME "anchor"
#else
#error "Select CONFIG_IMPULSE_ROLE_WATCH or CONFIG_IMPULSE_ROLE_ANCHOR"
#endif

/* Loop cadence. Coarse on purpose: this is a scheduling loop, not a control
 * loop, and chunk D will replace the sleep with Zephyr's PM subsystem. */
#define TICK_MS 100U

static struct impulse_schedule g_schedule;
static struct impulse_integrity g_integrity;
static struct impulse_enforcement_ctx g_enf;
static int16_t g_tz_offset_minutes;

/* Wall-clock UTC. The watch is the root of trust for time (§3.1); the app may
 * propose changes and the watch decides whether to accept them (§9.7).
 * TODO(chunk D): back this with the GRTC and the LFXO rather than a counter
 * seeded at boot, and confirm at runtime that LFCLK really is on LFXO. */
/* Sanity floor for a restored clock: 2025-01-01. Anything at or below this is
 * an uninitialised or corrupt record, not a real timestamp. */
#define IMPULSE_CLOCK_SANITY_FLOOR 1735689600LL
/* How often the wall clock is written. The worst case for a commitment is that
 * it starts this late after an unexpected reset, so it is a trade against NVS
 * wear rather than a free choice. */
#define IMPULSE_CLOCK_SAVE_INTERVAL_S 60

/*
 * Bench override for the worn sensor: -1 = use the sensor, 0/1 = force.
 *
 * BENCH ONLY, and it exists because the IR window is behind the enclosure on
 * the prototype, so the sensor cannot cross its threshold no matter what is
 * done to the board. Every stage of Sunrise Lock after "the alarm starts"
 * depends on a worn transition, so without this the whole donning-grace and
 * WATCH_WORN path is untestable.
 *
 * It is deliberately NOT reachable over BLE — only from the bring-up shell.
 * A forced worn state is a way to make the watch believe a commitment is being
 * honoured, which is precisely the thing the app must never be able to assert.
 */
static int8_t g_worn_override = -1;

/* When the WiFi fallback started, so it can be given up on. 0 = not trying. */
static int64_t g_wifi_escalate_since_ms;
static bool g_wifi_escalate_gave_up;

static bool g_clock_restored;
static int64_t g_utc_base;
static int64_t g_utc_base_uptime_ms;

/*
 * WALL CLOCK RETAINED ACROSS A WARM RESET.
 *
 * The flash save (IMPULSE_CLOCK_SAVE_INTERVAL_S) is written once a minute, so
 * a reset restored a clock up to a minute stale. That was tolerable while
 * resets were rare. It is not now: a fatal error deliberately REBOOTS (fatal.c),
 * and on 2026-09-12 a SoftDevice Controller assert did exactly that ~43 s into
 * a Sunrise Lock window. The restored clock came back 45 s slow — the watch
 * then stayed in ENFORCEMENT for 45 s past the window's real end, which looked
 * exactly like a failure to exit the window, and every window after it would
 * have been shifted the same way.
 *
 * This mirror is written every loop pass (a RAM write, free) and lives in
 * .noinit, which startup never zeroes. After a crash, a watchdog reset or the
 * reset button the RAM is intact, and the clock comes back within about a
 * second: the last second the loop saw, plus the time since boot. After a
 * power loss the magic is garbage and the flash save is the fallback, exactly
 * as before.
 */
#define RETAINED_CLOCK_MAGIC 0x434C4B52U /* "RKLC" */

struct retained_clock {
	uint32_t magic;
	uint32_t magic_inv; /* ~magic: random RAM matching both is ~2^-64 */
	int64_t utc;
};

static struct retained_clock g_retained_clock __noinit;

/*
 * Forces an immediate condition check rather than waiting out a poll interval.
 * Set on window entry, and (once the IMU is wired) on a motion interrupt.
 *
 * Without this the first check of a new window is gated on UPTIME, because the
 * poll deadline is compared against k_uptime_get(): a watch that had been
 * running for less than one poll interval would enter a window and sit at its
 * optimistic default of "condition met" for up to 180 s, driving nothing.
 * On a stayNear commitment that is 180 s of silent non-enforcement at exactly
 * the moment the window opens. Found on hardware, 2026-08-30.
 */
static bool g_force_poll;

/* Button activity, surfaced by `impulse status` so a press can be confirmed
 * without eyes on the board. */
/*
 * Battery percentage, refreshed on the slow loop. 0xFF is the spec's "not
 * available", which is what is reported until the first successful read.
 */
static uint8_t g_battery_pct = 0xFFU;
/* Raw millivolts as well as percent: percent is (mv-3300)*100/900, i.e. 9 mV
 * per point, far too coarse to measure a discharge RATE over minutes. The
 * overnight-endurance question needs the rate. */
static int32_t g_battery_mv;
static int32_t g_worn_delta_mv;

static uint32_t g_btn_short;
static uint32_t g_btn_long;
static uint32_t g_btn_very_long;

/*
 * Charger detect (§6.3). TP4057 CHRG is open-drain and pulls LOW while
 * charging, so the DT declares the pin ACTIVE_LOW with a pull-up and a logical
 * 1 here means "charging". That polarity was an ASSUMPTION until it was
 * checked against a real USB-C plug event — see the boot/status log.
 */
static const struct gpio_dt_spec charger =
	GPIO_DT_SPEC_GET(DT_NODELABEL(charger_detect), gpios);

static int64_t now_utc(void)
{
	return g_utc_base + (k_uptime_get() - g_utc_base_uptime_ms) / 1000;
}

static void clock_show_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(clock_off_work, clock_show_work_handler);

static void clock_show_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)impulse_led_ring_clear();
}

static bool status_ring_active(void);

static void on_button(enum impulse_button_event ev)
{
	switch (ev) {
	case IMPULSE_BTN_SHORT: {
		g_btn_short++;
		LOG_INF("button: SHORT");
		/* v3 §6.2: the ring is dark in DORMANT; a short press lights
		 * the analog clock for CLOCK_SHOW_MS and then clears it. This
		 * inverts v2 §5.7.2, which latched the clock through sleep.
		 * Status colours take priority over the clock (also §6.2), so
		 * during a window a press leaves the red/green status alone. */
		if (status_ring_active()) {
			break;
		}

		uint16_t minute;

		impulse_local_from_utc(now_utc(), g_tz_offset_minutes, NULL,
				       &minute);
		(void)impulse_led_ring_show_clock(minute, 40);
		(void)k_work_reschedule(&clock_off_work, K_MSEC(5000));
		break;
	}
	case IMPULSE_BTN_LONG:
	case IMPULSE_BTN_VERY_LONG:
		/* Counted separately: collapsing them made it impossible to
		 * tell a 1.5 s hold from a 5 s one, which is precisely what the
		 * hard-override seam has to distinguish later. */
		if (ev == IMPULSE_BTN_VERY_LONG) {
			g_btn_very_long++;
			LOG_INF("button: VERY_LONG");
		} else {
			LOG_INF("button: LONG");
		}
		/*
		 * RESERVED for the hard override (launch-plan §11.2), whose
		 * mechanism is UNDECIDED. v3 §6.2 says explicitly: leave the
		 * seam, do not implement the v2 two-button hold, and do not
		 * scatter press handling. Deliberately does nothing.
		 */
		g_btn_long++;
		LOG_WRN("long press ignored: hard override unimplemented (chunk J)");
		break;
	default:
		break;
	}
}


#if defined(CONFIG_IMPULSE_ROLE_WATCH) && \
	(defined(CONFIG_IMPULSE_CS_BACKEND) || defined(CONFIG_IMPULSE_NET))
/*
 * Tell this event's anchors that the watch has been removed (or put back on).
 *
 * BLE FIRST, WiFi as fallback — and never both, because BLE and WiFi cannot
 * run together on Board V1 and the FCC grant is expected to require they do
 * not. The two are naturally exclusive: WiFi is only reached for when there is
 * no BLE link, which is precisely the out-of-range case.
 */
static void escalate_to_anchors(uint8_t command,
				const struct impulse_event *ev)
{
	if (ev == NULL || ev->beep_anchor_count == 0U) {
		return;
	}

#if defined(CONFIG_IMPULSE_CS_BACKEND)
	if (impulse_cs_notify_watch_state(command, ev->id) == 0) {
		/* Reached it over the ranging link. WiFi is not needed, and
		 * asking for it would put two radios on the air at once. */
#if defined(CONFIG_IMPULSE_NET)
		impulse_wifi_request(false);
#endif
		g_wifi_escalate_since_ms = 0;
		g_wifi_escalate_gave_up = false;
		return;
	}
#endif

	/*
	 * No BLE link — the watch is out of range of the anchor. THIS is the
	 * case WiFi exists for: Sunrise Lock's known bypass is to carry the
	 * watch out of range and go back to bed, and BLE by construction
	 * cannot report an absence it cannot see.
	 */
#if !defined(CONFIG_IMPULSE_NET)
	/* No WiFi in this build: BLE was the only carrier and it is not
	 * available. Say so rather than failing mutely. */
	LOG_DBG("escalation 0x%02x: no BLE link and no WiFi in this build",
		command);
	return;
#else
	int64_t now_ms = k_uptime_get();

	if (g_wifi_escalate_since_ms == 0) {
		g_wifi_escalate_since_ms = now_ms;
		g_wifi_escalate_gave_up = false;
	}

	if ((now_ms - g_wifi_escalate_since_ms) >
	    ((int64_t)CONFIG_IMPULSE_ESCALATION_WIFI_GIVEUP_MIN * 60 * 1000)) {
		if (!g_wifi_escalate_gave_up) {
			g_wifi_escalate_gave_up = true;
			LOG_WRN("escalation: no anchor over BLE or WiFi for "
				"%d min — giving up (out of scope)",
				CONFIG_IMPULSE_ESCALATION_WIFI_GIVEUP_MIN);
			impulse_wifi_request(false);
		}
		return;
	}

	impulse_wifi_request(true);
	(void)impulse_udp_send_to_anchors(command, ev);
#endif /* CONFIG_IMPULSE_NET */
}
#endif

/* Recompute today's plan and pick up the currently active event. */
static void refresh_active_event(void)
{
	static struct impulse_day_plan plan;
	struct impulse_date today;
	uint16_t minute;
	const struct impulse_event *active;

	impulse_local_from_utc(now_utc(), g_tz_offset_minutes, &today, &minute);
	impulse_recalculate_day(&g_schedule, &today, g_tz_offset_minutes,
				&plan);

	active = impulse_active_event(&plan, minute);

	if (active == g_enf.active) {
		return;
	}

	if (active == NULL) {
		LOG_INF("window ended");
		impulse_enforcement_exit(&g_enf);
		impulse_motor_set(false);
		impulse_buzzer_set(false);
#if defined(CONFIG_IMPULSE_NET) && defined(CONFIG_IMPULSE_ROLE_WATCH)
#if defined(CONFIG_IMPULSE_NET)
		impulse_wifi_request(false);
#endif
		g_wifi_escalate_since_ms = 0;
		g_wifi_escalate_gave_up = false;
#endif
		return;
	}

	LOG_INF("window start: criteria=%u profile=%u", active->criteria,
		active->profile);
	impulse_enforcement_enter(&g_enf, active, now_utc(), g_enf.worn);
	g_force_poll = true;

#if defined(CONFIG_IMPULSE_NET) && defined(CONFIG_IMPULSE_ROLE_WATCH)
	/*
	 * §5.5.1: a window with beepAnchors KEEPS WIFI ALIVE and reconnects if
	 * it drops. The rule this replaces — "WiFi is abandoned once a window
	 * opens, nothing in enforcement needs it" — was false for exactly this
	 * case and shipped broken, because the whole anchor-escalation path
	 * needs the association.
	 */
	impulse_wifi_request(active->beep_anchor_count > 0U);

	/*
	 * §5.4.4 WINDOW-START WORN CHECK, and it is TRANSITION-INDEPENDENT on
	 * purpose. Sunrise Lock's normal case is a watch that has been sitting
	 * unworn on a nightstand for hours: there is no not-worn -> worn edge
	 * at window start, so an edge-triggered escalation would never fire on
	 * the one commitment this feature exists for.
	 */
	if (!g_enf.worn) {
		LOG_INF("window opened with the watch UNWORN — escalating");
		escalate_to_anchors(IMPULSE_CMD_WATCH_REMOVED, active);
	}
#endif
}

#if defined(CONFIG_IMPULSE_ROLE_ANCHOR)

static uint16_t g_max_beep_minutes = 30;

uint8_t impulse_app_anchor_apply_schedule(const uint8_t *blob, size_t len)
{
	static struct impulse_schedule incoming;

	if (impulse_blob_parse(blob, len, &incoming) != IMPULSE_BLOB_OK) {
		LOG_WRN("anchor schedule rejected: parse failure");
		return IMPULSE_END_FAILED;
	}

	g_schedule = incoming;
	g_schedule.crc32 = impulse_crc32(blob, len);
	(void)impulse_storage_save_schedule(&g_schedule);
	LOG_INF("anchor schedule stored: %u event(s)", g_schedule.count);

	/* §4.9: a schedule push is one of the moments the anchor re-checks
	 * whether a window involving it has begun, because it has no RTC
	 * callback system of its own. */
	if (impulse_app_anchor_in_active_event()) {
		impulse_anchor_on_window_start();
	}
	return IMPULSE_END_ACCEPTED;
}

void impulse_app_anchor_set_settings(uint16_t max_beep_minutes,
				     int16_t tz_offset_minutes)
{
	g_max_beep_minutes = max_beep_minutes;
	g_tz_offset_minutes = tz_offset_minutes;
	(void)impulse_storage_save_settings(g_tz_offset_minutes);
}

const struct impulse_event *impulse_app_find_active_event(const uint8_t *event_uuid)
{
	static struct impulse_day_plan plan;
	struct impulse_date today;
	uint16_t minute;

	/*
	 * §4.6/§4.7: resolve the event the watch named against OUR OWN local
	 * schedule and clock, not against anything in the datagram. The packet
	 * carries a UUID and nothing else — no window, no profile — precisely
	 * so that a watch cannot talk this anchor into beeping outside a window
	 * the anchor itself agrees is open.
	 *
	 * If the anchor has never had a valid time, local_from_utc yields a
	 * 1970 date, no event matches, and it stays silent. That is §4.7's
	 * stated fail-open for an unsynced anchor.
	 */
	impulse_local_from_utc(now_utc(), g_tz_offset_minutes, &today, &minute);
	impulse_recalculate_day(&g_schedule, &today, g_tz_offset_minutes, &plan);

	for (uint8_t i = 0; i < plan.count; i++) {
		const struct impulse_event *e = plan.events[i];

		if (!impulse_uuid_eq(e->id, event_uuid)) {
			continue;
		}
		if (minute < e->start_time || minute >= e->end_time) {
			return NULL; /* known event, but not now */
		}
		return e;
	}
	return NULL;
}

uint32_t impulse_app_anchor_schedule_crc(void)
{
	return g_schedule.crc32;
}

bool impulse_app_anchor_in_active_event(void)
{
	struct impulse_date today;
	uint16_t minute;

	impulse_local_from_utc(now_utc(), g_tz_offset_minutes, &today, &minute);
	return impulse_anchor_in_active_event(&g_schedule, &today, minute,
					      g_tz_offset_minutes);
}

#endif /* CONFIG_IMPULSE_ROLE_ANCHOR */

#if defined(CONFIG_IMPULSE_NET)

/*
 * Minimal JSON field extractor. The app sends exactly
 * {"ssid": "...", "password": "..."} and nothing else, so a parser is not
 * warranted — but a password can legitimately contain almost anything, so this
 * copies the raw bytes between the quotes rather than trying to interpret them.
 * Escape sequences are NOT decoded: a passphrase containing a backslash or a
 * quote would need a real parser, and would be silently wrong here.
 */
static bool json_field(const char *src, size_t len, const char *key, char *out,
		       size_t out_cap)
{
	char pat[24];
	int patlen = snprintk(pat, sizeof(pat), "\"%s\"", key);

	if (patlen <= 0) {
		return false;
	}
	for (size_t i = 0; i + (size_t)patlen < len; i++) {
		if (memcmp(&src[i], pat, (size_t)patlen) != 0) {
			continue;
		}
		size_t j = i + (size_t)patlen;

		while (j < len && src[j] != ':') {
			j++;
		}
		while (j < len && src[j] != '"') {
			j++;
		}
		if (j >= len) {
			return false;
		}
		j++; /* past the opening quote */
		size_t n = 0;

		while (j < len && src[j] != '"' && n + 1 < out_cap) {
			out[n++] = src[j++];
		}
		out[n] = '\0';
		return n > 0;
	}
	return false;
}

void impulse_app_apply_wifi_credentials(const uint8_t *json, size_t len)
{
	char ssid[IMPULSE_WIFI_SSID_MAX + 1] = {0};
	char psk[IMPULSE_WIFI_PSK_MAX + 1] = {0};

	if (!json_field((const char *)json, len, "ssid", ssid, sizeof(ssid))) {
		LOG_WRN("wifi credentials: no ssid field");
		return;
	}
	(void)json_field((const char *)json, len, "password", psk, sizeof(psk));

	/* Never log the passphrase, or its length — both are the kind of thing
	 * that ends up in a pasted bug report. */
	LOG_INF("wifi credentials received for \"%s\"", ssid);
	(void)impulse_wifi_set_credentials(ssid, psk);
}

#if defined(CONFIG_IMPULSE_ROLE_WATCH)
void impulse_app_apply_anchor_ips(const uint8_t *buf, size_t len)
{
	static struct impulse_anchor_ip tbl[8];

	if (len < 1U) {
		return;
	}
	uint8_t n = buf[0];
	size_t need = 1U + (size_t)n * (IMPULSE_UUID_LEN + 4U + 4U);

	if (n > ARRAY_SIZE(tbl) || len < need) {
		LOG_WRN("anchor IP table rejected: %u entries, %u bytes",
			n, (unsigned)len);
		return;
	}

	const uint8_t *p = &buf[1];

	for (uint8_t i = 0; i < n; i++) {
		memcpy(tbl[i].uuid, p, IMPULSE_UUID_LEN);
		p += IMPULSE_UUID_LEN;
		memcpy(tbl[i].ip, p, 4);
		p += 4;
		p += 4; /* timestamp: accepted, not used */
		LOG_INF("anchor ip[%u]: %02x%02x%02x%02x-... -> %u.%u.%u.%u", i,
			tbl[i].uuid[0], tbl[i].uuid[1], tbl[i].uuid[2],
			tbl[i].uuid[3], tbl[i].ip[0], tbl[i].ip[1],
			tbl[i].ip[2], tbl[i].ip[3]);
	}
	(void)impulse_storage_save_anchor_ips(tbl, n);
}
#endif /* CONFIG_IMPULSE_ROLE_WATCH */

#endif /* CONFIG_IMPULSE_NET */

void impulse_app_get_status(struct impulse_app_status *out)
{
	memset(out, 0, sizeof(*out));

	/*
	 * The WIRE encoding is 0=dormant, 1=enforcement, 2=dormant_sleep
	 * (§5.6), which is NOT the order of enum impulse_state. Casting the
	 * enum straight onto the wire reported DORMANT as "enforcement" — the
	 * app would have shown a commitment being enforced on an idle watch.
	 * Found by phone_sim on the first real read, 2026-08-30.
	 */
	switch (g_enf.state) {
	case IMPULSE_STATE_ENFORCEMENT:
		out->activity_state = 1U;
		break;
	case IMPULSE_STATE_DORMANT_SLEEP:
		out->activity_state = 2U;
		break;
	case IMPULSE_STATE_DORMANT:
	case IMPULSE_STATE_UNPAIRED:
	default:
		out->activity_state = 0U;
		break;
	}
	out->bt_connected =
#if defined(CONFIG_IMPULSE_BLE)
		impulse_ble_connected() ? 1U : 0U;
#else
		0U;
#endif
	/* TODO(chunk I): the WM02C is not populated on this board, so WiFi is
	 * reported as down rather than unknown. getOnWifi therefore cannot be
	 * satisfied, which is the fail-closed direction. */
	out->wifi_connected = 0U;
	out->worn = g_enf.worn ? 1U : 0U;
	out->battery_pct = g_battery_pct;
	out->condition_met = g_enf.condition_met ? 1U : 0U;
	out->schedule_crc = g_schedule.crc32;
	if (g_enf.active != NULL) {
		memcpy(out->active_event_id, g_enf.active->id,
		       IMPULSE_UUID_LEN);
	}
}

uint8_t impulse_app_apply_schedule(const uint8_t *blob, size_t len)
{
	/*
	 * STATIC, not on the stack. struct impulse_schedule is ~14 kB (64
	 * events at 224 bytes) and this runs on the system workqueue, whose
	 * stack is a few kB — putting it on the stack overflowed sysworkq the
	 * instant a real schedule arrived over BLE. Found on hardware,
	 * 2026-08-30, and only visible because logging is in immediate mode:
	 * a deferred backend loses the fault message.
	 *
	 * Safe as static because schedule transfer is single-threaded: one
	 * BEGIN..END at a time, always on the workqueue.
	 */
	static struct impulse_schedule proposed;
	struct impulse_diff_report report;
	uint8_t verdict;

	if (impulse_blob_parse(blob, len, &proposed) != IMPULSE_BLOB_OK) {
		LOG_WRN("schedule rejected: parse failure");
		return IMPULSE_END_FAILED;
	}

	/*
	 * Straight into the §9.3 gate. This call IS the product's self-binding
	 * guarantee: the app proposes, the watch disposes. There is deliberately
	 * no path from a BLE write to the stored schedule that goes around it.
	 */
	verdict = impulse_integrity_apply_push(&g_integrity, &g_schedule,
					       &proposed, g_enf.active,
					       now_utc(), g_tz_offset_minutes,
					       &report);

	LOG_INF("schedule push: verdict=0x%02x applied=%u quarantined=%u "
		"dropped=%u", verdict, report.applied, report.quarantined,
		report.dropped_queue_full);

	if (verdict != IMPULSE_END_REJECTED) {
		g_schedule.crc32 = impulse_crc32(blob, len);
		(void)impulse_storage_save_schedule(&g_schedule);
		(void)impulse_storage_save_integrity(&g_integrity);
		g_force_poll = true;
	}
	return verdict;
}

uint8_t impulse_app_apply_settings(uint8_t disconnected_is_dormant,
				   uint8_t away_is_dormant,
				   int16_t tz_offset_minutes,
				   uint16_t settle_window_min)
{
	uint8_t resp = IMPULSE_END_ACCEPTED;

	/* §9.7: a timezone change that would push the current local time
	 * outside the ACTIVE window ends that window early, so it is refused. */
	if (tz_offset_minutes != g_tz_offset_minutes &&
	    !impulse_integrity_time_change_allowed(g_enf.active, now_utc(),
						   g_tz_offset_minutes,
						   now_utc(),
						   tz_offset_minutes)) {
		LOG_WRN("timezone change rejected: would end the active window");
		return 0x02;
	}

	/* §9.8: clamp, then gate. Growing the settle window is a longer
	 * free-edit period and therefore a loosening. */
	if (settle_window_min < IMPULSE_SETTLE_WINDOW_FLOOR_MIN) {
		settle_window_min = IMPULSE_SETTLE_WINDOW_FLOOR_MIN;
	}
	if (settle_window_min > IMPULSE_SETTLE_WINDOW_CEIL_MIN) {
		settle_window_min = IMPULSE_SETTLE_WINDOW_CEIL_MIN;
	}
	if (settle_window_min > g_integrity.settle_window_min) {
		/* TODO(§9.8): should be queued as a pending SETTING change
		 * rather than simply refused. Refusing is the SAFE direction —
		 * it binds harder than the user asked — so it is acceptable
		 * until the pending-setting apply path exists. */
		LOG_WRN("settle window increase not applied (loosening)");
		resp = IMPULSE_END_QUARANTINED;
	} else {
		g_integrity.settle_window_min = settle_window_min;
	}

	/* TODO(§5.1.1): the dormancy flags are accepted and stored but nothing
	 * consumes them yet — the state machine has no connectivity-driven
	 * dormancy. false->true is a tightening and immediate; true->false is a
	 * loosening and must be gated once they do something. */
	ARG_UNUSED(disconnected_is_dormant);
	ARG_UNUSED(away_is_dormant);

	g_tz_offset_minutes = tz_offset_minutes;
	(void)impulse_storage_save_settings(g_tz_offset_minutes);
	(void)impulse_storage_save_integrity(&g_integrity);
	g_force_poll = true;
	return resp;
}

uint8_t impulse_app_apply_time(int64_t utc_seconds, int16_t tz_offset_minutes)
{
	if (!impulse_integrity_time_change_allowed(g_enf.active, now_utc(),
						   g_tz_offset_minutes,
						   utc_seconds,
						   tz_offset_minutes)) {
		LOG_WRN("time write rejected: would end the active window");
		return 0x02;
	}

	g_utc_base = utc_seconds;
	g_utc_base_uptime_ms = k_uptime_get();
	g_tz_offset_minutes = tz_offset_minutes;
	(void)impulse_storage_save_settings(g_tz_offset_minutes);
	g_force_poll = true;
	return IMPULSE_END_ACCEPTED;
}

uint8_t impulse_app_pass_spend(const uint8_t *event_id, uint32_t date_yyyymmdd,
			       uint8_t *remaining_out)
{
	bool ok = impulse_integrity_spend_pass(&g_integrity, event_id,
					       date_yyyymmdd, &g_schedule,
					       remaining_out);

	if (ok) {
		(void)impulse_storage_save_schedule(&g_schedule);
		(void)impulse_storage_save_integrity(&g_integrity);
		g_force_poll = true;
		return IMPULSE_END_ACCEPTED;
	}
	return 0x02; /* exhausted */
}

uint8_t impulse_app_pass_set_allowance(uint8_t allowance)
{
	if (allowance > g_integrity.pass_allowance) {
		/* Raising the allowance is a loosening (§9.6). */
		LOG_WRN("pass allowance raise not applied (loosening)");
		return IMPULSE_END_QUARANTINED;
	}
	g_integrity.pass_allowance = allowance;
	(void)impulse_storage_save_integrity(&g_integrity);
	return IMPULSE_END_ACCEPTED;
}

void impulse_app_pass_read(uint8_t *allowance, uint8_t *remaining)
{
	uint8_t used = g_integrity.pass_spend_count;

	*allowance = g_integrity.pass_allowance;
	*remaining = (used >= g_integrity.pass_allowance)
			     ? 0U
			     : (uint8_t)(g_integrity.pass_allowance - used);
}

size_t impulse_app_pending_payload(uint8_t *buf, size_t cap)
{
	uint64_t now_s = impulse_elapsed_now_s();
	size_t pos = 1;
	uint8_t n = 0;

	if (cap < 1U) {
		return 0;
	}

	for (size_t i = 0; i < CONFIG_IMPULSE_PENDING_QUEUE_MAX; i++) {
		const struct impulse_pending *p = &g_integrity.pending[i];
		uint32_t secs;

		if (!p->in_use || (pos + 21U) > cap) {
			continue;
		}

		memcpy(&buf[pos], p->target_id, IMPULSE_UUID_LEN);
		pos += IMPULSE_UUID_LEN;

		/* Wire change types: 0 delete, 1 loosen-modify, 2 negate-day,
		 * 3 setting change (§9.5). */
		buf[pos++] = (p->kind == IMPULSE_PENDING_DELETE)    ? 0U
			     : (p->kind == IMPULSE_PENDING_SETTING) ? 3U
			     : (p->proposed.negate)                 ? 2U
								    : 1U;

		secs = (p->apply_after_elapsed_s > now_s)
			       ? (uint32_t)(p->apply_after_elapsed_s - now_s)
			       : 0U;
		buf[pos++] = (uint8_t)(secs & 0xFFU);
		buf[pos++] = (uint8_t)((secs >> 8) & 0xFFU);
		buf[pos++] = (uint8_t)((secs >> 16) & 0xFFU);
		buf[pos++] = (uint8_t)((secs >> 24) & 0xFFU);
		n++;
	}

	buf[0] = n;
	return pos;
}

#if defined(CONFIG_IMPULSE_BRINGUP_SHELL)

void impulse_app_set_time(int64_t utc_seconds, int16_t tz_offset_minutes)
{
	g_utc_base = utc_seconds;
	g_utc_base_uptime_ms = k_uptime_get();
	g_clock_restored = false;
	/* Persist immediately. Waiting for the periodic save would lose an app
	 * sync to a reset in the following minute — and the sync is the only
	 * authoritative clock the device ever gets. */
	if (utc_seconds > IMPULSE_CLOCK_SANITY_FLOOR) {
		(void)impulse_storage_save_wall_clock(utc_seconds);
	}
	g_tz_offset_minutes = tz_offset_minutes;
	(void)impulse_storage_save_settings(g_tz_offset_minutes);
}

int64_t impulse_app_now_utc(void)
{
	return now_utc();
}

void impulse_app_force_worn(int8_t mode)
{
	g_worn_override = mode;
}

int impulse_app_install_demo(uint8_t criteria, uint8_t profile)
{
	struct impulse_date today;
	uint16_t minute;
	struct impulse_event *e = &g_schedule.events[0];

	impulse_local_from_utc(now_utc(), g_tz_offset_minutes, &today, &minute);

	memset(&g_schedule, 0, sizeof(g_schedule));
	memset(e, 0, sizeof(*e));
	for (int i = 0; i < IMPULSE_UUID_LEN; i++) {
		e->id[i] = (uint8_t)(0xD0 + i);
		e->anchor_id[i] = (uint8_t)(0xA0 + i);
	}
	e->recurrence = IMPULSE_RECUR_DAILY;
	/* A window that already covers the current minute. Note that entry is
	 * matched on the exact local minute and a window whose minute has
	 * begun is never entered (§5.3) — so the start must be strictly
	 * earlier than now, not equal to it. */
	e->start_time = (minute > 0U) ? (uint16_t)(minute - 1U) : 0U;
	e->end_time = (uint16_t)((minute + 30U > 1439U) ? 1439U : minute + 30U);
	e->criteria = criteria;
	e->profile = profile;
	/* Bench demo escalates to anchors, so the §5.5.1 path is exercisable
	 * without hand-building a schedule: the same synthetic UUID is both the
	 * proximity target and the single beepAnchor. A real anchor ignores it
	 * unless the UUID matches its own, which is the guard working. */
	e->anchor_profile = IMPULSE_ANCHOR_PROFILE_MEDIUM;
	e->beep_anchor_count = 1U;
	memcpy(e->beep_anchors[0], e->anchor_id, IMPULSE_UUID_LEN);
	e->has_anchor_id = true;
	e->reference_date = now_utc();
	g_schedule.count = 1;

	/* Force a re-evaluation on the next loop pass. */
	impulse_enforcement_exit(&g_enf);
	return 0;
}

int impulse_app_save_all(void)
{
	/* Report the FIRST failure rather than OR-ing error codes together:
	 * bitwise-OR of negative errnos produces a meaningless number, which
	 * is how a -EINVAL from the schedule write got past review. */
	int err = impulse_storage_save_schedule(&g_schedule);

	if (err == 0) {
		err = impulse_storage_save_integrity(&g_integrity);
	}
	if (err == 0) {
		err = impulse_storage_save_elapsed_base(
			impulse_elapsed_now_s());
	}
	return err;
}

void impulse_app_report(void)
{
	LOG_INF("schedule=%u event(s) tz=%d state=%d cond_met=%d",
		g_schedule.count, g_tz_offset_minutes, (int)g_enf.state,
		(int)g_enf.condition_met);
	if (g_schedule.count > 0U) {
		LOG_INF("  ev[0] start=%u end=%u crit=%u prof=%u",
			g_schedule.events[0].start_time,
			g_schedule.events[0].end_time,
			g_schedule.events[0].criteria,
			g_schedule.events[0].profile);
	}
	LOG_INF("  buttons: short=%u long=%u very_long=%u  battery=%u%%  worn_delta=%d mV",
		g_btn_short, g_btn_long, g_btn_very_long, g_battery_pct,
		g_worn_delta_mv);
	LOG_INF("  clock: utc=%lld restored_from_flash=%d (restored means the "
		"app has NOT synced since boot)",
		now_utc(), (int)g_clock_restored);
	LOG_INF("  charger: raw_pin=%d charging=%d",
		gpio_pin_get_raw(charger.port, charger.pin),
		gpio_pin_get_dt(&charger));
	LOG_INF("  prox: verdict=%d have=%d abstain_run=%u failsafe=%d",
		(int)g_enf.prox.verdict, (int)g_enf.prox.have_verdict,
		g_enf.prox.abstain_run,
		(int)impulse_prox_in_failsafe(&g_enf.prox));
}

#endif /* CONFIG_IMPULSE_BRINGUP_SHELL */


#if defined(CONFIG_IMPULSE_SYSWORKQ_WATCHDOG)
/*
 * SYSTEM WORKQUEUE LIVENESS.
 *
 * The system workqueue can deadlock permanently and take no thread down with
 * it. On a BLE disconnect the host runs its connection teardown there; for a
 * Channel Sounding link that reaches bt_ras_rrsp_free(), which calls
 * k_work_queue_drain(&rrsp_wq) and waits K_FOREVER. The k_work_cancel() just
 * above it does not stop an already-RUNNING handler, so a RAS reflector caught
 * mid-send keeps running and blocks forever on an ATT buffer that a dead
 * connection will never return.
 *
 * What makes this worth a watchdog rather than a fix is how it presents.
 * Every other thread is fine — main loop, logging, heartbeat, the radios — so
 * the device reports itself healthy in every way it knows how to report, while
 * being unreachable over BLE and unable to run a single deferred work item.
 * It read as "the anchor keeps dying while powered" for two days.
 *
 * The probe is a bare work item. If the system queue is running at all it will
 * be handled within milliseconds; if it is wedged, last_run stops advancing and
 * nothing else here has to understand why. The connection object is leaked
 * (its ref never drops), so no in-process recovery exists — a cold reboot is
 * the only way out, and it is cheap: the schedule is in NVS and the clock is
 * persisted.
 */
static int64_t g_sysworkq_last_run;

static void sysworkq_probe_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	g_sysworkq_last_run = k_uptime_get();
}

static K_WORK_DEFINE(g_sysworkq_probe, sysworkq_probe_handler);
#endif /* CONFIG_IMPULSE_SYSWORKQ_WATCHDOG */

/*
 * STATUS RING.
 *
 * Solid red while the user is not complying with a commitment being enforced;
 * flashing green while a window is open and they are. Dark otherwise — v3
 * §6.2 keeps the ring off in DORMANT, and this does not change that.
 *
 * On the watch that is the enforcement verdict itself. The anchor does not
 * enforce (v3 §4.2), so on the anchor red means its removal alarm is sounding
 * and green means a window involving it is open and quiet.
 *
 * The ring is only written when the wanted frame changes. A write is a full
 * SPI transfer of all twelve pixels and this runs every loop pass; resending
 * an identical frame ten times a second would be pure waste on a bus the
 * clock display also uses.
 *
 * PRIORITY (v3 §6.2): status beats the clock. During a window a short press
 * does not draw the time, and a clock already showing when a window begins is
 * cancelled and overwritten — its timer would otherwise clear the status frame
 * a few seconds later. Outside a window the clock owns the ring while its
 * timer runs, and dark is re-asserted once it has cleared.
 */
#define STATUS_RING_BRIGHTNESS 40
#define STATUS_RING_FLASH_MS   500

enum status_ring_frame {
	STATUS_RING_OFF,
	STATUS_RING_RED,
	STATUS_RING_GREEN_ON,
	STATUS_RING_GREEN_OFF,
};

static void status_ring_wanted(bool *alarm, bool *compliant)
{
	*alarm = false;
	*compliant = false;

#if defined(CONFIG_IMPULSE_ROLE_ANCHOR)
	if (impulse_anchor_beep_state()->active) {
		*alarm = true;
	} else if (impulse_app_anchor_in_active_event()) {
		*compliant = true;
	}
#else
	if (g_enf.state == IMPULSE_STATE_ENFORCEMENT) {
		if (g_enf.condition_met) {
			*compliant = true;
		} else {
			*alarm = true;
		}
	}
#endif
}

static bool status_ring_active(void)
{
	bool alarm;
	bool compliant;

	status_ring_wanted(&alarm, &compliant);
	return alarm || compliant;
}

static void update_status_ring(int64_t now_ms)
{
	static int last = -1;
	bool alarm;
	bool compliant;
	int want;

	status_ring_wanted(&alarm, &compliant);

	if (k_work_delayable_busy_get(&clock_off_work) != 0) {
		if (!alarm && !compliant) {
			last = -1;
			return;
		}
		/* A window began while the clock was up: status wins. If the
		 * clear handler is already running it may still wipe the ring,
		 * so redraw on a later pass rather than trusting the cache. */
		if (k_work_cancel_delayable(&clock_off_work) != 0) {
			last = -1;
			return;
		}
		last = -1;
	}

	if (alarm) {
		want = STATUS_RING_RED;
	} else if (compliant) {
		want = ((now_ms % (2 * STATUS_RING_FLASH_MS)) <
			STATUS_RING_FLASH_MS) ? STATUS_RING_GREEN_ON
					      : STATUS_RING_GREEN_OFF;
	} else {
		want = STATUS_RING_OFF;
	}

	if (want == last) {
		return;
	}

	switch (want) {
	case STATUS_RING_RED:
		(void)impulse_led_ring_set_all(STATUS_RING_BRIGHTNESS, 0, 0);
		break;
	case STATUS_RING_GREEN_ON:
		(void)impulse_led_ring_set_all(0, STATUS_RING_BRIGHTNESS, 0);
		break;
	default:
		(void)impulse_led_ring_clear();
		break;
	}
	last = want;
}

int main(void)
{
	int err;

	LOG_INF("Impulse firmware (%s) on %s", ROLE_NAME, CONFIG_BOARD_TARGET);

	err = impulse_outputs_init();
	if (err != 0) {
		LOG_ERR("outputs init failed (%d)", err);
	}

	/* Before anything else can fail: say how the last boot ended. */
	impulse_fatal_report_boot();

	err = impulse_led_ring_init();
	if (err != 0) {
		LOG_ERR("led ring init failed (%d)", err);
	}

	err = impulse_adc_init();
	if (err != 0) {
		LOG_ERR("adc init failed (%d)", err);
	}

	err = impulse_buttons_init(on_button);
	if (err != 0) {
		LOG_ERR("buttons init failed (%d)", err);
	}

	err = impulse_storage_init();
	if (err != 0) {
		LOG_ERR("storage init failed (%d)", err);
	}

#if defined(CONFIG_IMPULSE_NET)
	impulse_wifi_init();
	/* Bring the link up on the anchor unconditionally: it is mains-powered
	 * and must be reachable whenever a watch decides to escalate, which it
	 * cannot predict. The watch asks for the link per window instead. */
#if defined(CONFIG_IMPULSE_ROLE_ANCHOR)
	impulse_wifi_request(true);
	(void)impulse_udp_listener_start();
#endif
#endif

#if defined(CONFIG_IMPULSE_CS_BACKEND) || defined(CONFIG_IMPULSE_CS_REFLECTOR)
	/*
	 * Start the real ranging engine. Until it produces a fresh burst every
	 * measurement abstains, which is the correct behaviour rather than a
	 * degraded one: stayNear fails CLOSED and getAway fails open (§4.5).
	 */
	err = impulse_cs_backend_start();
	if (err != 0) {
		LOG_ERR("CS backend failed to start (%d)", err);
	} else {
		LOG_INF("CS backend: %s", impulse_cs_backend()->name);
	}
#else
	LOG_WRN("CS backend is STUBBED — every measurement abstains "
		"(build with cs.conf to enable the radio)");
#endif

#if defined(CONFIG_IMPULSE_ROLE_ANCHOR)
	impulse_anchor_init();
	err = impulse_servo_init();
	if (err != 0) {
		LOG_ERR("servo init failed (%d)", err);
	}
#endif

	impulse_integrity_init(&g_integrity);
	impulse_enforcement_init(&g_enf);

#if defined(CONFIG_IMPULSE_BLE)
	err = impulse_ble_start();
	if (err != 0) {
		LOG_ERR("BLE failed to start (%d) — the app cannot connect",
			err);
	}
#else
	LOG_WRN("BLE disabled: no app can configure this device");
#endif

	(void)impulse_storage_load_settings(&g_tz_offset_minutes);
	(void)impulse_storage_load_schedule(&g_schedule);
	(void)impulse_storage_load_integrity(&g_integrity);

	/* Seal any event whose settle record is missing (storage desync) so it
	 * cannot be loosened instantly. See integrity.c. */
	impulse_integrity_seal_unknown(&g_integrity, &g_schedule);

	{
		uint64_t base = 0;

		(void)impulse_storage_load_elapsed_base(&base);
		impulse_integrity_set_elapsed_base(base);
	}

	{
		/* Boot counter: says outright whether this board is resetting,
		 * which a wiped RTT buffer can never tell you. */
		uint32_t boots = 0;

		(void)impulse_storage_load_boot_count(&boots);
		boots++;
		(void)impulse_storage_save_boot_count(boots);
		LOG_WRN("BOOT #%u", boots);
	}

	{
		/*
		 * Restore the wall clock. A watch that boots at 1970 matches no
		 * window and enforces nothing, silently — so a reset, or a flat
		 * battery overnight, used to cancel a commitment outright.
		 *
		 * The restored value is deliberately treated as a LOWER BOUND,
		 * not as the truth: it lags by up to the save interval plus the
		 * time the device spent powered off, so a commitment can start
		 * late but never early. The sanity floor rejects an
		 * uninitialised or corrupt record rather than trusting it.
		 */
		int64_t saved = 0;

		(void)impulse_storage_load_wall_clock(&saved);
		if (g_retained_clock.magic == RETAINED_CLOCK_MAGIC &&
		    g_retained_clock.magic_inv == ~RETAINED_CLOCK_MAGIC &&
		    g_retained_clock.utc > IMPULSE_CLOCK_SANITY_FLOOR &&
		    g_retained_clock.utc >= saved) {
			/* Base at the boot instant: the retained second was the
			 * last one before the reset, and everything since boot is
			 * already counted by k_uptime_get(). */
			g_utc_base = g_retained_clock.utc;
			g_utc_base_uptime_ms = 0;
			g_clock_restored = true;
			LOG_WRN("clock RESTORED from retained RAM after a warm "
				"reset (utc=%lld, ~1 s accuracy)",
				g_retained_clock.utc);
		} else if (saved > IMPULSE_CLOCK_SANITY_FLOOR) {
			g_utc_base = saved;
			g_utc_base_uptime_ms = k_uptime_get();
			g_clock_restored = true;
			LOG_WRN("clock RESTORED from storage (utc=%lld). It lags "
				"by up to %d s plus off-time — the app re-syncing "
				"supersedes it.",
				saved, IMPULSE_CLOCK_SAVE_INTERVAL_S);
		} else {
			LOG_WRN("NO STORED CLOCK — the watch believes it is 1970 "
				"and will enforce NOTHING until the app sets the "
				"time.");
		}
	}

	LOG_INF("schedule: %u event(s), tz offset %d min", g_schedule.count,
		g_tz_offset_minutes);

	/*
	 * §9.4: promote any pending loosening whose 24 h has elapsed. Runs on
	 * boot as well as on every wake, with no app involvement — the app may
	 * have been uninstalled and the promotion must still happen.
	 */
	uint16_t promoted = impulse_integrity_promote(
		&g_integrity, &g_schedule, now_utc(), g_tz_offset_minutes);

	if (promoted > 0U) {
		LOG_INF("promoted %u pending change(s)", promoted);
		(void)impulse_storage_save_schedule(&g_schedule);
		(void)impulse_storage_save_integrity(&g_integrity);
	}

	report_lfclk();

#if defined(CONFIG_IMPULSE_BRINGUP_SHELL)
	/*
	 * One-shot SAADC sweep at boot. Printed rather than driven from the
	 * shell because the shell and the log backend share RTT buffer 0 and
	 * interfere; the log is the reliable channel on this board.
	 *
	 * The AIN-to-pin mapping for this part is in no document we have, so it
	 * is identified from live signals: the battery divider sits at Vbat/2
	 * and reads ~1800-2100 mV, and the IR receiver is whichever channel
	 * MOVES when the emitter is toggled.
	 */
	{
		int32_t dark[8], lit[8];

		impulse_ir_emitter_set(false);
		k_msleep(5);
		for (uint8_t ch = 0; ch < 8U; ch++) {
			if (impulse_adc_read_mv(ch, &dark[ch]) != 0) {
				dark[ch] = INT32_MIN;
			}
		}
		impulse_ir_emitter_set(true);
		k_msleep(5);
		for (uint8_t ch = 0; ch < 8U; ch++) {
			if (impulse_adc_read_mv(ch, &lit[ch]) != 0) {
				lit[ch] = INT32_MIN;
			}
		}
		impulse_ir_emitter_set(false);

		LOG_INF("SAADC sweep (ch: off_mV on_mV delta)");
		for (uint8_t ch = 0; ch < 8U; ch++) {
			if (dark[ch] == INT32_MIN) {
				LOG_INF("  AIN%-2u unreadable", ch);
			} else {
				LOG_INF("  AIN%-2u %6d %6d %+6d", ch, dark[ch],
					lit[ch], lit[ch] - dark[ch]);
			}
		}
	}
#endif

	impulse_hw_watchdog_start();

	int64_t last_tick = k_uptime_get();
	int64_t last_poll = 0;
	int64_t last_beat = 0;
	int64_t last_elapsed_save = 0;
	int64_t last_clock_save = 0;
	int64_t last_sense = 0;

#if defined(CONFIG_IMPULSE_SYSWORKQ_WATCHDOG)
	int64_t last_sysworkq_probe = 0;

	g_sysworkq_last_run = k_uptime_get();
#endif

	while (1) {
		int64_t now_ms = k_uptime_get();
		uint32_t dt = (uint32_t)(now_ms - last_tick);

		last_tick = now_ms;
		impulse_hw_watchdog_feed();

		if (g_utc_base > IMPULSE_CLOCK_SANITY_FLOOR) {
			g_retained_clock.utc = now_utc();
			g_retained_clock.magic = RETAINED_CLOCK_MAGIC;
			g_retained_clock.magic_inv = ~RETAINED_CLOCK_MAGIC;
		}

#if defined(CONFIG_IMPULSE_SYSWORKQ_WATCHDOG)
		{
			const int64_t stall_ms =
				(int64_t)CONFIG_IMPULSE_SYSWORKQ_STALL_S * 1000;

			/* Probe four times per stall window, so a single
			 * unlucky sample cannot trip the reboot. */
			if ((now_ms - last_sysworkq_probe) >= stall_ms / 4) {
				last_sysworkq_probe = now_ms;
				(void)k_work_submit(&g_sysworkq_probe);
			}

			if ((now_ms - g_sysworkq_last_run) > stall_ms) {
				LOG_ERR("system workqueue stalled for %lld ms "
					"— rebooting",
					now_ms - g_sysworkq_last_run);
				/* Give the logger a chance to drain. */
				k_sleep(K_MSEC(200));
				sys_reboot(SYS_REBOOT_COLD);
			}
		}
#endif

#if defined(CONFIG_IMPULSE_ROLE_ANCHOR)
		/*
		 * The anchor does not enforce (v3 §4.2) — it beeps when told
		 * the watch was removed, actuates the strap lock, and answers
		 * ranging. Its only autonomous timing job is the beep pattern
		 * and noticing when a window involving it begins.
		 */
		{
			static bool was_in_event;
			bool in_event = impulse_app_anchor_in_active_event();

			if (in_event && !was_in_event) {
				impulse_anchor_on_window_start();
			}
			was_in_event = in_event;

			(void)impulse_anchor_beep_tick(
				impulse_anchor_beep_state(), dt,
				g_max_beep_minutes);
		}
#else
		refresh_active_event();

#if defined(CONFIG_IMPULSE_CS_BACKEND)
		/* Range only while a commitment is being enforced. Free-running
		 * CS at ~10 Hz is the largest avoidable draw in the design, and
		 * a dormant watch has nothing to do with a distance. No-op
		 * unless the state actually changes. */
		impulse_cs_set_ranging(g_enf.state == IMPULSE_STATE_ENFORCEMENT);
#endif

		if (g_enf.state == IMPULSE_STATE_ENFORCEMENT) {
			uint32_t interval =
				impulse_enforcement_poll_interval_s(&g_enf);

			if (g_force_poll ||
			    (now_ms - last_poll) >= (int64_t)interval * 1000) {
				g_force_poll = false;
				last_poll = now_ms;

				/*
				 * TODO(chunk G): run a real CS procedure here.
				 * The stub backend always fails, which drives
				 * the abstention path — so with no backend a
				 * stayNear commitment fails CLOSED rather than
				 * silently reporting compliance. That is the
				 * intended behaviour for an unfinished build.
				 */
				if (impulse_criteria_is_anchor_based(
					    g_enf.active->criteria)) {
					struct impulse_cs_measurement m;

					impulse_cs_backend()->measure(
						g_enf.active->anchor_id, &m);
					impulse_prox_ingest(&g_enf.prox, &m);
				}

				/* TODO(chunk I): real WiFi state and dock
				 * status. Passing "not connected, docked"
				 * keeps getOffWifi satisfied and getOnWifi
				 * unsatisfied, which is the fail-closed
				 * direction for the criterion that binds. */
				(void)impulse_enforcement_check_condition(
					&g_enf, now_utc(), false, NULL, true);

#if defined(CONFIG_IMPULSE_ROLE_WATCH) && \
	(defined(CONFIG_IMPULSE_CS_BACKEND) || defined(CONFIG_IMPULSE_NET))
				/*
				 * §5.5.1 RE-ASSERTION. WATCH_REMOVED is sent on
				 * EVERY poll while the watch stays unworn and
				 * out of donning grace — not once on the edge.
				 *
				 * The single announcement was lost three ways,
				 * all reachable and one observed on the bench:
				 * no association at the instant of the edge; a
				 * dropped datagram (UDP, unacknowledged); and
				 * the anchor stopping on its own, because
				 * max_beep_minutes expired or it rebooted
				 * mid-window with nothing left to tell it to
				 * start again.
				 *
				 * Idempotent by construction — the anchor
				 * re-checks every one of its guards per packet
				 * — so this can only restart an alarm that
				 * should already be running. Cost is one
				 * 33-byte datagram per poll.
				 */
				bool in_grace =
					g_enf.grace_deadline_utc != 0 &&
					now_utc() < g_enf.grace_deadline_utc;

				if (!g_enf.worn && !in_grace) {
					escalate_to_anchors(
						IMPULSE_CMD_WATCH_REMOVED,
						g_enf.active);
				}
#endif
			}

			/*
			 * Drive the pins from the profile state EVERY pass,
			 * not only when tick() reports a change.
			 *
			 * tick() returns "did the output state change", and for
			 * a CONTINUOUS step it returns false forever by design —
			 * the caller is expected to stop it, not to be told
			 * about it. Gating the GPIO writes on that return value
			 * therefore meant every profile whose step is
			 * CONTINUOUS never drove its outputs AT ALL: the run
			 * struct said motor_on = 1 while the pad sat low.
			 *
			 * That is all three STRICT profiles — so the strictest
			 * enforcement level in the product was the only one
			 * that did nothing, and the duty-cycled profiles
			 * masked it because their step changes happened to
			 * produce the edge this needed. Observed 2026-09-12:
			 * cond_met=0, profile_run.motor_on=1, P3 OUT bit 3 low
			 * across 120 samples.
			 *
			 * Writing unconditionally is two GPIO writes per 100 ms
			 * tick and makes the whole class of bug unreachable.
			 */
			(void)impulse_enforcement_tick(&g_enf, dt);
			impulse_motor_set(g_enf.profile_run.motor_on);
			impulse_buzzer_set(g_enf.profile_run.buzzer_on);
		} else {
			/*
			 * NOT ENFORCING => NOTHING DRIVEN. This is a
			 * belt-and-braces sweep, and it is here because the
			 * invariant used to live only in the caller.
			 *
			 * impulse_profile_stop() clears the profile_run STRUCT
			 * but touches no pin, and the pins were cleared only on
			 * the "window ended" path. Any other exit — a schedule
			 * replaced mid-burst — therefore left the vibration
			 * motor LATCHED ON FOREVER. Observed on hardware
			 * 2026-09-10: P3 OUT read 0x08 continuously for 36 s
			 * after the schedule was swapped during a burst, where
			 * the criterion in force should have produced silence.
			 *
			 * A stuck motor is a flat battery and a device that
			 * buzzes until it dies; with the buzzer compiled in it
			 * is also a device that screams in public. Clearing
			 * every pass costs two GPIO writes per 100 ms tick and
			 * makes the failure unreachable from ANY exit path,
			 * including ones not yet written.
			 */
			impulse_motor_set(false);
			impulse_buzzer_set(false);
		}
#endif /* CONFIG_IMPULSE_ROLE_ANCHOR */

		update_status_ring(now_ms);

		/*
		 * Battery and worn sampling, once a second.
		 *
		 * Worn detection drives WATCH_REMOVED, the donning grace and
		 * the Sunrise Lock flow (§5.2, §5.4.4), so it has to run
		 * whether or not a window is active.
		 */
		if ((now_ms - last_sense) >= 1000) {
			int32_t mv;

			last_sense = now_ms;

			if (impulse_battery_mv(&mv) == 0) {
				/*
				 * Linear 3300..4200 mV -> 0..100%.
				 * TODO(hw): a LiPo discharge curve is not
				 * linear; this over-reports in the middle and
				 * under-reports near empty. Good enough to
				 * show a bar, not good enough to gate the
				 * §5.5.3 anchor-repair decision on.
				 */
				int32_t pct = (mv - 3300) * 100 / 900;

				if (pct < 0) {
					pct = 0;
				}
				if (pct > 100) {
					pct = 100;
				}
				g_battery_pct = (uint8_t)pct;
				g_battery_mv = mv;
			}

			if (g_worn_override >= 0) {
				bool forced = (g_worn_override != 0);

				if (forced != g_enf.worn) {
					LOG_WRN("worn FORCED -> %d (bench override)",
						(int)forced);
					impulse_enforcement_set_worn(
						&g_enf, forced, now_utc());
					g_force_poll = true;
#if defined(CONFIG_IMPULSE_ROLE_WATCH) && \
	(defined(CONFIG_IMPULSE_CS_BACKEND) || defined(CONFIG_IMPULSE_NET))
					if (g_enf.state ==
					    IMPULSE_STATE_ENFORCEMENT) {
						escalate_to_anchors(
							forced ? IMPULSE_CMD_WATCH_WORN
							       : IMPULSE_CMD_WATCH_REMOVED,
							g_enf.active);
					}
#endif
#if defined(CONFIG_IMPULSE_BLE)
					impulse_ble_notify_status();
#endif
				}
			} else if (impulse_worn_sample(&g_worn_delta_mv) == 0) {
				/* Hysteresis: cross the high mark to become
				 * worn, fall under the low mark to be released,
				 * hold the previous state in between. A single
				 * threshold chattered once per second whenever
				 * the delta sat near it, and every flip drives
				 * the donning grace and WATCH_REMOVED. */
				bool worn = g_enf.worn;

				if (g_worn_delta_mv >=
				    CONFIG_IMPULSE_WORN_THRESHOLD_MV) {
					worn = true;
				} else if (g_worn_delta_mv <
					   CONFIG_IMPULSE_WORN_RELEASE_MV) {
					worn = false;
				}

				if (worn != g_enf.worn) {
					LOG_INF("worn -> %d (delta %d mV)",
						(int)worn, g_worn_delta_mv);
					impulse_enforcement_set_worn(&g_enf,
								     worn,
								     now_utc());
					g_force_poll = true;
#if defined(CONFIG_IMPULSE_ROLE_WATCH) && \
	(defined(CONFIG_IMPULSE_CS_BACKEND) || defined(CONFIG_IMPULSE_NET))
					/*
					 * §5.5.1. The EDGE is what starts the
					 * escalation; the per-poll re-assert
					 * below is what makes it survive. Worn
					 * is sent on the edge only, because
					 * "stop beeping" is not something to
					 * repeat — the anchor obeys it at once.
					 */
					if (g_enf.state ==
					    IMPULSE_STATE_ENFORCEMENT) {
						escalate_to_anchors(
							worn ? IMPULSE_CMD_WATCH_WORN
							     : IMPULSE_CMD_WATCH_REMOVED,
							g_enf.active);
					}
#endif
#if defined(CONFIG_IMPULSE_BLE)
					impulse_ble_notify_status();
#endif
				}
			}
		}

		/*
		 * Persist the monotonic elapsed base periodically.
		 *
		 * §9.2 requires integrity timers to count elapsed time across
		 * REBOOTS so that writing the clock cannot accelerate a
		 * quarantined loosening. Without this the base is only ever
		 * written by an explicit save, so a power cycle rewinds every
		 * settle window and every pending apply_after toward zero —
		 * which is a loosening, and therefore the wrong direction.
		 *
		 * Ten minutes bounds the rewind to ten minutes while keeping
		 * flash writes to ~144/day, which NVS wear-levelling absorbs
		 * comfortably.
		 * TODO(chunk D): also save on the way into a deep sleep.
		 */
#if defined(CONFIG_IMPULSE_NET)
		impulse_wifi_tick(now_ms);
#endif

		if ((now_ms - last_clock_save) >=
		    (IMPULSE_CLOCK_SAVE_INTERVAL_S * 1000)) {
			last_clock_save = now_ms;
			if (g_utc_base > IMPULSE_CLOCK_SANITY_FLOOR) {
				(void)impulse_storage_save_wall_clock(now_utc());
			}
		}

		if ((now_ms - last_elapsed_save) >= 600000) {
			last_elapsed_save = now_ms;
			(void)impulse_storage_save_elapsed_base(
				impulse_elapsed_now_s());
		}

		/*
		 * Bring-up heartbeat. Also the clock-accuracy measurement: the
		 * printed uptime comes from the kernel tick, so comparing it to
		 * host wall-clock over several minutes distinguishes a crystal
		 * (tens of ppm) from the internal RC (whole percent) far more
		 * reliably than reading a source register.
		 */
		if ((now_ms - last_beat) >= 60000) {
			last_beat = now_ms;
			/* worn_delta and the charger pin ride along on the
			 * heartbeat so a PASSIVE rtt.sh capture can verify the
			 * worn threshold and the charger polarity. Both used to
			 * be visible only via `impulse status`, which needs the
			 * shell — and the shell shares RTT channel 0 with the
			 * log, so it cannot be driven while a capture runs. */
			LOG_INF("alive uptime_ms=%lld state=%d worn=%d worn_delta=%d mV charging=%d batt=%u%% vbat=%d mV",
				now_ms, (int)g_enf.state, (int)g_enf.worn,
				g_worn_delta_mv, gpio_pin_get_dt(&charger),
				g_battery_pct, g_battery_mv);
		}

		k_msleep(TICK_MS);
	}

	return 0;
}
