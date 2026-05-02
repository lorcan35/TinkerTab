/*
 * debug_server_mode.h — voice-mode switch endpoint family.
 *
 * Wave 23b follow-up (#332): fifth per-family extract from
 * debug_server.c (after K144 #336, OTA #340, codec #341, voice #342).
 *
 * Endpoint owned by this module:
 *   POST /mode?m=0|1|2|3|4&model=...  — switch active voice mode
 *
 * The async refresh helper that pokes the home-screen mode badge on
 * the LVGL thread also lives here since it's only used by the mode
 * handler.
 */

#pragma once

#include "esp_http_server.h"

/* Register the voice-mode endpoint family on `server`.  Called from
 * tab5_debug_server_start() in debug_server.c during boot. */
void debug_server_mode_register(httpd_handle_t server);
