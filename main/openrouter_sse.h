/**
 * openrouter_sse — line-buffered Server-Sent-Events chunk parser.
 *
 * Feeds bytes via openrouter_sse_feed; calls back per `data: <json>`
 * line with the JSON body (string lifetime: callback only).  Returns
 * true once the stream has hit `data: [DONE]`.  Comment lines (lines
 * starting with `:`) are dropped silently per the SSE spec.  Multi-
 * event single feed is fine; chunks split mid-line are buffered.
 *
 * Allocates a 4 KB line buffer in PSRAM via heap_caps_malloc on init;
 * caller must free with openrouter_sse_free.
 *
 * TT #370 — see docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*openrouter_sse_data_cb_t)(const char *json, size_t len, void *ctx);

typedef struct openrouter_sse_state openrouter_sse_state_t;

esp_err_t openrouter_sse_init(openrouter_sse_state_t **out,
                              openrouter_sse_data_cb_t cb, void *ctx);

/** Feed bytes; returns true if the [DONE] sentinel has been observed
 *  (cumulative across feeds). */
bool openrouter_sse_feed(openrouter_sse_state_t *s, const char *buf, size_t len);

void openrouter_sse_free(openrouter_sse_state_t *s);

#ifdef __cplusplus
}
#endif
