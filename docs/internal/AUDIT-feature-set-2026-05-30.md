# TinkerClaw Stack — Feature-Set Audit (2026-05-30)

> Scope: TinkerTab (Tab5/ESP32-P4 firmware) + TinkerBox (Dragon Q6A/aiohttp brain) + TinkerClaw (OpenClaw-fork gateway). Product thesis: **privacy-first, local-first** voice assistant; Tab5 is a thin client, Dragon is the brain; six voice tiers (0 Local / 1 Hybrid / 2 Cloud / 3 TinkerClaw / 4 Onboard-K144 / 5 Solo-OpenRouter).
> Method: grounded in source (`file:line`), `docs/`, and recent git. CLAUDE.md/MEMORY.md treated as a map, not truth — drift is flagged throughout. Severity reflects a privacy-first product: anything that breaks the core promise or is a security/safety hole is escalated above feature breadth.

---

## 1. Executive Summary

**Headline:** TinkerClaw is a genuinely differentiated privacy-first/local-first voice assistant with real systems depth — identity-threaded state machines, lock-free concurrency fixes, layered self-healing reconnect, on-device native tool-calling, and an emotionally-alive orb — that no mainstream smart-display competitor matches on topology. But its defining promise is **not enforced in code, config, or policy**, and the load-bearing **safety features are fiction, not partial**.

The 8 things that matter most:

1. **The privacy-first promise is contradicted at four layers** — policy (a global "trust all inbound" instruction), default config (`wake_src=dragon` idle-streams ~256 kbps mic to a brain with no handler), the voice path (privacy-lock is a no-op on spoken turns), and data lifecycle (Solo POSTs to Dragon; gateway mirrors `remember` into a second uncontrolled PII copy). **Security was never designed in.**
2. **OTA auto-rollback is not compiled in.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set` in the built `sdkconfig`; `mark_valid` runs unconditionally and early; the documented `/ota/rollback` endpoint does not exist. **A bad field-pushed build bricks devices to USB-reflash** with no recovery for a non-developer.
3. **Secrets live in repo, chat, and serial logs** — a real debug bearer token in `CLAUDE.md:454`, the debug token printed to UART every boot, hardcoded public ngrok hosts, plain-HTTP/`ws://` data plane with `skip_cert_common_name_check`. The WS token is compile-time-only, so a 401-lockout is an unrecoverable brick.
4. **The server-side spend cap is dead.** Real `cost_mils` is computed but never persisted to `events`; `_maybe_emit_cap_downgrade` can never fire on LLM cost. The Tab5 *client* cap works, so enforcement is single- not dual-tier; **Solo bypasses caps entirely** (uncapped cloud spend on the least-supervised path).
5. **Table-stakes daily-driver features are missing or broken**: no smart-home control (the integration that makes a smart display a smart display), no timers/alarms (recurrence persisted-but-inert, snooze unwired, fires once), the scheduler never reaches the LLM, memory is invisible on the default Local path, no out-of-box wakeword, no SoftAP onboarding, multimodal unreachable.
6. **"Built-then-gated-then-forgotten" is the dominant anti-pattern with security teeth**: ~1200 LOC of `#if 0`'d video calling whose RX path still auto-pops a modal on any frame; Solo RAG/QR built-but-unwired; phantom `tasks_*` tools the model is instructed to call; ~400 LOC of dead orb-anim code.
7. **The stack is operable by its author on one device on a known LAN, by nobody else anywhere** — no Dragon discovery, no on-device token provisioning, no fleet plane, no remote OTA, no data export, no multi-user, English-only with no i18n scaffolding. This is the productization blocker.
8. **The moats are real and worth protecting**: six genuine privacy/cost tiers (incl. no-Dragon/no-account escape hatches), on-device native tool-calling (436s→29s prompt-cache win), `turn_id` identity end-to-end, lock-free concurrency discipline, multi-ladder self-healing reconnect, and a coherent hardware-reactive orb.

---

## 2. Full Feature Map by Subsystem

### 2.1 TinkerTab (Tab5 firmware)

| Feature | Status | Notes (file:line) |
|---|---|---|
| 7-state voice FSM (IDLE/CONNECTING/READY/LISTENING/PROCESSING/SPEAKING/RECONNECTING) | shipped | `voice.c:434` mutex-guarded; obs event per transition `:461` |
| Persistent mic-capture + playback-drain tasks (no per-call spawn) | shipped | `voice.c:707`, `:1032` |
| Exponential-backoff-with-jitter reconnect, 30 s cap | shipped | `voice_ws_proto.c:1736`; mirrored CLOSED `:1819`/ERROR `:1901` |
| ngrok fallback (≥4 LAN fails) + LAN-recovery swap-back | shipped | `voice_ws_proto.c:1962`, `voice.c:1434` |
| Three-tier zombie-WiFi escalation, gated `!voice_is_connected()` | shipped | `voice.c:1399` |
| Bearer-auth WS upgrade + 401 lockout (3 consecutive) | shipped (but brick risk) | `voice_ws_proto.c:1932`; token compile-time only |
| Cancel/X-button (stop mic, flush, `{"type":"cancel"}`, FSM resolve) | shipped | `voice.c:2211` |
| Dictation FSM w/ `turn_id`, self-decay, 60 s stuck-watchdog | shipped | `voice_dictation.c:36/53` |
| Lock-free `voice_dictation_state()` hot-path read (TASK_WDT fix) | shipped | `voice_dictation.c:528` |
| Offline SD fallback (dictation/long-press only) | partial | `voice.c:1884`; Ask/orb-tap refuses `:1755` |
| Six-tier `voice_modes_route_text` (text only) | shipped | `voice_modes.c:64` |
| Conversation mode (SPEAKING→READY auto-relisten) | shipped | `voice.c:521` |
| G1 single-slot queued-turn | shipped | `voice.c:485/2545` |
| Barge-in (voice-interrupt TTS) | partial/broken | `voice_onboard.c:170` branch unreachable; `voice_wakeword.c:273` suppresses SPEAKING |
| OPUS uplink encoder | gated-off | SILK NSQ crash #264; decoder ready |
| Call-audio mode (VOICE_MODE_CALL / AUD0) | gated-off | tied to disabled video calling |
| `voice_start_reconnect_watchdog`/`_stop` | deprecated stub | `voice.c:2827` no-op |
| `s_last_activity_us` response-timeout | stub (dead) | written `voice_ws_proto.c:1843`, never read |
| Dictation→note (optimistic save, reconcile-by-turn_id) | shipped | `ui_notes.c:896/961/1020` |
| Notes search (case-insensitive substring, local ≤30 ring) | shipped (limited) | `ui_notes.c:325`; MAX_NOTES=30 silent overwrite |
| Offline SD WAV engine + 15 s transcription queue | shipped | `dictation_notes.c:388` |
| WAV close+reopen every ~2 s (silent drop on reopen fail) | fragile | `dictation_notes.c:323` |
| `note_id` FSM field | stub | declared `voice_dictation.h:99`, never written |
| Orb 4-state machine + circadian + IMU + mic-RMS aliveness | shipped | `ui_orb.c:1158/306/503/1828` |
| Presence-wake dim | gated-off/stub | `ui_orb.c:1275` debug-only; real signal in `vision_service.c:242` feeds brightness |
| Mode chip on home | dead/ghost | created+clickable but "killed", `ui_home.c:886` |
| Hardcoded greeting "Emile" | shipped (wrong) | `ui_home.c:353` |
| Legacy orb-anim system (orb_breath/ripple_pool) | dead (~200 LOC) | `ui_home.c:3400/3559` paints NULL stubs |
| Chat overlay (7-widget composite, token stream) | shipped | `ui_chat.c:544/478` |
| Voice overlay (transparent over home orb) | partial (hollow shell) | orb/wave/icon tree `#if 0` `ui_voice.c:1024` |
| Settings (11 sections, K144 health, NPU gauge) | shipped | `ui_settings.c`; no `wake_src` picker `:771-808` |
| Camera viewfinder + YOLO11n/POSE/SEG overlay | shipped | `ui_camera.c:688/1156` |
| Photo capture | partial (BMP-not-JPEG) | `camera.c:341` raw BMP w/ `.jpg` name |
| Resolution dropdown | stub (no-op) | `camera.c:298` |
| Two-way video calling (uplink/downlink/AUD0/PIP/mute) | shipped-but-gated-off | `voice_video.c`; nav tile `#if 0` `ui_nav_sheet.c:77-105` |
| Camera capture buffer lifecycle | racy | `camera.c:328-338` requeue-before-consume |
| Sessions/Memory/Agents REST overlays | shipped | sessions read-only (no tap-to-load) |
| Keyboard (custom QWERTY) | shipped | `ui_keyboard.c` |
| Channel notification router (8 platforms, dedupe/snooze/quiet) | shipped | `ui_notification.c:81/193` |
| Now-card overlay (REPLY/SNOOZE/DISMISS) | shipped (no queue) | `ui_home.c:3183`; collision destroys prior `:3186` |
| Voice-dictated channel reply (30 s TTL) | shipped | `voice.c:2660`, intercept `voice_ws_proto.c:610` |
| Widget platform (6 types, store, action round-trip) | shipped | `widget_store.c:56`, `voice_widget_ws.c:37` |
| CHART widget | partial (ASCII) | `ui_home.c:1758` text histogram |
| WiFi STA retry-forever + soft/hard/reboot kick | shipped | `wifi.c:38/146/162` |
| OTA dual-slot + SHA256 (read-back verify) + schedule-reboot | shipped | `ota.c:253/400` |
| OTA auto-rollback | **broken (not compiled)** | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set`; `mark_valid` early `main.c:652` |
| OTA auto-check (hourly) | stub | `config.h:66` defined, no timer |
| OTA remote/ngrok path | missing | `ota.c:54` LAN-only |
| `/ota/rollback` endpoint | missing | documented, absent |
| Heap watchdogs (PSRAM/SRAM frag/exhaust) | shipped (DMA disabled) | `heap_watchdog.c:123` |
| SoftAP/captive-portal provisioning | missing | first boot = Kconfig defaults or reachable UI |
| Wakeword ext_pcm (K144 ASR) | shipped (opt-in, debug-HTTP) | `voice_ext_pcm_stream.c`; no Settings picker |
| Wakeword dragon (default) | gated-off/broken | no Dragon WAK0 handler; idle-streams mic |
| K144 self-healing watchdog + 4h reboot | shipped (slow) | `voice_onboard.c:642/683` |
| vmode=4 onboard autonomous chain | shipped | `voice_onboard.c:1196` |
| Solo (vmode=5) text/audio turn over OpenRouter | shipped | `voice_solo.c:234/264` |
| Solo on-device RAG | gated-off/orphan | `solo_rag.c` test-endpoint only |
| Solo QR key provisioning | stub | `qr_decoder.c:44` zero callers; toast lies |
| Solo system prompt | missing | `openrouter_client.c:381` |
| Solo spend cap | missing | bypasses `voice_send_config_update_ex` |
| Debug server family (20 modules, bearer) | shipped | token logged to UART |

### 2.2 TinkerBox (Dragon brain)

| Feature | Status | Notes (file:line) |
|---|---|---|
| `LLMBackend` ABC + Protocol mixins (`SupportsNativeTools`…) | shipped | `base.py:43/201` |
| `create_llm` factory + 7 backends | shipped | `__init__.py:12` |
| LMStudio→Ollama boot fallback (one-shot probe) | shipped | `__init__.py:74` |
| Mid-session llama-server crash recovery | missing | one-shot probe; manual `systemctl restart` |
| CapabilityAwareRouter + fleet/tier policy | shipped-but-dormant | `router.py`; `config.yaml fleet: []` |
| Native OpenAI tool-calling (13-tool fast set, cache-stable) | shipped | `conversation.py:894`, `lmstudio_llm.py:226` |
| `native_tools` on Ollama fallback | broken (no-op) | Ollama has no `generate_with_tools` |
| Async "smart tier" background agent (:1235) | partial (untested, lossy) | `async_agent.py`; drops result on disconnect `server.py:1228` |
| NPU Genie backend | shipped-but-unused | `npu_genie.py` |
| TinkerClaw gateway adapter (mode-3) | shipped | `tinkerclaw_llm.py`; no `get_last_usage` |
| STT: Moonshine/whisper.cpp/Vosk/OpenRouter | shipped (2 live) | `stt/__init__.py` |
| TTS: Piper/Kokoro/OpenRouter/Edge/NeuTTS/Supertonic/Kitten | partial (3 reachable) | `config_swap.py:124` whitelist downgrade |
| Energy VAD (RMS>900, no adaptive floor) | shipped (limited) | `pipeline.py:508` |
| Partial/streaming STT on ask path | missing | Moonshine run non-streaming |
| Sentence/clause TTS flushing | shipped | `pipeline.py:1146` |
| Cloud→local STT/TTS auto-fallback | shipped | `pipeline.py:925/1416` |
| OPUS uplink decode | gated-off | Tab5 encoder broken (#264) |
| OPUS downlink encode | stub (unwired) | `audio_codec.py:79` never instantiated |
| Pre-TTS text cleaner | shipped | `text_cleaner.py` |
| Dictation segment STT + async title/summary | shipped (Cloud/Solo only) | `dictation_post.py:224` Local skips LLM |
| ConversationEngine native + prose tool loops | shipped | `conversation.py:894/727` |
| Append-only MessageStore + cross-modal continuity | shipped | `messages.py:30/108` |
| Memory facts + embeddings (nomic 768-dim, sqlite-vec) | shipped | `memory.py:301` |
| Memory injection on native Local path | broken | `inject_memory=False` `conversation.py:958`; `recall` excluded |
| FTS5 keyword/hybrid search | stub (dead) | built+triggered, never queried `memory.py:136` |
| Document RAG ANN index | missing | Python-loop scan `memory.py:441` |
| Memory dedup/forget/retention | missing | facts grow unbounded |
| Tool registry (~23 tools) + agent_log chokepoint | shipped | `registry.py:54` |
| 5-dialect tolerant marker parser | shipped | `parser.py:170` |
| Google Calendar (×4) + Gmail (×5) OAuth tools | shipped | `integrations_init.py` |
| Google Tasks (`tasks_*`) | stub/abandoned | referenced in prompts, no backend |
| Write-tool server-side confirmation gate | missing | prose-only |
| Home Assistant / Spotify integrations | missing | plan items, no code |
| Scheduler (parse/persist/boot-replay/offline-queue) | shipped | `scheduler/manager.py`, `store.py` |
| `schedule_reminder` in native fast set | missing | excluded `conversation.py:66` |
| Reminder→notification surface | broken | fires to chat store not now-card |
| Recurrence | stub (inert) | column persisted, never read `models.py:57` |
| Snooze action | stub (unwired) | `handle_snooze` no `register_action` |
| Dashboard 11-tab SPA + auth proxy | shipped | `dashboard.py:81` |
| Spend/billing rollup | broken (dead) | `cost_mils` never persisted to `events` `pipeline.py:1351` |
| OTA check/serve | shipped | `synthesize.py:315`; no upload UI |
| Channel reply RPC (gateway) | shipped (opt-in) | `channel_reply_handler.py`; default Mock `ok=true` |
| Inbound platform messages (W7-G) | stub/planned | `gateway.py:556` log+drop |
| ed25519 signed-connect handshake | shipped (verified) | `device_identity.py:155` byte-correct vs TS |
| Gateway self-grants `operator.admin` scope | shipped (over-scoped) | `gateway.py:400` |
| WS protocol (JSON + binary VID0/AUD0/raw) | shipped | `server.py:685`, `protocol.md` stale |

---

## 3. What We're Doing RIGHT (Moats)

1. **Truly local-first topology with six real tiers**, including no-Dragon (K144) and no-account (Solo) escape hatches. No mainstream competitor offers "the brain is optional"; HA Voice is the only peer and lacks on-device LLM tiers.
2. **On-device native tool-calling on edge hardware** — Granite 4.0 Nano-1B with a *measured* 436s→29s prompt-cache-stable optimization (byte-stable prefix, `_compact_tool_result`, curated 13-tool fast set, `conversation.py:550-635/91-136`). Real agentic local inference, not intent-matching.
3. **`turn_id` identity threaded end-to-end** (`voice_dictation.c:429/462/496`, `ui_notes.c:1025`) — a 60–90s-late summary can't clobber a fresh turn; host-tested (132 checks), origin-guarded, optimistic save frees the orb immediately.
4. **Lock-free hot-path state reads** (`voice_dictation_state()`, `ui_orb.c:2620`) as the correct TASK_WDT root-cause fix; generation-guarded async LVGL; PSRAM task stacks killing the heap_wd panic class; coredump-on-reboot. Production-grade, scarred-from-real-failures hardening.
5. **Battle-tested multi-ladder self-healing reconnect** (`voice_ws_proto.c:1736/1819/1901`, `voice.c:1399`) — jittered backoff, dual escalation ladders, `!voice_is_connected()` reboot gate, bidirectional ngrok fallback. Every threshold carries a regression comment.
6. **Honest, ordered degradation UX** (`voice_get_degraded_reason`, orb pip re-contracts the next tap, structured severity/scope error frames) — directly answers the user's own "no state legibility" complaint (`feedback_voice_ux_no_patching.md`).
7. **Privacy-by-default channel posture** — all 8 toggles default OFF, quiet-hours with correct wrap-around math, dedupe/snooze rings, 30 s reply-TTL, REPLY-mid-PROCESSING refusal.
8. **An end-to-end, in-use extensibility platform** — six widget types, capability handshake Dragon actually consumes (`Tab5Surface.for_skill()`), real reference skills (TimeSense, QuickPoll). Features as Python on the brain, not C reflashes.
9. **A coherent hardware-reactive orb identity** (`ui_orb.c`, ~2,640 LOC) — IMU tilt-specular, mic-RMS + treble-band hue, circadian palette, bounded "one motion per state" budget, hard-won render discipline. Echo Show/Nest Hub are photo-frame-plus-cards by comparison.
10. **Disciplined SOLID extraction tracked against git** — `debug_server.c` 4,520→861 LOC across 20 family modules; server-side WS commands each their own tested module with failure isolation.
11. **Roadmap-vs-reality honesty culture** — ground status in `git log --grep`, "audit before scoping," verified gauntlets, OAuth that survived Google's verification wall via device-code→PKCE. The gap is the docs, not the team.

---

## 4. What Needs Improvement

- **Effective-route legibility (slice 3.6).** The picker shows *intent*; nothing reflects the resolved route after privacy-lock/engine-pin/custom-model. The one UX deliverable the whole TT #724 program named as core is unshipped (`ui_mode_sheet.h:31`, `ui_home.c:523`). Home is mode-illegible (orb un-tinted, chip a ghost).
- **Conversation latency + feel.** Native path is non-streaming (`conversation.py:1084`) → silent for the entire 30–180 s inference window; no Ask-path STT partials; no real VAD; ~96 s structural Local floor with no thermal management in the LLM path.
- **Memory quality.** No relevance threshold (top-3 facts injected unconditionally, cosine ~always >0), no dedup/conflict-resolution, no retention, no hybrid search (FTS5 built but dead), document chunks unindexed. And invisible entirely on the default Local path.
- **Stuck-turn recovery legibility.** A real mode-aware watchdog exists (`ui_voice.c:341`, Local=300 s) but is overlay-coupled with loose budgets; the dead `s_last_activity_us` should drive a short mid-turn liveness timeout, and home-orb tap during PROCESSING should cancel.
- **Now-card / toast resilience.** Collisions silently lose high-priority messages (`ui_home.c:3186`); toasts are last-wins (`:2714`) so a burst shows only the last exactly when legibility matters most.
- **Dead-code hygiene.** ~400 LOC of dead orb-anim/overlay state machines, dead KWS path, `solo_session_rotate`, `qr_decoder_decode_frame`, non-streaming `process_text`, standalone `openrouter_tts` — all ship unused and obscure live paths.
- **Auth/host consistency.** Sessions/memory overlays use a compile-time token + hardcoded `192.168.1.91` while agents use the runtime NVS token + `dragon_host`; a token rotation fixes some surfaces and 401s others.
- **Config reproducibility.** Committed `config.yaml` (`fleet: []`, `lmstudio_model: "default"`) doesn't reproduce the live Granite/Qwen two-tier design.

---

## 5. What We're Doing WRONG and WHY (Root Causes)

### 5.1 Privacy promises live in copy, not in enforcement
- **Privacy lock is a no-op for voice** (`high`) — `voice_modes_route_text` is called only from `voice_send_text` (typed). A spoken turn streams PCM to Dragon and the LLM hop is chosen by Dragon's last `config_update` (`voice_ws_proto.c:640`). privacy_lock=ON + speak-in-Cloud sends voice + transcript to the cloud despite "Locked: on-device." *Root cause:* routing lives client-side on the text path only; voice routing delegated to Dragon with no client gate. *(Needs Dragon-side confirmation there is no server-side honor path — proof is by absence.)*
- **Solo violates "no Dragon" + has no spend cap** (`high`) — `voice_solo.c:211/399` POSTs every turn to Dragon's message store; cap enforcement lives in the bypassed `voice_send_config_update_ex`. *Root cause:* message-mirror added by a later cohesion audit without reconciling the privacy intent; cap coupled to the Dragon path Solo skips.

### 5.2 The load-bearing safety features are fiction, not partial
- **OTA auto-rollback not compiled in** (`critical`) — `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set` (verified in built `sdkconfig`); the bootloader PENDING_VERIFY state machine is absent, so `mark_valid` (`main.c:652`, run unconditionally early) is a no-op and a crash-looping image never reverts. The documented `/ota/rollback` endpoint (port 3500) doesn't exist (debug server is 8080). *Root cause:* `sdkconfig.defaults` only enabled `ALLOW_HTTP`, never the rollback flag; mark-valid wired for "boot reached app_main" not "device healthy"; rollback runbook never implemented as code.
- **Server-side spend cap is dead** (`high`) — real `cost_mils` (`text_path_receipt.py:136`) is sent only as a WS `receipt` frame, never `add_event`'d; the only persisted `api_usage` event (`pipeline.py:1351`) carries no `model`/`cost_mils`, so `_maybe_emit_cap_downgrade` can't fire. *Root cause:* the priced receipt and the persisted event are two paths that never converged. *(Correction vs source lenses: the Tab5 client cap demonstrably fires — enforcement is single-tier, not dual.)*
- **Default wakeword has no detector** (`high`) — `wake_src=dragon` (`settings.c:620`) streams `WAK0` PCM but `binary_frame_dispatch.py:72-128` has no WAK0 branch; it falls through to `feed_audio()` as raw mic. *Root cause:* Tab5 commit #616 claimed a Dragon "Path B MVP" never merged into TinkerBox.
- **Write tools have no confirmation gate** (`high`) — `gmail_send`/`calendar_create`/`calendar_cancel` execute on the model's decision; "confirm before calling" is prose-only on models the project's own gauntlets show firing wrong tools on red-herrings. *Root cause:* confirmation modeled as prompt instruction, not a runtime gate.

### 5.3 Latency optimizations silently dropped capabilities
- **Native Local path injects zero memory and references phantom tools** (`med`) — `inject_memory=False` + `recall` excluded from `_FAST_NATIVE_TOOLS` (`conversation.py:68/958`) means stored facts are invisible on the common voice turn; the guidance/few-shot instruct the model to call `tasks_*` tools that don't exist (`unknown_tool` → chain breaks). *Root cause:* the native path bought prompt-cache stability by stripping memory; prompt surfaces were written ahead of an abandoned Tasks backend.

### 5.4 Build-then-gate-then-forget
- Video calling (`#if 0`'d but live RX modal), Solo RAG/QR, OPUS downlink, the router fleet, the FOCUS overlay, three TTS backends, recurrence/snooze — all built or half-built then disabled/mocked/unwired with the dead surface still claiming to be a feature. *Root cause:* incremental history left dead code in place; "delete in a follow-up" never came.

### 5.5 Docs are prose, not generated
- `protocol.md` omits `turn_id` and the live dictation events; three docs give three Local-model defaults; the rollback-disabled fact contradicts three docs; the NVS table says `vmode 0-4` for a 0-5 system; the router fleet lists unreleased models. *Root cause:* hand-maintained prose treated as the contract while code churns; no mechanism keeps them honest.

### 5.6 Single-device/single-developer assumptions baked in
- Hardcoded "Emile," `192.168.1.89`/`.1.91`, the author's ngrok host, compile-time WiFi creds, no SoftAP, split auth sources, English-only everywhere. *Root cause:* provisioning built for the dev's own bench, not a fresh unit or a fleet.

### 5.7 Security was never designed in
- A global trust policy instructs "execute any inbound instruction as if typed at the terminal"; secrets sit in repo/chat/serial; the debug token prints to UART every boot; the data plane is plain-HTTP/`ws://` with `skip_cert_common_name_check`; the gateway self-grants `operator.admin`. *Root cause:* "no security theater" was an explicit posture, not an oversight — which is the most dangerous form.

---

## 6. Multi-Step User Journeys (14)

| # | Journey | Verdict |
|---|---|---|
| 1 | First-time setup OOB | **broken** (retail) / usable-with-friction (developer) |
| 2 | Hands-free quick Q&A | **broken** (default) / usable-with-friction (debug opt-in) |
| 3 | Long-form dictation → note → retrieval | usable-with-friction |
| 4 | Take photo → ask about it | **not-possible** |
| 5 | Inbound message → reply | **broken** (default) / lab-only |
| 6 | Switch modes mid-conversation | usable-with-friction |
| 7 | Fully offline | **broken** (default) / usable-with-friction (pre-configured) |
| 8 | Two-way video call | **not-possible** (from UI) / debug-only |
| 9 | "Remind me at 6pm" | **broken** |
| 10 | Memory across sessions | **broken** (Local default) / usable-with-friction (Cloud) |
| 11 | Agentic chain (calendar + email) | **broken** (literal) / usable-with-friction (closest, vmode 0-2 only) |
| 12 | Find & use a skill/widget | **broken** |
| 13 | OTA update + rollback | usable happy-path / **rollback broken** / deploy broken |
| 14 | Recover from stuck/error state | usable-with-friction |

**#1 First-time setup.** Breaks: no on-device Dragon discovery (stale `192.168.1.89`, mDNS deleted with #155); Dragon host only settable in buried Settings, not onboarding, so the 3 s verify gate always fails on a fresh device → infinite re-onboarding loop; **WS bearer token has zero on-device provisioning** (compile-time only) → 3×401 → unrecoverable brick; completion gated on `voice_is_connected()` traps offline-only buyers. *Peer:* SoftAP/BLE captive-portal + mDNS/SSDP brain discovery + 6-digit/QR token pairing (Sonos/Nest/HA Green). *Biggest win:* add a "Connect to Dragon" onboarding step (mDNS + token field) and decouple completion from Dragon RTT — closes all three criticals.

**#2 Hands-free Q&A.** Default `dragon` wake is dead and idle-streams mic; the working ext_pcm path is debug-HTTP-only with no Settings picker; barge-in is contradictory dead code; matcher is acoustically fragile (`{thinker,hick,hanker}`); Ask-path watchdog exists but loose/overlay-bound; no STT partials. *Peer:* always-on neural KWS, on by default, live partials, universal barge-in, no pre-wake cloud audio (Alexa/Nest/HomePod). *Biggest win:* default `wake_src=ext_pcm`/`off` + add a Settings picker; stop arming the dead WAK0 stream.

**#3 Dictation → retrieval.** Happy path is the most structurally-sound subsystem. Breaks: silent SD reopen-failure mid-recording can swallow the take; 30-note ring silently overwrites; search is substring-over-≤30-local-notes while Dragon holds full transcripts (FTS5 built but unqueried); Local mode gets first-50-chars title not an LLM summary; two racing failure timers (45 s + 60 s). *Peer:* durable-first capture, live on-device partials, server-side full-history semantic/FTS search (Otter/Recorder). *Biggest win:* server-side `/api/notes?q=` + page from Dragon (make Dragon the system of record).

**#4 Photo → ask.** Not-possible: Tab5 sends `user_image` but Dragon only handles `user_media` (`server.py:864` logs+drops); upload route returns only `media_id`, never echoes a `media` bubble; no capture→ask affordance; Local default (Granite 1B) has no vision. The entire Dragon vision stack (persistence, hydration, capability-gated routing) is built and dead. *Peer:* camera frame + question as a single multimodal turn with thumbnail continuity (Gemini Live/ChatGPT). *Biggest win:* rename `user_image`→`user_media` + carry question text + add a capture→"Ask about this" affordance (~20 LOC).

**#5 Inbound → reply.** Broken: gateway reader **drops every inbound event** (`gateway.py:556-561`), so a real platform message never reaches Tab5 outside the debug endpoint; default `channel_gateway.enabled=False` → MockConnector returns `ok=true` → green "Replied via X" ACK while nothing leaves Dragon; now-card collisions lose high-pri messages. *Peer:* durable inbound pipe, opaque correlation token, stacking notifications, truthful Sent/Delivered/Failed. *Biggest win:* implement W7-G inbound + make the mock return a "simulated" status the ACK renders honestly.

**#6 Switch modes mid-thread.** Context *does* carry server-side (history untouched on swap, `server.py:1466`) — but the in-flight slow answer is **discarded**, not re-run on the new model; user must re-ask; for spoken turns the new mode is advisory-only (privacy lock no-op); no effective-route surface; home is mode-illegible. *Peer:* per-turn retryable model with a persistent label ("retry with Opus") — ChatGPT/Claude/Perplexity. *Biggest win:* on mode-commit-during-PROCESSING, capture the last user message and auto-resend through the new mode after the config ACK.

**#7 Fully offline.** The orb hard-refuses ALL voice turns when WS is down (`ui_home.c:1926`), including vmode 4/5 which don't need Dragon — the two offline escape hatches are unreachable from the front door; "auto-starts SD recording on mic tap" is false for Ask (dictation-only); misleading "Reconnecting to Dragon" on Solo/K144 failures. Dictation-to-SD + background sync is genuinely best-in-class. *Peer:* explicit "offline, limited" mode that never shows a refusing control; on-device intent routing (Siri). *Biggest win:* move the `voice_is_connected()` gate out of the UI entry point into the route layer (skip for vmode 4/5).

**#8 Video call.** Not-possible from UI (nav tile `#if 0`'d); the full stack is debug-HTTP-only; no signaling/ringtone/identity/timeout; any stray VID0 frame auto-pops a full-screen modal; no AEC (guaranteed echo); two concurrent 5 fps capturers amplify the capture buffer race. *Peer:* explicit SDP signaling, one negotiated media path, AEC + jitter buffering, bandwidth adaptation (FaceTime/WhatsApp). *Biggest win:* resolve the half-state — cheapest is delete (removes the live RX modal vector + capture-race amplifier).

**#9 "Remind me at 6pm."** Broken at both ends: `schedule_reminder` is excluded from the native fast set (LLM never sees it); even reached, bare "at 6pm" doesn't parse (needs a today/tomorrow anchor); fired reminders surface only in the chat message store (no toast/now-card/orb-pulse/sound on home); Dismiss is dead, recurrence inert, no timer/alarm primitive. The durable middle (SQLite/boot-replay/offline-queue/caps) is excellent. *Peer:* deterministic reminder grammar, robust time parsing, unmissable OS-level fire, recurrence + snooze (Alexa/Google/Siri). *Biggest win:* wire the fire into `ui_notification_show` + add `schedule_reminder` to the fast set (~15 LOC connects fully-built pieces).

**#10 Memory across sessions.** `remember` works; `recall` is structurally disabled on the default Local path (`inject_memory=False` + `recall` excluded). Works only on prose/Cloud paths. No relevance floor, no dedup/conflict-resolution, FTS5 dead. *Peer:* relevance-filtered passive injection every turn + write-time consolidation + hybrid retrieval (ChatGPT/Claude). *Biggest win:* add `recall` to the fast set + inject a relevance-floored top-1–2 facts (~5 LOC flips broken→working).

**#11 Agentic chain.** Broken for the literal request: no `calendar_tomorrow` (only today/week); in **vmode=3 the named "smart" mode Dragon's Calendar/Gmail tools are invisible** (gateway runs its own skills); "email Sarah" can't resolve to an address (no contact lookup); 3-step task vs 3-call cap; no write confirmation; phantom `tasks_*`. *Peer:* relative-date as an argument, People-API contact resolution, planning without a hard cap, mandatory confirmation, real tool telemetry. *Biggest win:* route calendar/email/agentic asks through Dragon's own native tool loop (the unbuilt `tinkerbox-mcp` bridge) instead of the gateway.

**#12 Find & use a skill/widget.** Broken: a `ui_skills.c` catalog exists but lists *LLM tools* not widgets, isn't tappable-to-invoke (only stars), and never says what to speak; the only invocation is guessing a phrase; tapping a running widget navigates *away* to Agents; dismiss is a hidden long-press. *Peer:* discovery surface = launch surface, every entry shows "Try saying…" (Shortcuts/Alexa skills/Raycast). *Biggest win:* make skill cards launchable with example phrases (reuse `ui_agents.c:822` demo mechanism).

**#13 OTA.** Happy path SHA256-verifies and applies on a pristine boot heap. **Rollback is BROKEN** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE not set` → `mark_valid` no-op → bad build bricks to USB-reflash; documented `/ota/rollback` absent). No auto-check, LAN-only, no upload UI (deploy = manual scp + hand-edited `version.json`). *Peer:* always-on rollback gated on a real self-test, signed images, auto-check, deploy console (ESPHome/Particle/Mender). *Biggest win:* `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + gate `mark_valid` behind WS-`session_start`+home-painted.

**#14 Recover from stuck state.** Three of four states self-heal (PROCESSING watchdog `ui_voice.c:341`, WS-reconnect robust, spend-cap degrades to free Local). K144 self-heals but slowly (2-3 min, can escalate to full tablet reboot). Friction is legibility: 5-min Local watchdog budget, overlay-coupled cancel, dead `s_last_activity_us`, no `wake_src` recovery surface. *Peer:* liveness (not timeout) detection, always-present STOP, peripheral faults degrade locally (never reboot host). *Biggest win:* wire `s_last_activity_us` into a short mid-turn liveness timeout + route home-orb tap during PROCESSING/SPEAKING to cancel. *(Correction: the source map's "PROCESSING has no watchdog" is wrong — the true finding is "slow/overlay-bound recovery + dead liveness signal.")*

---

## 7. Competitive Gaps + Where We're Ahead

### 7.1 Gaps (priority weighted for privacy-first; p0 = breaks the promise / safety hole)

| Capability | Who has it | Our state | Priority | Effort |
|---|---|---|---|---|
| Out-of-box wakeword that doesn't leak mic | Alexa/Google/Siri/HA Voice/Ray-Ban | broken default + privacy regression | p0 | m |
| Privacy lock that holds for voice (our own promise) | (differentiator) | have-but-broken | p0 | m |
| Working firmware self-update with rollback | every shipping device | not compiled in (bricks) | p0 | m/s |
| Server-side confirmation gate on write tools | ChatGPT/Gemini | missing (prose-only) | p0 | m |
| Reliable timers/alarms | all five | missing | p0 | m |
| Persistent recallable memory | ChatGPT/Gemini/Limitless | invisible on default path | p0 | s |
| Effective-route honesty | (differentiator gap) | missing | p0 | m |
| Smart-home device control | Alexa/Google/Siri/HA Voice | missing (Google-only) | p0/p4-value | xl |
| Recurring reminders / snooze | Alexa/Google/Siri | persisted-but-inert | p1 | m |
| Sub-second streamed conversation + barge-in | ChatGPT AVM/Gemini Live | non-streaming, dead barge-in | p1 | l |
| Robust VAD / noise-gated end-of-speech | ChatGPT AVM/Gemini Live | RMS-only, never-ends in noise | p1 | m |
| Acoustic echo cancellation | every smart speaker | removed #162, never restored | p1 | l |
| Multimodal "what am I looking at?" | Gemini Live/Ray-Ban/ChatGPT | unreachable (frame mismatch) | p1 | s |
| Safe agentic actions (calendar/email/tasks) | ChatGPT/Gemini | phantom Tasks + no confirm | p1 | l |
| Self-updating fleet (auto-check/remote/upload) | every device | stub | p1 | m |
| First-boot provisioning (SoftAP/discovery) | every device | missing | p1 | m |
| Spend caps that actually fire | (our safety feature) | server-side dead | p1 | m |
| Inbound platform messages → device | smart displays/Limitless | drop+log stub | p1 | l |
| Stuck-turn self-recovery (fast/legible) | cloud (server-managed) | slow/overlay-bound | p1 | s |
| Reproducible-from-repo deploy | mature products | single-model box | p2 | s |
| Multi-user / voice-ID | Alexa/Google/Apple | hardcoded "Emile" | p2 | l |
| Music / media playback | Alexa/Google/Sonos/Siri | missing | p2 | l |
| Calling / intercom | Alexa/Google/Apple | gated-off | p2 | l |
| Routines / automations | Alexa/Google/HA | missing | p2 | l |
| Proactivity | Alexa Hunches/Gemini | untested + lossy | p2 | l |
| Data export / deletion | (privacy obligation) | missing | p2 | m |
| i18n / localization | all | English-only, no scaffolding | p2 | l |
| Accessibility (focus/SR/reduce-motion) | all | touch-only, settings no-op | p2 | m |
| Multi-room / multi-device | Alexa/Google/Sonos | single-device | p3 | xl |

### 7.2 Where we're ahead
- **No-account / no-Dragon escape hatches** (vmode 4/5) — architecturally unique; "the brain is optional."
- **On-device agentic native tool-calling** on $50-class edge hardware with a real measured latency win.
- **`turn_id`-correct dictation FSM** + optimistic save — capture UX rivals dedicated note pendants.
- **Production embedded resilience** (lock-free reads, generation guards, layered reconnect, coredumps) — Humane/Rabbit visibly lacked this.
- **Privacy-by-default channel posture** and an **honest, auditable engineering culture** (obs traces on every decision, suggestions copy that refuses to over-promise, safe-AST calculator, SSRF guards).
- **A genuine hardware-reactive orb identity** and a **live Python-on-the-brain extensibility platform**.

---

## 8. What's MISSING — "Half-Built Here" vs "Never Started"

### Half-built here (built, then gated/mocked/unwired)
- Two-way video calling — ~1200 LOC + 12 endpoints, nav tile `#if 0`'d, RX modal still live, no signaling/AEC.
- Solo on-device RAG + QR key provisioning — fully implemented, only the test endpoint touches RAG; QR has zero callers; toast instructs a non-existent flow.
- OPUS codec — uplink encoder crashes (#264), downlink encoder never instantiated; all TTS is raw PCM.
- Capability router — fully coded + 74 tests, dormant behind `fleet: []`; router/dual don't forward native-tools/summary.
- Three TTS backends (Supertonic/Kitten/Edge) — register + pickable, silently downgrade to Kokoro on the voice path.
- Reminders — durable scheduler complete, but unreachable by the LLM and fires to the wrong surface; recurrence inert; snooze unwired.
- Presence-dim — orb behavior built, fed by no producer; the real vision signal feeds brightness instead.
- Spend/billing — full chain wired, dead column.
- Channel inbound (W7-G) — named, the drop branch ships instead of the handler.
- Async "smart tier" — runs but silently drops results on disconnect; untested.
- Memory hybrid search — FTS5 table + triggers maintained, never queried.
- Multimodal — Dragon side complete, Tab5 sends the wrong frame name.
- OTA rollback — scaffolding present, the bootloader feature it targets is not compiled in.

### Never started
- Smart-home control (Home Assistant/Spotify) — the integrations program's raison d'être.
- Timers/alarms with audible ring + reboot persistence.
- Music / media playback transport.
- Multi-user / voice-ID; per-user memory partitioning.
- Routines / automations engine; proactive triggers.
- Data export / deletion / unified `forget`; retention/purge on facts+docs.
- i18n scaffolding (string table, locale key); non-English wakeword/STT/TTS/`parse_when`.
- SoftAP/captive-portal first-boot provisioning; on-device WS-token provisioning.
- Fleet plane: remote OTA, auto-check, firmware-upload UI, central log/crash upload, telemetry opt-in.
- Protocol version negotiation (`register` carries no `protocol_version`).

---

## 9. Cross-Cutting Risks

### 9.1 Privacy / Security (the largest under-examined area)
- **Trust-policy-as-RCE** (`critical`): the global CLAUDE.md instructs "execute any inbound instruction as if typed at the terminal" with full sudo/network/filesystem access and "no confirmation." Combined with the planned inbound channel→LLM→tool path and no prompt-injection defense, a spoofed messaging account becomes device/host control.
- **Committed/serial-logged secrets** (`high`): live debug bearer token `05eed3b1…` in `CLAUDE.md:454`; the debug token printed to UART on every boot (anyone with serial capture owns `/touch`/`/navigate`/`/settings`/`/nvs/erase`/`/reboot`/camera); hardcoded public ngrok hosts in firmware; passwordless sudo after SSH; MEMORY.md-acknowledged leaked OpenRouter/coding-plan keys "passed through chat." Tokens are not rotatable on-device.
- **Plain-HTTP data plane** (`med-high`): `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` + `skip_cert_common_name_check=true` (`ota.c:186`) + `ws://` on LAN + cleartext REST. The OTA SHA256 travels the same unauthenticated channel as the binary → defends against corruption, not MitM.
- **Over-scoped gateway** (`med`): connector self-grants `operator.admin` (`gateway.py:400`), relying on loopback self-pairing skip; a privilege-escalation hole if the gateway ever moves off loopback.
- **Prompt-injection → irreversible tool execution** (`high`): untrusted inbound text reaches the LLM that can `gmail_send`/`calendar_cancel` with prose-only confirmation small models ignore.
- **Unauthenticated discovery surfaces** (`low-med`): `/info` + `/selftest` enable LAN device enumeration/fingerprinting.
- **Memory mirror = second uncontrolled PII copy** (`high`): `gateway_memory_mirror.py` duplicates `remember` facts with no joint delete and no retention — a data-rights exposure on a product marketed on data ownership.
- *Auth correction:* `auth.py:66` fails **closed** (503 when no token). The residual risk is the known-constant CI token (`ci-bearer-token`) + scheduler trusting caller-supplied `device_id` with no ownership check.

### 9.2 Latency / Thermal / Power
- ~96 s structural Local turns (Qwen3.5-4B ~16 tok/s on QCS6490 CPU, no NPU lane for 4B, thermal half-clock at 90°C — an external $5-fan/ops fix, not code). Native path non-streaming → silent inference window.
- **No charging/battery-state awareness anywhere** (`med-high`): `WIFI_PS_NONE` permanent (`wifi.c:117`), idle mic-streaming, always-on orb animation, 4h K144 reboots + 60s UART resyncs, no screen-off/sleep, OTA can't gate on charging. Battery life is unscored and likely poor for a tablet form factor.

### 9.3 Reliability
- OTA rollback not compiled in (the headline). Mid-session llama-server death = hard wedge (one-shot probe, no re-probe/circuit-breaker). Ask-FSM recovery slow/overlay-bound + dead `s_last_activity_us`. Camera capture use-after-requeue race (`camera.c:328-338`, verified). Two racing dictation timers. Single shared aiosqlite connection serializes the whole server under a multi-chunk RAG scan.

### 9.4 Single-device / Supportability
- No fleet plane: no central log aggregation, no crash-report upload (coredumps stay on-device), no remote diagnostics, no telemetry opt-in/out, no remote OTA. The stack is operable on one device on one LAN by its author and by nobody else. Stale hardcoded IPs/host/name/token throughout; deploy is manual scp + hand-edited JSON with a SHA-typo footgun.

---

## 10. Prioritized Roadmap

> Priority weighting is for a privacy-first/local-first product: anything that breaks the core promise or is a security/safety hole is escalated above feature breadth.

### NOW — trust, safety, and the promise

| item | impact | effort | rationale |
|---|---|---|---|
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + gate `mark_valid` behind WS-`session_start`+home-painted | critical | s | Two lines + relocate one call turns the headline safety feature from fiction to real; without it the first field regression bricks devices. |
| Enforce privacy-lock/engine-override on the **voice** path (gate cloud uplink when locked; or refuse honestly) | critical | m | The product's defining claim, currently a no-op on the primary modality. Confirm a Dragon-side honor path too. |
| Stop the dead WAK0 idle-mic stream: default `wake_src=ext_pcm`/`off` + add a Settings picker | critical | m | Kills an always-on-mic privacy regression *and* makes the #1 differentiator reachable. Re-verify ext_pcm on the new USB transport first. |
| Scrub committed/serial-logged secrets; rotate leaked keys; stop printing the debug token to UART; add on-device WS-token provisioning | high | m | Secrets-in-repo/chat/serial is full device control; the token isn't fixable on-device today (401-lockout brick). |
| Reject "trust all inbound" before W7-G; add a server-side confirmation gate for write tools | critical | m | Closes the prompt-injection→irreversible-action chain *before* inbound ships. |
| Persist a priced `api_usage` event (`model`+`cost_mils`) + add a Solo client-side cap | high | s | Revives the dead server-side spend cap and closes uncapped Solo cloud spend. |
| Add Dragon discovery (mDNS) + WS-token field to onboarding; decouple onboarding-complete from Dragon RTT | high | m | Collapses the three critical first-boot breaks; today a retail buyer cannot complete setup. |

### NEXT — close the table-stakes and the half-states

| item | impact | effort | rationale |
|---|---|---|---|
| Wire fired reminder into `ui_notification_show` + add `schedule_reminder` to the native fast set; add a real timer/alarm primitive | high | m | ~15 lines connects a fully-built scheduler to the LLM and the notification surface; timers are the #1 daily use. |
| Re-enable memory on the Local path: add `recall` to the fast set + inject a relevance-floored top-1–2 facts | high | s | ~5 lines flips cross-session memory broken→working on the default privacy-first mode. |
| Ship effective-route honesty (3.6): chip=intent, orb/pill=resolved route | high | m | The one UX deliverable the remediation program named as core; also the only way the voice-privacy fix is legible. |
| Resolve the video-calling half-state — **delete** (cheapest) or finish signaling/ringtone/AEC | med | m/l | Removes the live auto-modal RX vector, dual-capture SRAM pressure, and the capture-race amplifier in one stroke. |
| Fix camera capture use-after-requeue race (requeue-after-consume, `CAM_BUFFER_COUNT≥4`) | high | s | Verified real (`camera.c:335`); prerequisite if calling re-enables. |
| Rename Tab5 `user_image`→`user_media` + carry question text + capture→"Ask about this"; route photo Qs to a vision tier | high | s | ~20 lines lights up the entire dead Dragon vision stack. |
| Add server-side `/api/notes?q=` search (wire the built FTS5) + page notes from Dragon | high | m | Makes "find a note days later" work; today a 30-note ring overwrites silently. |
| Generate contract surfaces from code (protocol frames + `turn_id`, NVS table, endpoint count, mode table, sdkconfig safety flags); delete the fictional router fleet | med | m | Highest-leverage doc fix; reconcile three Local-default answers and the rollback contradiction. |
| Ask-FSM liveness timeout (read dead `s_last_activity_us`) + home-orb tap-during-PROCESSING cancel; mid-session llama-server re-probe | high | s | Demote the overstated "no recovery" to its true form and make stuck-turn recovery fast and legible. |
| Add SoftAP/captive-portal first-boot provisioning | med | m | The standard normie onboarding path. |
| Wire `vision_service` presence edges → `ui_orb_set_presence`; delete dead orb-anim/overlay trees | med | s | One wire delivers a 90%-built feature; deleting dead code de-risks future re-enables. |

### LATER — productization plane, breadth, polish

| item | impact | effort | rationale |
|---|---|---|---|
| Smart-home (Home Assistant) tool — local REST + long-lived token | high | l | The category table-stake and the integrations program's raison d'être; highest product-value gap. |
| Capture the live two-tier Granite/Qwen config as repo artifacts; make router/dual forward native-tools + summary before enabling `fleet` | med-high | m | Repo can't reproduce the live brain; flipping `fleet` today silently disables native tools. |
| Data export + deletion (`/api/v1/export`, unified `forget` across the mirror, retention/purge) | high | m | A privacy-first product with no deletion story and a duplicating memory mirror is a data-rights exposure. |
| Fleet plane: remote OTA, auto-check timer, firmware-upload UI, central log/crash upload (telemetry opt-in) | med | l | Today operable on one device on one LAN by its author; this is the path to "shippable." |
| Streaming native path + real VAD (silero) + STT partials on Ask + AEC | high | l | The "live conversation" feel competitors ship as table stakes. |
| Multi-user/voice-ID (start: NVS-bound greeting name) + i18n scaffolding (string table, locale key) | med | l | Hardcoded "Emile" and English-only cap the addressable market. |
| Apply press-feedback/theme/font-scale/reduce-motion across sheets; queue toasts; degender picker copy; fix silent nav debounce no-op | med | m | Coherence + accessibility; busiest surfaces have no pressed state and burst-lose error toasts. |
| Music/media + routines + proactivity (persist async-tier results first — silently dropped) | med | l | Mainstream breadth; flag music as a deliberate voice-first scope cut. |

---

## 11. Appendix: Doc-vs-Code Drift

| Claim (doc) | Reality (code) | Where |
|---|---|---|
| OTA auto-rollback "if new firmware crashes, bootloader reverts" | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set`; `mark_valid` unconditional early | built `sdkconfig`, `main.c:652`, CLAUDE.md OTA/Recovery |
| `POST :3500/ota/rollback` manual recovery | endpoint absent; debug server is :8080 | `debug_server_*.c`, CLAUDE.md Recovery §1 |
| "Tab5 checks hourly" for OTA | no timer references `TAB5_OTA_CHECK_INTERVAL_MS` | `config.h:66` |
| Keepalive `{"type":"ping"}` JSON every 15s | no app-level JSON ping; only WS-frame ping | grep voice_ws_proto.c |
| "Offline Fallback: auto-starts SD recording on mic tap" | true only for long-press dictation; Ask refuses | `voice.c:1755/1884` |
| Reconnect watchdog 5s poll, 10→60s backoff | esp_websocket jittered 1→30s; named API is no-op stub | `voice.c:2827`, `voice_ws_proto.c:113` |
| vmode 4/5 "auto-downconvert to 0 on the wire" | raw int sent; downconvert removed | `voice_modes.c:41`, `debug_server_mode.c:68-78` |
| Dictation auto-stop 5s silence / 30s/5-min cap | cap is 14400s (4hr); 5s silence still wired | `voice_dictation.h:58`, `voice.c:193` |
| Presence-dim shipped hardware behavior | only `/orb/presence` debug; no producer | `ui_orb.h:8`, `vision_service.c:242` |
| Settings "55 objects / 7 sections" | 11 sections, far more objects | `ui_settings.c` |
| Voice overlay "animated orb / dictation waveform / chat bubbles" | dead-coded; home `ui_orb` owns the orb | `ui_voice.c:1024` |
| `ui_focus` = "focus-ring helpers" | it's the FOCUS activity overlay; no focus-ring exists | `ui_focus.c`, CLAUDE.md:906 |
| Camera "SC2336 2MP @ 0x30" | SC202CS, 1-lane RAW8, 0x36 | `camera.h:2-7` vs `camera.c`, `sdkconfig.defaults` |
| Camera "selectable resolution / JPEG photos" | resolution no-op; photos raw BMP | `camera.c:298/341` |
| Video calling "live, nav Call tile" | `#if 0`'d; debug-HTTP only | `ui_nav_sheet.c:77-105` |
| protocol.md v2.0.0 — heartbeat 600s | `heartbeat=60.0` | `server.py:588` |
| protocol.md — bare `{"type":"start"}` | `turn_id` required end-to-end, undocumented | `start_handler.py:69`, `voice.c:1788` |
| protocol.md §16 lists `config_ack`/`user_media` as Tab5→Dragon | Tab5 emits neither; sends `user_image` (unhandled) | `server.py:864` |
| "Memory hybrid search / keyword enabled" | FTS5 built+triggered, never queried | `memory.py:136/167` |
| "All stored facts auto-recalled before every LLM call" | false on native Local path (`inject_memory=False`) | `conversation.py:958` |
| "Document Service uses sqlite-vec" | facts do; chunks have no vec table | `memory.py:441` |
| Google Tasks "Phase 2 shipped `[x]`" | no backend/tool/registration; phantom prompt refs | `PLAN-tinkerbox-integrations.md:12`, `conversation.py:54` |
| Integrations REST `/{connect,disconnect,list}` | nested `/{name}/connect` etc. | `api/integrations.py:79-109` |
| "web_search up to 44 results" | `max_results=3` default, no pagination | `web_search.py:46` |
| TTS "piper/kokoro/edge_tts/openrouter" | 7 register; 3 reachable on voice path | `config_swap.py:124`, `dashboard.py:721` |
| "tinkerclaw forwards usage" | no `get_last_usage`; receipts emit `model="llm"` | `base.py:241-255`, `tinkerclaw_llm.py` |
| Local default `ministral-3:3b` (ARCH) / LFM2.5-VL (gauntlet) / Granite (config) | three docs, three answers; repo = `lmstudio_model:"default"` | `ARCHITECTURE.md`, `config.yaml` |
| vmode=3 model `MiniMax-M2.5` | live runs M2.7; M2.5 endpoint 404s | `config.py:156`, MEMORY.md |
| Router fleet (deepseek-v4/gemini-3/opus-4.7/gpt-5.5) | `fleet: []`; unreleased models | `config.yaml:89` |
| ARCHITECTURE "four modes (0/1/2/3)" | six tiers (0-5) | `config.h:53-58` |
| NVS table `vmode 0-4`, no `wake_src` key | ships 0-5; `wake_src` is the central wake control | `settings.c`, `debug_server_mode.c:58` |
| Dashboard "OTA tab uploads .bin / sets version" | no upload endpoint; check/apply only | `dashboard.py:974-1016`, `synthesize.py` |
| "62+ REST endpoints" | ~70+ registered | grep `app.router.add_` |
| "OpenRouter 35 models" | 45 caps / 47 pricing | OpenRouter registries |
| Scheduler `recurrence` / `handle_snooze` wired | persisted-but-inert / unwired | `models.py:57`, `manager.py:516` |
| `surfaces_scheduler_init.py` "boot replay reads `list_due(now)`" | manager uses `list_pending()` | `manager.py:114` |
| `widget.h` "v1 = LIVE only" | all six widget types shipped | `widget.h:6` |
| `PLAN-widget-platform.md` lists `widget_store.h` + `test_widget_store.c` | neither exists | tree |
| ext_pcm wake "live-verified" | verified on old IP/UART transport, pre-USB pivot (#621) | re-verify needed |
| `dictation_notes.{c,h}` (W5) vs `voice.c` line cites | recorder/queue moved out of `voice.c` post-snapshot | `e4f00bb`/`6fd5c8c` |
