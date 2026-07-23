/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**@file
 *
 * @brief   LED module.
 *
 * Module that handles LED behaviour.
 */

#ifndef LED_H__
#define LED_H__

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(led_chan);

enum led_msg_type {
    LED_RGB_SET,
};

struct led_msg {
    enum led_msg_type type;

    /** RGB values (0 to 255) */
    uint8_t red;
    uint8_t green;

    /** Duration of the RGB on/off cycle */
    uint32_t duration_on_msec;
    uint32_t duration_off_msec;

    /** Number of on/off cycles (-1 indicates forever) */
    int repetitions;
};

/** Convenience macro: publish green LED blink pattern (250 ms on, 500 ms off, n reps). */
#define LED_BLINK_GREEN(reps) \
    do { \
        const struct led_msg _led = { \
            .type = LED_RGB_SET, \
            .red = 0, \
            .green = 100, \
            .duration_on_msec = 250, \
            .duration_off_msec = 500, \
            .repetitions = (reps), \
        }; \
        int _err = zbus_chan_pub(&led_chan, &_led, PUB_TIMEOUT); \
        if (_err) { \
            LOG_ERR("Failed to publish LED pattern, error: %d", _err); \
            SEND_FATAL_ERROR(); \
        } \
    } while (0)

/** Convenience macro: publish red LED blink pattern (250 ms on, 500 ms off, n reps). */
#define LED_BLINK_RED(reps) \
    do { \
        const struct led_msg _led = { \
            .type = LED_RGB_SET, \
            .red = 100, \
            .green = 0, \
            .duration_on_msec = 250, \
            .duration_off_msec = 500, \
            .repetitions = (reps), \
        }; \
        int _err = zbus_chan_pub(&led_chan, &_led, PUB_TIMEOUT); \
        if (_err) { \
            LOG_ERR("Failed to publish LED pattern, error: %d", _err); \
            SEND_FATAL_ERROR(); \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* LED_H__ */
