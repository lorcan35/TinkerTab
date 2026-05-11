/*
 * voice_messages_sync.h — Wave 3-C-b (cross-stack cohesion audit
 * 2026-05-11): Tab5 POSTs SOLO and ONBOARD turn pairs to Dragon's
 * canonical messages DB via `POST /api/v1/sessions/{id}/messages`
 * (the endpoint added by W3-C-a, TinkerBox PR #276).
 *
 * Before this module, SOLO turns lived only in `solo_session_store`
 * on Tab5 SD (invisible to Dragon) and ONBOARD turns weren't
 * persisted at all.  After this module, Dragon's messages DB holds
 * the canonical record across all six voice modes, so the dashboard
 * Conversations tab sees mode 4/5 activity and cross-session memory
 * search works uniformly.
 *
 * ## API
 *
 * `voice_messages_sync_post(role, content, input_mode)` —
 * fire-and-forget; the actual HTTP POST runs on the shared
 * task_worker.  Returns ESP_OK if the job was queued, ESP_ERR_NO_MEM
 * if the worker queue is full (rare; caller logs + continues), or
 * ESP_ERR_INVALID_STATE if NVS session_id or dragon_host isn't
 * configured.
 *
 * ## Failure mode
 *
 * If the HTTP POST fails (Dragon down, 5xx, network error), the
 * failure is logged at WARN level and the message is dropped.  An
 * SD-card offline queue + drain-on-WS-reconnect path is deferred to
 * W3-C-c (#TBD).  Dropping is acceptable for the v1 cohesion
 * payoff — the canonical store gains the turn whenever the network
 * is up, and the historical `solo_session_store` on SD still has
 * the full record locally.
 *
 * ## Concurrency
 *
 * Caller calls from any context (LVGL or task).  The module
 * snapshots all NVS reads + JSON construction in the caller's
 * thread (cheap) and hands a self-contained PSRAM struct to the
 * worker — the worker never re-reads NVS.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>

/* Best-effort POST of one turn to Dragon's canonical messages DB.
 *
 * @param role        "user" | "assistant" | "system" | "tool"
 * @param content     non-NULL non-empty UTF-8.  Long values OK — the
 *                    module PSRAM-allocates a per-call buffer.
 * @param input_mode  "voice" for SOLO/ONBOARD audio turns,
 *                    "text" for typed turns, NULL → defaults to "text".
 * @return ESP_OK on enqueue, ESP_ERR_INVALID_ARG / _STATE / _NO_MEM
 *         otherwise.  Caller MUST NOT free anything passed in — the
 *         module copies all inputs.
 */
esp_err_t voice_messages_sync_post(const char *role,
                                   const char *content,
                                   const char *input_mode);
