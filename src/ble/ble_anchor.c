/* SPDX-License-Identifier: Apache-2.0 */
#include "ble_watch.h"

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../anchor/anchor.h"
#include "../hal/hal.h"
#include "../app_api.h"
#include "../schedule/schedule_blob.h"
#include "ble_uuids.h"

#if defined(CONFIG_IMPULSE_NET)
#include "../net/net_wifi.h"
#endif

#include <bluetooth/services/ras.h>

LOG_MODULE_REGISTER(impulse_ble_anchor, LOG_LEVEL_INF);

/*
 * Anchor GATT service — v2 §4.4, minus everything v3 §4.8 deletes (the four
 * proximity characteristics and Calibration Mode `…000F`). Same deferral rule
 * as the watch service: nothing real happens on a Bluetooth callback.
 */

#define IMPULSE_MAJOR 0x4A0F

static struct bt_conn *anchor_conn;
static struct bt_conn *dock_conn; /* the connection the app registered */
static uint16_t max_beep_minutes = 30;
static int16_t anchor_tz_offset_minutes;

static struct k_work identify_work;
static struct k_work adv_restart_work;

static uint8_t toggle_pending;
static struct k_work toggle_work;

static const struct bt_uuid_128 uuid_anchor_toggle =
	BT_UUID_INIT_128(IMPULSE_UUID_ANCHOR_TOGGLE);
static const struct bt_uuid_128 uuid_anchor_sched_ctrl =
	BT_UUID_INIT_128(IMPULSE_UUID_ANCHOR_SCHED_CTRL);
static const struct bt_uuid_128 uuid_anchor_settings =
	BT_UUID_INIT_128(IMPULSE_UUID_ANCHOR_SETTINGS);

static void notify_bytes(const struct bt_uuid *uuid, const uint8_t *data,
			 uint16_t len);

/* ---- deferred handlers -------------------------------------------------- */

static void identify_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	/* Blocking beep — must not run on the BLE callback. */
	impulse_anchor_identify();
}

static void toggle_work_handler(struct k_work *w)
{
	uint8_t resp;

	ARG_UNUSED(w);
	resp = impulse_anchor_toggle(toggle_pending,
				     impulse_app_anchor_in_active_event());
	notify_bytes(&uuid_anchor_toggle.uuid, &resp, 1);
}

/* ---- characteristic callbacks ------------------------------------------- */

static ssize_t write_identify(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, const void *buf,
			      uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(buf);
	ARG_UNUSED(offset); ARG_UNUSED(flags);
	/* Payload is ignored by design (§4.4) — any write means "beep". */
	(void)k_work_submit(&identify_work);
	return len;
}

static ssize_t write_anchor_settings(struct bt_conn *conn,
				     const struct bt_gatt_attr *attr,
				     const void *buf, uint16_t len,
				     uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;
	uint8_t resp = 0x01;

	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len < 4U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	max_beep_minutes = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
	anchor_tz_offset_minutes = (int16_t)(p[2] | ((uint16_t)p[3] << 8));
	LOG_INF("anchor settings: max_beep=%u min tz=%d", max_beep_minutes,
		anchor_tz_offset_minutes);
	impulse_app_anchor_set_settings(max_beep_minutes,
					anchor_tz_offset_minutes);
	notify_bytes(&uuid_anchor_settings.uuid, &resp, 1);
	return len;
}

static ssize_t write_toggle(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr, const void *buf,
			    uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	toggle_pending = p[0];
	/* Deferred: the servo move blocks for half a second. */
	(void)k_work_submit(&toggle_work);
	return len;
}

static ssize_t read_toggle(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	uint8_t out = impulse_servo_is_open() ? 0x01U : 0x00U;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &out,
				 sizeof(out));
}

static ssize_t write_dock_register(struct bt_conn *conn,
				   const struct bt_gatt_attr *attr,
				   const void *buf, uint16_t len,
				   uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);

	if (len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	/*
	 * The anchor accepts connections from watches and from the setup app
	 * as well as from the docking phone, so it must be told WHICH link to
	 * measure (§4.4). The writer nominates itself.
	 */
	if (p[0] == 0x01U) {
		dock_conn = conn;
		LOG_INF("dock phone registered");
	} else {
		dock_conn = NULL;
		LOG_INF("dock phone unregistered");
	}
	return len;
}

static ssize_t read_dock_status(struct bt_conn *conn,
				const struct bt_gatt_attr *attr, void *buf,
				uint16_t len, uint16_t offset)
{
	uint8_t out[2] = {0, 0};

	/*
	 * TODO(§4.11): docked/undocked needs the link RSSI of dock_conn polled
	 * every 2 s with DOCK_UNDOCK_CONFIRM_POLLS=3 before believing an
	 * undock. Reporting "no registered phone" until that exists is the
	 * fail-open direction the spec requires: an unknown dock state must
	 * never by itself start an enforcement (§5.4.1 Mode B step 3).
	 */
	out[0] = 0x00;
	out[1] = 0x00;
	return bt_gatt_attr_read(conn, attr, buf, len, offset, out,
				 sizeof(out));
}

static ssize_t read_wifi_status(struct bt_conn *conn,
				const struct bt_gatt_attr *attr, void *buf,
				uint16_t len, uint16_t offset)
{
	/* §4.4 payload:
	 *   [state][ssid_len][ssid][ipv4 4][rssi+128][slots][crc32 4]
	 */
	uint8_t out[2 + IMPULSE_WIFI_SSID_MAX + 4 + 1 + 1 + 4] = {0};
	size_t n = 0;

#if defined(CONFIG_IMPULSE_NET)
	const char *ssid = impulse_wifi_ssid();
	size_t slen = strlen(ssid);

	if (slen > IMPULSE_WIFI_SSID_MAX) {
		slen = IMPULSE_WIFI_SSID_MAX;
	}
	out[n++] = (uint8_t)impulse_wifi_state();
	out[n++] = (uint8_t)slen;
	memcpy(&out[n], ssid, slen);
	n += slen;
	impulse_wifi_ipv4(&out[n]);
	n += 4;
#else
	/* No WiFi in this build — say so rather than fabricating a status. */
	out[n++] = 0x00;
	out[n++] = 0;
	n += 4; /* 0.0.0.0 */
#endif
	out[n++] = 0;  /* rssi: reported as -128, i.e. "unknown" */
	out[n++] = 0;  /* slots_used: beacon slots are deleted in v3 §0.3 */

	/* Schedule CRC — §4.7's sync verification, so the app can CONFIRM an
	 * anchor is up to date rather than infer it. */
	uint32_t crc = impulse_app_anchor_schedule_crc();

	memcpy(&out[n], &crc, sizeof(crc));
	n += sizeof(crc);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, out, n);
}

static ssize_t write_anchor_sched_ctrl(struct bt_conn *conn,
				       const struct bt_gatt_attr *attr,
				       const void *buf, uint16_t len,
				       uint16_t offset, uint8_t flags);
static ssize_t write_anchor_sched_data(struct bt_conn *conn,
				       const struct bt_gatt_attr *attr,
				       const void *buf, uint16_t len,
				       uint16_t offset, uint8_t flags);
#if defined(CONFIG_IMPULSE_NET)
static ssize_t write_anchor_wifi_cred(struct bt_conn *conn,
				      const struct bt_gatt_attr *attr,
				      const void *buf, uint16_t len,
				      uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
	impulse_app_apply_wifi_credentials(buf, len);
	return len;
}
#else
static ssize_t write_anchor_wifi_cred(struct bt_conn *conn,
				      const struct bt_gatt_attr *attr,
				      const void *buf, uint16_t len,
				      uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(buf);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
	LOG_WRN("anchor wifi credentials dropped: this build has no WiFi");
	return len;
}
#endif

static ssize_t write_stub(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(buf);
	ARG_UNUSED(offset); ARG_UNUSED(flags);
	/* WiFi credentials: stored by the ESP32 build in a 4-slot table, but
	 * this board has no WiFi hardware. Accepted so app flows complete. */
	return len;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr); ARG_UNUSED(value);
}

BT_GATT_SERVICE_DEFINE(
	impulse_anchor_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_SVC)),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_IDENTIFY),
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_identify, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_WIFI_CRED),
			       BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL,
			       write_anchor_wifi_cred, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_SETTINGS),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, write_anchor_settings,
			       NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_SCHED_CTRL),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL,
			       write_anchor_sched_ctrl, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_SCHED_DATA),
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL,
			       write_anchor_sched_data, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_TOGGLE),
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
				       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_toggle, write_toggle, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_DOCK_REG),
			       BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL,
			       write_dock_register, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_DOCK_STAT),
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_dock_status, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_WIFI_STAT),
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_wifi_status, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static const struct bt_gatt_attr *find_value_attr(const struct bt_uuid *uuid)
{
	for (size_t i = 0; i < impulse_anchor_svc.attr_count; i++) {
		const struct bt_gatt_attr *a = &impulse_anchor_svc.attrs[i];

		if (bt_uuid_cmp(a->uuid, uuid) == 0) {
			return a;
		}
	}
	return NULL;
}

static void notify_bytes(const struct bt_uuid *uuid, const uint8_t *data,
			 uint16_t len)
{
	const struct bt_gatt_attr *attr = find_value_attr(uuid);

	if (attr == NULL || anchor_conn == NULL) {
		return;
	}
	(void)bt_gatt_notify(anchor_conn, attr, data, len);
}

/* ---- schedule transfer (same 3-phase protocol as the watch, §6.2) -------- */

#define ANCHOR_SCHED_BUF_MAX 4096
static struct {
	uint8_t buf[ANCHOR_SCHED_BUF_MAX];
	uint32_t received;
	bool active;
} anchor_xfer;

static uint8_t anchor_ack_pending;
static struct k_work anchor_ack_work;
static struct k_work anchor_end_work;

static void anchor_ack_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	notify_bytes(&uuid_anchor_sched_ctrl.uuid, &anchor_ack_pending, 1);
}

static void anchor_ack(uint8_t v)
{
	anchor_ack_pending = v;
	(void)k_work_submit(&anchor_ack_work);
}

static void anchor_end_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	/*
	 * §4.7: the anchor stores the schedule and recomputes locally. It does
	 * NOT run the §9 integrity gate — that is the watch's job as root of
	 * trust, and an anchor that quarantined its own schedule would simply
	 * be out of sync with the device that decides.
	 */
	uint8_t verdict = impulse_app_anchor_apply_schedule(anchor_xfer.buf,
							    anchor_xfer.received);

	anchor_xfer.active = false;
	anchor_xfer.received = 0;
	anchor_ack(verdict);
}

static ssize_t write_anchor_sched_ctrl(struct bt_conn *conn,
				       const struct bt_gatt_attr *attr,
				       const void *buf, uint16_t len,
				       uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case IMPULSE_XFER_BEGIN: {
		uint32_t declared;

		if (len < 5U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		declared = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
			   ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
		if (declared == 0U || declared > ANCHOR_SCHED_BUF_MAX) {
			anchor_xfer.active = false;
			anchor_ack(IMPULSE_END_FAILED);
			return len;
		}
		anchor_xfer.received = 0;
		anchor_xfer.active = true;
		anchor_ack(IMPULSE_END_ACCEPTED);
		break;
	}
	case IMPULSE_XFER_END:
		if (!anchor_xfer.active) {
			break;
		}
		if (len >= 5U) {
			uint32_t want = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
					((uint32_t)p[3] << 16) |
					((uint32_t)p[4] << 24);

			if (want != impulse_crc32(anchor_xfer.buf,
						  anchor_xfer.received)) {
				anchor_xfer.active = false;
				anchor_xfer.received = 0;
				anchor_ack(IMPULSE_END_FAILED);
				break;
			}
		}
		(void)k_work_submit(&anchor_end_work);
		break;
	case IMPULSE_XFER_ABORT:
		anchor_xfer.active = false;
		anchor_xfer.received = 0;
		anchor_ack(IMPULSE_END_ACCEPTED);
		break;
	default:
		break;
	}
	return len;
}

static ssize_t write_anchor_sched_data(struct bt_conn *conn,
				       const struct bt_gatt_attr *attr,
				       const void *buf, uint16_t len,
				       uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (!anchor_xfer.active) {
		return len;
	}
	if ((anchor_xfer.received + len) > ANCHOR_SCHED_BUF_MAX) {
		anchor_xfer.active = false;
		return len;
	}
	memcpy(&anchor_xfer.buf[anchor_xfer.received], buf, len);
	anchor_xfer.received += len;
	return len;
}

/* ---- advertising -------------------------------------------------------- */

/*
 * iBeacon (§4.3). Major is the fixed Impulse namespace fingerprint 0x4A0F,
 * which is how the watch filters out AirTags and Tiles while scanning.
 *
 * Minor is 0x0000: the beacon-schedule slot tagging of v2 §4.12 is DELETED by
 * v3 §0.3 along with the stepped-power PDR machinery it fed, so there are no
 * slots to encode. Watches already treat an all-zero Minor as "no schedule".
 *
 * Major and Minor go BIG-ENDIAN on air, per iBeacon convention — the one place
 * in this codebase that is not little-endian.
 */
static uint8_t ibeacon_data[25] = {
	0x4C, 0x00, /* company id placeholder, replaced below */
	0x02, 0x15, /* iBeacon type + length */
	/* 16-byte UUID filled at init */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	(IMPULSE_MAJOR >> 8) & 0xFF, IMPULSE_MAJOR & 0xFF, /* big-endian */
	0x00, 0x00, /* Minor */
	0xC5,       /* measured power at 1 m */
};

static struct bt_data anchor_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, ibeacon_data, sizeof(ibeacon_data)),
};

static const struct bt_data anchor_sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, IMPULSE_UUID_ANCHOR_SVC),
#if defined(CONFIG_IMPULSE_CS_REFLECTOR)
	/*
	 * The Ranging Service UUID goes in the SCAN RESPONSE, not the
	 * advertising data, because the advertising packet is full: flags (3) +
	 * the 25-byte iBeacon manufacturer payload (27 with its header) = 30 of
	 * 31 bytes, leaving no room for a 4-byte UUID16 entry. The scan
	 * response carries the 18-byte anchor UUID128 and has room.
	 *
	 * CONSEQUENCE, and it is not optional: a PASSIVE scanner never requests
	 * a scan response and so will never see this. The watch's CS scanner
	 * filters on exactly this UUID and must therefore scan ACTIVELY — see
	 * bt_scan_start() in cs_backend_nrf.c.
	 */
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_RANGING_SERVICE_VAL)),
#endif
};

static void adv_restart_work_handler(struct k_work *w)
{
	int err;

	ARG_UNUSED(w);
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, anchor_ad,
			      ARRAY_SIZE(anchor_ad), anchor_sd,
			      ARRAY_SIZE(anchor_sd));
	if (err != 0 && err != -EALREADY) {
		LOG_ERR("anchor advertising restart failed (%d)", err);
	}
}

static void anchor_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0U) {
		return;
	}
	anchor_conn = bt_conn_ref(conn);
	LOG_INF("anchor: peer connected");
}

static void anchor_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	LOG_INF("anchor: peer disconnected (0x%02x)", reason);

	if (conn == dock_conn) {
		dock_conn = NULL;
	}
	if (anchor_conn != NULL) {
		bt_conn_unref(anchor_conn);
		anchor_conn = NULL;
	}
	anchor_xfer.active = false;
	anchor_xfer.received = 0;
	(void)k_work_submit(&adv_restart_work);
}

BT_CONN_CB_DEFINE(anchor_conn_callbacks) = {
	.connected = anchor_connected,
	.disconnected = anchor_disconnected,
};

int impulse_ble_start(void)
{
	int err;

	k_work_init(&identify_work, identify_work_handler);
	k_work_init(&toggle_work, toggle_work_handler);
	k_work_init(&anchor_ack_work, anchor_ack_work_handler);
	k_work_init(&anchor_end_work, anchor_end_work_handler);
	k_work_init(&adv_restart_work, adv_restart_work_handler);

	/* Company id 0xFFFF, not Apple's 0x004C: iOS strips Apple
	 * manufacturer data from scan results (phone_sim constants.py). */
	ibeacon_data[0] = 0xFF;
	ibeacon_data[1] = 0xFF;
	memcpy(&ibeacon_data[4], impulse_anchor_uuid(), 16);

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, anchor_ad,
			      ARRAY_SIZE(anchor_ad), anchor_sd,
			      ARRAY_SIZE(anchor_sd));
	if (err != 0) {
		LOG_ERR("anchor advertising failed (%d)", err);
		return err;
	}

	LOG_INF("anchor advertising (iBeacon major 0x%04X)", IMPULSE_MAJOR);
	return 0;
}

void impulse_ble_notify_status(void) {}
void impulse_ble_notify_pending(void) {}

bool impulse_ble_connected(void)
{
	return anchor_conn != NULL;
}
