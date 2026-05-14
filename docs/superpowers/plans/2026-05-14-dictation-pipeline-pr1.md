# Dictation pipeline PR 1 — state machine + VAD + instrumentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the canonical `dict_pipeline_t` state machine in a new `main/voice_dictation.{c,h}` module, wire all existing dictation entry points (voice.c WS path + ui_notes.c REST polling path) to drive transitions, and add a `/dictation_pipeline` debug endpoint so PR 2/3 can verify the state without UI surfaces yet.

**Architecture:** New pure-C module owns the state, transitions, and a fan-out subscriber list. Existing dictation code paths call `voice_dictation_set_state(...)` at well-defined points. UI subscribers come in PR 2 + PR 3. The module is host-testable — caller passes monotonic timestamps so the module has zero ESP-IDF deps.

**Tech Stack:** ESP-IDF 5.5.2 component (`main/`), plain C state machine, host-test via `tests/host/` (assert.h + ctest pattern matching `test_md_strip.c`). Spec at [`docs/superpowers/specs/2026-05-14-dictation-pipeline-notes-redesign-design.md`](../specs/2026-05-14-dictation-pipeline-notes-redesign-design.md).

---

## File map

**Create:**
- `main/voice_dictation.h` — public state-machine API (types, subscribe, set_state, get)
- `main/voice_dictation.c` — implementation (subscriber list, transition validation, thread-safe state)
- `tests/host/test_voice_dictation.c` — host-targeted unit tests

**Modify:**
- `main/CMakeLists.txt` — add `voice_dictation.c` to component sources
- `tests/host/CMakeLists.txt` — add `test_voice_dictation` target
- `main/main.c` — call `voice_dictation_init()` at boot
- `main/voice.c` — instrument `voice_start_dictation` + `voice_stop_listening` + VAD auto-stop + 5-min hard cap
- `main/voice_ws_proto.c` — instrument `dictation_summary` JSON handler + WS disconnect failure path
- `main/ui_notes.c` — instrument `transcription_queue_task` transitions (overlap with PR #517's reason-marking)
- `main/debug_server_dictation.c` — add `GET /dictation_pipeline` reporting current state name + reason name + age

**Boundary rule:** `voice_dictation.c` MUST stay free of ESP-IDF deps for clean host testing. Anything platform-y (timers, mutexes) is shimmed in `tests/host/shim/` or stays in the instrumentation callers.

**Known deviations from spec:**

- The spec's locked-decisions table says "4 s VAD silence".  The existing
  `DICTATION_AUTO_STOP_FRAMES = 250` in `main/voice.c` is 5 s (250 frames
  × 20 ms).  PR 1 keeps this at 5 s to preserve current behaviour —
  changing it is a tuning knob best done in PR 2 once the orb's
  RECORDING state is visible and we can see whether 4 s feels snappier.
  Filed as a follow-up under the spec's "Risks + open questions" §2.

---

## Tasks

### Task 1 — Public types + API surface (`voice_dictation.h`)

**Files:**
- Create: `main/voice_dictation.h`

- [ ] **Step 1: Write the header**

```c
/* main/voice_dictation.h — Canonical state machine for a dictation
 * pipeline (RECORDING → UPLOADING → TRANSCRIBING → SAVED, with FAILED
 * as a terminal branch).  Multiple UI surfaces subscribe and render
 * the same state.
 *
 * Designed for host-testability: zero ESP-IDF deps in voice_dictation.c.
 * Callers pass monotonic timestamps in ms.  See spec at
 * docs/superpowers/specs/2026-05-14-dictation-pipeline-notes-redesign-design.md
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
 * Persists through subsequent transitions until next IDLE. */
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
```

- [ ] **Step 2: Commit the header (compiles standalone — no implementation yet)**

```bash
cd ~/projects/TinkerTab
git add main/voice_dictation.h
git commit -m "feat(dictation): voice_dictation.h public API"
```

---

### Task 2 — Empty implementation + minimal host test

**Files:**
- Create: `main/voice_dictation.c`
- Create: `tests/host/test_voice_dictation.c`
- Modify: `tests/host/CMakeLists.txt`

- [ ] **Step 1: Write the failing test first**

Create `tests/host/test_voice_dictation.c`:

```c
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

int main(void) {
   if (test_init_state_is_idle()) return 1;
   fprintf(stderr, "ok  %d checks passed\n", g_pass);
   return 0;
}
```

- [ ] **Step 2: Verify the test fails to build (no implementation yet)**

Add to `tests/host/CMakeLists.txt` (append at end, before any closing markers):

```cmake
# voice_dictation — pipeline state machine (PR 1).
add_executable(test_voice_dictation
    test_voice_dictation.c
    ${MAIN_DIR}/voice_dictation.c
)
target_include_directories(test_voice_dictation PRIVATE ${MAIN_DIR})
add_test(NAME voice_dictation COMMAND test_voice_dictation)
```

Run:

```bash
cd ~/projects/TinkerTab/tests/host
cmake -S . -B build && cmake --build build 2>&1 | tail -20
```

Expected: FAIL — `voice_dictation.c: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `main/voice_dictation.c`:

```c
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

void voice_dictation_set_state(dict_state_t new_state, dict_fail_t fail_reason,
                               uint32_t now_ms) {
   /* Implemented in Task 4. */
   (void)new_state; (void)fail_reason; (void)now_ms;
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
```

- [ ] **Step 4: Run the test — it should pass now**

```bash
cd ~/projects/TinkerTab/tests/host
cmake --build build && ctest --test-dir build -R voice_dictation --output-on-failure
```

Expected: `ok  3 checks passed` + `1/1 tests passed`.

- [ ] **Step 5: Commit**

```bash
cd ~/projects/TinkerTab
git add main/voice_dictation.c tests/host/test_voice_dictation.c tests/host/CMakeLists.txt
git commit -m "feat(dictation): voice_dictation.c minimal scaffold + host test"
```

---

### Task 3 — IDLE → RECORDING transition + subscriber callback

**Files:**
- Modify: `main/voice_dictation.c`
- Modify: `tests/host/test_voice_dictation.c`

- [ ] **Step 1: Write the failing test**

Append to `tests/host/test_voice_dictation.c` before `main()`:

```c
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
```

Add to `main()` after the existing call:

```c
   if (test_idle_to_recording_fires_subscriber()) return 1;
```

- [ ] **Step 2: Run to verify failure**

```bash
cd ~/projects/TinkerTab/tests/host
cmake --build build && ctest --test-dir build -R voice_dictation --output-on-failure
```

Expected: FAIL with `m.call_count != 1 (0 vs 1)` — set_state is still a stub.

- [ ] **Step 3: Implement transition + dispatch**

Replace the stub `voice_dictation_set_state` in `main/voice_dictation.c` with:

```c
static void dict_dispatch(void) {
   for (int i = 0; i < DICT_MAX_SUBSCRIBERS; i++) {
      if (s_subs[i].in_use && s_subs[i].cb) {
         s_subs[i].cb(&s_event, s_subs[i].user_data);
      }
   }
}

void voice_dictation_set_state(dict_state_t new_state, dict_fail_t fail_reason,
                               uint32_t now_ms) {
   /* Idempotent — same-state re-entry is a no-op (avoids spamming
    * subscribers during a chatty caller that re-asserts state). */
   if (new_state == s_event.state && fail_reason == s_event.fail_reason) {
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
```

- [ ] **Step 4: Run — should pass**

```bash
ctest --test-dir build -R voice_dictation --output-on-failure
```

Expected: `ok  9 checks passed`.

- [ ] **Step 5: Commit**

```bash
git add main/voice_dictation.c tests/host/test_voice_dictation.c
git commit -m "feat(dictation): IDLE→RECORDING transition + subscriber dispatch"
```

---

### Task 4 — Full forward pipeline + transition guard

**Files:**
- Modify: `main/voice_dictation.c`
- Modify: `tests/host/test_voice_dictation.c`

- [ ] **Step 1: Write the failing test for the full happy path**

Append before `main()`:

```c
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

   voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, 1000);
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
```

Add calls in `main()`:

```c
   if (test_full_happy_path()) return 1;
   if (test_idempotent_same_state()) return 1;
   if (test_invalid_backward_transition_blocked()) return 1;
   if (test_note_slot_persists_through_pipeline()) return 1;
```

- [ ] **Step 2: Run to verify failure**

```bash
ctest --test-dir build -R voice_dictation --output-on-failure
```

Expected: FAIL on `test_invalid_backward_transition_blocked` — current code doesn't guard transitions.

- [ ] **Step 3: Add transition guard**

In `main/voice_dictation.c`, add a guard helper above `voice_dictation_set_state`:

```c
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
```

Update the head of `voice_dictation_set_state` to use the guard (insert after the same-state-dup check):

```c
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
```

- [ ] **Step 4: Run — should pass**

```bash
ctest --test-dir build -R voice_dictation --output-on-failure
```

Expected: all checks pass (count ≈ 25+).

- [ ] **Step 5: Commit**

```bash
git add main/voice_dictation.c tests/host/test_voice_dictation.c
git commit -m "feat(dictation): full pipeline transitions + guard"
```

---

### Task 5 — FAILED transitions + retry path

**Files:**
- Modify: `tests/host/test_voice_dictation.c`

- [ ] **Step 1: Write tests**

Append before `main()`:

```c
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
```

Add to `main()`:

```c
   if (test_failed_from_uploading_auth()) return 1;
   if (test_failed_from_transcribing_empty()) return 1;
   if (test_retry_from_failed()) return 1;
   if (test_cancel_from_recording()) return 1;
   if (test_failed_clears_reason_on_idle()) return 1;
```

- [ ] **Step 2: Run — should already pass (Task 4's transition table already covers FAILED → UPLOADING and SAVED/FAILED → IDLE)**

```bash
cmake --build build && ctest --test-dir build -R voice_dictation --output-on-failure
```

Expected: pass. If not, the most likely fix is in `dict_transition_allowed` — re-read Task 4's guard logic and confirm `FAILED → UPLOADING` is allowed and `FAILED → IDLE` is allowed.

- [ ] **Step 3: Commit**

```bash
git add tests/host/test_voice_dictation.c
git commit -m "test(dictation): FAILED transitions + retry + cancel coverage"
```

---

### Task 6 — Multi-subscriber + unsubscribe

**Files:**
- Modify: `tests/host/test_voice_dictation.c`

- [ ] **Step 1: Write tests**

```c
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
```

Add to `main()`:

```c
   if (test_multiple_subscribers_all_fire()) return 1;
   if (test_unsubscribe_stops_callbacks()) return 1;
   if (test_subscriber_table_full_returns_minus_one()) return 1;
```

- [ ] **Step 2: Run — should pass with existing implementation**

```bash
ctest --test-dir build -R voice_dictation --output-on-failure
```

Expected: pass.

- [ ] **Step 3: Commit**

```bash
git add tests/host/test_voice_dictation.c
git commit -m "test(dictation): multi-subscriber + unsubscribe + table-full"
```

---

### Task 7 — Wire module into component build + boot init

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.c`

- [ ] **Step 1: Add to component sources**

Open `main/CMakeLists.txt`, find the existing `idf_component_register` SRCS list (alphabetically grouped), insert `"voice_dictation.c"` near other `voice_*.c` entries:

```cmake
# (existing line context — find the voice_* group)
        "voice.c"
        "voice_billing.c"
        "voice_codec.c"
        "voice_dictation.c"        # NEW — PR 1
        "voice_m5_llm.c"
        "voice_messages_sync.c"
        # …
```

- [ ] **Step 2: Run target build, confirm no compile errors**

```bash
cd ~/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -20
```

Expected: build succeeds (no link errors, no unused-symbol warnings).

- [ ] **Step 3: Call init from boot**

In `main/main.c`, find the boot sequence after `voice_init` (search `grep -n voice_init main/main.c`) and add:

```c
   /* PR 1: dictation pipeline state machine.  Idempotent; subscribers
    * register lazily once their owning screen / surface is created.
    * Must run after voice_init since transitions are driven from voice.c. */
   voice_dictation_init();
```

Add the include at the top of `main/main.c` near other voice includes:

```c
#include "voice_dictation.h"
```

- [ ] **Step 4: Rebuild + flash, confirm boot completes**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -8
python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac 2>&1 | tail -3
sleep 18
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" http://192.168.1.90:8080/info | head -c 200
```

Expected: device responds normally.  No PANIC.  Heap monitor reports normal numbers.

- [ ] **Step 5: Commit**

```bash
git add main/CMakeLists.txt main/main.c
git commit -m "feat(dictation): wire voice_dictation into component + boot init"
```

---

### Task 8 — Instrument `voice_start_dictation`

**Files:**
- Modify: `main/voice.c`

- [ ] **Step 1: Read the current path**

Open `main/voice.c` and find `voice_start_dictation` (around line 1592).  We're inserting at TWO sites: just before `voice_set_state(VOICE_STATE_LISTENING, ...)` near the end (around line 1689), and storing the note slot on the offline path so `voice_dictation_set_note_slot` gets the right index.

- [ ] **Step 2: Add the include at the top of voice.c**

Find the existing `#include "voice_..."` block (sorted alphabetically) and add:

```c
#include "voice_dictation.h"
```

- [ ] **Step 3: Add the transition at the end of voice_start_dictation**

In `voice_start_dictation`, immediately before the existing line:

```c
    voice_set_state(VOICE_STATE_LISTENING, offline ? "offline" : NULL);
    return ESP_OK;
}
```

insert:

```c
    /* PR 1: drive the dictation pipeline state machine.  The pipeline
     * is a separate higher-level state; the existing voice_state_t
     * (LISTENING/PROCESSING/READY) is unchanged. */
    voice_dictation_set_state(DICT_RECORDING, DICT_FAIL_NONE,
                              (uint32_t)(esp_timer_get_time() / 1000));
```

- [ ] **Step 4: Build + verify no warnings**

```bash
idf.py build 2>&1 | tail -10
```

Expected: success.  If there's an `esp_timer_get_time` undefined, the include is already there (used elsewhere in voice.c) — just confirm.

- [ ] **Step 5: Commit**

```bash
git add main/voice.c
git commit -m "feat(dictation): instrument voice_start_dictation → DICT_RECORDING"
```

---

### Task 9 — Instrument `voice_stop_listening` for the dictate path

**Files:**
- Modify: `main/voice.c`

- [ ] **Step 1: Locate `voice_stop_listening`**

It starts around line 1786.  We need to add a transition AFTER the function decides it's actually stopping a dictation (mode == DICTATE).  The branch points:

- offline_dictate path → DICT_UPLOADING (REST handoff happens later from ui_notes.c)
- WS dictate path → DICT_TRANSCRIBING (mic stops; Dragon already has the audio; we're waiting on `dictation_summary`)

- [ ] **Step 2: Add the transition near the end of voice_stop_listening**

Find the function's final `return ESP_OK;` (or the equivalent).  Just before it, add:

```c
    /* PR 1: pipeline transition driven by mode.  Skip if this wasn't a
     * dictation stop (e.g. voice-ask path, which doesn't touch the
     * dictation pipeline at all). */
    if (voice_get_mode() == VOICE_MODE_DICTATE) {
       dict_state_t next = offline_dictate ? DICT_UPLOADING : DICT_TRANSCRIBING;
       voice_dictation_set_state(next, DICT_FAIL_NONE,
                                 (uint32_t)(esp_timer_get_time() / 1000));
    }
```

The variable `offline_dictate` is already in scope from the function's earlier branch (line ~1807).  If that variable name was renamed in a follow-up, search the function body for its definition and use the matching name.

- [ ] **Step 3: Build + verify**

```bash
idf.py build 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 4: Commit**

```bash
git add main/voice.c
git commit -m "feat(dictation): instrument voice_stop_listening → UPLOADING/TRANSCRIBING"
```

---

### Task 10 — Instrument VAD auto-stop + add 5-min hard cap to WS path

**Files:**
- Modify: `main/voice.c`

- [ ] **Step 1: Locate the VAD auto-stop block**

Around line 831 in `main/voice.c`:

```c
              if (had_speech && total_silence_frames >= DICTATION_AUTO_STOP_FRAMES) {
                 ESP_LOGI(TAG, "Dictation: auto-stop after %dms of silence",
                          DICTATION_AUTO_STOP_FRAMES * TAB5_VOICE_CHUNK_MS / 1000);
                 /* … existing auto-stop send logic … */
              }
```

Around line 770, the ASK-mode hard cap:

```c
           if (voice_get_mode() == VOICE_MODE_ASK && frames_sent >= MAX_RECORD_FRAMES_ASK) {
              ESP_LOGI(TAG, "Max recording duration reached (%ds)", MAX_RECORD_FRAMES_ASK * TAB5_VOICE_CHUNK_MS / 1000);
```

DICTATE mode currently has no hard cap.  We add one matching the spec's 5-min ceiling (15 000 frames at 20 ms/frame).

- [ ] **Step 2: Add a constant near the existing MAX_RECORD_FRAMES_ASK definition (around line 185)**

```c
#define MAX_RECORD_FRAMES_ASK       1500
#define MAX_RECORD_FRAMES_DICT      15000   /* PR 1: 5 min hard cap on dictation */
```

- [ ] **Step 3: Add the dictation cap branch alongside the ASK cap**

Replace the existing ASK cap block with:

```c
           if (voice_get_mode() == VOICE_MODE_ASK && frames_sent >= MAX_RECORD_FRAMES_ASK) {
              ESP_LOGI(TAG, "Max recording duration reached (%ds)",
                       MAX_RECORD_FRAMES_ASK * TAB5_VOICE_CHUNK_MS / 1000);
              /* … existing auto-stop fallthrough … */
              break;
           }
           if (voice_get_mode() == VOICE_MODE_DICTATE && frames_sent >= MAX_RECORD_FRAMES_DICT) {
              ESP_LOGW(TAG, "Dictation hit 5-min cap — auto-stopping");
              voice_dictation_set_state(DICT_FAILED, DICT_FAIL_TOO_LONG,
                                        (uint32_t)(esp_timer_get_time() / 1000));
              /* Fall through to the same stop handler used by VAD auto-stop. */
              break;
           }
```

Adjust the surrounding code so the break exits the mic-write loop into the same `voice_stop_listening`-equivalent finalisation the VAD path uses.  If that finalisation is inline (in the same loop), copy the pattern verbatim from the VAD block.

- [ ] **Step 4: Build + flash + smoke test**

```bash
idf.py build 2>&1 | tail -5
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -5
python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac >/dev/null 2>&1
sleep 18
```

- [ ] **Step 5: Commit**

```bash
git add main/voice.c
git commit -m "feat(dictation): 5-min hard cap on dictation + pipeline integration"
```

---

### Task 11 — Instrument `dictation_summary` handler

**Files:**
- Modify: `main/voice_ws_proto.c`

- [ ] **Step 1: Open `voice_ws_proto.c` around line 795**

Current handler:

```c
   } else if (strcmp(type_str, "dictation_summary") == 0) {
      cJSON *title = cJSON_GetObjectItem(root, "title");
      cJSON *summary = cJSON_GetObjectItem(root, "summary");
      if (cJSON_IsString(title)) {
         strncpy(s_dictation_title, title->valuestring, sizeof(s_dictation_title) - 1);
         s_dictation_title[sizeof(s_dictation_title) - 1] = '\0';
      }
      if (cJSON_IsString(summary)) {
         strncpy(s_dictation_summary, summary->valuestring, sizeof(s_dictation_summary) - 1);
         s_dictation_summary[sizeof(s_dictation_summary) - 1] = '\0';
      }
      ESP_LOGI(TAG, "Dictation summary: \"%s\"", s_dictation_title);
      voice_set_state(VOICE_STATE_READY, "dictation_summary");
   }
```

- [ ] **Step 2: Add include + transition**

Add the include at the top:

```c
#include "voice_dictation.h"
```

Replace the handler block with:

```c
   } else if (strcmp(type_str, "dictation_summary") == 0) {
      cJSON *title = cJSON_GetObjectItem(root, "title");
      cJSON *summary = cJSON_GetObjectItem(root, "summary");
      if (cJSON_IsString(title)) {
         strncpy(s_dictation_title, title->valuestring, sizeof(s_dictation_title) - 1);
         s_dictation_title[sizeof(s_dictation_title) - 1] = '\0';
      }
      if (cJSON_IsString(summary)) {
         strncpy(s_dictation_summary, summary->valuestring, sizeof(s_dictation_summary) - 1);
         s_dictation_summary[sizeof(s_dictation_summary) - 1] = '\0';
      }
      ESP_LOGI(TAG, "Dictation summary: \"%s\"", s_dictation_title);
      voice_set_state(VOICE_STATE_READY, "dictation_summary");

      /* PR 1: pipeline transition.  Non-empty summary → SAVED; empty
       * summary (title also empty) → FAILED(EMPTY). */
      const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
      if (s_dictation_title[0] || s_dictation_summary[0]) {
         voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, now);
      } else {
         voice_dictation_set_state(DICT_FAILED, DICT_FAIL_EMPTY, now);
      }
   }
```

- [ ] **Step 3: Also instrument the cancelled branch (line ~793)**

The existing handler around line 793 of `main/voice_ws_proto.c`:

```c
   } else if (strcmp(type_str, "dictation_cancelled") == 0) {
      ESP_LOGI(TAG, "Dictation post-process cancelled (superseded or aborted)");
      voice_set_state(VOICE_STATE_READY, "dictation_cancelled");
   } else if (strcmp(type_str, "dictation_summary") == 0) {
```

Replace with:

```c
   } else if (strcmp(type_str, "dictation_cancelled") == 0) {
      ESP_LOGI(TAG, "Dictation post-process cancelled (superseded or aborted)");
      voice_set_state(VOICE_STATE_READY, "dictation_cancelled");
      voice_dictation_set_state(DICT_FAILED, DICT_FAIL_CANCELLED,
                                (uint32_t)(esp_timer_get_time() / 1000));
   } else if (strcmp(type_str, "dictation_summary") == 0) {
```

- [ ] **Step 4: Build + flash**

```bash
idf.py build 2>&1 | tail -5
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -3
python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac >/dev/null 2>&1
sleep 18
```

- [ ] **Step 5: Commit**

```bash
git add main/voice_ws_proto.c
git commit -m "feat(dictation): instrument dictation_summary → SAVED/FAILED(EMPTY)"
```

---

### Task 12 — Instrument WS disconnect failure path

**Files:**
- Modify: `main/voice_ws_proto.c`

- [ ] **Step 1: Find the disconnect handler**

`grep -n WEBSOCKET_EVENT_DISCONNECTED main/voice_ws_proto.c` — look for the case branch in the event handler.  The current behaviour transitions voice state to RECONNECTING when in-flight; we want to additionally fail the dictation pipeline if it was active.

- [ ] **Step 2: Add the conditional pipeline transition**

Inside the disconnect case (likely near a `voice_set_state(VOICE_STATE_RECONNECTING, ...)` line), insert:

```c
         /* PR 1: if a dictation was mid-pipeline, fail it with NETWORK
          * so the UI surfaces the disconnect.  No-op when pipeline is
          * already IDLE/SAVED/FAILED. */
         dict_state_t cur = voice_dictation_get().state;
         if (cur == DICT_RECORDING || cur == DICT_UPLOADING || cur == DICT_TRANSCRIBING) {
            voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK,
                                      (uint32_t)(esp_timer_get_time() / 1000));
         }
```

- [ ] **Step 3: Build + verify**

```bash
idf.py build 2>&1 | tail -5
```

- [ ] **Step 4: Commit**

```bash
git add main/voice_ws_proto.c
git commit -m "feat(dictation): WS disconnect fails in-flight pipeline → NETWORK"
```

---

### Task 13 — Instrument REST polling task transitions in `ui_notes.c`

**Files:**
- Modify: `main/ui_notes.c`

The REST polling task is `transcription_queue_task` (around line 999 after PR #517).  Each iteration picks a `NOTE_STATE_RECORDED` note, POSTs to `/api/v1/transcribe`, and either updates the note text + state or sets FAILED with a reason.

We thread the pipeline through it: on entering the upload, transition to UPLOADING; after headers received, transition to TRANSCRIBING; on success → SAVED; on each failure branch → FAILED with the matching reason.

- [ ] **Step 1: Add the include + a helper**

Top of `main/ui_notes.c`, near the other includes:

```c
#include "voice_dictation.h"
```

In the polling task, when `slot` is selected and the note is about to be uploaded:

```c
        note_entry_t *n = &s_notes[slot];
        ESP_LOGI(TAG, "Transcribing note [%d]: %s", slot, n->audio_path);
        n->state = NOTE_STATE_TRANSCRIBING;
        notes_save();

        /* PR 1: pipeline UPLOADING — REST POST begins. */
        voice_dictation_set_note_slot(slot);
        voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE,
                                  (uint32_t)(esp_timer_get_time() / 1000));
```

- [ ] **Step 2: Transition to TRANSCRIBING right after `esp_http_client_fetch_headers`**

Find the block right after the file write loop (around line 1157 after #517):

```c
        int content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
```

Add immediately after:

```c
        /* PR 1: TRANSCRIBING — request sent, headers received, Dragon
         * is now running STT.  Even if status != 200 we briefly pass
         * through TRANSCRIBING; the failure branch below converts to
         * FAILED with a specific reason. */
        voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE,
                                  (uint32_t)(esp_timer_get_time() / 1000));
```

- [ ] **Step 3: Hook each failure branch + the success branch**

After PR #517's structure, the success branch is around line 1175:

```c
                    if (text && text[0]) {
                        strncpy(n->text, text, MAX_NOTE_LEN - 1);
                        /* … */
                        n->state = NOTE_STATE_TRANSCRIBED;
                        n->fail_reason = NOTE_FAIL_NONE;
                    } else {
                        snprintf(n->text, MAX_NOTE_LEN, "(Empty transcription)");
                        n->state = NOTE_STATE_FAILED;
                        n->fail_reason = NOTE_FAIL_EMPTY;
                    }
```

Add pipeline transitions:

```c
                    if (text && text[0]) {
                        strncpy(n->text, text, MAX_NOTE_LEN - 1);
                        n->text[MAX_NOTE_LEN - 1] = '\0';
                        n->state = NOTE_STATE_TRANSCRIBED;
                        n->fail_reason = NOTE_FAIL_NONE;
                        voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE,
                                                  (uint32_t)(esp_timer_get_time() / 1000));
                    } else {
                        snprintf(n->text, MAX_NOTE_LEN, "(Empty transcription)");
                        n->state = NOTE_STATE_FAILED;
                        n->fail_reason = NOTE_FAIL_EMPTY;
                        voice_dictation_set_state(DICT_FAILED, DICT_FAIL_EMPTY,
                                                  (uint32_t)(esp_timer_get_time() / 1000));
                    }
```

The other failure branches (non-200 status, HTTP open fail, init NULL, file open fail, file too small, audio path missing) — each one already sets `n->fail_reason`.  Map each to the matching `dict_fail_t` and add the call:

| Note branch (PR #517) | Pipeline transition |
|---|---|
| `n->fail_reason = NOTE_FAIL_NO_AUDIO` (no audio path) | `DICT_FAILED, DICT_FAIL_NO_AUDIO` |
| `n->fail_reason = NOTE_FAIL_NO_AUDIO` (file open fail) | `DICT_FAILED, DICT_FAIL_NO_AUDIO` |
| `n->fail_reason = NOTE_FAIL_NO_AUDIO` (file too small) | `DICT_FAILED, DICT_FAIL_NO_AUDIO` |
| `n->fail_reason = NOTE_FAIL_NETWORK` (http init NULL) | `DICT_FAILED, DICT_FAIL_NETWORK` |
| `n->fail_reason = NOTE_FAIL_NETWORK` (http open fail) | `DICT_FAILED, DICT_FAIL_NETWORK` |
| `status == 401 \|\| 403` → `NOTE_FAIL_AUTH` | `DICT_FAILED, DICT_FAIL_AUTH` |
| other non-200 → `NOTE_FAIL_NETWORK` | `DICT_FAILED, DICT_FAIL_NETWORK` |

Add the call immediately after each existing `n->fail_reason = …`.

- [ ] **Step 4: Build + flash + smoke**

```bash
idf.py build 2>&1 | tail -5
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -3
python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac >/dev/null 2>&1
sleep 18
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" http://192.168.1.90:8080/info | head -c 200
```

Expected: device responds; no PANIC.

- [ ] **Step 5: Commit**

```bash
git add main/ui_notes.c
git commit -m "feat(dictation): instrument REST polling → UPLOADING/TRANSCRIBING/SAVED/FAILED"
```

---

### Task 14 — `/dictation_pipeline` debug endpoint

**Files:**
- Modify: `main/debug_server_dictation.c`

This gives PR 2 + PR 3 a way to verify the state machine works without a UI subscriber yet, and lets the live-verification step at the end of this PR confirm transitions are happening.

- [ ] **Step 1: Read the existing handler shape**

`main/debug_server_dictation.c` already registers `/dictation` GET + POST.  Add a sibling GET handler.

- [ ] **Step 2: Add the include + handler**

Top of file, near other includes:

```c
#include "voice_dictation.h"
```

Add a new handler function before `debug_server_dictation_register`:

```c
static esp_err_t pipeline_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   dict_event_t e = voice_dictation_get();

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "state",  voice_dictation_state_name(e.state));
   cJSON_AddStringToObject(root, "reason", voice_dictation_fail_name(e.fail_reason));
   cJSON_AddNumberToObject(root, "started_ms",     (double)e.started_ms);
   cJSON_AddNumberToObject(root, "stopped_ms",     (double)e.stopped_ms);
   cJSON_AddNumberToObject(root, "last_change_ms", (double)e.last_change_ms);
   cJSON_AddNumberToObject(root, "note_slot",      (double)e.note_slot);
   cJSON_AddNumberToObject(root, "now_ms",
                           (double)(esp_timer_get_time() / 1000));

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_sendstr(req, json);
   free(json);
   return ESP_OK;
}
```

In `debug_server_dictation_register`, after the existing handler registrations:

```c
   const httpd_uri_t uri_pipeline = {.uri = "/dictation_pipeline", .method = HTTP_GET, .handler = pipeline_handler};
   httpd_register_uri_handler(server, &uri_pipeline);
   ESP_LOGI(TAG, "Dictation pipeline endpoint registered");
```

- [ ] **Step 3: Build + flash + verify**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash 2>&1 | tail -3
python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac >/dev/null 2>&1
sleep 18
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/dictation_pipeline | python3 -m json.tool
```

Expected:

```json
{
    "state": "IDLE",
    "reason": "NONE",
    "started_ms": 0,
    "stopped_ms": 0,
    "last_change_ms": 0,
    "note_slot": -1,
    "now_ms": 8123
}
```

- [ ] **Step 4: Commit**

```bash
git add main/debug_server_dictation.c
git commit -m "feat(dictation): /dictation_pipeline GET debug endpoint"
```

---

### Task 15 — Live end-to-end verification

**Files:** None.  Verification only.

- [ ] **Step 1: Trigger an actual dictation + watch the pipeline transitions**

Open two terminals.

Terminal 1 (poll the pipeline every 0.4 s):

```bash
while true; do
  curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
       http://192.168.1.90:8080/dictation_pipeline | python3 -c "import sys,json; d=json.load(sys.stdin); print(f\"{d['state']:<14} reason={d['reason']:<10} note={d['note_slot']}\")"
  sleep 0.4
done
```

Terminal 2 (start a 5-second dictation via the debug endpoint):

```bash
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST "http://192.168.1.90:8080/dictation?action=start"
sleep 5
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST "http://192.168.1.90:8080/dictation?action=stop"
```

Expected sequence on Terminal 1:

```
IDLE            reason=NONE       note=-1
IDLE            reason=NONE       note=-1
RECORDING       reason=NONE       note=-1
RECORDING       reason=NONE       note=-1
…
TRANSCRIBING    reason=NONE       note=-1
TRANSCRIBING    reason=NONE       note=-1
SAVED           reason=NONE       note=-1
SAVED           reason=NONE       note=-1
```

(The exact sequence depends on whether Dragon is reachable.  Offline path: RECORDING → UPLOADING after the 15 s polling task picks the note up.)

- [ ] **Step 2: Trigger a failure case — disconnect Dragon while recording**

```bash
sshpass -p 'thedragon' ssh radxa@192.168.1.91 'sudo systemctl stop tinkerclaw-voice'

curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST "http://192.168.1.90:8080/dictation?action=start"
sleep 3
# Tab5 should detect WS disconnect; pipeline should transition to FAILED(NETWORK).
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/dictation_pipeline | python3 -m json.tool

sshpass -p 'thedragon' ssh radxa@192.168.1.91 'sudo systemctl start tinkerclaw-voice'
```

Expected after disconnect: `state: "FAILED", reason: "NETWORK"`.

- [ ] **Step 3: Trigger a notes-screen REST retry to verify the polling-task transitions fire**

Navigate to Notes, tap retry on a FAILED row.  Watch the pipeline state:

```bash
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST "http://192.168.1.90:8080/navigate?screen=notes"
# tap retry on a FAIL row via /touch — coordinates depend on the live row positions

# Concurrently:
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/dictation_pipeline | python3 -m json.tool
```

Expected: UPLOADING → TRANSCRIBING → SAVED (or FAILED with reason if the note was problematic).

- [ ] **Step 4: Final heap + state sanity**

```bash
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/heap | python3 -m json.tool | head -20
```

Internal free ≥ pre-PR baseline; no fragmentation alarm.

- [ ] **Step 5: Update memory with PR 1 completion**

Edit `~/.claude/projects/-home-rebelforce/memory/project_dictation_pipeline_redesign_2026_05_14.md` — change the "Status" section near the bottom to reflect PR 1 landed; note the live-verified transitions.

---

### Task 16 — Format-gate + push + open PR

**Files:** none (CI check).

- [ ] **Step 1: Run the format gate**

```bash
git fetch origin main:refs/remotes/origin/main
git-clang-format --binary clang-format-18 --diff origin/main \
   main/voice_dictation.h main/voice_dictation.c \
   main/voice.c main/voice_ws_proto.c main/ui_notes.c \
   main/debug_server_dictation.c main/main.c \
   tests/host/test_voice_dictation.c
```

Expected: empty diff or `clang-format did not modify any files`.  If non-empty:

```bash
git add -u
git-clang-format --binary clang-format-18 origin/main
git add -u
git commit --amend --no-edit
```

- [ ] **Step 2: File the GH issue + open the PR**

```bash
# Issue:
gh issue create --title "Wave: dictation pipeline state model + VAD + instrumentation (PR 1 of 4)" \
  --body "PR 1 of the dictation pipeline visibility + Notes reimagining design spec at docs/superpowers/specs/2026-05-14-dictation-pipeline-notes-redesign-design.md.

## Scope
Backend wiring only.  No UI changes.  Adds:
- main/voice_dictation.{c,h} — canonical state machine
- Instrumentation at every existing dictation entry/exit point (voice.c, voice_ws_proto.c, ui_notes.c)
- 5-min hard cap on the WS-streaming dictation path
- /dictation_pipeline GET debug endpoint for verification

## Verification
Host tests pass.  Live transitions verified on Tab5 192.168.1.90 ↔ Dragon 192.168.1.91 for all six states (IDLE, RECORDING, UPLOADING, TRANSCRIBING, SAVED, FAILED with each reason).

## Follow-ups
- PR 2: ui_orb.c + Dictate chip on home subscribe to this pipeline
- PR 3: ui_notes.c processing row at top of Timeline subscribes too
- PR 4: cross-stack action chips for reminder/list routing"

# Branch + push:
git checkout -b feat/dictation-pipeline-pr1
git push -u origin feat/dictation-pipeline-pr1

# PR:
gh pr create --title "feat(dictation): pipeline state model + VAD + instrumentation (PR 1 of 4)" \
  --body "\$(cat <<'EOF'
## Summary
- Adds canonical dict_pipeline_t state machine in new main/voice_dictation.{c,h}
- Instruments every existing dictation entry/exit point to drive transitions
- Adds 5-min hard cap to the WS-streaming dictation path (matches the SD cap from #517)
- Adds GET /dictation_pipeline debug endpoint for PR 2 + PR 3 verification

Builds the foundation for PR 2 (orb + Dictate chip) and PR 3 (Notes Timeline) without any visible UI change.

## Test plan
- [x] Host tests pass: ctest --test-dir build -R voice_dictation
- [x] Live state transitions verified on Tab5 192.168.1.90: IDLE → RECORDING → TRANSCRIBING → SAVED
- [x] Failure paths verified: WS disconnect mid-recording → FAILED(NETWORK); empty transcription → FAILED(EMPTY)
- [x] REST polling path emits UPLOADING → TRANSCRIBING → SAVED via retry on a FAIL note
- [x] git-clang-format --diff origin/main is clean

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Mark PR 1 done in memory + the spec**

Update `~/.claude/projects/-home-rebelforce/memory/MEMORY.md` index entry to reference PR number once `gh pr create` returns.

---

## Acceptance summary

When all 16 tasks are done:

- [ ] `voice_dictation.{c,h}` exists, fully tested by `test_voice_dictation`
- [ ] All 4 dictation entry/exit points (`voice_start_dictation`, `voice_stop_listening`, dictation_summary handler, WS disconnect) drive pipeline transitions
- [ ] REST polling task in `ui_notes.c` drives transitions
- [ ] 5-min hard cap on dictation
- [ ] `GET /dictation_pipeline` returns live state
- [ ] Host tests green, target build clean, format gate green
- [ ] Live transitions verified on Tab5 for happy path + at least one failure path
- [ ] PR open against `main`
- [ ] Memory updated reflecting PR 1 landed

PR 2 (orb + Dictate chip) gets its own plan once this lands and you've user-tested it.
