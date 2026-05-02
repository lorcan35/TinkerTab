/*
 * debug_server_voice.h — voice-pipeline debug endpoint family.
 *
 * Wave 23b follow-up (#332): fourth per-family extract from
 * debug_server.c (after K144 #336, OTA #337, codec #338).
 *
 * Endpoints owned by this module:
 *   GET  /voice            — voice pipeline state snapshot
 *   POST /voice/reconnect  — force voice WS reconnect
 *   POST /voice/cancel     — cancel in-flight voice turn
 *   POST /voice/clear      — clear Dragon + Tab5 conversation history
 *
 * Out of scope (separate future PRs): /voice/text (which delegates to
 * /chat), the /chat family, /mode, /input/text, /dictation — those
 * depend on chat_handler + chat_msg_store + heavier coupling and ship
 * as their own families.
 */

#pragma once

#include "esp_http_server.h"

/* Register the voice endpoint family on `server`.  Called from
 * tab5_debug_server_start() in debug_server.c during boot. */
void debug_server_voice_register(httpd_handle_t server);
