/**
 * @file voice_wakeword.h
 * @brief Always-on K144 ASR listener with phrase-match wakeword AND
 *        on-device long-form dictation in one task.
 *
 * Architecture (TT wakeword revival, 2026-05-17):
 *
 *   K144: audio.setup → asr.setup (sherpa-ncnn-streaming-zipformer-20M)
 *      └── streams asr.utf-8.stream {delta, finish} frames over UART
 *
 *   Tab5 task drains partials and runs a small state machine:
 *
 *      IDLE ─── partial transcript contains wake phrase
 *               (case-insensitive substring on accumulated segment text) ──┐
 *                                                                          ▼
 *                                                              WAKE callback fires
 *                                                                          │
 *                                                                          ▼
 *      LISTENING ── each partial appended to dictation buffer
 *                   exit on any of:
 *                     - end-phrase matched ("save", "done", "stop")
 *                     - segment finish AND ≥`silence_segments` since last delta
 *                     - hard timeout (default 4 hours, configurable)
 *                                                                          │
 *                                                                          ▼
 *                                                          FINAL callback fires with
 *                                                          accumulated text; back to IDLE
 *
 * The KWS unit was tried first (it's officially listed) but its
 * parse_config rejects every body shape on the current K144 firmware.
 * ASR-based phrase matching is the practical path: known-good model,
 * open-vocabulary at runtime, gives us full transcript "for free" so
 * dictation slots in on the same stream.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   VOICE_WAKEWORD_EVENT_WAKE,              /**< wake phrase matched */
   VOICE_WAKEWORD_EVENT_DICTATION_PARTIAL, /**< new transcript chunk during LISTENING */
   VOICE_WAKEWORD_EVENT_DICTATION_FINAL,   /**< end-of-utterance — full text in @p text */
   VOICE_WAKEWORD_EVENT_TRANSCRIPT,        /**< background ASR partial OUTSIDE wake (debug-only) */
} voice_wakeword_event_t;

/**
 * @brief Wakeword/dictation event callback.  Fires from the wakeword
 *        background task — caller is responsible for any LVGL bridging.
 *
 * @param event  Event kind.
 * @param text   Wake event: matched phrase fragment.  Dictation events:
 *               the live partial OR the final accumulated transcript.
 *               Pointer valid only during the call.
 * @param user   Caller-supplied opaque.
 */
typedef void (*voice_wakeword_cb_t)(voice_wakeword_event_t event, const char *text, void *user);

typedef struct {
   /** Wake phrase, case-insensitive substring match.  Default "tinker"
    *  if NULL.  Pointer must remain valid for the lifetime of the task. */
   const char *wake_phrase;
   /** End-of-dictation phrase, case-insensitive.  Default "save note" if
    *  NULL.  Empty string disables phrase-based stop (rely on silence /
    *  timeout). */
   const char *end_phrase;
   /** Max dictation buffer size in bytes (excluding terminator).  Default
    *  32 KB if 0. */
   size_t dictation_buf_bytes;
   /** Silent finish-segments before auto-stop during dictation.  Default
    *  3 (≈4-8 s of silence depending on K144 endpointer rules). */
   uint8_t silence_segments_to_stop;
   /** Hard dictation timeout in seconds; 0 = no timeout.  Default 14400
    *  (4 hours) to match Tab5's existing meeting-length cap (#573). */
   uint32_t dictation_timeout_s;
   /** Forward every background ASR partial (outside dictation) as
    *  VOICE_WAKEWORD_EVENT_TRANSCRIPT.  Verbose; off by default. */
   bool emit_background_transcripts;
} voice_wakeword_config_t;

/**
 * @brief Start the always-on K144 ASR + Tab5 phrase-match task.
 *
 * Idempotent: returns ESP_ERR_INVALID_STATE if already running.
 *
 * @param cfg   Tunables.  Pass NULL for defaults.  Fields with default
 *              values are documented in @ref voice_wakeword_config_t.
 * @param cb    Event callback.  May be NULL.
 * @param user  Opaque passed to @p cb.
 */
esp_err_t voice_wakeword_start(const voice_wakeword_config_t *cfg, voice_wakeword_cb_t cb, void *user);

/** @brief Stop the task + tear down the K144 chain.  Safe when not
 *         running. */
void voice_wakeword_stop(void);

/** @brief Whether the listener is live (chain up + task running). */
bool voice_wakeword_is_active(void);

/** @brief Force a dictation stop NOW from outside the task.  No-op when
 *         not in the LISTENING state.  Fires the DICTATION_FINAL event
 *         with whatever has been accumulated so far. */
void voice_wakeword_force_dictation_stop(void);

/* ── Status accessors (TT #578 — TinkerON debug surface) ──────────── */

/** @brief Snapshot of the listener's runtime state.  All fields are
 *         output-only; pass storage from the caller. */
typedef struct {
   bool armed;                /**< true iff the task is running */
   char wake_phrase[64];      /**< configured wake phrase (e.g. "hey tinker") */
   char wake_phrase_alt[64];  /**< auto-derived T→Th variant (or "") */
   char end_phrase[64];       /**< end-of-dictation phrase */
   uint32_t fire_count;       /**< number of WAKE events fired since arm */
   int64_t last_fire_ms;      /**< esp_timer ms at last wake (0 if never) */
   char last_match[64];       /**< phrase that matched on the last wake */
} voice_wakeword_status_t;

/** @brief Fill @p out with the current listener status.  All fields are
 *         populated even when the listener is not armed (armed=false +
 *         cached config values from the last start). */
void voice_wakeword_status(voice_wakeword_status_t *out);

/** @brief Restart the listener with a new wake phrase at runtime.
 *
 *  Convenience for live A/B testing of phonetic variants without a
 *  reflash.  Internally: stop → cache new phrase → start with the
 *  same callback + user pointer that were registered on the last
 *  successful start.  Returns ESP_ERR_INVALID_STATE if the listener
 *  has never been started.  On failure the previous phrase is NOT
 *  preserved — caller is responsible for re-issuing start with the
 *  known-good phrase. */
esp_err_t voice_wakeword_reconfigure_phrase(const char *new_phrase);

/** @brief Snapshot one ASR transcript ring-buffer entry. */
typedef struct {
   int64_t ms;                /**< esp_timer ms when this delta arrived */
   bool finish;               /**< whether the K144 marked this as a finish segment */
   char text[96];             /**< delta payload (silently truncated) */
} voice_wakeword_transcript_t;

/** @brief Copy up to @p max entries (newest-last) into @p out.  Returns
 *         the number actually filled.  Ring buffer is 32 entries; older
 *         deltas are evicted FIFO.  Useful for debug tail. */
size_t voice_wakeword_get_recent_transcripts(voice_wakeword_transcript_t *out, size_t max);

#ifdef __cplusplus
}
#endif
