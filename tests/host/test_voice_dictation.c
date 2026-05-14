/* Host-targeted unit tests for main/voice_dictation.c.
 *
 * Pure-C state machine — zero ESP-IDF deps in the module under test;
 * this file uses plain assert.h / stdio just like test_md_strip.c. */

#include "voice_dictation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;

#define CHECK(cond)                                                       \
   do {                                                                   \
      if (!(cond)) {                                                      \
         fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
         return 1;                                                        \
      }                                                                   \
      g_pass++;                                                           \
   } while (0)

#define CHECK_EQ(a, b)                                                              \
   do {                                                                             \
      if ((a) != (b)) {                                                             \
         fprintf(stderr, "FAIL %s:%d  %s != %s  (%d vs %d)\n", __FILE__, __LINE__,  \
                 #a, #b, (int)(a), (int)(b));                                       \
         return 1;                                                                  \
      }                                                                             \
      g_pass++;                                                                     \
   } while (0)

static int test_init_state_is_idle(void) {
   voice_dictation_init();
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_IDLE);
   CHECK_EQ(e.fail_reason, DICT_FAIL_NONE);
   CHECK_EQ(e.note_slot, -1);
   return 0;
}

/* Subscriber that records the last event it saw. */
typedef struct {
   int           call_count;
   dict_event_t  last;
} mock_sub_t;

static void mock_cb(const dict_event_t *e, void *ud) {
   mock_sub_t *m = (mock_sub_t *)ud;
   m->call_count++;
   m->last = *e;
}

static int test_idle_to_recording_fires_subscriber(void) {
   voice_dictation_init();
   mock_sub_t m = {0};
   int h = voice_dictation_subscribe(mock_cb, &m);
   CHECK(h >= 0);

   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 12345);

   CHECK_EQ(m.call_count, 1);
   CHECK_EQ(m.last.state, DICT_RECORDING);
   CHECK_EQ((int)m.last.started_ms, 12345);
   CHECK_EQ((int)m.last.last_change_ms, 12345);

   dict_event_t snapshot = voice_dictation_get();
   CHECK_EQ(snapshot.state, DICT_RECORDING);
   return 0;
}

static int test_full_happy_path(void) {
   voice_dictation_init();
   mock_sub_t m = {0};
   voice_dictation_subscribe(mock_cb, &m);

   voice_dictation_set_state(DICT_RECORDING,    DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING,    DICT_FAIL_NONE, 2000);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2100);
   voice_dictation_set_state(DICT_SAVED,        DICT_FAIL_NONE, 2400);
   voice_dictation_set_state(DICT_IDLE,         DICT_FAIL_NONE, 4400);

   CHECK_EQ(m.call_count, 5);
   CHECK_EQ(m.last.state, DICT_IDLE);
   CHECK_EQ((int)m.last.started_ms, 1000);
   CHECK_EQ((int)m.last.stopped_ms, 2000);
   CHECK_EQ(m.last.note_slot, -1);     /* IDLE reset cleared it */
   return 0;
}

static int test_idempotent_same_state(void) {
   voice_dictation_init();
   mock_sub_t m = {0};
   voice_dictation_subscribe(mock_cb, &m);

   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1500);  /* dup */
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 2000);  /* dup */

   CHECK_EQ(m.call_count, 1);   /* duplicates suppressed */
   return 0;
}

static int test_invalid_backward_transition_blocked(void) {
   voice_dictation_init();
   mock_sub_t m = {0};
   voice_dictation_subscribe(mock_cb, &m);

   /* Walk the pipeline forward to reach SAVED (the strict guard table
    * disallows IDLE → SAVED jumps, so we use the canonical path). */
   voice_dictation_set_state(DICT_RECORDING,    DICT_FAIL_NONE, 100);
   voice_dictation_set_state(DICT_UPLOADING,    DICT_FAIL_NONE, 200);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 300);
   voice_dictation_set_state(DICT_SAVED,        DICT_FAIL_NONE, 400);

   /* Invalid: SAVED → RECORDING (must go through IDLE first). */
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 2000);

   CHECK_EQ(m.last.state, DICT_SAVED);
   return 0;
}

static int test_note_slot_persists_through_pipeline(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_note_slot(7);
   voice_dictation_set_state(DICT_UPLOADING,    DICT_FAIL_NONE, 2000);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2100);
   voice_dictation_set_state(DICT_SAVED,        DICT_FAIL_NONE, 2400);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.note_slot, 7);

   voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, 4400);
   e = voice_dictation_get();
   CHECK_EQ(e.note_slot, -1);  /* cleared on IDLE */
   return 0;
}

int main(void) {
   if (test_init_state_is_idle()) return 1;
   if (test_idle_to_recording_fires_subscriber()) return 1;
   if (test_full_happy_path()) return 1;
   if (test_idempotent_same_state()) return 1;
   if (test_invalid_backward_transition_blocked()) return 1;
   if (test_note_slot_persists_through_pipeline()) return 1;
   fprintf(stderr, "ok  %d checks passed\n", g_pass);
   return 0;
}
