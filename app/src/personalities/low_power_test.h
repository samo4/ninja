/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Low Power Test personality:
 *   Bare minimum: connect to LTE, disconnect, sleep, repeat.
 *   No sensor sampling, no data posting, no REST — just
 *   measures baseline power of the LTE attach/detach cycle.
 */

#ifndef _LOW_POWER_TEST_H_
#define _LOW_POWER_TEST_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "network.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Timer channel types */
enum low_power_timer_msg_type {
    LOW_POWER_TIMER_EXPIRED_SAMPLE_DATA,
};

struct low_power_timer_msg {
    enum low_power_timer_msg_type type;
};

ZBUS_CHAN_DECLARE(timer_chan);

/* X-macro: low-power listens only to network + timer */
#define LOW_POWER_CHANNEL_LIST(X)       \
    X(network_chan, struct network_msg) \
    X(timer_chan, struct low_power_timer_msg)

#define LOW_POWER_MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(LOW_POWER_CHANNEL_LIST)

/* Low-power internal SMF states */
enum low_power_state {
    LOW_POWER_STATE_INITIAL_SAMPLE,
    LOW_POWER_STATE_CONNECTING,
    LOW_POWER_STATE_DISCONNECTING,
    LOW_POWER_STATE_SLEEPING,
    LOW_POWER_STATE_REBOOTING,
};

struct low_power_state_object {
    struct smf_ctx ctx;

    /* Set by main loop — received channel and buffer */
    const struct zbus_channel *chan;
    uint8_t msg_buf[LOW_POWER_MAX_MSG_SIZE];

    /* --- personality-specific fields --- */
    uint32_t sample_interval_sec;
};

void low_power_init(struct low_power_state_object *state);
void low_power_process(struct low_power_state_object *state);

#ifdef __cplusplus
}
#endif

#endif /* _LOW_POWER_TEST_H_ */
