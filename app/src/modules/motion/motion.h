/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _MOTION_H_
#define _MOTION_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channel provided by this module */
ZBUS_CHAN_DECLARE(motion_chan);

enum motion_msg_type {
	/* Output message types */

	/** Temperature sample data from the LIS2DTW12 sensor. */
	MOTION_TEMPERATURE_DATA = 0x1,

	/* Input message types */

	/** Request to sample the LIS2DTW12 temperature sensor. */
	MOTION_SAMPLE_REQUEST,
};

struct motion_msg {
	enum motion_msg_type type;

	/** Temperature in degrees Celsius. */
	double temperature;

	/** Timestamp when the sample was taken (uptime in milliseconds). */
	int64_t timestamp;
};

/**
 * @brief Get the current FSM state name of the motion module.
 *
 * @return Pointer to a statically allocated state name string.
 */
const char *motion_state_str(void);

#ifdef __cplusplus
}
#endif

#endif /* _MOTION_H_ */
