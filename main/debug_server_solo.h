/**
 * debug_server_solo.h — synthetic test endpoints for vmode=5 modules.
 *
 * Currently exposes:
 *   POST /solo/sse_test  — feed raw SSE bytes, get parsed deltas back
 *
 * Subsequent commits add /solo/llm_test and /solo/rag_test as the
 * solo modules grow.
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void debug_server_solo_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
