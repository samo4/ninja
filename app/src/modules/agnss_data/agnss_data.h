#pragma once

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

ZBUS_CHAN_DECLARE(agnss_data_chan);

enum agnss_data_msg_type {
    AGNSS_DATA_FETCH_DONE,
    AGNSS_DATA_FETCH_FAILED,
};

struct agnss_data_msg {
    enum agnss_data_msg_type type;
    int http_status;
};

#ifdef __cplusplus
}
#endif
