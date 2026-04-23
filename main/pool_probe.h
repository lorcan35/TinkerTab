/**
 * TinkerTab — LVGL pool-pressure probe (Phase 1 instrumentation).
 *
 * Registers a heap-alloc-failure callback that logs every time a
 * `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)` or similar returns NULL,
 * and spawns a background task that samples heap + LVGL-pool stats at
 * 1 Hz into a 900-entry PSRAM ring buffer (15 min of history).
 *
 * Read via GET /heap-history on the debug server; CSV format.
 *
 * See docs/STABILITY-INVESTIGATION.md Phase 1 for what we're hunting.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Initialize the probe. Registers the heap-alloc failure callback and
 * spawns the 1 Hz sampler task. Idempotent — safe to call twice.
 */
esp_err_t tab5_pool_probe_init(void);

/**
 * Debug-server handler: writes the ring buffer as CSV to the response.
 * Installed by debug_server.c as GET /heap-history (bearer-auth
 * required, same as other debug endpoints).
 */
esp_err_t tab5_pool_probe_http_handler(httpd_req_t *req);
