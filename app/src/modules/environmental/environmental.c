/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "environmental.h"

/* Register log module */
LOG_MODULE_REGISTER(environmental, CONFIG_APP_ENVIRONMENTAL_LOG_LEVEL);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(environmental_chan, struct environmental_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(environmental);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(environmental_chan, environmental, 0);

#define MAX_MSG_SIZE sizeof(struct environmental_msg)

BUILD_ASSERT(CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS >
                 CONFIG_APP_ENVIRONMENTAL_MSG_PROCESSING_TIMEOUT_SECONDS,
             "Watchdog timeout must be greater than maximum message processing time");

/* State machine */

/* Environmental module states.
 */
enum environmental_module_state {
    /* The module is running and waiting for sensor value requests */
    STATE_RUNNING,
};

//  Used to transfer context data between state changes.
struct environmental_state_object {
    struct smf_ctx ctx;              // must be first
    const struct zbus_channel *chan; // channel type that a message was received on
    uint8_t msg_buf[MAX_MSG_SIZE];
#if !defined(CONFIG_APP_ENVIRONMENTAL_LIS2DTW12)
    const struct device *const bme680;
#else
    struct spi_dt_spec lis2dtw12_spi;
#endif
};

/* Forward declarations of state handlers */
static enum smf_state_result state_running_run(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
    [STATE_RUNNING] = SMF_CREATE_STATE(NULL, state_running_run, NULL, NULL, NULL),
};

#if defined(CONFIG_APP_ENVIRONMENTAL_LIS2DTW12)
static double read_lis2dtw12_temperature(const struct spi_dt_spec *spi) {
    uint8_t tx_buf[3] = {0x0D | 0x80, 0x00, 0x00};
    uint8_t rx_buf[3] = {0};
    int err;

    const struct spi_buf tx_bufs = {.buf = tx_buf, .len = 3};
    const struct spi_buf rx_bufs = {.buf = rx_buf, .len = 3};
    const struct spi_buf_set tx = {.buffers = &tx_bufs, .count = 1};
    const struct spi_buf_set rx = {.buffers = &rx_bufs, .count = 1};

    err = spi_transceive(spi->bus, &spi->config, &tx, &rx);
    if (err) {
        LOG_ERR("LIS2DTW12 SPI read failed: %d", err);
        return -1.0;
    }

    /* rx_buf[0] dummy, rx_buf[1] = OUT_T_L, rx_buf[2] = OUT_T_H */
    int16_t raw = (int16_t)((rx_buf[2] << 8) | rx_buf[1]);

    /* Conversion: temp_C = (raw / 256.0) + 25.0 */
    return ((double)raw / 256.0) + 25.0;
}
#endif

static void sample_sensors(struct environmental_state_object *state) {
    int err;
#if !defined(CONFIG_APP_ENVIRONMENTAL_LIS2DTW12)
    struct sensor_value temp = {0};
#endif

#if defined(CONFIG_APP_ENVIRONMENTAL_LIS2DTW12)
    double temperature = read_lis2dtw12_temperature(&state->lis2dtw12_spi);
    if (temperature < -40.0) {
        SEND_FATAL_ERROR();
        return;
    }
#else
    err = sensor_sample_fetch(state->bme680);
    if (err) {
        LOG_ERR("sensor_sample_fetch, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    err = sensor_channel_get(state->bme680, SENSOR_CHAN_AMBIENT_TEMP, &temp);
    if (err) {
        LOG_ERR("sensor_channel_get, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }
#endif

    struct environmental_msg msg = {
        .type = ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE,
#if defined(CONFIG_APP_ENVIRONMENTAL_LIS2DTW12)
        .temperature = temperature,
#else
        .temperature = sensor_value_to_double(&temp),
#endif
        .timestamp = k_uptime_get(),
    };

    err = zbus_chan_pub(&environmental_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("zbus_chan_pub, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }
}

static void env_wdt_callback(int channel_id, void *user_data) {
    LOG_ERR("Watchdog expired, Channel: %d, Thread: %s", channel_id, k_thread_name_get((k_tid_t)user_data));
    SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* State handlers */

static enum smf_state_result state_running_run(void *obj) {
    struct environmental_state_object *state_object = obj;
    if (&environmental_chan == state_object->chan) {
        const struct environmental_msg *msg = (const struct environmental_msg *)state_object->msg_buf;
        if (msg->type == ENVIRONMENTAL_SENSOR_SAMPLE_REQUEST) {
            sample_sensors(state_object);
            return SMF_EVENT_HANDLED;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

static void env_module_thread(void) {
    int err;
    int task_wdt_id;
    const uint32_t wdt_timeout_ms = (CONFIG_APP_ENVIRONMENTAL_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
    const uint32_t execution_time_ms = (CONFIG_APP_ENVIRONMENTAL_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
    const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
    static struct environmental_state_object environmental_state = {
#if defined(CONFIG_APP_ENVIRONMENTAL_LIS2DTW12)
        .lis2dtw12_spi =
            {
                .bus = DEVICE_DT_GET(DT_NODELABEL(spi2)),
                .config =
                    {
                        .frequency = 1000000,
                        .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPOL | SPI_MODE_CPHA,
                        .slave = 0,
                    },
            },
#else
        .bme680 = DEVICE_DT_GET(DT_NODELABEL(bme680)),
#endif
    };

#if defined(CONFIG_APP_ENVIRONMENTAL_LIS2DTW12)
    environmental_state.lis2dtw12_spi.config.cs = (struct spi_cs_control){
        .gpio = GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(spi2), cs_gpios, 0),
        .delay = 0,
    };
#endif

    LOG_DBG("Environmental module task started");

#if defined(CONFIG_APP_ENVIRONMENTAL_LIS2DTW12)
    if (!spi_is_ready_dt(&environmental_state.lis2dtw12_spi)) {
        LOG_ERR("LIS2DTW12 SPI bus or CS not ready");
        SEND_FATAL_ERROR();
        return;
    }
#endif

    task_wdt_id = task_wdt_add(wdt_timeout_ms, env_wdt_callback, (void *)k_current_get());
    if (task_wdt_id < 0) {
        LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
        SEND_FATAL_ERROR();
        return;
    }

    smf_set_initial(SMF_CTX(&environmental_state), &states[STATE_RUNNING]);

    while (true) {
        err = task_wdt_feed(task_wdt_id);
        if (err) {
            LOG_ERR("task_wdt_feed, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }

        err = zbus_sub_wait_msg(&environmental, &environmental_state.chan, environmental_state.msg_buf, zbus_wait_ms);
        if (err == -ENOMSG) {
            continue;
        } else if (err) {
            LOG_ERR("zbus_sub_wait_msg, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
        err = smf_run_state(SMF_CTX(&environmental_state));
        if (err) {
            LOG_ERR("smf_run_state(), error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
    }
}

K_THREAD_DEFINE(environmental_module_thread_id, CONFIG_APP_ENVIRONMENTAL_THREAD_STACK_SIZE, env_module_thread, NULL,
                NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
