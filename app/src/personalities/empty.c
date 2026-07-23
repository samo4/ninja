/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Empty personality:
 *   Same flow as location_test, but instead of requesting location data
 *   it immediately publishes an empty location package to cloud_post.
 *   Runs once: connect → send → disconnect → sleep forever.
 *
 *   State machine:
 *     WAITING_MODULE  — wait for LTE connection (modem init)
 *     SAMPLING        — publish empty cloud_post_data, arm fallback timer
 *     WAITING_CLOUD   — wait for SEND_DONE / SEND_FAILED
 *     DISCONNECTING   — wait for NETWORK_DISCONNECTED
 *     SLEEPING        — modem off, sleep forever (no loop)
 */

#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/sys/reboot.h>

#include "app_common.h"
#if defined(CONFIG_APP_MOTION)
#include "motion.h"
#endif
#include "cloud_post.h"
#if defined(CONFIG_APP_LED)
#include "led.h"
#endif
#include "empty.h"
#include "network.h"

LOG_MODULE_REGISTER(empty, CONFIG_APP_LOG_LEVEL);

/* Fallback timeout if cloud_post never publishes SEND_DONE/SEND_FAILED. */
#define CLOUD_POST_FALLBACK_TIMEOUT_SECONDS 600

/* Define the timer channel */
ZBUS_CHAN_DEFINE(timer_chan, struct empty_timer_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* ── Timer work ─────────────────────────────────────────────────── */

static void timer_expired_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(sample_timer_work, timer_expired_fn);

static void timer_expired_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct empty_timer_msg msg = {.type = EMPTY_TIMER_EXPIRED};
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

/* ── Personality state string (for main heartbeat) ─────────────── */

static const char *empty_state_name;

const char *personality_state_str(void) { return empty_state_name ? empty_state_name : "?"; }

/* ── Helpers ────────────────────────────────────────────────────── */

static void request_disconnect(void) { PUBLISH_NETWORK(NETWORK_DISCONNECT); }

/* ── SMF states ─────────────────────────────────────────────────── */

static enum smf_state_result waiting_module_run(void *o);
static void sampling_entry(void *o);
static enum smf_state_result waiting_cloud_run(void *o);
static void disconnecting_entry(void *o);
static enum smf_state_result disconnecting_run(void *o);
static void sleeping_entry(void *o);
static enum smf_state_result sleeping_run(void *o);
static const struct smf_state states[] = {
    [EMPTY_STATE_WAITING_MODULE] = SMF_CREATE_STATE(NULL, waiting_module_run, NULL, NULL, NULL),
    [EMPTY_STATE_SAMPLING] = SMF_CREATE_STATE(sampling_entry, NULL, NULL, NULL, NULL),
    [EMPTY_STATE_WAITING_CLOUD] = SMF_CREATE_STATE(NULL, waiting_cloud_run, NULL, NULL, NULL),
    [EMPTY_STATE_DISCONNECTING] = SMF_CREATE_STATE(disconnecting_entry, disconnecting_run, NULL, NULL, NULL),
    [EMPTY_STATE_SLEEPING] = SMF_CREATE_STATE(sleeping_entry, sleeping_run, NULL, NULL, NULL),
};

static enum smf_state_result waiting_module_run(void *o) {
    struct empty_state_object *state = (struct empty_state_object *)o;

    if (state->chan == &network_chan) {
        const struct network_msg *msg = (const struct network_msg *)state->msg_buf;
        if (msg->type == NETWORK_CONNECTED) {
            LOG_INF("LTE connected, starting first cycle");
            smf_set_state(SMF_CTX(state), &states[EMPTY_STATE_SAMPLING]);
            return SMF_EVENT_HANDLED;
        }
    }

    return SMF_EVENT_PROPAGATE;
}

static void sampling_entry(void *o) {
    struct empty_state_object *state = (struct empty_state_object *)o;
    empty_state_name = "sampling";
    LOG_INF("sampling (cycle every %us)", state->sample_interval_sec);

    /* Publish empty location data to cloud_post immediately — no actual
     * location request, just an empty payload to exercise the cloud path.
     */
    const struct cloud_post_data empty_data = {
        .latitude = 0.0,
        .longitude = 0.0,
        .accuracy = 0.0f,
        .is_gnss_search = false,
    };
    int err = zbus_chan_pub(&cloud_post_data_chan, &empty_data, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish cloud_post_data, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

#if defined(CONFIG_APP_LED)
    LED_BLINK_RED(10);
#endif /* CONFIG_APP_LED */
       // #if defined(CONFIG_APP_MOTION)
       //     PUBLISH_MOTION(MOTION_SAMPLE_REQUEST);
       // #endif /* CONFIG_APP_MOTION */

    /* Arm a fallback timer in case cloud_post never responds. */
    timer_arm(CLOUD_POST_FALLBACK_TIMEOUT_SECONDS);
    empty_state_name = "waiting_cloud";
    smf_set_state(SMF_CTX(state), &states[EMPTY_STATE_WAITING_CLOUD]);
}

static enum smf_state_result waiting_cloud_run(void *o) {
    struct empty_state_object *state = (struct empty_state_object *)o;

    if (state->chan == &cloud_post_chan) {
        const struct cloud_post_msg *msg = (const struct cloud_post_msg *)state->msg_buf;

        if (msg->type == CLOUD_POST_SEND_DONE) {
            LOG_INF("cloud POST done (HTTP %d), disconnecting modem now", msg->http_status);
            request_disconnect();
            smf_set_state(SMF_CTX(state), &states[EMPTY_STATE_DISCONNECTING]);
            return SMF_EVENT_HANDLED;
        }

        if (msg->type == CLOUD_POST_SEND_FAILED) {
            LOG_WRN("cloud POST failed (%d), disconnecting", msg->http_status);
            request_disconnect();
            smf_set_state(SMF_CTX(state), &states[EMPTY_STATE_DISCONNECTING]);
            return SMF_EVENT_HANDLED;
        }
    }

    if (state->chan == &timer_chan) {
        LOG_WRN("fallback timeout expired, disconnecting modem");
        request_disconnect();
        smf_set_state(SMF_CTX(state), &states[EMPTY_STATE_DISCONNECTING]);
        return SMF_EVENT_HANDLED;
    }

    return SMF_EVENT_PROPAGATE;
}

static void disconnecting_entry(void *o) {
    ARG_UNUSED(o);
    empty_state_name = "disconnecting";
    LOG_DBG("%s", __func__);
}

static enum smf_state_result disconnecting_run(void *o) {
    struct empty_state_object *state = (struct empty_state_object *)o;
    if (state->chan == &network_chan) {
        const struct network_msg *msg = (const struct network_msg *)state->msg_buf;
        if (msg->type == NETWORK_DISCONNECTED) {
            LOG_INF("modem off, entering sleep");
            smf_set_state(SMF_CTX(state), &states[EMPTY_STATE_SLEEPING]);
            return SMF_EVENT_HANDLED;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

static void sleeping_entry(void *o) {
    ARG_UNUSED(o);
    empty_state_name = "sleeping";
    LOG_INF("sleeping with modem off forever");
}

static enum smf_state_result sleeping_run(void *o) {
    ARG_UNUSED(o);
    /* Sleep forever — no loop back. */
    return SMF_EVENT_PROPAGATE;
}

/* ── Public API ─────────────────────────────────────────────────── */

void empty_init(struct empty_state_object *state) {
    state->sample_interval_sec = CONFIG_APP_SAMPLING_INTERVAL_SECONDS;
    empty_state_name = "waiting_module";
    smf_set_initial(SMF_CTX(state), &states[EMPTY_STATE_WAITING_MODULE]);
    PUBLISH_NETWORK(NETWORK_CONNECT);
}

void empty_process(struct empty_state_object *state) {
    int err = smf_run_state(SMF_CTX(state));
    if (err) {
        LOG_ERR("smf_run_state(), error: %d", err);
        SEND_FATAL_ERROR();
    }
}
