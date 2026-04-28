# Widget Platform — Implementation Plan

> **Status: living plan; will be archived after Phase 1 ships.** This
> is the step-by-step build order — exact files, exact commit
> boundaries. Will move to `docs/historical/` once Phase 1
> (`widget_live` slot) lands and remaining phases transition to
> tracking-in-GitHub-issues. The companion spec [`WIDGETS.md`](WIDGETS.md)
> (the design lock) stays as a living reference.

**Branch:** `feat/widget-platform`
**Spec:** [`docs/WIDGETS.md`](./WIDGETS.md)
**Mockups:** [`.superpowers/brainstorm/widget-platform/`](../.superpowers/brainstorm/widget-platform/)
**Companion:** TinkerBox `docs/protocol.md` §17 + OpenClaw `docs/tools/skills.md` widget section.

This document is the build order. It's *prescriptive* — exact files, exact
commit boundaries, exact test gates. Follow top to bottom.

---

## Phase 0 — Prerequisites ✓ (done)

- [x] Merge `feat/ui-overhaul` to main (30+ commits shipped: v5 UI + stability)
- [x] Close 12 settled issues (#54, #56, #58, #60-66)
- [x] Audit TinkerBox (no existing surface concept; Tab5Surface is new; extends `dragon_voice/`)
- [x] Audit OpenClaw (skills are prose + YAML frontmatter; widget surface is new)
- [x] Design mockups (`.superpowers/brainstorm/widget-platform/` — hero, Time Sense 5 states, multi-timer, icon library, README)
- [x] Master spec (`docs/WIDGETS.md`)
- [x] Branch `feat/widget-platform` created from main

---

## Phase 1 — `widget_live` end-to-end

Smallest slice that proves the loop. When this phase is done, saying "pomodoro"
into the orb causes the orb to shift to calm emerald with narrative text,
tick down through tones (calm → active → approaching → urgent), let user tap
PAUSE mid-run, and end with a done-card.

### 1.1 Protocol spec (TinkerBox)

**File:** `TinkerBox/docs/protocol.md`
**Action:** Append §17 Widget Messages. Schemas for `widget_live`,
`widget_live_update`, `widget_live_dismiss`, `widget_action`. Mirror §12–15
style (existing rich-media section).

**Commit:** `docs(protocol): add §17 widget messages (refs #<TB-issue>)`

### 1.2 Tab5 — widget data model

**New files:**
- `main/widget.h` — typedef `widget_t`, enums `widget_type_t`, `widget_tone_t`, public API
- `main/widget_store.c` — CRUD over fixed-size PSRAM array, eviction by age
- `main/widget_store.h` — public functions

**Contract:**
```c
void        widget_store_init(void);
widget_t*   widget_store_upsert(const widget_t *w);   // create or update
widget_t*   widget_store_find(const char *card_id);
void        widget_store_dismiss(const char *card_id);
widget_t*   widget_store_live_active(void);           // highest-priority active live
int         widget_store_live_count(void);            // queue depth for counter pill
void        widget_store_gc(uint32_t now_ms);         // evict expired
```

All in PSRAM. Zero internal-SRAM allocation. Bounded to 32 entries.

**Tests:** `test/e2e/test_widget_store.c` — upsert, find, eviction, priority ordering.

**Commit:** `feat(widget): widget_store CRUD + priority queue (refs #<TT-store-issue>)`

### 1.3 Tab5 — WS message handlers

**File:** `main/voice.c`
**Edits:** extend `_handle_text_frame()` switch to accept:
- `widget_live` → parse JSON, upsert into widget_store, mark active, trigger ui_home live-slot refresh
- `widget_live_update` → find card_id, merge changed slots, trigger refresh
- `widget_live_dismiss` → mark inactive, trigger refresh

**Guard:** unknown widget type → log + ignore (forward-compat).

**Commit:** `feat(voice): widget_live WS handlers (refs #<TT-voice-issue>)`

### 1.4 Tab5 — home live-slot integration

**Files:**
- `main/ui_home.c` — in `ui_home_update_status()` add priority check:
  1. If `widget_store_live_active()` returns a widget, render its title + body
     in `s_poem_label`, its tone drives orb color + breathing, its icon
     replaces the mode-mark icon (existing `s_mode_diamond` + `s_mode_label`).
  2. Else fall through to existing edge-state logic.
- `main/ui_home.h` — add `ui_home_refresh_widget_slot()` public (called from
  widget_store after any change, via `lv_async_call`).

**Orb tone mapping:** existing `orb_paint_for_mode()` gets a sibling
`orb_paint_for_tone()` that accepts `widget_tone_t` and returns the
(top_color, bot_color) pair + target breathing-rate.

**Breathing rate:** exists in voice.c (`start_breathe_anim`) for the voice
overlay. For home orb, extend `ui_home.c` with a simple Y-float anim whose
period varies by tone (4000ms calm, 3000ms active, 2000ms approaching,
1000ms urgent, disabled for done).

**Counter pill:** new static `s_counter_pill` obj at bottom-left. Visible when
`widget_store_live_count() > 1`. Tap → (later phase) cycle; long-press →
stack sheet (phase 3).

**Commit:** `feat(home): integrate widget live slot with orb + poem (refs #<TT-home-issue>)`

### 1.5 Tab5 — widget_action back-channel

**File:** `main/voice.c`
**Add:** `voice_send_widget_action(const char *card_id, const char *event)` —
emits `{"type":"widget_action", "card_id":..., "event":...}` on the voice WS.

**Wire:** in `ui_home.c` when the live widget's action label is visible and
tapped, call this. Rate-limit at 4/sec.

**Commit:** `feat(voice): widget_action TX back-channel (refs #<TT-voice-issue>)`

### 1.6 Dragon — `Tab5Surface` facade

**New files:**
- `dragon_voice/surfaces/__init__.py`
- `dragon_voice/surfaces/base.py` — `Tab5Surface` class with `live`, `live_update`, `live_clear`, `card`, `dismiss`
- `dragon_voice/surfaces/manager.py` — per-session state: active live widgets, priority resolver, routes action events back to skills

**Wiring:**
- `dragon_voice/server.py` create_app() — instantiate `SurfaceManager`, attach to session state
- `dragon_voice/pipeline.py` — expose `session.surface` to tools/skills

**Commit (TinkerBox repo):** `feat(surface): Tab5Surface facade + manager (refs #<TB-surface-issue>)`

### 1.7 Dragon — Time Sense reference skill

**New file:** `dragon_voice/tools/timesense_tool.py`

Follows the `dragon_voice/tools/base.py` pattern (existing tool convention —
memorize/recall/web_search are the nearest examples). Skill receives voice
invocations via LLM tool-call path; emits widgets via
`session.surface.live(...)`.

**Manifest (forward-compat):** `dragon_voice/tools/timesense.manifest.yml` — listed
voice triggers + surfaces. Not loaded in phase 1, but present for OpenClaw
skill-porting parity later.

**Commit:** `feat(skill): timesense reference skill — widget_live pomodoro (refs #<TB-skill-issue>)`

### 1.8 End-to-end verification

**Test:**
1. Flash Tab5 with widget_live support.
2. Restart Dragon with Tab5Surface wired.
3. Say "pomodoro". Verify:
   - Orb shifts from amber idle to emerald calm
   - `s_poem_label` shows `Deep work / 25:00 remaining`
   - Every second, the body updates
   - At 90% progress, tone shifts to `approaching` (amber hot)
   - At 98%, tone shifts to `urgent` (rose)
   - Tap PAUSE → skill logs pause event, tone stays frozen
   - On completion, orb settles to green, card appears in chat saying "Done · 25 min"

**Acceptance gate:** above works three times in a row without any reboot.

**Commit:** `test(widget): end-to-end timesense verification (refs #<TT-e2e-issue>)`

---

## Phase 2 — `widget_card` with actions

### 2.1 Tab5 — extend card renderer

**File:** `main/chat_msg_view.c`
**Edit:** existing `card` renderer gets new optional `action` slot → renders
as a caption-style button at the bottom of the card with amber border.

**Wire:** action tap → same `voice_send_widget_action()` path.

**Commit:** `feat(chat): card widget gains tappable action slot`

### 2.2 Dragon — Tab5Surface.card

Already sketched in phase 1; formalize the `card()` method, add to
manager.py emission path.

### 2.3 Sample skills

Port 3 existing OpenClaw notifications to emit `widget_card` instead of
prose — e.g., email-drafted-card, meeting-reminder-card, weather-morning-card.

### 2.4 Agents overlay integration

`ui_agents.c` — render completed `widget_card` entries as historical rows.
Auto-linked to skill_id for retrospective.

---

## Phase 3 — `widget_list` + `widget_prompt` + stack sheet

### 3.1 Tab5 — list + prompt renderers

**New file:** `main/ui_widget.c` — centralizes non-live widget rendering.
- `list` → clones `ui_sessions.c` structure
- `prompt` → opens modal with keyboard (existing ui_keyboard.c) or voice-only

### 3.2 Tab5 — stack sheet overlay

**New file:** `main/ui_skills.c` — the stack bottom sheet (see mockup
`01-time-sense-flow.html` "STACK · bottom sheet").
Trigger: long-press on counter pill.

### 3.3 Counter pill cycle

Tap counter pill → cycles to next active live widget in the queue.
`widget_store_live_cycle_next()` promotes the next-highest-priority widget.

---

## Phase 4 — `widget_chart` + capability downgrade

### 4.1 Tab5 — chart renderer

Sparkline via `lv_line` with series points as vector coords. Bar via `lv_bar`
grid. Gauge via `lv_arc`. All in `main/ui_widget.c`.

### 4.2 Dragon — capability-aware router

`dragon_voice/surfaces/manager.py` — before emitting widget, consult device
capability list. If `chart` not supported, convert to `card` with text body
(`avg 42, trending up`).

### 4.3 First real chart skills

Three OpenClaw skills ported: spending glance, focus minutes, mood arc.

---

## Phase 5 — Second device renderer (Waveshare round or partner device)

Outside this repo's scope. But v1 spec is stable enough for an external
implementer to build a renderer that honors it.

---

## File-level summary

### New Tab5 files
```
docs/WIDGETS.md                              ✓ (this branch)
docs/PLAN-widget-platform.md                 ✓ (this branch)
.superpowers/brainstorm/widget-platform/*    ✓ (this branch)

main/widget.h                                phase 1
main/widget_store.c / .h                     phase 1
main/ui_widget.c / .h                        phase 3 (live can inline in ui_home.c for P1)
main/ui_skills.c / .h                        phase 3
test/e2e/test_widget_store.c                 phase 1
test/e2e/test_widget_e2e.py                  phase 1
test/e2e/widget_stress.py                    phase 5
```

### Modified Tab5 files
```
main/voice.c                   P1: WS handlers + widget_action TX
main/ui_home.c                 P1: live slot priority + orb tone
main/chat_msg_view.c           P2: card.action slot
main/CMakeLists.txt            P1: add widget_store.c; P3: add ui_widget.c + ui_skills.c
CLAUDE.md                      P1: add Widget Platform section
LEARNINGS.md                   as issues arise per phase
README.md                      P1: mention Widget Platform in feature list
```

### New Dragon files
```
dragon_voice/surfaces/__init__.py            phase 1
dragon_voice/surfaces/base.py                phase 1
dragon_voice/surfaces/manager.py             phase 1
dragon_voice/tools/timesense_tool.py         phase 1
dragon_voice/tools/timesense.manifest.yml    phase 1 (forward-compat)
```

### Modified Dragon files
```
dragon_voice/server.py                       P1: wire SurfaceManager into create_app
dragon_voice/pipeline.py                     P1: expose session.surface
dragon_voice/tools/__init__.py               P1: register Timesense tool
docs/protocol.md                             P1: §17 widget messages
LEARNINGS.md                                 P1+: pattern notes
```

### OpenClaw
```
docs/tools/skills.md                         P2: add "widget surface" section
                                             (showing how to declare surfaces
                                             in SKILL.md frontmatter, forward-
                                             compat with TinkerClaw)
```

---

## Git workflow (per CLAUDE.md)

Each sub-phase of Phase 1 is:

1. **Issue first.** File a GitHub issue on the relevant repo following the
   Bug/Root Cause/Culprit/Fix/Resolved format.
2. **Commit with issue ref.** `feat(...): description (refs #N)` or
   `(closes #N)` for the final sub-commit of the issue.
3. **Push after each commit.** Don't batch.
4. **Atomic commits.** One sub-phase = one commit, even if small.

### Issues to file (at start of implementation)

TinkerTab:
- `feat(widget): widget_live protocol + Tab5 renderer`
- `feat(widget): widget_action back-channel + home integration`
- `feat(widget): icon library render path`
- `feat(widget): phase-2 card action + phase-3 list/prompt`

TinkerBox:
- `feat(surface): Tab5Surface facade + SurfaceManager`
- `feat(skill): timesense reference skill`
- `docs(protocol): §17 widget messages`

---

## Rollback

At every phase boundary the device is shippable as-is. If phase 1 breaks,
revert the branch — main still has the v5 UI + stability merge and the
device continues working.

Widget store is a pure addition (new code + new WS types). Existing WS
messages (`media`, `card`, `audio_clip`) are unchanged in phase 1. Existing
screens are untouched.

---

## What this plan explicitly does NOT do

- **No Tab5 firmware flash is required for new skills.** That's the whole
  point. Phase 1 ships the plumbing; every subsequent skill is Python-only
  on the brain.
- **No changes to the voice pipeline.** Widget_live etc ride the existing
  voice WS. STT/LLM/TTS are unaffected.
- **No touch of rich-media JPEG path.** Phase 1 doesn't use `media` at all;
  phase 2 merely aliases `widget_media` to the existing `media` renderer.
