/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Main entry point — thin dispatcher.
 *
 * Select the personality at build time via Kconfig:
 *   CONFIG_APP_PERSONALITY_REPORTING     — periodic cloud reporting
 *   CONFIG_APP_PERSONALITY_LOW_POWER_TEST — measure, post, disconnect, sleep
 *
 * Each personality owns its own SMF state machine.  main() just
 * feeds the watchdog, waits for zbus messages, and dispatches
 * them to the active personality.
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "heartbeat.h"

/* ==================================================================
 * Personality selection — exactly one is compiled at build time.
 * Each header provides:
 *   - A _CHANNEL_LIST(X) macro listing subscribed channels
 *   - A _MAX_MSG_SIZE constant
 *   - A state object type with embedded smf_ctx
 *   - personality_init(state) / personality_process(state)
 * ================================================================== */

#if defined(CONFIG_APP_PERSONALITY_REPORTING)
#include "personalities/reporting.h"

#define PERSONALITY_CHANNEL_LIST REPORTING_CHANNEL_LIST
#define PERSONALITY_MAX_MSG_SIZE REPORTING_MAX_MSG_SIZE
#define PERSONALITY_STATE struct reporting_state
#define personality_init reporting_init
#define personality_process reporting_process

#elif defined(CONFIG_APP_PERSONALITY_LOW_POWER_TEST)
#include "personalities/low_power_test.h"

#define PERSONALITY_CHANNEL_LIST LOW_POWER_CHANNEL_LIST
#define PERSONALITY_MAX_MSG_SIZE LOW_POWER_MAX_MSG_SIZE
#define PERSONALITY_STATE struct low_power_state_object
#define personality_init low_power_init
#define personality_process low_power_process

#elif defined(CONFIG_APP_PERSONALITY_LOCATION_TEST)
#include "personalities/location_test.h"

#define PERSONALITY_CHANNEL_LIST LOCATION_TEST_CHANNEL_LIST
#define PERSONALITY_MAX_MSG_SIZE LOCATION_TEST_MAX_MSG_SIZE
#define PERSONALITY_STATE struct location_test_state_object
#define personality_init location_test_init
#define personality_process location_test_process

#elif defined(CONFIG_APP_PERSONALITY_EMPTY)
#include "personalities/empty.h"

#define PERSONALITY_CHANNEL_LIST EMPTY_CHANNEL_LIST
#define PERSONALITY_MAX_MSG_SIZE EMPTY_MAX_MSG_SIZE
#define PERSONALITY_STATE struct empty_state_object
#define personality_init empty_init
#define personality_process empty_process

#else
#error "No personality selected!"
#endif

LOG_MODULE_REGISTER(main, 4);

ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

// Add main_subscriber as observer to all channels in the personality's list
#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);

PERSONALITY_CHANNEL_LIST(ADD_OBSERVERS)

/* ── Watchdog ───────────────────────────────────────────────────── */

TASK_WDT_CALLBACK_DEFINE(task)

void main(void) {
    int err;
    TASK_WDT_TIMEOUTS(APP);
    TASK_WDT_ZBUS_TIMEOUT;
    static PERSONALITY_STATE state;

    LOG_INF("Main has started");

    personality_init(&state);
    heartbeat_start();

    TASK_WDT_ADD(task, wdt_timeout_ms)

    // Run the initial SMF transition (e.g. fire first sample)
    personality_process(&state);

    while (1) {
        TASK_WDT_FEED();
        err = zbus_sub_wait_msg(&main_subscriber, &state.chan, state.msg_buf, zbus_wait_ms);
        if (err == -ENOMSG) {
            continue;
        } else if (err) {
            LOG_ERR("zbus_sub_wait_msg, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
        personality_process(&state);
    }
}
