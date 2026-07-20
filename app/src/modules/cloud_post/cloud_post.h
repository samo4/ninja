/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_POST_H_
#define _CLOUD_POST_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(cloud_post_chan);

enum cloud_post_msg_type {
    CLOUD_POST_SEND_DONE,
    CLOUD_POST_SEND_FAILED,
};

struct cloud_post_msg {
    enum cloud_post_msg_type type;
    int http_status;
};

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_POST_H_ */
