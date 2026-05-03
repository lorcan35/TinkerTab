/*
 * debug_server_admin.h — system-control / storage / widget-injection
 * debug HTTP family.
 *
 * Wave 23b follow-up (#332): fifteenth per-family extract.  Owns
 * /reboot, /sdcard (storage listing), and /widget (test-harness
 * widget injection — bypasses Dragon).
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers /reboot + /sdcard + /widget against the live HTTPD server.
 * Called from debug_server.c init after the server is started. */
void debug_server_admin_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
