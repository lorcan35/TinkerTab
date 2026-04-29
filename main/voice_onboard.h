/**
 * @file voice_onboard.h
 * @brief Tab5-side glue for the K144 LLM Module (vmode=4 / Onboard).
 *
 * Owns:
 *   - Boot warm-up (probe + one-shot hi-infer to map NPU model)
 *   - Per-text-turn failover (Dragon-down + vmode=4 always-route)
 *   - Autonomous voice-assistant chain (mic → ASR → LLM → per-utterance TTS)
 *   - Chat-overlay mic-tap integration (toggle chain start/stop)
 *
 * Composition (DIP):
 *   voice_m5_llm   ── K144 control plane (UART, StackFlow JSON)
 *   voice_onboard  ── this file: lifecycle + UI binding
 *   voice          ── routes to us when tab5_settings_get_voice_mode() == 4
 *                     OR when Dragon WS is down + grace + vmode=0
 *
 * History: extracted from voice.c in TT #327 Wave 4b.  voice.c had grown
 * to ~4,300 lines mixing 5 voice modes; this module owns vmode=4 + the
 * Local-mode failover-when-WS-down path so future K144 work doesn't
 * accrete to voice.c.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Boot warm-up ─────────────────────────────────────────────────────── */

/** Schedule the one-shot K144 warm-up on the worker queue.  Idempotent;
 *  returns ESP_ERR_INVALID_STATE if already started.  Should be called
 *  once after `tab5_worker_init()` in app_main(). */
esp_err_t voice_onboard_start_warmup(void);

/** Failover gate state.  Returns one of:
 *    0 = UNKNOWN     — warm-up not posted yet
 *    1 = PROBING     — warm-up in flight
 *    2 = READY       — failover available
 *    3 = UNAVAILABLE — probe / warm-up failed
 *  Diagnostic only — do NOT make user-facing decisions on this without
 *  also checking voice_is_connected().  Read by debug server `/m5`,
 *  home-screen pill, and e2e harness. */
int voice_onboard_failover_state(void);

/* ── Per-text-turn failover ───────────────────────────────────────────── */

/** Schedule a single text turn through the K144 (one-shot infer).
 *  Used by voice_send_text:
 *    - vmode=4: always routes here regardless of WS state
 *    - vmode=0: routes here when Dragon WS has been down ≥ M5_FAILOVER_GRACE_MS
 *  Returns ESP_OK if the job was scheduled (queued on tab5_worker), or
 *  ESP_ERR_INVALID_STATE if the failover gate isn't READY, or
 *  ESP_ERR_NO_MEM on alloc / queue-full.  The caller is responsible for
 *  surfacing failures to the user (toast etc.). */
esp_err_t voice_onboard_send_text(const char *text);

/** True iff the per-text-turn failover engaged at least once while the
 *  Dragon WS was down — voice.c uses this on WS reconnect to show a
 *  "Dragon reconnected" toast.  Reading clears the flag so the toast
 *  fires exactly once per disconnect cycle. */
bool voice_onboard_consume_engagement_flag(void);

/* ── Autonomous voice-assistant chain (vmode=4 mic-tap) ───────────────── */

/** Bring up the audio→ASR→LLM chain on the K144 + spawn a drain task on
 *  Tab5.  Returns ESP_OK once the drain task is scheduled (the heavy
 *  ~5-15 sec K144 setup happens inside the drain task so the LVGL caller
 *  doesn't block).  ESP_ERR_INVALID_STATE if a chain is already active.
 *
 *  State transitions:
 *    READY → PROCESSING (chain setup) → LISTENING (chain ready)
 *    LISTENING → READY  (after voice_onboard_chain_stop or 10-min cap) */
esp_err_t voice_onboard_chain_start(void);

/** Signal the chain drain task to stop.  Safe to call when no chain is
 *  active (returns ESP_ERR_INVALID_STATE).  The actual teardown happens
 *  asynchronously inside the drain task; expect the state to return to
 *  READY within ~100-200 ms (or longer if a TTS chunk is mid-playback —
 *  audit #4 follow-up). */
esp_err_t voice_onboard_chain_stop(void);

/** True while the chain drain task is alive (set in chain_start, cleared
 *  in drain task on exit). */
bool voice_onboard_chain_active(void);

#ifdef __cplusplus
}
#endif
