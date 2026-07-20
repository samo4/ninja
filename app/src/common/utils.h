/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write text data to the console in small chunks.
 *
 * Zephyr's RTT log backend drops messages that don't fit in its output buffer
 * (@ref CONFIG_LOG_BACKEND_RTT_MODE_DROP).  This function bypasses the logger
 * and writes directly to the RTT channel in 64-byte chunks so that no data is
 * lost, regardless of the response size.
 *
 * When RTT is not available (UART console, etc.) the data is logged via
 * @c LOG_HEXDUMP_DBG.
 *
 * @param data  Pointer to the text buffer.
 * @param len   Number of bytes to write.
 */
void rtt_dump_text(const char *data, size_t len);

#ifdef __cplusplus
}
#endif
