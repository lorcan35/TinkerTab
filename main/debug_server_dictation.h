/*
 * debug_server_dictation.h — /dictation remote-driver endpoint family.
 *
 * Wave 23b follow-up (#332): ninth per-family extract from
 * debug_server.c (after K144 #336, OTA #340, codec #341, voice #342,
 * mode #344, settings #346, WiFi #347).  Tiny self-contained extract
 * — single handler hung off two URI methods, voice.h dependencies
 * only.
 *
 * Endpoints owned by this module:
 *   POST /dictation?action=start  — begin dictation pipeline
 *   POST /dictation?action=stop   — stop + flush accumulated transcript
 *   GET  /dictation               — snapshot transcript + voice state
 *
 * The handler is the automation equivalent of the long-press-mic
 * gesture and is exercised by the long-duration (5/10/30 min)
 * dictation pipeline tests in tests/e2e/.
 */

#pragma once

#include "esp_http_server.h"

/* Register the dictation endpoint family on `server`.
 * Called from tab5_debug_server_start() in debug_server.c during boot. */
void debug_server_dictation_register(httpd_handle_t server);
