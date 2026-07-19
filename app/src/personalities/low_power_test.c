/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Low Power Test personality:
 *   Measure sensors → post to cloud → turn off modem → sleep → repeat.
 *   Half as many measurements as reporting, but allows measuring
 *   board power consumption with everything off during sleep.
 *
 *   State machine:
 *     SAMPLING      — fire sensor-sample request + LED, arm cloud-post timer
 *     WAITING       — cloud_post module picks up sample, connects LTE, posts,
 *                     then personality disconnects modem
 *     DISCONNECTING — waiting for NETWORK_DISCONNECTED
 *     SLEEPING      — modem-off power-measurement window
 *     → back to SAMPLING
 */

#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/sys/reboot.h>

#include "app_common.h"
#include "cloud_post.h"
#include "environmental.h"
#include "low_power_test.h"
#include "network.h"

LOG_MODULE_REGISTER(low_power_test, CONFIG_APP_LOG_LEVEL);

/* Fallback timeout if cloud_post never publishes SEND_DONE/SEND_FAILED.
 * takes up to 5min to get LTE connection
 */
#define CLOUD_POST_FALLBACK_TIMEOUT_SECONDS 600

/* Define the timer channel */
ZBUS_CHAN_DEFINE(timer_chan, struct low_power_timer_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* ── Timer work ─────────────────────────────────────────────────── */

static void timer_expired_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(sample_timer_work, timer_expired_fn);

static void timer_expired_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct low_power_timer_msg msg = {.type = LOW_POWER_TIMER_EXPIRED};
    int err = zbus_chan_pub(&timer_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish timer expired, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

static void timer_arm(uint32_t delay_sec) {
    int err = k_work_reschedule(&sample_timer_work, K_SECONDS(delay_sec));
    if (err < 0) {
        LOG_ERR("k_work_reschedule sample_timer_work, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

/* ── RTT Heartbeat ──────────────────────────────────────────────── */

#define HEARTBEAT_INTERVAL_SEC 60

static void heartbeat_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_fn);

static void heartbeat_fn(struct k_work *work) {
    ARG_UNUSED(work);
    LOG_INF("♥");
    k_work_reschedule(&heartbeat_work, K_SECONDS(HEARTBEAT_INTERVAL_SEC));
}

static void heartbeat_start(void) { k_work_reschedule(&heartbeat_work, K_SECONDS(HEARTBEAT_INTERVAL_SEC)); }

static void heartbeat_stop(void) { k_work_cancel_delayable(&heartbeat_work); }

/* ── Helpers ────────────────────────────────────────────────────── */

static void fire_sample(struct low_power_state_object *state) {
    struct environmental_msg req = {
        .type = ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST,
    };

    int err = zbus_chan_pub(&environmental_chan, &req, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish environmental request, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    /* Arm a fallback timer — cloud_post should publish SEND_DONE/SEND_FAILED
     * on its own channel.  If something goes wrong this catches it.
     */
    timer_arm(CLOUD_POST_FALLBACK_TIMEOUT_SECONDS);
}

static void request_disconnect(void) {
    const struct network_msg msg = {.type = NETWORK_DISCONNECT};

    int err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish NETWORK_DISCONNECT, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

/* ── SMF states ─────────────────────────────────────────────────── */

static void sampling_entry(void *o);
static enum smf_state_result waiting_run(void *o);
static void disconnecting_entry(void *o);
static enum smf_state_result disconnecting_run(void *o);
static void sleeping_entry(void *o);
static enum smf_state_result sleeping_run(void *o);
static void rebooting_entry(void *o);

static const struct smf_state states[] = {
    [LOW_POWER_STATE_SAMPLING] = SMF_CREATE_STATE(sampling_entry, NULL, NULL, NULL, NULL),
    [LOW_POWER_STATE_WAITING] = SMF_CREATE_STATE(NULL, waiting_run, NULL, NULL, NULL),
    [LOW_POWER_STATE_DISCONNECTING] = SMF_CREATE_STATE(disconnecting_entry, disconnecting_run, NULL, NULL, NULL),
    [LOW_POWER_STATE_SLEEPING] = SMF_CREATE_STATE(sleeping_entry, sleeping_run, NULL, NULL, NULL),
    [LOW_POWER_STATE_REBOOTING] = SMF_CREATE_STATE(rebooting_entry, NULL, NULL, NULL, NULL),
};

static void sampling_entry(void *o) {
    struct low_power_state_object *state = (struct low_power_state_object *)o;
    LOG_INF("LP: sampling (cycle every %us)", state->sample_interval_sec);
    fire_sample(state);
    smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_WAITING]);
}

static enum smf_state_result waiting_run(void *o) {
    struct low_power_state_object *state = (struct low_power_state_object *)o;

    if (state->chan == &cloud_post_chan) {
        const struct cloud_post_msg *msg = (const struct cloud_post_msg *)state->msg_buf;

        if (msg->type == CLOUD_POST_SEND_DONE) {
            LOG_INF("LP: cloud POST done (HTTP %d), disconnecting modem now", msg->http_status);
            request_disconnect();
            smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_DISCONNECTING]);
            return SMF_EVENT_HANDLED;
        }

        if (msg->type == CLOUD_POST_SEND_FAILED) {
            LOG_WRN("LP: cloud POST failed (%d), disconnecting anyway", msg->http_status);
            request_disconnect();
            smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_DISCONNECTING]);
            return SMF_EVENT_HANDLED;
        }
    }

    if (state->chan == &timer_chan) {
        LOG_WRN("LP: fallback timeout expired, disconnecting modem");
        request_disconnect();
        smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_DISCONNECTING]);
        return SMF_EVENT_HANDLED;
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
            LOG_INF("LP: modem off, entering sleep");
            smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_SLEEPING]);
            return SMF_EVENT_HANDLED;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

static void sleeping_entry(void *o) {
    struct low_power_state_object *state = (struct low_power_state_object *)o;
    LOG_DBG("%s", __func__);
    LOG_INF("LP: sleeping with modem off for %us — measure power now", state->sample_interval_sec);
    heartbeat_start();
    timer_arm(state->sample_interval_sec);
}

static enum smf_state_result sleeping_run(void *o) {
    struct low_power_state_object *state = (struct low_power_state_object *)o;
    if (state->chan == &timer_chan) {
        heartbeat_stop();
        LOG_INF("LP: sleep expired, starting new cycle");
        smf_set_state(SMF_CTX(state), &states[LOW_POWER_STATE_SAMPLING]);
        return SMF_EVENT_HANDLED;
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
    smf_set_initial(SMF_CTX(state), &states[LOW_POWER_STATE_SAMPLING]);
}

void low_power_process(struct low_power_state_object *state) {
    int err = smf_run_state(SMF_CTX(state));
    if (err) {
        LOG_ERR("LP: smf_run_state(), error: %d", err);
        SEND_FATAL_ERROR();
    }
}
