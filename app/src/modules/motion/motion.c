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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "module_state.h"
#include "motion.h"

#if defined(CONFIG_APP_LED)
#include <zephyr/drivers/gpio.h>
#include "led.h"
#endif

LOG_MODULE_REGISTER(motion, CONFIG_APP_MOTION_LOG_LEVEL);

/* ── Channel definition ──────────────────────────────────────────── */

ZBUS_CHAN_DEFINE(motion_chan, struct motion_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(motion);

/* Observe own channel to receive requests */
ZBUS_CHAN_ADD_OBS(motion_chan, motion, 0);

/* Use the STMems standard driver for register access */
#include <lis2dtw12_reg.h>

/* Access the Zephyr lis2dw12 driver's built-in stmdev_ctx_t so we can
 * call lis2dtw12_*() register functions through the same SPI path.
 */
#include <lis2dw12.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"

// Wake-up threshold register (R/W)
// WK_THS[5:0]:
static const uint8_t WKUP_THR_62mg_AT_2g = 0x02;
static const uint8_t WKUP_THR_94mg_AT_2g = 0x03;
static const uint8_t WKUP_THR_125mg_AT_2g = 0x04;
static const uint8_t WKUP_THR_156mg_AT_2g = 0x05;
static const uint8_t WKUP_THR_188mg_AT_2g = 0x06;
static const uint8_t WKUP_THR_219mg_AT_2g = 0x07;
/// .. up to WKUP_THR_2000mg_AT_2g = 0x3F
static const uint8_t WKUP_SLEEP_ON = 1 << 6; // sleep/inactivity Default: 0 (0: sleep disabled; 1: sleep enabled)
static const uint8_t WKUP_SINGLE_DOUBLE_TAP =
    1 << 7; // (0: only single-tap event is enabled; 1: single and double-tap events are enabled)

// Don't Forget the Duration: WAKE_UP_DUR register (Register 0x31, bits 5-6): number of samples that must exceed the
// threshold to trigger a wake-up event.  Default: 0 (1 sample).  Valid values: 0-3 (1-4 samples).

#pragma GCC diagnostic pop

// Pointer to the Zephyr driver's stmdev_ctx_t (embedded in its config).
static const stmdev_ctx_t *driver_ctx;

/* ── Movement detection (INT1 GPIO interrupt + LED blink) ───────── */

#if defined(CONFIG_APP_LED)
/* INT1 from LIS2DTW12 is P0.30, mapped via irq-gpios in the DTS */
static struct gpio_dt_spec motion_int_gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(lis2dtw12), irq_gpios);

static struct gpio_callback motion_int_cb_data;
static struct k_work motion_blink_work;

static void motion_blink_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct led_msg blink_msg = {
        .type = LED_RGB_SET,
        .red = 255,
        .green = 0,
        .duration_on_msec = 200,
        .duration_off_msec = 200,
        .repetitions = 5,
    };

    int err = zbus_chan_pub(&led_chan, &blink_msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish LED blink: %d", err);
    }
}

static void motion_int_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    LOG_INF("Motion detected on INT1 (P0.%d)", motion_int_gpio.pin);
    k_work_submit(&motion_blink_work);
}
#endif /* CONFIG_APP_LED */

/* ── Driver context init ────────────────────────────────────────── */

static int init_driver_ctx(void) {
    const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(lis2dtw12));
    if (!device_is_ready(dev)) {
        LOG_ERR("LIS2DTW12 device not ready");
        return -ENODEV;
    }
    const struct lis2dw12_device_config *cfg = dev->config;
    driver_ctx = &cfg->ctx;
    return 0;
}

/**
 * @brief Configure wake-up detection on the LIS2DTW12.
 *
 * Basic init (SPI mode switch, ODR, range, BDU, power mode,
 * low-noise, HP filter, wake-up duration) is handled by the
 * Zephyr lis2dw12 driver from DTS properties.  Here we only
 * set the wake-up-specific parts that are runtime-only:
 * INT1 routing and threshold.
 */
static int configure_wakeup(void) {
    int err;

    /* Route wake-up interrupt to INT1 pin */
    lis2dtw12_ctrl4_int1_pad_ctrl_t int1_route = {.int1_wu = 1};
    err = lis2dtw12_pin_int1_route_set(driver_ctx, &int1_route);
    if (err) {
        LOG_ERR("INT1 route set failed: %d", err);
        return err;
    }

    err = lis2dtw12_wkup_threshold_set(driver_ctx, WKUP_THR_188mg_AT_2g);
    if (err) {
        LOG_ERR("WKUP threshold set failed: %d", err);
        return err;
    }

    LOG_INF("Wake-up configured");
    return 0;
}

/**
 * @brief Run accelerometer self-test to verify the sensor is functional.
 */
static int run_self_test(void) {
    int err;

    err = lis2dtw12_self_test_set(driver_ctx, 1);
    if (err) {
        LOG_ERR("Self-test enable failed: %d", err);
        return err;
    }

    k_sleep(K_MSEC(100));

    uint8_t st_val;
    err = lis2dtw12_self_test_get(driver_ctx, &st_val);
    if (err) {
        LOG_ERR("Self-test readback failed: %d", err);
        return err;
    }

    if (st_val != 1) {
        LOG_ERR("Self-test did not activate (got %d)", st_val);
        return -EIO;
    }

    err = lis2dtw12_self_test_set(driver_ctx, 0);
    if (err) {
        LOG_ERR("Self-test disable failed: %d", err);
        return err;
    }

    LOG_INF("Self-test passed");
    return 0;
}

/* ── Sampling ────────────────────────────────────────────────────── */

static double read_temperature(void) {
    int16_t raw;
    int err;

    err = lis2dtw12_temperature_raw_get(driver_ctx, &raw);
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

#define MAX_MSG_SIZE sizeof(struct motion_msg)

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
    return SMF_EVENT_PROPAGATE;
}

/* ── Thread ──────────────────────────────────────────────────────── */

static void motion_module_thread(void) {
    int err;
    TASK_WDT_TIMEOUTS(APP_MOTION);
    TASK_WDT_ZBUS_TIMEOUT;
    static struct motion_state_object motion_state;

    LOG_DBG("Motion module task started");

    /* Grab a pointer to the Zephyr driver's STMems context for our register access */
    if (init_driver_ctx() != 0) {
        SEND_FATAL_ERROR();
        return;
    }

    /*
     * Basic init (SPI mode switch, ODR, range, BDU, power mode,
     * low-noise, HP filter, wake-up duration) is handled by the
     * Zephyr lis2dw12 driver from DTS properties.  We only set
     * the wake-up INT1 routing and threshold.
     */
    if (configure_wakeup() != 0) {
        SEND_FATAL_ERROR();
        return;
    }

    if (run_self_test() != 0) {
        LOG_WRN("Self-test failed — sensor may be unreliable");
    }

    double boot_temp = read_temperature();
    LOG_INF("LIS2DTW12 temperature at boot: %.2f C", boot_temp);

#if defined(CONFIG_APP_LED)
    /* Set up INT1 GPIO interrupt for movement detection */
    if (!gpio_is_ready_dt(&motion_int_gpio)) {
        LOG_ERR("INT1 GPIO device not ready");
        SEND_FATAL_ERROR();
        return;
    }

    err = gpio_pin_configure_dt(&motion_int_gpio, GPIO_INPUT);
    if (err) {
        LOG_ERR("Failed to configure INT1 GPIO: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    err = gpio_pin_interrupt_configure_dt(&motion_int_gpio, GPIO_INT_EDGE_RISING);
    if (err) {
        LOG_ERR("Failed to configure INT1 interrupt: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    gpio_init_callback(&motion_int_cb_data, motion_int_handler, BIT(motion_int_gpio.pin));
    gpio_add_callback(motion_int_gpio.port, &motion_int_cb_data);

    k_work_init(&motion_blink_work, motion_blink_work_handler);

    LOG_INF("Movement interrupt enabled on INT1 (P0.%d)", motion_int_gpio.pin);
#endif /* CONFIG_APP_LED */

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
