/*
 * debug_server_settings.h — settings + factory-reset endpoint family.
 *
 * Wave 23b follow-up (#332): seventh per-family extract from
 * debug_server.c (after K144 #336, OTA #340, codec #341, voice #342,
 * mode #344).  Largest single family by LOC (~400 LOC handlers + the
 * factory-reset guard) — scoped tight to NVS-touching endpoints so
 * the WiFi family (kick + scan + status) can extract independently.
 *
 * Endpoints owned by this module:
 *   GET  /settings              — dump all NVS settings as JSON
 *   POST /settings              — update one or more NVS keys via JSON body
 *   POST /nvs/erase?confirm=... — factory reset (auth-token-prefix guarded)
 *
 * The settings_set handler is the largest single handler in the
 * historical debug_server.c — 280 LOC of "if key present + valid +
 * persisted, append to updated[] array" boilerplate covering every NVS
 * key the harness needs to drive remotely.  Kept verbatim during the
 * extract; the dial-resolver coupling to mode_manager + voice WS
 * reapply is the only non-trivial branch and stays here since both
 * helpers are already extern-declared at their call sites.
 */

#pragma once

#include "esp_http_server.h"

/* Register the settings + factory-reset endpoint family on `server`.
 * Called from tab5_debug_server_start() in debug_server.c during boot. */
void debug_server_settings_register(httpd_handle_t server);
