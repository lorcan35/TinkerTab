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
