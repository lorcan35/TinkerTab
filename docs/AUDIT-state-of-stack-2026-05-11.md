# Cross-stack state-of-the-stack audit — 2026-05-11

## Wave program status — shipped to main (updated as PRs land)

| Wave | Repo | PR | What |
|---|---|---|---|
| W1 | TinkerBox | #272 | protocol.md §19 numbering + #N→#124 |
| W1 | TinkerTab | #382 | tree hygiene + this audit doc landed |
| W2a | TinkerTab | #384 | SOLO audio playback chain (24→48 kHz upsample, speaker enable, backpressure) + STT prepend for real user transcript |
| W2b | TinkerTab | #386 | re-enabled `voice.c:1172` zombie-Wi-Fi reboot escalation |
| W3-A | TinkerBox | #274 | `VoiceMode.SOLO=5` added + `is_solo()` predicate + `config_swap` SOLO branch + 11 new tests |
| W3-B | TinkerTab | #388 | Tab5 sends real vmode 4/5 in `config_update` (no more clamp-to-0) |
| W3-C-a | TinkerBox | #276 | `POST /api/v1/sessions/{id}/messages` REST endpoint + 11 handler tests + CI gate |
| W3-C-b | TinkerTab | #390 | Tab5 SOLO turns POST to Dragon canonical store via new `voice_messages_sync.{c,h}` module |
| W3-C-c | TinkerTab | #392 | K144 turns (failover + autonomous chain) POST to Dragon canonical store |
| W3-C-d | TinkerTab | #394 | SD offline queue (`/sdcard/msgsync.txt`, FAT 8.3) + drain on WS reconnect |
| W8 (1/4) | TinkerTab | #400 | SOLO chip in mode-sheet preset row |
| W8 (2/4) | TinkerTab | #402 | honest say-pill copy ("Tap to talk" / "HOLD ORB FOR MODES") + `LV_SYMBOL_AUDIO` mic glyph |
| W8 (3/4) | TinkerTab | #404 | `ui_audio_cues.{c,h}` module + mode-switch cue (80 ms 880 Hz) |
| W8 (4/4) | TinkerTab | #406 | cancel cue (60 ms 220 Hz) in `voice_cancel` + error cue (120 ms 200 Hz) in voice_solo failure toasts — **closes #405** |
| W9-A (1/N) | TinkerTab | #409 | host-targeted unit test infra (`tests/host/CMakeLists.txt`) + `test_md_strip.c` (10 assertions / 3 public functions) + `host-tests` CI workflow (7 s gate).  Plain `<assert.h>` driver, no cmocka dep.  **Closes #408** |
| W9-A (2/N) | TinkerTab | #411 | `tests/host/shim/` ESP-IDF shims (heap_caps, esp_log, esp_err) + `test_openrouter_sse.c` (9 tests / 18 assertions: overflow drop+resume, 50 KB intact, [DONE] sentinel, CRLF, comment-line drop, multi-event single feed, split-feeds, optional space).  Pins both recent SSE regressions (W3-C overflow restart, TT #379 64 KB bump).  **Closes #410** |
| W9-A (3/N) | TinkerTab | #413 | `test_spring_anim.c` (9 tests / 18 assertions) — damped harmonic oscillator math.  Pins each of the 3 regime branches (under/critical/over), the SPRING_MAX_ELAPSED_S pathological-config bail-out, the zero-mass clamp, and SNAPPY/BOUNCY/SMOOTH preset envelopes.  **Closes #412** |
| W9-B | TinkerTab | #415 | `tests/e2e/discover.py` — Tab5 host discovery chain (arg → env → mDNS espressif.local → cache → fail).  Replaces hardcoded `192.168.1.90` in `runner.py`.  New `tests/e2e/nightly.sh` cron-deployable wrapper for `story_stress --reboot`.  Live-verified: 14/14 PASS through new discovery wiring.  **Closes #414** |
| W9-C | TinkerBox | #289 | `tests/test_protocol_contract.py` — 4 tests pinning bidirectional parity between `_handle_ws_voice` cmd_type branches and `docs/protocol.md` Tab5→Dragon headings.  Tolerant of ASCII `->` + Unicode `→` arrows and `Action — \`verb\`` heading style.  Allowlist self-test rejects stale entries.  Wired into ci.yml pytest gate.  **Closes TinkerBox #288** |
| W9-D | TinkerTab | #418 | `story_resilience` e2e scenario — 13 steps exercising voice.c's WS reconnect path: `/voice/reconnect` → `ws.connect` event → READY return → session_id preservation → post-reconnect chat → LLM completion → state-machine sanity.  Live-verified: 13/13 PASS in 128 s on Tab5 192.168.1.90.  **Closes #417** |

**W7 placeholder:** TinkerBox #270 (mode 3 full surface, 7 sub-waves W7-0..G).  Decision logged 2026-05-11: Option C (full surface).

**Wave 9 fully closed** — W9-A (host-test infra), W9-B (discovery + nightly), W9-C (protocol contract), W9-D (resilience) all merged.  Further W9-A slices (solo_session_store, solo_rag) deferred — they need cJSON / NVS / SD shims; ROI bar not met yet.

| W10-A (TT) | TinkerTab | #421 | CLAUDE.md slim: extract "Current Sprint" + Waves 11-19 + Key Fixes (April 2026) → new `docs/CHANGELOG.md`.  CLAUDE.md 1055 → 992 LOC (-6 %).  **Closes #420** |
| W10-A (TB) | TinkerBox | #291 | CLAUDE.md slim: extract Local LLM Benchmarks (13-row gauntlet table + failure-class analysis + when-to-use matrix) + "Current Sprint" issues table → new `docs/CHANGELOG.md`.  CLAUDE.md 769 → 683 LOC (-11 %).  Architecture Decisions stays in runbook (durable rules).  **Closes TinkerBox #290** |
| W10-C | TinkerTab | #423 | ADR scaffold: `docs/adr/{README,0000-template,0001-host-test-infra-pattern}.md`.  CLAUDE.md cross-links the criteria.  First record captures the W9-A "plain assert + ESP-IDF shims" choice.  **Closes #422** |
| W7-A | TinkerBox | #293 | Gateway tool-call SSE forwarding.  `TinkerClawBackend.set_tool_event_handler(on_tool_call)` setter + parser accumulates `delta.tool_calls` deltas by `index` and flushes on finish_reason/EOS.  Payload shape matches Dragon ToolRegistry's existing `{"tool": ..., "args": ...}` so Tab5's Wave 12 agent_log feed renders gateway activity with zero firmware changes.  `pipeline.py` auto-wires `conn_state["on_tool_call"]` via `hasattr` guard.  11 new tests / 50 total pass.  Earns the right to W7-B/C.  **Closes TinkerBox #292** |
| W7-A.b | TinkerBox | #295 | Bridge gateway tool_calls into `/api/v1/agent_log` ring.  Pre-W7-A.b the Wave-12 chokepoint was `ToolRegistry.execute`, which mode 3 bypasses — gateway calls never reached the cross-session feed.  Now `TinkerClawBackend._flush_tool_calls` also calls `agent_log.record_call(name, args)`.  Recording fires regardless of WS handler presence.  3 new tests / 14 total pass.  **Closes TinkerBox #294** |
| W7-B | TinkerBox | #297 | `GET /api/v1/agent_skills` catalog endpoint.  Union of (a) hardcoded OpenClaw core tools (bash/browser/edit_file/memory/read_file/search_files/task/web_search) verified against upstream source 2026-05-12 + (b) tool names observed in agent_log ring.  Each entry: name/source/uses/last_ts.  Avoids the OpenClaw gateway's WS-RPC `skills.status` complexity for v1 — live-discovery is W7-B.2.  7 new tests / 21 total pass.  **Closes TinkerBox #296** |
| W7-A.2 | TinkerBox | #299 | Synthesize gateway `tool_result` on first content burst after a tool_call.  `/v1/chat/completions` doesn't emit tool_result natively, so Tab5's chat-UI tool-spinner stayed forever and `/api/v1/agent_log` left calls at status=running.  Now `_pending_results` queue + `_emit_synthetic_results` fire one tool_result per pending tool on next content delta (or end-of-stream as safety net), flipping agent_log status to done.  9 new tests / 22 total pass.  **Closes TinkerBox #298** |
| W7-B (Tab5) | TinkerTab | #429 | Tab5-side `/api/v1/agent_skills` fetch + obs-event round-trip probe.  First slice — no LVGL render yet; validates the data path on live hardware before render decisions.  Live-verified: obs ring `agent_skills count=8 observed=0` confirming Tab5 fetches Dragon's static OpenClaw core tool catalog (bash / browser / edit_file / memory / read_file / search_files / task / web_search) end-to-end.  **Closes #428** |
| W7-B.3 | TinkerTab | #432 | Render the catalog as chips in the Agents overlay.  New `render_agent_skills` builds a "MODE 3 — AGENT SKILLS" section below the tools catalog with rose `TH_MODE_CLAW` header, "N AVAILABLE" count, flex-wrap chip grid (static = dim border, observed = rose-tinted fill).  Anchored via `lv_obj_align_to(LV_ALIGN_OUT_BOTTOM_LEFT)` so it can never overlap the catalog as Dragon adds more tools.  Screenshot-verified on Tab5 192.168.1.90.  **Closes #431** |

**Wave 10 (mostly) closed** — W10-A landed on both repos, W10-C ADR scaffold landed.  W10-B (docs-site PR #329 land-or-split) and W10-D (TinkerClaw fork scope-down per W7 outcome) remain.

**Wave 7 progressing — 6 slices shipped 2026-05-12** — Mode 3 now (1) surfaces real-time gateway tool calls to Tab5, (2) flips them to "done" on completion via synthesized tool_result events, (3) records both phases into the cross-session agent_log feed, (4) exposes a skill catalog REST endpoint, (5) Tab5 firmware live-verified fetching the catalog, (6) **the catalog now visibly renders in the Agents overlay**.  Full Tab5↔Dragon mode-3 visibility loop closed visually.  Next: W7-C memory bridge or W7-B.2 dynamic skill discovery (WS-RPC).

**Remaining roadmap:** W7-C memory bridge · W7-B.2 dynamic skill discovery (WS-RPC) · W7-0 notification design → W7-D/E/F/G · W10-B docs-site decision · W10-D fork scope-down (gated on W7 outcome).

**Update this section when shipping new waves so the wave program survives session compaction.**

---

**Product naming (corrected 2026-05-11):**
- **TinkerClaw** = the umbrella product brand. The Tab5 firmware connects to TinkerClaw running on Dragon hardware.
- **TinkerTab** = the Tab5 firmware repo (the face).
- **TinkerBox** = the Dragon-server source repo (the brain). Provides `tinkerclaw-voice` + `tinkerclaw-dashboard` + `tinkerclaw-ngrok` systemd units.
- **`~/projects/TinkerClaw/`** = the OpenClaw fork source. Provides the `tinkerclaw-gateway` agent runner (port 18789). Kept as a minimal rebrand of upstream OpenClaw on purpose so upstream sync stays cheap.
- **Dragon** = the Radxa Q6A hardware running the TinkerClaw stack (all four services).

**Scope of this audit:** TinkerTab firmware + TinkerBox server + TinkerClaw agent gateway + the 5 voice modes + cross-system piping. Ten parallel agent passes (5 deep-dives + 5 dimension audits: cohesion, docs, UI/UX, debug/obs, stability).

**Verdict in one sentence.** TinkerClaw the product is mechanically excellent and structurally honest about modes 0/1/2. The agent-gateway component (vmode=3) is a first-class service that's currently underused — plumbed as a thin LLM proxy when it has OpenClaw's whole agent surface (50 skills, 134 extensions, channels, memory, browser) sitting right there on localhost. Modes 4 (K144) and 5 (SOLO) bypass the rest of the TinkerClaw stack silently. Two operational regressions to fix immediately.

---

## Two things to fix today

1. **`voice.c:1172` zombie-Wi-Fi reboot is dead code.** Gated off with `if (false && …)` on 2026-05-09 as "SOLO-mode rescue," never re-enabled. The safety net for the 30-min stress run's 9 SW resets is dark.
2. **Dirty tree on `main`.** `main/voice_solo.c` carries the audio-playback fix from the prior session (24→48 kHz upsample + speaker enable + 1024-sample backpressure). Real bug fix — needs committing on its own branch, not reverting.

---

## Top findings by dimension

### Cohesion (the elephant)

- **Dragon's `VoiceMode` enum has no SOLO=5** (`dragon_voice/voice_modes.py:46-72`). If Tab5 ever sent `voice_mode:5` in `config_update`, Dragon's `VoiceMode.from_int(5)` returns `None` → silent downgrade to LOCAL. Tab5 disciplines around this by short-circuiting the wire in `main/voice_modes.c:113-132`.
- **Mode 4 lies to Dragon by design.** Tab5 in vmode=4 sends `config_update` clamped so Dragon's STT/TTS keep working. Two codebases agree to deceive each other at the protocol layer.
- **SOLO runs a parallel session log + parallel RAG that Dragon never sees.** `main/solo_session_store.c` on SD, `main/solo_rag.c` flat-float32. Three chat-message stores, three RAGs, zero sync.
- **Agent gateway (vmode=3) is underused.** The `tinkerclaw-gateway` service IS a first-class TinkerClaw component — `~/projects/TinkerClaw/` is intentionally a thin rebrand of OpenClaw so upstream sync stays cheap. The gap is that `dragon_voice/llm/tinkerclaw_llm.py:113-501` plumbs the gateway as a glorified `/v1/chat/completions` proxy with a `user` field, so OpenClaw's 50 skills + 134 extensions + channels + memory + browser are invisible to Tab5 — only LLM tokens come back.
- **Four credential surfaces.** `dragon_tok` + `or_key` in Tab5 NVS, `DRAGON_API_TOKEN` + `TINKERCLAW_TOKEN` + `openrouter_api_key` on Dragon (Dragon's OR key is independent of Tab5's!).
- **Three identity models.** Tab5 MAC `device_id`, Dragon `devices.hardware_id`, OpenClaw `user` field (= Dragon `session_key`, NOT device_id).

### Stability

- **`voice.c:1172` `false &&` gate** — see above.
- **`s_solo_pcm` has no max-utterance cap** (`main/voice.c:222,581`). Stuck VAD = unbounded PSRAM growth.
- **Two SOLO hot-fixes in 24h** (W4-B UAF `0853e64`, STT shape `9336ae9`) — async lifetime modeling on SOLO is shaky.
- **`lv_async_call` discipline holds** — 0 raw callers; wrapper at `main/ui_core.c:474-480` is the only definition site.
- **Heap watchdog hardened** — new internal-SRAM exhaustion detector (`main/heap_watchdog.c:90-91`).
- **Dragon DB integrity good** — WAL + foreign_keys + WAL-checkpoint-on-close + corruption recovery (`dragon_voice/db.py:56-83`).

### Debug / Observability

- **No cross-system trace ID.** A failed turn on Tab5 → matching Dragon log line is timestamp+grep guessing. Single largest debugging unlock available.
- **TinkerTab debug surface is genuinely rich** — 18 family modules post-Wave-23b, bearer-auth, 256-entry obs ring, Prometheus exposition.
- **Dragon `/health` lies** — claims `status:"ok"` unconditionally without probing STT/LLM/TTS (`dragon_voice/handlers/status.py:73`).
- **Dashboard "Logs" tab is misleadingly named** — shows event store only, not journald (`dashboard.py:944-963`).
- **No coredump scraper on Dragon side.** Tab5 `/coredump` exists, manual `espcoredump.py` only.
- **`video_upstream.py` has counters but no admin endpoint** to surface them.

### UI/UX

- **v4·D Sovereign Halo home is genuinely beautiful.** Onboarding carousel + Dragon RTT gate + 3-dial mode sheet + violet Agent consent modal are gold-standard.
- **Say-pill copy lies.** Reads "Hold to speak / HOLD FOR MODES" but the pill is bound to click only; long-press is on the orb above. Not even hold-to-talk — tap-to-start (`main/ui_home.c:782,797-829`).
- **Mic glyph is a U+2022 bullet dot, em-dash is ASCII `--`** — font subset gaps cripple brand identity (`ui_home.c:797,262`).
- **Settings is developer-debug masquerading as user-tunable** — 2,283 LOC, K144 NPU temperature + 11-model inventory line on screen 1 of Settings.
- **8 stale `s_stat_k/v[]` labels** get text written every 2s even though hidden (`ui_home.c:1403-1421, 1481-1491`).
- **SOLO mode not discoverable** — only reachable via being-already-in-solo cycle or `/mode?m=5` debug.
- **Device is mute except for LLM speech.** No mode-switch chirp, no cancel cue, no chain-stop tone. Highest perceived-quality jump per day of work available.
- **13+ gestures, 0 in-UI cheat sheet** after onboarding.
- **No reduced-motion accessibility toggle.**

### Documentation

- **TinkerBox `docs/protocol.md` §16 missing 9 Wave-23 messages** that Dragon actively emits: `media_rendering`, `api_usage`, `dictation_warning`, `dictation_postprocessing` / `_error` / `_cancelled`, `note_created`, `device_evicted`, `cancel_ack`, `tool_call_limit_reached`, `vision_capability`, `cap_downgrade`, `receipt`, `progress`.
- **`protocol.md` §19 has bugs** — heading is `## 19.` but subsections are `### 18.1-18.6`; `#N` placeholder in the body line 1651.
- **Audit doc line numbers cite past EOF.** TinkerTab `docs/AUDIT-solid-2026-05-03.md` cites `voice.c:1-4251` (actual EOF 2,454); TinkerBox equivalent cites `server.py:1640-2514` (actual EOF 1,472).
- **CLAUDE.md sprint sections are 486 LOC of dead weight** reloaded into every Claude context.
- **No ADRs anywhere.** Decisions live in PR descriptions + LEARNINGS scrolls.
- **Docs-site PR #329 sitting at green CI since 2026-05-01** — 29 kLOC Docusaurus site, decision needed.

### Open issues snapshot

| Repo | Open | S1 | S2 | S3 |
|---|---|---|---|---|
| TinkerTab | 3 | 0 | 1 | 2 |
| TinkerBox | 2 | 1 | 1 | 0 |
| TinkerClaw | 0 | – | – | – |

- TinkerTab `#316` Grove (hardware-blocked), `#333` ui_settings K144 cache, `#334` VID0/AUD0 dup
- TinkerBox `#201` WsDispatcher (P0), `#203` tools/registry.py split (P1)
- TinkerClaw: dormant; 5 stale dependabot PRs

---

## Wave roadmap

Each wave = one GH issue (this repo or TinkerBox), one branch, one squash-merged PR. Sub-items labeled W{N}-A/B/C/D per existing convention.

### Wave 1 — Quick wins (no GH issue; single bundled PR per repo)

**TinkerBox `docs/wave-1-protocol-v19-cleanup`:**
- W1-A: fix `protocol.md` §19 numbering (`### 18.1` → `### 19.1`) and `#N` placeholder
- W1-B: close stale PR #170 (audit doc superseded by Wave 23 ship)

**TinkerTab `chore/wave-1-tree-hygiene`:**
- W1-C: add `docs-site/` to `.gitignore`
- W1-D: delete 8 dead `s_stat_k/v[]` writes in `ui_home.c:1403-1421, 1481-1491`

**TinkerClaw (no branch):** close dependabot PRs #20-24 on the dormant fork

### Wave 2 — Stability honesty (TinkerTab)

GH issue: "Stability: re-enable zombie-Wi-Fi escalation + SOLO PSRAM cap + commit audio playback fix"

- W2-A: commit pending `voice_solo.c` audio-playback fix on its own branch `fix/solo-audio-playback` (24→48 kHz upsample + speaker enable + backpressure). Pre-merged into Wave 2 PR or separate PR per workflow preference.
- W2-B: flip `main/voice.c:1172` `if (false && …)` back to enabled; `!voice_is_connected()` guard already prevents the false-positives that triggered the 2026-05-09 gate-off.
- W2-C: cap `s_solo_pcm` at 60 s = 1.92 MB, force-stop with user-visible toast on exceed.
- W2-D: stack-watermark + heap-floor assertion in `tests/e2e/wave_acceptance.py` after `story_stress`.

### Wave 3 — Cohesion: modes 4/5 stop being silent (cross-repo)

**Single biggest cohesion fix.** Decision needed on Wave 7 (mode 3) doesn't block this.

GH issues: TinkerBox + TinkerTab paired

- W3-A: add `VoiceMode.SOLO = 5` to Dragon `voice_modes.py` with `is_dragon_managed_pipeline() == False`. ACK config_update cleanly instead of warn+downgrade.
- W3-B: Tab5 POSTs every K144 and SOLO turn (user + assistant pair) to `/api/v1/sessions/{id}/messages` via REST. Queue locally on SD if WS down; drain on reconnect. Pattern already exists from #44 offline-queue.
- W3-C: delete `solo_session_store.c` after W3-B lands (Dragon is canonical).
- W3-D: bridge `solo_rag` ↔ Dragon `memory_facts` via the same REST POST shape (or kill `solo_rag` and route SOLO through Dragon's memory tools).

### Wave 4 — Observability: cross-system trace ID + honest /health (cross-repo)

GH issue: "Add turn_id correlation across Tab5 ↔ Dragon"

- W4-A: Tab5 generates `turn_id` UUID on `{"type":"start"}` / `{"type":"text"}`; echoes back in every Dragon→Tab5 frame; included in every `tab5_debug_obs_event` detail and every Dragon `logger.info`.
- W4-B: Dragon `/health` actually probes STT/LLM/TTS with 2s timeouts; returns per-subsystem state (match Tab5 `/selftest` rigor).
- W4-C: Dragon `/api/v1/logs/tail?level=WARNING&since=…` that tails journald; wire dashboard "Logs" tab to it.
- W4-D: Dragon-side coredump scraper systemd unit polling Tab5 `/coredump`, auto-symbolicate against deployed firmware ELF.

### Wave 5 — Cost guard (TinkerBox)

GH issue: "Stand up spend tracker + daily cap with router-aware accounting"

- W5-A: `dragon_voice/billing/spend_tracker.py` keyed on session+model+date; hooks into existing `_PRICING_MILS_PER_M` in `openrouter_llm.py`.
- W5-B: `/api/v1/spend?day=YYYY-MM-DD` route; dashboard Overview tab.
- W5-C: `BUDGET_DAILY_CENTS` env var triggers Dragon→Tab5 `cap_downgrade` (currently only fires Tab5→Dragon).
- W5-D: SOLO mode parses `x-ratelimit-remaining-credits` header and POSTs receipt to a new `/api/v1/usage` endpoint (closes the SOLO cost-visibility gap from W3).

### Wave 6 — Finish Wave 23: WsDispatcher + _process_utterance (TinkerBox)

GH issue: TinkerBox #201 (already filed)

- W6-A: extract `_handle_ws_voice` 12-branch cmd_type dispatch into `ws_voice_dispatcher.py` with verb-per-file modules.
- W6-B: extract `_process_utterance` from `pipeline.py` (~440 LOC) into `pipeline/orchestrator.py`.
- W6-C: add CI LOC gate so server.py + pipeline.py can't regrow.
- W6-D: backfill `protocol.md` §16 with the 9 missing Wave-23 messages + add CI grep that fails when new `"type": "..."` literals appear without §16 entry.

### Wave 7 — Mode 3 full surface (decided 2026-05-11: Option C)

User picked **Option C (full surface)**: mode 3 becomes the flagship agent product — tool events + skill catalog + memory bridge + manual skill invocation + channel push + browser automation visibility. ~5-7 PRs over 3-4 weeks. Sub-waves below.

GH issue: "Wave 7: mode 3 = flagship agent product (gateway full surface)" — file once W7-0 design pass lands.

- **W7-0** — Design pass: Tab5 notification surface on the Sovereign Halo "now" card. What does an incoming Telegram message look like? Tone? Dismiss gesture? Reply path? This is a brainstorm + spec, NOT code. Anchor doc: `docs/PLAN-agent-mode-notification-surface.md`. Blocks W7-E.
- **W7-A** — Gateway SSE event forwarding. `dragon_voice/llm/tinkerclaw_llm.py` parses the gateway's SSE stream for `tool_call` / `tool_result` events and re-emits via Tab5's existing ToolRegistry wire shape (`{"type":"tool_call",...}` / `{"type":"tool_result",...}`). Tab5 already renders these (Wave 12 agent_log feed). **First shippable PR** — earns the right to W7-B/C.
- **W7-B** — Skill catalog browse. Gateway exposes `/skills` REST → Dragon proxies via `/api/v1/agent_skills` → Tab5 fetches on mode-3 entry. Agents overlay shows two sections: "Built-in tools" (Dragon's existing) and "Agent skills" (gateway's).
- **W7-C** — Memory bridge. When mode 3 is active, `remember` tool calls hit the gateway's hybrid (sqlite-vec + FTS5) memory instead of Dragon's `memory_facts`. `recall` does read-through across both stores. Documentation: "in mode 3, your memory lives with the agent."
- **W7-D** — Manual skill invocation. Tab5 Agents overlay gets per-skill chips; tap → Dragon → `POST /api/v1/agent_skills/{name}/invoke` → gateway executes → response surfaces as a normal LLM turn.
- **W7-E** — Notification surface on Tab5. Implementation of the W7-0 design. New `main/ui_notification.{c,h}` + Sovereign Halo "now" card extension. Toast-shaped for transient; persistent banner for unread.
- **W7-F** — Channel push routing. Gateway → Dragon → Tab5 notification. Per-channel allow/deny in Settings (Telegram on, WhatsApp on, etc.). Reply path: tap notification → mic-orb opens with channel context → user dictates reply → Dragon routes back through gateway → channel.
- **W7-G** — Browser automation visibility. When gateway uses CDP, Tab5 shows a discreet "watching" indicator + receives N-second-interval screenshots via media frames. Cancel gesture stops the run.

**Sequencing.** W7-A is independent and should ship first (cheap, no Tab5 UX, immediate "mode 3 feels different" payoff). W7-B/C/D fan out from A. W7-0 must precede W7-E. W7-F and W7-G depend on W7-E.

**Risk.** This is the largest of the open waves. Recommend filing it as its own multi-PR program (like Wave 23) with progress tracked in `docs/WAVE-7-PROGRESS.md`. Don't try to land it as one PR.

### Wave 8 — UX polish (TinkerTab)

GH issue: "Wave 8 UX: fonts, audio cues, Settings split, mode discoverability"

- W8-A: subset a real mic glyph + U+2014 em-dash into Montserrat (`lv_font_conv`). Fixes `ui_home.c:797,262`.
- W8-B: ship audio chirp set (mode-switch 40 ms 880 Hz / cancel 60 ms 220 Hz / chain-start / chain-stop / error). New `main/ui_audio_cues.{c,h}`.
- W8-C: split Settings → Settings + Diagnostics. K144 panel + NPU gauge + model inventory + NVS erase behind "Show diagnostics" toggle.
- W8-D: fix say-pill copy ↔ gesture mismatch — either implement true hold-to-talk (`LV_EVENT_PRESSED`/`RELEASED`) or rewrite labels to "Tap the orb to talk / Hold orb for modes".
- W8-E: add SOLO chip to mode-sheet preset row.
- W8-F: reduced-motion NVS toggle (`motion_off`) + Settings switch.

### Wave 9 — Test infrastructure (cross-repo)

GH issue: "Build host-targeted unit tests for Tab5 pure modules + nightly soak"

- W9-A: CMocka host CMake target for `md_strip`, `voice_billing`, `solo_rag`, `solo_session_store`, `openrouter_sse`. Wire into CI alongside the build job.
- W9-B: nightly cron `story_stress --reboot` against current LAN IP. Fix the hardcoded IP in `tests/e2e/driver.py` first.
- W9-C: contract test that pins `protocol.md` headings against `_handle_ws_voice` dispatch table.
- W9-D: Dragon-restart resilience scenario in e2e suite.

### Wave 10 — Documentation + housekeeping (cross-repo)

GH issue: "Doc strategy: split CLAUDE.md, decide on PR #329, kill TinkerClaw fork"

- W10-A: split TinkerTab + TinkerBox CLAUDE.md → RUNBOOK.md (loaded into context) + CHANGELOG.md / ARCHITECTURE.md (reference, archived). 486 LOC of sprint history out of conversation context.
- W10-B: land or split docs-site PR #329 per user decision.
- W10-C: adopt ADRs in `docs/adr/NNNN-slug.md` (template: context / decision / consequences).
- W10-D: kill or scope-down TinkerClaw fork per Wave 7 outcome.

---

## Mode 3-5 options (open decision — needed before Wave 7)

**Corrected framing.** TinkerClaw is the umbrella product. vmode=3 = "route the LLM call to the `tinkerclaw-gateway` service" — a first-class TinkerClaw component running on Dragon alongside `tinkerclaw-voice`. The `~/projects/TinkerClaw/` repo is a deliberately minimal rebrand of upstream OpenClaw so upstream sync stays cheap (not vaporware — intentional architecture).

The question is **how much of the agent gateway's existing surface should mode 3 plumb through to Tab5?**

### Option A — Light up the gateway: agent activity becomes visible

- Mode 3 = vmode=3 today + the agent gateway's `tool_call` activity surfaced to Tab5
- Dragon's `tinkerclaw_llm.py` parses the gateway's SSE stream for `tool_call` / `tool_result` events and re-emits as Tab5 WS frames (same shape Dragon's own ToolRegistry uses)
- OpenClaw skills appear in Tab5 `agent_log` + the Agents overlay
- ~1-2 PRs
- **No fork changes. No channel integration. Just makes mode 3 feel different from mode 2.**

### Option B — Bidirectional gateway: skills + memory bridge

- Option A plus:
- Tab5's `Agents` overlay can browse the gateway's skill catalog (gateway exposes `/skills` REST → Dragon proxies → Tab5 fetches)
- Memory bridge: when Tab5 stores a fact in mode 3, it lands in the gateway's hybrid (sqlite-vec + FTS5) memory AND Dragon's memory_facts (or just the gateway's, with Dragon doing read-through)
- Manual skill invocation chip from Tab5 → Dragon → gateway
- ~3-4 PRs over 2 weeks
- **Mode 3 becomes the agent mode that surfaces the gateway's real product.**

### Option C — Full gateway surface: channels + browser + bidirectional

- Option B plus:
- Channels: incoming Telegram/WhatsApp messages routed through the gateway can push to Tab5 as notifications (requires a Tab5 notification surface — Sovereign Halo "now" card is the natural host)
- Browser automation: when the gateway uses CDP-driven browser, Tab5 can show a "watching" indicator + receive screenshots / decisions
- Tab5 ↔ gateway becomes a 2-way agent channel, not just LLM tokens
- ~5-7 PRs over 3-4 weeks
- **Mode 3 becomes a flagship feature. Significant new Tab5 UX work.**

### Option D — Mode 3 = Skills hub (alternate framing)

- Mode 3 = mode 0 (local LLM) + the Agents overlay always-on + chip palette for manual skill invocation
- Doesn't use the agent gateway as LLM backend at all
- Skill catalog comes from Dragon's own `/api/v1/tools` (not the gateway)
- ~2 PRs
- **Reframes "agent mode" as user-driven Tab5-side discovery, not LLM-driven autonomy. Different product direction.**

### Option E — Kill mode 3

- Retire vmode=3 entirely (collapse to 4 modes: Local / Hybrid / Cloud / Onboard / Solo, renumbered)
- ~1 PR, mostly deletions
- **The minimalist case if the gateway isn't worth integrating with.**

### Recommendation

**Option A is the right Wave 7.** It's the smallest cohesive change that makes the gateway's existence visible to the user — mode 3 stops being "cloud mode but slower" and starts being "agent mode with visible tool activity." It doesn't touch the fork, doesn't add Tab5 surfaces, doesn't commit to channels. It earns the right to do Option B or C later if the activity surfacing proves the gateway's worth.

Option B is the natural follow-up wave (W11 or later) once Option A ships and you've used it. Option C is the v2 product direction once Tab5 has a notification surface. Option D is a different conversation about what "agent mode" means for the brand. Option E is on the table only if A's experiment shows the gateway adds nothing.

---

## Files referenced
- `main/voice.c:222,581,1172` — `s_solo_pcm` declaration, alloc site, disabled reboot gate
- `main/voice_solo.c` — pending audio-playback fix (uncommitted on `main`)
- `main/voice_modes.c:63-136` — 5-tier route decision
- `main/ui_home.c:782,797,262,1403-1421,1481-1491` — say-pill copy bug, mic glyph, em-dash, dead labels
- `main/ui_core.c:474-480` — `tab5_lv_async_call` wrapper (sole call site to LVGL primitive)
- `main/heap_watchdog.c:90-91` — new internal-SRAM exhaustion detector
- `dragon_voice/voice_modes.py:46-72` — `VoiceMode` enum missing SOLO=5
- `dragon_voice/server.py:520` — `_handle_ws_voice` dispatcher target of #201
- `dragon_voice/pipeline.py:776` — `_process_utterance` extraction target
- `dragon_voice/cap_downgrade.py:43` — only fires Tab5→Dragon
- `dragon_voice/llm/tinkerclaw_llm.py:113-501` — real adapter (lives in TinkerBox, not the fork)
- `dragon_voice/handlers/status.py:73` — lying `/health`
- `dragon_voice/video_upstream.py:121-141` — `_SessionVideoStats` not surfaced
- `dragon_voice/db.py:56-83` — solid DB integrity
- `docs/protocol.md:1310-1345,1651-1709` — §16 gaps, §19 numbering bug, `#N` placeholder
- `~/projects/TinkerClaw/` — fork (3 rebrand fixups over upstream OpenClaw)

## Provenance
Findings synthesized from 10 parallel general-purpose agent runs on 2026-05-11: TinkerTab firmware deep audit, TinkerBox Dragon audit, TinkerClaw + OpenClaw audit, cross-system piping audit, open issues sweep, cohesion audit, documentation audit, UI/UX audit, debug + observability audit, stability audit. Raw findings live in conversation history of session continued 2026-05-11.
