/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Motion module — LIS2DTW12 temperature sensor over SPI.
 *
 * This is a standalone module with its own zbus channel and thread,
 * completely independent of the environmental module.  It receives
 * MOTION_SAMPLE_REQUEST messages and responds with MOTION_TEMPERATURE_DATA.
 */

#include <string.h>

#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "module_state.h"
#include "motion.h"

LOG_MODULE_REGISTER(motion, CONFIG_APP_MOTION_LOG_LEVEL);

/* ── Channel definition ──────────────────────────────────────────── */

ZBUS_CHAN_DEFINE(motion_chan, struct motion_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(motion);

/* Observe own channel to receive requests */
ZBUS_CHAN_ADD_OBS(motion_chan, motion, 0);

/* Also listen for location triggers to sample temperature alongside location */
#if defined(CONFIG_LOCATION)
#include "location.h"
ZBUS_CHAN_ADD_OBS(location_chan, motion, 0);
#endif

/* Use the STMems standard driver for register access */
#include <lis2dtw12_reg.h>

#define LIS2DTW12_SPI_READ (1 << 7)

/* Forward declarations of our SPI helpers (defined below) */
static int lis2dtw12_spi_read(uint8_t reg, uint8_t *data, uint16_t len);
static int lis2dtw12_spi_write(uint8_t reg, uint8_t value);

/* ── STMems driver context ─────────────────────────────────────────
 * Wraps our SPI read/write into the callback format expected by the
 * ST driver library so we can call lis2dtw12_*() API functions.
 */

static int stmemsc_write(void *handle, uint8_t reg, const uint8_t *buf, uint16_t len) {
    ARG_UNUSED(handle);
    for (uint16_t i = 0; i < len; i++) {
        /* For multi-byte writes the ST driver increments the reg internally */
        int err = lis2dtw12_spi_write(reg + i, buf[i]);
        if (err) return err;
    }
    return 0;
}

static int stmemsc_read(void *handle, uint8_t reg, uint8_t *buf, uint16_t len) {
    ARG_UNUSED(handle);
    return lis2dtw12_spi_read(reg, buf, len);
}

static stmdev_ctx_t st_ctx = {
    .write_reg = stmemsc_write,
    .read_reg = stmemsc_read,
    .mdelay = NULL,
};

/* ── Static state ────────────────────────────────────────────────── */

static struct spi_dt_spec lis2dtw12_spi = {
    .bus = DEVICE_DT_GET(DT_NODELABEL(spi2)),
    .config =
        {
            .frequency = 1000000,
            .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPOL | SPI_MODE_CPHA,
            .slave = 0,
            .cs = SPI_CS_CONTROL_INIT(DT_NODELABEL(lis2dtw12)),
        },
};

/* ── Low-level SPI helpers ───────────────────────────────────────── */

static int lis2dtw12_spi_read(uint8_t reg, uint8_t *data, uint16_t len) {
    /*
     * Full-duplex SPI: TX and RX must have the same total length.
     * TX = 1 address byte + len dummy bytes to clock out the data.
     * RX = 1 dummy byte (discard) + len data bytes.
     */
    uint8_t tx_buf[1 + 4]; /* enough for up to 4-byte reads */
    tx_buf[0] = reg | LIS2DTW12_SPI_READ;
    (void)memset(tx_buf + 1, 0, len);

    const struct spi_buf tx_bufs = {.buf = tx_buf, .len = 1 + len};
    const struct spi_buf_set tx = {.buffers = &tx_bufs, .count = 1};
    const struct spi_buf rx_buf[2] = {
        {.buf = NULL, .len = 1},
        {.buf = data, .len = len},
    };
    const struct spi_buf_set rx = {.buffers = rx_buf, .count = 2};

    if (spi_transceive(lis2dtw12_spi.bus, &lis2dtw12_spi.config, &tx, &rx)) {
        return -EIO;
    }
    return 0;
}

/**
 * @brief Write a single byte to a register via SPI.
 */
static int lis2dtw12_spi_write(uint8_t reg, uint8_t value) {
    uint8_t tx_buf[2] = {reg & ~LIS2DTW12_SPI_READ, value};
    const struct spi_buf tx_bufs = {.buf = tx_buf, .len = 2};
    const struct spi_buf_set tx = {.buffers = &tx_bufs, .count = 1};
    /* RX dummy — full-duplex: discard whatever comes back */
    uint8_t rx_buf[2];
    const struct spi_buf rx_bufs = {.buf = rx_buf, .len = 2};
    const struct spi_buf_set rx = {.buffers = &rx_bufs, .count = 1};

    if (spi_transceive(lis2dtw12_spi.bus, &lis2dtw12_spi.config, &tx, &rx)) {
        return -EIO;
    }
    return 0;
}

/**
 * @brief Run accelerometer self-test to verify the sensor is functional.
 */
static int run_self_test(void) {
    int err;

    /* Enable positive self-test via ST driver API */
    err = lis2dtw12_self_test_set(&st_ctx, 1);
    if (err) {
        LOG_ERR("Self-test enable failed: %d", err);
        return err;
    }

    /* Wait for self-test to settle (guaranteed > 1 ODR cycle @ 25 Hz) */
    k_sleep(K_MSEC(100));

    /* Verify self-test is active by reading back */
    uint8_t st_val;
    err = lis2dtw12_self_test_get(&st_ctx, &st_val);
    if (err) {
        LOG_ERR("Self-test readback failed: %d", err);
        return err;
    }

    if (st_val != 1) {
        LOG_ERR("Self-test did not activate (got %d)", st_val);
        return -EIO;
    }

    /* Disable self-test */
    err = lis2dtw12_self_test_set(&st_ctx, 0);
    if (err) {
        LOG_ERR("Self-test disable failed: %d", err);
        return err;
    }

    LOG_INF("Self-test passed");
    return 0;
}

/**
 * @brief Initialise sensor configuration.
 *
 * Uses the ST driver API exclusively — no raw register writes.
 * Enables the accelerometer (required for temperature to work),
 * sets BDU, and configures the wake-up interrupt on movement.
 */
static int configure_sensor(void) {
    int err;

    /*
     * Power mode and ODR are set together via the ST API.
     * Accelerometer must be in active mode for the temperature sensor
     * to produce valid samples.
     */
    err = lis2dtw12_power_mode_set(&st_ctx, LIS2DTW12_CONT_LOW_PWR_12bit);
    if (err) {
        LOG_ERR("Power mode set failed: %d", err);
        return err;
    }

    err = lis2dtw12_data_rate_set(&st_ctx, LIS2DTW12_XL_ODR_25Hz);
    if (err) {
        LOG_ERR("ODR set failed: %d", err);
        return err;
    }

    /* BDU — block data update prevents tearing on multi-byte reads */
    err = lis2dtw12_block_data_update_set(&st_ctx, 1);
    if (err) {
        LOG_ERR("BDU set failed: %d", err);
        return err;
    }

    /* ±2g full scale */
    err = lis2dtw12_full_scale_set(&st_ctx, LIS2DTW12_2g);
    if (err) {
        LOG_ERR("Full scale set failed: %d", err);
        return err;
    }

    /* Route wake-up interrupt to INT1 pin */
    lis2dtw12_ctrl4_int1_pad_ctrl_t int1_route = {.int1_wu = 1};
    err = lis2dtw12_pin_int1_route_set(&st_ctx, &int1_route);
    if (err) {
        LOG_ERR("INT1 route set failed: %d", err);
        return err;
    }

    /* Enable low-noise and HP filter path (CTRL6) */
    lis2dtw12_ctrl6_t ctrl6 = {
        .low_noise = 1,
        .fds = 1,
        .fs = LIS2DTW12_2g,
    };
    err = lis2dtw12_write_reg(&st_ctx, LIS2DTW12_CTRL6, (uint8_t *)&ctrl6, 1);
    if (err) {
        LOG_ERR("CTRL6 write failed: %d", err);
        return err;
    }

    /* Wake-up threshold: ~94 mg at ±2g (3 × 31.25 mg/LSB) */
    err = lis2dtw12_wkup_threshold_set(&st_ctx, 0x03);
    if (err) {
        LOG_ERR("WKUP threshold set failed: %d", err);
        return err;
    }

    /* Wake-up duration: 2 consecutive ODR cycles */
    err = lis2dtw12_wkup_dur_set(&st_ctx, 0x02);
    if (err) {
        LOG_ERR("WKUP duration set failed: %d", err);
        return err;
    }

    LOG_DBG("Sensor configured: ODR=25Hz LP, BDU=1, wake-up @ ~94mg");
    return 0;
}

/* ── Sampling ────────────────────────────────────────────────────── */

static double read_temperature(void) {
    int16_t raw;
    int err;

    err = lis2dtw12_temperature_raw_get(&st_ctx, &raw);
    if (err) {
        LOG_ERR("LIS2DTW12 temperature read failed: %d", err);
        return -1.0;
    }

    /*
     * ST driver returns raw value as 16-bit two's complement.
     * For left-justified 12-bit data: shift right by 4 to get
     * a right-justified 12-bit value, sign-extend from 12 bits,
     * then apply sensitivity: 0.0625 C/LSB, offset 25 C.
     */
    int16_t raw_12bit = raw >> 4;
    return ((double)raw_12bit / 16.0) + 25.0;
}

static void verify_sensor(void) {
    uint8_t chip_id;
    int err;

    /* LIS2DTW12 boots in I2C mode; dummy read triggers SPI mode switch */
    {
        uint8_t dummy;
        (void)lis2dtw12_spi_read(0x00, &dummy, 1);
    }

    err = lis2dtw12_device_id_get(&st_ctx, &chip_id);
    if (err) {
        LOG_ERR("LIS2DTW12 SPI communication failed: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    if (chip_id != LIS2DTW12_ID) {
        LOG_ERR("LIS2DTW12 ID mismatch: expected 0x%02X, got 0x%02X", LIS2DTW12_ID, chip_id);
        SEND_FATAL_ERROR();
        return;
    }

    LOG_INF("LIS2DTW12 verified (ID: 0x%02X)", chip_id);
}

static void sample_temperature(void) {
    double temperature = read_temperature();
    if (temperature < -40.0) {
        LOG_ERR("LIS2DTW12 temperature out of range: %.2f", temperature);
        SEND_FATAL_ERROR();
        return;
    }

    struct motion_msg msg = {
        .type = MOTION_TEMPERATURE_DATA,
        .temperature = temperature,
        .timestamp = k_uptime_get(),
    };

    int err = zbus_chan_pub(&motion_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("zbus_chan_pub, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

/* ── SMF state machine ──────────────────────────────────────────── */

static const char *mot_state_str;

enum motion_module_state {
    STATE_RUNNING,
};

#if defined(CONFIG_LOCATION)
#define MAX_MSG_SIZE MAX(sizeof(struct motion_msg), sizeof(struct location_msg))
#else
#define MAX_MSG_SIZE sizeof(struct motion_msg)
#endif

BUILD_ASSERT(CONFIG_APP_MOTION_WATCHDOG_TIMEOUT_SECONDS > CONFIG_APP_MOTION_MSG_PROCESSING_TIMEOUT_SECONDS,
             "Watchdog timeout must be greater than maximum message processing time");

struct motion_state_object {
    struct smf_ctx ctx;
    const struct zbus_channel *chan;
    uint8_t msg_buf[MAX_MSG_SIZE];
};

static enum smf_state_result state_running_run(void *obj);

static const struct smf_state states[] = {
    [STATE_RUNNING] = SMF_CREATE_STATE(NULL, state_running_run, NULL, NULL, NULL),
};

TASK_WDT_CALLBACK_DEFINE(mot)

static enum smf_state_result state_running_run(void *obj) {
    mot_state_str = "run";
    struct motion_state_object *state_obj = obj;

    if (&motion_chan == state_obj->chan) {
        const struct motion_msg *msg = (const struct motion_msg *)state_obj->msg_buf;
        if (msg->type == MOTION_SAMPLE_REQUEST) {
            sample_temperature();
            return SMF_EVENT_HANDLED;
        }
    }
#if defined(CONFIG_LOCATION)
    if (&location_chan == state_obj->chan) {
        const struct location_msg *msg = (const struct location_msg *)state_obj->msg_buf;
        if (msg->type == LOCATION_CELLULAR_SEARCH_TRIGGER || msg->type == LOCATION_GNSS_SEARCH_TRIGGER ||
            msg->type == LOCATION_SEARCH_TRIGGER) {
            sample_temperature();
            return SMF_EVENT_HANDLED;
        }
    }
#endif
    return SMF_EVENT_PROPAGATE;
}

/* ── Thread ──────────────────────────────────────────────────────── */

static void motion_module_thread(void) {
    int err;
    TASK_WDT_TIMEOUTS(APP_MOTION);
    TASK_WDT_ZBUS_TIMEOUT;
    static struct motion_state_object motion_state;

    LOG_DBG("Motion module task started");

    if (!spi_is_ready_dt(&lis2dtw12_spi)) {
        LOG_ERR("LIS2DTW12 SPI bus not ready");
        SEND_FATAL_ERROR();
        return;
    }

    verify_sensor();

    if (configure_sensor() != 0) {
        SEND_FATAL_ERROR();
        return;
    }

    if (run_self_test() != 0) {
        LOG_WRN("Self-test failed — sensor may be unreliable");
    }

    /* Read and display temperature once at boot */
    {
        double boot_temp = read_temperature();
        if (boot_temp > -100.0) {
            LOG_INF("LIS2DTW12 temperature at boot: %.2f C", boot_temp);
        } else {
            LOG_ERR("LIS2DTW12 boot temperature read failed");
        }
    }

    TASK_WDT_ADD(mot, wdt_timeout_ms)

    smf_set_initial(SMF_CTX(&motion_state), &states[STATE_RUNNING]);

    while (true) {
        TASK_WDT_FEED();

        err = zbus_sub_wait_msg(&motion, &motion_state.chan, motion_state.msg_buf, zbus_wait_ms);
        if (err == -ENOMSG) {
            continue;
        } else if (err) {
            LOG_ERR("zbus_sub_wait_msg, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
        err = smf_run_state(SMF_CTX(&motion_state));
        if (err) {
            LOG_ERR("smf_run_state(), error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
    }
}

K_THREAD_DEFINE(motion_module_thread_id, CONFIG_APP_MOTION_THREAD_STACK_SIZE, motion_module_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

const char *motion_state_str(void) { return mot_state_str ? mot_state_str : "?"; }
