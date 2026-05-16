---
title: Wave program log
sidebar_label: Wave program log
---

# Wave program log

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Multi-week features ship as numbered "waves". Each wave is a tracked plan doc + a single PR that closes a coherent slice of audit findings. The wave log is the curated "what shipped" page; raw history lives in [the commit log on `main`](https://github.com/lorcan35/TinkerTab/commits/main).

This page summarises by-wave outcomes for the active programs. The original investigation docs (audit reports, plan docs with file:line refs) stay in the repo at [`docs/`](https://github.com/lorcan35/TinkerTab/tree/main/docs/) and aren't re-flowed here.

## TT #328 — UI/UX hardening (Waves 1-9, closed 2026-04-30)

Closed 14 of 16 audit P0s on `feat/k144-phase6a-baud-switch`:

- A11y contrast pass — every text element clears WCAG-AA against its background
- Mode-array drift — the five voice-mode tiers now share a single source-of-truth array
- Mic-button leak — the temporary "press the mic" hint that lingered is gone
- Atomic touch injection — the 5 disjoint hold/release injection sites unified into one helper
- Toast tones + persistent error banner + 4 new `error.*` obs classes
- Per-state voice icons — listening/processing/speaking get distinct glyphs
- Orb safe long-press + undo
- Universal `ui_tap_gate` debounce
- Chat-header touch-target lift
- Shared `widget_mode_dot` extract
- Nav-sheet 3×3 (Focus tile P0 #4)
- Dead-API removal
- Onboarding Wi-Fi step
- Dual mode-control collapse (orb long-press → mode-sheet)
- Discoverability chevron + first-launch hint

Two P0s deferred as larger scope: K144 as 5th tier in the 3-dial sheet, and orb-overload across 4 surfaces.

## TT #327 — K144 chain hardening (7 waves, closed 2026-04-29)

Three parallel audits (UI/UX, piping, architecture) of the K144 voice-assistant-chain integration surfaced 5 P0s + ~12 P1s. Closed via 7 wave PRs on PR #326. Notable artifacts:

- `bsp/tab5/uart_port_c` recursive mutex serialises every K144 UART transaction
- `voice_onboard.{c,h}` extracted from `voice.c` — owns the entire vmode=4 surface
- Per-utterance TTS workaround (chained `tts.setup` crashes mid-stream)
- `GET /m5` debug endpoint — closes the diagnostic gap where "chain didn't start" required serial+adb
- `m5.warmup`, `m5.chain` obs events feed the `/events` ring + e2e harness
- `tests/e2e/scenarios/runner.py` `story_onboard` (14 steps)

## TT #328 K144 recovery + observability (Waves 11-19, May 2026)

| Wave | Theme | Highlights |
|------|-------|-----------|
| 11 | Skill starring | NVS `star_skills` (comma-separated tool names); pinned tools sort to top with amber tint |
| 12 | Cross-session agent activity feed | Dragon `/api/v1/agent_log` ring buffer + Tab5 `ui_agents` fetches it on every overlay show |
| 13 | K144 recoverable | `voice_m5_llm_sys_reset()` + `voice_onboard_reset_failover()` + `POST /m5/reset` + 60s auto-retry (esp_timer-based, capped at 3 attempts/boot) + tap-to-recover on the K144 health chip |
| 14 | K144 observable | `voice_m5_llm_sys_hwinfo()` + `voice_m5_llm_sys_version()` typed wrappers; `GET /m5` enriched with hwinfo block; `POST /m5/refresh` |
| 15 | K144 model registry | `voice_m5_llm_sys_lsmode()` + `GET /m5/models` (5 min cache); Settings shows compact bucket inventory |
| 16 | K144 polish | Settings re-renders chip + gauge + inventory live on every state transition; auto-retry banner clears on recovery |
| 17 | Camera lifecycle | `lv_obj_clean(scr_camera)` + idempotent `alloc_canvas_buffer` (closes #247) |
| 18 | Widget icon library | 16 path-step DSL glyphs, lv_canvas + ARGB8888 PSRAM buffer, tone-driven color (closes #69) |
| 19 | OPUS encoder | Stack overflow root-caused via `/codec/opus_test` bisect: 24 KB watermark → bumped MIC_TASK_STACK_SIZE 16→32 KB; `esp_audio_codec` 2.4.1→2.5.0 (closes #264, #262) |

## Recurring lessons

Three patterns kept resurfacing across waves:

- **BSS-static causes boot loop** — Waves 11/13/15 all hit this with different size bands (3.6 KB, 16 KB FreeRTOS timer task, 1.8 KB). Default to PSRAM-lazy. See the [stability guide](/docs/firmware-reference/stability-guide).
- **One bump isn't a bisect** — Wave 19 OPUS originally diagnosed as "alignment / SIMD" because PR #263 bumped voice_mic stack 8→16 KB, saw it still crash, and concluded "not stack overflow." The actual watermark was 24 KB. Always bisect.
- **MCAUSE 0x1b is specifically the stack canary** — not "alignment", not "SIMD", not "TLSP cleanup". When you see this, bisect the stack size; don't theorise upstream.

## Open candidates (Wave 20+)

- **KWS revival on K144** — sherpa-onnx-kws-zipformer-gigaspeech is open-vocabulary (no custom training needed for "Hey Tinker"). Resurrects the wake-word feature TT #162 retired by sidestepping the original ESP32-P4 TDM blocker.
