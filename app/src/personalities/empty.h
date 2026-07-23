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
 *   Useful as a build test or a truly minimal-power baseline.
 */

#ifndef _EMPTY_H_
#define _EMPTY_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#if defined(CONFIG_APP_LED)
#include "led.h"
#endif
#if defined(CONFIG_APP_MOTION)
#include "motion.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* No channels — idle-only personality. */
#define EMPTY_CHANNEL_LIST(X)

#define EMPTY_MAX_MSG_SIZE 1

/* SMF states */
enum empty_state {
    EMPTY_STATE_BLINKING,
    EMPTY_STATE_IDLE,
};

struct empty_state_object {
    struct smf_ctx ctx;

    /* Set by main loop — received channel and buffer */
    const struct zbus_channel *chan;
    uint8_t msg_buf[EMPTY_MAX_MSG_SIZE];
};

void empty_init(struct empty_state_object *state);
void empty_process(struct empty_state_object *state);

#ifdef __cplusplus
}
#endif

#endif /* _EMPTY_H_ */
