/*
 * debug_server_inject.h — synthetic-injection debug HTTP family.
 *
 * Wave 23b follow-up (#332): seventeenth per-family extract.  Owns
 * the three /debug/inject_* test-harness endpoints that synthesise
 * faults / audio / WS frames so the e2e suite can exercise UX paths
 * (banners, toasts, STT round-trip) without needing real fault
 * conditions.
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers /debug/inject_audio + /debug/inject_error +
 * /debug/inject_ws against the live HTTPD server.  Called from
 * debug_server.c init after the server is started. */
void debug_server_inject_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
