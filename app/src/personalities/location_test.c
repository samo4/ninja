/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Location Test personality:
 *   Alternates between cellular-only and GNSS-only location requests.
 *   Cellular → GNSS → Cellular → GNSS → ...
 *   Each result is posted to cloud → modem off → sleep → repeat.
 *   Reuses cloud_post for HTTP communication — location data is forwarded
 *   via the location_chan that cloud_post now also observes.
 *
 *   State machine:
 *     CONNECTING       — re-establish LTE connection after sleep (modem was offline)
 *     SAMPLING         — fire LOCATION_CELLULAR_SEARCH_TRIGGER or LOCATION_GNSS_SEARCH_TRIGGER
 *     WAITING_LOCATION — wait for LOCATION_DATA or timer expiry
 *     WAITING_CLOUD    — cloud_post handles LTE connect + POST, personality
 *                        waits for SEND_DONE / SEND_FAILED
 *     DISCONNECTING    — waiting for NETWORK_DISCONNECTED
 *     SLEEPING         — modem-off power-measurement window
 *     → CONNECTING → SAMPLING → ...
 */

#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/sys/reboot.h>

#include "app_common.h"
#include "cloud_post.h"
#if defined(CONFIG_APP_LED)
#include "led.h"
#endif
#include "location.h"
#include "location_test.h"
#include "motion.h"
#include "network.h"

LOG_MODULE_REGISTER(location_test, CONFIG_APP_LOG_LEVEL);

/* Fallback timeout if cloud_post never publishes SEND_DONE/SEND_FAILED. */
#define CLOUD_POST_FALLBACK_TIMEOUT_SECONDS 600

/* Define the timer channel */
ZBUS_CHAN_DEFINE(timer_chan, struct location_test_timer_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* ── Timer work ─────────────────────────────────────────────────── */

static void timer_expired_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(sample_timer_work, timer_expired_fn);

static void timer_expired_fn(struct k_work *work) {
    ARG_UNUSED(work);
    const struct location_test_timer_msg msg = {.type = LOCATION_TEST_TIMER_EXPIRED};
    int err = zbus_chan_pub(&timer_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish timer expired, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

static void timer_arm(uint32_t delay_sec) {
    int err = k_work_reschedule(&sample_timer_work, K_SECONDS(delay_sec));
    if (err < 0) {
        LOG_ERR("k_work_reschedule sample_timer_work, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

/* ── Personality state string (for main heartbeat) ─────────────── */

static const char *lt_state_name;

const char *personality_state_str(void) { return lt_state_name ? lt_state_name : "?"; }

/* ── Helpers ────────────────────────────────────────────────────── */

static void fire_location_search(enum location_msg_type type) {
    const struct location_msg req = {
        .type = type,
    };

    int err = zbus_chan_pub(&location_chan, &req, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("Failed to publish location trigger (%d), error: %d", type, err);
        SEND_FATAL_ERROR();
        return;
    }
    PUBLISH_MOTION(MOTION_SAMPLE_REQUEST);
}

static void request_disconnect(void) { PUBLISH_NETWORK(NETWORK_DISCONNECT); }

/* ── SMF states ─────────────────────────────────────────────────── */

static enum smf_state_result waiting_module_run(void *o);
static void sampling_entry(void *o);
static enum smf_state_result waiting_location_run(void *o);
static enum smf_state_result waiting_cloud_run(void *o);
static void disconnecting_entry(void *o);
static enum smf_state_result disconnecting_run(void *o);
static void sleeping_entry(void *o);
static enum smf_state_result sleeping_run(void *o);
static void connecting_entry(void *o);
static enum smf_state_result connecting_run(void *o);
static void rebooting_entry(void *o);

static const struct smf_state states[] = {
    [LOCATION_TEST_STATE_WAITING_MODULE] = SMF_CREATE_STATE(NULL, waiting_module_run, NULL, NULL, NULL),
    [LOCATION_TEST_STATE_SAMPLING] = SMF_CREATE_STATE(sampling_entry, NULL, NULL, NULL, NULL),
    [LOCATION_TEST_STATE_WAITING_LOCATION] = SMF_CREATE_STATE(NULL, waiting_location_run, NULL, NULL, NULL),
    [LOCATION_TEST_STATE_WAITING_CLOUD] = SMF_CREATE_STATE(NULL, waiting_cloud_run, NULL, NULL, NULL),
    [LOCATION_TEST_STATE_DISCONNECTING] = SMF_CREATE_STATE(disconnecting_entry, disconnecting_run, NULL, NULL, NULL),
    [LOCATION_TEST_STATE_SLEEPING] = SMF_CREATE_STATE(sleeping_entry, sleeping_run, NULL, NULL, NULL),
    [LOCATION_TEST_STATE_CONNECTING] = SMF_CREATE_STATE(connecting_entry, connecting_run, NULL, NULL, NULL),
    [LOCATION_TEST_STATE_REBOOTING] = SMF_CREATE_STATE(rebooting_entry, NULL, NULL, NULL, NULL),
};

static void sampling_entry(void *o) {
    struct location_test_state_object *state = (struct location_test_state_object *)o;
    lt_state_name = "sampling";
    LOG_INF("sampling (cycle every %us)", state->sample_interval_sec);
    state->location_received = false;
    state->is_gnss_search = !state->use_cellular_next;
    if (state->use_cellular_next) {
        LOG_INF("requesting cellular location");
        fire_location_search(LOCATION_CELLULAR_SEARCH_TRIGGER);
    } else {
        LOG_INF("requesting GNSS location");
        fire_location_search(LOCATION_GNSS_SEARCH_TRIGGER);
    }
    state->use_cellular_next = !state->use_cellular_next;

    /* Arm a fallback timer in case location never comes back (2 min default + margin). */
    timer_arm(150);
    lt_state_name = "waiting_location";
    smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_WAITING_LOCATION]);
}

static enum smf_state_result waiting_location_run(void *o) {
    struct location_test_state_object *state = (struct location_test_state_object *)o;

    if (state->chan == &location_chan) {
        const struct location_msg *msg = (const struct location_msg *)state->msg_buf;

        if (msg->type == LOCATION_DATA) {
            LOG_INF("fix received (lat=%.6f, lon=%.6f, acc=%.1f)", msg->gnss_data.latitude, msg->gnss_data.longitude,
                    (double)msg->gnss_data.accuracy);
            state->location_received = true;

            /* Forward location data to cloud_post for HTTP POST */
            const struct cloud_post_data post_data = {
                .latitude = msg->gnss_data.latitude,
                .longitude = msg->gnss_data.longitude,
                .accuracy = msg->gnss_data.accuracy,
                .is_gnss_search = state->is_gnss_search,
            };
            int err = zbus_chan_pub(&cloud_post_data_chan, &post_data, PUB_TIMEOUT);
            if (err) {
                LOG_ERR("Failed to publish cloud_post_data, error: %d", err);
                SEND_FATAL_ERROR();
                return SMF_EVENT_HANDLED;
            }

            lt_state_name = "waiting_cloud";
            timer_arm(CLOUD_POST_FALLBACK_TIMEOUT_SECONDS);
            smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_WAITING_CLOUD]);
            return SMF_EVENT_HANDLED;
        }

        /* Ignore LOCATION_SEARCH_DONE — the personality only cares about
         * actual location data (success) or the fallback timer (failure).
         * This avoids racing with async cloud geolocation for cellular.
         */
        if (msg->type == LOCATION_SEARCH_DONE) {
            return SMF_EVENT_HANDLED;
        }
    }

    if (state->chan == &timer_chan) {
        LOG_WRN("location timeout — no fix obtained, sending empty");
        /* Publish zero-filled data to cloud_post so it sends empty coordinates. */
        const struct cloud_post_data empty = {
            .latitude = 0.0,
            .longitude = 0.0,
            .accuracy = 0.0f,
            .is_gnss_search = state->is_gnss_search,
        };
        int err = zbus_chan_pub(&cloud_post_data_chan, &empty, PUB_TIMEOUT);
        if (err) {
            LOG_ERR("Failed to publish cloud_post_data, error: %d", err);
            SEND_FATAL_ERROR();
            return SMF_EVENT_HANDLED;
        }
        lt_state_name = "waiting_cloud";
        timer_arm(CLOUD_POST_FALLBACK_TIMEOUT_SECONDS);
        smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_WAITING_CLOUD]);
        return SMF_EVENT_HANDLED;
    }

    return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result waiting_cloud_run(void *o) {
    struct location_test_state_object *state = (struct location_test_state_object *)o;

    if (state->chan == &cloud_post_chan) {
        const struct cloud_post_msg *msg = (const struct cloud_post_msg *)state->msg_buf;

        if (msg->type == CLOUD_POST_SEND_DONE) {
            LOG_INF("cloud POST done (HTTP %d), disconnecting modem now", msg->http_status);
            request_disconnect();
            smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_DISCONNECTING]);
            return SMF_EVENT_HANDLED;
        }

        if (msg->type == CLOUD_POST_SEND_FAILED) {
            LOG_WRN("cloud POST failed (%d), disconnecting", msg->http_status);
            request_disconnect();
            smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_DISCONNECTING]);
            return SMF_EVENT_HANDLED;
        }
    }

    if (state->chan == &timer_chan) {
        LOG_WRN("fallback timeout expired, disconnecting modem");
        request_disconnect();
        smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_DISCONNECTING]);
        return SMF_EVENT_HANDLED;
    }

    return SMF_EVENT_PROPAGATE;
}

static void disconnecting_entry(void *o) {
    ARG_UNUSED(o);
    lt_state_name = "disconnecting";
    LOG_DBG("%s", __func__);
}

static enum smf_state_result disconnecting_run(void *o) {
    struct location_test_state_object *state = (struct location_test_state_object *)o;
    if (state->chan == &network_chan) {
        const struct network_msg *msg = (const struct network_msg *)state->msg_buf;
        if (msg->type == NETWORK_DISCONNECTED) {
            LOG_INF("modem off, entering sleep");
            smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_SLEEPING]);
            return SMF_EVENT_HANDLED;
        }
    }
    return SMF_EVENT_PROPAGATE;
}
static bool net_connect_requested;
static bool location_ready;
static bool lte_ready;

static enum smf_state_result waiting_module_run(void *o) {
    struct location_test_state_object *state = (struct location_test_state_object *)o;

    /* Request network connect to trigger modem init and CFUN callback,
     * which the location module needs to initialize.
     */
    if (!net_connect_requested) {
        const struct network_msg msg = {.type = NETWORK_CONNECT};
        zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
        net_connect_requested = true;
        LOG_INF("requesting network connect for modem init");
    }

    if (state->chan == &location_chan) {
        const struct location_msg *msg = (const struct location_msg *)state->msg_buf;
        if (msg->type == LOCATION_MODULE_READY) {
            LOG_INF("ready");
            location_ready = true;
        }
    }

    if (state->chan == &network_chan) {
        const struct network_msg *msg = (const struct network_msg *)state->msg_buf;
        if (msg->type == NETWORK_CONNECTED) {
            LOG_INF("LTE connected");
            lte_ready = true;
        }
    }

    /* Wait for both location module and LTE before starting search,
     * so A-GNSS data can be fetched immediately when requested. */
    if (location_ready && lte_ready) {
        LOG_INF("both ready, starting first search");
        smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_SAMPLING]);
        return SMF_EVENT_HANDLED;
    }

    return SMF_EVENT_PROPAGATE;
}
static void sleeping_entry(void *o) {
    struct location_test_state_object *state = (struct location_test_state_object *)o;
    lt_state_name = "sleeping";
    LOG_DBG("%s", __func__);
    LOG_INF("sleeping with modem off for %us — measure power now", state->sample_interval_sec);

#if defined(CONFIG_APP_LED)
    LED_BLINK_RED(10);
#endif /* CONFIG_APP_LED */

    timer_arm(state->sample_interval_sec);
}

static enum smf_state_result sleeping_run(void *o) {
    struct location_test_state_object *state = (struct location_test_state_object *)o;
    if (state->chan == &timer_chan) {
        LOG_INF("sleep expired, reconnecting modem");
        smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_CONNECTING]);
        return SMF_EVENT_HANDLED;
    }
    return SMF_EVENT_PROPAGATE;
}

static void connecting_entry(void *o) {
    ARG_UNUSED(o);
    lt_state_name = "connecting";
    LOG_DBG("%s", __func__);
    /* Request LTE connection so the modem is initialized and GNSS becomes
     * available. Previously the modem was put offline during SLEEPING.
     */
    PUBLISH_NETWORK(NETWORK_CONNECT);
}

static enum smf_state_result connecting_run(void *o) {
    struct location_test_state_object *state = (struct location_test_state_object *)o;

    if (state->chan == &network_chan) {
        const struct network_msg *msg = (const struct network_msg *)state->msg_buf;
        if (msg->type == NETWORK_CONNECTED) {
            LOG_INF("LTE re-connected, starting new search cycle");
            smf_set_state(SMF_CTX(state), &states[LOCATION_TEST_STATE_SAMPLING]);
            return SMF_EVENT_HANDLED;
        }
    }

    return SMF_EVENT_PROPAGATE;
}

static void rebooting_entry(void *o) {
    ARG_UNUSED(o);
    lt_state_name = "rebooting";
    LOG_DBG("%s", __func__);
    LOG_PANIC();
    k_sleep(K_SECONDS(10));
    sys_reboot(SYS_REBOOT_COLD);
}

/* ── Public API ─────────────────────────────────────────────────── */

void location_test_init(struct location_test_state_object *state) {
    state->sample_interval_sec = CONFIG_APP_SAMPLING_INTERVAL_SECONDS;
    state->location_received = false;
    state->use_cellular_next = true;
    state->is_gnss_search = false;
    state->last_satellites_tracked = -1;
    lt_state_name = "waiting_module";
    smf_set_initial(SMF_CTX(state), &states[LOCATION_TEST_STATE_WAITING_MODULE]);
}

void location_test_process(struct location_test_state_object *state) {
    int err = smf_run_state(SMF_CTX(state));
    if (err) {
        LOG_ERR("smf_run_state(), error: %d", err);
        SEND_FATAL_ERROR();
    }
}
