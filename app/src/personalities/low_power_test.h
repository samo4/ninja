/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Low Power Test personality:
 *   Measure sensors → post to cloud → turn off modem → sleep → repeat.
 *   Half as many measurements as reporting, but allows measuring
 *   board power consumption with everything off during sleep.
 */

#ifndef _LOW_POWER_TEST_H_
#define _LOW_POWER_TEST_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "cloud_post.h"
#if defined(CONFIG_APP_MOTION)
#include "motion.h"
#elif defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif
#include "led.h"
#include "network.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Timer channel types */
enum low_power_timer_msg_type {
    LOW_POWER_TIMER_EXPIRED,
};

struct low_power_timer_msg {
    enum low_power_timer_msg_type type;
};

ZBUS_CHAN_DECLARE(timer_chan);

/* X-macro: subscribe to timer (self), sensor (motion or environmental),
 * network (connect/disconnect events), and cloud_post (POST done).
 */
#define LOW_POWER_CHANNEL_LIST(X)                                                           \
    X(timer_chan, struct low_power_timer_msg)                                               \
    X(cloud_post_chan, struct cloud_post_msg)                                               \
    IF_ENABLED(CONFIG_APP_MOTION, (X(motion_chan, struct motion_msg)))                      \
    IF_ENABLED(CONFIG_APP_ENVIRONMENTAL, (X(environmental_chan, struct environmental_msg))) \
    X(network_chan, struct network_msg)

#define LOW_POWER_MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(LOW_POWER_CHANNEL_LIST)

#define LOW_POWER_MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(LOW_POWER_CHANNEL_LIST)

/* SMF states */
enum low_power_state {
    LOW_POWER_STATE_SAMPLING,
    LOW_POWER_STATE_WAITING,
    LOW_POWER_STATE_DISCONNECTING,
    LOW_POWER_STATE_SLEEPING,
    LOW_POWER_STATE_REBOOTING,
};

struct low_power_state_object {
    struct smf_ctx ctx;

    /* Set by main loop — received channel and buffer */
    const struct zbus_channel *chan;
    uint8_t msg_buf[LOW_POWER_MAX_MSG_SIZE];

    /* Personality fields */
    uint32_t sample_interval_sec;
};

void low_power_init(struct low_power_state_object *state);
void low_power_process(struct low_power_state_object *state);

#ifdef __cplusplus
}
#endif

#endif /* _LOW_POWER_TEST_H_ */
