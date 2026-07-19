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
#include "network.h"
#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif

LOG_MODULE_REGISTER(cloud_post, CONFIG_APP_CLOUD_POST_LOG_LEVEL);

ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_post);

#if defined(CONFIG_APP_POWER)
ZBUS_CHAN_ADD_OBS(power_chan, cloud_post, 0);
#endif

#if defined(CONFIG_APP_ENVIRONMENTAL)
ZBUS_CHAN_ADD_OBS(environmental_chan, cloud_post, 0);
#endif

ZBUS_CHAN_ADD_OBS(network_chan, cloud_post, 0);

static const int REST_TIMEOUT_MS = 30000;

static struct {
    bool connected;
    bool connect_requested;
#if defined(CONFIG_APP_POWER)
    bool battery_received;
    struct power_msg battery_data;
#endif
#if defined(CONFIG_APP_ENVIRONMENTAL)
    bool env_received;
    struct environmental_msg env_data;
#endif
} mod;

static void mod_reset_samples(void) {
#if defined(CONFIG_APP_POWER)
    mod.battery_received = false;
#endif
#if defined(CONFIG_APP_ENVIRONMENTAL)
    mod.env_received = false;
#endif
}

static void mod_request_connect(void) {
    const struct network_msg msg = {.type = NETWORK_CONNECT};
    int err;

    if (mod.connect_requested) {
        return;
    }

    LOG_INF("Requesting LTE connection");
    err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("zbus_chan_pub NETWORK_CONNECT failed: %d", err);
        return;
    }
    mod.connect_requested = true;
}

static void cloud_post_send(void) {
    int err;
    char resp_buf[1024];
    char csv_body[256];
    const char *header_fields[] = {"Content-Type: text/csv\r\n", NULL};

#if defined(CONFIG_APP_POWER) && defined(CONFIG_APP_ENVIRONMENTAL)
    snprintf(csv_body, sizeof(csv_body), "%.0f,%.0f,%.3f,%d,%.2f", (double)mod.battery_data.timestamp,
             mod.battery_data.percentage, mod.battery_data.voltage, mod.battery_data.charging ? 1 : 0,
             mod.env_data.temperature);
#else
    snprintf(csv_body, sizeof(csv_body), "0,0,0,0,0");
#endif

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
        if (resp.response_len > 0) {
            /* Split into chunks to avoid Segger RTT message drops */
            int offset = 0;
            const int chunk_size = 64;
            while (offset < resp.response_len) {
                int len = resp.response_len - offset;
                if (len > chunk_size) {
                    len = chunk_size;
                }
                LOG_DBG("Response body [%d-%d]: %.*s", offset, offset + len - 1, len, resp.response + offset);
                offset += len;
            }
        }
        return;
    }

    LOG_ERR("REST request failed: %d", err);
}

static void cloud_post_wdt_callback(int channel_id, void *user_data) {
    LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id, k_thread_name_get((k_tid_t)user_data));
    SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void cloud_post_module_thread(void *arg1, void *arg2, void *arg3) {
    int err;
    int task_wdt_id;
    const struct zbus_channel *chan;
#if defined(CONFIG_APP_POWER) && defined(CONFIG_APP_ENVIRONMENTAL)
    uint8_t msg_buf[MAX(MAX(sizeof(struct power_msg), sizeof(struct environmental_msg)), sizeof(struct network_msg))];
#elif defined(CONFIG_APP_POWER)
    uint8_t msg_buf[MAX(sizeof(struct power_msg), sizeof(struct network_msg))];
#elif defined(CONFIG_APP_ENVIRONMENTAL)
    uint8_t msg_buf[MAX(sizeof(struct environmental_msg), sizeof(struct network_msg))];
#else
    uint8_t msg_buf[sizeof(struct network_msg)];
#endif

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

                if (0
#if defined(CONFIG_APP_POWER)
                    && mod.battery_received
#endif
#if defined(CONFIG_APP_ENVIRONMENTAL)
                    && mod.env_received
#endif
                ) {
                    cloud_post_send();
                    mod_reset_samples();
                }
            } else if (msg->type == NETWORK_DISCONNECTED) {
                LOG_DBG("LTE disconnected");
                mod.connected = false;
            }
        }
#if defined(CONFIG_APP_POWER)
        else if (chan == &power_chan) {
            const struct power_msg *msg = (const struct power_msg *)msg_buf;

            if (msg->type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
                mod.battery_received = true;
                mod.battery_data = *msg;
                LOG_DBG("Battery: %.0f%%, %.3fV, charging=%d", msg->percentage, msg->voltage, msg->charging);
            }
        }
#endif
#if defined(CONFIG_APP_ENVIRONMENTAL)
        else if (chan == &environmental_chan) {
            const struct environmental_msg *msg = (const struct environmental_msg *)msg_buf;

            if (msg->type == ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE) {
                mod.env_received = true;
                mod.env_data = *msg;
                LOG_DBG("Env: %.2f C", msg->temperature);
            }
        }
#endif

        /* Samples ready — send if connected, otherwise request connection */
        if ((!IS_ENABLED(CONFIG_APP_POWER)
#if defined(CONFIG_APP_POWER)
             || mod.battery_received
#endif
             ) &&
            (!IS_ENABLED(CONFIG_APP_ENVIRONMENTAL)
#if defined(CONFIG_APP_ENVIRONMENTAL)
             || mod.env_received
#endif
             )) {
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
