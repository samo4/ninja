/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "heartbeat.h"
#include "module_state.h"

LOG_MODULE_REGISTER(heartbeat, CONFIG_APP_LOG_LEVEL);

#define HEARTBEAT_INTERVAL_SEC 60

static void heartbeat_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_fn);

static void heartbeat_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	char buf[STATE_LINE_MAX_LEN];

	state_report_format(buf, sizeof(buf));
	LOG_INF("%s", buf);
	k_work_reschedule(&heartbeat_work, K_SECONDS(HEARTBEAT_INTERVAL_SEC));
}

void heartbeat_start(void)
{
	k_work_reschedule(&heartbeat_work, K_SECONDS(HEARTBEAT_INTERVAL_SEC));
}
