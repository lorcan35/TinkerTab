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

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
    * spinning on a take.
    *
    * IMPORTANT: We do NOT clear s_subs here.  In TinkerTab boot order
    * `ui_home_create` runs on the main task BEFORE the deferred LVGL-
    * timer init callback fires `voice_dictation_init`.  ui_orb_create's
    * pipeline subscription registers via voice_dictation_subscribe()
    * during ui_home_create — wiping s_subs at init time would silently
    * kill any already-registered subscriber and leave pipeline state
    * machine fires unobserved at the UI.  BSS zero-init already gives
    * us an empty subscriber table at first boot; nothing more is needed.
    *
    * s_event is BSS-zero-initialised too, but we leave the explicit
    * field initialisers here (still idempotent — equivalent of a no-op
    * after the first call) so the contract "init starts at DICT_IDLE
    * with note_slot=-1" remains visible at the init site. */
   if (s_lock == NULL) {
      s_lock = xSemaphoreCreateRecursiveMutex();
   }

   dict_lock();
   if (s_event.state == 0 && s_event.note_slot == 0) {
      /* First-init path — BSS-zero state has note_slot=0 which is a
       * valid slot id; fix it to the sentinel.  Re-entry path leaves
       * the existing s_event alone (a dictation might already be in
       * flight if another module called set_state before init). */
      s_event.state = DICT_IDLE;
      s_event.fail_reason = DICT_FAIL_NONE;
      s_event.note_slot = -1;
   }
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
   /* Failures terminate any in-flight state. */
   if (next == DICT_FAILED) return cur != DICT_IDLE;
   /* IDLE accepts from any terminal state. */
   if (next == DICT_IDLE) return cur == DICT_SAVED || cur == DICT_FAILED || cur == DICT_RECORDING;
   switch (cur) {
      case DICT_IDLE:
         return next == DICT_RECORDING;
      case DICT_RECORDING:
         return next == DICT_UPLOADING || next == DICT_TRANSCRIBING;
      case DICT_UPLOADING:
         return next == DICT_TRANSCRIBING;
      case DICT_TRANSCRIBING:
         return next == DICT_SAVED;
      case DICT_SAVED:
         return next == DICT_IDLE || next == DICT_RECORDING;
         /* SAVED is a terminal display state (UI auto-fades to
          * IDLE after ~2 s).  PR 1 doesn't ship a UI subscriber
          * to drive that fade yet, so allow directly starting
          * a new dictation from SAVED without forcing callers
          * to interleave an explicit IDLE transition. */
      case DICT_FAILED:
         return next == DICT_UPLOADING || next == DICT_RECORDING;
         /* retry resumes upload (REST) or restarts recording */
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

   if (new_state == DICT_RECORDING) {
      s_event.started_ms = now_ms;
      s_event.stopped_ms = 0;
   } else if (s_event.state == DICT_RECORDING) {
      /* Leaving RECORDING — capture stop time. */
      s_event.stopped_ms = now_ms;
   }

   if (new_state == DICT_IDLE) {
      /* Reset session-specific fields when fully returning to IDLE. */
      s_event.note_slot = -1;
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
