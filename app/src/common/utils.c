/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>
#include <zephyr/types.h>

#include <errno.h>
#include <string.h>

#include <net/rest_client.h>

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

/* ---------------------------------------------------------------------------
 * RTT text dump helper
 * -------------------------------------------------------------------------*/

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

/* ---------------------------------------------------------------------------
 * Chunked HTTP fetcher
 *
 * The nRF91 series modem has a ~2 KB limit on TLS receive records, so the
 * HTTP response (headers + body) must stay under that limit.  This function
 * works around the limitation by requesting small byte ranges and
 * concatenating the results.
 * -------------------------------------------------------------------------*/

/*
 * Per-request response buffer.  The modem TLS limit is ~2 KB; we reserve
 * 512 bytes for HTTP response headers and use the rest for the payload.
 */
#define HTTP_CHUNK_RESP_BUF_SIZE 2048

/*
 * Maximum safe payload per chunk: leave room for HTTP response headers
 * within the modem's TLS buffer.
 */
#define HTTP_CHUNK_MAX_PAYLOAD (HTTP_CHUNK_RESP_BUF_SIZE - 512)

int http_fetch_chunked(const char *host, uint16_t port, const char *url, int sec_tag, const char *body, size_t body_len,
                       size_t chunk_size, uint8_t *out_buf, size_t out_buf_size, size_t *out_len) {
    int err;
    size_t total = 0;

    if (!out_len) {
        return -EINVAL;
    }
    *out_len = 0;

    if (chunk_size > HTTP_CHUNK_MAX_PAYLOAD) {
        LOG_ERR("chunk_size %zu exceeds maximum %u", chunk_size, HTTP_CHUNK_MAX_PAYLOAD);
        return -EINVAL;
    }

    for (uint32_t offset = 0; offset < out_buf_size;) {
        char resp_buf[HTTP_CHUNK_RESP_BUF_SIZE];
        char range_str[48];
        uint32_t chunk_end;

        chunk_end = offset + chunk_size - 1;
        if (chunk_end >= out_buf_size) {
            chunk_end = out_buf_size - 1;
        }

        {
            int n = snprintf(range_str, sizeof(range_str), "Range: bytes=%u-%u\r\n", offset, chunk_end);
            if (n < 0 || n >= (int)sizeof(range_str)) {
                LOG_ERR("Failed to format Range header");
                return -ENOMEM;
            }
        }

        const char *header_fields[] = {"Content-Type: application/json\r\n", range_str, NULL};

        LOG_DBG("Fetching chunk bytes %u-%u (total: %zu)", offset, chunk_end, total);

        struct rest_client_req_context req = {0};
        struct rest_client_resp_context resp = {0};

        rest_client_request_defaults_set(&req);

        req.host = host;
        req.port = port;
        req.url = url;
        req.sec_tag = sec_tag;
        req.tls_peer_verify = 0;
        req.http_method = HTTP_POST;
        req.header_fields = header_fields;
        req.body = body;
        req.body_len = body_len;
        req.resp_buff = resp_buf;
        req.resp_buff_len = sizeof(resp_buf);
        req.timeout_ms = 15000;

        err = rest_client_request(&req, &resp);
        if (err) {
            LOG_ERR("Chunk request at offset %u failed: %d", offset, err);
            return err;
        }

        if (resp.http_status_code != 200) {
            LOG_ERR("Server returned HTTP %d for chunk at %u", resp.http_status_code, offset);
            return -EIO;
        }

        if (resp.response_len == 0) {
            break;
        }

        if (total + resp.response_len > out_buf_size) {
            LOG_ERR("Output buffer would overflow "
                    "(%zu + %d > %zu)",
                    total, resp.response_len, out_buf_size);
            return -ENOMEM;
        }

        memcpy(out_buf + total, resp.response, resp.response_len);
        total += resp.response_len;

        LOG_DBG("Chunk received: %d bytes, total: %zu", resp.response_len, total);

        /* A short chunk means this was the last one. */
        if ((size_t)resp.response_len < chunk_size) {
            break;
        }

        offset = chunk_end + 1;
    }

    *out_len = total;
    return 0;
}
