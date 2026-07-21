/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "environmental.h"
#include "module_state.h"

/* Register log module */
LOG_MODULE_REGISTER(environmental, CONFIG_APP_ENVIRONMENTAL_LOG_LEVEL);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(environmental_chan, struct environmental_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(environmental);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(environmental_chan, environmental, 0);

#define MAX_MSG_SIZE sizeof(struct environmental_msg)

BUILD_ASSERT(CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS >
                 CONFIG_APP_ENVIRONMENTAL_MSG_PROCESSING_TIMEOUT_SECONDS,
             "Watchdog timeout must be greater than maximum message processing time");

// Current FSM state name (for heartbeat reporting).
static const char *env_state_str;

// State machine

// Environmental module states
enum environmental_module_state {
    STATE_RUNNING,
};

//  Used to transfer context data between state changes.
struct environmental_state_object {
    struct smf_ctx ctx;              // must be first
    const struct zbus_channel *chan; // channel type that a message was received on
    uint8_t msg_buf[MAX_MSG_SIZE];
    const struct device *const bme680;
};

// Forward declarations of state handlers
static enum smf_state_result state_running_run(void *obj);

// State machine definition
static const struct smf_state states[] = {
    [STATE_RUNNING] = SMF_CREATE_STATE(NULL, state_running_run, NULL, NULL, NULL),
};

static void sample_sensors(struct environmental_state_object *state) {
    int err;
    double temperature;

    struct sensor_value temp = {0};
    err = sensor_sample_fetch(state->bme680);
    if (err) {
        LOG_ERR("sensor_sample_fetch, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    err = sensor_channel_get(state->bme680, SENSOR_CHAN_AMBIENT_TEMP, &temp);
    if (err) {
        LOG_ERR("sensor_channel_get, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }
    temperature = sensor_value_to_double(&temp);

    struct environmental_msg msg = {
        .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE,
        .temperature = temperature,
        .timestamp = k_uptime_get(),
    };

    err = zbus_chan_pub(&environmental_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("zbus_chan_pub, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }
}

TASK_WDT_CALLBACK_DEFINE(env)

// State handlers

static enum smf_state_result state_running_run(void *obj) {
    env_state_str = "run";
    struct environmental_state_object *state_object = obj;
    if (&environmental_chan == state_object->chan) {
        const struct environmental_msg *msg = (const struct environmental_msg *)state_object->msg_buf;
        if (msg->type == ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST) {
            sample_sensors(state_object);
            return SMF_EVENT_HANDLED;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

static void env_module_thread(void) {
    int err;
    TASK_WDT_TIMEOUTS(APP_ENVIRONMENTAL);
    TASK_WDT_ZBUS_TIMEOUT;
    static struct environmental_state_object environmental_state = {
        .bme680 = DEVICE_DT_GET(DT_NODELABEL(bme680)),
    };

    LOG_DBG("Environmental module task started");

    TASK_WDT_ADD(env, wdt_timeout_ms)

    smf_set_initial(SMF_CTX(&environmental_state), &states[STATE_RUNNING]);

    while (true) {
        TASK_WDT_FEED();

        err = zbus_sub_wait_msg(&environmental, &environmental_state.chan, environmental_state.msg_buf, zbus_wait_ms);
        if (err == -ENOMSG) {
            continue;
        } else if (err) {
            LOG_ERR("zbus_sub_wait_msg, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
        err = smf_run_state(SMF_CTX(&environmental_state));
        if (err) {
            LOG_ERR("smf_run_state(), error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
    }
}

K_THREAD_DEFINE(environmental_module_thread_id, CONFIG_APP_ENVIRONMENTAL_THREAD_STACK_SIZE, env_module_thread, NULL,
                NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

const char *environmental_state_str(void) { return env_state_str ? env_state_str : "?"; }
