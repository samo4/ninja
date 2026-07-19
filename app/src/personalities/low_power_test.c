/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Low Power Test personality:
 *   Bare minimum LTE attach/detach cycle.
 *   Connect to LTE → disconnect → sleep → repeat.
 *   No sensor sampling, no data posting, no REST.
 *   Useful for measuring baseline power of the LTE cycle.
 */

#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "app_common.h"
#include "low_power_test.h"
#include "network.h"

LOG_MODULE_REGISTER(low_power_test, CONFIG_APP_LOG_LEVEL);

/* Define the timer channel */
ZBUS_CHAN_DEFINE(timer_chan, struct low_power_timer_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* ── Forward declarations ──────────────────────────────────────── */

static void timer_work_fn(struct k_work *work);
static void timer_start(uint32_t delay_sec);
static void timer_stop(void);

static K_WORK_DELAYABLE_DEFINE(lp_timer_work, timer_work_fn);

/* ── SMF state handlers ────────────────────────────────────────── */

static enum smf_state_result initial_sample_run(void *o);
static enum smf_state_result connecting_run(void *o);
static void disconnecting_entry(void *o);
static enum smf_state_result disconnecting_run(void *o);
static void sleeping_entry(void *o);
static enum smf_state_result sleeping_run(void *o);
static void rebooting_entry(void *o);

static const struct smf_state states[] = {
    [LOW_POWER_STATE_INITIAL_SAMPLE] = SMF_CREATE_STATE(NULL, initial_sample_run, NULL, NULL, NULL),
    [LOW_POWER_STATE_CONNECTING] = SMF_CREATE_STATE(NULL, connecting_run, NULL, NULL, NULL),
    [LOW_POWER_STATE_DISCONNECTING] = SMF_CREATE_STATE(disconnecting_entry, disconnecting_run, NULL, NULL, NULL),
    [LOW_POWER_STATE_SLEEPING] = SMF_CREATE_STATE(sleeping_entry, sleeping_run, NULL, NULL, NULL),
    [LOW_POWER_STATE_REBOOTING] = SMF_CREATE_STATE(rebooting_entry, NULL, NULL, NULL, NULL),
};

/* ── Timer work ─────────────────────────────────────────────────── */

static void timer_work_fn(struct k_work *work) {
    int err;
    const struct low_power_timer_msg msg = {
        .type = LOW_POWER_TIMER_EXPIRED_SAMPLE_DATA,
    };

    ARG_UNUSED(work);

    err = zbus_chan_pub(&timer_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish LP timer expired, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

static void timer_start(uint32_t delay_sec) {
    int err;

    err = k_work_reschedule(&lp_timer_work, K_SECONDS(delay_sec));
    if (err < 0) {
        LOG_ERR("k_work_reschedule lp_timer_work, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

static void timer_stop(void) {
    int err;

    err = k_work_cancel_delayable(&lp_timer_work);
    if (err < 0) {
        LOG_ERR("k_work_cancel_delayable lp_timer_work, error: %d", err);
    }
}

/* ── Helpers ────────────────────────────────────────────────────── */

static void request_connect(void) {
    int err;
    const struct network_msg msg = {.type = NETWORK_CONNECT};

    err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish NETWORK_CONNECT, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

static void request_disconnect(void) {
    int err;
    const struct network_msg msg = {.type = NETWORK_DISCONNECT};

    err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish NETWORK_DISCONNECT, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

/* ── SMF state handlers ─────────────────────────────────────────── */

static enum smf_state_result initial_sample_run(void *o) {
    struct low_power_state_object *state = (struct low_power_state_object *)o;

    LOG_INF("LP: requesting initial LTE connection");
    request_connect();

    smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_CONNECTING]);
    return SMF_EVENT_HANDLED;
}

static enum smf_state_result connecting_run(void *o) {
    struct low_power_state_object *state = (struct low_power_state_object *)o;

    if (state->chan == &network_chan) {
        const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

        if (msg->type == NETWORK_CONNECTED) {
            LOG_INF("LP: LTE connected, disconnecting immediately");
            request_disconnect();
            smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_DISCONNECTING]);
            return SMF_EVENT_HANDLED;
        }

        if (msg->type == NETWORK_DISCONNECTED) {
            LOG_WRN("LP: LTE disconnected while connecting, retrying");
            request_connect();
            return SMF_EVENT_HANDLED;
        }
    }

    return SMF_EVENT_PROPAGATE;
}

static void disconnecting_entry(void *o) {
    ARG_UNUSED(o);
    LOG_DBG("%s", __func__);
}

static enum smf_state_result disconnecting_run(void *o) {
    struct low_power_state_object *state = (struct low_power_state_object *)o;

    if (state->chan == &network_chan) {
        const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

        if (msg->type == NETWORK_DISCONNECTED) {
            LOG_INF("LP: disconnected, entering sleep");
            timer_stop();
            smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_SLEEPING]);
            return SMF_EVENT_HANDLED;
        }
    }

    return SMF_EVENT_PROPAGATE;
}

static void sleeping_entry(void *o) {
    struct low_power_state_object *state = (struct low_power_state_object *)o;

    LOG_DBG("%s", __func__);
    timer_start(state->sample_interval_sec);
}

static enum smf_state_result sleeping_run(void *o) {
    struct low_power_state_object *state = (struct low_power_state_object *)o;

    if (state->chan == &timer_chan) {
        const struct low_power_timer_msg *msg = (const struct low_power_timer_msg *)state->msg_buf;

        if (msg->type == LOW_POWER_TIMER_EXPIRED_SAMPLE_DATA) {
            LOG_INF("LP: timer expired, starting new cycle");
            request_connect();
            smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_CONNECTING]);
            return SMF_EVENT_HANDLED;
        }
    }

    return SMF_EVENT_PROPAGATE;
}

static void rebooting_entry(void *o) {
    ARG_UNUSED(o);
    LOG_DBG("%s", __func__);

    LOG_PANIC();
    k_sleep(K_SECONDS(10));
    sys_reboot(SYS_REBOOT_COLD);
}

/* ── Public API ─────────────────────────────────────────────────── */

void low_power_init(struct low_power_state_object *state) {
    state->sample_interval_sec = CONFIG_APP_SAMPLING_INTERVAL_SECONDS;

    smf_set_initial(SMF_CTX(state), &states[LOW_POWER_STATE_INITIAL_SAMPLE]);
}

void low_power_process(struct low_power_state_object *state) {
    int err;

    err = smf_run_state(SMF_CTX(state));
    if (err) {
        LOG_ERR("LP: smf_run_state(), error: %d", err);
        SEND_FATAL_ERROR();
    }
}
