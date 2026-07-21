#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include <modem/location.h>
#include <nrf_modem_gnss.h>

#include "agnss_data.h"
#include "app_common.h"
#include "location.h"
#include "network.h"
#include "utils.h"

LOG_MODULE_REGISTER(agnss_data, CONFIG_APP_AGNSS_DATA_LOG_LEVEL);

/* Buffer for storing the A-GNSS response data from the proxy. */
#define AGNSS_DATA_BUF_SIZE CONFIG_APP_AGNSS_DATA_BUFFER_SIZE

/* ---------------------------------------------------------------------------
 * zbus channels and subscription
 * -------------------------------------------------------------------------*/

ZBUS_CHAN_DEFINE(agnss_data_chan, struct agnss_data_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(agnss_data);

/* Observe the location channel for A-GNSS assistance requests. */
ZBUS_CHAN_ADD_OBS(location_chan, agnss_data, 0);
/* Observe the network channel to track LTE connectivity. */
ZBUS_CHAN_ADD_OBS(network_chan, agnss_data, 0);

/* ---------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------*/

static struct {
    /** Whether LTE is currently connected. */
    bool lte_connected;
    bool connect_requested;
    struct nrf_modem_gnss_agnss_data_frame cached_request;
    bool request_pending;
    bool initialized;
} mod;

/* ---------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/

static void request_lte_connect(void) {
    if (mod.connect_requested) {
        return;
    }
    const struct network_msg msg = {.type = NETWORK_CONNECT};
    int err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to request NETWORK_CONNECT: %d", err);
        return;
    }
    mod.connect_requested = true;
    LOG_INF("LTE connect requested");
}

static void publish_result(enum agnss_data_msg_type type, int http_status) {
    const struct agnss_data_msg msg = {
        .type = type,
        .http_status = http_status,
    };

    int err = zbus_chan_pub(&agnss_data_chan, &msg, PUB_TIMEOUT);

    if (err) {
        LOG_ERR("Failed to publish AGNSS data result: %d", err);
    }
}

/* ---------------------------------------------------------------------------
 * A-GNSS data fetching
 * -------------------------------------------------------------------------*/

static int fetch_agnss_data(const struct nrf_modem_gnss_agnss_data_frame *agnss_req) {
    char request_body[512];
    int body_len = snprintf(request_body, sizeof(request_body), "{");
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "\"data_flags\":%u,",
                         (unsigned int)agnss_req->data_flags);
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "\"system_count\":%u,\"system\":[",
                         (unsigned int)agnss_req->system_count);

    for (uint8_t i = 0; i < agnss_req->system_count && i < NRF_MODEM_GNSS_MAX_SYSTEMS; i++) {
        if (i > 0) {
            body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, ",");
        }
        body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len,
                             "{\"system_id\":%u,\"sv_mask_ephe\":%llu,\"sv_mask_alm\":%llu}",
                             (unsigned int)agnss_req->system[i].system_id,
                             (unsigned long long)agnss_req->system[i].sv_mask_ephe,
                             (unsigned long long)agnss_req->system[i].sv_mask_alm);
    }
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "]}");

    if (body_len < 0 || body_len >= (int)sizeof(request_body)) {
        LOG_ERR("Failed to serialise A-GNSS request");
        return -ENOMEM;
    }

#define AGNSS_CHUNK_SIZE 1400
    static uint8_t full_buf[AGNSS_DATA_BUF_SIZE];
    size_t total_len = 0;
    int err = http_fetch_chunked(CONFIG_APP_AGNSS_DATA_HOST, CONFIG_APP_AGNSS_DATA_PORT, CONFIG_APP_AGNSS_DATA_URL,
                                 CONFIG_APP_AGNSS_DATA_SEC_TAG, request_body, body_len, AGNSS_CHUNK_SIZE, full_buf,
                                 sizeof(full_buf), &total_len);
    if (err) {
        LOG_ERR("Failed to fetch A-GNSS data: %d", err);
        return err;
    }
    LOG_INF("A-GNSS data fetched: %zu bytes", total_len);
    err = location_agnss_data_process(full_buf, total_len);
    if (err) {
        LOG_ERR("location_agnss_data_process failed: %d", err);
        return err;
    }

    LOG_DBG("A-GNSS data fetched and processed successfully");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Message handling
 * -------------------------------------------------------------------------*/

static void handle_agnss_request(const struct nrf_modem_gnss_agnss_data_frame *agnss_req) {
    if (!mod.lte_connected) {
        LOG_DBG("LTE not connected, caching A-GNSS request for later");
        mod.cached_request = *agnss_req;
        mod.request_pending = true;
        request_lte_connect();
        return;
    }
    int err = fetch_agnss_data(agnss_req);
    if (err) {
        LOG_ERR("Failed to fetch A-GNSS data: %d", err);
        publish_result(AGNSS_DATA_FETCH_FAILED, err);
        return;
    }
    publish_result(AGNSS_DATA_FETCH_DONE, 200);
    mod.request_pending = false;
}

static void handle_network_event(const struct network_msg *msg) {
    switch (msg->type) {
        case NETWORK_CONNECTED:
            LOG_DBG("LTE connected");

            mod.lte_connected = true;
            mod.connect_requested = false;

            /* If we have a cached A-GNSS request, process it now. */
            if (mod.request_pending) {
                LOG_DBG("Processing cached A-GNSS request");
                int err = fetch_agnss_data(&mod.cached_request);

                if (err) {
                    LOG_ERR("Failed to fetch cached A-GNSS data: %d", err);
                    publish_result(AGNSS_DATA_FETCH_FAILED, err);
                } else {
                    publish_result(AGNSS_DATA_FETCH_DONE, 200);
                    mod.request_pending = false;
                }
            }
            break;

        case NETWORK_DISCONNECTED:
            LOG_DBG("LTE disconnected");
            mod.lte_connected = false;
            break;

        default:
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Module thread
 * -------------------------------------------------------------------------*/

TASK_WDT_CALLBACK_DEFINE(agnss_data)

static void agnss_data_module_thread(void *arg1, void *arg2, void *arg3) {
    int err;
    const struct zbus_channel *chan;

    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    TASK_WDT_TIMEOUTS(APP_AGNSS_DATA);
    TASK_WDT_ZBUS_TIMEOUT;

    TASK_WDT_ADD(agnss_data, wdt_timeout_ms)

    mod.initialized = true;
    LOG_DBG("A-GNSS data module started");

    while (true) {
        TASK_WDT_FEED();

        uint8_t msg_buf[MAX(sizeof(struct location_msg), sizeof(struct network_msg))];
        err = zbus_sub_wait_msg(&agnss_data, &chan, msg_buf, zbus_wait_ms);
        if (err == -ENOMSG) {
            continue;
        } else if (err) {
            LOG_ERR("zbus_sub_wait_msg failed: %d", err);
            SEND_FATAL_ERROR();
            return;
        }

        if (chan == &location_chan) {
            const struct location_msg *location_msg = (const struct location_msg *)msg_buf;
            if (location_msg->type == LOCATION_AGNSS_REQUEST) {
                LOG_DBG("A-GNSS assistance request received");
                handle_agnss_request(&location_msg->agnss_request);
            }
        } else if (chan == &network_chan) {
            const struct network_msg *network_msg = (const struct network_msg *)msg_buf;
            handle_network_event(network_msg);
        }
    }
}

K_THREAD_DEFINE(agnss_data_module_thread_id, CONFIG_APP_AGNSS_DATA_THREAD_STACK_SIZE, agnss_data_module_thread, NULL,
                NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
