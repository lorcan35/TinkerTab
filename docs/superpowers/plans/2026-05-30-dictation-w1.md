# Dictation Redesign — W1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make `voice_dictation.c` own session identity (`turn_id`) and terminal-state liveness (self-decay), add `DICT_CANCELLED` as a first-class terminal, and give the two producers (live WS + offline REST queue) an atomic, mutually-exclusive claim — all without any wire or persistence change. Identity ships WITH decay so a 60–90 s-late summary can't be lost.

**Architecture:** The FSM gains fields (`origin`, `turn_id`, `resolution_pending`, `note_id`) and three new entry/resolve APIs (`voice_dictation_begin`, `voice_dictation_try_begin_offline`, `voice_dictation_resolve_if_current`). Self-decay is driven by a module `esp_timer` one-shot whose callback **enqueues onto `task_worker`** (never dispatches from the timer task — avoids the LVGL-thread crash class) and re-arms on a full queue. A single monotonic-ms accessor `voice_dictation_now_ms()` is the one clock, host-shimmable. Two new host shims (a fake-clock `esp_timer` + a synchronous `task_worker` stub) make decay deterministically testable.

**Tech Stack:** C11, ESP-IDF 5.5.2 (`esp_timer`, `task_worker`), CMake host tests under `tests/host/`, `idf.py build` for the target gate.

**Closes audit findings:** S1-3, S2-1, S2-7, S3-5. (S2-2 partial via guarded `FAILED→SAVED`, finalized W3. S2-8 deferred to W2 — needs Dragon turn_id echo.)

---

## File Structure

- `main/voice_dictation.h` — MODIFY: enums (`DICT_CANCELLED`), struct fields, new macros (`DICT_TURN_ID_LEN`, `DICT_NOTE_ID_LEN`, `DICT_ORIGIN_*`), new API decls.
- `main/voice_dictation.c` — MODIFY: transition table, `set_state` timestamp/decay logic, new APIs, self-decay timer + worker job, `voice_dictation_now_ms`, `voice_dictation_fail_caption`, `voice_turn_id_gen` (lives here — pure).
- `main/voice.c` — MODIFY: `gen_turn_id` delegates to `voice_turn_id_gen`; `TURN_ID_LEN`↔`DICT_TURN_ID_LEN` unified; `voice_start_dictation` routes through `voice_dictation_begin(WS,…)` (toast on NULL) and sends the FSM turn_id on `start`.
- `main/ui_notes.c` — MODIFY: offline `transcription_queue_task` gates via `voice_dictation_try_begin_offline` instead of `voice_get_state()`.
- `main/ui_orb.c` — MODIFY: delete `saved_fade_to_idle_cb` + its timer; render `DICT_CANCELLED`.
- `main/ui_home.c` — MODIFY: delete the FAILED/SAVED→IDLE auto-fade (≈:1940) + the pre-Ask FSM reset; render `DICT_CANCELLED`.
- `tests/host/shim/esp_timer.h` — CREATE: fake-clock one-shot registry + `host_clock_advance_ms()`.
- `tests/host/shim/task_worker.h` + `tests/host/shim/task_worker_stub.c` — CREATE: synchronous FIFO stub + `tab5_worker_pump()` + `tab5_worker_stub_set_full()`.
- `tests/host/CMakeLists.txt` — MODIFY: add the stub source + `-DDICT_HOST_TEST` to the `test_voice_dictation` target.
- `tests/host/test_voice_dictation.c` — MODIFY: rewrite `test_saved_to_recording_allowed`→`_now_refused`; add the W1 tests.

**Host/target seam:** `voice_dictation.c` does `#include "esp_timer.h"` and `#include "task_worker.h"`. The host build's `SHIM_DIR` is searched first, so the host picks up the shim `esp_timer.h`/`task_worker.h`; the target picks up the real ones. `voice_dictation_now_ms()` is `#ifdef DICT_HOST_TEST` → fake clock, `#else` → `esp_timer_get_time()/1000`.

---

## Task 1 — Host shims (fake-clock esp_timer + synchronous task_worker)

**Files:** Create `tests/host/shim/esp_timer.h`, `tests/host/shim/task_worker.h`, `tests/host/shim/task_worker_stub.c`. Modify `tests/host/CMakeLists.txt`.

- [ ] **Step 1.1 — esp_timer fake-clock shim.** Header-only registry (max 4 one-shots): `esp_timer_handle_t`, `esp_timer_create_args_t {callback, arg, name}`, `esp_timer_create`, `esp_timer_start_once(h, us)` (armed, `deadline_us = g_fake_now_us + us`), `esp_timer_stop`, `esp_timer_delete`, `esp_timer_get_time()`→`g_fake_now_us`. Plus test helpers: `void host_clock_set_ms(uint32_t)`, `void host_clock_advance_ms(uint32_t)` — the latter bumps the clock AND fires every armed one-shot whose `deadline_us <= g_fake_now_us` (disarm-then-call, matching real one-shot semantics). Globals `static` in the header guarded so single-TU inclusion is fine (the test exe is one TU + the stub TU; keep esp_timer state in the header as `static` and only the test TU advances it).
- [ ] **Step 1.2 — task_worker stub.** `task_worker.h` shim: `typedef void (*tab5_worker_fn_t)(void*)`, `esp_err_t tab5_worker_enqueue(fn,arg,tag)`, plus host-only `void tab5_worker_pump(void)` and `void tab5_worker_stub_set_full(bool)`. `task_worker_stub.c` implements a 16-slot FIFO: enqueue pushes (returns `ESP_OK`, or `ESP_ERR_NO_MEM` when `set_full(true)` or FIFO full — to exercise the re-arm path); `tab5_worker_pump` pops + calls each `fn(arg)` FIFO-order; never frees `arg` (matches real contract).
- [ ] **Step 1.3 — CMake.** Add `${SHIM_DIR}/task_worker_stub.c` to the `test_voice_dictation` executable and `target_compile_definitions(test_voice_dictation PRIVATE DICT_HOST_TEST=1)`. `esp_timer.h`/`task_worker.h` resolve via the existing `${SHIM_DIR}` include.
- [ ] **Step 1.4 — Build the (unchanged) suite.** Run `cmake -S tests/host -B tests/host/build && cmake --build tests/host/build && ctest --test-dir tests/host/build`. Expected: PASS (shims exist but `voice_dictation.c` doesn't use them yet — no behavior change). Commit: `test(dictation): host esp_timer fake-clock + task_worker stub shims`.

---

## Task 2 — FSM struct, enums, macros, transition table

**Files:** `main/voice_dictation.h`, `main/voice_dictation.c`, `tests/host/test_voice_dictation.c`.

- [ ] **Step 2.1 — Failing tests first.** In `test_voice_dictation.c`: rewrite `test_saved_to_recording_allowed`→`test_saved_to_recording_now_refused` (after SAVED, `set_state(RECORDING)` is refused; state stays SAVED). Add `test_cancelled_is_terminal_not_failed` (RECORDING→`set_state(DICT_CANCELLED)`; `e.state==DICT_CANCELLED`, `e.fail_reason==DICT_FAIL_NONE`). Wire both into `main()`. Build → expect FAIL (DICT_CANCELLED undefined / SAVED→RECORDING still allowed).
- [ ] **Step 2.2 — Header.** Add `DICT_CANCELLED` to `dict_state_t` (after `DICT_FAILED`). Add `typedef enum { DICT_ORIGIN_NONE=0, DICT_ORIGIN_WS, DICT_ORIGIN_OFFLINE } dict_origin_t;`. Add `#define DICT_TURN_ID_LEN 13` and `#define DICT_NOTE_ID_LEN 40`. Extend `dict_event_t` with `dict_origin_t origin; char turn_id[DICT_TURN_ID_LEN]; bool resolution_pending; char note_id[DICT_NOTE_ID_LEN];` (after `note_slot`, preserving existing field order). Keep `DICT_FAIL_CANCELLED` (ABI; removed W5). Declare `voice_dictation_now_ms`, `voice_dictation_fail_caption`, `voice_turn_id_gen`, and the three new entry/resolve APIs (signatures from the spec).
- [ ] **Step 2.3 — Transition table** (`dict_transition_allowed`): IDLE→RECORDING|UPLOADING(offline); RECORDING→UPLOADING|TRANSCRIBING|FAILED|CANCELLED; UPLOADING→TRANSCRIBING|SAVED|FAILED|CANCELLED; TRANSCRIBING→SAVED|FAILED|CANCELLED; SAVED→IDLE; FAILED→IDLE|SAVED|UPLOADING|RECORDING; CANCELLED→IDLE. (Remove `SAVED→RECORDING`; add `FAILED→SAVED`, `UPLOADING→SAVED`, `IDLE→UPLOADING`, all CANCELLED edges.) `next==DICT_CANCELLED` allowed from any non-IDLE, non-terminal-already state. `next==DICT_IDLE` allowed from SAVED|FAILED|CANCELLED|RECORDING.
- [ ] **Step 2.4 — set_state bookkeeping.** `fail_reason` cleared unless `new_state==DICT_FAILED`. On `DICT_IDLE`: also clear `origin=NONE`, `turn_id[0]=0`, `resolution_pending=false`, `note_id[0]=0` (full session reset). Update `voice_dictation_state_name`/`fail_name` for `DICT_CANCELLED`.
- [ ] **Step 2.5 — `voice_dictation_now_ms`** in voice_dictation.c: `#ifdef DICT_HOST_TEST` → `extern` fake-clock read (`esp_timer_get_time()/1000` against the shim works too — the shim's `esp_timer_get_time` returns the fake clock, so a single `return (uint32_t)(esp_timer_get_time()/1000);` body works for BOTH builds). Use the unified body. Replace internal `now_ms` derivations as needed.
- [ ] **Step 2.6 — `voice_dictation_fail_caption(dict_fail_t)`** — pure function returning the user-facing caption per reason ("Couldn't reach Dragon", "Nothing heard", etc.); host-testable. (Renderers append state suffixes later — W4/W5.)
- [ ] **Step 2.7 — Build host + target-compile-sanity.** `ctest` green (the two new/rewritten tests pass; the existing 18 still pass — note `test_full_happy_path` etc. are unaffected; the FSM additions are additive). Then `idf.py build` to confirm the header changes compile against the target (existing callers still pass 3-arg `set_state`). Commit: `feat(dictation): FSM identity fields + CANCELLED terminal + transition table rewrite (W1)`.

---

## Task 3 — Self-liveness (decay via task_worker) + resolution_pending

**Files:** `main/voice_dictation.c`, `tests/host/test_voice_dictation.c`.

- [ ] **Step 3.1 — Failing tests.** Add: `test_saved_decays_to_idle` (set SAVED; `host_clock_advance_ms(DICT_DECAY_SAVED_MS+1)`; `tab5_worker_pump()`; state==IDLE). `test_failed_decays_to_idle` (FAILED→…→IDLE after `DICT_DECAY_FAILED_MS`). `test_cancelled_decays_to_idle`. `test_decay_suppressed_while_resolution_pending` (set `resolution_pending` via a WS begin+stop sim; advance+pump; stays terminal). `test_decay_requeues_on_full_worker` (`tab5_worker_stub_set_full(true)`; advance fires timer→enqueue fails→re-arm; `set_full(false)`; advance `DICT_DECAY_RETRY_MS`; pump; IDLE). Build → FAIL.
- [ ] **Step 3.2 — Decay machinery.** Add `#define DICT_DECAY_SAVED_MS 2000`, `DICT_DECAY_FAILED_MS 5000`, `DICT_DECAY_CANCELLED_MS 1500`, `DICT_DECAY_RETRY_MS 250`. Module `static esp_timer_handle_t s_dict_decay_timer`. On entering a terminal state in `set_state`, if `!resolution_pending`, arm the one-shot for the per-state delay (create lazily, mirroring voice.c:2239-2248). `dict_decay_timer_cb` → `tab5_worker_enqueue(dict_decay_apply_job, NULL, "dict_decay")`; on `!= ESP_OK`, `esp_timer_start_once(s_dict_decay_timer, DICT_DECAY_RETRY_MS*1000)`. `dict_decay_apply_job` → snapshot; if `DICT_IS_TERMINAL(state) && !resolution_pending` → `set_state(DICT_IDLE, NONE, now_ms())`. Add `#define DICT_IS_TERMINAL(s) ((s)==DICT_SAVED||(s)==DICT_FAILED||(s)==DICT_CANCELLED)`. Disarm on leaving terminal / on IDLE.
- [ ] **Step 3.3 — Build + ctest green.** `idf.py build` (links real esp_timer + task_worker). Commit: `feat(dictation): FSM-owned terminal self-decay via task_worker (W1)`.

---

## Task 4 — Entry/resolve APIs (begin / try_begin_offline / resolve_if_current)

**Files:** `main/voice_dictation.c`, `tests/host/test_voice_dictation.c`.

- [ ] **Step 4.1 — Failing tests.** `test_begin_ws_mints_turn_id` (begin(WS) → RECORDING + non-empty turn_id; returns ptr). `test_begin_refused_during_offline_upload` (try_begin_offline → UPLOADING; begin(WS) returns NULL, state unchanged). `test_try_begin_offline_refused_when_live` (begin(WS) live → try_begin_offline returns false). `test_try_begin_offline_atomic_vs_decay` (SAVED; advance past decay so timer-cb enqueued but NOT pumped; try_begin_offline — deterministic per lock; pump; no double-apply). `test_resolve_if_current_drops_stale` (begin(WS) id=A; resolve_if_current("B",SAVED) refused; resolve_if_current(<A>,SAVED) applied). `test_missing_turn_id_treated_as_match` (resolve_if_current("" , …) applies). `test_failed_to_saved_late_correction_ws_only`. Build → FAIL.
- [ ] **Step 4.2 — Implement** `voice_turn_id_gen(char out[DICT_TURN_ID_LEN])` (12 hex + NUL; host: deterministic counter under `DICT_HOST_TEST`, target: from `esp_random()`/time — keep it local, no globals). `voice_dictation_begin(origin, adopt_turn_id, now_ms)`: under lock, refuse (return NULL) if a turn of a different origin is live (`state!=IDLE && state not terminal`); else mint (or adopt) turn_id, set origin, transition to RECORDING (WS) and return the FSM turn_id ptr. `voice_dictation_try_begin_offline(adopt_turn_id, note_slot, now_ms)`: under ONE lock take, return false unless `state` is IDLE or terminal; else set origin=OFFLINE, copy turn_id, set note_slot, `resolution_pending=true`, transition IDLE→UPLOADING; return true. `voice_dictation_resolve_if_current(turn_id, st, reason, now_ms)`: under lock, match if `turn_id` empty OR equals live `turn_id`; on match clear `resolution_pending` and `set_state(st,reason,now_ms)`, return applied bool.
- [ ] **Step 4.3 — Build + ctest green.** `idf.py build`. Commit: `feat(dictation): begin/try_begin_offline/resolve_if_current identity APIs (W1)`.

---

## Task 5 — Wire into target (voice.c, ui_notes.c, ui_orb.c, ui_home.c)

**Files:** `main/voice.c`, `main/ui_notes.c`, `main/ui_orb.c`, `main/ui_home.c`.

- [ ] **Step 5.1 — voice.c turn_id unification.** Replace `gen_turn_id`'s body to call `voice_turn_id_gen(s_current_turn_id)` (keep the `tab5_debug_obs_event("turn.start", …)` side-effect). `#define TURN_ID_LEN DICT_TURN_ID_LEN` (include voice_dictation.h) + `_Static_assert(TURN_ID_LEN==13, …)`.
- [ ] **Step 5.2 — voice_start_dictation.** Route through `const char *tid = voice_dictation_begin(DICT_ORIGIN_WS, NULL, voice_dictation_now_ms());` — on `NULL` (cross-origin busy), toast "Busy transcribing — try again" (reuse the TT #621 R7 mic-busy toast path) and return; on success copy `tid` into `s_current_turn_id` so the `start` frame carries it. (The `start` frame already sends `s_current_turn_id`.)
- [ ] **Step 5.3 — ui_notes.c offline queue.** In `transcription_queue_task`, replace the `voice_get_state()` gate (≈:1620) with: mint nothing here — call `voice_dictation_try_begin_offline(<gen turn_id>, slot, voice_dictation_now_ms())`; if it returns false, `continue` (skip this 15 s tick). (turn_id is threaded onto the REST POST in W4; W1 only needs the atomic claim.)
- [ ] **Step 5.4 — UI deletions + CANCELLED render.** ui_orb.c: delete `saved_fade_to_idle_cb` + `s_saved_fade_timer` arm/teardown; add a `DICT_CANCELLED` case to the orb pipeline renderer (neutral grey, "Cancelled", no retry CTA). ui_home.c: delete the FAILED/SAVED→IDLE auto-fade block (≈:1940) and the pre-Ask FSM reset; add `DICT_CANCELLED` to its state switch (≈:2388) rendering neutrally.
- [ ] **Step 5.5 — Build + format.** `idf.py build` green; `git-clang-format --diff origin/main` clean on all touched `.c/.h`. Commit: `feat(dictation): wire FSM identity + self-decay into voice/notes/orb/home (W1)`.

---

## Task 6 — W1 acceptance

- [ ] **Step 6.1 — Full host suite + target build.** `ctest --test-dir tests/host/build` all green (incl. all new W1 tests + the rewritten one); `idf.py build` green; clang-format clean.
- [ ] **Step 6.2 — Adversarial review.** Run a review workflow over the W1 diff (thread-safety/crash-class lens — confirm no `lv_*` reachable from `dict_decay_timer_cb`; FSM-correctness lens; audit-coverage lens for S1-3/S2-1/S2-7/S3-5). Address confirmed findings.
- [ ] **Step 6.3 — Live-verify checklist (deferred to a flash session):** SAVED→IDLE ~2 s and FAILED→IDLE ~5 s with NO dictation UI mounted (poll `/dictation_pipeline`); offline queue drains ≥3 queued notes; a turn awaiting resolution does NOT decay; starting a WS dictation while an offline upload is mid-flight is refused with a toast.

---

## Self-Review notes
- **Spec coverage:** every W1 finding (S1-3, S2-1, S2-7, S3-5) maps to a task (S1-3→T4/T5.3, S2-1→T3, S2-7→T2/T5.4, S3-5→T2.1/T2.3). S2-2 is only *partially* addressed in W1 (guarded `FAILED→SAVED`); the cross-turn guard becomes authoritative in W2 — noted, not claimed closed.
- **Type consistency:** `DICT_TURN_ID_LEN`(13) ↔ `TURN_ID_LEN`(13) reconciled via `#define` + `_Static_assert`; `voice_dictation_now_ms()` is the single clock used by decay, callers, and tests; `DICT_NOTE_ID_LEN`(40) is provisional and re-pinned against the longest real Dragon id in W4.
- **No placeholders:** each step names the file, the symbol, and the acceptance command. Novel code (shims, decay, APIs) is specified by interface + behavior; final source lands in the files during execution.
