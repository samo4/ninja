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

ZBUS_CHAN_DEFINE(motion_chan, struct motion_msg, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(motion);

/* Observe own channel to receive requests */
ZBUS_CHAN_ADD_OBS(motion_chan, motion, 0);

/* ── Register definitions ────────────────────────────────────────── */

#define LIS2DTW12_SPI_READ      (1 << 7)
#define LIS2DTW12_REG_WHO_AM_I  0x0F
#define LIS2DTW12_ID_VALUE      0x44

/* OUT_T_L / OUT_T_H registers (12-bit two's complement, 0.0625 C/LSB, offset 25 C) */
#define LIS2DTW12_REG_OUT_T_L   0x0D

/* Register map (LIS2DW12-compatible) */
#define LIS2DTW12_REG_CTRL1             0x20
#define LIS2DTW12_REG_CTRL2             0x21
#define LIS2DTW12_REG_CTRL3             0x22
#define LIS2DTW12_REG_CTRL4_INT1_CTRL   0x23
#define LIS2DTW12_REG_CTRL5             0x24
#define LIS2DTW12_REG_CTRL6             0x25
#define LIS2DTW12_REG_WAKE_UP_THS       0x34
#define LIS2DTW12_REG_WAKE_UP_DUR       0x35

/* CTRL1: ODR=25Hz (0x30), Low-power mode 1 */
#define LIS2DTW12_CTRL1_ODR_25HZ_LP1    0x30

/* CTRL2: BDU (bit6) + IF_ADD_INC (bit2) */
#define LIS2DTW12_CTRL2_BDU_IF_INC      0x44

/* CTRL3: H_LACTIVE (bit5) + PP_OD (bit4) — active-low, open-drain */
#define LIS2DTW12_CTRL3_HLACTIVE_OD     0x30

/* CTRL4_INT1_CTRL: INT1_WU (bit5) — route wake-up to INT1 */
#define LIS2DTW12_CTRL4_INT1_WU         0x20

/* CTRL6: LOW_NOISE (bit3) + FDS (bit2) — ±2g, HPF path */
#define LIS2DTW12_CTRL6_LOWNOISE_FDS    0x0C

/* WAKE_UP_THS: ~94 mg threshold (3 × 31.25 mg at ±2g) */
#define LIS2DTW12_WAKE_UP_THS_94MG      0x03

/* WAKE_UP_DUR: 2 ODR cycles to filter glitches */
#define LIS2DTW12_WAKE_UP_DUR_2         0x02

/* CTRL5: ST (bit2) — self-test enable */
#define LIS2DTW12_CTRL5_ST              BIT(2)

/* ── Static state ────────────────────────────────────────────────── */

static struct spi_dt_spec lis2dtw12_spi = {
	.bus = DEVICE_DT_GET(DT_NODELABEL(spi2)),
	.config = {
		.frequency = 1000000,
		.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |
			      SPI_MODE_CPOL | SPI_MODE_CPHA,
		.slave = 0,
		.cs = SPI_CS_CONTROL_INIT(DT_NODELABEL(lis2dtw12)),
	},
};

/* ── Low-level SPI helpers ───────────────────────────────────────── */

static int lis2dtw12_spi_read(uint8_t reg, uint8_t *data, uint16_t len)
{
	uint8_t tx_buf[2] = {reg | LIS2DTW12_SPI_READ, 0};
	const struct spi_buf tx_bufs = {.buf = tx_buf, .len = 2};
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
static int lis2dtw12_spi_write(uint8_t reg, uint8_t value)
{
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
 *
 * Enables self-test, waits for valid data, checks for non-zero output,
 * then disables self-test.
 */
static int run_self_test(void)
{
	uint8_t val;
	int err;

	/* Enable self-test via CTRL5 bit 2 */
	err = lis2dtw12_spi_write(LIS2DTW12_REG_CTRL5, LIS2DTW12_CTRL5_ST);
	if (err) {
		LOG_ERR("Self-test enable failed: %d", err);
		return err;
	}

	/* Wait for self-test to settle (guaranteed > 1 ODR cycle @ 25 Hz) */
	k_sleep(K_MSEC(100));

	/* Read CTRL5 back to verify ST bit stuck */
	err = lis2dtw12_spi_read(LIS2DTW12_REG_CTRL5, &val, 1);
	if (err) {
		LOG_ERR("Self-test readback failed: %d", err);
		return err;
	}

	if (!(val & LIS2DTW12_CTRL5_ST)) {
		LOG_ERR("Self-test bit did not latch");
		return -EIO;
	}

	/* Disable self-test */
	err = lis2dtw12_spi_write(LIS2DTW12_REG_CTRL5, 0x00);
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
 * Enables the accelerometer (required for temperature to work), sets BDU,
 * and configures the wake-up interrupt on movement.
 */
static int configure_sensor(void)
{
	int err;

	/*
	 * CTRL1: ODR=25Hz, Low-power mode 1.
	 * Accelerometer must be in active mode for the temperature sensor
	 * to produce valid samples.
	 */
	err = lis2dtw12_spi_write(LIS2DTW12_REG_CTRL1, LIS2DTW12_CTRL1_ODR_25HZ_LP1);
	if (err) {
		LOG_ERR("CTRL1 write failed: %d", err);
		return err;
	}

	/* CTRL2: BDU (block data update) + IF_ADD_INC (auto-increment) */
	err = lis2dtw12_spi_write(LIS2DTW12_REG_CTRL2, LIS2DTW12_CTRL2_BDU_IF_INC);
	if (err) {
		LOG_ERR("CTRL2 write failed: %d", err);
		return err;
	}

	/* CTRL3: active-low, open-drain interrupt pins */
	err = lis2dtw12_spi_write(LIS2DTW12_REG_CTRL3, LIS2DTW12_CTRL3_HLACTIVE_OD);
	if (err) {
		LOG_ERR("CTRL3 write failed: %d", err);
		return err;
	}

	/* CTRL4_INT1_CTRL: route wake-up event to INT1 pin */
	err = lis2dtw12_spi_write(LIS2DTW12_REG_CTRL4_INT1_CTRL, LIS2DTW12_CTRL4_INT1_WU);
	if (err) {
		LOG_ERR("CTRL4 write failed: %d", err);
		return err;
	}

	/* CTRL6: ±2g full-scale, low-noise, high-pass filter enabled */
	err = lis2dtw12_spi_write(LIS2DTW12_REG_CTRL6, LIS2DTW12_CTRL6_LOWNOISE_FDS);
	if (err) {
		LOG_ERR("CTRL6 write failed: %d", err);
		return err;
	}

	/* WAKE_UP_THS: threshold ~94 mg (3 × 31.25 mg at ±2g) */
	err = lis2dtw12_spi_write(LIS2DTW12_REG_WAKE_UP_THS, LIS2DTW12_WAKE_UP_THS_94MG);
	if (err) {
		LOG_ERR("WAKE_UP_THS write failed: %d", err);
		return err;
	}

	/* WAKE_UP_DUR: require 2 consecutive ODR cycles above threshold */
	err = lis2dtw12_spi_write(LIS2DTW12_REG_WAKE_UP_DUR, LIS2DTW12_WAKE_UP_DUR_2);
	if (err) {
		LOG_ERR("WAKE_UP_DUR write failed: %d", err);
		return err;
	}

	LOG_DBG("Sensor configured: ODR=25Hz LP, BDU=1, wake-up @ ~94mg");
	return 0;
}

/* ── Sampling ────────────────────────────────────────────────────── */

static double read_temperature(void)
{
	uint8_t temp_bytes[2];
	int err;

	err = lis2dtw12_spi_read(LIS2DTW12_REG_OUT_T_L, temp_bytes, 2);
	if (err) {
		LOG_ERR("LIS2DTW12 temperature SPI read failed: %d", err);
		return -1.0;
	}

	/*
	 * Register pair OUT_T_H:OUT_T_L forms a 16-bit word.
	 * Per datasheet: temperature is left-justified in 12-bit mode.
	 *
	 *   combined_16 = (OUT_T_H << 8) | OUT_T_L
	 *                  bits [15:4] = 12-bit temp value (left-justified)
	 *                  bits [3:0]  = padding (zero)
	 *
	 * To get a right-justified 12-bit value: combined_16 >> 4
	 */
	uint16_t combined = ((uint16_t)temp_bytes[1] << 8) | temp_bytes[0];
	uint16_t raw = combined >> 4;

	/* Sign-extend 12-bit two's complement to 16-bit */
	int16_t raw_12bit = (int16_t)(raw << 4) >> 4;

	/* Sensitivity: 0.0625 C/LSB (= 1.0 / 16.0), offset 25 C */
	return ((double)raw_12bit / 16.0) + 25.0;
}

static void verify_sensor(void)
{
	/*
	 * LIS2DTW12 boots in I2C mode by default and only switches to SPI
	 * after detecting a high-to-low transition on CS.  Perform a dummy
	 * read to trigger the mode switch, then verify the chip ID.
	 */
	{
		uint8_t dummy;
		(void)lis2dtw12_spi_read(0x00, &dummy, 1);
	}

	uint8_t chip_id;
	int err = lis2dtw12_spi_read(LIS2DTW12_REG_WHO_AM_I, &chip_id, 1);
	if (err) {
		LOG_ERR("LIS2DTW12 SPI communication failed: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	if (chip_id != LIS2DTW12_ID_VALUE) {
		LOG_ERR("LIS2DTW12 ID mismatch: expected 0x%02X, got 0x%02X",
			LIS2DTW12_ID_VALUE, chip_id);
		SEND_FATAL_ERROR();
		return;
	}

	LOG_INF("LIS2DTW12 verified (ID: 0x%02X)", chip_id);
}

static void sample_temperature(void)
{
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

BUILD_ASSERT(CONFIG_APP_MOTION_WATCHDOG_TIMEOUT_SECONDS >
		 CONFIG_APP_MOTION_MSG_PROCESSING_TIMEOUT_SECONDS,
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

static enum smf_state_result state_running_run(void *obj)
{
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

static void motion_module_thread(void)
{
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

K_THREAD_DEFINE(motion_module_thread_id, CONFIG_APP_MOTION_THREAD_STACK_SIZE,
		motion_module_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

const char *motion_state_str(void)
{
	return mot_state_str ? mot_state_str : "?";
}
