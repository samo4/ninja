/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Cloud Reporting personality:
 *   Periodically requests an environmental sensor sample on
 *   CONFIG_APP_SAMPLING_INTERVAL_SECONDS boundaries.
 *
 *   The environmental module is self-propagating — once the first
 *   request is sent it keeps sampling autonomously.  The cloud_post
 *   module independently picks up each sample and sends it over LTE.
 *
 *   This personality only owns the sample-request timer; it does
 *   not manage the network or location.
 */

#ifndef _REPORTING_H_
#define _REPORTING_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "environmental.h"
#include "led.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Timer channel types */
enum timer_msg_type {
    TIMER_EXPIRED_SAMPLE_DATA,
};

struct timer_msg {
    enum timer_msg_type type;
};

ZBUS_CHAN_DECLARE(timer_chan);

/* The reporting personality only needs the timer — everything else
 * (network, cloud_post) runs independently on its own subscribers.
 */
#define REPORTING_CHANNEL_LIST(X) X(timer_chan, struct timer_msg)

#define REPORTING_MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(REPORTING_CHANNEL_LIST)

/* SMF states */
enum reporting_smf_state {
    STATE_SAMPLING,
    STATE_WAITING,
    STATE_REBOOTING,
};

struct reporting_state {
    struct smf_ctx ctx;

    /* Set by main loop — received channel and buffer */
    const struct zbus_channel *chan;
    uint8_t msg_buf[REPORTING_MAX_MSG_SIZE];

    /* Personality fields */
    uint32_t sample_interval_sec;
};

void reporting_init(struct reporting_state *state);
void reporting_process(struct reporting_state *state);

#ifdef __cplusplus
}
#endif

#endif /* _REPORTING_H_ */
