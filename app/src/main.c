/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "network.h"
#if defined(CONFIG_APP_LOCATION)
#include "location.h"
#endif

#if defined(CONFIG_APP_LED)
#include "led.h"
#endif /* CONFIG_APP_LED */

#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif /* CONFIG_APP_ENVIRONMENTAL */

#if defined(CONFIG_APP_POWER)
#include "power.h"
#endif /* CONFIG_APP_POWER */

/* Register log module */
LOG_MODULE_REGISTER(main, 4);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(main_subscriber);

enum timer_msg_type {
    /* Timer for sampling data has expired.
     * This timer is used to trigger the sampling of data from the sensors.
     * The timer is set to expire every CONFIG_APP_SAMPLING_INTERVAL_SECONDS.
     */
    TIMER_EXPIRED_SAMPLE_DATA,
};

struct timer_msg {
    enum timer_msg_type type;
};

ZBUS_CHAN_DEFINE(timer_chan, struct timer_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 * We use the X-macros to make the code more maintainable.
 */
#define CHANNEL_LIST(X)                                                      \
    X(network_chan, struct network_msg)                                      \
    IF_ENABLED(CONFIG_APP_LOCATION, (X(location_chan, struct location_msg))) \
    X(timer_chan, struct timer_msg)                                          \
    IF_ENABLED(CONFIG_APP_POWER, (X(power_chan, struct power_msg)))

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add main_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);

/*
 * Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list.
 * Example: ZBUS_CHAN_ADD_OBS(cloud_chan, main_subscriber, 0);
 */
CHANNEL_LIST(ADD_OBSERVERS)

/* Forward declarations */
static void timer_sample_data_work_fn(struct k_work *work);
static void timer_sample_start(uint32_t delay_sec);
static void timer_sample_stop(void);

/* Delayable work used to schedule triggers */
static K_WORK_DELAYABLE_DEFINE(timer_sample_data_work, timer_sample_data_work_fn);

/* Forward declarations of state handlers */
static enum smf_state_result waiting_for_modules_init_run(void *o);
static enum smf_state_result running_run(void *o);
static void sampling_entry(void *o);
static enum smf_state_result sampling_run(void *o);
static void waiting_entry(void *o);
static enum smf_state_result waiting_run(void *o);
static void waiting_exit(void *o);
static void rebooting_entry(void *o);

enum app_state {
    /* Waiting for module initialization */
    STATE_WAITING_FOR_MODULES_INIT,
    /* Main application is running */
    STATE_RUNNING,
    /* Sampling sensor data */
    STATE_SAMPLING,
    /* Waiting for next sample trigger or user input */
    STATE_WAITING,
    /* Cleanup and reboot the device. Terminal state */
    STATE_REBOOTING,
};

/* State object for the app module.
 * Used to transfer data between state changes.
 */
struct main_state {
    /* This must be first */
    struct smf_ctx ctx;

    /* Last channel type that a message was received on */
    const struct zbus_channel *chan;

    /* Last received message */
    uint8_t msg_buf[MAX_MSG_SIZE];

    /* Trigger interval */
    uint32_t sample_interval_sec;

    /* Start time of the most recent sampling. This is used to calculate the correct
     * time when scheduling the next sampling trigger.
     */
    uint32_t sample_start_time;

    /* Used to fire the very first sample immediately on boot regardless
     * of sample_start_time.
     */
    bool first_sample_pending;

    /* Flags to track if each module is ready */
    struct {
#if defined(CONFIG_APP_LOCATION)
        bool location_ready;
#endif
    } modules_ready;
};

/* Construct state table */
static const struct smf_state states[] = {
    /* Initial state, waiting for modules to initialize */
    [STATE_WAITING_FOR_MODULES_INIT] = SMF_CREATE_STATE(NULL, waiting_for_modules_init_run, NULL, NULL, NULL),
    /* Top-level states */
    [STATE_RUNNING] = SMF_CREATE_STATE(NULL, running_run, NULL, NULL, &states[STATE_SAMPLING]),
    /* Operation states */
    [STATE_SAMPLING] = SMF_CREATE_STATE(sampling_entry, sampling_run, NULL, &states[STATE_RUNNING], NULL),
    [STATE_WAITING] = SMF_CREATE_STATE(waiting_entry, waiting_run, waiting_exit, &states[STATE_RUNNING], NULL),
    /* Reboot state */
    [STATE_REBOOTING] = SMF_CREATE_STATE(rebooting_entry, NULL, NULL, NULL, NULL),
};

/* Static helper function */

static void task_wdt_callback(int channel_id, void *user_data) {
    LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id, k_thread_name_get((k_tid_t)user_data));

    SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* Common helpers for substates */

static void trigger_sampling(struct main_state *state_object) {
    int err;

#if defined(CONFIG_APP_LOCATION)
    struct location_msg location_msg = {
        .type = LOCATION_SEARCH_TRIGGER,
    };
#endif

#if defined(CONFIG_APP_LED)
    /* Green pattern to indicate sampling */
    struct led_msg led_msg = {
        .type = LED_RGB_SET,
        .red = 0,
        .green = 55,
        .duration_on_msec = 250,
        .duration_off_msec = 2000,
        .repetitions = 10,
    };

    err = zbus_chan_pub(&led_chan, &led_msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish LED pattern message, error: %d", err);
        SEND_FATAL_ERROR();

        return;
    }
#endif /* CONFIG_APP_LED */

    state_object->sample_start_time = k_uptime_seconds();
    state_object->first_sample_pending = false;

#if defined(CONFIG_APP_POWER)
    struct power_msg power_msg = {
        .type = POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST,
    };

    err = zbus_chan_pub(&power_chan, &power_msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish power battery sample request, error: %d", err);
        SEND_FATAL_ERROR();

        return;
    }
#endif /* CONFIG_APP_POWER */

#if defined(CONFIG_APP_ENVIRONMENTAL)
    struct environmental_msg environmental_msg = {
        .type = ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST,
    };

    err = zbus_chan_pub(&environmental_chan, &environmental_msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish environmental sensor sample request, error: %d", err);
        SEND_FATAL_ERROR();

        return;
    }
#endif /* CONFIG_APP_ENVIRONMENTAL */

#if defined(CONFIG_APP_LOCATION)
    err = zbus_chan_pub(&location_chan, &location_msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish location search trigger, error: %d", err);
        SEND_FATAL_ERROR();

        return;
    }
#endif /* CONFIG_APP_LOCATION */
}

static void waiting_entry_common(const struct main_state *state_object) {
    uint32_t time_elapsed;
    uint32_t time_remaining;

    /* Reschedule the next sample trigger */

    if (state_object->first_sample_pending) {
        time_remaining = 0;
    } else {
        time_elapsed = k_uptime_seconds() - state_object->sample_start_time;

        if (time_elapsed > state_object->sample_interval_sec) {
            LOG_WRN("Sampling took longer than the interval, time_elapsed: %d, "
                    "interval: %d",
                    time_elapsed, state_object->sample_interval_sec);
            time_remaining = 0;
        } else {
            time_remaining = state_object->sample_interval_sec - time_elapsed;
        }
    }

    LOG_DBG("Next sample trigger in %d seconds", time_remaining);

    timer_sample_start(time_remaining);
}

static void waiting_exit_common(void) { timer_sample_stop(); }

static void timer_sample_data_work_fn(struct k_work *work) {
    int err;
    const struct timer_msg msg = {.type = TIMER_EXPIRED_SAMPLE_DATA};

    ARG_UNUSED(work);

    err = zbus_chan_pub(&timer_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish sample data timer expired message, error: %d", err);
        SEND_FATAL_ERROR();

        return;
    }
}

static void timer_sample_start(uint32_t delay_sec) {
    int err;

    err = k_work_reschedule(&timer_sample_data_work, K_SECONDS(delay_sec));
    if (err < 0) {
        LOG_ERR("k_work_reschedule timer_sample_data_work, error: %d", err);
        SEND_FATAL_ERROR();

        return;
    }
}

static void timer_sample_stop(void) {
    int err;

    err = k_work_cancel_delayable(&timer_sample_data_work);
    if (err < 0) {
        LOG_ERR("k_work_cancel_delayable timer_sample_data_work, error: %d", err);
    }
}

/* Zephyr State Machine framework handlers */

/* STATE_WAITING_FOR_MODULES_INIT */
static enum smf_state_result waiting_for_modules_init_run(void *o) {
    struct main_state *state_object = (struct main_state *)o;

    /* Wait for location module to be ready (if enabled), then transition to running. */
#if defined(CONFIG_APP_LOCATION)
    if (state_object->chan == &location_chan) {
        const struct location_msg *msg = (const struct location_msg *)state_object->msg_buf;

        if (msg->type == LOCATION_MODULE_READY) {
            smf_set_state(SMF_CTX(state_object), &states[STATE_RUNNING]);
            return SMF_EVENT_HANDLED;
        }
    }
#else
    /* No modules that need initialization tracking, go straight to running. */
    smf_set_state(SMF_CTX(state_object), &states[STATE_RUNNING]);
    return SMF_EVENT_HANDLED;
#endif /* CONFIG_APP_LOCATION */

    return SMF_EVENT_PROPAGATE;
}

/* STATE_RUNNING */
static enum smf_state_result running_run(void *o) {
    ARG_UNUSED(o);
    return SMF_EVENT_PROPAGATE;
}

/* STATE_SAMPLING */

static void sampling_entry(void *o) {
    struct main_state *state_object = (struct main_state *)o;

    LOG_INF("trigger_sampling: %us", state_object->sample_interval_sec);
    trigger_sampling(state_object);
}

static enum smf_state_result sampling_run(void *o) {
#if defined(CONFIG_APP_LOCATION)
    struct main_state *state_object = (struct main_state *)o;
    if (state_object->chan == &location_chan) {
        const struct location_msg *msg = (const struct location_msg *)state_object->msg_buf;

        if (msg->type == LOCATION_SEARCH_DONE) {
            smf_set_state(SMF_CTX(state_object), &states[STATE_WAITING]);
            return SMF_EVENT_HANDLED;
        }
    }
#else
    /* No location module: transition to waiting state immediately so that the
     * next sampling timer can be scheduled without waiting for a
     * LOCATION_SEARCH_DONE that will never come.
     */
    struct main_state *state_object = (struct main_state *)o;

    smf_set_state(SMF_CTX(state_object), &states[STATE_WAITING]);
    return SMF_EVENT_HANDLED;
#endif /* CONFIG_APP_LOCATION */

    return SMF_EVENT_PROPAGATE;
}

/* STATE_WAITING */

static void waiting_entry(void *o) {
    const struct main_state *state_object = (const struct main_state *)o;

    LOG_DBG("%s", __func__);
    waiting_entry_common(state_object);
}

static enum smf_state_result waiting_run(void *o) {
    struct main_state *state_object = (struct main_state *)o;

    if (state_object->chan == &timer_chan) {
        const struct timer_msg *msg = (const struct timer_msg *)state_object->msg_buf;

        if (msg->type == TIMER_EXPIRED_SAMPLE_DATA) {
            smf_set_state(SMF_CTX(state_object), &states[STATE_SAMPLING]);

            return SMF_EVENT_HANDLED;
        }
    }

    return SMF_EVENT_PROPAGATE;
}

static void waiting_exit(void *o) {
    ARG_UNUSED(o);
    LOG_DBG("%s", __func__);

    waiting_exit_common();
}

/* STATE_REBOOTING */

static void rebooting_entry(void *o) {
    ARG_UNUSED(o);

    LOG_DBG("%s", __func__);

    /* Flush log buffer */
    LOG_PANIC();

    k_sleep(K_SECONDS(10));

    sys_reboot(SYS_REBOOT_COLD);
}

int main(void) {
    int err;
    int task_wdt_id;
    const uint32_t wdt_timeout_ms = (CONFIG_APP_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
    const uint32_t execution_time_ms = (CONFIG_APP_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
    const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
    static struct main_state main_state;

    main_state.sample_interval_sec = CONFIG_APP_SAMPLING_INTERVAL_SECONDS;
    main_state.first_sample_pending = true;

    LOG_DBG("Main has started");

    task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());
    if (task_wdt_id < 0) {
        LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
        SEND_FATAL_ERROR();

        return -EFAULT;
    }

    smf_set_initial(SMF_CTX(&main_state), &states[STATE_WAITING_FOR_MODULES_INIT]);

    while (1) {
        err = task_wdt_feed(task_wdt_id);
        if (err) {
            LOG_ERR("task_wdt_feed, error: %d", err);
            SEND_FATAL_ERROR();

            return err;
        }

        err = zbus_sub_wait_msg(&main_subscriber, &main_state.chan, main_state.msg_buf, zbus_wait_ms);
        if (err == -ENOMSG) {
            continue;
        } else if (err) {
            LOG_ERR("zbus_sub_wait_msg, error: %d", err);
            SEND_FATAL_ERROR();

            return err;
        }

        err = smf_run_state(SMF_CTX(&main_state));
        if (err) {
            LOG_ERR("smf_run_state(), error: %d", err);
            SEND_FATAL_ERROR();

            return err;
        }
    }
}
