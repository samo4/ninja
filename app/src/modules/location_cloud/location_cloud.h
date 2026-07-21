/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

ZBUS_CHAN_DECLARE(location_cloud_chan);

enum location_cloud_msg_type {
    LOCATION_CLOUD_AGNSS_FETCH_DONE,
    LOCATION_CLOUD_AGNSS_FETCH_FAILED,
    LOCATION_CLOUD_CELLULAR_DONE,
    LOCATION_CLOUD_CELLULAR_FAILED,
};

struct location_cloud_msg {
    enum location_cloud_msg_type type;
    int http_status;
};

#ifdef __cplusplus
}
#endif
