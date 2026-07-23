/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _ENVIRONMENTAL_H_
#define _ENVIRONMENTAL_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(environmental_chan);

enum environmental_msg_type {
    /* Output message types */

    /* Response message to a request for current environmental sensor values.
     * The sampled values are found in the respective fields of the message structure.
     */
    ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE = 0x1,

    /* Input message types */

    /* Request to sample the current environmental sensor values.
     * The response is sent as a ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE message.
     */
    ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST,
};

struct environmental_msg {
    enum environmental_msg_type type;

    /** Contains the current temperature in celsius. */
    double temperature;

    /** Timestamp when the sample was taken in milliseconds.
     *  This is either:
     * - Unix time in milliseconds if the system clock was synchronized at sampling time, or
     * - Uptime in milliseconds if the system clock was not synchronized at sampling time.
     * Only valid for ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE events.
     */
    int64_t timestamp;
};

/** Convenience macro: publish an environmental message of the given type. */
#define PUBLISH_ENVIRONMENTAL(msg_type)                                          \
    do {                                                                         \
        const struct environmental_msg _msg = {.type = (msg_type)};              \
        int _err = zbus_chan_pub(&environmental_chan, &_msg, PUB_TIMEOUT);       \
        if (_err) {                                                              \
            LOG_ERR("Failed to publish environmental message, error: %d", _err); \
            SEND_FATAL_ERROR();                                                  \
        }                                                                        \
    } while (0)

/**
 * @brief Get the current FSM state name of the environmental module.
 *
 * @return Pointer to a statically allocated state name string.
 */
const char *environmental_state_str(void);

#ifdef __cplusplus
}
#endif

#endif /* _ENVIRONMENTAL_H_ */
