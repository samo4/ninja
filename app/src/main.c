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

#else
#error "No personality selected!"
#endif

LOG_MODULE_REGISTER(main, 4);

ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

// Add main_subscriber as observer to all channels in the personality's list
#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);

PERSONALITY_CHANNEL_LIST(ADD_OBSERVERS)

// Static helper function
static void task_wdt_callback(int channel_id, void *user_data) {
    LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id, k_thread_name_get((k_tid_t)user_data));
    SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

int main(void) {
    int err;
    int task_wdt_id;
    const uint32_t wdt_timeout_ms = (CONFIG_APP_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
    const uint32_t execution_time_ms = (CONFIG_APP_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
    const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
    static PERSONALITY_STATE state;

    LOG_INF("Main has started");

    personality_init(&state);

    task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());
    if (task_wdt_id < 0) {
        LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
        SEND_FATAL_ERROR();
        return -EFAULT;
    }

    // Run the initial SMF transition (e.g. fire first sample)
    personality_process(&state);

    while (1) {
        err = task_wdt_feed(task_wdt_id);
        if (err) {
            LOG_ERR("task_wdt_feed, error: %d", err);
            SEND_FATAL_ERROR();
            return err;
        }
        err = zbus_sub_wait_msg(&main_subscriber, &state.chan, state.msg_buf, zbus_wait_ms);
        if (err == -ENOMSG) {
            continue;
        } else if (err) {
            LOG_ERR("zbus_sub_wait_msg, error: %d", err);
            SEND_FATAL_ERROR();
            return err;
        }
        personality_process(&state);
    }
}
