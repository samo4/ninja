/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <date_time.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "modem/lte_lc.h"
#include "modem/modem_info.h"
#include "module_state.h"
#include "network.h"
#include "utils.h"

LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

/* Current FSM state name (for heartbeat reporting). */
static const char *net_state_str;

/* Tracks whether a network disconnect was initiated intentionally.
 * Used to avoid misleading "Network search failed" messages following
 * a deliberate disconnect request.
 */
static bool intentional_disconnect;

BUILD_ASSERT(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS > CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS,
             "Watchdog timeout must be greater than maximum message processing time");

ZBUS_CHAN_DEFINE(network_chan, struct network_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(network);

ZBUS_CHAN_ADD_OBS(network_chan, network, 0);

#if defined(CONFIG_APP_NETWORK_SEARCH_NETWORK_ON_STARTUP)
#error "CONFIG_APP_NETWORK_SEARCH_NETWORK_ON_STARTUP is not supported in this build. Remove it from prj.conf."
#endif

#define MAX_MSG_SIZE sizeof(struct network_msg)

/* State machine */

/* Network module states.
 */
enum network_module_state {
    /* The module is running */
    STATE_RUNNING,
    /* The device is not connected to a network */
    STATE_DISCONNECTED,
    /* The device is disconnected from network and is not searching */
    STATE_DISCONNECTED_IDLE,
    /* The device is disconnected and the modem is searching for networks */
    STATE_DISCONNECTED_SEARCHING,
    /* The device is connected to a network */
    STATE_CONNECTED,

    /* The device has initiated detachment from network, but the modem has not confirmed
     * detachment yet.
     */
    STATE_DISCONNECTING,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct network_state_object {
    /* This must be first */
    struct smf_ctx ctx;

    /* Last channel type that a message was received on */
    const struct zbus_channel *chan;

    /* Buffer for last ZBus message */
    uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static enum smf_state_result state_disconnected_run(void *obj);
static enum smf_state_result state_disconnected_idle_run(void *obj);
static void state_disconnected_searching_entry(void *obj);
static enum smf_state_result state_disconnected_searching_run(void *obj);
static void state_disconnecting_entry(void *obj);
static enum smf_state_result state_disconnecting_run(void *obj);
static enum smf_state_result state_connected_run(void *obj);
static void state_connected_entry(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
    [STATE_RUNNING] = SMF_CREATE_STATE(state_running_entry, state_running_run, NULL, NULL, /* No parent state */
                                       &states[STATE_DISCONNECTED]),
    [STATE_DISCONNECTED] = SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
                                            &states[STATE_RUNNING], &states[STATE_DISCONNECTED_IDLE]),
    [STATE_DISCONNECTED_IDLE] = SMF_CREATE_STATE(NULL, state_disconnected_idle_run, NULL, &states[STATE_DISCONNECTED],
                                                 NULL), /* No initial transition */
    [STATE_DISCONNECTED_SEARCHING] =
        SMF_CREATE_STATE(state_disconnected_searching_entry, state_disconnected_searching_run, NULL,
                         &states[STATE_DISCONNECTED], NULL), /* No initial transition */
    [STATE_CONNECTED] = SMF_CREATE_STATE(state_connected_entry, state_connected_run, NULL, &states[STATE_RUNNING],
                                         NULL), /* No initial transition */
    [STATE_DISCONNECTING] = SMF_CREATE_STATE(state_disconnecting_entry, state_disconnecting_run, NULL,
                                             &states[STATE_RUNNING], NULL), /* No initial transition */
};

static void network_status_notify(enum network_msg_type status) {
    struct network_msg msg = {
        .type = status,
    };

    int err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("zbus_chan_pub, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }
}

static void network_msg_send(const struct network_msg *msg) {
    int err = zbus_chan_pub(&network_chan, msg, PUB_TIMEOUT);
    if (err) {
        LOG_ERR("zbus_chan_pub, error: %d", err);
        SEND_FATAL_ERROR();
    }
}

static struct k_work_delayable connected_dwork;

static void connected_dwork_handler(struct k_work *work) {
    LOG_INF(VT100_GREEN "Network ready" VT100_RESET);
    network_status_notify(NETWORK_CONNECTED);
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt) {
    switch (evt->type) {
        case LTE_LC_EVT_NW_REG_STATUS:
            if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
                intentional_disconnect = false;
                LOG_INF(VT100_RED "Network search failed" VT100_RESET);
                LOG_ERR("No SIM card detected!");
                network_status_notify(NETWORK_UICC_FAILURE);
            } else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
                if (intentional_disconnect) {
                    LOG_INF(VT100_YELLOW "No longer registered (intentional disconnect)" VT100_RESET);
                } else {
                    LOG_INF(VT100_RED "Network search failed" VT100_RESET);
                    LOG_WRN("Not registered, check rejection cause");
                    network_status_notify(NETWORK_ATTACH_REJECTED);
                }
            } else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
                LOG_INF("NW reg status: %d (roaming)", evt->nw_reg_status);
            } else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) {
                LOG_INF("NW reg status: %d (home)", evt->nw_reg_status);
            } else if (evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING) {
                LOG_INF("NW reg status: %d (searching)", evt->nw_reg_status);
            } else {
                LOG_INF("NW reg status: %d", evt->nw_reg_status);
            }
            break;
        case LTE_LC_EVT_PDN:
            switch (evt->pdn.type) {
                case LTE_LC_EVT_PDN_ACTIVATED: {
                    int pdn_err = k_work_schedule(&connected_dwork, K_SECONDS(30));
                    if (pdn_err < 0) {
                        LOG_ERR("Failed to schedule connected work, error: %d", pdn_err);
                    }
                    LOG_DBG("PDN connection activated, will notify connected in 30s");
                    break;
                }
                case LTE_LC_EVT_PDN_DEACTIVATED: {
                    k_work_cancel_delayable(&connected_dwork);
                    LOG_DBG("deactivated");
                    network_status_notify(NETWORK_DISCONNECTED);
                    break;
                }
                case LTE_LC_EVT_PDN_NETWORK_DETACH: {
                    k_work_cancel_delayable(&connected_dwork);
                    LOG_DBG("detach");
                    network_status_notify(NETWORK_DISCONNECTED);
                    break;
                }
                case LTE_LC_EVT_PDN_SUSPENDED: {
                    k_work_cancel_delayable(&connected_dwork);
                    LOG_INF("suspended");
                    network_status_notify(NETWORK_DISCONNECTED);
                    break;
                }
                case LTE_LC_EVT_PDN_RESUMED: {
                    LOG_DBG("PDN connection resumed");
                    network_status_notify(NETWORK_CONNECTED);
                    break;
                }
                default:
                    break;
            }
            break;
        case LTE_LC_EVT_MODEM_EVENT:
            /* If a reset loop happens in the field, it should not be necessary
             * to perform any action. The modem will try to re-attach to the LTE network after
             * the 30-minute block.
             */
            if (evt->modem_evt.type == LTE_LC_MODEM_EVT_RESET_LOOP) {
                LOG_WRN("The modem has detected a reset loop!");
                network_status_notify(NETWORK_MODEM_RESET_LOOP);
            } else if (evt->modem_evt.type == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
                LOG_INF(VT100_RED "Network search failed" VT100_RESET);
                network_status_notify(NETWORK_LIGHT_SEARCH_DONE);
            } else if (evt->modem_evt.type == LTE_LC_MODEM_EVT_SEARCH_DONE) {
                LOG_INF(VT100_RED "Network search failed" VT100_RESET);
                network_status_notify(NETWORK_SEARCH_DONE);
            }
            break;
        case LTE_LC_EVT_PSM_UPDATE: {
            struct network_msg msg = {
                .type = NETWORK_PSM_PARAMS,
                .psm_cfg = evt->psm_cfg,
            };
            LOG_DBG("PSM parameters received, TAU: %d, Active time: %d", msg.psm_cfg.tau, msg.psm_cfg.active_time);
            network_msg_send(&msg);
            break;
        }
        case LTE_LC_EVT_EDRX_UPDATE: {
            struct network_msg msg = {
                .type = NETWORK_EDRX_PARAMS,
                .edrx_cfg = evt->edrx_cfg,
            };
            LOG_DBG("eDRX parameters received, mode: %d, eDRX: %0.2f s, PTW: %.02f s", msg.edrx_cfg.mode,
                    (double)msg.edrx_cfg.edrx, (double)msg.edrx_cfg.ptw);
            network_msg_send(&msg);
            break;
        }
        case LTE_LC_EVT_LTE_MODE_UPDATE:
            LOG_INF("LTE mode: %s", evt->lte_mode == 7 ? "LTE-M" : evt->lte_mode == 9 ? "NB-IoT" : "other");
            break;
        case LTE_LC_EVT_RRC_UPDATE:
            //     LOG_DBG("RRC state: %s", evt->rrc_mode ? "Connected" : "Idle");
            break;
        case LTE_LC_EVT_CELL_UPDATE:
            // LOG_INF("Cell: TAC %u ID %u", evt->cell.tac, evt->cell.id);
            break;
        case LTE_LC_EVT_MODEM_SLEEP_EXIT:
            // LOG_DBG("Modem sleep exit");
            break;
        case LTE_LC_EVT_MODEM_SLEEP_ENTER:
            // LOG_DBG("Modem sleep enter (type %d)", evt->modem_sleep.type);
            break;
        default:
            LOG_DBG("Unhandled LTE event type: %d", evt->type);
            break;
    }
}

static void request_system_mode(void) {
    struct network_msg msg = {
        .type = NETWORK_SYSTEM_MODE_RESPONSE,
    };
    enum lte_lc_system_mode_preference dummy_preference;
    int err = lte_lc_system_mode_get(&msg.system_mode, &dummy_preference);
    if (err) {
        LOG_ERR("lte_lc_system_mode_get, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }
    network_msg_send(&msg);
}

static int network_disconnect(void) {
    intentional_disconnect = true;
    int err = lte_lc_power_off(); // not lte_lc_offline
    if (err) {
        LOG_ERR("lte_lc_power_off, error: %d", err);
        return err;
    }
    return 0;
}

/* State handlers */

static void state_running_entry(void *obj) {
    ARG_UNUSED(obj);
    net_state_str = "running";
    LOG_DBG("state_running_entry");
    int err = nrf_modem_lib_init();
    if (err) {
        LOG_ERR("Failed to initialize the modem library, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    lte_lc_register_handler(lte_lc_evt_handler);

    //* Register handler for default PDP context.
    err = lte_lc_pdn_default_ctx_events_enable();
    if (err) {
        LOG_ERR("lte_lc_pdn_default_ctx_events_enable, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }

    k_work_init_delayable(&connected_dwork, connected_dwork_handler);
    LOG_DBG("Network module started");
}

static enum smf_state_result state_running_run(void *obj) {
    struct network_state_object const *state_object = obj;

    if (&network_chan == state_object->chan) {
        const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;
        switch (msg->type) {
            case NETWORK_DISCONNECTED:
                smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);
                return SMF_EVENT_HANDLED;
            case NETWORK_UICC_FAILURE:
                smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);
                return SMF_EVENT_HANDLED;
            case NETWORK_SYSTEM_MODE_REQUEST:
                request_system_mode();
                return SMF_EVENT_HANDLED;
            default:
                break;
        }
    }

    return SMF_EVENT_PROPAGATE;
}

static void state_disconnected_entry(void *obj) {
    ARG_UNUSED(obj);
    net_state_str = "disconnected";
    LOG_DBG("->disconnected");
}

static enum smf_state_result state_disconnected_run(void *obj) {
    struct network_state_object const *state_object = obj;
    if (&network_chan == state_object->chan) {
        const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;
        switch (msg->type) {
            case NETWORK_CONNECTED:
                smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);
                return SMF_EVENT_HANDLED;
            case NETWORK_DISCONNECTED:
                return SMF_EVENT_HANDLED;
            default:
                break;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

static void state_disconnected_searching_entry(void *obj) {
    ARG_UNUSED(obj);
    net_state_str = "searching...";
    intentional_disconnect = false;
    LOG_INF(VT100_YELLOW "searching..." VT100_RESET);
    int err = lte_lc_connect_async(lte_lc_evt_handler);
    if (err) {
        LOG_ERR("lte_lc_connect_async, error: %d", err);
        return;
    }
}

static enum smf_state_result state_disconnected_searching_run(void *obj) {
    int err;
    struct network_state_object const *state_object = obj;
    if (&network_chan == state_object->chan) {
        const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;
        switch (msg->type) {
            case NETWORK_CONNECT:
                return SMF_EVENT_HANDLED;
            case NETWORK_SEARCH_STOP:
                __fallthrough;
            case NETWORK_DISCONNECT:
                err = network_disconnect();
                if (err) {
                    LOG_ERR("network_disconnect, error: %d", err);
                    SEND_FATAL_ERROR();
                }
                smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);
                return SMF_EVENT_HANDLED;
            default:
                break;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result state_disconnected_idle_run(void *obj) {
    int err;
    net_state_str = "idle";
    struct network_state_object const *state_object = obj;
    if (&network_chan == state_object->chan) {
        const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;
        switch (msg->type) {
            case NETWORK_DISCONNECT:
                return SMF_EVENT_HANDLED;
            case NETWORK_CONNECT:
                smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_SEARCHING]);
                return SMF_EVENT_HANDLED;
            case NETWORK_SYSTEM_MODE_SET_LTEM:
                err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
                if (err) {
                    LOG_ERR("lte_lc_system_mode_set, error: %d", err);
                    SEND_FATAL_ERROR();
                }
                return SMF_EVENT_HANDLED;
            case NETWORK_SYSTEM_MODE_SET_NBIOT:
                err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NBIOT_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
                if (err) {
                    LOG_ERR("lte_lc_system_mode_set, error: %d", err);
                    SEND_FATAL_ERROR();
                }
                return SMF_EVENT_HANDLED;
            case NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT:
                err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
                if (err) {
                    LOG_ERR("lte_lc_system_mode_set, error: %d", err);
                    SEND_FATAL_ERROR();
                }
                return SMF_EVENT_HANDLED;
            default:
                break;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

static void state_connected_entry(void *obj) {
    ARG_UNUSED(obj);
    net_state_str = "connected";
    LOG_DBG("state_connected_entry");
}

static enum smf_state_result state_connected_run(void *obj) {
    struct network_state_object const *state_object = obj;
    if (&network_chan == state_object->chan) {
        const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;
        if (msg->type == NETWORK_DISCONNECT) {
            smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTING]);
            return SMF_EVENT_HANDLED;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

static void state_disconnecting_entry(void *obj) {
    ARG_UNUSED(obj);
    net_state_str = "disconnecting";
    LOG_DBG("state_disconnecting_entry");
    int err = network_disconnect();
    if (err) {
        LOG_ERR("network_disconnect, error: %d", err);
        SEND_FATAL_ERROR();
        return;
    }
}

static enum smf_state_result state_disconnecting_run(void *obj) {
    struct network_state_object const *state_object = obj;
    if (&network_chan == state_object->chan) {
        const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;
        if (msg->type == NETWORK_DISCONNECTED) {
            smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);
            return SMF_EVENT_HANDLED;
        }
    }
    return SMF_EVENT_PROPAGATE;
}

TASK_WDT_CALLBACK_DEFINE(network)

static void network_module_thread(void) {
    int err;
    TASK_WDT_TIMEOUTS(APP_NETWORK);
    TASK_WDT_ZBUS_TIMEOUT;
    static struct network_state_object network_state;

    TASK_WDT_ADD(network, wdt_timeout_ms)

    smf_set_initial(SMF_CTX(&network_state), &states[STATE_RUNNING]);

    while (true) {
        TASK_WDT_FEED();

        err = zbus_sub_wait_msg(&network, &network_state.chan, network_state.msg_buf, zbus_wait_ms);
        if (err == -ENOMSG) {
            continue;
        } else if (err) {
            LOG_ERR("zbus_sub_wait_msg, error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }

        err = smf_run_state(SMF_CTX(&network_state));
        if (err) {
            LOG_ERR("smf_run_state(), error: %d", err);
            SEND_FATAL_ERROR();
            return;
        }
    }
}

K_THREAD_DEFINE(network_module_thread_id, CONFIG_APP_NETWORK_THREAD_STACK_SIZE, network_module_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

const char *network_state_str(void) { return net_state_str ? net_state_str : "?"; }
