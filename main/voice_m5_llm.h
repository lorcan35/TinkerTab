/**
 * @file voice_m5_llm.h
 * @brief Tab5 ↔ K144 LLM Module sidecar — synchronous, blocking inference
 *        over the stacked M5-Bus UART path.
 *
 * Composition (SOLID, DIP):
 *   uart_port_c    ── byte-stream transport (Phase 1)
 *   m5_stackflow   ── JSON marshalling     (Phase 2)
 *   voice_m5_llm   ── this file: lifecycle + streaming infer (Phase 3)
 *
 * Lifecycle a single `voice_m5_llm_infer` call performs:
 *   1. Lazy `llm.setup` once per session — sends model + params to the K144,
 *      caches the returned `work_id` (e.g. "llm.1000").
 *   2. `llm.inference` with the prompt — reads streaming chunks
 *      (`object == "llm.utf-8.stream"`) and concatenates `delta` strings
 *      into the caller's buffer until `finish == true` or timeout.
 *
 * Heavy synchronous call — TTFT ~600 ms + ~70 ms/token for the bundled
 * `qwen2.5-0.5B-prefill-20e` per Phase 0 bench.  Run from a worker task,
 * NEVER from the LVGL UI task or the voice WS RX task.
 *
 * Phase 4 (failover wiring in voice.c) will dispatch this from the existing
 * `tab5_worker_init()` job queue.
 *
 * See `docs/PLAN-m5-llm-module.md` Phase 3 for acceptance criteria.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send a `sys.ping` to the K144 and wait briefly for the ACK.
 *
 * Cheap — doesn't touch the LLM unit, just confirms the StackFlow daemon
 * is alive and reachable.  Lazy-inits the Port C UART on first call.
 *
 * @return ESP_OK on a clean ack with `error.code == 0`;
 *         ESP_ERR_TIMEOUT on no response within ~500 ms;
 *         ESP_ERR_INVALID_RESPONSE on a parse failure or non-zero error code;
 *         propagated esp_err_t on UART init failure.
 */
esp_err_t voice_m5_llm_probe(void);

/**
 * @brief Generate a reply for @p prompt, append into @p output (NUL-terminated).
 *
 * Performs the full setup→inference→stream-collect loop:
 *   - First call after boot (or after @ref voice_m5_llm_release) issues
 *     `llm.setup` and caches the assigned `work_id`.  Subsequent calls
 *     reuse it.
 *   - Sends `llm.inference` with the prompt as `data` (string form).
 *   - Reads streaming chunks until `finish == true` or @p timeout_s elapses.
 *
 * @param prompt      User-facing prompt text.  Must be non-NULL/non-empty.
 * @param output      Destination buffer; receives concatenated deltas as a
 *                    NUL-terminated UTF-8 string.  Output may be truncated
 *                    if it would exceed @p output_cap; the function still
 *                    drains the stream so the K144 returns to ready state.
 * @param output_cap  Capacity of @p output in bytes (NUL included).
 * @param timeout_s   Hard cap on total operation including setup + streaming.
 *                    Phase 0 measured ~5 s for a 75-token reply on this
 *                    model — pass 30 s as a sane default for short replies.
 *
 * @return ESP_OK on a complete stream (finish==true) within timeout;
 *         ESP_ERR_TIMEOUT if the deadline elapsed before finish;
 *         ESP_ERR_INVALID_ARG on NULL/empty inputs;
 *         ESP_ERR_INVALID_RESPONSE on parse / non-zero error_code from the K144;
 *         propagated esp_err_t on UART or transport failure.
 */
esp_err_t voice_m5_llm_infer(const char *prompt, char *output, size_t output_cap, uint32_t timeout_s);

/**
 * @brief Release any cached `llm.setup` work_id by sending `llm.exit`.
 *
 * Safe to call when no setup is cached (no-op).  Subsequent
 * @ref voice_m5_llm_infer calls will re-setup before inference.
 */
void voice_m5_llm_release(void);

/**
 * @brief Whether a `llm.setup` work_id is currently cached locally.
 *
 * Diagnostic only — does not contact the module.  Consumers should treat
 * the K144 as available iff @ref voice_m5_llm_probe returns ESP_OK.
 */
bool voice_m5_llm_is_ready(void);

#ifdef __cplusplus
}
#endif
