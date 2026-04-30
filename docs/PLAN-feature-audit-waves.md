# PLAN — Feature-Audit Waves (April 2026 punch-list)

> **Source:** comprehensive audit on 2026-04-30 across Dictation / Onboard
> (K144) / Agent (TinkerClaw) / Cloud / Hybrid.  Cross-cutting themes
> identified: mode/model invisible mid-session, cost/spend has no daily
> surface, errors fail silently when a UI handler is missing, capability
> negotiation is partial (35-model router landed but Tab5 still hardcodes
> some checks), onboarding doesn't position the modes.  Tracking under
> #328 like the prior audit hardening.

## Method (applies to every wave)

1. Code change scoped to ONE coherent area per wave.  Never bundle.
2. **Build → flash → reboot → wait alive.**  Never test against a
   not-yet-flashed binary.
3. **Run user-story scenarios** against the live device via the existing
   `tests/e2e/` harness.  Each wave gets ≥10 stories that walk through
   the change *and* the surfaces it touches (regression net).  Stories
   are touch-driven (taps, swipes, long-presses) — not synthetic API
   calls — and verify outcomes via `/events`, `/screen`, `/voice`.
4. Per-wave artifacts:
   - Code commit (squash-mergeable, refs #328).
   - New scenario file under `tests/e2e/scenarios/` (e.g.,
     `wave1_visibility.py`) with the stories.
   - Screenshots from the run kept in `tests/e2e/runs/`.
5. Verify all `--quick` touch-stress phases still pass after each wave
   (regression check — we added 14 phases as the safety net, use them).
6. Push to `main` only after green tests.

## Wave order (re-confirmed against actual code state)

After spot-checking the audit findings against the source, several
"gaps" turn out to already be wired (mode is dynamic via
`tab5_settings_get_voice_mode()`; per-message receipts already render
in `chat_msg_view.c:548`).  The waves below are the ones that survive
verification.

### Wave 1 — Visibility (daily spend + dictation lie) ← THIS SESSION
**Concrete gaps that survived verification:**
- **Daily spend has no surface.**  Per-message stamps already render
  in chat bubbles (verified at `chat_msg_view.c:548`).  But there is
  no rolling total + cap progress visible anywhere.  User can hit cap
  and silently downgrade with no warning.
- **Orb subtitle lies.**  `ui_home.c:787` sets `"HOLD FOR DICTATE"`
  but `ui_home.c:1722 orb_long_press_cb` calls `ui_mode_sheet_show()`
  — confirmed by reading both sites.  Either the label is wrong, or
  the binding is wrong.

**Files:**
- `main/chat_header.{c,h}` — add daily-spend badge slot
- `main/settings.{c,h}` — surface cap-progress helper (already exposes
  `spent_mils` / `cap_mils`; just need a derived "%-of-cap")
- `main/ui_home.c:787` — fix the label OR rebind the action.  Pick
  whichever matches user intent (we'll go with **fix the label** —
  the mode-sheet behaviour is already discoverable + tested via Wave 8
  of the prior audit).

**User stories (≥10) — all touch-driven:**
1. Boot → home; daily-spend badge shows "$0.000 / $1.000" (default cap).
2. Send a Cloud-mode text turn; badge updates to non-zero on next paint.
3. Send a Local-mode text turn; badge stays unchanged (FREE turns
   don't accumulate).
4. Cap exceeded story: set `cap_mils=100` (1¢), send a Cloud turn
   that costs more, badge turns red, toast "daily cap reached."
5. Settings → bump cap to $2.00; badge re-renders with new
   denominator.
6. Day-rollover: warp `spent_day` back, send a turn; today's badge
   resets to fresh accumulation.
7. Open chat overlay from home; badge present in chat header *and*
   home pill (or wherever we stamp it — TBD).
8. Switch mode via long-press orb → mode sheet → Cloud preset; badge
   visible after mode flip.
9. Long-press orb (regression check on the relabel): subtitle reads
   "HOLD FOR MODES"; mode sheet still opens; nothing else regresses.
10. Reboot Tab5; daily total persists across reboots (NVS).
11. Heavy 5-turn Cloud burst; badge tracks within ±1¢ of summed
    receipts in `/voice`.
12. Mode-flip during streaming reply; badge attribution still goes
    to the mode the turn was *initiated* in.

### Wave 2 — Error surfacing
- Wire `dictation_postprocessing_error` handler → toast.
- Wire `config_update.error` for STT/TTS-fallback → persistent banner.
- `agent_error` handler with `recovery` field → auto-fallback toast.
- ≥10 stories: induce each error class, assert toast/banner appears,
  assert it dismisses on recovery.

### Wave 3 — Dictation rescue
- Fix orb-subtitle (already in W1) **+** add dictation entry from chat
  overlay; stop button in voice overlay; SD-card fallback for
  WS-down dictation.
- ≥12 stories: long-form dictation, mid-stream WS drop, tap-to-stop,
  silence-auto-stop, save-as-Note, save-into-chat, Notes-revisit-edit,
  multi-language attempt (expect graceful), stress.

### Wave 4 — Cloud picker + capability sync
- Settings dropdown for Cloud LLM (Haiku / Sonnet / GPT-4o / Gemini).
- Tab5 listens for `vision_capability` in `config_update` ACK + on
  every model swap (currently only fires on initial connect).
- ≥10 stories: pick each model, send vision turn, verify routing,
  verify camera-chip light-up correctness.

### Wave 5 — Hybrid story
- Mode-sheet subtitles with latency + privacy stamp.
- Onboarding reorder: Hybrid as "recommended."
- Per-mode spend breakdown (extends Wave 1's NVS keys).
- ≥10 stories: walk a fresh user through onboarding; observe defaults;
  exercise Hybrid + photo (should auto-bump or block clearly).

### Wave 6 — Agent surface
- New `ui_skills.c` screen in nav sheet.
- Dragon endpoint `GET /api/v1/skills` + `tinkerclaw/agent_log`.
- Live tool-progress toast for skill executions >2s.
- ≥12 stories: discover skills, enable/disable, run a skill,
  observe progress, observe cost contribution to spend totals.

### Wave 7 — Onboard polish
- Mic-mute guard in `voice_onboard_chain_start`.
- Health chip next to Settings → Onboard radio.
- Failover bubble visual differentiation ("K144" badge variant).
- ≥10 stories: cold-start probe, warmup-fail, mic-mute-then-chain
  (must refuse), failover engage + recover, multi-turn drift.

## Self-check
- Each wave produces a working device + green tests before the next
  wave starts.  No "land all 7 then fix."
- Wave 1 touches the chat header and home pill — both already
  exercised by `touch_stress.py`'s phases 4/8/10/12, so we'll know
  fast if we regressed.
- "User stories" are real touch-driven scenarios, not API smoke.
  Each story produces a screenshot timeline you can hand to the user
  to demo what changed.
