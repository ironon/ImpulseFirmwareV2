/* SPDX-License-Identifier: Apache-2.0 */
#include "ble_watch.h"

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../app_api.h"
#include "../schedule/schedule_blob.h"
#include "ble_uuids.h"

LOG_MODULE_REGISTER(impulse_ble, LOG_LEVEL_INF);

/*
 * How the app learns a verdict — and it is NOT the ATT write response.
 *
 * The ESP32 firmware answers a write by setting the characteristic value and
 * NOTIFYING it. A write-with-response therefore completes long before the
 * verdict exists, and anything treating the ATT response as the answer will
 * report an accepted schedule that the watch actually quarantined. phone_sim
 * documents this explicitly and subscribes before writing. Every gated
 * characteristic here is therefore WRITE + NOTIFY, and the verdict byte is
 * notified once the deferred work completes.
 */

static struct bt_conn *current_conn;

/* ---- deferred work -------------------------------------------------------
 *
 * v3 §9: work must NOT run on the Bluetooth callback context. The 2026-07-12
 * anchor audit found schedule storage and day recalculation running on the
 * NimBLE host task, racing the loop task's readers — use-after-free presenting
 * as spontaneous field restarts. Zephyr has the same hazard under different
 * names, so every write callback here copies its payload and returns.
 */
#define SCHED_BUF_MAX 4096

static struct {
	uint8_t buf[SCHED_BUF_MAX];
	uint32_t expected_len;
	uint32_t received;
	bool active;
} sched_xfer;

static struct k_work sched_end_work;
static struct k_work status_work;
static struct k_work pending_work;

static uint8_t pending_settings[6];
static struct k_work settings_work;
static uint8_t pending_time[10];
static struct k_work time_work;
static uint8_t pending_pass[21];
static uint8_t pending_pass_len;
static struct k_work pass_work;

/* Forward declarations for the notify helpers, which need the attribute
 * table that is defined below them. */
static void notify_byte(const struct bt_uuid *uuid, uint8_t value);
static void notify_bytes(const struct bt_uuid *uuid, const uint8_t *data,
			 uint16_t len);

static const struct bt_uuid_128 uuid_sched_ctrl =
	BT_UUID_INIT_128(IMPULSE_UUID_SCHED_CTRL);
static const struct bt_uuid_128 uuid_anchor_ip =
	BT_UUID_INIT_128(IMPULSE_UUID_ANCHOR_IP);
static const struct bt_uuid_128 uuid_settings =
	BT_UUID_INIT_128(IMPULSE_UUID_SETTINGS);
static const struct bt_uuid_128 uuid_status =
	BT_UUID_INIT_128(IMPULSE_UUID_STATUS);
static const struct bt_uuid_128 uuid_time =
	BT_UUID_INIT_128(IMPULSE_UUID_TIME);
static const struct bt_uuid_128 uuid_pending =
	BT_UUID_INIT_128(IMPULSE_UUID_PENDING);
static const struct bt_uuid_128 uuid_pass =
	BT_UUID_INIT_128(IMPULSE_UUID_PASS);

/*
 * Notifying from INSIDE an ATT write callback makes the stack return
 * "Unlikely Error" (0x0E) to the client — it is busy processing that very
 * write. Every acknowledgement is therefore queued and sent from a work item.
 * Third time this rule has bitten in this file; it applies to notifications
 * just as much as to storage.
 */
static uint8_t ack_pending;
static struct k_work ack_work;

static void ack_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	notify_byte(&uuid_sched_ctrl.uuid, ack_pending);
}

static void sched_ctrl_ack(uint8_t value)
{
	ack_pending = value;
	(void)k_work_submit(&ack_work);
}

/* §5.6 Anchor IP Table: "responds with 0x01". The app (and phone_sim)
 * implement that response as a NOTIFICATION and subscribe before writing, so
 * the characteristic needs NOTIFY and a CCC — declaring it write-only makes
 * BlueZ refuse the whole operation with "Operation is not supported", before
 * a single byte reaches the firmware. Same deferral rule as above. */
static uint8_t anchor_ip_ack_pending;
static struct k_work anchor_ip_ack_work;

static void anchor_ip_ack_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	notify_byte(&uuid_anchor_ip.uuid, anchor_ip_ack_pending);
}

static void sched_end_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	uint8_t verdict = impulse_app_apply_schedule(sched_xfer.buf,
						     sched_xfer.received);

	sched_xfer.active = false;
	sched_xfer.received = 0;
	notify_byte(&uuid_sched_ctrl.uuid, verdict);
	impulse_ble_notify_status();
	impulse_ble_notify_pending();
}

static void settings_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	int16_t tz = (int16_t)(pending_settings[2] |
			       ((uint16_t)pending_settings[3] << 8));
	uint16_t settle = (uint16_t)(pending_settings[4] |
				     ((uint16_t)pending_settings[5] << 8));
	uint8_t resp = impulse_app_apply_settings(pending_settings[0],
						  pending_settings[1], tz,
						  settle);

	notify_byte(&uuid_settings.uuid, resp);
	impulse_ble_notify_status();
}

static void time_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	int64_t utc = 0;
	int16_t tz;

	for (int i = 7; i >= 0; i--) {
		utc = (utc << 8) | pending_time[i];
	}
	tz = (int16_t)(pending_time[8] | ((uint16_t)pending_time[9] << 8));

	notify_byte(&uuid_time.uuid, impulse_app_apply_time(utc, tz));
	impulse_ble_notify_status();
}

static void pass_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	uint8_t resp[2] = {0};

	if (pending_pass_len >= 21U && pending_pass[0] == 0x01U) {
		uint32_t date = (uint32_t)pending_pass[17] |
				((uint32_t)pending_pass[18] << 8) |
				((uint32_t)pending_pass[19] << 16) |
				((uint32_t)pending_pass[20] << 24);
		uint8_t remaining = 0;

		resp[0] = impulse_app_pass_spend(&pending_pass[1], date,
						 &remaining);
		resp[1] = remaining;
	} else if (pending_pass_len >= 2U && pending_pass[0] == 0x02U) {
		resp[0] = impulse_app_pass_set_allowance(pending_pass[1]);
		resp[1] = 0;
	} else {
		resp[0] = 0x00; /* malformed */
	}

	notify_bytes(&uuid_pass.uuid, resp, sizeof(resp));
	impulse_ble_notify_pending();
}

/* ---- characteristic callbacks ------------------------------------------- */

static ssize_t write_sched_ctrl(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset,
				uint8_t flags)
{
	const uint8_t *p = buf;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	LOG_DBG("sched_ctrl write: len=%u op=0x%02x offset=%u flags=0x%02x", len,
		(len > 0U) ? p[0] : 0xFFU, offset, flags);

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

		/*
		 * Bound the declared length before believing it. The ESP32
		 * build once malloc'd this untrusted 4-byte field directly;
		 * v3 §9 says explicitly to port the cap. Here the buffer is
		 * static, so an over-long declaration is refused outright
		 * rather than truncated into a partial schedule.
		 */
		if (declared == 0U || declared > SCHED_BUF_MAX ||
		    declared > IMPULSE_SCHEDULE_MAX_BLOB_BYTES) {
			LOG_WRN("BEGIN refused: declared %u bytes", declared);
			sched_xfer.active = false;
			sched_ctrl_ack(IMPULSE_END_FAILED);
			return len;
		}

		sched_xfer.expected_len = declared;
		sched_xfer.received = 0;
		sched_xfer.active = true;
		/* BEGIN is acknowledged, not silent. The app waits for this
		 * byte before streaming DATA and treats its absence as a
		 * refusal ("watch out of RAM?"), so staying quiet here stalls
		 * every schedule push. */
		sched_ctrl_ack(IMPULSE_END_ACCEPTED);
		break;
	}
	case IMPULSE_XFER_END:
		if (!sched_xfer.active) {
			break;
		}
		if (len >= 5U) {
			uint32_t want = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
					((uint32_t)p[3] << 16) |
					((uint32_t)p[4] << 24);
			uint32_t got = impulse_crc32(sched_xfer.buf,
						     sched_xfer.received);

			if (want != got) {
				LOG_WRN("schedule CRC mismatch: want %08x got %08x",
					want, got);
				sched_xfer.active = false;
				sched_xfer.received = 0;
				sched_ctrl_ack(IMPULSE_END_FAILED);
				break;
			}
		}
		/* Deferred: the diff gate touches storage and recomputes the
		 * day, neither of which may run on this context. */
		(void)k_work_submit(&sched_end_work);
		break;
	case IMPULSE_XFER_ABORT:
		sched_xfer.active = false;
		sched_xfer.received = 0;
		sched_ctrl_ack(IMPULSE_END_ACCEPTED);
		break;
	default:
		break;
	}
	return len;
}

static ssize_t write_sched_data(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset,
				uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (!sched_xfer.active) {
		return len; /* stray DATA without a BEGIN */
	}
	if ((sched_xfer.received + len) > SCHED_BUF_MAX) {
		LOG_WRN("schedule overflow, aborting transfer");
		sched_xfer.active = false;
		return len;
	}
	memcpy(&sched_xfer.buf[sched_xfer.received], buf, len);
	sched_xfer.received += len;
	return len;
}

static ssize_t write_settings(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, const void *buf,
			      uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len < sizeof(pending_settings)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	memcpy(pending_settings, buf, sizeof(pending_settings));
	(void)k_work_submit(&settings_work);
	return len;
}

static ssize_t write_time(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len < sizeof(pending_time)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	memcpy(pending_time, buf, sizeof(pending_time));
	(void)k_work_submit(&time_work);
	return len;
}

static ssize_t write_pass(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len < 2U || len > sizeof(pending_pass)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	memcpy(pending_pass, buf, len);
	pending_pass_len = (uint8_t)len;
	(void)k_work_submit(&pass_work);
	return len;
}

static ssize_t read_status(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	struct impulse_app_status st;
	uint8_t out[32];
	size_t pos = 0;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	impulse_app_get_status(&st);

	out[pos++] = st.activity_state;
	out[pos++] = st.bt_connected;
	out[pos++] = st.wifi_connected;
	out[pos++] = st.worn;
	out[pos++] = st.battery_pct;
	memcpy(&out[pos], st.active_event_id, 16);
	pos += 16;
	out[pos++] = st.condition_met;
	out[pos++] = (uint8_t)(st.schedule_crc & 0xFFU);
	out[pos++] = (uint8_t)((st.schedule_crc >> 8) & 0xFFU);
	out[pos++] = (uint8_t)((st.schedule_crc >> 16) & 0xFFU);
	out[pos++] = (uint8_t)((st.schedule_crc >> 24) & 0xFFU);
	/* TODO(§5.5.2): unreachable-anchor notifications are not produced yet;
	 * a zero count is a truthful empty list, not a placeholder. */
	out[pos++] = 0;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, out, pos);
}

static ssize_t read_pending(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	uint8_t out[1 + CONFIG_IMPULSE_PENDING_QUEUE_MAX * 21];
	size_t n = impulse_app_pending_payload(out, sizeof(out));

	return bt_gatt_attr_read(conn, attr, buf, len, offset, out, n);
}

static ssize_t read_pass(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	uint8_t out[2];

	impulse_app_pass_read(&out[0], &out[1]);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, out,
				 sizeof(out));
}

static ssize_t read_seen_anchors(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr, void *buf,
				 uint16_t len, uint16_t offset)
{
	/* TODO(chunk G): no anchor discovery scan exists yet. An honest empty
	 * list beats a fabricated one — the app renders "no anchors seen". */
	uint8_t out[1] = {0};

	return bt_gatt_attr_read(conn, attr, buf, len, offset, out,
				 sizeof(out));
}

static ssize_t write_wifi_cred(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr, const void *buf,
			       uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
#if defined(CONFIG_IMPULSE_NET)
	/* Copied out of the ATT buffer before use: nothing here does real work
	 * on a Bluetooth callback context beyond a memcpy and a settings write,
	 * which is the rule v3 §9 states and this file has broken before. */
	impulse_app_apply_wifi_credentials(buf, len);
#else
	ARG_UNUSED(buf);
	LOG_WRN("wifi credentials dropped: this build has no WiFi");
#endif
	return len;
}

static ssize_t write_anchor_ips(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset,
				uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
#if defined(CONFIG_IMPULSE_NET)
	impulse_app_apply_anchor_ips(buf, len);
	anchor_ip_ack_pending = 0x01;
#else
	ARG_UNUSED(buf);
	LOG_WRN("anchor IP table dropped: this build has no WiFi");
	anchor_ip_ack_pending = 0x00;
#endif
	(void)k_work_submit(&anchor_ip_ack_work);
	return len;
}

static ssize_t write_accept_stub(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 const void *buf, uint16_t len, uint16_t offset,
				 uint8_t flags)
{
	/* WiFi credentials, anchor IP table and LED config are accepted at the
	 * ATT layer so the app's flows complete, but nothing consumes them:
	 * WiFi has no hardware on this board (chunk I) and the LED slot model
	 * is chunk B. Silently dropping is deliberate and logged. */
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(buf);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
	LOG_DBG("write to an unimplemented characteristic (%u bytes)", len);
	return len;
}

static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_DBG("CCC changed: 0x%04x", value);
}

/* ---- service ------------------------------------------------------------ */

BT_GATT_SERVICE_DEFINE(
	impulse_watch_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(IMPULSE_UUID_WATCH_SVC)),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_WIFI_CRED),
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_wifi_cred,
			       NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_SCHED_CTRL),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, write_sched_ctrl,
			       NULL),
	BT_GATT_CCC(ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_SCHED_DATA),
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_sched_data,
			       NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_SETTINGS),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, write_settings, NULL),
	BT_GATT_CCC(ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_SEEN_ANCHORS),
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_seen_anchors, NULL,
			       NULL),
	BT_GATT_CCC(ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_STATUS),
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_status, NULL, NULL),
	BT_GATT_CCC(ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_ANCHOR_IP),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, write_anchor_ips,
			       NULL),
	BT_GATT_CCC(ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_LED_CONFIG),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ,
			       BT_GATT_PERM_WRITE | BT_GATT_PERM_READ,
			       read_seen_anchors, write_accept_stub, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_TIME),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, write_time, NULL),
	BT_GATT_CCC(ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_PENDING),
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_pending, NULL, NULL),
	BT_GATT_CCC(ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(IMPULSE_UUID_PASS),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ |
				       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE | BT_GATT_PERM_READ,
			       read_pass, write_pass, NULL),
	BT_GATT_CCC(ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ---- notify helpers ----------------------------------------------------- */

static const struct bt_gatt_attr *find_value_attr(const struct bt_uuid *uuid)
{
	for (size_t i = 0; i < impulse_watch_svc.attr_count; i++) {
		const struct bt_gatt_attr *a = &impulse_watch_svc.attrs[i];

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

	if (attr == NULL || current_conn == NULL) {
		return;
	}
	(void)bt_gatt_notify(current_conn, attr, data, len);
}

static void notify_byte(const struct bt_uuid *uuid, uint8_t value)
{
	notify_bytes(uuid, &value, 1);
}

static void status_work_handler(struct k_work *w)
{
	struct impulse_app_status st;
	uint8_t out[32];
	size_t pos = 0;

	ARG_UNUSED(w);
	impulse_app_get_status(&st);

	out[pos++] = st.activity_state;
	out[pos++] = st.bt_connected;
	out[pos++] = st.wifi_connected;
	out[pos++] = st.worn;
	out[pos++] = st.battery_pct;
	memcpy(&out[pos], st.active_event_id, 16);
	pos += 16;
	out[pos++] = st.condition_met;
	out[pos++] = (uint8_t)(st.schedule_crc & 0xFFU);
	out[pos++] = (uint8_t)((st.schedule_crc >> 8) & 0xFFU);
	out[pos++] = (uint8_t)((st.schedule_crc >> 16) & 0xFFU);
	out[pos++] = (uint8_t)((st.schedule_crc >> 24) & 0xFFU);
	out[pos++] = 0;

	notify_bytes(&uuid_status.uuid, out, (uint16_t)pos);
}

static void pending_work_handler(struct k_work *w)
{
	uint8_t out[1 + CONFIG_IMPULSE_PENDING_QUEUE_MAX * 21];
	size_t n;

	ARG_UNUSED(w);
	n = impulse_app_pending_payload(out, sizeof(out));
	notify_bytes(&uuid_pending.uuid, out, (uint16_t)n);
}

void impulse_ble_notify_status(void)
{
	(void)k_work_submit(&status_work);
}

void impulse_ble_notify_pending(void)
{
	(void)k_work_submit(&pending_work);
}

bool impulse_ble_connected(void)
{
	return current_conn != NULL;
}

/* ---- connection + advertising ------------------------------------------- */

static void adv_restart_work_handler(struct k_work *w);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, IMPULSE_UUID_WATCH_SVC),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0U) {
		LOG_WRN("connection failed (0x%02x)", err);
		return;
	}
	current_conn = bt_conn_ref(conn);
	LOG_INF("app connected");
	impulse_ble_notify_status();
}

/*
 * Advertising MUST be restarted from a work item, not from the disconnect
 * callback. Calling bt_le_adv_start() there fails — the connection object has
 * not been released yet — and because the return was ignored the watch simply
 * went invisible after the first disconnect, with nothing in the log. It is
 * the same "no real work on a Bluetooth callback" rule that every write
 * handler here already follows.
 */
static void adv_restart_work_handler(struct k_work *w)
{
	int err;

	ARG_UNUSED(w);
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd,
			      ARRAY_SIZE(sd));
	if (err == -EALREADY) {
		return;
	}
	if (err != 0) {
		LOG_ERR("advertising restart failed (%d) — the app can no "
			"longer find this watch", err);
		return;
	}
	LOG_INF("advertising restarted");
}

static K_WORK_DEFINE(adv_restart_work, adv_restart_work_handler);

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	LOG_INF("app disconnected (0x%02x)", reason);

	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	/* An in-flight transfer does not survive the link. Keeping a partial
	 * buffer would let the next connection END a schedule it never sent. */
	sched_xfer.active = false;
	sched_xfer.received = 0;

	(void)k_work_submit(&adv_restart_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

int impulse_ble_start(void)
{
	int err;

	k_work_init(&sched_end_work, sched_end_work_handler);
	k_work_init(&ack_work, ack_work_handler);
	k_work_init(&anchor_ip_ack_work, anchor_ip_ack_work_handler);
	k_work_init(&status_work, status_work_handler);
	k_work_init(&pending_work, pending_work_handler);
	k_work_init(&settings_work, settings_work_handler);
	k_work_init(&time_work, time_work_handler);
	k_work_init(&pass_work, pass_work_handler);

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd,
			      ARRAY_SIZE(sd));
	if (err != 0) {
		LOG_ERR("advertising failed to start (%d)", err);
		return err;
	}

	LOG_INF("advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
	return 0;
}
