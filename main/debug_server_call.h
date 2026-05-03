/*
 * debug_server_call.h — video-call + uplink-streaming debug HTTP family.
 *
 * Wave 23b follow-up (#332): tenth per-family extract.  All 12 video/
 * call control handlers moved verbatim from debug_server.c.
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers all 12 video/call URIs against the live HTTPD server.
 * Called from debug_server.c init after the server is started. */
void debug_server_call_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
