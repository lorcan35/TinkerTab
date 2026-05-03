/*
 * debug_server_input.h — touch + text-input debug HTTP family.
 *
 * Wave 23b follow-up (#332): thirteenth per-family extract.  Owns
 * /touch (synthetic touch injection) + /input/text (LVGL textarea
 * remote control) + the seqlock-protected injection-state buffer
 * the LVGL touch driver reads from.
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers /touch + /input/text against the live HTTPD server.
 * Called from debug_server.c init after the server is started. */
void debug_server_input_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
