/* main/voice_dictation.h — Canonical state machine for a dictation
 * pipeline (RECORDING → UPLOADING → TRANSCRIBING → SAVED, with FAILED
 * as a terminal branch).  Multiple UI surfaces subscribe and render
 * the same state.
 *
 * Designed for host-testability: zero ESP-IDF deps beyond the freertos
 * semphr.h primitives in voice_dictation.c (which the host build shims
 * to no-ops under tests/host/shim/freertos/).  Callers pass monotonic
 * timestamps in ms.  See spec at
 * docs/superpowers/specs/2026-05-14-dictation-pipeline-notes-redesign-design.md
 *
 * THREAD SAFETY:
 *   - All public functions are thread-safe.  The module is protected by
 *     a single FreeRTOS recursive mutex created lazily on first init.
 *     Multiple concurrent FreeRTOS tasks (mic capture, WS event handler,
 *     ui_notes transcription_queue_task, httpd) may call any function
 *     freely.
 *   - Subscribers MAY call `voice_dictation_get()` or
 *     `voice_dictation_set_state()` from within their callback — the
 *     mutex is recursive, so re-entry from the dispatch path is safe.
 *   - Subscribers MUST NOT block indefinitely in their callback.  The
 *     mutex is held for the entire dispatch loop, so a blocking
 *     subscriber blocks every other caller in the system.  Subscribers
 *     should defer real work to a worker task (e.g. tab5_worker).
 *   - Subscribers SHOULD NOT call `voice_dictation_subscribe()` or
 *     `voice_dictation_unsubscribe()` from within a callback — mutating
 *     the subscriber table while the dispatch loop is iterating is
 *     undefined.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   DICT_IDLE = 0,
   DICT_RECORDING,
   DICT_UPLOADING,
   DICT_TRANSCRIBING,
   DICT_SAVED,
   DICT_FAILED,
} dict_state_t;

typedef enum {
   DICT_FAIL_NONE = 0,
   DICT_FAIL_AUTH,        /* Dragon returned 401 / 403 */
   DICT_FAIL_NETWORK,     /* Other HTTP error / WS disconnect / open failed */
   DICT_FAIL_EMPTY,       /* Dragon returned 200 with empty STT text */
   DICT_FAIL_NO_AUDIO,    /* WAV missing or unreadable / Dragon got no PCM */
   DICT_FAIL_TOO_LONG,    /* hit 5-min hard cap during recording */
   DICT_FAIL_CANCELLED,   /* user tapped cancel */
} dict_fail_t;

typedef struct {
   dict_state_t state;
   dict_fail_t  fail_reason;     /* meaningful only when state == DICT_FAILED */
   uint32_t     started_ms;      /* time of last IDLE→RECORDING (0 if never recorded) */
   uint32_t     stopped_ms;      /* time of RECORDING→next (0 while still recording) */
   uint32_t     last_change_ms;  /* time of most recent transition */
   int          note_slot;       /* -1 until SD WAV slot allocated; >=0 after */
} dict_event_t;

typedef void (*dict_subscriber_t)(const dict_event_t *event, void *user_data);

/* Boot-time init.  Idempotent.  Safe to call before FreeRTOS scheduler.
 * Initialises state to DICT_IDLE, clears subscriber list. */
void voice_dictation_init(void);

/* Register a subscriber.  Returns >=0 handle on success, -1 if the
 * subscriber table is full (compile-time constant DICT_MAX_SUBSCRIBERS). */
int voice_dictation_subscribe(dict_subscriber_t cb, void *user_data);

/* Remove a subscriber by handle.  No-op if handle is invalid. */
void voice_dictation_unsubscribe(int handle);

/* Drive a state transition.  Caller supplies monotonic time in ms.
 * Invalid transitions (e.g. SAVED → RECORDING directly) log a warning
 * and are NO-OPs — the state machine never moves backward except via
 * the explicit SAVED→IDLE / FAILED→IDLE retry paths.
 *
 * fail_reason is ignored unless new_state == DICT_FAILED. */
void voice_dictation_set_state(dict_state_t new_state, dict_fail_t fail_reason,
                               uint32_t now_ms);

/* Attach a note slot to the current pipeline (called when SD WAV starts).
 * Persists through subsequent transitions until next IDLE.
 *
 * Contract:
 *   - Only honoured when current state is DICT_RECORDING or DICT_UPLOADING
 *     (the window during which a note slot has meaning).  Outside that
 *     window the call is silently ignored.
 *   - Any `slot < -1` is clamped to -1 (clear).  Negative values other
 *     than -1 are not meaningful and reserved. */
void voice_dictation_set_note_slot(int slot);

/* Snapshot current state.  Thread-safe (returns a copy under lock). */
dict_event_t voice_dictation_get(void);

/* Human-readable state/reason names — useful for logs, debug endpoint,
 * and live verification.  Static strings; do not free. */
const char *voice_dictation_state_name(dict_state_t s);
const char *voice_dictation_fail_name(dict_fail_t f);

#ifdef __cplusplus
}
#endif
