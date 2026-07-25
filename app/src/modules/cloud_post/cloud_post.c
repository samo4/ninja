/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include <hw_id.h>
#include <modem/modem_battery.h>
#include <modem/modem_info.h>

#include "app_common.h"
#include "cloud_post.h"
#include "network.h"
#include "utils.h"

LOG_MODULE_REGISTER(cloud_post, CONFIG_APP_CLOUD_POST_LOG_LEVEL);

ZBUS_CHAN_DEFINE(cloud_post_chan, struct cloud_post_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(cloud_post_data_chan, struct cloud_post_data, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_post);

ZBUS_CHAN_ADD_OBS(cloud_post_data_chan, cloud_post, 0);
ZBUS_CHAN_ADD_OBS(network_chan, cloud_post, 0);

static struct {
    bool connected;
    bool connect_requested;
    bool data_received;
    // actual data:
    double temperature;
    double location_latitude;
    double location_longitude;
    float location_accuracy;
    bool is_gnss_search;
} mod;

static char device_uid[HW_ID_LEN];

static void mod_request_connect(void) {
    if (mod.connect_requested) {
        return;
    }
    PUBLISH_NETWORK(NETWORK_CONNECT);
    mod.connect_requested = true;
}

static void cloud_post_send(void) {
    char csv_body[256];
    static uint8_t resp_buf[2048];
    size_t total_len = 0;
    int voltage_mv = 0;
    int err = modem_battery_voltage_get(&voltage_mv);
    if (err) {
        LOG_WRN("Failed to get modem battery voltage: %d", err);
        voltage_mv = -err;
    }
    snprintf(csv_body, sizeof(csv_body), "%s,%d,%.2f,%.6f,%.6f,%.1f,%d", device_uid, voltage_mv, mod.temperature,
             mod.location_latitude, mod.location_longitude, (double)mod.location_accuracy, mod.is_gnss_search);
    int status = http_fetch_chunked(CONFIG_APP_CLOUD_POST_URL, "text/csv", csv_body, strlen(csv_body), 1400, resp_buf,
                                    sizeof(resp_buf), &total_len);
    if (status == 0) {
        LOG_INF("Cloud POST response: HTTP 200, body: %zu bytes", total_len);
        struct cloud_post_msg done_msg = {.type = CLOUD_POST_SEND_DONE, .http_status = 200};
        zbus_chan_pub(&cloud_post_chan, &done_msg, PUB_TIMEOUT);
    } else {
        LOG_ERR("Cloud POST returned HTTP %d", status);
        struct cloud_post_msg fail_msg = {.type = CLOUD_POST_SEND_FAILED, .http_status = status};
        zbus_chan_pub(&cloud_post_chan, &fail_msg, PUB_TIMEOUT);
        if (total_len > 0) {
            rtt_dump_text((const char *)resp_buf, total_len);
        }
    }
}

TASK_WDT_CALLBACK_DEFINE(cloud_post)

static void cloud_post_module_thread(void *arg1, void *arg2, void *arg3) {
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    TASK_WDT_ADD(cloud_post, REST_TIMEOUT_MS + 60000)

    int err;
    const struct zbus_channel *chan;
    uint8_t msg_buf[MAX(sizeof(struct cloud_post_data), sizeof(struct network_msg))];

    int ret = hw_id_get(device_uid, sizeof(device_uid));
    if (ret == 0) {
        LOG_INF("Device ID: %s", device_uid);
    } else {
        LOG_WRN("Failed to get device ID: %d", ret);
        strncpy(device_uid, "error", sizeof(device_uid) - 1);
    }

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
        } else if (chan == &cloud_post_data_chan) {
            const struct cloud_post_data *data = (const struct cloud_post_data *)msg_buf;
            LOG_INF("personality data received (lat=%.6f, lon=%.6f, acc=%.1f, gnss=%d)", data->latitude,
                    data->longitude, (double)data->accuracy, data->is_gnss_search);
            mod.location_latitude = data->latitude;
            mod.location_longitude = data->longitude;
            mod.location_accuracy = data->accuracy;
            mod.is_gnss_search = data->is_gnss_search;
            mod.data_received = true;
        }
        if (mod.data_received) {
            if (mod.connected) {
                cloud_post_send();
                mod.data_received = false;
            } else {
                mod_request_connect();
            }
        }
    }
}

K_THREAD_DEFINE(cloud_post_thread_id, CONFIG_APP_CLOUD_POST_THREAD_STACK_SIZE, cloud_post_module_thread, NULL, NULL,
                NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
