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
 *   Useful as a power measurement baseline that still exercises the
 *   full LTE + cloud path.
 */

#ifndef _EMPTY_H_
#define _EMPTY_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "cloud_post.h"
#if defined(CONFIG_APP_LED)
#include "led.h"
#endif
#include "network.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Timer channel types */
enum empty_timer_msg_type {
    EMPTY_TIMER_EXPIRED,
};

struct empty_timer_msg {
    enum empty_timer_msg_type type;
};

ZBUS_CHAN_DECLARE(timer_chan);

/* X-macro: subscribe to timer (self), cloud_post (POST done), network (disconnect/connect). */
#define EMPTY_CHANNEL_LIST(X)                 \
    X(timer_chan, struct empty_timer_msg)     \
    X(cloud_post_chan, struct cloud_post_msg) \
    X(network_chan, struct network_msg)

#define EMPTY_MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(EMPTY_CHANNEL_LIST)

/* SMF states */
enum empty_state {
    EMPTY_STATE_WAITING_MODULE,
    EMPTY_STATE_SAMPLING,
    EMPTY_STATE_WAITING_CLOUD,
    EMPTY_STATE_DISCONNECTING,
    EMPTY_STATE_SLEEPING,
};

struct empty_state_object {
    struct smf_ctx ctx;

    /* Set by main loop — received channel and buffer */
    const struct zbus_channel *chan;
    uint8_t msg_buf[EMPTY_MAX_MSG_SIZE];

    /* Personality fields */
    uint32_t sample_interval_sec;
};

void empty_init(struct empty_state_object *state);
void empty_process(struct empty_state_object *state);

#ifdef __cplusplus
}
#endif

#endif /* _EMPTY_H_ */
