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

#include <hw_id.h>
#include <modem/modem_battery.h>
#include <modem/modem_info.h>

#include "app_common.h"
#include "cloud_post.h"
#if defined(CONFIG_APP_MOTION)
#include "motion.h"
#elif defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif
#if defined(CONFIG_LOCATION)
#include "location.h"
#endif
#include "network.h"

#include "utils.h"

LOG_MODULE_REGISTER(cloud_post, CONFIG_APP_CLOUD_POST_LOG_LEVEL);

ZBUS_CHAN_DEFINE(cloud_post_chan, struct cloud_post_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_post);

#if defined(CONFIG_APP_MOTION)
ZBUS_CHAN_ADD_OBS(motion_chan, cloud_post, 0);
#elif defined(CONFIG_APP_ENVIRONMENTAL)
ZBUS_CHAN_ADD_OBS(environmental_chan, cloud_post, 0);
#endif
#if defined(CONFIG_LOCATION)
ZBUS_CHAN_ADD_OBS(location_chan, cloud_post, 0);
#endif
ZBUS_CHAN_ADD_OBS(network_chan, cloud_post, 0);

static const int REST_TIMEOUT_MS = 30000;

static struct {
    bool connected;
    bool connect_requested;
    bool env_received;
#if defined(CONFIG_APP_MOTION)
    struct motion_msg env_data;
#elif defined(CONFIG_APP_ENVIRONMENTAL)
    struct environmental_msg env_data;
#endif
    bool location_received;
    double location_latitude;
    double location_longitude;
    float location_accuracy;
} mod;

static char device_uid[HW_ID_LEN];

static void mod_reset_samples(void) {
    mod.env_received = false;
    mod.location_received = false;
}

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
    int voltage_mv = 0;

    int err = modem_battery_voltage_get(&voltage_mv);
    if (err) {
        LOG_WRN("Failed to get modem battery voltage: %d", err);
        voltage_mv = -err;
    }
    snprintf(csv_body, sizeof(csv_body), "%s,%d,%.2f,%.6f,%.6f,%.1f", device_uid, voltage_mv, mod.env_data.temperature,
             mod.location_latitude, mod.location_longitude, (double)mod.location_accuracy);
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
        LOG_INF("Cloud POST response: \x1b[32mHTTP %d (%s)\x1b[0m, body: %d bytes", resp.http_status_code,
                resp.http_status_code_str, resp.response_len);
        struct cloud_post_msg done_msg = {.type = CLOUD_POST_SEND_DONE, .http_status = resp.http_status_code};
        zbus_chan_pub(&cloud_post_chan, &done_msg, PUB_TIMEOUT);
    } else {
        LOG_ERR("REST request failed: %d", err);
        /* Notify subscribers that POST failed */
        struct cloud_post_msg fail_msg = {.type = CLOUD_POST_SEND_FAILED, .http_status = err};
        zbus_chan_pub(&cloud_post_chan, &fail_msg, PUB_TIMEOUT);
    }
    // LOG_DBG("Response body: %.*s", resp.response_len > 64 ? 64 : resp.response_len, resp.response);
    rtt_dump_text(resp.response, resp.response_len);
}

TASK_WDT_CALLBACK_DEFINE(cloud_post)

static void cloud_post_module_thread(void *arg1, void *arg2, void *arg3) {
    int err;
    const struct zbus_channel *chan;
#if defined(CONFIG_APP_MOTION)
#define ENV_MSG_TYPE struct motion_msg
#elif defined(CONFIG_APP_ENVIRONMENTAL)
#define ENV_MSG_TYPE struct environmental_msg
#endif
#if defined(CONFIG_LOCATION)
    uint8_t msg_buf[MAX(sizeof(ENV_MSG_TYPE), MAX(sizeof(struct network_msg), sizeof(struct location_msg)))];
#else
    uint8_t msg_buf[MAX(sizeof(ENV_MSG_TYPE), sizeof(struct network_msg))];
#endif

    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    TASK_WDT_ADD(cloud_post, REST_TIMEOUT_MS + 60000)

    {
        int ret = hw_id_get(device_uid, sizeof(device_uid));
        if (ret == 0) {
            LOG_INF("Device ID: %s", device_uid);
        } else {
            LOG_WRN("Failed to get device ID: %d", ret);
            strncpy(device_uid, "error", sizeof(device_uid) - 1);
        }
    }

    mod_reset_samples();

    while (true) {
        TASK_WDT_FEED();

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
            } else if (msg->type == NETWORK_DISCONNECTED) {
                LOG_DBG("LTE disconnected");
                mod.connected = false;
            }
        }
#if defined(CONFIG_LOCATION)
        else if (chan == &location_chan) {
            const struct location_msg *msg = (const struct location_msg *)msg_buf;
            if (msg->type == LOCATION_DATA) {
                mod.location_received = true;
                mod.location_latitude = msg->gnss_data.latitude;
                mod.location_longitude = msg->gnss_data.longitude;
                mod.location_accuracy = msg->gnss_data.accuracy;
            }
        }
#endif
#if defined(CONFIG_APP_MOTION)
        else if (chan == &motion_chan) {
            const struct motion_msg *msg = (const struct motion_msg *)msg_buf;
            if (msg->type == MOTION_TEMPERATURE_DATA) {
                mod.env_received = true;
                mod.env_data = *msg;
                LOG_DBG("Motion temp: %.2f C", msg->temperature);
            }
        }
#elif defined(CONFIG_APP_ENVIRONMENTAL)
        else if (chan == &environmental_chan) {
            const struct environmental_msg *msg = (const struct environmental_msg *)msg_buf;
            if (msg->type == ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE) {
                mod.env_received = true;
                mod.env_data = *msg;
                LOG_DBG("Env: %.2f C", msg->temperature);
            }
        }
#endif

#if defined(CONFIG_LOCATION)
        if (mod.location_received) {
#else
        if (mod.env_received) {
#endif
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
