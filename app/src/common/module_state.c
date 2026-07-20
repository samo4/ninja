/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#include "module_state.h"

/* ── Weak default implementations ──────────────────────────────────
 * Modules that do NOT implement their own getter will fall back to
 * these, returning "?" so the heartbeat line still works.
 */

__attribute__((weak)) const char *personality_state_str(void) { return "?"; }
__attribute__((weak)) const char *network_state_str(void) { return "?"; }
__attribute__((weak)) const char *location_state_str(void) { return "?"; }
__attribute__((weak)) const char *environmental_state_str(void) { return "?"; }

/* ── State report formatting ────────────────────────────────────── */

int state_report_format(char *buf, size_t len) {
    int off = 0;

    off += snprintf(buf + off, len - off, "\u2665");
    off += snprintf(buf + off, len - off, " per:%s", personality_state_str());

    /* Append each module that deviates from the default "?" */
    const char *net = network_state_str();
    if (strcmp(net, "?") != 0) {
        off += snprintf(buf + off, len - off, " net:%s", net);
    }

    const char *loc = location_state_str();
    if (strcmp(loc, "?") != 0) {
        off += snprintf(buf + off, len - off, " loc:%s", loc);
    }

    const char *env = environmental_state_str();
    if (strcmp(env, "?") != 0) {
        off += snprintf(buf + off, len - off, " env:%s", env);
    }

    return off;
}
