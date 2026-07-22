/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Cloud Reporting personality:
 *   Owns the periodic sample-request timer.  On every tick it
 *   publishes an ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST — the
 *   environmental module samples once in response, and cloud_post
 *   independently picks up the result and sends it over LTE.
 *
 *   The self-propagating timer loop lives entirely here:
 *     SAMPLING (fire request + schedule timer) → WAITING (wait for timer)
 *     → SAMPLING → ...
 */

#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/sys/reboot.h>

#include "app_common.h"
#if defined(CONFIG_APP_MOTION)
#include "motion.h"
#elif defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif
#if defined(CONFIG_APP_LED)
#include "led.h"
#endif
#include "reporting.h"

LOG_MODULE_REGISTER(reporting, CONFIG_APP_LOG_LEVEL);

/* Personality state string (for main heartbeat). */
static const char *rpt_state_str;

const char *personality_state_str(void) { return rpt_state_str ? rpt_state_str : "?"; }

ZBUS_CHAN_DEFINE(timer_chan, struct timer_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* ── Timer work ─────────────────────────────────────────────────── */

static void timer_expired_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(sample_timer_work, timer_expired_fn);

static void timer_expired_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct timer_msg msg = {.type = TIMER_EXPIRED_SAMPLE_DATA};
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

/* ── Helpers ────────────────────────────────────────────────────── */

static void fire_sample(struct reporting_state *state) {
#if defined(CONFIG_APP_LED)
    struct led_msg led = {
        .type = LED_RGB_SET,
        .red = 0,
        .green = 55,
        .duration_on_msec = 250,
        .duration_off_msec = 2000,
        .repetitions = 10,
    };

    int err = zbus_chan_pub(&led_chan, &led, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish LED pattern, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }
#endif /* CONFIG_APP_LED */

#if defined(CONFIG_APP_MOTION)
    struct motion_msg req = {
        .type = MOTION_SAMPLE_REQUEST,
    };

    err = zbus_chan_pub(&motion_chan, &req, PUB_TIMEOUT);
#elif defined(CONFIG_APP_ENVIRONMENTAL)
    struct environmental_msg req = {
        .type = ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST,
    };

    err = zbus_chan_pub(&environmental_chan, &req, PUB_TIMEOUT);
#endif
    if (err) {
        LOG_ERR("Failed to publish sensor request, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    // Arm the next sample
    timer_arm(state->sample_interval_sec);
}

/* ── SMF states ─────────────────────────────────────────────────── */

static void sampling_entry(void *o);
static enum smf_state_result waiting_run(void *o);
static void rebooting_entry(void *o);

static const struct smf_state states[] = {
    [STATE_SAMPLING] = SMF_CREATE_STATE(sampling_entry, NULL, NULL, NULL, NULL),
    [STATE_WAITING] = SMF_CREATE_STATE(NULL, waiting_run, NULL, NULL, NULL),
    [STATE_REBOOTING] = SMF_CREATE_STATE(rebooting_entry, NULL, NULL, NULL, NULL),
};

static void sampling_entry(void *o) {
    struct reporting_state *state = (struct reporting_state *)o;
    rpt_state_str = "sampling";
    LOG_INF("sampling (every %us)", state->sample_interval_sec);
    fire_sample(state);
    rpt_state_str = "waiting";
    smf_set_state(SMF_CTX(state), &states[STATE_WAITING]);
}

static enum smf_state_result waiting_run(void *o) {
    struct reporting_state *state = (struct reporting_state *)o;
    if (state->chan == &timer_chan) {
        const struct timer_msg *msg = (const struct timer_msg *)state->msg_buf;
        if (msg->type == TIMER_EXPIRED_SAMPLE_DATA) {
            smf_set_state(SMF_CTX(state), &states[STATE_SAMPLING]);
            return SMF_EVENT_HANDLED;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

static void rebooting_entry(void *o) {
    ARG_UNUSED(o);
    rpt_state_str = "rebooting";
    LOG_DBG("%s", __func__);

    LOG_PANIC();
    k_sleep(K_SECONDS(10));
    sys_reboot(SYS_REBOOT_COLD);
}

/* ── Public API ─────────────────────────────────────────────────── */

void reporting_init(struct reporting_state *state) {
    state->sample_interval_sec = CONFIG_APP_SAMPLING_INTERVAL_SECONDS;

    smf_set_initial(SMF_CTX(state), &states[STATE_SAMPLING]);
}

void reporting_process(struct reporting_state *state) {
    int err = smf_run_state(SMF_CTX(state));
    if (err) {
        LOG_ERR("smf_run_state(), error: %d", err);
        SEND_FATAL_ERROR();
    }
}
