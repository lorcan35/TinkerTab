# Dictation pipeline visibility + Notes screen reimagined

> **Status:** Brainstormed 2026-05-14.  Spec approved, ready to plan implementation.
>
> **Successors:** PR #517 closed the Notes-screen 100% FAIL bug by adding bearer auth + reason chips + retry/clear surfaces.  This spec builds the next layer: making dictation pipeline state *visible* across home and Notes, adding a discoverable home entry, and reimagining the Notes IA from a flat-card list to a day-grouped timeline.

## Problem

After PR #517 dictation works again technically — Tab5 voice-notes upload, transcribe, and persist.  But the UX around it is still hostile:

1. **No status visibility while a dictation is in flight.**  After tapping a Retry or starting a new recording the user has no idea what state the pipeline is in — recording? uploading? Dragon thinking? saved? — until the row's badge eventually changes.  User words: *"i want it to give me status like transcribing, sent, something that shows me whats going on."*

2. **No discoverable home-screen launcher.**  Dictation today launches only via long-press orb (invisible affordance, often confused with voice-ask) or the Notes-screen `+ NEW VOICE NOTE` button (requires a nav).  User wants a *visible* home-screen entry.

3. **Notes screen IA is janky.**  Flat reverse-chronological card list, ~110 px per row, ~200 px of fixed chrome (topbar + new-note buttons + search bar) before the first note, 5–6 visible cards max.  No grouping, no filtering, no signal that any item was a reminder / list / actionable.  User words: *"the notes should be reimagined.  its very janky."*

4. **No surfaces for the four use-cases the user actually wants.**  All four were named: quick capture, long-form thinking, actionable items (reminders / todos / lists), searchable journal.  Today everything lands as an undifferentiated voice-note.  Reminders and lists don't exist as types.

## Goals

- **One mental model for dictation state, surfaced consistently** across the heroic orb on home, a compact chip on home, and a mini-orb echo on the Notes processing row.
- **Visible home entry** for dictation in the existing visual language (mode-chip-style dark pill, no saturated gradient).
- **Notes screen rebuilt** as a day-grouped timeline with type-coloured rows, filter pills, a compact 44 px row, an amber FAB, and a top processing section that mirrors the orb state for in-flight items.
- **Post-hoc routing via action chips:** every dictation lands as a NOTE first; Dragon's classifier proposes a conversion to REMINDER or LIST and the user confirms with one tap.  Wrong classifications cost one ✕ tap to dismiss.

## Non-goals (deferred, not blockers)

- **Task type** as a first-class category (overlap with Reminder; revisit if real demand emerges)
- **Search redesign** — current search stays in place during the Notes rebuild; collapse to a topbar icon is a nice-to-have follow-up
- **Multi-select / bulk-delete row gesture** in Notes — long-press behavior beyond row-tap stays as today
- **Cross-screen status chip** for screens that aren't home or Notes — dictation happens on home; the user would be on Notes anyway to see in-flight items.  Add later if the gap appears.
- **Public scheduler UI** for non-dictation reminders — `POST /api/v1/scheduler/notifications` already exists on Dragon, just no UI today.  Out of scope here.

## Locked design decisions (from brainstorm round)

| Decision | Choice | Rationale |
|---|---|---|
| Routing model | **Always a note, then convert** | Lowest risk of misrouting; one frictionless launch.  Action chips do the conversion post-save. |
| Use-case scope | All four (note / reminder / list / journal) | User picked "all"; system supports without forcing pre-classification. |
| Home entry | **Dictate chip in mode-chip language** (dark pill, thin border, monospace caption, red breathing dot, "Dictate · TAP TO START · 🎤") | Earlier amber gradient pills clashed with the muted home; user rejected.  Mode-chip style fits the existing language. |
| Orb status grammar | **Full morph** (body tint + breathing + ripple + caption) | User picked option C; expressive enough to read across the room. |
| Notes IA | **Timeline** — day-grouped (TODAY / YESTERDAY / THIS WEEK / OLDER), 44 px rows, type-dot color (amber/blue/green/red), filter pills, amber FAB | User picked option A; information density over per-row context. |
| Cross-screen status | **Mini-orb echo on Notes processing row** + heroic orb on home (no separate top-status-bar chip in v1) | Dictation happens on home; user would be on Notes anyway to see in-flight items.  Single visual grammar across surfaces. |
| Auto-stop | **VAD silence (4 s) + 5 min hard cap** | Port existing dictation-mode VAD to the new pipeline.  Hard cap from PR #517 extended to live path. |
| Action chip scope v1 | **Reminder + List only** | Reminder uses existing `POST /api/v1/scheduler/notifications`.  List = note with `note_type_t = LIST` + comma/and/newline split.  Task and others deferred. |
| Action chip threshold | confidence ≥ 0.75 | Below the bar = plain note, no chip.  Don't pester the user with iffy guesses. |

## Architecture

### Pipeline state model (new)

`dictation_pipeline_t` becomes the canonical state for any in-flight dictation.  Lives in `voice.c` (or a new `voice_dictation.c` if cleaner — TBD during planning).

```c
typedef enum {
    DICT_IDLE      = 0,
    DICT_RECORDING,
    DICT_UPLOADING,
    DICT_TRANSCRIBING,
    DICT_SAVED,           /* terminal — auto-transitions to IDLE after 2 s */
    DICT_FAILED,          /* terminal — manual retry only */
} dict_state_t;

typedef enum {
    DICT_FAIL_NONE = 0,
    DICT_FAIL_AUTH,
    DICT_FAIL_NETWORK,
    DICT_FAIL_EMPTY,
    DICT_FAIL_NO_AUDIO,
    DICT_FAIL_TOO_LONG,
} dict_fail_t;

typedef struct {
    dict_state_t  state;
    dict_fail_t   fail_reason;
    uint32_t      started_ms;        /* esp_timer_get_time / 1000 */
    uint32_t      stopped_ms;        /* 0 until RECORDING ends */
    uint32_t      duration_ms;       /* computed continuously while RECORDING */
    int           note_slot;         /* -1 if no note created yet; >=0 once SD WAV started */
    char          partial[256];      /* live stt_partial preview (if available) */
} dict_pipeline_t;
```

**Single global instance** (one dictation at a time in v1).  Module-level mutex (`s_dict_mutex`) guards transitions.  Subscribers register a callback that fires on state changes; UI surfaces subscribe (orb + Dictate chip + Notes processing row).

**State transitions:**

```
IDLE  --tap Dictate-->  RECORDING
RECORDING  --VAD silence 4s OR tap-stop OR 5min cap-->  UPLOADING
RECORDING  --tap cancel-->  IDLE (discard SD WAV)
UPLOADING  --HTTP 200-->  TRANSCRIBING
UPLOADING  --HTTP 401 / 4xx-->  FAILED (AUTH / NETWORK)
UPLOADING  --HTTP open fail-->  FAILED (NETWORK)
TRANSCRIBING  --200 + non-empty text-->  SAVED  --2 s timer-->  IDLE
TRANSCRIBING  --200 + empty text-->  FAILED (EMPTY)
TRANSCRIBING  --5xx-->  FAILED (NETWORK)
SAVED  --auto 2 s-->  IDLE
FAILED  --tap retry-->  UPLOADING
```

**The existing Notes-side polling task (15 s sweep) is preserved as a backup** for notes that get stuck in `NOTE_STATE_RECORDED` — e.g. after a reboot mid-pipeline.  The new pipeline is the foreground path; the polling task is the safety net.  PR #517's bearer-auth fix applies to both.

### UI surface contract

Three surfaces consume the pipeline state via a single `dict_subscribe(cb, user_data)` API:

| Surface | File | Renders |
|---|---|---|
| Heroic orb on home | `ui_orb.c` | Body gradient + breath rate + ripple + halo + caption-line under orb |
| Dictate chip on home | `ui_home.c` (new widget) | Dark pill, red dot pulse, label/caption swaps in place |
| Mini-orb echo on Notes | `ui_notes.c` (new processing-row widget) | 28 px mini orb + state caption + duration + cancel × |

All three surfaces *render the same state object*.  They never disagree — the pipeline is canonical.

Visual mapping per state:

| State | Orb body | Halo / ripple | Caption | Mini-orb | Dictate chip label |
|---|---|---|---|---|---|
| IDLE | circadian | normal breath | (none — say-pill text) | hidden | `Dictate · TAP TO START · 🎤` |
| RECORDING | red gradient + 0.85 s bloom | red halo on mic RMS + internal ripple rings | `● RECORDING 0:23` | red gradient, mini ripple | `● RECORDING · 0:23 · ×` |
| UPLOADING | amber gradient | amber arc 0→100 % | `UPLOADING…` | amber gradient | `UPLOADING…` |
| TRANSCRIBING | amber gradient | amber comet (existing PROCESSING animation, repurposed) | `TRANSCRIBING…` | amber gradient + comet | `TRANSCRIBING…` |
| SAVED (2 s) | green pulse | green ring expanding | `✓ Saved to Notes` | green pulse | `✓ Saved` |
| FAILED | red flat | none | `● AUTH — tap to retry` (reason varies) | red flat | `● AUTH — tap to retry` |

### Home layout

The current home stays exactly as it is today (after the wave-1.6 / 1.8 / 1.9 cleanup), with **one addition**: a new `s_dictate_chip` widget directly below the existing `s_mode_chip`, 12 px gap, same width (~280 px), same 36 px height, same dark-pill aesthetic.  No other chrome moves.

```
┌──────────────────────────────────────────┐
│  ● Remote (ngrok)        Thu · 19:18    │   status bar (unchanged)
│                                          │
│                                          │
│                 [orb]                    │   orb at y=320 (unchanged)
│                                          │
│                                          │
│        Evening, Emile —                  │   italic greeting (unchanged)
│        what's on your mind?              │
│                                          │
│        [● Local   ON-DEVICE   ⌄]         │   mode chip (unchanged)
│        [● Dictate TAP TO START 🎤]       │   ← NEW chip (this design)
│                                          │
│                                          │
│                                          │
│        Hold orb for modes      ⋮⋮        │   bottom say-pill + nav chevron
└──────────────────────────────────────────┘   (unchanged per user constraint)
```

The mode chip and Dictate chip together form a two-row stack of "what's the system doing" controls.

### Notes layout

Total rebuild of `refresh_list` + `add_note_card` in `ui_notes.c`.  Same file, same persistence layer, same load/save semantics — render path is what changes.

```
┌──────────────────────────────────────────┐
│  Notes              CLEAR N FAILED  7 ▴  │   topbar — preserves #517's clear button
├──────────────────────────────────────────┤
│  ● [mini-orb] RECORDING   0:23   ✕      │   processing row, only when in-flight
│                                          │   (single instance in v1 — see risk 6)
├──────────────────────────────────────────┤
│  [ALL] [NOTES] [REMINDERS] [LISTS]       │   filter pills (44 px)
├──────────────────────────────────────────┤
│  TODAY                                   │   day-section header (24 px)
│  ● 18:32  Roger.                  VOICE  │   row (44 px)
│  ● 17:05  Call mom Tuesday 6 PM REMIND   │
│      ┌───────────────────────────────┐   │   ← action chip on row above
│      │ 📅 Set reminder Tue 6 PM ✓ ✕  │   │
│      └───────────────────────────────┘   │
│  ● 14:21  Eggs, bread, butter…    LIST   │
│  YESTERDAY                               │
│  ● 22:14  Idea: revisit orb halo  VOICE  │
│  ● 11:03  en                       TEXT  │
│  THIS WEEK                               │
│  …                                       │
│                                          │
│                                  [🎤]    │   amber FAB, bottom-right
└──────────────────────────────────────────┘
```

**Removed permanent chrome** (vs today):
- `+ NEW VOICE NOTE` / `+ NEW TEXT NOTE` row (80 px)
- Always-on search bar (56 px)
- Voice/text buttons → replaced by amber FAB (long-press FAB = "new text note")

**Net vertical chrome:** today ≈ 200 px before first row.  New ≈ 88 px (topbar 44 + filter pills 44).  Plus the processing row (60 px) when active.  About 2.5 more rows visible.

**Row anatomy (44 px):**
```
●     18:32     Roger.                                 VOICE
└┐    └┐        └┐                                     └┐
type   time    body (single line, ellipsis on overflow)  state/type badge
6 px  40 px   flex                                       right-align
```

Tap a row → existing edit overlay (no redesign here).  Tap retry-button on FAILED row → flips state back to `NOTE_STATE_RECORDED` (existing #517 behaviour).

**Filter pills:** All / Notes / Reminders / Lists.  Tapping a pill filters in place.  No "Failed" pill — the topbar's CLEAR FAILED button already toggles the failed view when tapped (cycles: ALL → FAILED only → ALL).

**Persistence schema additions:**

```c
typedef enum {
    NOTE_TYPE_NOTE = 0,       /* default */
    NOTE_TYPE_REMINDER,
    NOTE_TYPE_LIST,
} note_type_t;

typedef struct {
    /* … existing fields from ui_notes.c (after #517) … */
    note_type_t  type;        /* persisted as "ty" — defaults to NOTE on missing */
    /* action chip pending state — cleared on confirm or dismiss */
    struct {
        uint8_t kind;         /* 0=none, 1=reminder, 2=list */
        uint8_t confidence;   /* 0-100, /100 from Dragon's float */
        char    payload[128]; /* ISO date for reminder, item-count + delimiter info for list */
    } pending_chip;           /* persisted as "pc" object */
} note_entry_t;
```

Both fields are forward-compatible (missing on disk = absent / NOTE).  No migration needed.

### Action chip data flow

Today Dragon already sends `dictation_summary` with `title` + `summary` after STT.  We extend it with a `proposed_action`:

```json
{
  "type": "dictation_summary",
  "title": "Call mom Tuesday",
  "summary": "Reminder to call mom about the July trip",
  "proposed_action": {
    "kind": "reminder",          /* "reminder" | "list" | "none" */
    "confidence": 0.86,
    "payload": {
      "when": "2026-05-19T18:00",
      "label": "Call mom"
    }
  }
}
```

**Dragon side:** new classifier pass after STT in `pipeline._post_process_dictation`.  Cheap local LLM call (ministral-3:3b, ~1 s).  System prompt asks for `{kind, confidence, payload}` JSON output.  NL date parsing for reminders re-uses the existing `dragon_voice/scheduler/parser.py`.

**Tab5 side:** Tab5 stores `pending_chip` on the note.  Notes screen renders the chip below the row body if confidence ≥ 0.75.  Tap ✓ → fires `POST /api/v1/scheduler/notifications` for reminders, in-place type flip for lists; chip cleared and persisted.  Tap ✕ → chip cleared, note stays NOTE.

**One chip per note maximum.**  If Dragon's classifier returns both reminder and list candidates, it picks the higher-confidence one before sending.

## Phasing

Four PRs, each branched off `main` after the prior PR merges (not stacked branches — #513→#514→#515 stacking was painful):

**Merge order:** PR 1 → PR 2 + PR 3 (can land in parallel, both depend only on PR 1) → PR 4 (depends on PR 3's schema additions).  Each PR is independently revertable.

### PR 1 — `feat(voice): dictation pipeline state model + VAD auto-stop`

Backend wiring only.  No visible UI change.

**Files:**
- `main/voice.c` (or new `main/voice_dictation.c`) — `dict_pipeline_t` state machine + transitions + subscriber registry
- `main/voice.h` — public API: `dict_get_state`, `dict_start`, `dict_stop`, `dict_cancel`, `dict_subscribe`
- `main/voice_ws_proto.c` — hook into existing `dictation_summary` handler to drive `DICT_TRANSCRIBING → DICT_SAVED`
- Existing dictation/notes paths instrument transitions; visible behaviour unchanged

**Acceptance:**
- Long-press orb (existing path) drives the new state machine; `/dictation` debug GET returns the new state name
- VAD silence auto-stops at 4 s (matches today's dictation-mode behaviour)
- 5 min hard cap auto-stops with `DICT_FAIL_TOO_LONG`
- All existing dictation tests pass; new unit test for state-machine transitions

### PR 2 — `feat(ui): orb + Dictate chip surface the pipeline state`

Visible win — heroic orb morphs through pipeline states; new Dictate chip on home.

**Files:**
- `main/ui_orb.c` — new `ui_orb_set_pipeline_state(state, fail_reason, duration_ms)` API; new gradient/halo/ripple/comet variants per state; caption-line under orb
- `main/ui_orb.h` — exports above
- `main/ui_home.c` — new `s_dictate_chip` widget below mode chip; subscribes to pipeline; renders state-appropriate label
- `main/ui_home.h` — no new public exports

**Acceptance:**
- Tap Dictate chip → orb morphs RECORDING → UPLOADING → TRANSCRIBING → SAVED → IDLE
- Mic RMS visibly drives the red halo bloom rate during RECORDING
- Cancel × on chip aborts the pipeline cleanly (no orphan SD WAV)
- Live verification on Tab5 192.168.1.90 — visual confirmation of all five states
- No regression in voice-ask (orb's existing LISTENING/PROCESSING/SPEAKING states unaffected)

### PR 3 — `feat(ui): Notes screen reimagined — Timeline IA + processing row + FAB`

Full Notes rerender.

**Files:**
- `main/ui_notes.c` — rebuild `refresh_list` + `add_note_card`; add day-section logic + filter-pill state + processing-row widget + amber FAB; remove old voice/text button row + always-on search bar
- `main/ui_notes.h` — minor additions if any (likely none)

**Schema additions** (forward-compatible):
- `note_type_t` enum + `type` field on `note_entry_t` + `"ty"` JSON key
- `pending_chip` substruct + `"pc"` JSON key (populated by PR 4; PR 3 just allocates space)

**Acceptance:**
- 14 existing notes render correctly in the new timeline (no missing rows, no crashes)
- Filter pills correctly filter by type
- Tap FAB → triggers the same pipeline as the home Dictate chip
- Processing row appears at top when any dictation is in flight; mini-orb mirrors heroic orb state
- Empty / loading / search-active states render without breakage
- Hide/show pattern preserved for the screen-recreate path (per CLAUDE.md fragmentation rules)
- No PANIC across 50 fast home↔notes navs

### PR 4 — `feat(notes): action chips for reminder + list conversion`

Cross-stack (Dragon + Tab5).

**Dragon files:**
- `dragon_voice/pipeline.py` — extend `_post_process_dictation` with classifier pass
- `dragon_voice/dictation/classifier.py` (new) — local LLM call + JSON validation + confidence floor
- `dragon_voice/protocol/dictation_summary.py` (or wherever the frame is built) — add `proposed_action` field
- Tests for classifier + protocol shape

**Tab5 files:**
- `main/voice_ws_proto.c` — parse `proposed_action` from `dictation_summary`; persist as `pending_chip` on the matching note
- `main/ui_notes.c` — chip widget rendering + tap handlers (✓ confirms + fires REST call or flips type, ✕ dismisses)
- `main/voice.c` (or new helper) — `POST /api/v1/scheduler/notifications` call for reminder confirmation

**Acceptance:**
- Live: dictate "call mom Tuesday at six pm" → chip appears with parsed date → ✓ creates scheduled notification on Dragon
- Live: dictate "eggs, bread, butter, oat milk" → list chip → ✓ splits transcript into 4 bullet items + type flips to LIST
- Confidence < 0.75 → no chip (verified by dictating a vague utterance)
- ✕ dismiss clears chip + persists; reopen Notes confirms it's gone
- Dragon predates Tab5 update: Tab5 ignores `proposed_action` field gracefully, no chip rendered (forward-compat)
- Tab5 predates Dragon update: existing `dictation_summary` shape continues to work, no errors

## Risks + open questions

1. **Mic RMS for orb bloom during RECORDING** — exists today on the LISTENING state.  Reuse should be straightforward but PR 2 needs to verify the wiring works for the new pipeline state path.
2. **VAD silence-detection accuracy** — the existing dictation-mode VAD has been live a while; porting should be low-risk but the new pipeline's 4 s threshold is a tuning knob to live-validate.
3. **Notes 14-row rerender perf** — the timeline render path is more complex (day-grouping + filter check + chip widget per row).  44 px rows mean 12+ visible; LVGL render budget should be fine but profile during PR 3.
4. **Action-chip confidence calibration** — 0.75 is a guess.  Real-world tuning during PR 4; may move up if false positives are annoying.
5. **What happens on Dragon disconnect mid-dictation?** — RECORDING continues to SD (existing fallback path).  When WS recovers, the SD WAV gets picked up by the 15 s polling task and goes through REST `/api/v1/transcribe` with bearer (#517 fix).  The pipeline state during disconnect should transition to a special `OFFLINE_QUEUED` state, or just stay UPLOADING with a longer timeout.  TBD during PR 1.
6. **Concurrent multi-dictation** — v1 is single-instance.  If user starts a second dictation while the first is uploading, second blocks with a toast "wait — still saving last one".  Probably fine for v1; revisit if real demand emerges.

## Verification

Per-PR live-on-device verification on Tab5 192.168.1.90 ↔ Dragon 192.168.1.91.  No PR ships without a screenshot proving the state transitions visibly work.  Notes screen passes the "no missing rows on 14 existing entries" check before merge.

End-to-end happy path (post-PR 4):

1. Tap Dictate chip on home
2. Say "Call mom Tuesday at six pm"
3. Stop talking
4. Orb cycles RECORDING (red, ripple) → UPLOADING (amber arc) → TRANSCRIBING (amber comet) → SAVED (green pulse) → IDLE
5. Navigate to Notes
6. New row at top of TODAY group with the transcript
7. Reminder chip below the row with parsed date
8. Tap ✓ → chip disappears, type-dot turns blue, badge says REMIND
9. Scheduled notification fires on Tuesday at 6 PM via existing Dragon scheduler

If this works, the original "i want it to give me status like transcribing, sent, something that shows me whats going on" + "see whats being processed in notes" + "notes should be reimagined" are all closed.

## Cross-links

- [PR #517](https://github.com/lorcan35/TinkerTab/pull/517) — bearer-auth + reason chips + retry/clear surfaces (this spec builds on it)
- [Notes/dictation audit memory](../../../.claude/projects/-home-rebelforce/memory/project_dictation_audit_2026_05_14.md) — root-cause + live verification of the 401 bug that motivated this work
- [Orb redesign spec 2026-05-14](2026-05-14-orb-redesign-design.md) — orb state machine + visual language that this builds on
- TinkerTab `CLAUDE.md` — NVS schema, dictation-mode VAD details, scheduler endpoint signature
- TinkerBox `CLAUDE.md` — `POST /api/v1/scheduler/notifications` shape, `dictation_summary` WS frame
