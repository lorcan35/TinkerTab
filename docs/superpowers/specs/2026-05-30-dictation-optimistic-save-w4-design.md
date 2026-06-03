# Dictation Optimistic Save (W4) — Design

> **Status:** approved design, ready for implementation plan.
> **Program:** W4 of the dictation structural redesign (see
> `2026-05-30-dictation-redesign-design.md`; W1/W2/W3 shipped on
> `feat/dictation-redesign`). This is the "single persistence + dedup-by-turn_id
> + retry" wave, delivered as an **optimistic-save** UX.

**Goal:** Stopping a dictation is *done the instant you stop it*. The orb returns
to ready immediately and the note lands in Notes right away; transcription, title,
summary, and search-embedding fill in afterward, in the background, with zero user
action — even across a Dragon outage.

**One-sentence principle:** Decouple **capture** (owns the orb, finishes at stop)
from **enrichment** (owns a note-level badge, runs in the background).

---

## Why

Today, when you stop a dictation:

- The audio is already captured — streamed to Dragon live, or saved as a `.wav`
  on SD when offline — and Dragon auto-creates the note within ~2 s (`note_created`
  observed landing + syncing `201` at stop time).
- **But** the Tab5 UI then blocks in "Generating summary…" until Dragon's LLM
  produces a title + summary, which on Local mode can take 60–90 s. The user is
  pinned to a processing orb waiting on the *slowest, most failure-prone* step.

The save isn't the problem; the note + transcript exist almost immediately. The
problem is that we gate the "done" UX on the LLM enrichment. That is backwards,
and it makes a slow/flaky enrichment step able to block — or appear to lose — a
recording. (Live evidence the fragile step bites: a 10-min dictation's
`Embedding failed: Server disconnected`, and the summary wait stalling the orb.)

## Approved decisions (from brainstorming)

- **D-UX1 — Stop experience:** orb snaps back to idle/ready instantly + a quiet
  toast `Saved — summarizing…`. The note appears in Notes with a transcribing/
  pending badge. (Chosen over a "Saved ✓" beat or auto-jumping to Notes.)
- **D-UX2 — Failure / offline path:** **auto-finish later, silently.** The note
  saves immediately with whatever transcript exists + a quiet `pending` badge;
  Tab5 re-drives transcribe+summary in the background whenever Dragon is
  reachable; it completes on its own and the badge clears. No manual retry chip,
  no lost dictation.
- **D-D1 (carried from the parent spec):** Dragon `notes/db.py` remains the sole
  **authoritative** note owner. The Tab5 offline note is a *transient optimistic
  placeholder* keyed by `turn_id`, reconciled to Dragon's note on sync (see
  Reconciliation). No duplicate notes.

---

## Architecture: two decoupled layers

### Layer 1 — Capture (owns the orb)

The `voice_dictation.c` FSM. The orb follows **capture only**:

- `ui_orb_pipeline_active()` becomes true for **`DICT_RECORDING` only** (was
  `state != DICT_IDLE`). The moment recording stops, capture is complete (audio
  is streamed for WS, or on SD for offline) and the orb returns to idle.
- On stop, the FSM no longer holds the orb in `TRANSCRIBING` waiting for the
  summary. `TRANSCRIBING`/`UPLOADING` become **background note states**, not orb
  states. `resolution_pending` (W2) and the W3 stuck-watchdog continue to bound
  the FSM's own return to `IDLE`, but they no longer drive the orb.

Net: the "Generating summary…" orb state is gone. Capture ends → orb idle → toast.

### Layer 2 — Enrichment (owns the note badge)

The Notes layer (`ui_notes.c` + Dragon). A note carries its **own** lifecycle,
surfaced as a small badge on the note row — independent of the orb/FSM:

```
   transcribing…  →  summarizing…  →  done        (badge clears)
                  ↘  pending (Dragon away)  ↗      (auto-retries, then done)
```

For an online turn the transcript is essentially complete at stop (streamed
partials), so the badge is usually just `summarizing…` and clears within ~2 s.
For offline it walks the full chain.

---

## Flow

### Stop (always — instant, identical from the user's view)

1. FSM: `RECORDING → SAVED` (capture done). Orb returns to idle.
2. `ui_home_show_toast("Saved — summarizing…")` (now marshaled — safe from any task).
3. A note row for this `turn_id` is shown in Notes immediately with the
   transcript-so-far + the appropriate badge.

### Online (Dragon connected)

4. Dragon already auto-creates the note at stop and emits `note_created` (~2 s)
   then `dictation_summary` (title + summary) — both carry `turn_id`.
5. Tab5 matches by `turn_id`, fills in title/summary/final transcript **in place**,
   clears the badge. No new row.

### Offline (Dragon unreachable)

4. Tab5 writes a **local placeholder note** immediately from the SD `.wav` +
   partial transcript, badge `pending`.
5. The existing transcription/offline queue re-drives transcribe+summary whenever
   Dragon is reachable. On success, Dragon's authoritative `note_created` (same
   `turn_id`) **reconciles** the placeholder (adopts Dragon's `note_id`), then
   `dictation_summary` clears the badge. Still no second row.

### Reconciliation (the join that preserves D-D1 and prevents dups)

The note row is keyed by **`turn_id`** (W1/W2 identity, already echoed both ways).
When Dragon's authoritative `note_created` arrives:

- If a placeholder row with that `turn_id` exists → **adopt** Dragon's `note_id`
  into it and update fields (no new row).
- Else → create the row from Dragon's note (normal online case where the
  placeholder and `note_created` race — last writer by `turn_id` wins, single row).

Dragon stays the sole authoritative owner; the placeholder is a transient view.

---

## State model

### Note enrichment state (new, note-level)

A per-note field (Tab5 side; mirrors what Dragon reports):

| State | Meaning | Badge |
|-------|---------|-------|
| `ENRICH_TRANSCRIBING` | audio captured, transcript not final (offline/upload) | `transcribing…` |
| `ENRICH_SUMMARIZING` | transcript final, title/summary pending | `summarizing…` |
| `ENRICH_PENDING` | Dragon unreachable; will auto-finish on reconnect | `pending` |
| `ENRICH_DONE` | title + summary present | (none) |

The badge is shown whenever a note is not `ENRICH_DONE`. For the fast online case
it appears briefly (~2 s) and clears — accepted as honest/consistent. (Tunable:
a future debounce could suppress it under N seconds; **default is always-show** for
simplicity and to keep the offline and online code paths identical.)

### Capture FSM (unchanged states, changed orb mapping)

States stay `IDLE/RECORDING/UPLOADING/TRANSCRIBING/SAVED/FAILED/CANCELLED`. The
only behavioral change is the **orb mapping** (`ui_orb_pipeline_active` →
RECORDING-only) and that stop snaps toward `SAVED` for the orb's purposes rather
than parking the orb in `TRANSCRIBING`. The FSM's `turn_id`, `resolution_pending`,
self-decay, and stuck-watchdog are retained as-is for FSM liveness.

---

## What changes, by repo

### Tab5 (TinkerTab) — the bulk

- **`main/ui_orb.c`** — `ui_orb_pipeline_active()` returns true for
  `DICT_RECORDING` only (lock-free `voice_dictation_state()`), so the orb returns
  to idle at stop instead of during the summary wait.
- **`main/voice.c` / `main/voice_dictation.c`** — at stop, drive the capture FSM
  so the orb is released immediately (capture complete). Keep `turn_id` +
  `resolution_pending` + stuck-watchdog for FSM bookkeeping; they no longer gate
  the orb.
- **`main/ui_home.c`** — fire the `Saved — summarizing…` toast on stop (uses the
  now-marshaled `ui_home_show_toast`).
- **`main/ui_notes.c`** — the heart of the change:
  - Create/ensure a note row for the live `turn_id` at stop (placeholder when
    offline; or the streamed-transcript row when online).
  - Render the enrichment badge from the note's enrichment state.
  - **Reconcile by `turn_id`** on `note_created` (adopt `note_id`, no dup row).
  - Consume `dictation_summary` / note-update as an **in-place** update (title +
    summary + final transcript), then clear the badge — instead of treating it as
    the gate that ends the dictation.
  - Background re-drive: on Dragon reconnect, re-run transcribe+summary for any
    `ENRICH_PENDING` notes (extends the existing transcription/offline queue).
- **`main/voice_ws_proto.c`** — route `note_created` / `dictation_summary` to the
  note-update path (by `turn_id`) rather than the orb-resolution path.

### Dragon (TinkerBox) — minor

- **`note_created` must carry `turn_id`** so Tab5 can reconcile. (Verify; add if
  missing.) `dictation_summary` already carries it (W2).
- **Embedding must not block the note.** The note is saved/usable the moment the
  transcript exists; embedding (semantic index) runs after and **retries in the
  background** on failure — never blocks or fails the note. (Fixes the observed
  `Embedding failed: Server disconnected` degrading the save.)
- Optional: an explicit `note_updated` frame if reusing `dictation_summary` for
  the in-place update proves ambiguous; otherwise reuse `dictation_summary`.

---

## Protocol delta (see TinkerBox `docs/protocol.md`)

- `note_created` (Dragon→Tab5): **add `turn_id`** field. Tab5 uses it to reconcile
  the optimistic placeholder. Backward-compatible (absent `turn_id` → treat as a
  fresh note, current behavior).
- `dictation_summary` (Dragon→Tab5): unchanged shape (already has `turn_id`);
  Tab5 now applies it as an **in-place note update** instead of an
  end-of-dictation gate.

---

## Error handling / resilience

- **Dragon offline at stop:** local placeholder note saved immediately
  (`ENRICH_PENDING`); background queue completes it on reconnect → reconcile by
  `turn_id`. (D-UX2.)
- **Summary/transcription fails on Dragon:** the note keeps the raw transcript and
  stays `ENRICH_PENDING`; the background queue retries (auto). The W3 stuck-watchdog
  still bounds the FSM's own return to IDLE so the *orb/FSM* never wedges
  (orthogonal to the note badge now).
- **Embedding fails:** note is saved + usable; only semantic search of that note
  is deferred until a background re-embed succeeds. Never surfaced as a failure.
- **Stop with no Dragon note ever arriving** (extreme): the placeholder remains a
  valid local note with the captured audio + partial transcript; it is never lost.
- **Rapid stop→start:** each turn has its own `turn_id`; a late update for turn A
  reconciles A's row, never B's (W2 turn_id gating already guarantees this).

## Testing

- **Host (FSM, `tests/host/test_voice_dictation.c`):** orb-active maps to
  RECORDING-only; stop reaches the orb-idle condition immediately; turn_id stable
  across the capture→enrichment handoff.
- **Tab5 reconciliation (host or harness):** placeholder created on stop;
  `note_created` with matching `turn_id` adopts the row (single row, not two);
  non-matching `turn_id` does not corrupt the placeholder.
- **Live (debug HTTP + `/debug/inject_ws`):**
  - Online stop → orb idle within one frame; toast fires; note row appears;
    `dictation_summary` injected → title/summary fill in place, badge clears.
  - Offline stop (Dragon WS down) → placeholder `pending`; bring Dragon back →
    note auto-completes, badge clears, exactly one row.
  - Reconnect-churn does not duplicate or orphan notes.
- **Dragon (pytest):** `note_created` carries `turn_id`; an embedding failure does
  not fail/roll back the note insert.

## Scope / non-goals

- **In scope:** the orb decoupling, the note placeholder + badge + reconciliation,
  background auto-finish, the `note_created.turn_id` + non-blocking-embedding
  Dragon changes.
- **Not in scope (W5 / later):** de-bloating `ui_notes.c`, de-forking the
  dictation enums, the dispatch-table RX extraction, any change to Dragon's
  summarization model or STT backend. The badge-debounce ("only show if >N s") is
  a deferred tunable, not built here.

## Open questions

None blocking. The badge-on-fast-online question (D-UX1 follow-up) is resolved by
the always-show default with a noted future debounce.
