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

static int test_failed_from_uploading_auth(void) {
   voice_dictation_init();
   mock_sub_t m = {0};
   voice_dictation_subscribe(mock_cb, &m);

   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 2000);
   voice_dictation_set_state(DICT_FAILED,    DICT_FAIL_AUTH, 2100);

   CHECK_EQ(m.last.state, DICT_FAILED);
   CHECK_EQ(m.last.fail_reason, DICT_FAIL_AUTH);
   return 0;
}

static int test_failed_from_transcribing_empty(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING,    DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING,    DICT_FAIL_NONE, 2000);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2100);
   voice_dictation_set_state(DICT_FAILED,       DICT_FAIL_EMPTY, 2400);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_FAILED);
   CHECK_EQ(e.fail_reason, DICT_FAIL_EMPTY);
   return 0;
}

static int test_retry_from_failed(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_FAILED,    DICT_FAIL_NETWORK, 1100);
   /* User taps retry → caller drives UPLOADING. */
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 5000);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_UPLOADING);
   CHECK_EQ(e.fail_reason, DICT_FAIL_NONE);   /* cleared on leave-FAILED */
   return 0;
}

static int test_cancel_from_recording(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_CANCELLED, 1500);
   voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, 1600);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_IDLE);
   CHECK_EQ(e.fail_reason, DICT_FAIL_NONE);
   return 0;
}

static int test_failed_clears_reason_on_idle(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_AUTH, 2000);
   voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, 3000);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.fail_reason, DICT_FAIL_NONE);
   return 0;
}

static int test_multiple_subscribers_all_fire(void) {
   voice_dictation_init();
   mock_sub_t a = {0}, b = {0};
   voice_dictation_subscribe(mock_cb, &a);
   voice_dictation_subscribe(mock_cb, &b);

   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);

   CHECK_EQ(a.call_count, 1);
   CHECK_EQ(b.call_count, 1);
   return 0;
}

static int test_unsubscribe_stops_callbacks(void) {
   voice_dictation_init();
   mock_sub_t a = {0}, b = {0};
   int ha = voice_dictation_subscribe(mock_cb, &a);
   voice_dictation_subscribe(mock_cb, &b);

   voice_dictation_unsubscribe(ha);
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);

   CHECK_EQ(a.call_count, 0);
   CHECK_EQ(b.call_count, 1);
   return 0;
}

static int test_subscriber_table_full_returns_minus_one(void) {
   voice_dictation_init();
   mock_sub_t dummy[8] = {0};
   int handles[8] = {0};
   int got_full = 0;
   for (int i = 0; i < 8; i++) {
      handles[i] = voice_dictation_subscribe(mock_cb, &dummy[i]);
      if (handles[i] == -1) got_full = 1;
   }
   CHECK_EQ(got_full, 1);   /* DICT_MAX_SUBSCRIBERS is 4 */
   return 0;
}

int main(void) {
   if (test_init_state_is_idle()) return 1;
   if (test_idle_to_recording_fires_subscriber()) return 1;
   if (test_full_happy_path()) return 1;
   if (test_idempotent_same_state()) return 1;
   if (test_invalid_backward_transition_blocked()) return 1;
   if (test_note_slot_persists_through_pipeline()) return 1;
   if (test_failed_from_uploading_auth()) return 1;
   if (test_failed_from_transcribing_empty()) return 1;
   if (test_retry_from_failed()) return 1;
   if (test_cancel_from_recording()) return 1;
   if (test_failed_clears_reason_on_idle()) return 1;
   if (test_multiple_subscribers_all_fire()) return 1;
   if (test_unsubscribe_stops_callbacks()) return 1;
   if (test_subscriber_table_full_returns_minus_one()) return 1;
   fprintf(stderr, "ok  %d checks passed\n", g_pass);
   return 0;
}
