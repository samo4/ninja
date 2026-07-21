/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**@file
 *
 * @brief Module state reporting interface.
 *
 * Modules with an SMF state machine can optionally implement a state string
 * getter so that the main heartbeat can collect and display the current
 * FSM state of every module in a single log line.
 *
 * Each getter returns a pointer to a statically allocated string (never NULL).
 * Modules that are not compiled in, or that do not implement the getter,
 * will report "?" via the weak default defined in module_state.c.
 */

#ifndef MODULE_STATE_H__
#define MODULE_STATE_H__

#include <stddef.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of a combined state line (including NUL). */
#define STATE_LINE_MAX_LEN 160

/**
 * @brief Optional: personality state string.
 *
 * Each personality (location_test, low_power_test, reporting) shall provide
 * this function returning a human-readable name of the current SMF state.
 */
const char *personality_state_str(void);

/**
 * @brief Optional: network module FSM state string.
 */
const char *network_state_str(void);

/**
 * @brief Optional: location module FSM state string.
 */
const char *location_state_str(void);

/**
 * @brief Optional: environmental module FSM state string.
 */
const char *environmental_state_str(void);

/**
 * @brief Optional: motion module FSM state string.
 */
const char *motion_state_str(void);

/**
 * @brief Format a combined one-line state report into a buffer.
 *
 * Example output:
 *   "♥ per:waiting_cloud net:connected loc:inactive env:run"
 *
 * @param[out] buf   Destination buffer.
 * @param[in]  len   Size of the destination buffer.
 *
 * @return The number of characters written (excluding NUL).
 */
int state_report_format(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_STATE_H__ */
