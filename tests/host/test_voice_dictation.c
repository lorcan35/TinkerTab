/* Host-targeted unit tests for main/voice_dictation.c.
 *
 * Pure-C state machine — zero ESP-IDF deps in the module under test;
 * this file uses plain assert.h / stdio just like test_md_strip.c. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"   /* host fake-clock helpers (host_clock_*, host_test_reset) */
#include "task_worker.h" /* host worker stub (tab5_worker_pump, _set_full) */
#include "voice_dictation.h"

static int g_pass = 0;

#define CHECK(cond)                                                      \
   do {                                                                  \
      if (!(cond)) {                                                     \
         fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
         return 1;                                                       \
      }                                                                  \
      g_pass++;                                                          \
   } while (0)

#define CHECK_EQ(a, b)                                                                                          \
   do {                                                                                                         \
      if ((a) != (b)) {                                                                                         \
         fprintf(stderr, "FAIL %s:%d  %s != %s  (%d vs %d)\n", __FILE__, __LINE__, #a, #b, (int)(a), (int)(b)); \
         return 1;                                                                                              \
      }                                                                                                         \
      g_pass++;                                                                                                 \
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
   int call_count;
   dict_event_t last;
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

   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 2000);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2100);
   voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, 2400);
   voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, 4400);

   CHECK_EQ(m.call_count, 5);
   CHECK_EQ(m.last.state, DICT_IDLE);
   CHECK_EQ((int)m.last.started_ms, 1000);
   CHECK_EQ((int)m.last.stopped_ms, 2000);
   CHECK_EQ(m.last.note_slot, -1); /* IDLE reset cleared it */
   return 0;
}

static int test_idempotent_same_state(void) {
   voice_dictation_init();
   mock_sub_t m = {0};
   voice_dictation_subscribe(mock_cb, &m);

   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1500); /* dup */
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 2000); /* dup */

   CHECK_EQ(m.call_count, 1); /* duplicates suppressed */
   return 0;
}

static int test_invalid_backward_transition_blocked(void) {
   voice_dictation_init();
   mock_sub_t m = {0};
   voice_dictation_subscribe(mock_cb, &m);

   /* Walk forward to TRANSCRIBING (the strict guard table disallows
    * IDLE → TRANSCRIBING jumps, so we use the canonical path). */
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 100);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 200);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 300);

   /* Invalid: TRANSCRIBING → RECORDING (no resume mid-flight without
    * going through SAVED or FAILED first). */
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 2000);

   CHECK_EQ(m.last.state, DICT_TRANSCRIBING);
   return 0;
}

static int test_note_slot_persists_through_pipeline(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_note_slot(7);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 2000);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2100);
   voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, 2400);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.note_slot, 7);

   voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, 4400);
   e = voice_dictation_get();
   CHECK_EQ(e.note_slot, -1); /* cleared on IDLE */
   return 0;
}

static int test_failed_from_uploading_auth(void) {
   voice_dictation_init();
   mock_sub_t m = {0};
   voice_dictation_subscribe(mock_cb, &m);

   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 2000);
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_AUTH, 2100);

   CHECK_EQ(m.last.state, DICT_FAILED);
   CHECK_EQ(m.last.fail_reason, DICT_FAIL_AUTH);
   return 0;
}

static int test_failed_from_transcribing_empty(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 2000);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2100);
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_EMPTY, 2400);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_FAILED);
   CHECK_EQ(e.fail_reason, DICT_FAIL_EMPTY);
   return 0;
}

static int test_retry_from_failed(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, 1100);
   /* User taps retry → caller drives UPLOADING. */
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 5000);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_UPLOADING);
   CHECK_EQ(e.fail_reason, DICT_FAIL_NONE); /* cleared on leave-FAILED */
   return 0;
}

static int test_cancel_from_recording(void) {
   /* W1 (S2-7): cancel drives the neutral CANCELLED terminal (reason NONE),
    * which then self-decays to IDLE — NOT the deprecated FAILED+CANCELLED. */
   voice_dictation_init();
   host_test_reset();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_CANCELLED, DICT_FAIL_NONE, 1500);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_CANCELLED);
   CHECK_EQ(e.fail_reason, DICT_FAIL_NONE);

   host_clock_advance_ms(2000); /* > DICT_DECAY_CANCELLED_MS (1.5 s) */
   tab5_worker_pump();
   CHECK_EQ(voice_dictation_get().state, DICT_IDLE);
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
   CHECK_EQ(got_full, 1); /* DICT_MAX_SUBSCRIBERS is 4 */
   return 0;
}

static int test_set_note_slot_rejected_in_idle(void) {
   voice_dictation_init();
   voice_dictation_set_note_slot(42); /* in IDLE — should be ignored */
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.note_slot, -1); /* unchanged */
   return 0;
}

static int test_set_note_slot_accepted_in_recording(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_note_slot(7);
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.note_slot, 7);
   return 0;
}

static int test_set_note_slot_accepted_in_uploading(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 2000);
   voice_dictation_set_note_slot(11);
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.note_slot, 11);
   return 0;
}

static int test_set_note_slot_rejected_after_saved(void) {
   voice_dictation_init();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 2000);
   voice_dictation_set_note_slot(13);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2100);
   voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, 2400);
   voice_dictation_set_note_slot(99); /* should be ignored — past upload window */
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.note_slot, 13); /* still the legit one from UPLOADING */
   return 0;
}

/* Subscriber that re-enters into voice_dictation_get() to verify the
 * recursive lock works. */
static int g_reentrant_get_state = -1;
static void reentrant_cb(const dict_event_t *e, void *ud) {
   (void)e;
   (void)ud;
   dict_event_t inner = voice_dictation_get(); /* MUST not deadlock */
   g_reentrant_get_state = (int)inner.state;
}

static int test_subscriber_can_reenter_get(void) {
   voice_dictation_init();
   voice_dictation_subscribe(reentrant_cb, NULL);
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   CHECK_EQ(g_reentrant_get_state, (int)DICT_RECORDING);
   return 0;
}

static int test_saved_to_recording_now_refused(void) {
   /* W1 (S3-5): SAVED→RECORDING was removed.  Self-decay reaches IDLE first,
    * and a fast re-dictate goes through voice_dictation_begin (which snaps
    * the terminal to IDLE).  A RAW set_state(RECORDING) from SAVED is refused. */
   voice_dictation_init();
   host_test_reset();
   mock_sub_t m = {0};
   voice_dictation_subscribe(mock_cb, &m);

   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 2000);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2100);
   voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, 2400);

   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 5000); /* refused */

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_SAVED); /* unchanged */
   return 0;
}

static int test_cancelled_is_terminal_not_failed(void) {
   /* W1 (S2-7): cancel is the DICT_CANCELLED *state* with reason NONE —
    * not FAILED/CANCELLED.  Renders neutral, no "TAP TO RETRY". */
   voice_dictation_init();
   host_test_reset();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_CANCELLED, DICT_FAIL_NONE, 1500);

   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_CANCELLED);
   CHECK_EQ(e.fail_reason, DICT_FAIL_NONE);
   return 0;
}

static int test_begin_ws_mints_turn_id(void) {
   voice_dictation_init();
   host_test_reset();
   const char *tid = voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1000);
   CHECK(tid != NULL);
   CHECK(tid[0] != '\0');
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_RECORDING);
   CHECK_EQ(e.origin, DICT_ORIGIN_WS);
   CHECK(strlen(e.turn_id) > 0);
   CHECK(strcmp(e.turn_id, tid) == 0);
   return 0;
}

static int test_begin_refused_during_offline_upload(void) {
   /* Reverse race: a WS dictation starting mid-offline-upload must be
    * refused (the offline POST runs for seconds outside the lock). */
   voice_dictation_init();
   host_test_reset();
   bool ok = voice_dictation_try_begin_offline("offlineid", 3, 1000);
   CHECK(ok);
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_UPLOADING);
   CHECK_EQ(e.origin, DICT_ORIGIN_OFFLINE);

   const char *tid = voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1100);
   CHECK(tid == NULL); /* refused */
   e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_UPLOADING); /* unchanged */
   CHECK_EQ(e.origin, DICT_ORIGIN_OFFLINE);
   return 0;
}

static int test_try_begin_offline_refused_when_live(void) {
   /* Forward race: the offline queue cannot claim while a WS turn is live. */
   voice_dictation_init();
   host_test_reset();
   const char *tid = voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1000);
   CHECK(tid != NULL);
   bool ok = voice_dictation_try_begin_offline("x", 1, 1100);
   CHECK(!ok);
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_RECORDING);
   CHECK_EQ(e.origin, DICT_ORIGIN_WS);
   return 0;
}

/* ── W1 self-liveness (decay via the worker) ── */

static int test_saved_decays_to_idle(void) {
   voice_dictation_init();
   host_test_reset();
   /* origin NONE here, so resolution_pending stays false → SAVED arms decay. */
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 1500);
   voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, 1600); /* arms SAVED decay */

   host_clock_advance_ms(1000); /* < 2 s — not yet due */
   tab5_worker_pump();
   CHECK_EQ(voice_dictation_get().state, DICT_SAVED);

   host_clock_advance_ms(2000); /* total 3 s > DICT_DECAY_SAVED_MS */
   tab5_worker_pump();
   CHECK_EQ(voice_dictation_get().state, DICT_IDLE);
   return 0;
}

static int test_failed_decays_to_idle(void) {
   voice_dictation_init();
   host_test_reset();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, 1100); /* arms FAILED decay */

   host_clock_advance_ms(6000); /* > DICT_DECAY_FAILED_MS (5 s) */
   tab5_worker_pump();
   CHECK_EQ(voice_dictation_get().state, DICT_IDLE);
   return 0;
}

static int test_cancelled_decays_to_idle(void) {
   voice_dictation_init();
   host_test_reset();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_CANCELLED, DICT_FAIL_NONE, 1100);

   host_clock_advance_ms(2000); /* > DICT_DECAY_CANCELLED_MS (1.5 s) */
   tab5_worker_pump();
   CHECK_EQ(voice_dictation_get().state, DICT_IDLE);
   return 0;
}

static int test_decay_suppressed_while_resolution_pending(void) {
   /* A WS turn awaiting Dragon's terminal: a timeout-driven FAILED must NOT
    * decay, so a 60-90s-late summary can still correct it (FAILED→SAVED). */
   voice_dictation_init();
   host_test_reset();
   voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1000);                /* RECORDING, origin WS */
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2000); /* sets resolution_pending */
   CHECK(voice_dictation_get().resolution_pending);

   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, 3000); /* timeout; pending still true */
   host_clock_advance_ms(6000);
   tab5_worker_pump();
   CHECK_EQ(voice_dictation_get().state, DICT_FAILED); /* NOT decayed — resolution pending */
   return 0;
}

static int test_decay_requeues_on_full_worker(void) {
   /* Full worker queue at decay time must not strand the terminal: the timer
    * cb re-arms a short retry, and the decay lands once the queue drains. */
   voice_dictation_init();
   host_test_reset();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, 1100); /* arms FAILED decay */

   tab5_worker_stub_set_full(true);
   host_clock_advance_ms(6000); /* timer fires → enqueue fails → re-arm @ +250ms */
   tab5_worker_pump();          /* nothing queued */
   CHECK_EQ(voice_dictation_get().state, DICT_FAILED); /* not decayed yet */

   tab5_worker_stub_set_full(false);
   host_clock_advance_ms(300); /* > DICT_DECAY_RETRY_MS (250) */
   tab5_worker_pump();
   CHECK_EQ(voice_dictation_get().state, DICT_IDLE); /* decayed via the retry */
   return 0;
}

static int test_try_begin_offline_atomic_vs_decay(void) {
   /* SAVED's decay job is enqueued (timer fired) but not yet pumped; the
    * offline queue claims in the gap; the stale decay job must NOT clobber
    * the freshly-claimed turn. */
   voice_dictation_init();
   host_test_reset();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, 1500);
   voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, 1600); /* arms SAVED decay */

   host_clock_advance_ms(3000); /* timer fires → dict_decay_apply_job enqueued (NOT pumped) */

   bool ok = voice_dictation_try_begin_offline("newturn", 5, 0); /* claim in the gap */
   CHECK(ok);
   CHECK_EQ(voice_dictation_get().state, DICT_UPLOADING);

   tab5_worker_pump(); /* run the stale decay job — must no-op */
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_UPLOADING); /* not clobbered to IDLE */
   CHECK_EQ(e.origin, DICT_ORIGIN_OFFLINE);
   return 0;
}

/* ── W1 resolution semantics (turn_id gating + late correction) ── */

static int test_resolve_if_current_drops_stale(void) {
   voice_dictation_init();
   host_test_reset();
   const char *tid = voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1000);
   CHECK(tid != NULL);
   char idA[DICT_TURN_ID_LEN];
   strncpy(idA, tid, sizeof(idA));
   idA[sizeof(idA) - 1] = '\0';
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2000);

   /* A summary for a DIFFERENT turn must be dropped. */
   bool applied = voice_dictation_resolve_if_current("ffffffffffff", DICT_SAVED, DICT_FAIL_NONE, 3000);
   CHECK(!applied);
   CHECK_EQ(voice_dictation_get().state, DICT_TRANSCRIBING);

   /* The summary for the live turn applies. */
   applied = voice_dictation_resolve_if_current(idA, DICT_SAVED, DICT_FAIL_NONE, 3100);
   CHECK(applied);
   CHECK_EQ(voice_dictation_get().state, DICT_SAVED);
   return 0;
}

static int test_missing_turn_id_treated_as_match(void) {
   voice_dictation_init();
   host_test_reset();
   voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1000);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2000);
   CHECK(voice_dictation_resolve_if_current("", DICT_SAVED, DICT_FAIL_NONE, 3000)); /* "" matches */
   CHECK_EQ(voice_dictation_get().state, DICT_SAVED);

   voice_dictation_begin(DICT_ORIGIN_WS, NULL, 4000);
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 4100);
   CHECK(voice_dictation_resolve_if_current(NULL, DICT_SAVED, DICT_FAIL_NONE, 4200)); /* NULL matches */
   return 0;
}

static int test_failed_to_saved_late_correction_ws_only(void) {
   /* WS turn: a timeout drives FAILED while Dragon's slow summary is still
    * coming; the late summary corrects FAILED→SAVED, which then self-decays. */
   voice_dictation_init();
   host_test_reset();
   const char *tid = voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1000);
   char idA[DICT_TURN_ID_LEN];
   strncpy(idA, tid, sizeof(idA));
   idA[sizeof(idA) - 1] = '\0';
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2000); /* pending=true (WS) */
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, 3000);    /* timeout; pending kept */
   CHECK_EQ(voice_dictation_get().state, DICT_FAILED);
   CHECK(voice_dictation_get().resolution_pending);

   CHECK(voice_dictation_resolve_if_current(idA, DICT_SAVED, DICT_FAIL_NONE, 90000));
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_SAVED);
   CHECK(!e.resolution_pending); /* cleared on SAVED → now decays */

   host_clock_advance_ms(3000);
   tab5_worker_pump();
   CHECK_EQ(voice_dictation_get().state, DICT_IDLE);
   return 0;
}

static int test_begin_same_origin_keeps_turn_id(void) {
   /* W2 F5: a same-origin re-entrant begin returns the SAME id (no re-mint),
    * so a Dragon summary echoing that id still resolves the live turn. */
   voice_dictation_init();
   host_test_reset();
   const char *t1 = voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1000);
   CHECK(t1 != NULL);
   char id1[DICT_TURN_ID_LEN];
   strncpy(id1, t1, sizeof(id1));
   id1[sizeof(id1) - 1] = '\0';
   const char *t2 = voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1100); /* re-entrant */
   CHECK(t2 != NULL);
   CHECK(strcmp(id1, t2) == 0);                        /* same id, not re-minted */
   CHECK_EQ(voice_dictation_get().state, DICT_RECORDING);
   CHECK(strcmp(id1, voice_dictation_get().turn_id) == 0);
   return 0;
}

static int test_failed_to_saved_cross_turn_dropped(void) {
   /* W2: even with a pending WS resolution, a SAVED echoing the WRONG turn_id
    * is dropped by resolve_if_current — turn_id match is the authority now. */
   voice_dictation_init();
   host_test_reset();
   const char *tid = voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1000);
   char idA[DICT_TURN_ID_LEN];
   strncpy(idA, tid, sizeof(idA));
   idA[sizeof(idA) - 1] = '\0';
   voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2000); /* pending */
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, 3000);
   CHECK(!voice_dictation_resolve_if_current("0000deadbeef", DICT_SAVED, DICT_FAIL_NONE, 4000));
   CHECK_EQ(voice_dictation_get().state, DICT_FAILED); /* wrong-turn SAVED dropped */
   CHECK(voice_dictation_resolve_if_current(idA, DICT_SAVED, DICT_FAIL_NONE, 4100));
   CHECK_EQ(voice_dictation_get().state, DICT_SAVED); /* matching turn corrects it */
   return 0;
}

static int test_offline_failed_decays_to_idle(void) {
   /* An offline (REST) FAILED is definitive — the REST response IS the
    * resolution — so it clears pending and decays, unlike a WS FAILED which
    * keeps pending for a possible late correction. */
   voice_dictation_init();
   host_test_reset();
   CHECK(voice_dictation_try_begin_offline("offid", 2, 0)); /* UPLOADING, OFFLINE, pending */
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, 1000);
   CHECK(!voice_dictation_get().resolution_pending); /* cleared (offline) */
   host_clock_advance_ms(6000);
   tab5_worker_pump();
   CHECK_EQ(voice_dictation_get().state, DICT_IDLE);
   return 0;
}

static int test_failed_to_saved_refused_when_not_pending(void) {
   /* No WS resolution pending → a stray SAVED must NOT resurrect a FAILED. */
   voice_dictation_init();
   host_test_reset();
   voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE, 1000);
   voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, 1100); /* origin NONE, not pending */
   bool applied = voice_dictation_resolve_if_current("", DICT_SAVED, DICT_FAIL_NONE, 2000);
   CHECK(!applied); /* guard refuses */
   CHECK_EQ(voice_dictation_get().state, DICT_FAILED);
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
   if (test_set_note_slot_rejected_in_idle()) return 1;
   if (test_set_note_slot_accepted_in_recording()) return 1;
   if (test_set_note_slot_accepted_in_uploading()) return 1;
   if (test_set_note_slot_rejected_after_saved()) return 1;
   if (test_subscriber_can_reenter_get()) return 1;
   if (test_saved_to_recording_now_refused()) return 1;
   if (test_cancelled_is_terminal_not_failed()) return 1;
   if (test_begin_ws_mints_turn_id()) return 1;
   if (test_begin_refused_during_offline_upload()) return 1;
   if (test_try_begin_offline_refused_when_live()) return 1;
   if (test_saved_decays_to_idle()) return 1;
   if (test_failed_decays_to_idle()) return 1;
   if (test_cancelled_decays_to_idle()) return 1;
   if (test_decay_suppressed_while_resolution_pending()) return 1;
   if (test_decay_requeues_on_full_worker()) return 1;
   if (test_try_begin_offline_atomic_vs_decay()) return 1;
   if (test_resolve_if_current_drops_stale()) return 1;
   if (test_missing_turn_id_treated_as_match()) return 1;
   if (test_failed_to_saved_late_correction_ws_only()) return 1;
   if (test_begin_same_origin_keeps_turn_id()) return 1;
   if (test_failed_to_saved_cross_turn_dropped()) return 1;
   if (test_offline_failed_decays_to_idle()) return 1;
   if (test_failed_to_saved_refused_when_not_pending()) return 1;
   fprintf(stderr, "ok  %d checks passed\n", g_pass);
   return 0;
}
