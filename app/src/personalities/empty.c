/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Empty personality:
 *   Blinks LED 10×, samples motion sensor once, then sits idle forever.
 *   No network, no cloud, no timer — the personality never subscribes
 *   to any channel after init.
 *
 *   State machine:
 *     BLINKING — blink LED 10×, sample motion sensor, enter idle
 *     IDLE     — do nothing forever
 */

#include <zephyr/logging/log.h>
#include <zephyr/smf.h>

#include "app_common.h"
#include "empty.h"

LOG_MODULE_REGISTER(empty, CONFIG_APP_LOG_LEVEL);

/* ── Personality state string (for main heartbeat) ─────────────── */

static const char *empty_state_name;

const char *personality_state_str(void) { return empty_state_name ? empty_state_name : "?"; }

/* ── SMF states ─────────────────────────────────────────────────── */

static void blinking_entry(void *o);
static enum smf_state_result idle_run(void *o);

static const struct smf_state states[] = {
    [EMPTY_STATE_BLINKING] = SMF_CREATE_STATE(blinking_entry, NULL, NULL, NULL, NULL),
    [EMPTY_STATE_IDLE] = SMF_CREATE_STATE(NULL, idle_run, NULL, NULL, NULL),
};

static void blinking_entry(void *o) {
    struct empty_state_object *state = (struct empty_state_object *)o;
    empty_state_name = "blinking";

#if defined(CONFIG_APP_LED)
    LED_BLINK_RED(10);
#endif /* CONFIG_APP_LED */
#if defined(CONFIG_APP_MOTION)
    PUBLISH_MOTION(MOTION_SAMPLE_REQUEST);
#endif /* CONFIG_APP_MOTION */

    empty_state_name = "idle";
    smf_set_state(SMF_CTX(state), &states[EMPTY_STATE_IDLE]);
}

static enum smf_state_result idle_run(void *o) {
    ARG_UNUSED(o);
    /* Nothing to do — ever. */
    return SMF_EVENT_PROPAGATE;
}

/* ── Public API ─────────────────────────────────────────────────── */

void empty_init(struct empty_state_object *state) {
    ARG_UNUSED(state);
    empty_state_name = "init";
    smf_set_initial(SMF_CTX(state), &states[EMPTY_STATE_BLINKING]);
}

void empty_process(struct empty_state_object *state) {
    int err = smf_run_state(SMF_CTX(state));

    if (err) {
        LOG_ERR("smf_run_state(), error: %d", err);
        SEND_FATAL_ERROR();
    }
}
