/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "le_audio.h"

#include <zephyr/zbus/zbus.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>

/* TODO: Remove when a get_info function is implemented in host */
#include <../subsys/bluetooth/audio/bap_endpoint.h>
#include <../subsys/bluetooth/audio/bap_iso.h>

#include "macros_common.h"
#include "nrf5340_audio_common.h"
#include "ble_hci_vsc.h"
#include "ble_audio_services.h"
#include "channel_assignment.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cis_gateway, CONFIG_BLE_LOG_LEVEL);

ZBUS_CHAN_DEFINE(le_audio_chan, struct le_audio_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

#define HCI_ISO_BUF_ALLOC_PER_CHAN 2
#define CIS_CONN_RETRY_TIMES 5
#define CIS_CONN_RETRY_DELAY_MS 500
#define CONNECTION_PARAMETERS                                                                      \
	BT_LE_CONN_PARAM(CONFIG_BLE_ACL_CONN_INTERVAL, CONFIG_BLE_ACL_CONN_INTERVAL,               \
			 CONFIG_BLE_ACL_SLAVE_LATENCY, CONFIG_BLE_ACL_SUP_TIMEOUT)

/* For being able to dynamically define iso_tx_pools */
#define NET_BUF_POOL_ITERATE(i, _)                                                                 \
	NET_BUF_POOL_FIXED_DEFINE(iso_tx_pool_##i, HCI_ISO_BUF_ALLOC_PER_CHAN,                     \
				  BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU), 8, NULL);
#define NET_BUF_POOL_PTR_ITERATE(i, ...) IDENTITY(&iso_tx_pool_##i)
LISTIFY(CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SNK_COUNT, NET_BUF_POOL_ITERATE, (;))
/* clang-format off */
static struct net_buf_pool *iso_tx_pools[] = { LISTIFY(CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SNK_COUNT,
						       NET_BUF_POOL_PTR_ITERATE, (,)) };
/* clang-format on */

struct le_audio_headset {
	char *ch_name;
	bool hci_wrn_printed;
	uint32_t seq_num;
	struct bt_bap_stream sink_stream;
	struct bt_bap_ep *sink_ep;
	struct bt_codec sink_codec_cap[CONFIG_CODEC_CAP_COUNT_MAX];
	struct bt_bap_stream source_stream;
	struct bt_bap_ep *source_ep;
	struct bt_codec source_codec_cap[CONFIG_CODEC_CAP_COUNT_MAX];
	struct bt_conn *headset_conn;
	struct net_buf_pool *iso_tx_pool;
	atomic_t iso_tx_pool_alloc;
	struct k_work_delayable stream_start_sink_work;
	struct k_work_delayable stream_start_source_work;
	bool qos_reconfigure;
	uint32_t reconfigure_pd;
};

struct worker_data {
	uint8_t channel_index;
	enum bt_audio_dir dir;
	uint8_t retries;
} __aligned(4);

struct temp_cap_storage {
	struct bt_conn *conn;
	/* Must be the same size as sink_codec_cap and source_codec_cap */
	struct bt_codec codec[CONFIG_CODEC_CAP_COUNT_MAX];
};

static struct le_audio_headset headsets[CONFIG_BT_MAX_CONN];

K_MSGQ_DEFINE(kwork_msgq, sizeof(struct worker_data),
	      2 * (CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SNK_COUNT +
		   CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SRC_COUNT),
	      sizeof(uint32_t));

static struct temp_cap_storage temp_cap[CONFIG_BT_MAX_CONN];

/* Make sure that we have at least one headset device per CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SNK */
BUILD_ASSERT(ARRAY_SIZE(headsets) >= CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SNK_COUNT,
	     "We need to have at least one headset device per ASE SINK");

/* Make sure that we have at least one headset device per CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SRC */
BUILD_ASSERT(ARRAY_SIZE(headsets) >= CONFIG_BT_BAP_UNICAST_CLIENT_ASE_SRC_COUNT,
	     "We need to have at least one headset device per ASE SOURCE");

static le_audio_receive_cb receive_cb;
static le_audio_timestamp_cb timestamp_cb;

static struct bt_bap_unicast_group *unicast_group;

static struct bt_bap_lc3_preset lc3_preset_sink = BT_BAP_LC3_UNICAST_PRESET_NRF5340_AUDIO_SINK;
static struct bt_bap_lc3_preset lc3_preset_source = BT_BAP_LC3_UNICAST_PRESET_NRF5340_AUDIO_SOURCE;

static uint8_t bonded_num;
static bool playing_state = true;

static void ble_acl_start_scan(void);
static bool ble_acl_gateway_all_links_connected(void);

static struct bt_bap_unicast_client_discover_params audio_sink_discover_param[CONFIG_BT_MAX_CONN];

#if (CONFIG_BT_AUDIO_RX)
static struct bt_bap_unicast_client_discover_params audio_source_discover_param[CONFIG_BT_MAX_CONN];

static int discover_source(struct bt_conn *conn);
#endif /* (CONFIG_BT_AUDIO_RX) */

static void le_audio_event_publish(enum le_audio_evt_type event)
{
	int ret;
	struct le_audio_msg msg;

	msg.event = event;

	ret = zbus_chan_pub(&le_audio_chan, &msg, K_NO_WAIT);
	ERR_CHK(ret);
}

/**
 * @brief  Get the common presentation delay for all headsets.
 *
 * @param  index        The index of the headset to test against
 * @param  pres_dly_us  A pointer where the presentation delay is stored
 *
 * @return 0 if successful, -EINVAL on an error
 */
static int headset_pres_delay_find(uint8_t index, uint32_t *pres_dly_us)
{
	uint32_t pres_dly_min = headsets[index].sink_ep->qos_pref.pd_min;
	uint32_t pres_dly_max = headsets[index].sink_ep->qos_pref.pd_max;
	uint32_t pref_dly_min = headsets[index].sink_ep->qos_pref.pref_pd_min;
	uint32_t pref_dly_max = headsets[index].sink_ep->qos_pref.pref_pd_max;

	for (int i = 0; i < ARRAY_SIZE(headsets); i++) {
		if (headsets[i].sink_ep != NULL) {
			pres_dly_min = MAX(pres_dly_min, headsets[i].sink_ep->qos_pref.pd_min);
			pres_dly_max = MIN(pres_dly_max, headsets[i].sink_ep->qos_pref.pd_max);
			pref_dly_min = MAX(pref_dly_min, headsets[i].sink_ep->qos_pref.pref_pd_min);
			pref_dly_max = MIN(pref_dly_max, headsets[i].sink_ep->qos_pref.pref_pd_max);
		}
	}

	if (IS_ENABLED(CONFIG_BT_AUDIO_PRES_DELAY_SRCH_MIN)) {
		*pres_dly_us = pres_dly_min;

		return 0;
	}

	if (IS_ENABLED(CONFIG_BT_AUDIO_PRES_DELAY_SRCH_MAX)) {
		*pres_dly_us = pres_dly_max;

		return 0;
	}

	if (IS_ENABLED(CONFIG_BT_AUDIO_PRES_DELAY_SRCH_PREF_MIN)) {
		*pres_dly_us = pref_dly_min;

		return 0;
	}

	if (IS_ENABLED(CONFIG_BT_AUDIO_PRES_DELAY_SRCH_PREF_MAX)) {
		*pres_dly_us = pref_dly_max;

		return 0;
	}

	if (IS_ENABLED(CONFIG_BT_AUDIO_PRES_DELAY_SRCH_PREF_MIN)) {
		if (IN_RANGE(CONFIG_BT_AUDIO_PRESENTATION_DELAY_US, pres_dly_min, pres_dly_max)) {
			*pres_dly_us = CONFIG_BT_AUDIO_PRESENTATION_DELAY_US;
		} else {
			LOG_WRN("Preferred local presentaion delay outside of range");

			if (pres_dly_max < CONFIG_BT_AUDIO_PRESENTATION_DELAY_US) {
				*pres_dly_us = pres_dly_max;

				LOG_WRN("Selecting maximum common delay: %d us ", pres_dly_max);
			} else {
				*pres_dly_us = pres_dly_min;

				LOG_WRN("Selecting minimum common delay: %d us ", pres_dly_min);
			}
		}

		return 0;
	}

	LOG_ERR("Trying to use unrecognised search mode");
	return -EINVAL;
}

/**
 * @brief  Check if an endpoint is in the given state
 *
 * @note   If the endpoint is NULL, it is not in the
 *         given state, and this function returns false
 *
 * @param[in] ep    The endpoint to check
 * @param state     The state to check for
 *
 * @return True if the endpoint is in the given state, false otherwise
 */
static bool ep_state_check(struct bt_bap_ep *ep, enum bt_bap_ep_state state)
{
	if (ep == NULL) {
		LOG_DBG("Endpoint is NULL");
		return false;
	}

	if (ep->status.state == state) {
		return true;
	}

	return false;
}

/**
 * @brief  Get channel index based on connection
 *
 * @param[in]  conn   The connection to search for
 * @param[out] index  The channel index
 *
 * @return 0 if succesfull, -EINVAL if there is no match
 */
static int channel_index_get(const struct bt_conn *conn, uint8_t *index)
{
	if (conn == NULL) {
		LOG_ERR("No connection provided");
		return -EINVAL;
	}

	for (int i = 0; i < ARRAY_SIZE(headsets); i++) {
		if (headsets[i].headset_conn == conn) {
			*index = i;
			return 0;
		}
	}

	LOG_WRN("Connection not found");

	return -EINVAL;
}

static uint32_t get_and_incr_seq_num(const struct bt_bap_stream *stream)
{
	uint8_t channel_index;

	if (channel_index_get(stream->conn, &channel_index) == 0) {
		return headsets[channel_index].seq_num++;
	}

	LOG_WRN("Could not find matching stream %p", stream);

	return 0;
}

static int update_sink_stream_qos(struct le_audio_headset *headset, uint32_t pres_delay_us)
{
	int ret;

	if (headset->sink_stream.qos == NULL) {
		LOG_WRN("No QoS found for %p", &headset->sink_stream);
		return -ENXIO;
	}

	if (headset->sink_stream.qos->pd != pres_delay_us) {
		if (playing_state) {
			LOG_DBG("Update streaming %s headset, connection %p, stream %p",
				headset->ch_name, &headset->headset_conn, &headset->sink_stream);

			headset->qos_reconfigure = true;
			headset->reconfigure_pd = pres_delay_us;

			ret = bt_bap_stream_disable(&headset->sink_stream);
			if (ret) {
				LOG_ERR("Unable to disable stream: %d", ret);
				return ret;
			}
		} else {
			LOG_DBG("Reset %s headset, connection %p, stream %p", headset->ch_name,
				&headset->headset_conn, &headset->sink_stream);

			headset->sink_stream.qos->pd = pres_delay_us;

			ret = bt_bap_stream_qos(headset->headset_conn, unicast_group);
			if (ret) {
				LOG_ERR("Unable to configure %s headset: %d", headset->ch_name,
					ret);
				return ret;
			}
		}
	}

	return 0;
}

static void unicast_client_location_cb(struct bt_conn *conn, enum bt_audio_dir dir,
				       enum bt_audio_location loc)
{
	int ret;

	if (dir == BT_AUDIO_DIR_SINK) {
		if (loc & BT_AUDIO_LOCATION_FRONT_LEFT || loc & BT_AUDIO_LOCATION_SIDE_LEFT) {
			headsets[AUDIO_CH_L].headset_conn = conn;
		} else if (loc & BT_AUDIO_LOCATION_FRONT_RIGHT ||
			   loc & BT_AUDIO_LOCATION_SIDE_RIGHT) {
			headsets[AUDIO_CH_R].headset_conn = conn;
		} else {
			LOG_ERR("Channel location not supported");
			ret = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			if (ret) {
				LOG_ERR("Failed to disconnect %d", ret);
			}
		}

		if (!ble_acl_gateway_all_links_connected()) {
			ble_acl_start_scan();
		} else {
			LOG_INF("All headsets connected");
		}
	}
}

static void available_contexts_cb(struct bt_conn *conn, enum bt_audio_context snk_ctx,
				  enum bt_audio_context src_ctx)
{
	char addr[BT_ADDR_LE_STR_LEN];

	(void)bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_DBG("conn: %s, snk ctx %d src ctx %d\n", addr, snk_ctx, src_ctx);
}

static const struct bt_bap_unicast_client_cb unicast_client_cbs = {
	.location = unicast_client_location_cb,
	.available_contexts = available_contexts_cb,
};

static void stream_sent_cb(struct bt_bap_stream *stream)
{
	int ret;
	uint8_t channel_index;

	ret = channel_index_get(stream->conn, &channel_index);
	if (ret) {
		LOG_ERR("Channel index not found");
	} else {
		atomic_dec(&headsets[channel_index].iso_tx_pool_alloc);
	}
}

static void stream_configured_cb(struct bt_bap_stream *stream, const struct bt_codec_qos_pref *pref)
{
	int ret;
	uint8_t channel_index;
	uint32_t new_pres_dly_us;

	ret = channel_index_get(stream->conn, &channel_index);
	if (ret) {
		LOG_ERR("Channel index not found");
		return;
	}

	if (stream->ep->dir == BT_AUDIO_DIR_SINK) {
		LOG_INF("%s sink stream configured", headsets[channel_index].ch_name);
	} else if (stream->ep->dir == BT_AUDIO_DIR_SOURCE) {
		LOG_INF("%s source stream configured", headsets[channel_index].ch_name);
	} else {
		LOG_WRN("Endpoint direction not recognized: %d", stream->ep->dir);
		return;
	}

	ret = headset_pres_delay_find(channel_index, &new_pres_dly_us);
	if (ret) {
		LOG_ERR("Cannot get a valid presentation delay");
		return;
	}

#if CONFIG_STREAM_BIDIRECTIONAL
	if (ep_state_check(headsets[channel_index].sink_stream.ep,
			   BT_BAP_EP_STATE_CODEC_CONFIGURED) &&
	    ep_state_check(headsets[channel_index].source_stream.ep,
			   BT_BAP_EP_STATE_CODEC_CONFIGURED))
#endif /* CONFIG_STREAM_BIDIRECTIONAL */
	{
		for (int i = 0; i < ARRAY_SIZE(headsets); i++) {
			if (i != channel_index && headsets[i].headset_conn != NULL) {
				ret = update_sink_stream_qos(&headsets[i], new_pres_dly_us);
				if (ret) {
					LOG_ERR("Presentation delay not set for %s headset: %d",
						headsets[channel_index].ch_name, ret);
				}
			}
		}

		LOG_DBG("Set %s headset, connection %p, stream %p", headsets[channel_index].ch_name,
			&headsets[channel_index].headset_conn,
			&headsets[channel_index].sink_stream);

		headsets[channel_index].sink_stream.qos->pd = new_pres_dly_us;

		ret = bt_bap_stream_qos(headsets[channel_index].headset_conn, unicast_group);
		if (ret) {
			LOG_ERR("QoS not set for %s headset: %d", headsets[channel_index].ch_name,
				ret);
		}
	}
}

static void stream_qos_set_cb(struct bt_bap_stream *stream)
{
	int ret;
	uint8_t channel_index;

	ret = channel_index_get(stream->conn, &channel_index);

	if (headsets[channel_index].qos_reconfigure) {
		LOG_DBG("Reconfiguring: %s to PD: %d", headsets[channel_index].ch_name,
			headsets[channel_index].reconfigure_pd);

		headsets[channel_index].qos_reconfigure = false;
		headsets[channel_index].sink_stream.qos->pd =
			headsets[channel_index].reconfigure_pd;

		ret = bt_bap_stream_qos(headsets[channel_index].headset_conn, unicast_group);
		if (ret) {
			LOG_ERR("Unable to reconfigure %s: %d", headsets[channel_index].ch_name,
				ret);
		}
	} else {
		LOG_DBG("Set %s to PD: %d", headsets[channel_index].ch_name, stream->qos->pd);

		if (playing_state) {
			ret = bt_bap_stream_enable(stream, lc3_preset_sink.codec.meta,
						   lc3_preset_sink.codec.meta_count);
			if (ret) {
				LOG_ERR("Unable to enable stream: %d", ret);
				return;
			}

			LOG_INF("Enable stream %p", stream);
		}
	}
}

static void stream_enabled_cb(struct bt_bap_stream *stream)
{
	int ret;
	uint8_t channel_index;
	struct worker_data work_data;

	LOG_DBG("Stream enabled: %p", stream);

	ret = channel_index_get(stream->conn, &channel_index);
	if (ret) {
		LOG_ERR("Error getting channel index");
		return;
	}

#if CONFIG_STREAM_BIDIRECTIONAL
	if (ep_state_check(headsets[channel_index].sink_stream.ep, BT_BAP_EP_STATE_ENABLING) &&
	    ep_state_check(headsets[channel_index].source_stream.ep, BT_BAP_EP_STATE_ENABLING)) {
#endif /* CONFIG_STREAM_BIDIRECTIONAL */

		if (!k_work_delayable_is_pending(&headsets[channel_index].stream_start_sink_work)) {
			work_data.channel_index = channel_index;
			work_data.retries = 0;
			work_data.dir = BT_AUDIO_DIR_SINK;

			LOG_DBG("k_msg_put: ch: %d, dir: %d, retries %d", work_data.channel_index,
				work_data.dir, work_data.retries);
			ret = k_msgq_put(&kwork_msgq, &work_data, K_NO_WAIT);
			if (ret) {
				LOG_ERR("No space in the queue for work_data");
				return;
			}
			k_work_schedule(&headsets[channel_index].stream_start_sink_work, K_NO_WAIT);
		}
#if CONFIG_STREAM_BIDIRECTIONAL
		if (!k_work_delayable_is_pending(
			    &headsets[channel_index].stream_start_source_work)) {
			work_data.channel_index = channel_index;
			work_data.retries = 0;
			work_data.dir = BT_AUDIO_DIR_SOURCE;

			LOG_DBG("k_msg_put: ch: %d, dir: %d, retries %d", work_data.channel_index,
				work_data.dir, work_data.retries);
			ret = k_msgq_put(&kwork_msgq, &work_data, K_NO_WAIT);
			if (ret) {
				LOG_ERR("No space in the queue for work_data");
				return;
			}
			k_work_schedule(&headsets[channel_index].stream_start_source_work,
					K_NO_WAIT);
		}
	}
#endif /* CONFIG_STREAM_BIDIRECTIONAL */
}

static void stream_started_cb(struct bt_bap_stream *stream)
{
	uint8_t channel_index;

	/* Reset sequence number for the stream */
	if (channel_index_get(stream->conn, &channel_index) == 0) {
		headsets[channel_index].seq_num = 0;
	}

	LOG_INF("Stream %p started", (void *)stream);

	le_audio_event_publish(LE_AUDIO_EVT_STREAMING);
}

static void stream_metadata_updated_cb(struct bt_bap_stream *stream)
{
	LOG_DBG("Audio Stream %p metadata updated", (void *)stream);
}

static void stream_disabled_cb(struct bt_bap_stream *stream)
{
	LOG_DBG("Audio Stream %p disabled", (void *)stream);
}

static void stream_stopped_cb(struct bt_bap_stream *stream, uint8_t reason)
{
	int ret;
	uint8_t channel_index;

	LOG_INF("Stream %p stopped. Reason %d", (void *)stream, reason);

	ret = channel_index_get(stream->conn, &channel_index);
	if (ret) {
		LOG_ERR("Channel index not found");
	} else {
		atomic_clear(&headsets[channel_index].iso_tx_pool_alloc);
		headsets[channel_index].hci_wrn_printed = false;
	}

	/* Check if the other stream is streaming, send event if not */
	if (stream == &headsets[AUDIO_CH_L].sink_stream) {
		if (!ep_state_check(headsets[AUDIO_CH_R].sink_stream.ep,
				    BT_BAP_EP_STATE_STREAMING)) {
			le_audio_event_publish(LE_AUDIO_EVT_NOT_STREAMING);
		}
	} else if (stream == &headsets[AUDIO_CH_R].sink_stream) {
		if (!ep_state_check(headsets[AUDIO_CH_L].sink_stream.ep,
				    BT_BAP_EP_STATE_STREAMING)) {
			le_audio_event_publish(LE_AUDIO_EVT_NOT_STREAMING);
		}
	} else {
		LOG_WRN("Unknown stream");
	}
}

static void stream_released_cb(struct bt_bap_stream *stream)
{
	LOG_DBG("Audio Stream %p released", (void *)stream);

	/* Check if the other stream is streaming, send event if not */
	if (stream == &headsets[AUDIO_CH_L].sink_stream) {
		if (!ep_state_check(headsets[AUDIO_CH_R].sink_stream.ep,
				    BT_BAP_EP_STATE_STREAMING)) {
			le_audio_event_publish(LE_AUDIO_EVT_NOT_STREAMING);
		}
	} else if (stream == &headsets[AUDIO_CH_R].sink_stream) {
		if (!ep_state_check(headsets[AUDIO_CH_L].sink_stream.ep,
				    BT_BAP_EP_STATE_STREAMING)) {
			le_audio_event_publish(LE_AUDIO_EVT_NOT_STREAMING);
		}
	} else {
		LOG_WRN("Unknown stream");
	}
}

#if (CONFIG_BT_AUDIO_RX)
static void stream_recv_cb(struct bt_bap_stream *stream, const struct bt_iso_recv_info *info,
			   struct net_buf *buf)
{
	int ret;
	bool bad_frame = false;
	uint8_t channel_index;
	static uint32_t data_size_mismatch_cnt;

	if (receive_cb == NULL) {
		LOG_ERR("The RX callback has not been set");
		return;
	}

	if (!(info->flags & BT_ISO_FLAGS_VALID)) {
		bad_frame = true;
	}

	ret = channel_index_get(stream->conn, &channel_index);
	if (ret) {
		LOG_ERR("Channel index not found");
		return;
	}

	uint32_t octets_per_frame = bt_codec_cfg_get_octets_per_frame(stream->codec);

	if (buf->len != octets_per_frame && bad_frame != true) {
		data_size_mismatch_cnt++;
		if ((data_size_mismatch_cnt % 500) == 1) {
			LOG_DBG("Data size mismatch, received: %d, codec: %d, total: %d", buf->len,
				octets_per_frame, data_size_mismatch_cnt);
		}

		return;
	}

	receive_cb(buf->data, buf->len, bad_frame, info->ts, channel_index);
}
#endif /* (CONFIG_BT_AUDIO_RX) */

static struct bt_bap_stream_ops stream_ops = {
	.configured = stream_configured_cb,
	.qos_set = stream_qos_set_cb,
	.enabled = stream_enabled_cb,
	.started = stream_started_cb,
	.metadata_updated = stream_metadata_updated_cb,
	.disabled = stream_disabled_cb,
	.stopped = stream_stopped_cb,
	.released = stream_released_cb,
#if (CONFIG_BT_AUDIO_RX)
	.recv = stream_recv_cb,
#endif /* (CONFIG_BT_AUDIO_RX) */
#if (CONFIG_BT_AUDIO_TX)
	.sent = stream_sent_cb,
#endif /* (CONFIG_BT_AUDIO_TX) */
};

static void work_stream_start(struct k_work *work)
{
	int ret;
	struct worker_data work_data;

	ret = k_msgq_get(&kwork_msgq, &work_data, K_NO_WAIT);
	if (ret) {
		LOG_ERR("Cannot get info for start stream");
		return;
	}

	LOG_DBG("k_msg_get: ch: %d, dir: %d, retries %d", work_data.channel_index, work_data.dir,
		work_data.retries);

	if (work_data.dir == BT_AUDIO_DIR_SINK) {
		ret = bt_bap_stream_start(&headsets[work_data.channel_index].sink_stream);
	} else if (work_data.dir == BT_AUDIO_DIR_SOURCE) {
		ret = bt_bap_stream_start(&headsets[work_data.channel_index].source_stream);
	} else {
		LOG_ERR("Trying to use unknown direction");
		bt_conn_disconnect(headsets[work_data.channel_index].headset_conn,
				   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	work_data.retries++;

	if ((ret == -EBUSY) && (work_data.retries < CIS_CONN_RETRY_TIMES)) {
		LOG_DBG("Got connect error from stream %d Retrying. code: %d count: %d",
			work_data.channel_index, ret, work_data.retries);
		LOG_DBG("k_msg_put: ch: %d, dir: %d, retries %d", work_data.channel_index,
			work_data.dir, work_data.retries);
		ret = k_msgq_put(&kwork_msgq, &work_data, K_NO_WAIT);
		if (ret) {
			LOG_ERR("No space in the queue for work_data");
			return;
		}
		/* Delay added to prevent controller overloading */
		if (work_data.dir == BT_AUDIO_DIR_SINK) {
			k_work_reschedule(&headsets[work_data.channel_index].stream_start_sink_work,
					  K_MSEC(CIS_CONN_RETRY_DELAY_MS));
		} else if (work_data.dir == BT_AUDIO_DIR_SOURCE) {
			k_work_reschedule(
				&headsets[work_data.channel_index].stream_start_source_work,
				K_MSEC(CIS_CONN_RETRY_DELAY_MS));
		}
	} else if (ret != 0) {
		LOG_ERR("Failed to establish CIS, ret = %d", ret);
		bt_conn_disconnect(headsets[work_data.channel_index].headset_conn,
				   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

/**
 * @brief Set the allocation to a preset codec configuration.
 *
 * @param codec The preset codec configuration.
 * @param loc   Location bitmask setting.
 *
 */
static void bt_codec_allocation_set(struct bt_codec *codec, enum bt_audio_location loc)
{
	for (size_t i = 0; i < codec->data_count; i++) {
		if (codec->data[i].data.type == BT_CODEC_CONFIG_LC3_CHAN_ALLOC) {
			codec->data[i].value[0] = loc & 0xff;
			codec->data[i].value[1] = (loc >> 8) & 0xFFU;
			codec->data[i].value[2] = (loc >> 16) & 0xFFU;
			codec->data[i].value[3] = (loc >> 24) & 0xFFU;
			codec->data[i].data.data = codec->data[i].value;
			return;
		}
	}
}

static int temp_cap_index_get(struct bt_conn *conn, uint8_t *index)
{
	if (conn == NULL) {
		LOG_ERR("No conn provided");
		return -EINVAL;
	}

	for (int i = 0; i < ARRAY_SIZE(temp_cap); i++) {
		if (temp_cap[i].conn == conn) {
			*index = i;
			return 0;
		}
	}

	/* Connection not found in temp_cap, searching for empty slot */
	for (int i = 0; i < ARRAY_SIZE(temp_cap); i++) {
		if (temp_cap[i].conn == NULL) {
			temp_cap[i].conn = conn;
			*index = i;
			return 0;
		}
	}

	LOG_WRN("No more space in temp_cap");

	return -ECANCELED;
}

/**
 * @brief  Check if the gateway can support the headset codec capabilities
 *
 * @note   Currently only the sampling frequency is checked
 *
 * @param  cap_array  The array of pointers to codec capabilities
 * @param  size       The size of cap_array
 *
 * @return True if valid codec capability found, false otherwise
 */
static bool valid_codec_cap_check(struct bt_codec cap_array[], size_t size)
{
	const struct bt_codec_data *element;

	/* Only the sampling frequency is checked */
	for (int i = 0; i < size; i++) {
		if (bt_codec_get_val(&cap_array[i], BT_CODEC_LC3_FREQ, &element)) {
			if (element->data.data[0] & BT_AUDIO_CODEC_CAPABILIY_FREQ) {
				return true;
			}
		}
	}

	return false;
}

static void discover_sink_cb(struct bt_conn *conn, struct bt_codec *codec, struct bt_bap_ep *ep,
			     struct bt_bap_unicast_client_discover_params *params)
{
	int ret = 0;
	uint8_t channel_index = 0;
	uint8_t temp_cap_index = 0;

	if (params->err == BT_ATT_ERR_ATTRIBUTE_NOT_FOUND) {
		LOG_WRN("No sinks found");
		return;
	} else if (params->err) {
		LOG_ERR("Discovery failed: %d", params->err);
		return;
	}

	ret = temp_cap_index_get(conn, &temp_cap_index);
	if (ret) {
		LOG_ERR("Could not get temporary CAP storage index");
		return;
	}

	if (codec != NULL) {
		if (codec->id != BT_CODEC_LC3_ID) {
			LOG_DBG("Only the LC3 codec is supported");
			return;
		}

		/* params->num_caps is an increasing index that starts at 0 */
		if (params->num_caps <= ARRAY_SIZE(temp_cap[temp_cap_index].codec)) {
			struct bt_codec *store_loc =
				&temp_cap[temp_cap_index].codec[params->num_caps];

			memcpy(store_loc, codec, sizeof(struct bt_codec));

			for (int i = 0; i < codec->data_count; i++) {
				store_loc->data[i].data.data = store_loc->data[i].value;
			}

			for (int i = 0; i < codec->meta_count; i++) {
				store_loc->meta[i].data.data = store_loc->meta[i].value;
			}
		} else {
			LOG_WRN("No more space. Increase CODEC_CAPAB_COUNT_MAX");
		}

		return;
	}

	ret = channel_index_get(conn, &channel_index);
	if (ret) {
		LOG_ERR("Unknown connection, should not reach here");
		return;
	}

	/* At this point the location/channel index of the headset is always known */
	for (int i = 0; i < CONFIG_CODEC_CAP_COUNT_MAX; i++) {
		memcpy(&headsets[channel_index].sink_codec_cap[i], &temp_cap[temp_cap_index].codec,
		       (sizeof(struct bt_codec)));
	}

	if (ep != NULL) {
		/* params->num_eps is an increasing index that starts at 0 */
		if (params->num_eps > 0) {
			LOG_WRN("More than one sink endpoints found, ep idx 0 is used by default");
			return;
		}

		headsets[channel_index].sink_ep = ep;
		return;
	}

	if (headsets[channel_index].sink_ep == NULL) {
		LOG_WRN("No sink endpoints found");
		return;
	}

	LOG_DBG("Sink discover complete: err %d", params->err);

	(void)memset(params, 0, sizeof(*params));

#if (CONFIG_BT_VCP_VOL_CTLR)
	ret = ble_vcs_discover(conn, channel_index);
	if (ret) {
		LOG_ERR("Could not do VCS discover");
	}
#endif /* (CONFIG_BT_VCP_VOL_CTLR ) */

	if (valid_codec_cap_check(headsets[channel_index].sink_codec_cap,
				  ARRAY_SIZE(headsets[channel_index].sink_codec_cap))) {
		if (conn == headsets[AUDIO_CH_L].headset_conn) {
			bt_codec_allocation_set(&lc3_preset_sink.codec,
						BT_AUDIO_LOCATION_FRONT_LEFT);
		} else if (conn == headsets[AUDIO_CH_R].headset_conn) {
			bt_codec_allocation_set(&lc3_preset_sink.codec,
						BT_AUDIO_LOCATION_FRONT_RIGHT);
		} else {
			LOG_ERR("Unknown connection, cannot set allocation");
		}

		ret = bt_bap_stream_config(conn, &headsets[channel_index].sink_stream,
					   headsets[channel_index].sink_ep, &lc3_preset_sink.codec);
		if (ret) {
			LOG_ERR("Could not configure sink stream");
		}
	} else {
		LOG_WRN("No valid codec capability found for %s headset sink",
			headsets[channel_index].ch_name);
	}

	/* Free up the slot in temp_cap */
	memset(temp_cap[temp_cap_index].codec, 0, sizeof(temp_cap[temp_cap_index].codec));
	temp_cap[temp_cap_index].conn = NULL;

#if (CONFIG_BT_AUDIO_RX)
	ret = discover_source(conn);
	if (ret) {
		LOG_WRN("Failed to discover source: %d", ret);
	}
#endif /* (CONFIG_BT_AUDIO_RX) */
}

#if (CONFIG_BT_AUDIO_RX)
static void discover_source_cb(struct bt_conn *conn, struct bt_codec *codec, struct bt_bap_ep *ep,
			       struct bt_bap_unicast_client_discover_params *params)
{
	int ret = 0;

	uint8_t channel_index = 0;
	uint8_t temp_cap_index = 0;

	if (params->err == BT_ATT_ERR_ATTRIBUTE_NOT_FOUND) {
		LOG_WRN("No sources found");
		return;
	} else if (params->err) {
		LOG_ERR("Discovery failed: %d", params->err);
		return;
	}

	ret = temp_cap_index_get(conn, &temp_cap_index);
	if (ret) {
		LOG_ERR("Could not get temporary CAP storage index");
		return;
	}

	if (codec != NULL) {
		if (codec->id != BT_CODEC_LC3_ID) {
			LOG_DBG("Only the LC3 codec is supported");
			return;
		}

		/* params->num_caps is an increasing index that starts at 0 */
		if (params->num_caps <= ARRAY_SIZE(temp_cap[temp_cap_index].codec)) {
			struct bt_codec *store_loc =
				&temp_cap[temp_cap_index].codec[params->num_caps];

			memcpy(store_loc, codec, sizeof(struct bt_codec));

			for (int i = 0; i < codec->data_count; i++) {
				store_loc->data[i].data.data = store_loc->data[i].value;
			}

			for (int i = 0; i < codec->meta_count; i++) {
				store_loc->meta[i].data.data = store_loc->meta[i].value;
			}
		} else {
			LOG_WRN("No more space. Increase CODEC_CAPAB_COUNT_MAX");
		}

		return;
	}

	ret = channel_index_get(conn, &channel_index);
	if (ret) {
		LOG_ERR("Unknown connection, should not reach here");
		return;
	}

	/* At this point the location/channel index of the headset is always known */
	for (int i = 0; i < CONFIG_CODEC_CAP_COUNT_MAX; i++) {
		memcpy(&headsets[channel_index].source_codec_cap[i],
		       &temp_cap[temp_cap_index].codec, (sizeof(struct bt_codec)));
	}

	if (ep != NULL) {
		/* params->num_eps is an increasing index that starts at 0 */
		if (params->num_eps > 0) {
			LOG_WRN("More than one src endpoints found, ep idx 0 is used by default");
			return;
		}

		headsets[channel_index].source_ep = ep;
		return;
	}

	if (headsets[channel_index].source_ep == NULL) {
		LOG_WRN("No source endpoints found");
		return;
	}

	LOG_DBG("Source discover complete: err %d", params->err);

	(void)memset(params, 0, sizeof(*params));

	if (valid_codec_cap_check(headsets[channel_index].source_codec_cap,
				  ARRAY_SIZE(headsets[channel_index].source_codec_cap))) {
		if (conn == headsets[AUDIO_CH_L].headset_conn) {
			bt_codec_allocation_set(&lc3_preset_source.codec,
						BT_AUDIO_LOCATION_FRONT_LEFT);
		} else if (conn == headsets[AUDIO_CH_R].headset_conn) {
			bt_codec_allocation_set(&lc3_preset_source.codec,
						BT_AUDIO_LOCATION_FRONT_RIGHT);
		} else {
			LOG_ERR("Unknown connection, cannot set allocation");
		}

		ret = bt_bap_stream_config(conn, &headsets[channel_index].source_stream,
					   headsets[channel_index].source_ep,
					   &lc3_preset_source.codec);
		if (ret) {
			LOG_WRN("Could not configure stream");
		}
	} else {
		LOG_WRN("No valid codec capability found for %s headset source",
			headsets[channel_index].ch_name);
	}

	/* Free up the slot in temp_cap */
	memset(temp_cap[temp_cap_index].codec, 0, sizeof(temp_cap[temp_cap_index].codec));
	temp_cap[temp_cap_index].conn = NULL;
}
#endif /* (CONFIG_BT_AUDIO_RX) */

static bool ble_acl_gateway_all_links_connected(void)
{
	for (int i = 0; i < ARRAY_SIZE(headsets); i++) {
		if (headsets[i].headset_conn == NULL) {
			return false;
		}
	}

	return true;
}

static void bond_check(const struct bt_bond_info *info, void *user_data)
{
	char addr_buf[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(&info->addr, addr_buf, BT_ADDR_LE_STR_LEN);

	LOG_DBG("Stored bonding found: %s", addr_buf);
	bonded_num++;
}

static void bond_connect(const struct bt_bond_info *info, void *user_data)
{
	int ret;
	const bt_addr_le_t *adv_addr = user_data;
	struct bt_conn *conn;

	if (!bt_addr_le_cmp(&info->addr, adv_addr)) {
		LOG_DBG("Found bonded device");

		ret = bt_le_scan_stop();
		if (ret) {
			LOG_WRN("Stop scan failed: %d", ret);
		}

		ret = bt_conn_le_create(adv_addr, BT_CONN_LE_CREATE_CONN, CONNECTION_PARAMETERS,
					&conn);
		if (ret) {
			LOG_WRN("Create ACL connection failed: %d", ret);
			ble_acl_start_scan();
		}
	}
}

static int device_found(uint8_t type, const uint8_t *data, uint8_t data_len,
			const bt_addr_le_t *addr)
{
	int ret;
	struct bt_conn *conn;

	if (ble_acl_gateway_all_links_connected()) {
		LOG_DBG("All headset connected");
		return 0;
	}

	if ((data_len == DEVICE_NAME_PEER_LEN) &&
	    (strncmp(DEVICE_NAME_PEER, data, DEVICE_NAME_PEER_LEN) == 0)) {
		LOG_DBG("Device found");

		ret = bt_le_scan_stop();
		if (ret) {
			LOG_WRN("Stop scan failed: %d", ret);
		}

		ret = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, CONNECTION_PARAMETERS, &conn);
		if (ret) {
			LOG_ERR("Could not init connection");
			ble_acl_start_scan();
			return ret;
		}

		return 0;
	}

	return -ENOENT;
}

/** @brief  Parse BLE advertisement package.
 */
static void ad_parse(struct net_buf_simple *p_ad, const bt_addr_le_t *addr)
{
	while (p_ad->len > 1) {
		uint8_t len = net_buf_simple_pull_u8(p_ad);
		uint8_t type;

		/* Check for early termination */
		if (len == 0) {
			return;
		}

		if (len > p_ad->len) {
			LOG_WRN("AD malformed");
			return;
		}

		type = net_buf_simple_pull_u8(p_ad);

		if (device_found(type, p_ad->data, len - 1, addr) == 0) {
			return;
		}

		(void)net_buf_simple_pull(p_ad, len - 1);
	}
}

static void on_device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			    struct net_buf_simple *p_ad)
{
	switch (type) {
	case BT_GAP_ADV_TYPE_ADV_DIRECT_IND:
		/* Direct advertising has no payload, so no need to parse */
		if (bonded_num) {
			bt_foreach_bond(BT_ID_DEFAULT, bond_connect, (void *)addr);
		}
		break;
	case BT_GAP_ADV_TYPE_ADV_IND:
		/* Fall through */
	case BT_GAP_ADV_TYPE_EXT_ADV:
		/* Fall through */
	case BT_GAP_ADV_TYPE_SCAN_RSP:
		/* Note: May lead to connection creation */
		if (bonded_num < CONFIG_BT_MAX_PAIRED) {
			ad_parse(p_ad, addr);
		}
		break;
	default:
		break;
	}
}

static void ble_acl_start_scan(void)
{
	int ret;
	/* Scan interval as two times of ISO interval */
	int scan_interval =
		bt_codec_cfg_get_frame_duration_us(&lc3_preset_sink.codec) / 1000 / 0.625 * 2;
	/* Scan window as two times of ISO interval */
	int scan_window =
		bt_codec_cfg_get_frame_duration_us(&lc3_preset_sink.codec) / 1000 / 0.625 * 2;
	struct bt_le_scan_param *scan_param =
		BT_LE_SCAN_PARAM(NRF5340_AUDIO_GATEWAY_SCAN_TYPE, BT_LE_SCAN_OPT_FILTER_DUPLICATE,
				 scan_interval, scan_window);

	/* Reset number of bonds found */
	bonded_num = 0;

	bt_foreach_bond(BT_ID_DEFAULT, bond_check, NULL);

	if (bonded_num >= CONFIG_BT_MAX_PAIRED) {
		LOG_INF("All bonded slots filled, will not accept new devices");
	}

	ret = bt_le_scan_start(scan_param, on_device_found);
	if (ret && ret != -EALREADY) {
		LOG_WRN("Scanning failed to start: %d", ret);
		return;
	}

	LOG_INF("Scanning successfully started");
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	int ret;
	char addr[BT_ADDR_LE_STR_LEN];
	uint16_t conn_handle;
	enum ble_hci_vs_tx_power conn_tx_pwr;

	(void)bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		LOG_ERR("ACL connection to %s failed, error %d", addr, err);

		bt_conn_unref(conn);
		ble_acl_start_scan();

		return;
	}

	/* ACL connection established */
	LOG_INF("Connected: %s", addr);

	ret = bt_hci_get_conn_handle(conn, &conn_handle);
	if (ret) {
		LOG_ERR("Unable to get conn handle");
	} else {
#if (CONFIG_NRF_21540_ACTIVE)
		conn_tx_pwr = CONFIG_NRF_21540_MAIN_DBM;
#else
		conn_tx_pwr = CONFIG_BLE_CONN_TX_POWER_DBM;
#endif /* (CONFIG_NRF_21540_ACTIVE) */
		ret = ble_hci_vsc_conn_tx_pwr_set(conn_handle, conn_tx_pwr);
		if (ret) {
			LOG_ERR("Failed to set TX power for conn");
		} else {
			LOG_DBG("TX power set to %d dBm for connection %p", conn_tx_pwr,
				(void *)conn);
		}
	}

	ret = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (ret) {
		LOG_ERR("Failed to set security to L2: %d", ret);
	}
}

static void disconnected_headset_cleanup(uint8_t chan_idx)
{
	headsets[chan_idx].headset_conn = NULL;
	k_work_cancel_delayable(&headsets[chan_idx].stream_start_sink_work);
	headsets[chan_idx].sink_ep = NULL;
	memset(headsets[chan_idx].sink_codec_cap, 0, sizeof(headsets[chan_idx].sink_codec_cap));
	k_work_cancel_delayable(&headsets[chan_idx].stream_start_source_work);
	headsets[chan_idx].source_ep = NULL;
	memset(headsets[chan_idx].source_codec_cap, 0, sizeof(headsets[chan_idx].source_codec_cap));
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	int ret;
	uint8_t channel_index;
	char addr[BT_ADDR_LE_STR_LEN];

	(void)bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);

	bt_conn_unref(conn);

	ret = channel_index_get(conn, &channel_index);
	if (ret) {
		LOG_WRN("Unknown connection");
	} else {
		disconnected_headset_cleanup(channel_index);
	}

	ble_acl_start_scan();
}

static int discover_sink(struct bt_conn *conn)
{
	int ret = 0;
	static uint8_t discover_index;

	audio_sink_discover_param[discover_index].func = discover_sink_cb;
	audio_sink_discover_param[discover_index].dir = BT_AUDIO_DIR_SINK;
	ret = bt_bap_unicast_client_discover(conn, &audio_sink_discover_param[discover_index]);
	discover_index++;

	/* Avoid multiple discover procedure sharing the same params */
	if (discover_index >= CONFIG_BT_MAX_CONN) {
		discover_index = 0;
	}

	return ret;
}

#if (CONFIG_BT_AUDIO_RX)
static int discover_source(struct bt_conn *conn)
{
	int ret = 0;
	static uint8_t discover_index;

	audio_source_discover_param[discover_index].func = discover_source_cb;
	audio_source_discover_param[discover_index].dir = BT_AUDIO_DIR_SOURCE;
	ret = bt_bap_unicast_client_discover(conn, &audio_source_discover_param[discover_index]);
	discover_index++;

	/* Avoid multiple discover procedure sharing the same params */
	if (discover_index >= CONFIG_BT_MAX_CONN) {
		discover_index = 0;
	}

	return ret;
}
#endif /* (CONFIG_BT_AUDIO_RX) */

static void security_changed_cb(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	int ret;

	if (err) {
		LOG_ERR("Security failed: level %d err %d", level, err);
		ret = bt_conn_disconnect(conn, err);
		if (ret) {
			LOG_ERR("Failed to disconnect %d", ret);
		}
	} else {
		LOG_DBG("Security changed: level %d", level);
		ret = discover_sink(conn);
		if (ret) {
			LOG_WRN("Failed to discover sink: %d", ret);
		}
	}
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
	.security_changed = security_changed_cb,
};

#if (CONFIG_BT_MCS)
/**
 * @brief	Callback handler for play/pause.
 *
 * @note	This callback is called from MCS after receiving a
 *		command from the client or the local media player.
 *
 * @param[in]	play  Boolean to indicate if the stream should be resumed or paused.
 */
static void le_audio_play_pause_cb(bool play)
{
	int ret;

	LOG_DBG("Play/pause cb, state: %d", play);

	if (play) {
		if (ep_state_check(headsets[AUDIO_CH_L].sink_stream.ep,
				   BT_BAP_EP_STATE_QOS_CONFIGURED)) {
			ret = bt_bap_stream_enable(&headsets[AUDIO_CH_L].sink_stream,
						   lc3_preset_sink.codec.meta,
						   lc3_preset_sink.codec.meta_count);

			if (ret) {
				LOG_WRN("Failed to enable left stream");
			}
		}

		if (ep_state_check(headsets[AUDIO_CH_R].sink_stream.ep,
				   BT_BAP_EP_STATE_QOS_CONFIGURED)) {
			ret = bt_bap_stream_enable(&headsets[AUDIO_CH_R].sink_stream,
						   lc3_preset_sink.codec.meta,
						   lc3_preset_sink.codec.meta_count);

			if (ret) {
				LOG_WRN("Failed to enable right stream");
			}
		}
	} else {
		le_audio_event_publish(LE_AUDIO_EVT_NOT_STREAMING);

		if (ep_state_check(headsets[AUDIO_CH_L].sink_stream.ep,
				   BT_BAP_EP_STATE_STREAMING)) {
			ret = bt_bap_stream_disable(&headsets[AUDIO_CH_L].sink_stream);

			if (ret) {
				LOG_WRN("Failed to disable left stream");
			}
		}

		if (ep_state_check(headsets[AUDIO_CH_R].sink_stream.ep,
				   BT_BAP_EP_STATE_STREAMING)) {
			ret = bt_bap_stream_disable(&headsets[AUDIO_CH_R].sink_stream);

			if (ret) {
				LOG_WRN("Failed to disable right stream");
			}
		}
	}

	playing_state = play;
}
#endif /* CONFIG_BT_MCS */

static int iso_stream_send(uint8_t const *const data, size_t size, struct le_audio_headset headset)
{
	int ret;
	struct net_buf *buf;

	/* net_buf_alloc allocates buffers for APP->NET transfer over HCI RPMsg,
	 * but when these buffers are released it is not guaranteed that the
	 * data has actually been sent. The data might be queued on the NET core,
	 * and this can cause delays in the audio.
	 * When stream_sent_cb() is called the data has been sent.
	 * Data will be discarded if allocation becomes too high, to avoid audio delays.
	 * If the NET and APP core operates in clock sync, discarding should not occur.
	 */
	if (atomic_get(&headset.iso_tx_pool_alloc) >= HCI_ISO_BUF_ALLOC_PER_CHAN) {
		if (!headset.hci_wrn_printed) {
			LOG_WRN("HCI ISO TX overrun on %s ch - Single print", headset.ch_name);
			headset.hci_wrn_printed = true;
		}

		return -ENOMEM;
	}

	headset.hci_wrn_printed = false;

	buf = net_buf_alloc(headset.iso_tx_pool, K_NO_WAIT);
	if (buf == NULL) {
		/* This should never occur because of the iso_tx_pool_alloc check above */
		LOG_WRN("Out of TX buffers");
		return -ENOMEM;
	}

	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
	net_buf_add_mem(buf, data, size);

	atomic_inc(&headset.iso_tx_pool_alloc);

	ret = bt_bap_stream_send(&headset.sink_stream, buf,
				 get_and_incr_seq_num(&headset.sink_stream), BT_ISO_TIMESTAMP_NONE);
	if (ret < 0) {
		LOG_WRN("Failed to send audio data to %s: %d", headset.ch_name, ret);
		net_buf_unref(buf);
		atomic_dec(&headset.iso_tx_pool_alloc);
	}

	return 0;
}

static int initialize(le_audio_receive_cb recv_cb, le_audio_timestamp_cb timestmp_cb)
{
	int ret;
	static bool initialized;
	int headset_iterator = 0;
	int stream_iterator = 0;
	struct bt_bap_unicast_group_stream_pair_param pair_params[ARRAY_SIZE(headsets)];
	/* 2 streams (one sink and one source stream) for each headset */
	struct bt_bap_unicast_group_stream_param stream_params[(ARRAY_SIZE(headsets) * 2)];
	struct bt_bap_unicast_group_param param;

	if (initialized) {
		LOG_WRN("Already initialized");
		return -EALREADY;
	}

	if (recv_cb == NULL) {
		LOG_ERR("Receieve callback is NULL");
		return -EINVAL;
	}

	if (timestmp_cb == NULL) {
		LOG_ERR("Timestamp callback is NULL");
		return -EINVAL;
	}

	receive_cb = recv_cb;
	timestamp_cb = timestmp_cb;

	for (int i = 0; i < ARRAY_SIZE(headsets); i++) {
		headsets[i].sink_stream.ops = &stream_ops;
		headsets[i].source_stream.ops = &stream_ops;
		k_work_init_delayable(&headsets[i].stream_start_sink_work, work_stream_start);
		k_work_init_delayable(&headsets[i].stream_start_source_work, work_stream_start);
	}

	ret = bt_bap_unicast_client_register_cb(&unicast_client_cbs);
	if (ret != 0) {
		LOG_ERR("Failed to register client callbacks: %d", ret);
		return ret;
	}

	for (int i = 0; i < ARRAY_SIZE(headsets); i++) {
		headsets[i].iso_tx_pool = iso_tx_pools[i];
		switch (i) {
		case AUDIO_CH_L:
			headsets[i].ch_name = "LEFT";
			break;
		case AUDIO_CH_R:
			headsets[i].ch_name = "RIGHT";
			break;
		default:
			LOG_WRN("Trying to set name to undefined channel");
			headsets[i].ch_name = "UNDEFINED";
			break;
		}
	}

	for (int i = 0; i < ARRAY_SIZE(stream_params); i++) {
		/* Every other stream should be sink or source */
		if ((i % 2) == 0) {
			stream_params[i].qos = &lc3_preset_sink.qos;
			stream_params[i].stream = &headsets[headset_iterator].sink_stream;
		} else {
			stream_params[i].qos = &lc3_preset_source.qos;
			stream_params[i].stream = &headsets[headset_iterator].source_stream;
			headset_iterator++;
		}
	}

	for (int i = 0; i < ARRAY_SIZE(pair_params); i++) {
		pair_params[i].tx_param = &stream_params[stream_iterator];
		stream_iterator++;
#if (CONFIG_BT_AUDIO_RX)
		pair_params[i].rx_param = &stream_params[stream_iterator];
#else
		pair_params[i].rx_param = NULL;
#endif /* (CONFIG_BT_AUDIO_RX) */
		stream_iterator++;
	}

	param.params = pair_params;
	param.params_count = ARRAY_SIZE(pair_params);

	if (IS_ENABLED(CONFIG_BT_AUDIO_PACKING_INTERLEAVED)) {
		param.packing = BT_ISO_PACKING_INTERLEAVED;
	} else {
		param.packing = BT_ISO_PACKING_SEQUENTIAL;
	}

	ret = bt_bap_unicast_group_create(&param, &unicast_group);
	if (ret) {
		LOG_ERR("Failed to create unicast group: %d", ret);
		return ret;
	}

#if (CONFIG_BT_VCP_VOL_CTLR)
	ret = ble_vcs_client_init();
	if (ret) {
		LOG_ERR("VCS client init failed");
		return ret;
	}
#endif /* (CONFIG_BT_VCP_VOL_CTLR) */

#if (CONFIG_BT_MCS)
	ret = ble_mcs_server_init(le_audio_play_pause_cb);
	if (ret) {
		LOG_ERR("MCS server init failed");
		return ret;
	}
#endif /* CONFIG_BT_MCS */

	bt_conn_cb_register(&conn_callbacks);

	initialized = true;

	return 0;
}

int le_audio_user_defined_button_press(enum le_audio_user_defined_action action)
{
	return 0;
}

int le_audio_config_get(uint32_t *bitrate, uint32_t *sampling_rate, uint32_t *pres_delay)
{
	LOG_WRN("Getting config from gateway is not yet supported");
	return -ENOTSUP;
}

int le_audio_volume_up(void)
{
	int ret;

	ret = ble_vcs_volume_up();
	if (ret) {
		LOG_WRN("Failed to increase volume");
		return ret;
	}

	return 0;
}

int le_audio_volume_down(void)
{
	int ret;

	ret = ble_vcs_volume_down();
	if (ret) {
		LOG_WRN("Failed to decrease volume");
		return ret;
	}

	return 0;
}

int le_audio_volume_mute(void)
{
	int ret;

	ret = ble_vcs_volume_mute();
	if (ret) {
		LOG_WRN("Failed to mute volume");
		return ret;
	}

	return 0;
}

int le_audio_play_pause(void)
{
	int ret;

	if (IS_ENABLED(CONFIG_STREAM_BIDIRECTIONAL)) {
		LOG_WRN("Play/pause not supported for bidirectional mode");
	} else {
		ret = ble_mcs_play_pause(NULL);
		if (ret) {
			LOG_WRN("Failed to change streaming state");
			return ret;
		}
	}

	return 0;
}

int le_audio_send(struct encoded_audio enc_audio)
{
	int ret;
	size_t data_size_pr_stream;
	struct bt_iso_tx_info tx_info = { 0 };

	if ((enc_audio.num_ch == 1) || (enc_audio.num_ch == ARRAY_SIZE(headsets))) {
		data_size_pr_stream = enc_audio.size / enc_audio.num_ch;
	} else {
		LOG_ERR("Num encoded channels must be 1 or equal to num headsets");
		return -EINVAL;
	}

	if (data_size_pr_stream != LE_AUDIO_SDU_SIZE_OCTETS(CONFIG_BT_AUDIO_BITRATE_UNICAST_SINK)) {
		LOG_ERR("The encoded data size does not match the SDU size");
		return -ECANCELED;
	}

	if (ep_state_check(headsets[AUDIO_CH_L].sink_stream.ep, BT_BAP_EP_STATE_STREAMING)) {
		ret = bt_iso_chan_get_tx_sync(&headsets[AUDIO_CH_L].sink_stream.ep->iso->chan,
					      &tx_info);
	} else if (ep_state_check(headsets[AUDIO_CH_R].sink_stream.ep, BT_BAP_EP_STATE_STREAMING)) {
		ret = bt_iso_chan_get_tx_sync(&headsets[AUDIO_CH_R].sink_stream.ep->iso->chan,
					      &tx_info);
	} else {
		LOG_ERR("No headset in stream state");
		return -ECANCELED;
	}

	if (ret) {
		LOG_DBG("Error getting ISO TX anchor point: %d", ret);
	}

	if (tx_info.ts != 0 && !ret) {
		if (!IS_ENABLED(CONFIG_STREAM_BIDIRECTIONAL)) {
			timestamp_cb(tx_info.ts, true);
		}
	}

	if (ep_state_check(headsets[AUDIO_CH_L].sink_stream.ep, BT_BAP_EP_STATE_STREAMING)) {
		ret = iso_stream_send(enc_audio.data, data_size_pr_stream, headsets[AUDIO_CH_L]);
		if (ret) {
			LOG_DBG("Failed to send data to left channel");
		}
	}

	if (ep_state_check(headsets[AUDIO_CH_R].sink_stream.ep, BT_BAP_EP_STATE_STREAMING)) {
		if (enc_audio.num_ch == 1) {
			ret = iso_stream_send(enc_audio.data, data_size_pr_stream,
					      headsets[AUDIO_CH_R]);
		} else {
			ret = iso_stream_send(&enc_audio.data[data_size_pr_stream],
					      data_size_pr_stream, headsets[AUDIO_CH_R]);
		}
		if (ret) {
			LOG_DBG("Failed to send data to right channel");
		}
	}

	return 0;
}

int le_audio_enable(le_audio_receive_cb recv_cb, le_audio_timestamp_cb timestmp_cb)
{
	int ret;

	ret = initialize(recv_cb, timestmp_cb);
	if (ret) {
		return ret;
	}

	ble_acl_start_scan();
	return 0;
}

int le_audio_disable(void)
{
	return 0;
}
