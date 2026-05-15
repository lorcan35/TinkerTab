# Dictation pipeline PR 3 — Notes screen reimagined

**Goal:** Notes screen subscribes to the dictation pipeline that landed in #519 / #521.  Timeline IA (day sections), filter pills (All / Voice / Text / Pending), processing row at top with mini-orb mirroring the heroic orb's pipeline color, amber FAB bottom-right that fires the same `voice_start_dictation` as the home Dictate chip.  Forward-compat schema additions (`note_type_t`, `pending_chip`) for PR 4's action chips.

**Architecture:**  one render path (`refresh_list`) emits day-section headers + filtered rows + a top processing row that subscribes to `voice_dictation_subscribe_lvgl`.  The processing row is hidden when `pipeline == DICT_IDLE` and morphs through the same red/amber/green/red color story as the home orb.  Removes the legacy voice/text button row + always-on search bar.

**Spec:** `docs/superpowers/specs/2026-05-14-dictation-pipeline-notes-redesign-design.md` § PR 3.

Closes #523.  Builds on PR 2 (#521).

---

## File map

**Modify:**
- `main/ui_notes.c` — schema additions + new `refresh_list` with day-sections + filter-pill state + processing-row widget + FAB.  Strip the old voice/text button row + always-on search bar.
- `main/ui_notes.h` — likely no public-API change.  Confirm during execution.

**No new files.**

**Boundary rules:**
- Hide/show pattern preserved (`s_destroying` guard remains).
- Existing notes load/save JSON gains `"ty"` (type) + `"pc"` (pending_chip) keys.  PR 3 reads/writes them; PR 4 will populate `pc`.
- Pipeline subscriber must run on the LVGL thread (`voice_dictation_subscribe_lvgl`).

---

## Tasks

### Task 1 — Schema additions (note_type_t + pending_chip)

**Files:** `main/ui_notes.c` (struct only).

- [ ] **Step 1: Add `note_type_t` enum** near the existing `note_state_t` enum.

```c
/* PR 3: classification of the note's content — informs the filter pills + the
 * eventual action chips in PR 4.  Forward-compatible: existing notes load
 * with type=NOTE_TYPE_AUTO and the refresh_list code picks the type from
 * `is_voice` so behaviour is unchanged.  PR 4 will start populating real
 * types via Dragon's classifier. */
typedef enum {
   NOTE_TYPE_AUTO = 0,    /* infer from is_voice (legacy) */
   NOTE_TYPE_TEXT,        /* user typed */
   NOTE_TYPE_VOICE,       /* dictated, no further classification */
   NOTE_TYPE_LIST,        /* dictated, parsed as a list (PR 4) */
   NOTE_TYPE_REMINDER,    /* dictated, parsed as a reminder (PR 4) */
} note_type_t;

typedef struct {
   /* Empty in PR 3 — slot is allocated so PR 4 can populate without
    * a second migration.  PR 4 will add fields like `kind`, `payload`,
    * `confidence`, `dismissed`.  JSON key "pc" is reserved. */
   uint8_t _reserved;
} pending_chip_t;
```

- [ ] **Step 2: Add `type` + `pending` fields to `note_entry_t`.**

```c
typedef struct {
   char text[MAX_NOTE_LEN];
   char audio_path[MAX_AUDIO_PATH];
   note_state_t state;
   note_fail_t fail_reason;
   note_type_t type;          /* PR 3 */
   pending_chip_t pending;    /* PR 3 (reserved for PR 4) */
   bool is_voice;
   /* ... unchanged ... */
} note_entry_t;
```

- [ ] **Step 3: Persist `type` in JSON load/save.**  Add `"ty"` key.  PR 3 doesn't persist `pc` (no fields to persist yet).

- [ ] **Step 4: Build clean.**

```bash
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -5
```

- [ ] **Step 5: Commit.**

```bash
git commit -am "feat(notes): note_type_t + pending_chip forward-compat schema"
```

---

### Task 2 — Filter-pill state + day-section grouping

**Files:** `main/ui_notes.c`.

- [ ] **Step 1: Add filter state.**

```c
typedef enum {
   NOTE_FILTER_ALL = 0,
   NOTE_FILTER_VOICE,
   NOTE_FILTER_TEXT,
   NOTE_FILTER_PENDING,    /* notes with any non-zero pending_chip (PR 4) */
} note_filter_t;
static note_filter_t s_filter = NOTE_FILTER_ALL;

static lv_obj_t *s_filter_pills_row = NULL;
static lv_obj_t *s_filter_pill[4]   = {NULL};
```

- [ ] **Step 2: Build the filter-pill row** under the title.  Tap on each toggles `s_filter` + calls `refresh_list`.  Active pill: amber bg.  Inactive: dark pill with thin border.

- [ ] **Step 3: Day-section helper.**

```c
typedef enum {
   DAY_SECTION_TODAY = 0,
   DAY_SECTION_YESTERDAY,
   DAY_SECTION_THIS_WEEK,
   DAY_SECTION_OLDER,
} day_section_t;

static day_section_t classify_note_day(const note_entry_t *n);
static const char *day_section_label(day_section_t s);
```

Compare `n->day/month/year` against the current RTC date; classify into one of the four buckets.

- [ ] **Step 4: Rebuild `refresh_list` to emit section headers + filtered rows.**  Each section header is a thin label widget ("TODAY", "YESTERDAY", …) with FONT_CAPTION + letter-spacing 2, amber accent.  Filter the per-row pass on `s_filter`.

- [ ] **Step 5: Build + flash + verify visually that 14 existing notes still render under correct headers.**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
sleep 22
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" -X POST http://192.168.1.90:8080/navigate?screen=notes
sleep 2
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" http://192.168.1.90:8080/screenshot.jpg -o /tmp/notes-timeline.jpg
file /tmp/notes-timeline.jpg
```

- [ ] **Step 6: Commit.**

```bash
git commit -am "feat(notes): filter pills + day-section timeline IA"
```

---

### Task 3 — Processing row + pipeline subscriber

**Files:** `main/ui_notes.c`.

- [ ] **Step 1: Build the processing-row widget** at the top of `s_list`.  Layout: mini-orb (24 px circle with gradient) on the left + state caption ("RECORDING 0:23" / "TRANSCRIBING" / "SAVING" / "FAILED · TAP TO RETRY") + close-× on the right.  Hidden by default.

- [ ] **Step 2: Subscribe via `voice_dictation_subscribe_lvgl`** in `ui_notes_create`.  Callback updates the mini-orb color + caption + visibility based on `dict_event_t.state`.

- [ ] **Step 3: Tap × → `voice_cancel()` + drive pipeline to FAILED/CANCELLED** (same path as the home chip).

- [ ] **Step 4: Live test: start dictation from home, navigate to Notes, verify processing row appears + mirrors orb state.  Stop dictation, verify row disappears (or stays briefly on SAVED before fading).**

- [ ] **Step 5: Commit.**

```bash
git commit -am "feat(notes): processing row subscribes to dictation pipeline"
```

---

### Task 4 — Amber FAB + remove legacy voice/text row + always-on search bar

**Files:** `main/ui_notes.c`.

- [ ] **Step 1: Build the FAB** — 56 px round amber button (`#F59E0B`), bottom-right at (`SW - 80, SH - 200`).  LV_SYMBOL_AUDIO inside.  Tap → `voice_start_dictation` (same flow as home Dictate chip).

- [ ] **Step 2: Remove the old `s_input_area` + voice/text buttons** + the always-on search textarea.

- [ ] **Step 3: Move search to a top-bar icon.**  Single magnifier glyph tappable in the top right.  Tap → focuses a slim search ta that appears between title and filter pills.  Tap again → collapses.

- [ ] **Step 4: Verify layout + tap.**  Build + flash + screenshot.

- [ ] **Step 5: Commit.**

```bash
git commit -am "feat(notes): amber FAB + reorg search; remove legacy input row"
```

---

### Task 5 — Format gate + live e2e + PR

- [ ] **Step 1: Format gate.**

```bash
git fetch origin main:refs/remotes/origin/main
git-clang-format --binary clang-format-18 --diff origin/main main/ui_notes.c main/ui_notes.h
# If non-empty, run without --diff and amend.
```

- [ ] **Step 2: Live e2e.**

  - Navigate home + tap Dictate chip; mid-RECORDING, navigate to Notes — processing row should already be present with correct state.
  - Tap × on the processing row — pipeline → FAILED/CANCELLED.
  - Tap amber FAB — pipeline → RECORDING, processing row goes red.
  - Filter pills cycle correctly.
  - 50 fast home↔notes navs — no PANIC, no fragmentation reboot.

- [ ] **Step 3: Push + PR.**

```bash
git push -u origin feat/dictation-pipeline-pr3
gh pr create --title "feat(dictation): Notes timeline + processing row + FAB (PR 3 of 4)" \
  --body "...standard body, refs #523..."
```

---

## Acceptance summary

When all tasks land:

- [ ] `note_type_t` enum + `type` field on `note_entry_t` + `pending_chip` substruct (reserved for PR 4)
- [ ] Day-section headers grouping notes by recency
- [ ] Filter pills All / Voice / Text / Pending
- [ ] Processing row at top while dictation is in-flight; mini-orb mirrors heroic orb
- [ ] Amber FAB bottom-right fires `voice_start_dictation`
- [ ] Old voice/text button row + always-on search bar gone
- [ ] All existing notes render in new timeline; filter pills work; FAB works; no PANIC across 50 navs
- [ ] Format gate green, build clean, PR open
