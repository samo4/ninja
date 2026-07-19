/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <net/rest_client.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include <modem/modem_battery.h>
#include <modem/modem_info.h>

#include "app_common.h"
#include "cloud_post.h"
#include "environmental.h"
#include "network.h"

LOG_MODULE_REGISTER(cloud_post, CONFIG_APP_CLOUD_POST_LOG_LEVEL);

ZBUS_CHAN_DEFINE(cloud_post_chan, struct cloud_post_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_post);

ZBUS_CHAN_ADD_OBS(environmental_chan, cloud_post, 0);

ZBUS_CHAN_ADD_OBS(network_chan, cloud_post, 0);

static const int REST_TIMEOUT_MS = 30000;

static struct {
    bool connected;
    bool connect_requested;
    bool env_received;
    struct environmental_msg env_data;
} mod;

static void mod_reset_samples(void) { mod.env_received = false; }

static void mod_request_connect(void) {
    const struct network_msg msg = {.type = NETWORK_CONNECT};
    if (mod.connect_requested) {
        return;
    }

    LOG_INF("Requesting LTE connection");
    int err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("zbus_chan_pub NETWORK_CONNECT failed: %d", err);
        return;
    }
    mod.connect_requested = true;
}

static void cloud_post_send(void) {
    char resp_buf[1024];
    char csv_body[256];
    const char *header_fields[] = {"Content-Type: text/csv\r\n", NULL};
    char imei_buf[32] = {0};
    int voltage_mv = 0;

    // no imei :-(
    strncpy(imei_buf, "xxx", sizeof(imei_buf) - 1);

    int err = modem_battery_voltage_get(&voltage_mv);
    if (err) {
        LOG_WRN("Failed to get modem battery voltage: %d", err);
        voltage_mv = -err;
    }
    LOG_INF("Modem battery voltage: %d mV", voltage_mv);

    snprintf(csv_body, sizeof(csv_body), "%s,%d,%.2f", imei_buf, voltage_mv, mod.env_data.temperature);

    LOG_INF("Sending to %s:%d%s: %s", CONFIG_APP_CLOUD_POST_HOST, CONFIG_APP_CLOUD_POST_PORT, CONFIG_APP_CLOUD_POST_URL,
            csv_body);

    struct rest_client_req_context req = {0};
    struct rest_client_resp_context resp = {0};

    rest_client_request_defaults_set(&req);

    req.host = CONFIG_APP_CLOUD_POST_HOST;
    req.port = CONFIG_APP_CLOUD_POST_PORT;
    req.url = CONFIG_APP_CLOUD_POST_URL;
    req.sec_tag = CONFIG_APP_CLOUD_POST_SEC_TAG;
    req.tls_peer_verify = 0;
    req.http_method = HTTP_POST;
    req.header_fields = header_fields;
    req.body = csv_body;
    req.body_len = strlen(csv_body);
    req.resp_buff = resp_buf;
    req.resp_buff_len = sizeof(resp_buf);
    req.timeout_ms = REST_TIMEOUT_MS;

    err = rest_client_request(&req, &resp);
    if (err == 0) {
        LOG_INF("Cloud POST response: HTTP %d (%s), body: %d bytes", resp.http_status_code, resp.http_status_code_str,
                resp.response_len);
        LOG_INF("Response body: %.*s", resp.response_len > 64 ? 64 : resp.response_len, resp.response);
        struct cloud_post_msg done_msg = {.type = CLOUD_POST_SEND_DONE, .http_status = resp.http_status_code};
        zbus_chan_pub(&cloud_post_chan, &done_msg, PUB_TIMEOUT);
        return;
    }

    LOG_ERR("REST request failed: %d", err);

    /* Notify subscribers that POST failed */
    struct cloud_post_msg fail_msg = {.type = CLOUD_POST_SEND_FAILED, .http_status = err};
    zbus_chan_pub(&cloud_post_chan, &fail_msg, PUB_TIMEOUT);
}

static void cloud_post_wdt_callback(int channel_id, void *user_data) {
    LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id, k_thread_name_get((k_tid_t)user_data));
    SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void cloud_post_module_thread(void *arg1, void *arg2, void *arg3) {
    int err;
    int task_wdt_id;
    const struct zbus_channel *chan;
    uint8_t msg_buf[MAX(sizeof(struct environmental_msg), sizeof(struct network_msg))];

    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    task_wdt_id = task_wdt_add(REST_TIMEOUT_MS + 60000, cloud_post_wdt_callback, (void *)k_current_get());
    if (task_wdt_id < 0) {
        LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
        SEND_FATAL_ERROR();
        return;
    }

    LOG_DBG("Cloud POST module task started");

    mod_reset_samples();

    while (true) {
        err = task_wdt_feed(task_wdt_id);
        if (err) {
            LOG_ERR("task_wdt_feed, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }

        err = zbus_sub_wait_msg(&cloud_post, &chan, msg_buf, K_FOREVER);
        if (err) {
            LOG_ERR("zbus_sub_wait_msg, error: %d", err);
            continue;
        }

        if (chan == &network_chan) {
            const struct network_msg *msg = (const struct network_msg *)msg_buf;

            if (msg->type == NETWORK_CONNECTED) {
                LOG_INF("LTE connected");
                mod.connected = true;
                mod.connect_requested = false;

                /* Samples will be sent from the logic below when all data is ready */
            } else if (msg->type == NETWORK_DISCONNECTED) {
                LOG_DBG("LTE disconnected");
                mod.connected = false;
            }
        } else if (chan == &environmental_chan) {
            const struct environmental_msg *msg = (const struct environmental_msg *)msg_buf;

            if (msg->type == ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE) {
                mod.env_received = true;
                mod.env_data = *msg;
                LOG_DBG("Env: %.2f C", msg->temperature);
            }
        }

        /* Samples ready — send if connected, otherwise request connection */
        if ((!IS_ENABLED(CONFIG_APP_ENVIRONMENTAL) || mod.env_received)) {
            if (mod.connected) {
                cloud_post_send();
                mod_reset_samples();
            } else {
                mod_request_connect();
            }
        }
    }
}

K_THREAD_DEFINE(cloud_post_thread_id, CONFIG_APP_CLOUD_POST_THREAD_STACK_SIZE, cloud_post_module_thread, NULL, NULL,
                NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
