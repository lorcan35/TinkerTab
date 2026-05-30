/* main/voice_dictation.c — dictation pipeline state machine.
 *
 * Pure C, zero ESP-IDF deps beyond the freertos/semphr.h primitives,
 * which the host build shims to no-ops under tests/host/shim/freertos/.
 * Callers (voice.c, ui_notes.c, debug_server_dictation.c) supply
 * monotonic timestamps via voice_dictation_set_state(state, reason,
 * now_ms).
 *
 * Thread safety: target build wraps every public call with a FreeRTOS
 * recursive mutex (lazily created in voice_dictation_init()); host
 * build uses the no-op shim in tests/host/shim/freertos/semphr.h.
 * The mutex is recursive because dispatching to subscribers may re-
 * enter via voice_dictation_get() or voice_dictation_set_state() — a
 * documented and supported pattern.  See `dict_lock()` /
 * `dict_unlock()` below. */

#include "voice_dictation.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "task_worker.h"
#ifndef DICT_HOST_TEST
#include "esp_random.h"
#endif

#define DICT_MAX_SUBSCRIBERS 4

/* W1 self-liveness: terminal states decay back to IDLE on their own so a
 * dictation no longer depends on whichever UI surface happens to be mounted
 * to make progress (S2-1).  The decay job runs on task_worker — NEVER from
 * the esp_timer task — because the dispatch path marshals to LVGL, and the
 * timer task is not a safe context for that (the crash class PR #259 closed). */
#define DICT_DECAY_SAVED_MS 2000
#define DICT_DECAY_FAILED_MS 5000
#define DICT_DECAY_CANCELLED_MS 1500
#define DICT_DECAY_RETRY_MS 250 /* re-arm delay when the worker queue is full */

#define DICT_IS_TERMINAL(s) ((s) == DICT_SAVED || (s) == DICT_FAILED || (s) == DICT_CANCELLED)

typedef struct {
   dict_subscriber_t cb;
   void *user_data;
   bool in_use;
} dict_sub_t;

static dict_event_t s_event;
static dict_sub_t s_subs[DICT_MAX_SUBSCRIBERS];
static SemaphoreHandle_t s_lock = NULL;
static esp_timer_handle_t s_dict_decay_timer = NULL; /* one-shot, lazily created */

/* Lock / unlock the module-wide recursive mutex.  On host these collapse
 * to no-ops via the semphr.h shim, so the test suite exercises the same
 * code path without any compile-time switch. */
static inline void dict_lock(void) {
   if (s_lock) {
      xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
   }
}
static inline void dict_unlock(void) {
   if (s_lock) {
      xSemaphoreGiveRecursive(s_lock);
   }
}

void voice_dictation_init(void) {
   /* Lazily create the mutex on first call.  Subsequent calls reuse the
    * existing handle so init remains idempotent.  We deliberately do NOT
    * destroy + recreate because callers in other tasks may already be
    * spinning on a take. */
   if (s_lock == NULL) {
      s_lock = xSemaphoreCreateRecursiveMutex();
   }

   dict_lock();
   memset(&s_event, 0, sizeof(s_event));
   s_event.state = DICT_IDLE;
   s_event.fail_reason = DICT_FAIL_NONE;
   s_event.note_slot = -1;
   memset(s_subs, 0, sizeof(s_subs));
   dict_unlock();
}

int voice_dictation_subscribe(dict_subscriber_t cb, void *user_data) {
   if (!cb) return -1;
   dict_lock();
   int handle = -1;
   for (int i = 0; i < DICT_MAX_SUBSCRIBERS; i++) {
      if (!s_subs[i].in_use) {
         s_subs[i].cb = cb;
         s_subs[i].user_data = user_data;
         s_subs[i].in_use = true;
         handle = i;
         break;
      }
   }
   dict_unlock();
   return handle;
}

void voice_dictation_unsubscribe(int handle) {
   if (handle < 0 || handle >= DICT_MAX_SUBSCRIBERS) return;
   dict_lock();
   s_subs[handle].in_use = false;
   s_subs[handle].cb = NULL;
   s_subs[handle].user_data = NULL;
   dict_unlock();
}

/* MUST be called with dict_lock() held.  Iterates the subscriber table
 * and fires every active callback with the current `s_event`.  The lock
 * remains held during dispatch — subscribers may re-enter (it's a
 * recursive mutex) but they may not mutate the subscriber table. */
static void dict_dispatch_locked(void) {
   for (int i = 0; i < DICT_MAX_SUBSCRIBERS; i++) {
      if (s_subs[i].in_use && s_subs[i].cb) {
         s_subs[i].cb(&s_event, s_subs[i].user_data);
      }
   }
}

/* Worker-context job: decay a still-terminal, non-pending state to IDLE.
 *
 * The re-check and the IDLE write happen under a SINGLE lock hold (review F1):
 * the worker runs on Core-1 while voice_dictation_begin runs on the UI/WS
 * task, so a get()-then-separate-set_state() split is a genuine multicore
 * TOCTOU — begin() could snap a terminal→IDLE→RECORDING (a fresh live turn)
 * in the gap, and the stale IDLE write would then wipe it (RECORDING→IDLE is
 * a legal edge).  One lock hold makes the job's view atomic w.r.t. begin:
 * begin() blocks on the same lock, so it runs strictly before or after this
 * job, never between its check and its write.  set_state re-enters the
 * recursive mutex — safe. */
static void dict_decay_apply_job(void *arg) {
   (void)arg;
   dict_lock();
   if (DICT_IS_TERMINAL(s_event.state) && !s_event.resolution_pending) {
      voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, voice_dictation_now_ms());
   }
   dict_unlock();
}

/* esp_timer-task callback.  Does NOT touch the dispatch path — it only hands
 * the decay off to the shared worker (the same context existing producers
 * use).  On a full worker queue it re-arms a short retry so the terminal
 * still decays rather than wedging (no headless block). */
static void dict_decay_timer_cb(void *arg) {
   (void)arg;
   if (tab5_worker_enqueue(dict_decay_apply_job, NULL, "dict_decay") != ESP_OK) {
      if (s_dict_decay_timer) {
         esp_timer_start_once(s_dict_decay_timer, (uint64_t)DICT_DECAY_RETRY_MS * 1000);
      }
   }
}

/* MUST hold dict_lock().  Arm the one-shot for the current terminal state's
 * decay delay, unless a resolution is still pending (then leave disarmed so
 * the terminal persists until the real resolution lands).  Disarms for
 * non-terminal states. */
static void dict_arm_decay_locked(void) {
   if (!s_dict_decay_timer) {
      const esp_timer_create_args_t args = {
          .callback = dict_decay_timer_cb,
          .name = "dict_decay",
      };
      if (esp_timer_create(&args, &s_dict_decay_timer) != ESP_OK) return;
   }
   esp_timer_stop(s_dict_decay_timer); /* re-arm cleanly */
   if (!DICT_IS_TERMINAL(s_event.state) || s_event.resolution_pending) return;
   uint32_t ms = DICT_DECAY_SAVED_MS;
   if (s_event.state == DICT_FAILED) {
      ms = DICT_DECAY_FAILED_MS;
   } else if (s_event.state == DICT_CANCELLED) {
      ms = DICT_DECAY_CANCELLED_MS;
   }
   esp_timer_start_once(s_dict_decay_timer, (uint64_t)ms * 1000);
}

/* Return true if going from `cur` to `next` is a valid transition.
 * Forward through the pipeline + any → FAILED + SAVED/FAILED/IDLE → IDLE.
 * Bounces (e.g. RECORDING → RECORDING) are filtered by the dup check
 * in set_state itself, so we don't list them here. */
static bool dict_transition_allowed(dict_state_t cur, dict_state_t next) {
   /* CANCELLED terminates an in-flight (non-IDLE, non-terminal) turn. */
   if (next == DICT_CANCELLED) return cur == DICT_RECORDING || cur == DICT_UPLOADING || cur == DICT_TRANSCRIBING;
   /* FAILED terminates any in-flight state (but not the other terminals). */
   if (next == DICT_FAILED) return cur != DICT_IDLE && cur != DICT_SAVED && cur != DICT_CANCELLED;
   /* IDLE accepts from any terminal (self-decay path) + RECORDING (cancel). */
   if (next == DICT_IDLE)
      return cur == DICT_SAVED || cur == DICT_FAILED || cur == DICT_CANCELLED || cur == DICT_RECORDING;
   switch (cur) {
      case DICT_IDLE:
         /* RECORDING = WS begin; UPLOADING = offline begin (no RECORDING phase,
          * the WAV already exists). */
         return next == DICT_RECORDING || next == DICT_UPLOADING;
      case DICT_RECORDING:
         return next == DICT_UPLOADING || next == DICT_TRANSCRIBING;
      case DICT_UPLOADING:
         /* SAVED added: the offline path resolves straight from UPLOADING. */
         return next == DICT_TRANSCRIBING || next == DICT_SAVED;
      case DICT_TRANSCRIBING:
         return next == DICT_SAVED;
      case DICT_FAILED:
         /* SAVED = late-correction of a false-FAIL (S2-2); the W1 semantic
          * guard (resolution_pending && origin==WS) is enforced in set_state.
          * UPLOADING/RECORDING = retry.  SAVED→RECORDING was removed (S3-5):
          * self-decay reaches IDLE first, so a re-dictate goes IDLE→RECORDING. */
         return next == DICT_SAVED || next == DICT_UPLOADING || next == DICT_RECORDING;
      case DICT_SAVED:
      case DICT_CANCELLED:
         /* Terminal display states: only → IDLE (handled above, via self-decay). */
         return false;
   }
   return false;
}

void voice_dictation_set_state(dict_state_t new_state, dict_fail_t fail_reason, uint32_t now_ms) {
   dict_lock();

   /* Idempotent — same-state re-entry is a no-op (avoids spamming
    * subscribers during a chatty caller that re-asserts state). */
   if (new_state == s_event.state && fail_reason == s_event.fail_reason) {
      dict_unlock();
      return;
   }

   if (!dict_transition_allowed(s_event.state, new_state)) {
      /* Refused — log via the host shim print so it's visible during
       * host tests, but don't abort: callers may race and we want the
       * state machine resilient.  On target, this maps to ESP_LOGW
       * once we add an esp_log shim in tests/host/. */
      fprintf(stderr, "voice_dictation: refused %s -> %s\n", voice_dictation_state_name(s_event.state),
              voice_dictation_state_name(new_state));
      dict_unlock();
      return;
   }

   /* Late-correction guard: FAILED→SAVED corrects a false-FAIL (a late
    * dictation_summary after a timeout-driven FAILED), but only while a
    * resolution is still pending — a SAVED with no pending resolution is a
    * stray.  W2: the cross-turn safety is now the turn_id match in
    * voice_dictation_resolve_if_current (the only caller that should drive
    * FAILED→SAVED), so the W1 origin==WS qualifier is dropped. */
   if (s_event.state == DICT_FAILED && new_state == DICT_SAVED && !s_event.resolution_pending) {
      dict_unlock();
      return;
   }

   if (new_state == DICT_RECORDING) {
      s_event.started_ms = now_ms;
      s_event.stopped_ms = 0;
   } else if (s_event.state == DICT_RECORDING) {
      /* Leaving RECORDING — capture stop time. */
      s_event.stopped_ms = now_ms;
   }

   /* A WS turn entering TRANSCRIBING means Tab5 has sent `stop` and is now
    * awaiting Dragon's terminal frame.  Mark resolution_pending so a terminal
    * reached by some OTHER path (e.g. a future timeout-driven FAILED) does
    * not self-decay before the real summary lands and corrects it. */
   if (new_state == DICT_TRANSCRIBING && s_event.origin == DICT_ORIGIN_WS) {
      s_event.resolution_pending = true;
   }

   if (new_state == DICT_IDLE) {
      /* Full session reset on return to IDLE. */
      s_event.note_slot = -1;
      s_event.origin = DICT_ORIGIN_NONE;
      s_event.turn_id[0] = '\0';
      s_event.resolution_pending = false;
      s_event.note_id[0] = '\0';
   } else if (new_state == DICT_SAVED || new_state == DICT_CANCELLED) {
      /* A definitive resolution landed — clear pending so the terminal can
       * self-decay. */
      s_event.resolution_pending = false;
   } else if (new_state == DICT_FAILED && s_event.origin != DICT_ORIGIN_WS) {
      /* Non-WS (offline REST) FAILED is definitive — the REST response IS the
       * resolution, so clear pending and let it decay.  A WS FAILED instead
       * KEEPS pending: it may be premature (a timeout/grace FAILED) and a
       * 60-90s-late summary can still correct it via FAILED→SAVED before it
       * would otherwise decay.  (W3's stuck-watchdog bounds the WS case.) */
      s_event.resolution_pending = false;
   }

   s_event.state = new_state;
   s_event.fail_reason = (new_state == DICT_FAILED) ? fail_reason : DICT_FAIL_NONE;
   s_event.last_change_ms = now_ms;

   /* Self-liveness: arm a one-shot to decay this terminal back to IDLE (unless
    * a resolution is still pending); disarm for non-terminal states. */
   dict_arm_decay_locked();

   dict_dispatch_locked();
   dict_unlock();
}

void voice_dictation_set_note_slot(int slot) {
   /* Clamp any "out-of-band negative" to the canonical sentinel. */
   if (slot < -1) slot = -1;

   dict_lock();
   /* Only honour during the window where a note_slot has meaning. */
   if (s_event.state == DICT_RECORDING || s_event.state == DICT_UPLOADING) {
      s_event.note_slot = slot;
   }
   dict_unlock();
}

uint32_t voice_dictation_now_ms(void) {
   /* One clock for the whole FSM.  On target esp_timer_get_time() is the
    * microsecond monotonic counter; under host tests the esp_timer shim
    * returns the test-driven fake clock — same body serves both. */
   return (uint32_t)(esp_timer_get_time() / 1000);
}

void voice_turn_id_gen(char out[DICT_TURN_ID_LEN]) {
   if (!out) return;
#ifdef DICT_HOST_TEST
   /* Deterministic monotonic ids under host tests so assertions are stable. */
   static unsigned s_seq = 0;
   snprintf(out, DICT_TURN_ID_LEN, "%012x", ++s_seq);
#else
   /* Same shape voice.c used before W1 (12 hex / 48 bits): 8 hex from one
    * esp_random + 4 from another.  The wire format is unchanged. */
   uint32_t a = esp_random();
   uint32_t b = esp_random();
   snprintf(out, DICT_TURN_ID_LEN, "%08lx%04lx", (unsigned long)a, (unsigned long)(b & 0xFFFFu));
#endif
}

const char *voice_dictation_begin(dict_origin_t origin, const char *adopt_turn_id, uint32_t now_ms) {
   dict_lock();
   /* A turn already in flight (RECORDING/UPLOADING/TRANSCRIBING).  A settled
    * terminal is NOT live — we snap past it below. */
   bool live =
       (s_event.state == DICT_RECORDING || s_event.state == DICT_UPLOADING || s_event.state == DICT_TRANSCRIBING);
   if (live && s_event.origin != DICT_ORIGIN_NONE) {
      /* DIFFERENT origin → refuse (the WS↔offline collision, S1-3).  SAME
       * origin → re-entrant begin, not a new turn: return the EXISTING turn_id
       * WITHOUT re-minting (review F5 — re-minting would orphan the id that
       * W2's resolve-by-id authority depends on). */
      const char *ret = (s_event.origin == origin) ? s_event.turn_id : NULL;
      dict_unlock();
      return ret;
   }
   /* Fast re-tap before a terminal has self-decayed: snap to IDLE first so
    * the fresh turn starts clean (SAVED→RECORDING was removed in W1). */
   if (s_event.state == DICT_SAVED || s_event.state == DICT_FAILED || s_event.state == DICT_CANCELLED) {
      voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, now_ms);
   }
   if (adopt_turn_id && adopt_turn_id[0]) {
      strncpy(s_event.turn_id, adopt_turn_id, DICT_TURN_ID_LEN - 1);
      s_event.turn_id[DICT_TURN_ID_LEN - 1] = '\0';
   } else {
      voice_turn_id_gen(s_event.turn_id);
   }
   s_event.origin = origin;
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, now_ms);
   const char *ret = (s_event.state == DICT_RECORDING) ? s_event.turn_id : NULL;
   dict_unlock();
   return ret;
}

bool voice_dictation_try_begin_offline(const char *adopt_turn_id, int note_slot, uint32_t now_ms) {
   dict_lock();
   /* Claim only if no turn is in flight: IDLE or a settled terminal. */
   bool claimable = (s_event.state == DICT_IDLE || s_event.state == DICT_SAVED || s_event.state == DICT_FAILED ||
                     s_event.state == DICT_CANCELLED);
   if (!claimable) {
      dict_unlock();
      return false;
   }
   if (s_event.state != DICT_IDLE) {
      voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, now_ms);
   }
   if (adopt_turn_id && adopt_turn_id[0]) {
      strncpy(s_event.turn_id, adopt_turn_id, DICT_TURN_ID_LEN - 1);
      s_event.turn_id[DICT_TURN_ID_LEN - 1] = '\0';
   } else {
      voice_turn_id_gen(s_event.turn_id);
   }
   s_event.origin = DICT_ORIGIN_OFFLINE;
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, now_ms);
   if (s_event.state != DICT_UPLOADING) {
      dict_unlock();
      return false;
   }
   /* The offline path has no Dragon WS terminal — the REST response is the
    * resolution, so mark it pending here (cleared by the queue when the HTTP
    * response applies the terminal in W4). */
   if (note_slot < -1) note_slot = -1;
   s_event.note_slot = note_slot;
   s_event.resolution_pending = true;
   dict_unlock();
   return true;
}

bool voice_dictation_resolve_if_current(const char *turn_id, dict_state_t st, dict_fail_t reason, uint32_t now_ms) {
   dict_lock();
   /* Missing/empty incoming id = match (old Dragon, no echo yet — W1).
    * Otherwise it must equal the live turn's id. */
   bool match =
       (!turn_id || turn_id[0] == '\0' || (s_event.turn_id[0] != '\0' && strcmp(turn_id, s_event.turn_id) == 0));
   if (!match) {
      dict_unlock();
      return false;
   }
   /* set_state owns the per-terminal pending bookkeeping (SAVED/CANCELLED
    * clear it; FAILED keeps it) and the FAILED→SAVED late-correction guard. */
   voice_dictation_set_state(st, reason, now_ms);
   bool applied = (s_event.state == st);
   dict_unlock();
   return applied;
}

dict_event_t voice_dictation_get(void) {
   dict_lock();
   dict_event_t snapshot = s_event;
   dict_unlock();
   return snapshot;
}

/* Pure-function name lookups — no shared state touched, no lock needed.
 * Skipping the lock here is the deliberate choice: callers from log/
 * diagnostic paths shouldn't pay a mutex-take just to map an enum to
 * a static string literal. */
const char *voice_dictation_state_name(dict_state_t s) {
   switch (s) {
      case DICT_IDLE:
         return "IDLE";
      case DICT_RECORDING:
         return "RECORDING";
      case DICT_UPLOADING:
         return "UPLOADING";
      case DICT_TRANSCRIBING:
         return "TRANSCRIBING";
      case DICT_SAVED:
         return "SAVED";
      case DICT_FAILED:
         return "FAILED";
      case DICT_CANCELLED:
         return "CANCELLED";
   }
   return "UNKNOWN";
}

const char *voice_dictation_fail_name(dict_fail_t f) {
   switch (f) {
      case DICT_FAIL_NONE:
         return "NONE";
      case DICT_FAIL_AUTH:
         return "AUTH";
      case DICT_FAIL_NETWORK:
         return "NETWORK";
      case DICT_FAIL_EMPTY:
         return "EMPTY";
      case DICT_FAIL_NO_AUDIO:
         return "NO_AUDIO";
      case DICT_FAIL_TOO_LONG:
         return "TOO_LONG";
      case DICT_FAIL_CANCELLED:
         return "CANCELLED";
   }
   return "UNKNOWN";
}

const char *voice_dictation_fail_caption(dict_fail_t f) {
   /* Single source for the user-facing reason string (S2-11).  The renderer
    * appends any state suffix ("  TAP TO RETRY", "  Cancelled"); it is not
    * baked in here. */
   switch (f) {
      case DICT_FAIL_NONE:
         return "";
      case DICT_FAIL_AUTH:
         return "Not authorized";
      case DICT_FAIL_NETWORK:
         return "Couldn't reach Dragon";
      case DICT_FAIL_EMPTY:
         return "Nothing heard";
      case DICT_FAIL_NO_AUDIO:
         return "No audio captured";
      case DICT_FAIL_TOO_LONG:
         return "Recording too long";
      case DICT_FAIL_CANCELLED:
         return "Cancelled";
   }
   return "Failed";
}
