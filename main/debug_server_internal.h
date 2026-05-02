/*
 * debug_server_internal.h — shared interface for debug_server.c and its
 * per-family extracted modules (debug_server_m5.c, …).
 *
 * Wave 23b (#332): the historical 4,520-LOC debug_server.c is being
 * split family-by-family per the cross-stack architecture audit
 * (2026-05-01).  Each family file owns its handlers + URI registrations
 * + family-local helpers; this header exposes the cross-cutting
 * primitives every family needs (auth gate, common types).
 *
 * Not exposed publicly — `debug_server.h` remains the only public-facing
 * header.  This file is for in-tree compilation units that participate in
 * the debug server's URI map.
 */

#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

/* Bearer-token auth gate.  Every family handler that requires auth
 * (i.e. everything except /info and /selftest) calls this as the
 * first line and returns ESP_OK on `false`.  See debug_server.c for
 * the implementation — this is a thin wrapper around the existing
 * `check_auth` static so the historic call sites stay untouched.
 */
bool tab5_debug_check_auth(httpd_req_t *req);

/* Forward decl for cJSON — keep this header self-sufficient without
 * pulling cJSON.h into every consumer just for the prototype. */
struct cJSON;

/* Build-and-send-JSON helper.  Takes ownership of `root` (calls
 * cJSON_Delete after PrintUnformatted) and emits with
 * Content-Type: application/json + the standard CORS headers.
 * Used by ~57 handlers in debug_server.c and growing — each
 * per-family extract gains access without re-implementing. */
esp_err_t tab5_debug_send_json_resp(httpd_req_t *req, struct cJSON *root);

/* Factory-reset confirm-token check.  POST /nvs/erase?confirm=<token>
 * requires the first 8 hex chars of the bearer auth token as a guard
 * against accidental triggering.  Returns true iff `confirm` matches.
 * The auth token itself is a file-static inside debug_server.c — this
 * wrapper lets the settings family extract validate without leaking
 * the secret through the internal header. */
bool tab5_debug_check_factory_reset_confirm(const char *confirm);
