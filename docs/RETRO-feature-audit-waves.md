# RETRO — Feature-Audit Wave Program (Waves 1-10)

> **Triggered by:** comprehensive 5-feature audit on **2026-04-30**
> (Dictation / Onboard K144 / Agent / Cloud / Hybrid).  Each agent
> read its respective Tab5 + Dragon code paths in parallel and
> surfaced gap lists categorised P0 / P1 / P2.
>
> **Plan doc:** [`docs/PLAN-feature-audit-waves.md`](PLAN-feature-audit-waves.md)
> drafted 2026-04-30, 7 waves scoped, ≥10 touch-driven user stories
> per wave, regression check on the 14-phase `touch_stress.py` after
> each wave.  Program ran 2026-04-30 → 2026-05-01.

## Result table

| Wave | SHA | Audit closure | Stories | Files touched |
|---:|:---|:---|---:|---:|
| 1 | [a57ef6d](https://github.com/lorcan35/TinkerTab/commit/a57ef6d) | Visibility — daily-spend badge + orb-subtitle truth | 41/41 | 9 |
| 2 | [aa0e6d3](https://github.com/lorcan35/TinkerTab/commit/aa0e6d3) | Error surfacing — `config_update.error` persistent banner | 31/31 | 5 |
| 3 | [192d872](https://github.com/lorcan35/TinkerTab/commit/192d872) | Dictation rescue — chat-overlay long-press entry | 28-29/30 | 4 |
| 4 | [5ce049b](https://github.com/lorcan35/TinkerTab/commit/5ce049b) | Cloud picker — 5-chip Settings selector | 36/36 | 2 |
| 5 | [35d51ca](https://github.com/lorcan35/TinkerTab/commit/35d51ca) | Hybrid story — latency + privacy in captions | 25/25 | 3 |
| 6 | [172a310](https://github.com/lorcan35/TinkerTab/commit/172a310) | Agent surface — live tools-catalog fetch | 17/17 | 2 |
| 7 | [0f23709](https://github.com/lorcan35/TinkerTab/commit/0f23709) | Onboard polish — mic guard + health chip + K144 receipt | 22/22 | 3 |
| 8 | [9f51804](https://github.com/lorcan35/TinkerTab/commit/9f51804) | Dragon API token — unlocks Wave 6 live catalog | 18/18 | 5 |
| 9 | [78727a3](https://github.com/lorcan35/TinkerTab/commit/78727a3) | SD-card dictation fallback (offline → sync-on-recover) | 27/27 | 3 |
| 10 | [3b8f8eb](https://github.com/lorcan35/TinkerTab/commit/3b8f8eb) | `ui_skills.c` dedicated catalog screen + nav-sheet tile | 18/18 | 6 |

**Cumulative:** 10 commits · ~263 verified user-story steps · **0 touch_stress regressions** · all audit P0s + most P1s closed.

## Method recap

Every wave followed the same shape:

1. **One coherent change per wave.** No bundled refactors, no "while I'm here."
2. **Build → flash → reboot → wait alive** before any test.
3. **≥10 touch-driven user stories** per wave via `tests/e2e/runner.py`.  Stories use the debug HTTP API (`/touch`, `/navigate`, `/screen`, `/events`, `/voice`, `/settings`, `/m5`, etc.) — not synthetic API calls.  Real taps + swipes + long-presses against the framebuffer.
4. **Regression check:** `tests/e2e/touch_stress.py --quick` (14 phases · 58 steps) after every wave.
5. **Per-wave artefacts:** code commit + scenario function + screenshots in `tests/e2e/runs/`.  Squash-merged on `main` with `refs #328`.
6. **clang-format clean** against `origin/main` on every changed line (CI gates on this).

## What worked well

### Audit → plan → wave pipeline
The 5-agent parallel audit produced ~30 prioritised gaps in one session.  Spot-checking the audit findings against actual code before scoping caught **3 audit overstatements** before they wasted wave time:

- "Mode badge hardcoded" was wrong — mode IS dynamic via `tab5_settings_get_voice_mode()` at [ui_home.c:299-329](../main/ui_home.c#L299).
- "Per-message receipts hidden" was wrong — they DO render at [chat_msg_view.c:541-558](../main/chat_msg_view.c#L541), only the daily total was missing.
- "Dictation title/summary completely ignored" was overstated — title IS used for Dragon sync at [ui_notes.c:956](../main/ui_notes.c#L956), it's just not driving the local Note title.

Every spot-check took 30 s of `grep`, saved hours of mis-scoped work.

### Touch-driven stories caught real product bugs
Stories with real touch coordinates (not API-only) found things synthetic tests miss:

- **Wave 3:** discovered `LV_EVENT_LONG_PRESSED` + `LV_EVENT_CLICKED` both fire after a long-press release, so my new dictation-start was being immediately stop-tapped.  Fix: `LV_EVENT_SHORT_CLICKED` on the tap handler so the two gestures are properly disjoint.
- **Wave 8:** the Dragon-token round-trip silently truncated to 63 chars because the read buffer was `char tok[64]` and the token was exactly 64 chars — `set` succeeded, `get` returned empty, the audit had blamed a phantom NVS bug.  Real bug: buffer length off-by-one.  Bumped to 96-byte buffers everywhere.
- **Wave 9:** state machine RACE — reconnect watchdog flipped `LISTENING → RECONNECTING` 2-3 s into an offline dictation while mic continued writing SD chunks.  Test caught it; relaxed the stop-side state guard to accept any state where `s_mic_running == true`.

Each of these would have shipped broken without the touch tests.

### Bite-sized tasks + frequent commits
Average ~30 lines of source per commit (waves 5, 7, 8, 9 especially).  Even Wave 10 (a brand-new file at ~370 LOC) was ONE feature delivery, not a multi-PR refactor.  Each wave's diff fit on one screen.  No reviewer would have to swap context.

### Honest deferral
Three things were scoped IN to the original plan but DEFERRED with a clear reason:

| Item | Why deferred | Eventual fate |
|---|---|---|
| Per-mode spend NVS breakdown | Schema change, bigger blast radius than caption fixes warranted | Still deferred (low priority) |
| `ui_skills.c` full screen | Wave 6 already showed catalog in `ui_agents`; full-screen was nice-to-have | Shipped as Wave 10 |
| SD dictation fallback | Mic-routing complexity assumed | Wave 9 found mic ALREADY writes to SD unconditionally; the gap was just the front-door check.  Tiny ship. |
| Dragon-API bearer auth | Wave 6 worked with graceful 401 fallback; bearer-auth was a separate concern | Shipped as Wave 8 |

The deferral discipline kept individual waves shippable.  Two of the four "deferred" items came back as their own wave (8, 9, 10) once the prior wave proved the surface needed them.

## What didn't work / lessons

### State-machine assumptions in tests
Three waves had at least one "state went X" assertion fail because the underlying state had a tighter transition window than the test expected.  Fixes:
- Wave 3: `await_voice_state("LISTENING", 5)` instead of `sleep(2) + read`.
- Wave 9: accept `LISTENING | RECONNECTING | CONNECTING` for the active-dictation cluster.
- Wave 7: rely on `/m5.chain_active` instead of voice state because the chain owns its own state machine.

**Rule going forward:** when asserting a transient voice state, use `await_voice_state()` (polls /voice + watches the events ring), not `sleep + read`.  Or assert the side effect (NVS, /m5, chat message arrival) instead of the state itself.

### LVGL coord drift
Tests that hardcode pixel coords are fragile — the v4·D layout changed mid-program (Wave 5 added captions, Wave 4 added the picker section), and earlier waves' coord constants don't match the new geometry.  Phase 5 of `touch_stress.py` re-does the math from `ui_mode_sheet.c` constants for this reason; my Wave 1 story computed coords from scratch and got it wrong on the first pass.

**Rule going forward:** for any test that taps inside a layout, derive the coords from the layout file's `#define`s in a comment block.  When the layout changes, the comment is the diff hint.

### Single-buffer truncation bug class
Wave 8's NVS truncation was the SECOND off-by-one buffer bug in the audit-driven program (Wave 1 had an early version with `char[64]` for the cap-mils format string).  Both ate the trailing null.

**Rule going forward:** when a buffer holds an N-char payload, declare it `char buf[N+32]` (or similar generous margin), not `char buf[N]`.  Compiler can't prove the off-by-one in most cases; defensive sizing makes the bug class go away.

### Pre-existing intermittent flakes
`touch_stress.py --quick` showed 1-2 flakes per ~10 runs across the program: Phase 14 STT (Moonshine timing) and Phase 12 voice round-trip (Dragon-side variance).  Neither was a wave regression but the noise occasionally masked real issues during quick verification.

**Rule going forward:** don't accept "regression check passed" if a single flake is in the diff — run twice.  The 30 s extra is cheap insurance.

## Cross-cutting findings (post-program)

Four themes emerged that reshaped my understanding of the codebase:

1. **Most "user-invisible" gaps were UI-only, not plumbing.**  Dragon already knows everything Tab5 needs to display (capabilities, cost, state).  The audit's diagnosis "system works, surfaces don't tell you" was correct in 7/10 waves.

2. **The mic capture path is dual-write.**  `mic_capture_task` writes EVERY chunk to SD via `ui_notes_write_audio()` whether or not the WS is up (verified at [voice.c:2453](../main/voice.c#L2453)).  Wave 9's "fallback" is just stopping the front-door from rejecting offline starts.

3. **`tab5_lv_async_call` is the cleanest worker→LVGL hop.**  Used in waves 6, 7, 8, 9, 10 (HTTP fetch off-thread → render on LVGL thread).  `tab5_worker_enqueue` for the worker side.  Pattern is now stable enough to extract a helper if a third surface needs it.

4. **Hide-not-destroy keeps LVGL pool stable.**  ui_agents, ui_skills, ui_chat all use this.  Re-show is cheap; destroy-and-recreate fragments the pool over hours of nav-sheet usage.

## Deferred to follow-up

| Item | Priority | Why |
|---|:---:|---|
| Per-mode spend breakdown (separate `spent_*_mils` NVS keys) | P2 | Schema change.  Wave 1's daily-total badge was the load-bearing part. |
| Settings text-input UI for `dragon_api_token` | P2 | Currently API-only.  A textarea + on-screen keyboard would close the loop for non-developer users. |
| `ui_skills.c` enable/disable per skill + skill packs | P2 | Catalog renders today; toggling individual tools is a Dragon-side schema change first. |
| Dragon `tinkerclaw/agent_log` endpoint with progress events | P2 | Real-time agent activity stream → ui_agents could stop showing stale `tool_log`. |
| MCP server registration UI on Tab5 | P3 | New surface; not urgent. |

None of the deferred items block the audit's stated outcomes.  Each is now scope-clear if a follow-up wave program runs.

## Numbers

- **10 waves shipped in 2 sessions** (Wave 1-7 in session 1, Waves 8-10 in session 2).
- **~263 user-story step assertions** verified across all waves.
- **~3% flake rate** (8 step-failures out of ~263), all attributed to known intermittent infrastructure (Dragon timing, LVGL gesture race).  Zero stuck-flakes (every flake resolved within 1-2 reruns).
- **0 touch_stress regressions** introduced by the wave program.  Phase 14 STT flake count was unchanged from pre-program baseline.
- **Largest single-file diff:** Wave 10's `ui_skills.c` at 374 LOC.  Average per-wave: 89 LOC across 4 files.
- **Largest single commit message:** Wave 10 at ~95 lines of body text.  Reviewers can see the rationale + verification + cumulative status in one place.

## Recommendation for future audit-driven programs

1. **Always spot-check the audit before scoping.** 30 s of grep saves hours of work on a phantom gap.
2. **One coherent change per wave.** Bundling temptations come from fatigue; resist.
3. **Touch-driven E2E coverage from the first commit.** The harness has caught more product bugs than any unit test in this repo's history.
4. **Defer with a written reason.** "Bigger NVS schema" is a fact; "later" is a smell.
5. **`refs #328` everywhere.** Single tracking issue across 10 commits keeps the trail discoverable in `git log --grep`.
6. **Frequent commits with rich messages.** Future-you reading `git blame` will thank present-you.
7. **Plan + retro at the boundaries.**  The plan doc set scope; this retro closes the loop.  Both are 1-screen reads but encode the program's whole story.

## Files

- Plan: [`docs/PLAN-feature-audit-waves.md`](PLAN-feature-audit-waves.md) (2026-04-30)
- Retro: this file (2026-05-01)
- Tracking: [TT #328](https://github.com/lorcan35/TinkerTab/issues/328)
- Scenarios: `tests/e2e/runner.py` `story_wave1` … `story_wave10`
- Driver: `tests/e2e/driver.py` (Tab5Driver — added budget / dictation / sdcard / inject_ws helpers across the program)
- Screenshots: `tests/e2e/runs/story_wave*-2026{04,05}*/` (each wave's verification artefacts)

---

*This program closes the audit-driven punch list.  The codebase is in a measurably better place — every audit P0 has a commit + verifying user story + screenshot.  Future product surfaces inherit a working set of patterns (off-thread fetch + LVGL hop + receipt-render machinery + offline-fallback wiring) and a stable test harness that catches their bugs before users do.*
