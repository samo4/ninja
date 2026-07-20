#include <net/rest_client.h>
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

LOG_MODULE_REGISTER(agnss_data, CONFIG_APP_AGNSS_DATA_LOG_LEVEL);

/* Buffer for storing the A-GNSS response data from the proxy. */
#define AGNSS_DATA_BUF_SIZE CONFIG_APP_AGNSS_DATA_BUFFER_SIZE

#define REST_REQUEST_TIMEOUT_MS 15000

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

/**
 * @brief Fetch A-GNSS data from the configured proxy server.
 *
 * Sends the A-GNSS request parameters to the proxy HTTP endpoint.
 * The proxy is expected to return raw A-GNSS data that can be passed
 * directly to location_agnss_data_process().
 *
 * @param agnss_req  Pointer to the A-GNSS request parameters from the GNSS
 *                   subsystem.
 *
 * @return 0 on success, a negative errno on failure.
 */
static int fetch_agnss_data(const struct nrf_modem_gnss_agnss_data_frame *agnss_req) {
    int err;
    char resp_buf[AGNSS_DATA_BUF_SIZE];

    /*
     * Serialise the A-GNSS request parameters into a JSON payload that
     * the proxy server understands.  The proxy is expected to return
     * raw A-GNSS assistance data (the same format that
     * location_agnss_data_process() expects).
     */
    char request_body[512];
    int body_len = snprintf(request_body, sizeof(request_body),
                            "{"
                            "\"data_flags\":%u,"
                            "\"system_count\":%u"
                            "}",
                            (unsigned int)agnss_req->data_flags, (unsigned int)agnss_req->system_count);

    if (body_len < 0 || body_len >= (int)sizeof(request_body)) {
        LOG_ERR("Failed to serialise A-GNSS request");
        return -ENOMEM;
    }

    /* Prepare the REST request. */
    const char *header_fields[] = {"Content-Type: application/json\r\n", NULL};

    struct rest_client_req_context req = {0};
    struct rest_client_resp_context resp = {0};

    rest_client_request_defaults_set(&req);

    req.host = CONFIG_APP_AGNSS_DATA_HOST;
    req.port = CONFIG_APP_AGNSS_DATA_PORT;
    req.url = CONFIG_APP_AGNSS_DATA_URL;
    req.sec_tag = CONFIG_APP_AGNSS_DATA_SEC_TAG;
    req.tls_peer_verify = 0;
    req.http_method = HTTP_POST;
    req.header_fields = header_fields;
    req.body = request_body;
    req.body_len = body_len;
    req.resp_buff = resp_buf;
    req.resp_buff_len = sizeof(resp_buf);
    req.timeout_ms = REST_REQUEST_TIMEOUT_MS;

    LOG_DBG("Fetching A-GNSS data from %s:%d%s", CONFIG_APP_AGNSS_DATA_HOST, CONFIG_APP_AGNSS_DATA_PORT,
            CONFIG_APP_AGNSS_DATA_URL);

    err = rest_client_request(&req, &resp);
    if (err) {
        LOG_ERR("REST request to AGNSS proxy failed: %d", err);
        return err;
    }

    if (resp.http_status_code != 200) {
        LOG_ERR("AGNSS proxy returned HTTP %d", resp.http_status_code);
        return -EIO;
    }

    LOG_DBG("A-GNSS data received: %d bytes", resp.response_len);

    /* Feed the received data back to the GNSS subsystem. */
    err = location_agnss_data_process(resp.response, resp.response_len);
    if (err) {
        LOG_ERR("location_agnss_data_process failed: %d", err);
        return err;
    }

    LOG_INF("A-GNSS data fetched and processed successfully");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Message handling
 * -------------------------------------------------------------------------*/

static void handle_agnss_request(const struct nrf_modem_gnss_agnss_data_frame *agnss_req) {
    int err;

    if (!mod.lte_connected) {
        LOG_DBG("LTE not connected, caching A-GNSS request for later");

        mod.cached_request = *agnss_req;
        mod.request_pending = true;

        request_lte_connect();
        return;
    }

    err = fetch_agnss_data(agnss_req);
    if (err) {
        LOG_ERR("Failed to fetch A-GNSS data: %d", err);
        publish_result(AGNSS_DATA_FETCH_FAILED, err);
        return;
    }

    publish_result(AGNSS_DATA_FETCH_DONE, 200);

    /* Clear any previously cached request since we succeeded. */
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

static void agnss_data_wdt_callback(int channel_id, void *user_data) {
    LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id, k_thread_name_get((k_tid_t)user_data));

    SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void agnss_data_module_thread(void *arg1, void *arg2, void *arg3) {
    int err;
    const struct zbus_channel *chan;

    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    const uint32_t wdt_timeout_ms = CONFIG_APP_AGNSS_DATA_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
    const uint32_t execution_time_ms = CONFIG_APP_AGNSS_DATA_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC;
    const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);

    int task_wdt_id = task_wdt_add(wdt_timeout_ms, agnss_data_wdt_callback, (void *)k_current_get());
    if (task_wdt_id < 0) {
        LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
        SEND_FATAL_ERROR();
        return;
    }

    mod.initialized = true;
    LOG_DBG("A-GNSS data module started");

    while (true) {
        err = task_wdt_feed(task_wdt_id);
        if (err) {
            LOG_ERR("Failed to feed the watchdog: %d", err);
            SEND_FATAL_ERROR();
            return;
        }

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
