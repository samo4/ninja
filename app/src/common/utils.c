/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>
#include <zephyr/types.h>

#include "utils.h"

LOG_MODULE_REGISTER(utils, CONFIG_APP_LOG_LEVEL);

/* Each chunk is small enough to fit comfortably inside the RTT up-buffer
 * (typically 1 kiB) so the NO_BLOCK_SKIP mode used by the default channel 0
 * buffer has no reason to discard it.
 */
#define RTT_DUMP_CHUNK_SIZE 64

#if defined(CONFIG_USE_SEGGER_RTT)
#include <SEGGER_RTT.h>
#endif

void rtt_dump_text(const char *data, size_t len) {
    if (!data || len == 0) {
        return;
    }

#if defined(CONFIG_USE_SEGGER_RTT)
    /* Opening banner. */
    SEGGER_RTT_WriteString(0, "\n----- Response body (");

    {
        char hdr[48];
        int n = snprintf(hdr, sizeof(hdr), "%u bytes) -----\n", (unsigned int)len);
        if (n > 0) {
            SEGGER_RTT_Write(0, hdr, n);
        }
    }

    /* Write in small chunks so the RTT buffer never overflows. */
    {
        char chunk[RTT_DUMP_CHUNK_SIZE + 1];
        size_t offset = 0;

        while (offset < len) {
            size_t todo = len - offset;

            if (todo > RTT_DUMP_CHUNK_SIZE) {
                todo = RTT_DUMP_CHUNK_SIZE;
            }

            /* Sanitise non-printable characters. */
            for (size_t i = 0; i < todo; i++) {
                char c = data[offset + i];
                chunk[i] = (c >= 32 && c < 127) ? c : '.';
            }
            chunk[todo] = '\0';

            SEGGER_RTT_Write(0, chunk, todo);
            offset += todo;
        }
    }

    /* Trailing newline and footer. */
    SEGGER_RTT_WriteString(0, "\n----- End of response body -----\n");

#else /* !CONFIG_USE_SEGGER_RTT */

    LOG_HEXDUMP_DBG(data, len, "Response body");

#endif /* CONFIG_USE_SEGGER_RTT */
}
