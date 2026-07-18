/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>
#include <date_time.h>
#include <adp536x.h>
#include <modem/nrf_modem_lib.h>
#include "modem/lte_lc.h"

#include "app_common.h"
#include "power.h"

LOG_MODULE_REGISTER(power, CONFIG_APP_POWER_LOG_LEVEL);

/* ADP5360 charger status register bits [2:0] */
#define ADP536X_CHG_STATUS_OFF      0
#define ADP536X_CHG_STATUS_TRICKLE  1
#define ADP536X_CHG_STATUS_CC       2
#define ADP536X_CHG_STATUS_CV       3
#define ADP536X_CHG_STATUS_COMPLETE 4
#define ADP536X_CHG_STATUS_LDO      5
#define ADP536X_CHG_STATUS_TIMEOUT  6
#define ADP536X_CHG_STATUS_DETECT   7

/* Active charging states mask (any charging state except OFF) */
#define ADP536X_CHG_ACTIVE_MASK 0x07

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(power);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(power_chan,
		 struct power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Private channel message types for internal state management. */
enum priv_power_msg_type {
	/** Modem has been initialized */
	POWER_PRIV_MODEM_INITIALIZED,
	/** Modem is entering sleep mode */
	POWER_PRIV_MODEM_SLEEP_ENTRY,
	/** Modem is exiting sleep mode */
	POWER_PRIV_MODEM_SLEEP_EXIT,
	/** Timer for sampling data has expired. */
	POWER_PRIV_TIMER_EXPIRED,
};

struct priv_power_msg {
	/* Type of the message */
	enum priv_power_msg_type type;
};

/* Create private power channel for internal messaging that is not intended for external use. */
ZBUS_CHAN_DEFINE(priv_power_chan,
		 struct priv_power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X)                                                                            \
	X(power_chan, struct power_msg)                                                            \
	X(priv_power_chan, struct priv_power_msg)

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add the power subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, power, 0);

CHANNEL_LIST(ADD_OBSERVERS)

enum power_module_state {
	/* The power module has started and is running */
	STATE_RUNNING,
	/* The device is active, performing tasks */
	STATE_ACTIVE,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct power_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last zbus message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Battery percentage (0-100) */
	uint8_t percentage;

	/* Battery voltage in millivolts */
	uint16_t voltage_mv;

	/* Charging status */
	bool charging;
};

/* Forward declarations of work function */
static void timer_sample_work_fn(struct k_work *work);

/* Delayable work used to schedule triggers */
K_WORK_DELAYABLE_DEFINE(timer_sample_work, timer_sample_work_fn);

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_active_entry(void *obj);
static enum smf_state_result state_active_run(void *obj);
static void state_active_exit(void *obj);

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry,
				 state_running_run,
				 NULL,
				 NULL,
				 &states[STATE_ACTIVE]),
	[STATE_ACTIVE] =
		SMF_CREATE_STATE(state_active_entry,
				 state_active_run,
				 state_active_exit,
				 &states[STATE_RUNNING],
				 NULL),
};

static void power_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id,
		k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void timer_sample_work_fn(struct k_work *work)
{
	int err;
	const struct priv_power_msg msg = {.type = POWER_PRIV_TIMER_EXPIRED};

	ARG_UNUSED(work);

	err = zbus_chan_pub(&priv_power_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("Failed to publish, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void timer_sample_start(uint32_t delay_ms)
{
	int err = k_work_reschedule(&timer_sample_work, K_MSEC(delay_ms));

	if (err < 0) {
		LOG_ERR("reschedule timer_sample_work, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void timer_sample_stop(void)
{
	int err = k_work_cancel_delayable(&timer_sample_work);

	if (err < 0) {
		LOG_ERR("cancel timer_sample_work, error: %d", err);
	}
}

static void send_battery_percentage_sample_response(const struct power_state_object *state_object)
{
	int err;
	struct power_msg response = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
	};

	response.percentage = state_object->percentage;
	response.charging = state_object->charging;
	response.voltage = (double)state_object->voltage_mv / 1000.0;
	err = date_time_now(&response.timestamp);
	if (err != 0 && err != -ENODATA) {
		LOG_ERR("date_time_now, error: %d", err);
	}

	LOG_DBG("Battery: %u%%, %.3f V, %s",
		 state_object->percentage,
		 (double)state_object->voltage_mv / 1000.0,
		 state_object->charging ? "charging" : "not charging");

	err = zbus_chan_pub(&power_chan, &response, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub sample response, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

#if defined(CONFIG_APP_POWER_SHELL)
static void log_battery_sample(const struct power_state_object *state_object)
{
	LOG_INF("Battery percentage: %u%%", state_object->percentage);
	LOG_INF("Battery voltage: %u mV", state_object->voltage_mv);
	LOG_INF("Battery is %s charging", state_object->charging ? "" : "not ");
}
#endif /* CONFIG_APP_POWER_SHELL */

static int sample_and_process(struct power_state_object *state_object)
{
	int err;
	uint8_t percentage;
	uint16_t millivolts;

	err = adp536x_fg_soc(&percentage);
	if (err) {
		LOG_ERR("adp536x_fg_soc failed: %d", err);
		return err;
	}

	err = adp536x_fg_volts(&millivolts);
	if (err) {
		LOG_ERR("adp536x_fg_volts failed: %d", err);
		return err;
	}

	state_object->percentage = percentage;
	state_object->voltage_mv = millivolts;

	/* Determine charging state from ADP5360 charger status */
	uint8_t chg_status;

	err = adp536x_charger_status_1_read(&chg_status);
	if (err) {
		LOG_ERR("adp536x_charger_status_1_read failed: %d", err);
		/* Assume not charging if we can't read status */
		state_object->charging = false;
	} else {
		state_object->charging = (chg_status & ADP536X_CHG_ACTIVE_MASK) != 0 &&
					 (chg_status & ADP536X_CHG_ACTIVE_MASK) != ADP536X_CHG_STATUS_OFF;
	}

	timer_sample_start(CONFIG_APP_POWER_SAMPLE_INTERVAL_MS);

	return 0;
}

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	int err;
	const struct power_msg msg = {.type = POWER_MODULE_READY};

	err = zbus_chan_pub(&power_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_running_run(void *obj)
{
	const struct power_state_object *state_object = obj;

	/* Handle sample requests */
	if (state_object->chan == &power_chan) {
		const struct power_msg *power_msg = (const struct power_msg *)state_object->msg_buf;

		if (power_msg->type == POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST) {
			send_battery_percentage_sample_response(state_object);
			return SMF_EVENT_HANDLED;
		}
#if defined(CONFIG_APP_POWER_SHELL)
		if (power_msg->type == POWER_BATTERY_SAMPLE_LOG) {
			log_battery_sample(state_object);
			return SMF_EVENT_HANDLED;
		}
#endif /* CONFIG_APP_POWER_SHELL */
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_active_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	/* Start periodic sampling timer */
	timer_sample_start(CONFIG_APP_POWER_SAMPLE_INTERVAL_MS);
}

static enum smf_state_result state_active_run(void *obj)
{
	struct power_state_object *state_object = obj;

	if (state_object->chan == &priv_power_chan) {
		const struct priv_power_msg *msg =
			(const struct priv_power_msg *)state_object->msg_buf;

		if (msg->type == POWER_PRIV_TIMER_EXPIRED) {
			sample_and_process(state_object);
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_active_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	timer_sample_stop();
}

static void power_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_POWER_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	static struct power_state_object power_state = {0};

	LOG_DBG("Power module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, power_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	/* Initialize the state machine — start in RUNNING, immediately transition to ACTIVE */
	smf_set_initial(SMF_CTX(&power_state), &states[STATE_RUNNING]);
	smf_set_state(SMF_CTX(&power_state), &states[STATE_ACTIVE]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("Failed to feed the watchdog: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&power, &power_state.chan, power_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&power_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(power_module_thread_id, CONFIG_APP_POWER_THREAD_STACK_SIZE, power_module_thread,
		NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
