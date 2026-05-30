# Dictation Optimistic Save (W4) — Implementation Plan

> **For agentic workers:** Execute tasks in order. Each task is independently committable. Run the exact commands shown; the expected pass/fail output is given so you can verify before moving on. Never claim a step passed without running its verification command. All paths are absolute. Commit after every task with the shown message. Reference "W4" in every commit.

**Goal:** Make stopping a dictation feel instantaneous. The orb returns to idle the moment recording stops; a quiet `Saved — summarizing…` toast fires; a note row for the live `turn_id` appears immediately in Notes with an enrichment badge; transcription/title/summary fill in afterward in the background — online via Dragon's `note_created` + `dictation_summary` (reconciled by `turn_id`, no duplicate row), or offline via the existing SD-WAV transcription queue. No manual retry, no lost dictation. Dragon stays the sole authoritative note owner (D-D1); the Tab5 placeholder is a transient view reconciled by `turn_id`.

**Architecture:** Two decoupled layers.
- **Capture** owns the orb (`voice_dictation.c` FSM). `ui_orb_pipeline_active()` flips from `state != DICT_IDLE` to **`DICT_RECORDING`-only**, so the orb releases at stop. The FSM's `turn_id` / `resolution_pending` / self-decay / W3 stuck-watchdog stay as-is for FSM liveness; they no longer drive the orb.
- **Enrichment** owns a per-note badge (`ui_notes.c` + Dragon). A `note_entry_t` gains a `turn_id` field and an `enrich_state` field. The badge renders from `enrich_state` until `ENRICH_DONE`. `note_created` reconciles by `turn_id` (adopt `note_id`, no dup row); `dictation_summary` is applied as an in-place update; the offline transcription queue auto-finishes pending notes on Dragon reconnect.

**Tech Stack:** TinkerTab — C / ESP-IDF v5.5.2, LVGL 9.2.2, host unit tests via CMake + ctest (`tests/host/`). TinkerBox — Python 3 / aiohttp / asyncio, pytest. CI gates: TinkerTab runs `git-clang-format --diff origin/main` on changed C/H lines (your diff must be clean). TinkerBox runs `ruff --select F821,F722,F811,F823,B006,B904,E722,B007,RUF006` + a named pytest set in `.github/workflows/ci.yml`.

---

## File Structure

| File | Responsibility / Change |
|------|--------------------------|
| `/home/rebelforce/projects/TinkerTab/main/ui_orb.c` | **Modify** `ui_orb_pipeline_active()` (line 2628) → `DICT_RECORDING`-only. Orb releases at stop. |
| `/home/rebelforce/projects/TinkerTab/tests/host/test_voice_dictation.c` | **Modify** add host tests asserting the orb-active predicate maps to RECORDING-only and turn_id is stable across capture→enrichment handoff (predicate logic extracted as a pure inline). |
| `/home/rebelforce/projects/TinkerTab/main/voice.c` | **Modify** `voice_stop_listening()` (online dictate branch, ~line 2174): fire `ui_home_show_toast("Saved — summarizing…")` and seed the optimistic note row at stop. |
| `/home/rebelforce/projects/TinkerTab/main/ui_notes.c` | **THE BIG ONE.** Add `enrich_state_t` + `turn_id` + `note_id` fields to `note_entry_t`; add `ui_notes_seed_optimistic(turn_id)`, `ui_notes_reconcile_note_created(turn_id, note_id, title)`, `ui_notes_apply_summary(turn_id, title, summary)`; render the enrichment badge in `add_note_card_sectioned` (line ~2730); persist the new fields; background re-drive `ENRICH_PENDING` notes in `transcription_queue_task`. |
| `/home/rebelforce/projects/TinkerTab/main/ui_notes.h` | **Modify** declare the three new public functions. |
| `/home/rebelforce/projects/TinkerTab/main/voice_ws_proto.c` | **Modify** `note_created` handler (line 1008) → call `ui_notes_reconcile_note_created`; `dictation_summary` handler (line 928) → call `ui_notes_apply_summary` by `turn_id`. |
| `/home/rebelforce/projects/TinkerBox/dragon_voice/stop_handler.py` | **Verify** `note_created` already carries `turn_id` (it does, line 172). Harden test coverage. |
| `/home/rebelforce/projects/TinkerBox/tests/test_stop_handler.py` | **Modify** add assertions that `note_created` carries `turn_id`. |
| `/home/rebelforce/projects/TinkerBox/dragon_voice/notes/service.py` | **Modify** `_embed_note` → retry on failure so an embedding error never degrades the note (already non-blocking; add bounded background retry). |
| `/home/rebelforce/projects/TinkerBox/tests/test_notes_embed_retry.py` | **Create** new test: an embedding failure does not fail the note insert and the embed retries. |
| `/home/rebelforce/projects/TinkerBox/.github/workflows/ci.yml` | **Modify** add the new test file to the named CI set. |

**Task ordering rationale:** Orb decoupling + toast first (Tasks 1–3) — small, independent, immediately improve the feel; each is independently shippable. Then the `ui_notes.c` enrichment data model + reconcile + badge core (Tasks 4–8). Then the proto wiring (Task 9). Then background auto-finish (Task 10). Then the Dragon `turn_id` verification + non-blocking-embedding (Tasks 11–13). **Independently shippable:** Task 1+2 (orb), Task 3 (toast), Task 11 (Dragon turn_id test), Task 12+13 (Dragon embed-retry).

---

## Task 1 — Orb releases at stop: `ui_orb_pipeline_active()` → RECORDING-only (host TDD)

The orb's active predicate is the single line `voice_dictation_state() != DICT_IDLE` at `ui_orb.c:2628`. The spec wants it true for `DICT_RECORDING` only. The decision is pure (a function of the FSM state enum), so we extract it as a host-testable pure helper in `voice_dictation.c` and call it from the orb.

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/voice_dictation.h` (after the `voice_dictation_state()` decl, line 145)
- Modify `/home/rebelforce/projects/TinkerTab/main/voice_dictation.c` (after `voice_dictation_state()`, line 504)
- Modify `/home/rebelforce/projects/TinkerTab/tests/host/test_voice_dictation.c` (add tests + register in `main()`)

Steps:
- [ ] In `/home/rebelforce/projects/TinkerTab/main/voice_dictation.h`, add the declaration after line 145 (`dict_state_t voice_dictation_state(void);`):
  ```c
  /* W4: pure predicate — does this FSM state mean "capture is actively running
   * and should hold the orb"?  RECORDING only.  At stop the FSM moves to
   * TRANSCRIBING/SAVED/etc. (background note states), so the orb releases.
   * Lock-free + pure; safe from the orb paint hot-path. */
  bool voice_dictation_orb_active(dict_state_t s);
  ```
- [ ] In `/home/rebelforce/projects/TinkerTab/main/voice_dictation.c`, add the definition immediately after `voice_dictation_state()` (after line 504):
  ```c
  bool voice_dictation_orb_active(dict_state_t s) { return s == DICT_RECORDING; }
  ```
- [ ] Add a failing host test in `/home/rebelforce/projects/TinkerTab/tests/host/test_voice_dictation.c` before `int main(void)` (after `test_failed_to_saved_refused_when_not_pending`, line 665):
  ```c
  static int test_orb_active_recording_only(void) {
     /* W4: the orb follows capture ONLY.  RECORDING holds the orb; every other
      * state (incl. the at-stop TRANSCRIBING/UPLOADING/SAVED) releases it so the
      * orb snaps back to idle the instant recording stops. */
     CHECK(voice_dictation_orb_active(DICT_RECORDING));
     CHECK(!voice_dictation_orb_active(DICT_IDLE));
     CHECK(!voice_dictation_orb_active(DICT_UPLOADING));
     CHECK(!voice_dictation_orb_active(DICT_TRANSCRIBING));
     CHECK(!voice_dictation_orb_active(DICT_SAVED));
     CHECK(!voice_dictation_orb_active(DICT_FAILED));
     CHECK(!voice_dictation_orb_active(DICT_CANCELLED));
     return 0;
  }

  static int test_orb_releases_at_stop(void) {
     /* End-to-end: begin a WS turn (orb active), then drive the at-stop
      * TRANSCRIBING — orb_active must be false even though the FSM is still
      * resolving the turn (resolution_pending, turn_id intact). */
     voice_dictation_init();
     host_test_reset();
     const char *tid = voice_dictation_begin(DICT_ORIGIN_WS, NULL, 1000);
     CHECK(tid != NULL);
     CHECK(voice_dictation_orb_active(voice_dictation_state())); /* RECORDING */
     voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, 2000);
     CHECK(!voice_dictation_orb_active(voice_dictation_state())); /* released */
     dict_event_t e = voice_dictation_get();
     CHECK(e.resolution_pending);                 /* FSM still resolving */
     CHECK(strcmp(e.turn_id, tid) == 0);          /* turn_id stable across handoff */
     return 0;
  }
  ```
- [ ] Register both in `main()` (after the `test_failed_to_saved_refused_when_not_pending()` line, ~line 706):
  ```c
     if (test_orb_active_recording_only()) return 1;
     if (test_orb_releases_at_stop()) return 1;
  ```
- [ ] Build + run the host test — expect it to PASS now (the helper exists). Run:
  ```bash
  cmake -S /home/rebelforce/projects/TinkerTab/tests/host -B /home/rebelforce/projects/TinkerTab/tests/host/build && cmake --build /home/rebelforce/projects/TinkerTab/tests/host/build && ctest --test-dir /home/rebelforce/projects/TinkerTab/tests/host/build -R voice_dictation --output-on-failure
  ```
  Expected: `voice_dictation .......... Passed` and `100% tests passed`. (To see the TDD red first: temporarily comment out the `voice_dictation_orb_active` definition → the test target fails to link with an undefined-reference error.)
- [ ] Format-check the changed C lines:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git fetch origin main:refs/remotes/origin/main 2>/dev/null; git-clang-format --binary clang-format-18 --diff origin/main main/voice_dictation.c main/voice_dictation.h
  ```
  Expected: empty / "did not modify any files".
- [ ] Commit:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git add main/voice_dictation.c main/voice_dictation.h tests/host/test_voice_dictation.c && git commit -m "feat(orb): add voice_dictation_orb_active RECORDING-only predicate (W4)"
  ```

---

## Task 2 — Wire the orb to the new predicate

Replace the orb's inline `!= DICT_IDLE` check with the new pure helper.

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_orb.c` (`ui_orb_pipeline_active`, line 2614–2629)

Steps:
- [ ] In `/home/rebelforce/projects/TinkerTab/main/ui_orb.c`, change the return at line 2628 from:
  ```c
     return voice_dictation_state() != DICT_IDLE;
  ```
  to:
  ```c
     /* W4: orb follows CAPTURE only — RECORDING holds the orb; at stop the FSM
      * moves to background note states (TRANSCRIBING/SAVED/…) and the orb snaps
      * back to idle instantly while enrichment continues in the Notes badge. */
     return voice_dictation_orb_active(voice_dictation_state());
  ```
- [ ] Build the firmware to confirm it compiles:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -20
  ```
  Expected: `Project build complete.`
- [ ] Format-check:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git-clang-format --binary clang-format-18 --diff origin/main main/ui_orb.c
  ```
  Expected: empty.
- [ ] Live-verify (flash, then drive a dictation via the debug server). Flash:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py -p /dev/ttyACM0 flash
  ```
  Then (token `05eed3b13bf62d92cfd8ac424438b9f2`, IP `192.168.1.90`):
  ```bash
  export T=05eed3b13bf62d92cfd8ac424438b9f2 H=192.168.1.90:8080
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/dictation?action=start"; sleep 2
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/dictation?action=stop"
  # immediately snapshot the pipeline + a screenshot
  curl -s -H "Authorization: Bearer $T" "http://$H/dictation_pipeline" | python3 -m json.tool
  curl -s -H "Authorization: Bearer $T" -o /tmp/orb_after_stop.jpg "http://$H/screenshot.jpg"
  ```
  **Pass:** within ~1 frame of the stop, the orb shows its idle/ready visuals (not the amber "TRANSCRIBING" spinner) in `/tmp/orb_after_stop.jpg`, even while `/dictation_pipeline` may still report `TRANSCRIBING` + `resolution_pending: true`. **Fail:** orb still shows the amber processing spinner after stop.
- [ ] Commit:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git add main/ui_orb.c && git commit -m "feat(orb): orb follows capture only, releases at dictation stop (W4)"
  ```

---

## Task 3 — `Saved — summarizing…` toast on stop (online dictate branch)

The online dictate stop branch in `voice.c` currently drives `DICT_TRANSCRIBING` then `VOICE_STATE_PROCESSING` (lines 2169–2179) with no toast. `ui_home_show_toast` is already marshaled onto the ui_task (declared in `ui_home.h:32`, defined `ui_home.c:2933`). Fire it here.

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/voice.c` (online dictate branch in `voice_stop_listening`, lines 2169–2179)

Steps:
- [ ] In `/home/rebelforce/projects/TinkerTab/main/voice.c`, inside the `if (voice_get_mode() == VOICE_MODE_DICTATE)` block at lines 2174–2176, after the `voice_dictation_set_state(DICT_TRANSCRIBING, …)` call (line 2175), add the toast + optimistic-row seed:
  ```c
     if (voice_get_mode() == VOICE_MODE_DICTATE) {
        voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, (uint32_t)(esp_timer_get_time() / 1000));
        /* W4 (D-UX1): capture is done the instant we send `stop`.  Quiet toast +
         * an optimistic note row keyed by this turn_id; Dragon's note_created +
         * dictation_summary reconcile it by turn_id (no dup row). */
        ui_home_show_toast("Saved — summarizing…");
        ui_notes_seed_optimistic(s_current_turn_id);
     }
  ```
  (`ui_notes_seed_optimistic` is added in Task 5; this line will not compile until Task 5 lands — that is intentional sequencing. If you want Task 3 independently buildable, add only the `ui_home_show_toast` line here and add the `ui_notes_seed_optimistic` call in Task 5's wiring step.)
- [ ] Confirm `ui_notes.h` is included in `voice.c` (it is — `voice.c:56` includes `ui_home.h`; verify the notes include):
  ```bash
  grep -n '#include "ui_notes.h"' /home/rebelforce/projects/TinkerTab/main/voice.c
  ```
  If absent, add `#include "ui_notes.h"` next to the `ui_home.h` include at line 56.
- [ ] (If shipping Task 3 alone) Build with just the toast line:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -5
  ```
  Expected: `Project build complete.`
- [ ] Format-check + commit:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git-clang-format --binary clang-format-18 --diff origin/main main/voice.c && git add main/voice.c && git commit -m "feat(voice): fire 'Saved — summarizing…' toast on dictation stop (W4)"
  ```

---

## Task 4 — Enrichment data model: `enrich_state_t` + `turn_id` + `note_id` on `note_entry_t`

Add the per-note enrichment lifecycle and identity fields. These are pure struct + enum additions plus a persistence round-trip.

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_notes.c` (enum block ~line 94, struct ~line 173, persistence save ~line 533, persistence load ~line 724)

Steps:
- [ ] In `/home/rebelforce/projects/TinkerTab/main/ui_notes.c`, after the `note_state_t` enum (line 101), add:
  ```c
  /* W4: per-note enrichment lifecycle, independent of the orb/FSM.  Surfaced as
   * a small badge on the row until ENRICH_DONE.  Mirrors what Dragon reports:
   *   transcribing…  →  summarizing…  →  done  (badge clears)
   *                  ↘  pending (Dragon away)  ↗  (auto-finishes on reconnect) */
  typedef enum {
     ENRICH_NONE = 0,      /* not an in-flight dictation row (plain note) */
     ENRICH_TRANSCRIBING,  /* audio captured, transcript not final */
     ENRICH_SUMMARIZING,   /* transcript final, title/summary pending */
     ENRICH_PENDING,       /* Dragon unreachable; auto-finishes on reconnect */
     ENRICH_DONE,          /* title + summary present — badge clears */
  } enrich_state_t;
  ```
- [ ] In the `note_entry_t` struct (lines 173–188), add three fields before the closing `} note_entry_t;` (after `bool needs_sync;`, line 187):
  ```c
     bool needs_sync;  /* S6: true if not yet synced to Dragon */
     /* ── W4 optimistic-save + reconcile-by-turn_id ── */
     enrich_state_t enrich;          /* ENRICH_NONE for non-dictation rows */
     char turn_id[DICT_TURN_ID_LEN]; /* "" unless this is a dictation row */
     char note_id[DICT_NOTE_ID_LEN]; /* Dragon's authoritative id once adopted */
  ```
  (These widths come from `voice_dictation.h`: `DICT_TURN_ID_LEN` = 13, `DICT_NOTE_ID_LEN` = 40.)
- [ ] Confirm `voice_dictation.h` is included in `ui_notes.c` (it is — it uses `voice_dictation_set_state` etc.):
  ```bash
  grep -n '#include "voice_dictation.h"' /home/rebelforce/projects/TinkerTab/main/ui_notes.c
  ```
  Expected: a line near the top. If absent, add it next to the other voice includes.
- [ ] Persist the new fields in `notes_save()` (after the `pc` block, before `cJSON_AddItemToArray(arr, obj)` at line 550):
  ```c
        /* W4: persist enrichment state + identity so a reboot mid-enrichment
         * keeps the badge + can still reconcile a late note_created by turn_id. */
        if (n->enrich != ENRICH_NONE) cJSON_AddNumberToObject(obj, "en", (int)n->enrich);
        if (n->turn_id[0]) cJSON_AddStringToObject(obj, "tid", n->turn_id);
        if (n->note_id[0]) cJSON_AddStringToObject(obj, "nid", n->note_id);
  ```
- [ ] Restore them in `notes_load()` (after the `ty` / `pc` restore block, around line 732). Insert:
  ```c
        cJSON *jen = cJSON_GetObjectItem(item, "en");
        n->enrich = cJSON_IsNumber(jen) ? (enrich_state_t)(int)jen->valuedouble : ENRICH_NONE;
        const char *jtid = cJSON_GetStringValue(cJSON_GetObjectItem(item, "tid"));
        if (jtid) { strncpy(n->turn_id, jtid, DICT_TURN_ID_LEN - 1); n->turn_id[DICT_TURN_ID_LEN - 1] = '\0'; }
        const char *jnid = cJSON_GetStringValue(cJSON_GetObjectItem(item, "nid"));
        if (jnid) { strncpy(n->note_id, jnid, DICT_NOTE_ID_LEN - 1); n->note_id[DICT_NOTE_ID_LEN - 1] = '\0'; }
  ```
- [ ] Build:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -5
  ```
  Expected: `Project build complete.`
- [ ] Format-check + commit:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git-clang-format --binary clang-format-18 --diff origin/main main/ui_notes.c && git add main/ui_notes.c && git commit -m "feat(notes): add enrich_state + turn_id + note_id to note_entry_t (W4)"
  ```

---

## Task 5 — `ui_notes_seed_optimistic(turn_id)` + a turn_id finder

Create the function that seeds (or finds) the placeholder row at stop. It must be idempotent against the existing pipeline-armed slot (`s_pipeline_armed_slot`) and the local FAB slot (`s_rec_note_slot`) so a single dictation never lands twice. Marshals to the LVGL thread like `ui_notes_add_dictated_async` does.

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_notes.c` (add helper + function near `ui_notes_add_dictated_async`, ~line 1071)
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_notes.h` (declare)

Steps:
- [ ] In `/home/rebelforce/projects/TinkerTab/main/ui_notes.c`, add a turn_id finder near `find_note_idx_by_text` (after line 307):
  ```c
  /* W4: find the note row for a turn_id, or -1.  The reconcile join (note_created
   * / dictation_summary) keys on turn_id — the W1/W2 identity already echoed both
   * ways — so a late update for turn A only ever touches A's row, never B's. */
  static int find_note_idx_by_turn_id(const char *turn_id) {
     if (!turn_id || !turn_id[0]) return -1;
     for (int i = 0; i < MAX_NOTES; i++) {
        if (s_notes[i].used && s_notes[i].turn_id[0] && strcmp(s_notes[i].turn_id, turn_id) == 0) return i;
     }
     return -1;
  }
  ```
- [ ] Add the async callback + public seed function right after `ui_notes_add_dictated_async` (after line 1079):
  ```c
  /* W4: optimistic placeholder seed.  Runs on the LVGL thread.  Creates (or
   * tags, if a slot already exists for this turn) a note row keyed by turn_id
   * with enrich=SUMMARIZING — capture is done, only title/summary are pending.
   * Idempotent: a second seed for the same turn_id no-ops; if the home-Dictate
   * pipeline-armed slot or the local FAB slot already owns the recording, we
   * tag THAT slot with the turn_id rather than minting a second row. */
  static void notes_seed_optimistic_cb(void *arg) {
     char *turn_id = (char *)arg;
     if (!turn_id) return;
     notes_load();
     if (find_note_idx_by_turn_id(turn_id) >= 0) { free(turn_id); return; } /* already seeded */

     /* If a recording slot is already open for this turn (pipeline-armed home
      * Dictate chip, or local "+ NEW VOICE NOTE" FAB), tag it — don't dup. */
     int slot = -1;
     if (s_pipeline_armed_slot && s_rec_note_slot >= 0) {
        slot = s_rec_note_slot;
        if (slot < MAX_NOTES && !s_notes[slot].used) {
           s_notes[slot].used = true;
           if (s_note_count < MAX_NOTES) s_note_count++;
        }
     } else if (s_rec_note_slot >= 0) {
        slot = s_rec_note_slot;
     }

     if (slot < 0) {
        /* Fresh placeholder row. */
        tab5_rtc_time_t rtc = {0};
        tab5_rtc_get_time(&rtc);
        slot = s_next_slot;
        note_entry_t *n = &s_notes[slot];
        memset(n, 0, sizeof(*n));
        snprintf(n->text, MAX_NOTE_LEN, "Saved — summarizing…");
        n->state = NOTE_STATE_TRANSCRIBED; /* online: transcript streamed live */
        n->is_voice = true;
        n->type = NOTE_TYPE_VOICE;
        n->hour = rtc.hour; n->minute = rtc.minute; n->day = rtc.day;
        n->month = rtc.month; n->year = rtc.year;
        n->used = true;
        s_next_slot = (s_next_slot + 1) % MAX_NOTES;
        if (s_note_count < MAX_NOTES) s_note_count++;
     }

     note_entry_t *n = &s_notes[slot];
     n->type = NOTE_TYPE_VOICE;
     n->is_voice = true;
     n->enrich = ENRICH_SUMMARIZING;
     strncpy(n->turn_id, turn_id, DICT_TURN_ID_LEN - 1);
     n->turn_id[DICT_TURN_ID_LEN - 1] = '\0';
     ESP_LOGI(TAG, "Optimistic row seeded: slot %d turn_id=%s", slot, turn_id);
     notes_save();
     refresh_list();
     free(turn_id);
  }

  void ui_notes_seed_optimistic(const char *turn_id) {
     if (!turn_id || !turn_id[0] || strcmp(turn_id, "-") == 0) return;
     size_t n = strnlen(turn_id, DICT_TURN_ID_LEN - 1);
     char *copy = (char *)malloc(n + 1);
     if (!copy) return;
     memcpy(copy, turn_id, n);
     copy[n] = '\0';
     tab5_lv_async_call(notes_seed_optimistic_cb, copy);
  }
  ```
- [ ] Declare it in `/home/rebelforce/projects/TinkerTab/main/ui_notes.h` after `ui_notes_add_dictated_async` (line 71):
  ```c
  /** W4: seed (or tag) an optimistic note row for a live dictation turn_id at
   *  stop time.  Marshals to the LVGL thread.  Idempotent per turn_id; tags an
   *  existing recording slot instead of duplicating.  Reconciled later by
   *  ui_notes_reconcile_note_created / ui_notes_apply_summary (same turn_id). */
  void ui_notes_seed_optimistic(const char *turn_id);
  ```
- [ ] Confirm the `voice.c` call from Task 3 now resolves. Build:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -5
  ```
  Expected: `Project build complete.`
- [ ] Format-check + commit:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git-clang-format --binary clang-format-18 --diff origin/main main/ui_notes.c main/ui_notes.h main/voice.c && git add main/ui_notes.c main/ui_notes.h main/voice.c && git commit -m "feat(notes): ui_notes_seed_optimistic + turn_id finder, idempotent placeholder (W4)"
  ```

---

## Task 6 — Reconcile `note_created` by turn_id (adopt note_id, no dup row)

The join that preserves D-D1. When Dragon's authoritative `note_created` arrives with a `turn_id`: if a row already exists for it, adopt the `note_id` + update fields (no new row); else create the row from Dragon's note (the online race where placeholder and note_created race — last writer by turn_id wins, single row).

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_notes.c` (add reconcile fn after Task 5's seed fn, ~line 1145)
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_notes.h` (declare)

Steps:
- [ ] In `/home/rebelforce/projects/TinkerTab/main/ui_notes.c`, add the async-marshaled reconcile. Define a small heap arg struct + callback + public fn after `ui_notes_seed_optimistic`:
  ```c
  /* W4: note_created reconcile payload (heap, freed by the LVGL-thread cb). */
  typedef struct {
     char turn_id[DICT_TURN_ID_LEN];
     char note_id[DICT_NOTE_ID_LEN];
     char title[128];
  } reconcile_arg_t;

  static void notes_reconcile_note_created_cb(void *arg) {
     reconcile_arg_t *a = (reconcile_arg_t *)arg;
     if (!a) return;
     notes_load();
     int slot = find_note_idx_by_turn_id(a->turn_id);
     if (slot < 0) {
        /* Online race: note_created beat the optimistic seed, or no turn_id was
         * echoed.  Create the row from Dragon's note — single row by turn_id. */
        tab5_rtc_time_t rtc = {0};
        tab5_rtc_get_time(&rtc);
        slot = s_next_slot;
        note_entry_t *n = &s_notes[slot];
        memset(n, 0, sizeof(*n));
        n->state = NOTE_STATE_TRANSCRIBED;
        n->is_voice = true;
        n->type = NOTE_TYPE_VOICE;
        n->enrich = ENRICH_SUMMARIZING;
        n->hour = rtc.hour; n->minute = rtc.minute; n->day = rtc.day;
        n->month = rtc.month; n->year = rtc.year;
        n->used = true;
        strncpy(n->turn_id, a->turn_id, DICT_TURN_ID_LEN - 1);
        n->turn_id[DICT_TURN_ID_LEN - 1] = '\0';
        snprintf(n->text, MAX_NOTE_LEN, "%s", a->title[0] ? a->title : "Saved — summarizing…");
        s_next_slot = (s_next_slot + 1) % MAX_NOTES;
        if (s_note_count < MAX_NOTES) s_note_count++;
        ESP_LOGI(TAG, "note_created → fresh row slot %d (no placeholder), turn_id=%s", slot, a->turn_id);
     } else {
        ESP_LOGI(TAG, "note_created → adopt into slot %d, turn_id=%s", slot, a->turn_id);
     }
     /* Adopt Dragon's authoritative id (D-D1).  Keep enrich at SUMMARIZING until
      * dictation_summary lands the title+summary, then it flips to DONE. */
     note_entry_t *n = &s_notes[slot];
     strncpy(n->note_id, a->note_id, DICT_NOTE_ID_LEN - 1);
     n->note_id[DICT_NOTE_ID_LEN - 1] = '\0';
     if (n->enrich == ENRICH_NONE || n->enrich == ENRICH_PENDING || n->enrich == ENRICH_TRANSCRIBING) {
        n->enrich = ENRICH_SUMMARIZING;
     }
     n->needs_sync = false; /* Dragon already owns it */
     notes_save();
     refresh_list();
     free(a);
  }

  void ui_notes_reconcile_note_created(const char *turn_id, const char *note_id, const char *title) {
     if (!note_id || !note_id[0]) return;
     reconcile_arg_t *a = (reconcile_arg_t *)calloc(1, sizeof(*a));
     if (!a) return;
     if (turn_id && strcmp(turn_id, "-") != 0) strncpy(a->turn_id, turn_id, DICT_TURN_ID_LEN - 1);
     strncpy(a->note_id, note_id, DICT_NOTE_ID_LEN - 1);
     if (title) strncpy(a->title, title, sizeof(a->title) - 1);
     tab5_lv_async_call(notes_reconcile_note_created_cb, a);
  }
  ```
- [ ] Declare in `/home/rebelforce/projects/TinkerTab/main/ui_notes.h`:
  ```c
  /** W4: reconcile Dragon's authoritative note_created by turn_id.  If a row for
   *  turn_id exists, adopts note_id in place (no dup row); otherwise creates the
   *  row from Dragon's note.  Marshals to the LVGL thread.  turn_id may be "-"
   *  (absent) → treated as a fresh note (backward-compatible). */
  void ui_notes_reconcile_note_created(const char *turn_id, const char *note_id, const char *title);
  ```
- [ ] Build:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -5
  ```
  Expected: `Project build complete.`
- [ ] Format-check + commit:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git-clang-format --binary clang-format-18 --diff origin/main main/ui_notes.c main/ui_notes.h && git add main/ui_notes.c main/ui_notes.h && git commit -m "feat(notes): reconcile note_created by turn_id, adopt note_id no dup row (W4)"
  ```

---

## Task 7 — `ui_notes_apply_summary(turn_id, title, summary)` — in-place update + clear badge

`dictation_summary` becomes an in-place update of the reconciled row (title + final transcript/summary), then flips `enrich` to `ENRICH_DONE`.

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_notes.c` (add after Task 6's reconcile fn)
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_notes.h` (declare)

Steps:
- [ ] In `/home/rebelforce/projects/TinkerTab/main/ui_notes.c`, add:
  ```c
  /* W4: dictation_summary apply payload (heap, freed by the LVGL-thread cb). */
  typedef struct {
     char turn_id[DICT_TURN_ID_LEN];
     char title[128];
     char body[MAX_NOTE_LEN];
  } summary_arg_t;

  static void notes_apply_summary_cb(void *arg) {
     summary_arg_t *a = (summary_arg_t *)arg;
     if (!a) return;
     notes_load();
     int slot = find_note_idx_by_turn_id(a->turn_id);
     if (slot < 0) {
        /* No row for this turn (turn_id absent, or summary raced ahead of every
         * seed/note_created).  Fall back to the legacy add path so the dictation
         * still lands — never lose a dictation. */
        ESP_LOGW(TAG, "apply_summary: no row for turn_id=%s — legacy add", a->turn_id[0] ? a->turn_id : "-");
        if (a->body[0]) ui_notes_add(a->body, true);
        free(a);
        return;
     }
     note_entry_t *n = &s_notes[slot];
     if (a->body[0]) {
        strncpy(n->text, a->body, MAX_NOTE_LEN - 1);
        n->text[MAX_NOTE_LEN - 1] = '\0';
     }
     n->state = NOTE_STATE_TRANSCRIBED;
     n->enrich = ENRICH_DONE; /* title+summary present → badge clears */
     ESP_LOGI(TAG, "apply_summary → slot %d DONE, turn_id=%s", slot, a->turn_id);
     /* PR4 pending-chip handoff still attaches to the same slot inline. */
     pending_chip_apply_inline(slot);
     notes_save();
     refresh_list();
     free(a);
  }

  void ui_notes_apply_summary(const char *turn_id, const char *title, const char *summary) {
     summary_arg_t *a = (summary_arg_t *)calloc(1, sizeof(*a));
     if (!a) return;
     if (turn_id && strcmp(turn_id, "-") != 0) strncpy(a->turn_id, turn_id, DICT_TURN_ID_LEN - 1);
     if (title) strncpy(a->title, title, sizeof(a->title) - 1);
     if (summary) strncpy(a->body, summary, MAX_NOTE_LEN - 1);
     tab5_lv_async_call(notes_apply_summary_cb, a);
  }
  ```
  Note: `summary_arg_t` is `MAX_NOTE_LEN` (32 KB) — `calloc` it from heap, do not stack-allocate. The body carries the final transcript/summary text; the existing `voice_get_dictation_text()` may be a better source than the summary string — Task 9 decides what to pass.
- [ ] Declare in `/home/rebelforce/projects/TinkerTab/main/ui_notes.h`:
  ```c
  /** W4: apply dictation_summary as an in-place note update keyed by turn_id —
   *  sets the body text + flips enrich to DONE so the badge clears.  Marshals to
   *  the LVGL thread.  If no row matches turn_id, falls back to a fresh add so a
   *  dictation is never lost.  `summary` is the body text to store. */
  void ui_notes_apply_summary(const char *turn_id, const char *title, const char *summary);
  ```
- [ ] Build:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -5
  ```
  Expected: `Project build complete.`
- [ ] Format-check + commit:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git-clang-format --binary clang-format-18 --diff origin/main main/ui_notes.c main/ui_notes.h && git add main/ui_notes.c main/ui_notes.h && git commit -m "feat(notes): ui_notes_apply_summary in-place update + clear badge by turn_id (W4)"
  ```

---

## Task 8 — Render the enrichment badge on the note row

Extend the existing badge `switch (n->state)` block in `add_note_card_sectioned` so a non-`ENRICH_NONE`/non-`ENRICH_DONE` enrichment overrides the row badge text/color with the enrichment label. Always show until `ENRICH_DONE`.

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_notes.c` (`add_note_card_sectioned`, badge block lines 2730–2779)

Steps:
- [ ] In `/home/rebelforce/projects/TinkerTab/main/ui_notes.c`, immediately after the existing `switch (n->state) { … }` badge block closes (after line 2774, the line `}` that ends `default:`'s switch — i.e. just before `lv_label_set_text(badge, badge_text);` at line 2775), add the enrichment override:
  ```c
     /* W4: the enrichment badge takes precedence while a dictation row is still
      * filling in (independent of the orb/FSM).  Always shown until ENRICH_DONE. */
     switch (n->enrich) {
        case ENRICH_TRANSCRIBING:
           badge_text = "Transcribing…";
           badge_color = 0xFCD34D; /* amber */
           break;
        case ENRICH_SUMMARIZING:
           badge_text = "Summarizing…";
           badge_color = 0xFCD34D; /* amber */
           break;
        case ENRICH_PENDING:
           badge_text = "Pending";
           badge_color = 0x8E8E98; /* neutral grey — auto-finishes, not a failure */
           break;
        case ENRICH_NONE:
        case ENRICH_DONE:
        default:
           break; /* leave the state-derived badge above untouched */
     }
  ```
- [ ] Build:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -5
  ```
  Expected: `Project build complete.`
- [ ] Format-check:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git-clang-format --binary clang-format-18 --diff origin/main main/ui_notes.c
  ```
  Expected: empty.
- [ ] Commit (live-verify happens in Task 9 once the proto is wired):
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git add main/ui_notes.c && git commit -m "feat(notes): render enrichment badge (transcribing/summarizing/pending) on rows (W4)"
  ```

---

## Task 9 — Wire `voice_ws_proto.c`: route `note_created` + `dictation_summary` to the reconcile/apply paths

`note_created` is currently log-only (`voice_ws_proto.c:1008–1012`). `dictation_summary` (lines 928–1007) currently resolves the FSM and calls `ui_notes_add_dictated_async`. Route both through the new turn_id-keyed reconcile/apply.

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/voice_ws_proto.c` (`note_created` handler line 1008; `dictation_summary` handler lines 954–969)

Steps:
- [ ] In `/home/rebelforce/projects/TinkerTab/main/voice_ws_proto.c`, replace the body of the `note_created` branch (lines 1009–1012) with a `turn_id` extract + reconcile call:
  ```c
     } else if (strcmp(type_str, "note_created") == 0) {
        cJSON *nid = cJSON_GetObjectItem(root, "note_id");
        cJSON *ntitle = cJSON_GetObjectItem(root, "title");
        const char *nc_turn = cJSON_GetStringValue(cJSON_GetObjectItem(root, "turn_id"));
        ESP_LOGI(TAG, "Dragon auto-created note: id=%s title=\"%s\" turn_id=%s",
                 cJSON_IsString(nid) ? nid->valuestring : "?",
                 cJSON_IsString(ntitle) ? ntitle->valuestring : "?", nc_turn ? nc_turn : "-");
        /* W4: reconcile by turn_id — adopt note_id into the optimistic row (no
         * dup), or create the row if note_created raced ahead of the seed. */
        if (cJSON_IsString(nid) && nid->valuestring && nid->valuestring[0]) {
           ui_notes_reconcile_note_created(nc_turn, nid->valuestring,
                                           cJSON_IsString(ntitle) ? ntitle->valuestring : NULL);
        }
  ```
- [ ] In the `dictation_summary` branch, replace the `ui_notes_add_dictated_async(...)` block (lines 962–969) with the in-place apply. The branch already computed `summ_turn`, `s_dictation_title`, `s_dictation_summary`, and `voice_get_dictation_text()`. Change the `if (summ_applied && summ_non_empty) { … }` body's note-add section to:
  ```c
        if (summ_applied && summ_non_empty) {
           /* W4: apply as an in-place update of the turn's row (set by the
            * optimistic seed / note_created reconcile), then clear the badge.
            * Body = the streamed transcript when present, else the summary/title.
            * apply_summary falls back to a fresh add if no row matches turn_id,
            * so a dictation is never lost. */
           const char *transcript = voice_get_dictation_text();
           const char *body = (transcript && transcript[0]) ? transcript
                              : s_dictation_summary[0]       ? s_dictation_summary
                                                             : s_dictation_title;
           ui_notes_apply_summary(summ_turn, s_dictation_title, body);
           /* PR4 proposed_action parse stays exactly as-is below. */
  ```
  Leave the `proposed_action` parse block (lines 970–1006) unchanged — it already calls `ui_notes_attach_pending_chip_async`, which `notes_apply_summary_cb` consumes inline via `pending_chip_apply_inline(slot)`.
- [ ] Build:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -5
  ```
  Expected: `Project build complete.`
- [ ] Format-check:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git-clang-format --binary clang-format-18 --diff origin/main main/voice_ws_proto.c
  ```
  Expected: empty.
- [ ] **Live-verify the online reconcile** (flash first). The `/debug/inject_ws` endpoint routes a JSON string through `voice.c`'s WS text dispatcher exactly as if Dragon sent it. Use the SAME turn_id across seed → note_created → dictation_summary.
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py -p /dev/ttyACM0 flash
  export T=05eed3b13bf62d92cfd8ac424438b9f2 H=192.168.1.90:8080
  # 1) start + stop a dictation → seeds an optimistic row + reads back the live turn_id
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/dictation?action=start"; sleep 2
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/dictation?action=stop"
  TID=$(curl -s -H "Authorization: Bearer $T" "http://$H/dictation_pipeline" | python3 -c 'import sys,json;print(json.load(sys.stdin)["turn_id"])')
  echo "turn_id=$TID"
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/navigate?screen=notes"; sleep 1
  curl -s -H "Authorization: Bearer $T" -o /tmp/w4_seeded.jpg "http://$H/screenshot.jpg"
  # 2) inject Dragon's note_created with the SAME turn_id → must adopt, NOT duplicate
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/debug/inject_ws" \
       -d "{\"type\":\"note_created\",\"note_id\":\"note-w4-test\",\"title\":\"Groceries\",\"transcript\":\"buy milk and eggs\",\"turn_id\":\"$TID\"}"; sleep 1
  curl -s -H "Authorization: Bearer $T" -o /tmp/w4_reconciled.jpg "http://$H/screenshot.jpg"
  # 3) inject dictation_summary with the SAME turn_id → fills title/summary in place, badge clears
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/debug/inject_ws" \
       -d "{\"type\":\"dictation_summary\",\"title\":\"Groceries\",\"summary\":\"Buy milk and eggs on the way home.\",\"turn_id\":\"$TID\"}"; sleep 1
  curl -s -H "Authorization: Bearer $T" -o /tmp/w4_done.jpg "http://$H/screenshot.jpg"
  ```
  **Pass criteria:**
  - `/tmp/w4_seeded.jpg`: exactly ONE new voice note row with a `Summarizing…` (amber) badge.
  - `/tmp/w4_reconciled.jpg`: still ONE row (note_id adopted, no second row) — badge still `Summarizing…`.
  - `/tmp/w4_done.jpg`: still ONE row, body shows the summary text, the badge has CLEARED (shows `Voice`, not `Summarizing…`).
  **Fail:** two rows appear after step 2 (dup) → reconcile didn't match turn_id; badge never clears after step 3 → apply_summary didn't match.
- [ ] **Live-verify cross-turn safety:** inject a `dictation_summary` with a DIFFERENT turn_id and confirm the seeded row is untouched:
  ```bash
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/debug/inject_ws" \
       -d '{"type":"dictation_summary","title":"Other","summary":"unrelated","turn_id":"ffffffffffff"}'; sleep 1
  curl -s -H "Authorization: Bearer $T" -o /tmp/w4_crossturn.jpg "http://$H/screenshot.jpg"
  ```
  **Pass:** the `Groceries` row is unchanged; a separate fallback row "unrelated" may appear (the no-match legacy-add path) but the original is not corrupted.
- [ ] Commit:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git add main/voice_ws_proto.c && git commit -m "feat(ws): route note_created + dictation_summary to turn_id reconcile/apply (W4)"
  ```

---

## Task 10 — Background auto-finish: offline placeholder + re-drive `ENRICH_PENDING` on reconnect

Offline: at stop with Dragon down, the existing `voice_stop_listening` offline branch (`voice.c:2098–2112`) already saves the SD WAV as a `NOTE_STATE_RECORDED` note and toasts. Tag that note with `enrich = ENRICH_PENDING` + the turn_id so the badge shows `Pending`, and let the existing `transcription_queue_task` finish it on reconnect (it already drives the FSM and writes the transcript; it just needs to clear the badge). No manual retry.

**Files:**
- Modify `/home/rebelforce/projects/TinkerTab/main/voice.c` (offline branch, after `ui_notes_stop_recording(NULL)` line 2101)
- Modify `/home/rebelforce/projects/TinkerTab/main/ui_notes.c` (`ui_notes_stop_recording` to stamp enrich on the offline slot, and `transcription_queue_task` success/begin paths to flip enrich)

Steps:
- [ ] Add a helper in `/home/rebelforce/projects/TinkerTab/main/ui_notes.c` that tags the most-recently-finalized recorded slot as pending with a turn_id, near `find_note_idx_by_turn_id`:
  ```c
  /* W4: tag the just-finalized SD recording as an offline-pending dictation row
   * so the badge shows "Pending" and the transcription queue auto-finishes it on
   * reconnect (reconciled by turn_id when Dragon's note_created lands).  Pass the
   * live FSM turn_id so a later note_created can adopt this row in place. */
  void ui_notes_mark_offline_pending(const char *turn_id) {
     int idx = find_most_recent_used_slot();
     if (idx < 0) return;
     note_entry_t *n = &s_notes[idx];
     n->enrich = ENRICH_PENDING;
     if (turn_id && turn_id[0] && strcmp(turn_id, "-") != 0) {
        strncpy(n->turn_id, turn_id, DICT_TURN_ID_LEN - 1);
        n->turn_id[DICT_TURN_ID_LEN - 1] = '\0';
     }
     notes_save();
     refresh_list();
  }
  ```
  Declare it in `/home/rebelforce/projects/TinkerTab/main/ui_notes.h`:
  ```c
  /** W4: mark the just-finalized SD recording as offline-pending (badge "Pending")
   *  and stamp turn_id so reconnect note_created reconciles it in place.  Called
   *  from voice.c's offline dictation stop branch. */
  void ui_notes_mark_offline_pending(const char *turn_id);
  ```
- [ ] In `/home/rebelforce/projects/TinkerTab/main/voice.c`, in the offline branch, right after `ui_notes_stop_recording(NULL);` (line 2101) add:
  ```c
        ui_notes_stop_recording(NULL);
        ui_notes_mark_offline_pending(s_current_turn_id);
  ```
- [ ] In `/home/rebelforce/projects/TinkerTab/main/ui_notes.c`'s `transcription_queue_task`, when a transcription SUCCEEDS (line 1820, the `n->state = NOTE_STATE_TRANSCRIBED;` success branch), flip the badge to `ENRICH_DONE`:
  ```c
                       n->state = NOTE_STATE_TRANSCRIBED;
                       n->fail_reason = NOTE_FAIL_NONE;
                       n->enrich = ENRICH_DONE; /* W4: offline auto-finish — badge clears */
  ```
  And when it begins working a pending note (after `n->state = NOTE_STATE_TRANSCRIBING;` at line 1669), set the transcribing badge:
  ```c
        n->state = NOTE_STATE_TRANSCRIBING;
        if (n->enrich == ENRICH_PENDING) n->enrich = ENRICH_TRANSCRIBING; /* W4 */
  ```
- [ ] Build:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -5
  ```
  Expected: `Project build complete.`
- [ ] **Live-verify offline → auto-finish:** force Dragon unreachable, dictate, confirm `Pending`, bring Dragon back, confirm auto-completion with exactly one row.
  ```bash
  export T=05eed3b13bf62d92cfd8ac424438b9f2 H=192.168.1.90:8080
  # Drop the WS so the stop takes the offline branch (point dragon_host at a dead IP, or pull Dragon's cable / stop the service).
  # Simplest: stop the Dragon voice service, then:
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/dictation?action=start"; sleep 3
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/dictation?action=stop"; sleep 1
  curl -s -H "Authorization: Bearer $T" -X POST "http://$H/navigate?screen=notes"; sleep 1
  curl -s -H "Authorization: Bearer $T" -o /tmp/w4_offline_pending.jpg "http://$H/screenshot.jpg"
  # restart Dragon's tinkerclaw-voice, wait for the 15s queue tick + transcription:
  sleep 45
  curl -s -H "Authorization: Bearer $T" -o /tmp/w4_offline_done.jpg "http://$H/screenshot.jpg"
  ```
  **Pass:** `/tmp/w4_offline_pending.jpg` shows one row with a grey `Pending` badge; `/tmp/w4_offline_done.jpg` shows the SAME single row with the transcript filled in and the badge cleared. **Fail:** two rows, or the badge never clears, or the dictation is lost.
- [ ] Format-check + commit:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && git-clang-format --binary clang-format-18 --diff origin/main main/voice.c main/ui_notes.c main/ui_notes.h && git add main/voice.c main/ui_notes.c main/ui_notes.h && git commit -m "feat(notes): offline placeholder enrich=pending + auto-finish on reconnect (W4)"
  ```

---

## Task 11 — Dragon: pin `note_created.turn_id` with a test (real TDD)

`note_created` already carries `turn_id` (`stop_handler.py:172`), but no test asserts it — a future refactor could drop it and silently break Tab5 reconcile. Add the assertion as a regression pin.

**Files:**
- Modify `/home/rebelforce/projects/TinkerBox/tests/test_stop_handler.py`

Steps:
- [ ] Run the existing suite first to confirm green baseline:
  ```bash
  cd /home/rebelforce/projects/TinkerBox && python3 -m pytest -q tests/test_stop_handler.py
  ```
  Expected: all pass (currently 13 tests).
- [ ] Add a failing-then-passing test in `/home/rebelforce/projects/TinkerBox/tests/test_stop_handler.py`, inside `class TestDictateMode`, after `test_dictate_transcript_truncated_to_200_chars`:
  ```python
      @pytest.mark.asyncio
      async def test_dictate_note_created_carries_turn_id(self):
          """W4: Tab5 reconciles the optimistic note row by turn_id, so the
          note_created frame MUST echo the turn's id (pulled from conn_state,
          stashed there by start_handler).  Pin it so a refactor can't drop it."""
          ws = _make_ws()
          pipeline = _make_pipeline(transcript="A long enough transcript")
          notes_svc = _make_notes_svc(note_id="note-77")

          await handle_stop_command(
              ws,
              ws_id="ws-tid",
              conn_state={
                  "pipeline": pipeline,
                  "mode": "dictate",
                  "turn_id": "abc123def456",
              },
              conn_lock=asyncio.Lock(),
              notes_svc=notes_svc,
          )

          frame = ws.send_json.await_args.args[0]
          assert frame["type"] == "note_created"
          assert frame["turn_id"] == "abc123def456"

      @pytest.mark.asyncio
      async def test_dictate_note_created_turn_id_defaults_to_dash(self):
          """When conn_state has no turn_id (legacy / boot race), the frame
          still carries a turn_id field ('-') so the Tab5 handler's lookup is
          well-defined (treated as 'no match' → fresh note)."""
          ws = _make_ws()
          pipeline = _make_pipeline(transcript="A long enough transcript")
          notes_svc = _make_notes_svc()

          await handle_stop_command(
              ws,
              ws_id="ws-tid2",
              conn_state={"pipeline": pipeline, "mode": "dictate"},  # no turn_id
              conn_lock=asyncio.Lock(),
              notes_svc=notes_svc,
          )

          frame = ws.send_json.await_args.args[0]
          assert frame["turn_id"] == "-"
  ```
- [ ] Run the test:
  ```bash
  cd /home/rebelforce/projects/TinkerBox && python3 -m pytest -q tests/test_stop_handler.py -k turn_id
  ```
  Expected: 2 passed (the code already emits `turn_id`, so these pass immediately — they are regression pins, not red-first; if you want red-first, temporarily delete the `"turn_id": turn_id,` line in `stop_handler.py:172` → both fail with KeyError).
- [ ] Ruff-check the changed file (the narrow gate):
  ```bash
  cd /home/rebelforce/projects/TinkerBox && ruff check --select F821,F722,F811,F823,B006,B904,E722,B007,RUF006 tests/test_stop_handler.py
  ```
  Expected: `All checks passed!`
- [ ] Commit:
  ```bash
  cd /home/rebelforce/projects/TinkerBox && git add tests/test_stop_handler.py && git commit -m "test(stop): pin note_created carries turn_id for W4 reconcile"
  ```

---

## Task 12 — Dragon: embedding never blocks/fails the note + retries (real TDD)

`_embed_note` / `_get_embedding` (`notes/service.py:323–344`) already catch exceptions and return `[]`, and run via `_spawn_bg` (non-blocking). The spec's remaining gap: on failure the embedding is simply dropped (no retry), so the note is permanently unsearchable. Add a bounded background retry so a transient `Embedding failed: Server disconnected` self-heals, while still never blocking or failing the note insert.

**Files:**
- Create `/home/rebelforce/projects/TinkerBox/tests/test_notes_embed_retry.py`
- Modify `/home/rebelforce/projects/TinkerBox/dragon_voice/notes/service.py` (`_embed_note`)

Steps:
- [ ] Write the failing test first in `/home/rebelforce/projects/TinkerBox/tests/test_notes_embed_retry.py`:
  ```python
  """W4: embedding must never fail/roll back the note insert, and a transient
  embedding error must retry in the background until it succeeds."""
  from __future__ import annotations

  from unittest.mock import AsyncMock, MagicMock

  import pytest

  from dragon_voice.notes.service import NotesService


  def _make_service() -> NotesService:
      svc = NotesService.__new__(NotesService)  # bypass __init__ / config
      svc._db = MagicMock()
      svc._db.update = AsyncMock()
      svc._bg_tasks = set()
      return svc


  @pytest.mark.asyncio
  async def test_embed_retries_then_succeeds(monkeypatch):
      """First two _get_embedding calls fail (return []), the third succeeds.
      _embed_note must keep retrying until it stores a non-empty vector, and
      must NOT raise (so the note insert is never affected)."""
      svc = _make_service()
      calls = {"n": 0}

      async def fake_get_embedding(text):
          calls["n"] += 1
          if calls["n"] < 3:
              return []      # simulate "Server disconnected"
          return [0.1, 0.2, 0.3]

      monkeypatch.setattr(svc, "_get_embedding", fake_get_embedding)
      # Speed the retry backoff for the test.
      monkeypatch.setattr("dragon_voice.notes.service._EMBED_RETRY_DELAYS", [0, 0, 0])

      await svc._embed_note("note-1", "some transcript")

      assert calls["n"] == 3
      svc._db.update.assert_awaited_once_with("note-1", {"embedding": [0.1, 0.2, 0.3]})


  @pytest.mark.asyncio
  async def test_embed_gives_up_after_max_retries_without_raising(monkeypatch):
      """All attempts fail → _embed_note returns quietly, never raises, never
      calls db.update (no embedding stored), and the note is otherwise fine."""
      svc = _make_service()

      async def always_fail(text):
          return []

      monkeypatch.setattr(svc, "_get_embedding", always_fail)
      monkeypatch.setattr("dragon_voice.notes.service._EMBED_RETRY_DELAYS", [0, 0, 0])

      # Must not raise.
      await svc._embed_note("note-2", "transcript")
      svc._db.update.assert_not_awaited()
  ```
- [ ] Run it — expect FAIL (the retry loop + `_EMBED_RETRY_DELAYS` don't exist yet):
  ```bash
  cd /home/rebelforce/projects/TinkerBox && python3 -m pytest -q tests/test_notes_embed_retry.py
  ```
  Expected: failures — `AttributeError: ... has no attribute '_EMBED_RETRY_DELAYS'` and the first test sees `calls["n"] == 1`.
- [ ] Implement the retry in `/home/rebelforce/projects/TinkerBox/dragon_voice/notes/service.py`. Add a module-level constant near the top (after the imports, before `_placeholder_title`):
  ```python
  # W4: bounded background retry for note embeddings.  An embedding failure
  # (e.g. "Server disconnected") must never block or fail the note insert — the
  # note is saved + usable the moment the transcript exists.  Only the semantic
  # index of that note is deferred until a retry succeeds.  Delays in seconds.
  _EMBED_RETRY_DELAYS = [2, 10, 30]
  ```
  Replace `_embed_note` (lines 323–328) with:
  ```python
      async def _embed_note(self, note_id: str, text: str) -> None:
          """Generate + store the note embedding, retrying transient failures.

          Never raises — embedding is best-effort background work.  On every
          attempt failure we wait a backoff and retry; after the last delay we
          give up quietly (the note stays usable, just unindexed until a later
          edit re-embeds it).
          """
          for delay in [0, *_EMBED_RETRY_DELAYS]:
              if delay:
                  await asyncio.sleep(delay)
              try:
                  embedding = await self._get_embedding(text[:8000])
              except Exception:  # noqa: BLE001 — best-effort, never propagate
                  logger.warning("Embedding attempt failed for %s", note_id, exc_info=True)
                  continue
              if embedding:
                  await self._db.update(note_id, {"embedding": embedding})
                  logger.info("Embedded note %s (%d dims)", note_id, len(embedding))
                  return
          logger.warning("Embedding gave up for %s after %d retries", note_id, len(_EMBED_RETRY_DELAYS))
  ```
- [ ] Run the test again — expect PASS:
  ```bash
  cd /home/rebelforce/projects/TinkerBox && python3 -m pytest -q tests/test_notes_embed_retry.py
  ```
  Expected: 2 passed.
- [ ] Ruff-check (note the `B007` unused-loop-var and `B904` raise-from codes are in the gate — the implementation avoids both):
  ```bash
  cd /home/rebelforce/projects/TinkerBox && ruff check --select F821,F722,F811,F823,B006,B904,E722,B007,RUF006 dragon_voice/notes/service.py tests/test_notes_embed_retry.py
  ```
  Expected: `All checks passed!`
- [ ] Commit:
  ```bash
  cd /home/rebelforce/projects/TinkerBox && git add dragon_voice/notes/service.py tests/test_notes_embed_retry.py && git commit -m "fix(notes): retry note embedding in background, never fail the note (W4)"
  ```

---

## Task 13 — Dragon: add the new test to the CI named set

CI only runs the named test files in `.github/workflows/ci.yml`. Add the new file so the embed-retry guard is enforced.

**Files:**
- Modify `/home/rebelforce/projects/TinkerBox/.github/workflows/ci.yml`

Steps:
- [ ] In `/home/rebelforce/projects/TinkerBox/.github/workflows/ci.yml`, add to the pytest named list (after `tests/test_notes_db_async.py`, line 76 — keep alphabetical-ish grouping with the other notes tests):
  ```yaml
              tests/test_notes_db_async.py \
              tests/test_notes_embed_retry.py \
  ```
  Also add `tests/test_stop_handler.py` if it is not already in the list (it is referenced by `test_start_handler.py` at line 131 — verify):
  ```bash
  grep -n "test_stop_handler" /home/rebelforce/projects/TinkerBox/.github/workflows/ci.yml || echo "NOT PRESENT — add it next to test_start_handler.py"
  ```
  If absent, add `tests/test_stop_handler.py \` next to `tests/test_start_handler.py` (line 131).
- [ ] Verify the named set runs clean locally (the two files this wave touches):
  ```bash
  cd /home/rebelforce/projects/TinkerBox && python3 -m pytest -q tests/test_stop_handler.py tests/test_notes_embed_retry.py
  ```
  Expected: all pass.
- [ ] Commit:
  ```bash
  cd /home/rebelforce/projects/TinkerBox && git add .github/workflows/ci.yml && git commit -m "ci: add W4 notes embed-retry + stop-handler tests to named set"
  ```

---

## Final integration verification (both repos)

After all tasks land, run the full gate locally before opening PRs.

- [ ] TinkerTab host tests:
  ```bash
  cmake --build /home/rebelforce/projects/TinkerTab/tests/host/build && ctest --test-dir /home/rebelforce/projects/TinkerTab/tests/host/build --output-on-failure
  ```
  Expected: `100% tests passed`.
- [ ] TinkerTab firmware build + full diff format check:
  ```bash
  . /home/rebelforce/esp/esp-idf/export.sh && cd /home/rebelforce/projects/TinkerTab && idf.py build 2>&1 | tail -3 && git-clang-format --binary clang-format-18 --diff origin/main main/*.c main/*.h
  ```
  Expected: `Project build complete.` + empty format diff.
- [ ] TinkerTab end-to-end on hardware — the full online happy path, asserting a SINGLE row and a cleared badge (re-run the Task 9 inject sequence; confirm one row + badge cleared). Optionally run the existing harness smoke:
  ```bash
  cd /home/rebelforce/projects/TinkerTab && export TAB5_TOKEN=05eed3b13bf62d92cfd8ac424438b9f2 && python3 tests/e2e/runner.py story_smoke
  ```
  Expected: smoke passes (no dictation regression).
- [ ] TinkerBox ruff (full gate) + named pytest:
  ```bash
  cd /home/rebelforce/projects/TinkerBox && ruff check --select F821,F722,F811,F823,B006,B904,E722,B007,RUF006 dragon_voice/ tests/ && python3 -m pytest -q tests/test_stop_handler.py tests/test_notes_embed_retry.py tests/test_notes_db_async.py
  ```
  Expected: `All checks passed!` + all pytest pass.

---

## Notes for the implementing engineer

- **Threading discipline (critical):** Every `ui_notes.c` function that touches `s_notes` AND `refresh_list` must run on the LVGL thread. All three new public functions (`ui_notes_seed_optimistic`, `ui_notes_reconcile_note_created`, `ui_notes_apply_summary`) marshal via `tab5_lv_async_call` exactly like the existing `ui_notes_add_dictated_async` (`ui_notes.c:1071`). Never call them synchronously from the WS RX task. `ui_notes_mark_offline_pending` is called from `voice_stop_listening` which already runs on a task that calls `ui_notes_stop_recording` directly — confirm that context matches the existing offline-branch calls (it does; `ui_notes_stop_recording(NULL)` is already called there).
- **LVGL `lv_async_call` is LIFO** (documented at `ui_notes.c:1408`). The existing pending-chip handoff sidesteps this by attaching inline. The W4 reconcile/apply each carry their full payload in one heap arg, so there is no two-callback ordering hazard — keep it that way.
- **`summary_arg_t` is 32 KB** (`MAX_NOTE_LEN`). Always `calloc` it from heap (the code does); never stack-allocate, or you blow the 8 KB task stack.
- **Backward compatibility:** every handler treats a missing/`"-"` `turn_id` as "no match → fresh note" (the FSM already does this in `voice_dictation_resolve_if_current`, `voice_dictation.c:472`). An old Dragon that doesn't echo `turn_id` keeps working — you just get a fresh row instead of an in-place update. This matches the spec's "backward-compatible" requirement.
- **Don't touch the FSM resolution path:** `voice_dictation_resolve_if_current` / `resolution_pending` / W3 stuck-watchdog stay exactly as-is — they bound the FSM's own return to IDLE and are orthogonal to the note badge now. The proto changes in Task 9 keep the existing `voice_dictation_resolve_if_current(...)` calls; they only ADD the `ui_notes_*` routing alongside.
- **The `find_note_idx_by_text` heuristic (`ui_notes.c:301`) is now superseded for dictation rows** by `find_note_idx_by_turn_id`, but leave it — it is still used by the `needs_sync` text-note path. Do not refactor it (out of W4 scope per the spec's "Not in scope: de-bloating ui_notes.c").

### Critical Files for Implementation
- `/home/rebelforce/projects/TinkerTab/main/ui_notes.c`
- `/home/rebelforce/projects/TinkerTab/main/voice_ws_proto.c`
- `/home/rebelforce/projects/TinkerTab/main/voice_dictation.c`
- `/home/rebelforce/projects/TinkerTab/main/ui_orb.c`
- `/home/rebelforce/projects/TinkerBox/dragon_voice/notes/service.py`