# Dictation Redesign — W2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans.

**Goal:** Activate identity end-to-end: one cancel/finalize chokepoint that resolves the FSM (S1-5), turn_id-gated resolution so stale/cross-turn frames are dropped (S2-8/S2-9), start-side WS-failure symmetry (S2-10), and the origin-discipline cluster from the W1 review (F3/F5/F6). The W1 `origin==WS` qualifier on FAILED→SAVED is replaced by the now-authoritative turn_id match.

**Architecture:** Tab5 sends `turn_id` on `segment`/`stop`/`cancel` and resolves WS terminals via `voice_dictation_resolve_if_current(turn_id,…)`. A new `voice_dictation_finalize()` in voice.c is the single resolver (stops mic, snaps voice_state, drives the FSM terminal). Dragon captures the turn's id at `finish_dictation` and stamps it on the async post-process emits so a late summary can't be mis-stamped with the next turn's id (the real S2-8 root cause on the server).

**Tech Stack:** C11 / ESP-IDF (Tab5), Python/pytest (Dragon), host CMake tests, `idf.py build`.

**Closes:** S1-5, S2-8, S2-9, S2-10 + review F3/F5/F6.

---

## File Structure
- `main/voice_dictation.{c,h}` — F5 (begin same-origin: return existing id, don't re-mint), F6 helper, drop the `origin==WS` qualifier on FAILED→SAVED (turn_id is authoritative now). Host tests.
- `main/voice.c` — `voice_dictation_finalize(terminal, reason, turn_id)`; `voice_cancel` calls it for dictate mode (S1-5); start-side `start`-send failure → finalize(FAILED,NETWORK) (S2-10); send `turn_id` on `segment`/`stop`/`cancel`; offline RECORDING begins with origin=OFFLINE (F6).
- `main/voice_ws_proto.c` — `dictation_summary`/`dictation_empty?`/`dictation_postprocessing_error`/`_cancelled` handlers extract the echoed `turn_id` and call `voice_dictation_resolve_if_current` (S2-8/S2-9); WS-disconnect fail gated on `origin==DICT_ORIGIN_WS` (F3).
- `main/debug_server_dictation.c` — add `turn_id`/`origin`/`resolution_pending`/`note_id` to `/dictation_pipeline` JSON.
- Dragon `dragon_voice/pipeline.py`, `dictation_post.py`, `stop_handler.py` — capture turn_id at finish, thread to the post-process emits + note_created. pytest.

---

## Task 1 — FSM: F5 + F6 helper + drop the origin==WS qualifier (host-tested)
- [ ] **1.1 Failing tests.** `test_begin_same_origin_keeps_turn_id` (begin WS twice → 2nd returns the SAME id, state stays RECORDING). `test_failed_to_saved_cross_turn_dropped` (WS turn A → TRANSCRIBING → FAILED; resolve_if_current("WRONG", SAVED) refused even though pending; resolve_if_current(idA, SAVED) applied). Build → FAIL.
- [ ] **1.2 begin same-origin (F5).** In `voice_dictation_begin`, if a turn of the SAME origin is already in-flight (RECORDING/UPLOADING/TRANSCRIBING), return the EXISTING `s_event.turn_id` WITHOUT re-minting or transitioning (it's a no-op re-entry, not a new turn). Only mint on a transition from IDLE/terminal.
- [ ] **1.3 Drop origin==WS qualifier.** In `set_state`, change the FAILED→SAVED guard from `(resolution_pending && origin==WS)` to just `resolution_pending` — the cross-turn safety now comes from `resolve_if_current`'s turn_id match (the only caller that should drive FAILED→SAVED). Keep the `resolution_pending` gate (a SAVED with no pending resolution is still a stray). Update `test_failed_to_saved_refused_when_not_pending` (still valid) and the W1 `_ws_only` test name/semantics.
- [ ] **1.4 Build host + target.** ctest green; idf build green. Commit.

## Task 2 — voice_dictation_finalize + voice_cancel (S1-5) + frame turn_ids (S2-10)
- [ ] **2.1 finalize.** Add to voice.c: `void voice_dictation_finalize(dict_state_t terminal, dict_fail_t reason, const char *turn_id)` — stop mic if running, drive the FSM via `voice_dictation_resolve_if_current(turn_id, terminal, reason, now)`, then snap voice_state (READY if WS up else IDLE). Idempotent.
- [ ] **2.2 voice_cancel (S1-5).** After voice_cancel's existing voice_state logic, if `voice_get_mode()==VOICE_MODE_DICTATE` (or the FSM has a live turn), call `voice_dictation_finalize(DICT_CANCELLED, DICT_FAIL_NONE, voice_dictation_get().turn_id)` as the LAST step. Send `turn_id` on the `cancel` frame.
- [ ] **2.3 Frame turn_ids.** `segment`/`stop` frames carry `"turn_id":"<s_current_turn_id>"` (forward-compat; Dragon's echo already uses conn_state, but this lets it disambiguate). 4hr-cap stop + offline finalise route through the same shape.
- [ ] **2.4 Start-side symmetry (S2-10).** In `voice_start_dictation`, if the `start`-frame `voice_ws_send_text` fails, `voice_dictation_finalize(DICT_FAILED, DICT_FAIL_NETWORK, tid)` (mirror the stop-side handling).
- [ ] **2.5 F6.** The offline-fallback RECORDING path sets `origin=DICT_ORIGIN_OFFLINE` (begin via a new `voice_dictation_begin(DICT_ORIGIN_OFFLINE,…)` OR set origin before the RECORDING set_state) so the collision guard sees it.
- [ ] **2.6 Build target.** idf build green; clang-format. Commit.

## Task 3 — Tab5 WS resolution via resolve_if_current (S2-8/S2-9) + F3
- [ ] **3.1 Resolution handlers.** In voice_ws_proto.c, the `dictation_summary` / `dictation_postprocessing_error` handlers extract `turn_id` from the frame and apply their terminal via `voice_dictation_resolve_if_current(turn_id, DICT_SAVED, …)` instead of direct `voice_dictation_set_state` — so a stale/cross-turn frame is dropped.
- [ ] **3.2 F3 — disconnect gate.** The WS-disconnect handler (voice_ws_proto.c ~1668) only fails the dictation FSM when `voice_dictation_get().origin == DICT_ORIGIN_WS` (an in-flight OFFLINE REST upload must not be failed by a WS drop).
- [ ] **3.3 Build target.** idf build green; clang-format. Commit.

## Task 4 — Dragon: capture turn_id at finish, stamp the async post-process (S2-8 server root cause)
- [ ] **4.1 Failing pytest.** `test_late_postprocess_stamps_its_own_turn_id`: finish_dictation for turn A captures id A; even if conn_state["turn_id"] is later set to B, the dictation_summary/empty/error frames carry A. (Simulate by mutating conn_state between finish and the emit.)
- [ ] **4.2 Capture + thread.** `finish_dictation` reads `conn_state["turn_id"]` (or the pipeline's stored id) at finish time into a local; pass it into `_post_process_dictation(transcript, turn_id=…)` → `run_dictation_post_process(…, turn_id=…)`, which sets `event["turn_id"] = turn_id` on every emitted frame (the callback's `if "turn_id" not in event` guard then leaves it). stop_handler's `note_created` stamps the same captured id.
- [ ] **4.3 pytest.** New test + existing dictation tests green; ruff gate clean. Commit.

## Task 5 — debug_server_dictation JSON + W2 acceptance
- [ ] **5.1 JSON.** `/dictation_pipeline` adds `turn_id`, `origin` (string), `resolution_pending` (bool), `note_id` — additive.
- [ ] **5.2 Acceptance.** Full host suite + idf build + Dragon dictation pytest green; clang-format clean; adversarial review workflow over the W2 diff; address must-fixes.
- [ ] **5.3 Live-verify (deferred to flash):** cancel mid-dictation → neutral "Cancelled"; back-to-back A/B → A's late summary dropped, B unaffected; offline upload survives a WS blip.

---

## Self-Review
- Coverage: S1-5→T2.2; S2-8→T3.1 (Tab5 drop) + T4.2 (Dragon stamp); S2-9→T3.1; S2-10→T2.4; F3→T3.2; F5→T1.2; F6→T2.5.
- Type consistency: `voice_dictation_finalize` lives in voice.c (touches voice_state); the FSM-only `resolve_if_current` it calls is already host-tested. turn_id is the 12-hex DICT_TURN_ID_LEN string throughout.
- Host blind spot: finalize + the voice_ws_proto wiring aren't host-unit-testable (voice.c/voice_ws_proto.c aren't in the host build) — verified by idf build + the Dragon pytest + live. The host-testable FSM bits (F5, qualifier, resolve gating) get tests in T1.
