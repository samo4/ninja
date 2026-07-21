/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Location cloud module — unified cloud communication for location services.
 *
 * Handles two types of location-related cloud requests:
 *
 *   1. LOCATION_AGNSS_REQUEST  — A-GNSS assistance data (ephemerides,
 *      almanacs, etc.) is requested by the GNSS subsystem.  This module
 *      serialises the request into JSON, fetches the data from a configurable
 *      HTTP proxy, and feeds it back via location_agnss_data_process().
 *
 *   2. LOCATION_CLOUD_REQUEST  — Cellular (serving cell, neighbour cells,
 *      GCI cells) and/or Wi-Fi scan data is available for cloud-based
 *      positioning.  This module serialises the data into JSON, sends it to
 *      a configurable HTTP endpoint, and feeds the response back via
 *      location_cloud_location_ext_result_set().
 *
 * Both services share the same LTE-connectivity tracking: if LTE is not
 * connected when a request arrives, the module requests a network connection
 * and processes cached/pending requests once LTE is up.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include <modem/location.h>
#include <nrf_modem_gnss.h>

#include "app_common.h"
#include "location.h"
#include "location_cloud.h"
#include "network.h"
#include "utils.h"

LOG_MODULE_REGISTER(location_cloud, CONFIG_APP_LOCATION_CLOUD_LOG_LEVEL);

/* ---------------------------------------------------------------------------
 * zbus channels and subscription
 * -------------------------------------------------------------------------*/

ZBUS_CHAN_DEFINE(location_cloud_chan, struct location_cloud_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(location_cloud);

/* Observe the location channel for A-GNSS assistance and cloud location requests. */
ZBUS_CHAN_ADD_OBS(location_chan, location_cloud, 0);
/* Observe the network channel to track LTE connectivity. */
ZBUS_CHAN_ADD_OBS(network_chan, location_cloud, 0);

#define CHUNK_SIZE 1400

/* ---------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------*/

static struct {
    /** Whether LTE is currently connected. */
    bool lte_connected;
    bool connect_requested;

    /** Cached A-GNSS request (when LTE was not yet available). */
    struct nrf_modem_gnss_agnss_data_frame cached_agnss_request;
    bool agnss_pending;

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

static void publish_result(enum location_cloud_msg_type type, int http_status) {
    const struct location_cloud_msg msg = {
        .type = type,
        .http_status = http_status,
    };

    int err = zbus_chan_pub(&location_cloud_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish location cloud result: %d", err);
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
                             "{\"system_id\":%u,\"sv_mask_ephe\":%llu,"
                             "\"sv_mask_alm\":%llu}",
                             (unsigned int)agnss_req->system[i].system_id,
                             (unsigned long long)agnss_req->system[i].sv_mask_ephe,
                             (unsigned long long)agnss_req->system[i].sv_mask_alm);
    }
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "]}");

    if (body_len < 0 || body_len >= (int)sizeof(request_body)) {
        LOG_ERR("Failed to serialise A-GNSS request");
        return -ENOMEM;
    }

    LOG_INF("Sending to %s:%d%s: %s", CONFIG_APP_LOCATION_CLOUD_HOST, CONFIG_APP_LOCATION_CLOUD_PORT,
            CONFIG_APP_LOCATION_CLOUD_AGNSS_URL, request_body);

    static uint8_t full_buf[CONFIG_APP_LOCATION_CLOUD_BUFFER_SIZE];
    size_t total_len = 0;

    int err = http_fetch_chunked(CONFIG_APP_LOCATION_CLOUD_HOST, CONFIG_APP_LOCATION_CLOUD_PORT,
                                 CONFIG_APP_LOCATION_CLOUD_AGNSS_URL, CONFIG_APP_LOCATION_CLOUD_SEC_TAG, request_body,
                                 body_len, CHUNK_SIZE, full_buf, sizeof(full_buf), &total_len);
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
 * Cellular / Wi-Fi cloud location request
 * -------------------------------------------------------------------------*/

static int send_cellular_cloud_request(const struct location_cloud_request_data *cloud_req) {
    char request_body[2048];
    int body_len = snprintf(request_body, sizeof(request_body), "{");
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "\"current_cell\":{");
    body_len += snprintf(
        request_body + body_len, sizeof(request_body) - body_len,
        "\"id\":%u,\"mcc\":%d,\"mnc\":%d,\"tac\":%u,"
        "\"timing_advance\":%u,\"earfcn\":%u,"
        "\"rsrp\":%d,\"rsrq\":%d",
        (unsigned int)cloud_req->current_cell.id, cloud_req->current_cell.mcc, cloud_req->current_cell.mnc,
        (unsigned int)cloud_req->current_cell.tac, (unsigned int)cloud_req->current_cell.timing_advance,
        (unsigned int)cloud_req->current_cell.earfcn, cloud_req->current_cell.rsrp, cloud_req->current_cell.rsrq);
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "},");
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "\"neighbor_cells\":[");
    for (uint8_t i = 0; i < cloud_req->ncells_count; i++) {
        if (i > 0) {
            body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, ",");
        }
        body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len,
                             "{\"earfcn\":%u,\"phys_cell_id\":%u,"
                             "\"rsrp\":%d,\"rsrq\":%d}",
                             (unsigned int)cloud_req->neighbor_cells[i].earfcn,
                             (unsigned int)cloud_req->neighbor_cells[i].phys_cell_id, cloud_req->neighbor_cells[i].rsrp,
                             cloud_req->neighbor_cells[i].rsrq);
    }
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "],");
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "\"gci_cells\":[");
    for (uint8_t i = 0; i < cloud_req->gci_cells_count; i++) {
        if (i > 0) {
            body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, ",");
        }
        body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len,
                             "{\"id\":%u,\"mcc\":%d,\"mnc\":%d,\"tac\":%u,"
                             "\"earfcn\":%u,\"rsrp\":%d}",
                             (unsigned int)cloud_req->gci_cells[i].id, cloud_req->gci_cells[i].mcc,
                             cloud_req->gci_cells[i].mnc, (unsigned int)cloud_req->gci_cells[i].tac,
                             (unsigned int)cloud_req->gci_cells[i].earfcn, cloud_req->gci_cells[i].rsrp);
    }
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "],");

#if defined(CONFIG_LOCATION_METHOD_WIFI)
    /* Wi-Fi access points. */
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "\"wifi_aps\":[");
    for (uint16_t i = 0; i < cloud_req->wifi_cnt; i++) {
        if (i > 0) {
            body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, ",");
        }
        body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len,
                             "{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                             "\"rssi\":%d}",
                             cloud_req->wifi_aps[i].mac[0], cloud_req->wifi_aps[i].mac[1],
                             cloud_req->wifi_aps[i].mac[2], cloud_req->wifi_aps[i].mac[3],
                             cloud_req->wifi_aps[i].mac[4], cloud_req->wifi_aps[i].mac[5], cloud_req->wifi_aps[i].rssi);
    }
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "]");
#else
    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "\"wifi_aps\":[]");
#endif /* CONFIG_LOCATION_METHOD_WIFI */

    body_len += snprintf(request_body + body_len, sizeof(request_body) - body_len, "}");

    if (body_len < 0 || body_len >= (int)sizeof(request_body)) {
        LOG_ERR("Failed to serialise cellular cloud request");
        return -ENOMEM;
    }

    LOG_INF("Sending to %s:%d%s: %s", CONFIG_APP_LOCATION_CLOUD_HOST, CONFIG_APP_LOCATION_CLOUD_PORT,
            CONFIG_APP_LOCATION_CLOUD_CELLULAR_URL, request_body);

    static uint8_t cell_buf[CONFIG_APP_LOCATION_CLOUD_BUFFER_SIZE];
    size_t total_len = 0;

    int err = http_fetch_chunked(CONFIG_APP_LOCATION_CLOUD_HOST, CONFIG_APP_LOCATION_CLOUD_PORT,
                                 CONFIG_APP_LOCATION_CLOUD_CELLULAR_URL, CONFIG_APP_LOCATION_CLOUD_SEC_TAG,
                                 request_body, body_len, CHUNK_SIZE, cell_buf, sizeof(cell_buf), &total_len);
    if (err) {
        LOG_ERR("Failed to send cellular cloud request: %d", err);
        return err;
    }

    LOG_INF("Cellular cloud location response: %zu bytes", total_len);

    /* Parse the cloud response and feed it back to the location library.
     * nRF Cloud REST API returns:
     *   {"lat": 45.524098, "lon": -122.688408, "uncertainty": 300}
     */
    struct location_data cloud_location = {0};
    bool parsed = false;

    if (total_len > 0) {
        /* Simple JSON parse — extract lat, lon, uncertainty. */
        char *lat_ptr = strstr((char *)cell_buf, "\"lat\"");
        char *lon_ptr = strstr((char *)cell_buf, "\"lon\"");
        char *unc_ptr = strstr((char *)cell_buf, "\"uncertainty\"");

        if (lat_ptr && lon_ptr) {
            lat_ptr = strchr(lat_ptr, ':');
            lon_ptr = strchr(lon_ptr, ':');
            if (lat_ptr && lon_ptr) {
                cloud_location.latitude = strtod(lat_ptr + 1, NULL);
                cloud_location.longitude = strtod(lon_ptr + 1, NULL);
                if (unc_ptr) {
                    char *val = strchr(unc_ptr, ':');
                    if (val) {
                        cloud_location.accuracy = (float)strtod(val + 1, NULL);
                    }
                }
                parsed = true;
            }
        }
    }

    if (parsed) {
        location_cloud_location_ext_result_set(LOCATION_EXT_RESULT_SUCCESS, &cloud_location);
        LOG_DBG("Cellular cloud location processed successfully "
                "(lat=%.6f, lon=%.6f, uncertainty=%.1f)",
                cloud_location.latitude, cloud_location.longitude, (double)cloud_location.accuracy);
    } else {
        LOG_WRN("Could not parse cloud location response, reporting unknown");
        location_cloud_location_ext_result_set(LOCATION_EXT_RESULT_UNKNOWN, NULL);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Message handling
 * -------------------------------------------------------------------------*/

static void handle_agnss_request(const struct nrf_modem_gnss_agnss_data_frame *agnss_req) {
    if (!mod.lte_connected) {
        LOG_DBG("LTE not connected, caching A-GNSS request for later");
        mod.cached_agnss_request = *agnss_req;
        mod.agnss_pending = true;
        request_lte_connect();
        return;
    }

    int err = fetch_agnss_data(agnss_req);

    if (err) {
        LOG_ERR("Failed to fetch A-GNSS data: %d", err);
        publish_result(LOCATION_CLOUD_AGNSS_FETCH_FAILED, err);
        return;
    }

    publish_result(LOCATION_CLOUD_AGNSS_FETCH_DONE, 200);
    mod.agnss_pending = false;
}

static void handle_cloud_location_request(const struct location_cloud_request_data *cloud_req) {
    if (!mod.lte_connected) {
        LOG_DBG("LTE not connected, requesting connect for cellular cloud request");
        request_lte_connect();
        return;
    }

    int err = send_cellular_cloud_request(cloud_req);

    if (err) {
        LOG_ERR("Failed to send cellular cloud request: %d", err);
        publish_result(LOCATION_CLOUD_CELLULAR_FAILED, err);
        return;
    }

    publish_result(LOCATION_CLOUD_CELLULAR_DONE, 200);
}

static void handle_network_event(const struct network_msg *msg) {
    switch (msg->type) {
        case NETWORK_CONNECTED:
            LOG_DBG("LTE connected");

            mod.lte_connected = true;
            mod.connect_requested = false;

            /* If we have a cached A-GNSS request, process it now. */
            if (mod.agnss_pending) {
                LOG_DBG("Processing cached A-GNSS request");
                int err = fetch_agnss_data(&mod.cached_agnss_request);

                if (err) {
                    LOG_ERR("Failed to fetch cached A-GNSS data: %d", err);
                    publish_result(LOCATION_CLOUD_AGNSS_FETCH_FAILED, err);
                } else {
                    publish_result(LOCATION_CLOUD_AGNSS_FETCH_DONE, 200);
                    mod.agnss_pending = false;
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

TASK_WDT_CALLBACK_DEFINE(location_cloud)

static void location_cloud_module_thread(void *arg1, void *arg2, void *arg3) {
    int err;
    const struct zbus_channel *chan;

    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    TASK_WDT_TIMEOUTS(APP_LOCATION_CLOUD);
    TASK_WDT_ZBUS_TIMEOUT;

    TASK_WDT_ADD(location_cloud, wdt_timeout_ms)

    mod.initialized = true;
    LOG_DBG("Location cloud module started");

    while (true) {
        TASK_WDT_FEED();

        uint8_t msg_buf[MAX(sizeof(struct location_msg), sizeof(struct network_msg))];

        err = zbus_sub_wait_msg(&location_cloud, &chan, msg_buf, zbus_wait_ms);
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
            } else if (location_msg->type == LOCATION_CLOUD_REQUEST) {
                LOG_DBG("Cloud location request received (cellular/Wi-Fi)");
                handle_cloud_location_request(&location_msg->cloud_request);
            }
        } else if (chan == &network_chan) {
            const struct network_msg *network_msg = (const struct network_msg *)msg_buf;

            handle_network_event(network_msg);
        }
    }
}

K_THREAD_DEFINE(location_cloud_module_thread_id, CONFIG_APP_LOCATION_CLOUD_THREAD_STACK_SIZE,
                location_cloud_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
