/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <net/rest_client.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Timeout in ms for REST client requests. */
#define REST_TIMEOUT_MS 30000

/** Number of retries on transient REST client errors (total attempts = 1 + REST_RETRY_COUNT). */
#define REST_RETRY_COUNT 3

/**
 * @brief Perform a REST client request with retry logic and shared timeout.
 *
 * Sets @c req->timeout_ms to @ref REST_TIMEOUT_MS and retries up to
 * @ref REST_RETRY_COUNT times on transient errors.
 *
 * The caller must call @c rest_client_request_defaults_set() and populate
 * all relevant @p req fields (host, port, url, sec_tag, http_method, body,
 * resp_buff, resp_buff_len, etc.) before calling this function.
 *
 * @param req   REST client request context (partially populated by caller).
 * @param resp  REST client response context (filled on success).
 *
 * @return 0 on success, negative errno on failure after all retries.
 */
int rest_client_request_with_retry(struct rest_client_req_context *req, struct rest_client_resp_context *resp);

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

/**
 * @brief Fetch data via HTTP POST in byte-range chunks.
 *
 * The nRF91 series modem has a ~2 KB limit on TLS receive records, so large
 * HTTP responses must be fetched in small chunks.  This function sends POST
 * requests with a @c Range header and concatenates the chunk responses into
 * a single buffer.
 *
 * Each request includes @p body (e.g. a JSON payload) and requests
 * @p chunk_size bytes.  The loop stops when a response is smaller than
 * @p chunk_size, indicating the final chunk.
 *
 * @param host          Server hostname.
 * @param port          Server port.
 * @param url           URL path.
 * @param sec_tag       TLS security tag.
 * @param body          HTTP request body (sent with every chunk request).
 * @param body_len      Length of @p body.
 * @param chunk_size    Max bytes to request per chunk.  The HTTP response
 *                      (headers + body) must fit in the modem TLS buffer,
 *                      so values of 1200-1400 are typical.
 * @param[out] out_buf  Buffer for the concatenated response data.
 * @param out_buf_size  Size of @p out_buf.
 * @param[out] out_len  Total bytes received across all chunks.
 *
 * @return 0 on success, a negative errno on failure.
 */
int http_fetch_chunked(const char *host, uint16_t port, const char *url, int sec_tag, const char *body, size_t body_len,
                       size_t chunk_size, uint8_t *out_buf, size_t out_buf_size, size_t *out_len);

#ifdef __cplusplus
}
#endif
