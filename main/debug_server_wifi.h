/*
 * debug_server_wifi.h — WiFi diagnostic + recovery endpoint family.
 *
 * Wave 23b follow-up (#332): eighth per-family extract from
 * debug_server.c (after K144 #336, OTA #340, codec #341, voice #342,
 * mode #344, settings #346).  Two small handlers extracted as a unit
 * since they share the WiFi diagnostic surface (status read + recovery
 * kick) and live next to each other in the URI map.
 *
 * Endpoints owned by this module:
 *   GET  /wifi/status                   — connection state + AP info snapshot
 *   POST /wifi/kick?mode=soft|hard|reboot — escalation tiers from #146,
 *                                           drives the test-harness recovery
 *                                           paths without waiting for real flap
 *
 * The /info handler in debug_server.c also reads `tab5_wifi_connected`
 * + the netif IP — it keeps its own private copy of the small
 * `get_wifi_ip` helper rather than going through this header, since
 * those two read-only callers don't justify a public surface for the
 * helper.
 */

#pragma once

#include "esp_http_server.h"

/* Register the WiFi diagnostic + recovery endpoint family on `server`.
 * Called from tab5_debug_server_start() in debug_server.c during boot. */
void debug_server_wifi_register(httpd_handle_t server);
