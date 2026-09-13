/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Channel Sounding REFLECTOR — the anchor half of the ranging pair.
 *
 * Adapted from NCS's `ras_reflector` sample (the copy in ../nrf_cs_test/
 * reflector is the one proven working against Board V1). The reflector is much
 * simpler than the initiator: it answers tones and serves ranging data over
 * RAS, and computes no distance at all. The watch does the maths, because the
 * watch is the root of trust and an anchor is not trusted to report a distance
 * (v3 §4).
 *
 * THREE deliberate departures from the sample, each of which is a bug if
 * carried over verbatim into a product image:
 *
 *  1. No bt_enable() here. The anchor application owns Bluetooth init, and
 *     calling it twice returns -EALREADY and aborts the engine.
 *
 *  2. No sys_reboot() on disconnect. The sample reboots to restart its
 *     one-shot flow; in a product that means the app closing reboots the
 *     device. Cost us a real defect on the watch side — see agent-notes,
 *     2026-09-10.
 *
 *  3. NO ASSUMPTION THAT THERE IS ONLY ONE LINK. The sample is a peripheral
 *     with a single connection, so it keeps one global `connection`. An anchor
 *     is a peripheral to BOTH the phone app and the ranging watch, and — unlike
 *     the watch, where the CS peer is the only CENTRAL link — the two are
 *     indistinguishable by role. So this module does not try to guess which
 *     connection is "the" ranging peer: it enables the reflector role on every
 *     link and lets the initiator declare itself by creating a CS config. A
 *     phone that never starts CS simply never triggers anything here.
 */

#include "proximity.h"
#include "../fatal.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/cs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <bluetooth/services/ras.h>

LOG_MODULE_REGISTER(impulse_cs_refl, LOG_LEVEL_INF);

static bool reflector_enabled;

/*
 * RRSP INSTANCES ARE ALLOCATED AND FREED HERE, NOT BY NCS.
 *
 * CONFIG_BT_RAS_RRSP_AUTO_ALLOC_INSTANCE (off, cs_reflector.conf) frees the
 * instance from inside the `disconnected` callback, which runs on the SYSTEM
 * workqueue. bt_ras_rrsp_free() then k_work_queue_drain()s the RRSP queue,
 * whose send handler can be blocked K_FOREVER allocating from att_pool. ATT
 * buffers come back via bt_conn_tx_notify, which ALSO runs on the system
 * workqueue (CONFIG_BT_CONN_TX_NOTIFY_WQ is off) — so the buffer the sender is
 * waiting for is released only by the thread that is waiting for the sender.
 * Permanent. Before the sysworkq watchdog it took the anchor off the air for
 * an hour; after it, it rebooted the anchor ~60 s after every CS link drop,
 * three times in one Sunrise Lock window (2026-09-12, 23:30).
 *
 * Doing the free on a private queue lets the system workqueue finish the
 * disconnect, release the buffers, and the sender return -ENOTCONN, so the
 * drain completes. A free that still has not completed after
 * RRSP_FREE_STUCK_MS means the premise above was wrong; reboot rather than
 * keep an anchor whose ranging service can never serve again.
 */
#define RRSP_FREE_STUCK_MS 20000
#define RRSP_FREE_WQ_STACK_SIZE 1024

static K_THREAD_STACK_DEFINE(rrsp_free_stack, RRSP_FREE_WQ_STACK_SIZE);
static struct k_work_q rrsp_free_wq;

struct rrsp_free_slot {
	struct k_work work;
	struct k_timer stuck;
	struct bt_conn *conn; /* holds a reference until the free completes */
};

static struct rrsp_free_slot rrsp_free_slots[CONFIG_BT_MAX_CONN];

static void rrsp_free_stuck(struct k_timer *t)
{
	ARG_UNUSED(t);
	LOG_ERR("reflector: RRSP free stuck for %d ms — rebooting",
		RRSP_FREE_STUCK_MS);
	impulse_note_reboot("RRSP free stuck");
	sys_reboot(SYS_REBOOT_COLD);
}

static void rrsp_free_handler(struct k_work *w)
{
	struct rrsp_free_slot *slot =
		CONTAINER_OF(w, struct rrsp_free_slot, work);
	struct bt_conn *conn = slot->conn;

	bt_ras_rrsp_free(conn);
	k_timer_stop(&slot->stuck);
	slot->conn = NULL;
	bt_conn_unref(conn);
}

static int rrsp_free_init(void)
{
	static const struct k_work_queue_config cfg = {
		.name = "impulse rrsp free",
	};

	for (size_t i = 0; i < ARRAY_SIZE(rrsp_free_slots); i++) {
		k_work_init(&rrsp_free_slots[i].work, rrsp_free_handler);
		k_timer_init(&rrsp_free_slots[i].stuck, rrsp_free_stuck, NULL);
	}
	k_work_queue_init(&rrsp_free_wq);
	k_work_queue_start(&rrsp_free_wq, rrsp_free_stack,
			   K_THREAD_STACK_SIZEOF(rrsp_free_stack),
			   K_PRIO_PREEMPT(10), &cfg);
	return 0;
}

SYS_INIT(rrsp_free_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static void refl_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	for (size_t i = 0; i < ARRAY_SIZE(rrsp_free_slots); i++) {
		struct rrsp_free_slot *slot = &rrsp_free_slots[i];

		if (slot->conn == NULL) {
			slot->conn = bt_conn_ref(conn);
			k_timer_start(&slot->stuck, K_MSEC(RRSP_FREE_STUCK_MS),
				      K_NO_WAIT);
			(void)k_work_submit_to_queue(&rrsp_free_wq, &slot->work);
			return;
		}
	}

	/* More pending frees than links can exist: an earlier free never
	 * finished, and its stuck timer is about to reboot us anyway. */
	LOG_ERR("reflector: no RRSP free slot — earlier free still pending");
}

static void refl_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0U) {
		return;
	}

	/* Every link, as the NCS auto-alloc did: the watch is not
	 * distinguishable from the app until it creates a CS config. */
	int arc = bt_ras_rrsp_alloc(conn);

	if (arc != 0) {
		LOG_WRN("reflector: RRSP alloc failed (err %d)", arc);
	}

	if (!reflector_enabled) {
		return;
	}

	/*
	 * Advertise our willingness to reflect on THIS link. Harmless on the
	 * app's connection: enabling the role costs nothing until an initiator
	 * actually creates a CS config, and the phone never will.
	 */
	const struct bt_le_cs_set_default_settings_param default_settings = {
		.enable_initiator_role = false,
		.enable_reflector_role = true,
		.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
		.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
	};

	int rc = bt_le_cs_set_default_settings(conn, &default_settings);

	if (rc != 0) {
		LOG_WRN("reflector: default settings failed (err %d)", rc);
	}
}

static void refl_config_complete(struct bt_conn *conn, uint8_t status,
				 struct bt_conn_le_cs_config *config)
{
	ARG_UNUSED(config);

	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("reflector: CS config failed (HCI 0x%02x)", status);
		return;
	}

	/*
	 * An initiator has just declared itself on this link. Procedure
	 * parameters are the sample's, which are deliberately PERMISSIVE
	 * (interval 1..100, subevent 10-75 ms): the initiator picks the actual
	 * cadence, and the reflector should accept whatever it asks for rather
	 * than impose a second opinion.
	 */
	const struct bt_le_cs_set_procedure_parameters_param procedure_params = {
		.config_id = 0,
		.max_procedure_len = 1000,
		.min_procedure_interval = 1,
		.max_procedure_interval = 100,
		.max_procedure_count = 0,
		.min_subevent_len = 10000,
		.max_subevent_len = 75000,
		.tone_antenna_config_selection = BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1,
		.phy = BT_LE_CS_PROCEDURE_PHY_2M,
		.tx_power_delta = 0x80,
		.preferred_peer_antenna = BT_LE_CS_PROCEDURE_PREFERRED_PEER_ANTENNA_1,
		.snr_control_initiator = BT_LE_CS_SNR_CONTROL_NOT_USED,
		.snr_control_reflector = BT_LE_CS_SNR_CONTROL_NOT_USED,
	};

	int rc = bt_le_cs_set_procedure_parameters(conn, &procedure_params);

	if (rc != 0) {
		LOG_ERR("reflector: procedure parameters failed (err %d)", rc);
		return;
	}

	LOG_INF("reflector: ranging configured for this link");
}

static void refl_security_enabled(struct bt_conn *conn, uint8_t status)
{
	ARG_UNUSED(conn);

	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("reflector: CS security failed (HCI 0x%02x)", status);
	}
}

static void refl_procedure_enabled(struct bt_conn *conn, uint8_t status,
				   struct bt_conn_le_cs_procedure_enable_complete *params)
{
	ARG_UNUSED(conn);

	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("reflector: procedure enable failed (HCI 0x%02x)", status);
		return;
	}

	LOG_INF("reflector: procedures %s",
		(params != NULL && params->state == 1U) ? "ENABLED" : "disabled");
}

BT_CONN_CB_DEFINE(impulse_refl_conn_cb) = {
	.connected = refl_connected,
	.disconnected = refl_disconnected,
	.le_cs_config_complete = refl_config_complete,
	.le_cs_security_enable_complete = refl_security_enabled,
	.le_cs_procedure_enable_complete = refl_procedure_enabled,
};

/* ---------------------------------------------------------------- backend */

static void impulse_cs_measure(const uint8_t *anchor_id,
			       struct impulse_cs_measurement *out)
{
	ARG_UNUSED(anchor_id);

	/*
	 * An anchor never measures. It answers. Returning a failure here drives
	 * the §4.5 abstention path, which is correct and not a stub: if anchor
	 * code ever asks for a distance, that is a bug in the caller.
	 */
	memset(out, 0, sizeof(*out));
	out->result = IMPULSE_CS_FAIL_PROCEDURE;
}

static int impulse_cs_reflector_start(void)
{
	reflector_enabled = true;
	LOG_INF("reflector: armed (RAS responder advertising)");
	return 0;
}

static int impulse_cs_reflector_stop(void)
{
	reflector_enabled = false;
	return 0;
}

static const struct impulse_cs_backend impulse_nrf_reflector = {
	.name = "nrf-cs-reflector",
	.measure = impulse_cs_measure,
	.reflector_start = impulse_cs_reflector_start,
	.reflector_stop = impulse_cs_reflector_stop,
};

int impulse_cs_backend_start(void)
{
	impulse_cs_backend_set(&impulse_nrf_reflector);
	return impulse_cs_reflector_start();
}
