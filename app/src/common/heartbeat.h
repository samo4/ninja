/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**@file
 *
 * @brief Heartbeat — periodic one-line FSM state report.
 *
 * Collects the current SMF state from every module that exposes a state
 * string getter and prints a combined line over RTT once per minute.
 *
 * Call @ref heartbeat_start after the personality has been initialised.
 */

#ifndef HEARTBEAT_H__
#define HEARTBEAT_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the periodic heartbeat.
 *
 * The first heartbeat fires after @c HEARTBEAT_INTERVAL_SEC seconds
 * and reschedules itself automatically.
 */
void heartbeat_start(void);

#ifdef __cplusplus
}
#endif

#endif /* HEARTBEAT_H__ */
