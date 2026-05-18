/*
 * debug_server_tinkeron.h — TinkerON (K144) full health + control family.
 *
 * TT #578 (2026-05-18): extends the existing m5 endpoint surface with
 * a dedicated tinkeron family that gives the user runtime visibility
 * + control over the always-on wakeword listener from the Tab5 debug
 * HTTP server.  Builds on the wakeword work in PR #576.
 *
 * Endpoints owned by this module:
 *   GET  /tinkeron/status        — combined snapshot: wakeword + K144 hwinfo
 *   POST /tinkeron/arm?on=1|0    — start / stop the always-on listener
 *   POST /tinkeron/wake_phrase   — change wake phrase at runtime
 *   POST /tinkeron/reboot        — full K144 Linux reboot via sys.reboot
 *   GET  /tinkeron/transcripts   — recent ASR partials (debug tail)
 */

#pragma once

#include "esp_http_server.h"

/* Register the TinkerON endpoint family.  Called from
 * tab5_debug_server_start() in debug_server.c during boot. */
void debug_server_tinkeron_register(httpd_handle_t server);
