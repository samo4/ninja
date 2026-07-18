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

#include "app_common.h"
#include "cloud_post.h"
#include "environmental.h"
#include "power.h"

/* Register log module */
LOG_MODULE_REGISTER(cloud_post, CONFIG_APP_CLOUD_POST_LOG_LEVEL);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_post);

/* Observe channels for battery and environmental sensor data */
ZBUS_CHAN_ADD_OBS(power_chan, cloud_post, 0);
ZBUS_CHAN_ADD_OBS(environmental_chan, cloud_post, 0);

/* State tracking for pending sample data */
static struct {
    bool battery_received;
    struct power_msg battery_data;
    bool env_received;
    struct environmental_msg env_data;
} sample_state;

static void sample_state_reset(void) {
    sample_state.battery_received = false;
    sample_state.env_received = false;
}

static void cloud_post_send(void) {
    int err;
    char resp_buf[256];
    char csv_body[256];
    const char *header_fields[] = {"Content-Type: text/csv\r\n", NULL};

    /* Format CSV:
     * timestamp,battery_pct,battery_voltage,charging,temperature,pressure,humidity
     */
    snprintf(csv_body, sizeof(csv_body), "%.0f,%.0f,%.3f,%d,%.2f,%.2f,%.2f",
             (double)sample_state.battery_data.timestamp, sample_state.battery_data.percentage,
             sample_state.battery_data.voltage, sample_state.battery_data.charging ? 1 : 0,
             sample_state.env_data.temperature, sample_state.env_data.pressure, sample_state.env_data.humidity);

    LOG_INF("Sending to %s:%d%s: %s", CONFIG_APP_CLOUD_POST_HOST, CONFIG_APP_CLOUD_POST_PORT, CONFIG_APP_CLOUD_POST_URL,
            csv_body);

    struct rest_client_req_context req = {0};
    struct rest_client_resp_context resp = {0};

    rest_client_request_defaults_set(&req);

    req.host = CONFIG_APP_CLOUD_POST_HOST;
    req.port = CONFIG_APP_CLOUD_POST_PORT;
    req.url = "/api/Ninja";
    req.sec_tag = CONFIG_APP_CLOUD_POST_SEC_TAG;
    /* Public endpoint — skip TLS certificate verification */
    req.tls_peer_verify = 0;
    req.http_method = HTTP_POST;
    req.header_fields = header_fields;
    req.body = csv_body;
    req.body_len = strlen(csv_body);
    req.resp_buff = resp_buf;
    req.resp_buff_len = sizeof(resp_buf);
    req.timeout_ms = 10000;

    err = rest_client_request(&req, &resp);
    if (err) {
        LOG_ERR("REST request failed: %d", err);
    } else {
        LOG_INF("Cloud POST response: HTTP %d (%s), body: %d bytes", resp.http_status_code, resp.http_status_code_str,
                resp.response_len);
        if (resp.response_len > 0) {
            LOG_DBG("Response body: %.*s", resp.response_len, resp.response);
        }
    }
}

static void cloud_post_wdt_callback(int channel_id, void *user_data) {
    LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id, k_thread_name_get((k_tid_t)user_data));
    SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void cloud_post_module_thread(void) {
    int err;
    int task_wdt_id;
    const struct zbus_channel *chan;
    uint8_t msg_buf[MAX(sizeof(struct power_msg), sizeof(struct environmental_msg))];

    task_wdt_id =
        task_wdt_add(CONFIG_APP_CLOUD_POST_TIMEOUT_MS + 10000, cloud_post_wdt_callback, (void *)k_current_get());
    if (task_wdt_id < 0) {
        LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
        SEND_FATAL_ERROR();
        return;
    }

    LOG_DBG("Cloud POST module task started");

    sample_state_reset();

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

        if (chan == &power_chan) {
            const struct power_msg *msg = (const struct power_msg *)msg_buf;

            if (msg->type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
                sample_state.battery_received = true;
                sample_state.battery_data = *msg;
                LOG_DBG("Battery sample: %.0f%%, %.3fV, charging=%d", msg->percentage, msg->voltage, msg->charging);
            }
        } else if (chan == &environmental_chan) {
            const struct environmental_msg *msg = (const struct environmental_msg *)msg_buf;

            if (msg->type == ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE) {
                sample_state.env_received = true;
                sample_state.env_data = *msg;
                LOG_DBG("Environmental sample: %.2f C, %.2f Pa, %.2f %%", msg->temperature, msg->pressure,
                        msg->humidity);
            }
        }

        /* When both battery and environmental data are available, send to cloud */
        if (sample_state.battery_received && sample_state.env_received) {
            cloud_post_send();
            sample_state_reset();
        }
    }
}

K_THREAD_DEFINE(cloud_post_thread_id, CONFIG_APP_CLOUD_POST_THREAD_STACK_SIZE, cloud_post_module_thread, NULL, NULL,
                NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
