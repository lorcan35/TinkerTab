/*
 * debug_server_ota.h — OTA firmware update endpoint family.
 *
 * Wave 23b follow-up (#332): second per-family extract from
 * debug_server.c, after K144 (#336).  Same shape as debug_server_m5.h.
 *
 * Endpoints:
 *   GET  /ota/check  — query Dragon for available firmware update
 *   POST /ota/apply  — schedule the update (reboots into fresh DMA heap)
 */

#pragma once

#include "esp_http_server.h"

/* Register the OTA endpoint family on `server`.  Called from
 * tab5_debug_server_start() in debug_server.c during boot. */
void debug_server_ota_register(httpd_handle_t server);
