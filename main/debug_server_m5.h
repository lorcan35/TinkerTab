/*
 * debug_server_m5.h — K144 (M5Stack LLM Module) endpoint family.
 *
 * Wave 23b (#332): extracted from debug_server.c as the proof-of-pattern
 * for the per-family split called for in the cross-stack architecture
 * audit (2026-05-01).
 *
 * Endpoints owned by this module:
 *   GET  /m5          — diagnostic snapshot (chain + failover + cached hwinfo)
 *   POST /m5/refresh  — force sys.hwinfo refresh outside the 30 s TTL
 *   GET  /m5/models   — surface sys.lsmode model registry (5 min cache)
 *   POST /m5/reset    — trigger voice_onboard_reset_failover()
 *
 * Plus two private cache layers (hwinfo + modelist) — see the .c file.
 */

#pragma once

#include "esp_http_server.h"

/* Register the K144 endpoint family on `server`.  Called from
 * tab5_debug_server_start() in debug_server.c during boot. */
void debug_server_m5_register(httpd_handle_t server);
