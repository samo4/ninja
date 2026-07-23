/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Empty personality:
 *   Does absolutely nothing — single idle state, no sensors, no cloud,
 *   no network, no timer, no external dependencies.  The main loop
 *   blocks on zbus (and times out to feed the watchdog), but the
 *   personality never subscribes to any channel.
 *
 *   State machine:
 *     IDLE — empty idle state, never subscribes to anything
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

static enum smf_state_result idle_run(void *o);

static const struct smf_state states[] = {
    [EMPTY_STATE_IDLE] = SMF_CREATE_STATE(NULL, idle_run, NULL, NULL, NULL),
};

static enum smf_state_result idle_run(void *o) {
    ARG_UNUSED(o);
    /* Nothing to do — ever. */
    return SMF_EVENT_PROPAGATE;
}

/* ── Public API ─────────────────────────────────────────────────── */

void empty_init(struct empty_state_object *state) {
    ARG_UNUSED(state);
    empty_state_name = "idle";
    smf_set_initial(SMF_CTX(state), &states[EMPTY_STATE_IDLE]);
}

void empty_process(struct empty_state_object *state) {
    int err = smf_run_state(SMF_CTX(state));

    if (err) {
        LOG_ERR("smf_run_state(), error: %d", err);
        SEND_FATAL_ERROR();
    }
}
