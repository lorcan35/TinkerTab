/*
 * debug_server_metrics.h — diagnostics / metrics / device-control debug
 * HTTP family.
 *
 * Wave 23b follow-up (#332): eighteenth (and final) per-family extract.
 * Owns the 11 small read-only/control endpoints that didn't fit any
 * other family bucket: process snapshot, log tail, prometheus metrics,
 * obs events ring, heap-history sampling, battery state, display
 * brightness, audio (vol+mute), keyboard layout dump, tool_log
 * injection, net ping, plus the /heap/probe-csv pool-probe relay.
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers all 12 metrics/diagnostics endpoints against the live
 * HTTPD server.  Called from debug_server.c init after the server
 * is started. */
void debug_server_metrics_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
