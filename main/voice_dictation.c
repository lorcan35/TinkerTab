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
#ifndef DICT_HOST_TEST
#include "esp_random.h"
#endif

#define DICT_MAX_SUBSCRIBERS 4

typedef struct {
   dict_subscriber_t cb;
   void *user_data;
   bool in_use;
} dict_sub_t;

static dict_event_t s_event;
static dict_sub_t s_subs[DICT_MAX_SUBSCRIBERS];
static SemaphoreHandle_t s_lock = NULL;

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

   /* W1 late-correction guard: FAILED→SAVED corrects a false-FAIL (a late
    * dictation_summary after a timeout-driven FAILED), but ONLY while a WS
    * resolution is still pending — otherwise a stale summary could resurrect
    * an unrelated FAILED.  (No turn_id echo from Dragon yet in W1; W2's
    * turn_id match in resolve_if_current supersedes this.) */
   if (s_event.state == DICT_FAILED && new_state == DICT_SAVED &&
       !(s_event.resolution_pending && s_event.origin == DICT_ORIGIN_WS)) {
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
   }

   s_event.state = new_state;
   s_event.fail_reason = (new_state == DICT_FAILED) ? fail_reason : DICT_FAIL_NONE;
   s_event.last_change_ms = now_ms;

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
   uint32_t hi = esp_random();
   uint32_t lo = esp_random();
   snprintf(out, DICT_TURN_ID_LEN, "%06lx%06lx", (unsigned long)(hi & 0xFFFFFFu), (unsigned long)(lo & 0xFFFFFFu));
#endif
}

const char *voice_dictation_begin(dict_origin_t origin, const char *adopt_turn_id, uint32_t now_ms) {
   dict_lock();
   /* Refuse if a turn of a DIFFERENT origin is actively in flight (the WS↔
    * offline collision, S1-3).  A settled terminal of the other origin is
    * not "live" — we snap past it below. */
   bool live =
       (s_event.state == DICT_RECORDING || s_event.state == DICT_UPLOADING || s_event.state == DICT_TRANSCRIBING);
   if (live && s_event.origin != DICT_ORIGIN_NONE && s_event.origin != origin) {
      dict_unlock();
      return NULL;
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
