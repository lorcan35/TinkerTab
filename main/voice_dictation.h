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
   DICT_CANCELLED, /* W1: first-class terminal — an intentional cancel, NOT a
                    * failure.  Renders neutral (no "TAP TO RETRY").  Self-decays
                    * to IDLE like the other terminals. */
} dict_state_t;

typedef enum {
   DICT_FAIL_NONE = 0,
   DICT_FAIL_AUTH,     /* Dragon returned 401 / 403 */
   DICT_FAIL_NETWORK,  /* Other HTTP error / WS disconnect / open failed */
   DICT_FAIL_EMPTY,    /* Dragon returned 200 with empty STT text */
   DICT_FAIL_NO_AUDIO, /* WAV missing or unreadable / Dragon got no PCM */
   DICT_FAIL_TOO_LONG, /* recording exceeded the hard cap.  The 4-hr
                        * dictation cap (voice.c MAX_RECORD_FRAMES_DICT,
                        * = 720000 frames; was wrongly documented as
                        * "5-min" pre-2026-05-29) now clean-stops to
                        * SAVED instead of failing, so this reason is
                        * reserved — kept for the reason taxonomy + the
                        * /dictation_pipeline debug contract + orb render. */
   /* W5: DICT_FAIL_CANCELLED removed — cancel is the DICT_CANCELLED *state*
    * (reason DICT_FAIL_NONE) since W1; the reason value had no producers. */
} dict_fail_t;

/* Which producer owns the in-flight dictation turn.  One global FSM is
 * shared by the live WS path and the offline REST queue; origin tags
 * ownership so they can refuse each other (S1-3) instead of clobbering one
 * note_slot field. */
typedef enum {
   DICT_ORIGIN_NONE = 0,
   DICT_ORIGIN_WS,      /* live dictation over the voice WebSocket */
   DICT_ORIGIN_OFFLINE, /* Core-1 transcription_queue_task re-upload (REST) */
} dict_origin_t;

/* turn_id is 12 hex chars + NUL.  MUST equal voice.c's TURN_ID_LEN — a
 * _Static_assert in voice.c guards the two stay equal. */
#define DICT_TURN_ID_LEN 13
/* Dragon note id once known (W4).  Provisional bound, re-pinned against the
 * longest real note id when W4 lands. */
#define DICT_NOTE_ID_LEN 40

typedef struct {
   dict_state_t state;
   dict_fail_t fail_reason; /* meaningful only when state == DICT_FAILED */
   uint32_t started_ms;     /* time of last IDLE→RECORDING (0 if never recorded) */
   uint32_t stopped_ms;     /* time of RECORDING→next (0 while still recording) */
   uint32_t last_change_ms; /* time of most recent transition */
   int note_slot;           /* -1 until SD WAV slot allocated; >=0 after */
   /* --- W1: session identity + self-liveness --- */
   dict_origin_t origin;           /* producer that owns this turn */
   char turn_id[DICT_TURN_ID_LEN]; /* "" until a turn begins */
   bool resolution_pending;        /* a resolution is in flight on this turn's
                                    * transport; gates terminal self-decay so a
                                    * 60-90s-late summary can still correct it */
   char note_id[DICT_NOTE_ID_LEN]; /* Dragon note id once known; "" otherwise (W4) */
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
void voice_dictation_set_state(dict_state_t new_state, dict_fail_t fail_reason, uint32_t now_ms);

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

/* Lock-free read of JUST the FSM state enum — no mutex, never blocks.
 * MUST be used (instead of voice_dictation_get().state) by any caller in a
 * render-hot-path or high-frequency poll loop: the orb paint (every LVGL
 * frame), the vision task loop, the wake-stream pump.  voice_dictation_get()
 * acquires the recursive mutex with portMAX_DELAY; taking it every frame wedged
 * ui_task on the dictation lock during the dictation-stop contention burst and
 * tripped the task-WDT (2026-05-30 coredump). */
dict_state_t voice_dictation_state(void);

/* W4: pure predicate — does this FSM state mean "capture is actively running
 * and should hold the orb"?  RECORDING only.  At stop the FSM moves to
 * TRANSCRIBING/SAVED/etc. (background note states), so the orb releases and
 * snaps back to idle while enrichment continues in the Notes badge.
 * Lock-free + pure; safe from the orb paint hot-path. */
bool voice_dictation_orb_active(dict_state_t s);

/* Human-readable state/reason names — useful for logs, debug endpoint,
 * and live verification.  Static strings; do not free. */
const char *voice_dictation_state_name(dict_state_t s);
const char *voice_dictation_fail_name(dict_fail_t f);

/* User-facing caption for a fail reason (the single source replacing the
 * divergent per-surface strings — S2-11).  Pure; static strings.  The
 * state-specific suffix ("TAP TO RETRY" etc.) is appended by the renderer,
 * not baked in here. */
const char *voice_dictation_fail_caption(dict_fail_t f);

/* The ONE monotonic-ms clock for the FSM.  On target = esp_timer_get_time()/
 * 1000; under host tests the esp_timer shim returns the fake clock, so the
 * same body serves both.  Used by self-decay, resolve callers, and (W5) the
 * inlined timestamp sites. */
uint32_t voice_dictation_now_ms(void);

/* Write a fresh 12-hex-char + NUL turn id into `out` (>= DICT_TURN_ID_LEN).
 * Pure (no globals); voice.c's gen_turn_id delegates here so there is ONE
 * generator and one id per turn. */
void voice_turn_id_gen(char out[DICT_TURN_ID_LEN]);

/* Begin a WS dictation turn: mint identity + transition IDLE→RECORDING in one
 * locked op.  `adopt_turn_id` NULL = mint a fresh id; non-NULL = adopt it.
 * Returns a pointer to the FSM's turn_id (valid until the next IDLE), or NULL
 * if a turn of a DIFFERENT origin is already live (caller should toast busy). */
const char *voice_dictation_begin(dict_origin_t origin, const char *adopt_turn_id, uint32_t now_ms);

/* Atomic claim for the offline REST queue: if (and only if) the FSM is IDLE
 * or in a terminal state, adopt `turn_id`, set origin=OFFLINE + note_slot,
 * mark resolution_pending, and transition to UPLOADING — all under one lock
 * take.  Returns false (no state change) if a turn is live.  Closes the
 * queue↔decay and WS↔offline races (S1-3). */
bool voice_dictation_try_begin_offline(const char *adopt_turn_id, int note_slot, uint32_t now_ms);

/* Apply a terminal/resolution transition ONLY if `turn_id` matches the live
 * turn.  A missing/empty `turn_id` is treated as a MATCH (forward-compat with
 * an old Dragon that does not echo turn_id yet — W1; W2 makes the echo
 * authoritative and closes the late-summary-for-the-wrong-turn class, S2-9).
 * Returns true iff the transition was applied. */
bool voice_dictation_resolve_if_current(const char *turn_id, dict_state_t st, dict_fail_t reason, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
