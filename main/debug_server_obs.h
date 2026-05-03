/*
 * debug_server_obs.h — observability debug HTTP family.
 *
 * Wave 23b follow-up (#332): twelfth per-family extract.  Owns the
 * heap-state, log, crashlog, coredump streaming, and heap-trace
 * (leak-hunt) endpoints.
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers /log /crashlog /coredump /heap_trace_start /heap_trace_dump
 * /heap against the live HTTPD server.  Called from debug_server.c
 * init after the server is started. */
void debug_server_obs_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
