/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Location Test personality:
 *   Request GNSS fix → post to cloud → turn off modem → sleep → repeat.
 *   If no location is found, still sends an empty POST.
 *   Reuses cloud_post for HTTP communication.
 */

#ifndef _LOCATION_TEST_H_
#define _LOCATION_TEST_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "cloud_post.h"
#include "location.h"
#include "network.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Timer channel types */
enum location_test_timer_msg_type {
    LOCATION_TEST_TIMER_EXPIRED,
};

struct location_test_timer_msg {
    enum location_test_timer_msg_type type;
};

ZBUS_CHAN_DECLARE(timer_chan);

/* X-macro: subscribe to timer (self), location (GNSS fix),
 * cloud_post (POST done), network (disconnect).
 */
#define LOCATION_TEST_CHANNEL_LIST(X)             \
    X(timer_chan, struct location_test_timer_msg) \
    X(location_chan, struct location_msg)         \
    X(cloud_post_chan, struct cloud_post_msg)     \
    X(network_chan, struct network_msg)

#define LOCATION_TEST_MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(LOCATION_TEST_CHANNEL_LIST)

/* SMF states */
enum location_test_state {
    LOCATION_TEST_STATE_WAITING_MODULE,
    LOCATION_TEST_STATE_SAMPLING,
    LOCATION_TEST_STATE_WAITING_LOCATION,
    LOCATION_TEST_STATE_WAITING_CLOUD,
    LOCATION_TEST_STATE_DISCONNECTING,
    LOCATION_TEST_STATE_SLEEPING,
    LOCATION_TEST_STATE_REBOOTING,
};

struct location_test_state_object {
    struct smf_ctx ctx;

    /* Set by main loop — received channel and buffer */
    const struct zbus_channel *chan;
    uint8_t msg_buf[LOCATION_TEST_MAX_MSG_SIZE];

    /* Personality fields */
    uint32_t sample_interval_sec;
    bool location_received;
    bool use_cellular_next;
};

void location_test_init(struct location_test_state_object *state);
void location_test_process(struct location_test_state_object *state);

#ifdef __cplusplus
}
#endif

#endif /* _LOCATION_TEST_H_ */
