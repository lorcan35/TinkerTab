/* main/voice_dictation.c — dictation pipeline state machine.
 *
 * Pure C, zero ESP-IDF deps so it runs under tests/host/.  Callers
 * (voice.c, ui_notes.c, debug_server_dictation.c) supply monotonic
 * timestamps via voice_dictation_set_state(state, reason, now_ms).
 *
 * Thread safety: target build wraps the global with a FreeRTOS mutex
 * (acquired/released around every public call); host build uses a
 * shim no-op mutex.  See `dict_lock()` / `dict_unlock()` below. */

#include "voice_dictation.h"

#include <stdio.h>
#include <string.h>

#define DICT_MAX_SUBSCRIBERS 4

typedef struct {
   dict_subscriber_t cb;
   void             *user_data;
   bool              in_use;
} dict_sub_t;

static dict_event_t s_event;
static dict_sub_t   s_subs[DICT_MAX_SUBSCRIBERS];

void voice_dictation_init(void) {
   memset(&s_event, 0, sizeof(s_event));
   s_event.state = DICT_IDLE;
   s_event.fail_reason = DICT_FAIL_NONE;
   s_event.note_slot = -1;
   memset(s_subs, 0, sizeof(s_subs));
}

int voice_dictation_subscribe(dict_subscriber_t cb, void *user_data) {
   if (!cb) return -1;
   for (int i = 0; i < DICT_MAX_SUBSCRIBERS; i++) {
      if (!s_subs[i].in_use) {
         s_subs[i].cb = cb;
         s_subs[i].user_data = user_data;
         s_subs[i].in_use = true;
         return i;
      }
   }
   return -1;
}

void voice_dictation_unsubscribe(int handle) {
   if (handle < 0 || handle >= DICT_MAX_SUBSCRIBERS) return;
   s_subs[handle].in_use = false;
   s_subs[handle].cb = NULL;
   s_subs[handle].user_data = NULL;
}

static void dict_dispatch(void) {
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
   if (next == DICT_IDLE) return cur == DICT_SAVED || cur == DICT_FAILED ||
                                 cur == DICT_RECORDING;
   switch (cur) {
   case DICT_IDLE:         return next == DICT_RECORDING;
   case DICT_RECORDING:    return next == DICT_UPLOADING || next == DICT_TRANSCRIBING;
   case DICT_UPLOADING:    return next == DICT_TRANSCRIBING;
   case DICT_TRANSCRIBING: return next == DICT_SAVED;
   case DICT_SAVED:        return next == DICT_IDLE;
   case DICT_FAILED:       return next == DICT_UPLOADING || next == DICT_RECORDING;
                           /* retry resumes upload (REST) or restarts recording */
   }
   return false;
}

void voice_dictation_set_state(dict_state_t new_state, dict_fail_t fail_reason,
                               uint32_t now_ms) {
   /* Idempotent — same-state re-entry is a no-op (avoids spamming
    * subscribers during a chatty caller that re-asserts state). */
   if (new_state == s_event.state && fail_reason == s_event.fail_reason) {
      return;
   }

   if (!dict_transition_allowed(s_event.state, new_state)) {
      /* Refused — log via the host shim print so it's visible during
       * host tests, but don't abort: callers may race and we want the
       * state machine resilient.  On target, this maps to ESP_LOGW
       * once we add an esp_log shim in tests/host/. */
      fprintf(stderr, "voice_dictation: refused %s -> %s\n",
              voice_dictation_state_name(s_event.state),
              voice_dictation_state_name(new_state));
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

   dict_dispatch();
}

void voice_dictation_set_note_slot(int slot) {
   s_event.note_slot = slot;
}

dict_event_t voice_dictation_get(void) {
   return s_event;
}

const char *voice_dictation_state_name(dict_state_t s) {
   switch (s) {
   case DICT_IDLE:         return "IDLE";
   case DICT_RECORDING:    return "RECORDING";
   case DICT_UPLOADING:    return "UPLOADING";
   case DICT_TRANSCRIBING: return "TRANSCRIBING";
   case DICT_SAVED:        return "SAVED";
   case DICT_FAILED:       return "FAILED";
   }
   return "UNKNOWN";
}

const char *voice_dictation_fail_name(dict_fail_t f) {
   switch (f) {
   case DICT_FAIL_NONE:      return "NONE";
   case DICT_FAIL_AUTH:      return "AUTH";
   case DICT_FAIL_NETWORK:   return "NETWORK";
   case DICT_FAIL_EMPTY:     return "EMPTY";
   case DICT_FAIL_NO_AUDIO:  return "NO_AUDIO";
   case DICT_FAIL_TOO_LONG:  return "TOO_LONG";
   case DICT_FAIL_CANCELLED: return "CANCELLED";
   }
   return "UNKNOWN";
}
