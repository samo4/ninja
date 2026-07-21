/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _APP_COMMON_H_
#define _APP_COMMON_H_

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/util.h> /* For Zephyr's utility macros, including MAX */
#include <zephyr/task_wdt/task_wdt.h>
#if defined(CONFIG_MEMFAULT)
#include <memfault/panics/assert.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Handle fatal error.
 *  @param is_watchdog_timeout Boolean indicating if the macro was called upon a watchdog timeout.
 */
#define FATAL_ERROR_HANDLE(is_watchdog_timeout)                          \
    do {                                                                 \
        LOG_PANIC();                                                     \
        if (is_watchdog_timeout) {                                       \
            IF_ENABLED(CONFIG_MEMFAULT, (MEMFAULT_SOFTWARE_WATCHDOG())); \
        }                                                                \
        k_sleep(K_SECONDS(10));                                          \
        __ASSERT(false, "SEND_FATAL_ERROR() macro called");              \
    } while (0)

/** @brief Macro used to handle fatal errors. */
#define SEND_FATAL_ERROR() FATAL_ERROR_HANDLE(0)

/** @brief Macro used to handle watchdog timeouts. */
#define SEND_FATAL_ERROR_WATCHDOG_TIMEOUT() FATAL_ERROR_HANDLE(1)

/* Helper macro to create union member from channel and type */
#define UNION_MEMBER(_chan, _type) _type _chan##_data_type;

/**
 * @brief Macro to compute the maximum message size from a list of channel types.
 *
 * @param _CHAN_LIST List of channels to compute the maximum message size from.
 *		     The list should be in the format: (CHANNEL_NAME, type)
 *
 * @return Maximum message size from the list of channels
 */
#define MAX_MSG_SIZE_FROM_LIST(_CHAN_LIST) sizeof(union {_CHAN_LIST(UNION_MEMBER)})

/* Timeout for zbus channel publishing */
#define PUB_TIMEOUT K_MSEC(100)

/**
 * @brief Define a task WDT callback for a module thread.
 *
 * Creates a static callback function named @p prefix##_wdt_callback that logs
 * the watchdog expiry and triggers a fatal error via SEND_FATAL_ERROR_WATCHDOG_TIMEOUT().
 *
 * @param prefix Symbol prefix used to generate the callback name.
 */
#define TASK_WDT_CALLBACK_DEFINE(prefix)                                                                         \
    static void prefix##_wdt_callback(int channel_id, void *user_data) {                                         \
        LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id, k_thread_name_get((k_tid_t)user_data)); \
        SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();                                                                     \
    }

/**
 * @brief Register the current thread with the task WDT.
 *
 * Declares @c task_wdt_id, calls task_wdt_add() with the given @p prefix
 * callback and timeout, and returns from the enclosing void function on
 * failure.
 *
 * Requires a matching TASK_WDT_CALLBACK_DEFINE(prefix) earlier in the file.
 *
 * @param prefix     Symbol prefix matching TASK_WDT_CALLBACK_DEFINE().
 * @param timeout_ms Watchdog timeout in milliseconds.
 */
#define TASK_WDT_ADD(prefix, timeout_ms)                                                        \
    int task_wdt_id = task_wdt_add(timeout_ms, prefix##_wdt_callback, (void *)k_current_get()); \
    if (task_wdt_id < 0) {                                                                      \
        LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);                             \
        SEND_FATAL_ERROR();                                                                     \
        return;                                                                                 \
    }

/**
 * @brief Declare the WDT timeout variables from Kconfig.
 *
 * Declares @c wdt_timeout_ms and @c execution_time_ms as @c const @c uint32_t
 * using the given CONFIG root (e.g. @c APP_AGNSS_DATA expands to
 * @c CONFIG_APP_AGNSS_DATA_WATCHDOG_TIMEOUT_SECONDS).
 *
 * @param config_root The CONFIG_* root, e.g. @c APP for main() or
 *                    @c APP_AGNSS_DATA for the A-GNSS module.
 */
#define TASK_WDT_TIMEOUTS(config_root)                                                                \
    const uint32_t wdt_timeout_ms = (CONFIG_##config_root##_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC); \
    const uint32_t execution_time_ms = (CONFIG_##config_root##_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC)

/**
 * @brief Declare the zbus wait timeout from the WDT deadline.
 *
 * Requires @c wdt_timeout_ms and @c execution_time_ms in scope (declared just
 * before the feed loop).
 */
#define TASK_WDT_ZBUS_TIMEOUT const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms)

/**
 * @brief Feed the task WDT and return on failure (void function).
 *
 * Requires @c err (int) and @c task_wdt_id in scope.
 */
#define TASK_WDT_FEED()                                  \
    err = task_wdt_feed(task_wdt_id);                    \
    if (err) {                                           \
        LOG_ERR("Failed to feed the watchdog: %d", err); \
        SEND_FATAL_ERROR();                              \
        return;                                          \
    }

#ifdef __cplusplus
}
#endif

#endif /* _APP_COMMON_H_ */
