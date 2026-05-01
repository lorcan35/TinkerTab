---
title: Changelog
sidebar_label: Changelog
---

# Changelog

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Top-line per Wave / Phase. Detailed history is in the commit log on `main` for both repos.

## TinkerTab firmware

### May 2026 (Waves 11-19)

- **Wave 19** — OPUS encoder uplink unblocked. Stack overflow root-caused via `/codec/opus_test` synthetic endpoint bisect: 24 KB watermark. MIC_TASK_STACK_SIZE 16→32 KB (PSRAM-backed). `esp_audio_codec` 2.4.1→2.5.0. Closes #264, #262.
- **Wave 18** — Widget icon library. 16 path-step DSL glyphs (clock/briefcase/laundry/coffee/book/car/pot/person/droplet/check/alert/sun/moon/cloud/calendar/star), rendered via lv_canvas + ARGB8888 PSRAM buffer, tone-driven color (calm→emerald / active→amber / urgent→rose). Closes #69.
- **Wave 17** — Camera lifecycle hybrid. `ui_camera_destroy` → `lv_obj_clean(scr_camera)` instead of full delete; `alloc_canvas_buffer` idempotent; `canvas_buf` stays resident. Live-verified: 397 s mixed-screen stress with no reboots (pre-fix: reboot every 90-120 s). Closes #247.
- **Wave 16** — K144 polish. Settings re-renders chip + gauge + inventory live on every state transition. Auto-retry banner clears on recovery via `mark_k144_recovered()`.
- **Wave 15** — K144 model registry. `voice_m5_llm_sys_lsmode()` + `GET /m5/models` (5 min cache). Settings shows compact bucket inventory.
- **Wave 14** — K144 observable. Typed `voice_m5_llm_sys_hwinfo()` + `voice_m5_llm_sys_version()` wrappers. `GET /m5` enriched with hwinfo block. `POST /m5/refresh` forces fresh fetch.
- **Wave 13** — K144 recoverable. `voice_m5_llm_sys_reset()` + `voice_onboard_reset_failover()` + `POST /m5/reset` + `esp_timer` 60s auto-retry (capped at 3 attempts/boot). Live timing: 9.6s reset round-trip.
- **Wave 12** — Cross-session agent activity feed. `ui_agents` fetches Dragon's `/api/v1/agent_log` on every overlay show.
- **Wave 11** — Skill starring. NVS `star_skills` (comma-separated tool names). Pinned tools sort to top with amber tint + "PINNED" caption.

### April 2026 (TT #327 + TT #328 phases)

- **TT #327** — K144 chain hardening (7 waves). UART recursive mutex; `voice_onboard.{c,h}` extracted from `voice.c`; per-utterance TTS workaround; `GET /m5` debug endpoint; `m5.warmup` / `m5.chain` obs events.
- **TT #328 (UI/UX hardening, Waves 1-9)** — A11y contrast, mode-array drift, mic-button leak, atomic touch injection, toast tones + 4 new `error.*` obs classes, per-state voice icons, orb safe long-press + undo, `ui_tap_gate` debounce, chat-header touch-target lift, `widget_mode_dot` extract, nav-sheet 3×3, dead-API removal, onboarding Wi-Fi step, dual mode-control collapse, discoverability chevron + first-launch hint.

### Earlier highlights

- Five-tier voice mode (Local / Hybrid / Cloud / TinkerClaw / Onboard K144)
- Two-way video calling (VID0 + AUD0 binary framing, Dragon as broadcast relay)
- Rich media chat (code blocks + tables + image URLs rendered as JPEG inline bubbles)
- Dragon multi-model router with capability-aware per-turn LLM selection
- Spring animation engine + 3 wirings (toast / orb / mode-dot)
- LVGL pool OOM crash fixed (PR #183: `lv_mem_add_pool` 2 MB PSRAM expansion at boot)
- Camera v4·D + photo-to-chat upload flow
- Skill platform v1 + Time Sense reference

## Dragon server

### April-May 2026

- **Multi-model router (#183-#188)** — Capability-aware per-turn model selection. Six modalities (TEXT/VISION/VIDEO/AUDIO_IN/AUDIO_OUT/TOOL_CALLING). Per-backend caps. Tier-based fleet config. `fleet_summary` in WS protocol.
- **`server.py` decomposition (#65)** — 2,747 LOC monolith → four sibling packages: `middleware/`, `handlers/`, `lifecycle/`, slimmed `server.py`.
- **Cross-modal continuity** — `_handle_user_media` re-threaded through ConversationEngine; vision turns persist into MessageStore as multimodal content arrays; follow-up text turns re-hydrate the photo from MediaStore.
- **Local LLM gauntlet** — 11-model benchmark; default switched from qwen3:0.6b → ministral-3:3b. 7/10 correct-tool fires on the 10-prompt gauntlet. (See `docs/historical/AUDIT-WAVE-14.md`.)
- **WS keepalive during inference (#76)** — fires `ws.ping()` every 5 s during slow Local-mode turns. Closes the eviction race that emptied replies on every 60-90s turn.
- **Empty-reply guard (#77)** — `tools/response_wrap.py` synthesises a one-line ack from tool results when FC-trained models emit a tool call and stop.
- **Tolerant tool parser (#74, #79, #82)** — three accepted dialects (legacy, standard FC, bracketed-name). Tolerates xLAM bracket quirks, missing `</args>`, stray `>` after markers.
- **Rich Media Chat** — MediaPipeline (Pygments + Pillow) renders code/tables/images as JPEG; MediaStore with 24h cleanup; URL signer for unauthenticated rendering paths.
- **Scheduler (Phase 5)** — In-process scheduler + sqlite-backed notification store. Survives Dragon reboots. RUNAWAY_CAP_PER_DEVICE = 100.
- **TinkerClaw integration (#155)** — `vmode=3` routes LLM through TinkerClaw Gateway agent runner (localhost only). STT + TTS still local. CDP browser streaming retired.
- **UX-gap remediation (UX-GAPS.md, Phases 1-6)** — 21 verified gaps closed across 6 phases.

### Foundation milestones

- **Sessions != connections** — sessions survive disconnects; reconnect resumes
- **API-first architecture** — every capability accessible via REST
- **Memory + RAG** — facts + documents + Ollama nomic-embed-text (768-dim) + sqlite-vec
- **11-tab dashboard SPA**
- **Notes** — sessions tagged `type='recording'`, not a parallel system
- **Cloud mode + auto-fallback** — OpenRouter STT/TTS with local fallback on cloud failure
- **Dictation post-processing** — auto-titled / summarised long captures
- **OTA infra** — `/api/ota/check` + `/api/ota/firmware.bin` with SHA256 verification

## Watching for changes

- **Both repos** — squash-merge to main with conventional-commit messages: `git log --oneline main`
- **TinkerTab plans** — `~/projects/TinkerTab/docs/PLAN-*.md` always reflects the latest in-flight roadmap
- **TinkerBox audits** — `~/projects/TinkerBox/docs/AUDIT-WAVE-*.md` and `WAVE-*-PROGRESS.md` track active wave work

For raw history: `git log --oneline --no-merges` in either repo.
