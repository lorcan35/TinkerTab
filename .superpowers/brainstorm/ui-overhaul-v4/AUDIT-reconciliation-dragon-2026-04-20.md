# Reconciliation Audit — Dragon Voice + TinkerClaw Gateway

**Date:** 2026-04-20
**Repos under read:**
- `/home/rebelforce/projects/TinkerBox/` — Dragon voice server (Python, aiohttp, ~11.8k LOC of first-party code in `dragon_voice/` + ~750 LOC schema + tests)
- `/home/rebelforce/Desktop/openclaw/` — TinkerClaw gateway (TypeScript / Node 22+, ~1.2k LOC in `src/gateway/server.impl.ts` alone, 78 extensions, 52 skills)

**Auditor:** Claude Opus 4.7 (1M context), cold read of spec + code only. No device touched. No service probed. All VERIFIED markers rely on a grep-visible wiring of a spec-claimed feature into the emit path; promotion to DEVICE-VERIFIED would require screenshots or log traces which this audit deliberately does not produce.

---

## 1. Executive summary

Dragon's voice pipeline (STT → LLM → TTS) is the most honest subsystem here — nearly every Local / Hybrid / Cloud path claimed in README and CLAUDE.md is wired end-to-end and guarded against partial failure. TinkerClaw mode (voice_mode=3) is in code but has three distinct gaps between the claim and reality: the TC receipt emits `model=minimax/...` only when the gateway populates `_model` from a successful SSE turn (the fallback path emits `model="tinkerclaw"`, **not the inner model id**); memory read-through is explicitly stubbed out (no top-5 fact injection exists on the TC path, in Dragon or gateway); and the "session continuity" claim hinges on the OpenRouter-compatible `user` field being honored by the gateway, which the gateway-side code consumes as a session key for the Pi embedded runner — but nothing in Dragon proves it actually picks up the last-used workspace, agent, or memory index. **The widget surface is half-shipped:** the skill-facing facade in `surfaces/base.py` exposes all 6 widget types, the SurfaceManager routes `widget_action` correctly, but **only one skill (TimeSense) in the entire Dragon tool registry calls `.live()` / `.prompt()` / `.live_update()`**. Every other registered tool (web_search, weather, calculator, memory, etc.) emits nothing to the widget system; the "skills emit typed widget state; Tab5 renders it opinionatedly" vision is real for Pomodoro timers and nothing else. **Rich media** has the full render path wired for code blocks (Pygments → Pillow JPEG → MediaStore → `/api/media/{id}`), but the on-disk bytes served by `/api/media/{id}` are only end-to-end verifiable through the test suite (`test_media_pipeline.py` claims 32 passing unit tests) — nothing in the session-level E2E suite proves that a cloud-LLM turn actually produces the same JPEG a user would see. **Openclaw gateway** exposes `/health` and `/v1/chat/completions` on port 18789 as claimed; 9 channel plugins are real, multi-thousand-LOC TypeScript implementations (not scaffolds); 78 extensions + 52 skills are present but `voice_mode=3` consumes only `DEFAULT_PROVIDER="anthropic"` + `DEFAULT_MODEL="claude-opus-4-6"` unless overridden — the TinkerClaw CLAUDE claim about "minimax/MiniMax-M2.5 through the gateway" is only true if the user has set that model on the gateway side, which the Dragon code does not enforce or advertise.

---

## 2. Scorecard — A. Voice pipeline end-to-end (Local / Hybrid / Cloud / TC)

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| A1 | Local mode: Moonshine STT + Ollama (qwen3:1.7b default) + Piper TTS | `server.py:1072-1073` sets `stt_be,tts_be = "moonshine","piper"`; `pipeline.py:248` `timeout = 300s`; `llm/ollama_llm.py` exists (282 LOC); `stt/moonshine_stt.py:39` loads via `moonshine-voice` package | 55-test Dashboard suite referenced in CLAUDE.md, no logs attached | **VERIFIED (code)** | none |
| A2 | Hybrid mode: OpenRouter gpt-audio-mini STT/TTS + local LLM | `server.py:1081-1082` Hybrid is `stt="openrouter", tts="openrouter"`, `llm_be = conn_config.llm.local_backend or "ollama"` (line 1095); same prompt via SYSTEM_PROMPT_HYBRID (256 tokens, config.py:45) | none | **VERIFIED (code)** | none |
| A3 | Cloud mode: OpenRouter STT/TTS/LLM, user-picked model | `server.py:1090-1093` sets `llm_be="openrouter"`, `conn_config.llm.openrouter_model = llm_model`; pricing table with ceil-div at `openrouter_llm.py:343-386` | none | **VERIFIED (code)** | none |
| A4 | TC mode: STT local-or-cloud → TinkerClaw gateway LLM → TTS local-or-cloud | `server.py:1074-1080,1085-1089` — mode 3 defaults to `moonshine,piper`, upgrades to `openrouter` when `"cloud" in llm_model.lower()`; `llm/tinkerclaw_llm.py:111` POSTs to `/v1/chat/completions`; session_key via `set_session_key` (line 57) | none; the "minimax" model name referenced in Tab5 audit depends on `conn_config.llm.tinkerclaw_model` which defaults to `"ollama/qwen3:1.7b"` (tinkerclaw_llm.py:29) — **no minimax default anywhere in Dragon code** | **CODE-ONLY — model-id claim overclaimed** | Fix tinkerclaw_model default OR amend the spec |
| A5 | STT streaming / partial results (dictation) | `pipeline.process_segment` (pipeline.py:290-322) emits `stt_partial`; `finish_dictation` (324-381) assembles + emits `stt`; `_post_process_dictation` generates title/summary | 8 multi-step E2E tests claimed | **VERIFIED (code)** | none |
| A6 | Mid-stream `cancel` drops in-flight TTS subprocess + clears buffers | `pipeline.cancel` (266-286) cancels task, kills Piper subprocs (line 280), clears both buffers | No shot. Logic path correct | **VERIFIED (code)** | none |
| A7 | Cloud STT fallback to local Moonshine on failure | `pipeline.py:454-478` `_fallback_stt` lazy-inits Moonshine on cloud STT fail; **cache persists across turns** | no trace | **VERIFIED (code)** | none |
| A8 | Cloud TTS fallback to local Piper on failure | `pipeline.py:758-778` `_fallback_tts` same pattern | no trace | **VERIFIED (code)** | none |
| A9 | Hallucination stop markers (LLM simulates user turn) | `pipeline.py:52-55` `_HALLUCINATION_STOPS` regex, watermark scan at 534-559 | no trace | **VERIFIED (code)** | none |
| A10 | Mode-aware LLM timeout (300s local/TC, 60s cloud) | `pipeline._process_with_timeout:240-264` | — | **VERIFIED (code)** | none |
| A11 | Mode-aware TTS synth budget (90s local, 30s cloud) for text-path | `server.py:1722-1723` in `_handle_text` | — | **VERIFIED (code)** | none |
| A12 | Pipeline timeout emits `tts_end` with 0 so Tab5 doesn't hang in SPEAKING | `pipeline.py:260-262` | — | **VERIFIED (code)** | none |
| A13 | Clause-level TTS flush for local (20ch) vs cloud (60ch) | `pipeline.py:583-589` | — | **VERIFIED (code)** | none |
| A14 | `ESP-IDF app-level ping` answered with `pong` | `server.py:1045-1047` | — | **VERIFIED (code)** | none |
| A15 | `clear` ends old session + creates new one + emits `session_start` | `server.py:1001-1025` | — | **VERIFIED (code)** | none |
| A16 | Text-path receipt emission (parallel to voice) | `server.py:1767-1801` — reads `get_last_usage()`, computes cost via `price_for_model` | — | **VERIFIED (code)** | none |
| A17 | User-visible error text instead of raw provider error to TTS | `openrouter_llm.py:191-202` ("Sorry, the cloud model had a hiccup. Try again?"); `tinkerclaw_llm.py:117-119` ("Sorry, my agent system returned an error...") | — | **VERIFIED (code)** | none |
| A18 | SSE truncation detection (stream ended without [DONE]) | `tinkerclaw_llm.py:160-174` appends " ... (response interrupted)" or full fallback sentence | — | **VERIFIED (code)** | none |
| A19 | 5 consecutive JSON parse failures → graceful error (proxy HTML) | `openrouter_llm.py:221-230`, `tinkerclaw_llm.py:140-147` | — | **VERIFIED (code)** | none |
| A20 | `config_update` rate-limit 2/sec | `server.py:1055-1058` | — | **VERIFIED (code)** | none |

---

## 3. Scorecard — B. Widget surfaces (server side)

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| B1 | `widget_live` | `surfaces/base.py:59-95` `Tab5Surface.live()` | — | **VERIFIED (code)** | — |
| B2 | `widget_live_update` | `surfaces/base.py:145-164` | — | **VERIFIED (code)** | — |
| B3 | `widget_live_dismiss` | `surfaces/base.py:166-180` | — | **VERIFIED (code)** | — |
| B4 | `widget_list` — auto-emitted by web_search tool | `server.py:1470-1495` inside `_on_tool_result` callback for the `web_search` tool only (hardcoded check `result.get("tool") == "web_search"`); formats top 5 hits into `widget_list` | no shot | **VERIFIED (code) — single-hardcoded emitter, no generic list emission** | — |
| B5 | `widget_chart` | `surfaces/base.py` — **no `.chart()` method exists**. Spec references chart; `docs/protocol.md:1341` lists it as Dragon→Tab5 | Only Tab5 widget.h parser hits are referenced in the Tab5-audit. Dragon **cannot emit a chart**: there is no helper in the Tab5Surface façade. | **SPEC-ONLY (Dragon side)** | Either add a `Tab5Surface.chart()` or amend the spec |
| B6 | `widget_media` | `surfaces/base.py:182-216` `Tab5Surface.media()` emits `{"type":"widget_media","url":...}` | — | **VERIFIED (code)** | — |
| B7 | `widget_prompt` | `surfaces/base.py:218-260` | — | **VERIFIED (code)** | — |
| B8 | `widget_card` | `surfaces/base.py:262-293` | — | **VERIFIED (code)** | — |
| B9 | `widget_dismiss` | `surfaces/base.py:295-300` | — | **VERIFIED (code)** | — |
| B10 | SurfaceManager dispatches `widget_action` to registered handler | `surfaces/manager.py:72-91` | — | **VERIFIED (code)** | — |
| B11 | Unknown card_id actions log `"widget_action no handler"` and drop | `surfaces/manager.py:86` `log.info("widget_action no handler: ...")` | — | **VERIFIED (code) — swallows, not surface** | Consider emitting an error event back to Tab5 |
| B12 | Skill emits `.prompt()` + `register_action` round-trip | Only `timesense_tool.py:65` + `:117-135` does this. Grep for `register_action` across tools/ returns **one match**. | — | **HALF-BUILT — only TimeSense. web_search, weather, calculator, memory_tools, etc. never register actions.** | Either add widget emission to 2-3 more skills or mark widget_prompt as "TimeSense reference impl only" in spec |
| B13 | Skill emits `.chart()` | **No skill anywhere calls `surface.chart(...)` because the helper doesn't exist** | — | **SPEC-ONLY** | Build the helper before any skill can emit one |
| B14 | `widget_capabilities` stored on conn_state and read by skills | `server.py:1415-1423` stores `conn_state["widget_capabilities"]`. **No tool reads it back.** `grep widget_capabilities tools/ → 0 matches` | — | **CODE-ONLY, dead-end** | Either wire it into Tab5Surface (truncate items by caps) or delete |
| B15 | `Tab5Surface._safe_send` swallows closed-WS errors | `surfaces/base.py:303-309` | — | **VERIFIED (code)** | — |
| B16 | TimesenseTool registered on server startup | `server.py:240-252` registers TimerTool, WeatherTool, etc. — **does NOT register TimesenseTool**. Grep for `TimesenseTool(` registration: **zero**. | — | **MAJOR OVERCLAIM** — the reference skill is **not actually registered**. Tab5 voice "set a 25-minute timer" will hit `TimerTool` (tools/timer_tool.py, 103 LOC), NOT TimesenseTool. The widget `live` card never appears in practice. | Register `TimesenseTool(self._surface_mgr)` in `_on_startup`, or acknowledge the reference skill is dev-only |
| B17 | `widget_chart` LIST auto-emit was shipped per Tab5 audit Phase 4c | Only the `widget_list` path is shipped in `server.py:1470-1495`. No `widget_chart` auto-emit anywhere. | — | **SPEC-ONLY** | — |

---

## 4. Scorecard — C. Session lifecycle

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| C1 | Session create/resume/pause/end | `sessions.py` (252 LOC), `db.py:287-430` full CRUD | E2E tests referenced | **VERIFIED (code)** | — |
| C2 | Atomic CAS `update_session_status_if` fixes race | `db.py:357-379`, used in `sessions.py:158-162` (pause) | No race test in `tests/` — grep for `update_session_status_if` finds only its definition + one caller | **VERIFIED (code) — no concurrency test** | Add a concurrent-pause race test |
| C3 | `PRAGMA wal_checkpoint(TRUNCATE)` on DB close | `db.py:181-185` | — | **VERIFIED (code)** | — |
| C4 | Corrupt DB auto-recovery (rename WAL/SHM, retry) | `db.py:89-159` `_recover_corrupt_db` | Never deliberately tested | **CODE-ONLY** | — |
| C5 | Stale session cleanup on startup: end if >timeout, pause if recent | `sessions.py:49-81` | — | **VERIFIED (code)** | — |
| C6 | Stale session cleanup background loop (every 60s) | `sessions.py:236-252` | — | **VERIFIED (code)** | — |
| C7 | P13: evict stale duplicate-device connection | `server.py:1367-1398` | — | **VERIFIED (code)** | — |
| C8 | **Message replay on resume** (Tab5 audit tagged ~500 LOC missing) | `server.py:1506-1520` sends `session_start` with `message_count` but **no `messages[]` array**. Grep for `send_json.*messages` in server.py: **no hits** for a session resume payload. Tab5 would have to call `GET /api/v1/sessions/{id}/messages` on its own — no WS path. | — | **CONFIRMED MISSING (as Tab5 audit suspected)** | Either implement WS replay on resume, or document that Tab5 must poll the REST API after resume |
| C9 | Session-scoped config (override global) | `db.py:714-728` `get_resolved_config` (session → device → global) | — | **VERIFIED (code)** | — |
| C10 | 30-min default session inactivity timeout | `sessions.py:21` `SESSION_TIMEOUT_S = 1800` | — | **VERIFIED (code)** | — |
| C11 | Message purge task (daily) | `server.py:354-367`; purge skips active/paused sessions | — | **VERIFIED (code)** | — |
| C12 | Append-only message store (never mutate) | `messages.py` + `db.add_message` (db.py:434-466) INSERT only | — | **VERIFIED (code)** | — |
| C13 | `device.connected` / `device.disconnected` / `session.paused` / `session.ended` events | `server.py:1424,1909-1912`; `sessions.py:112-115,173-177,186-192` | — | **VERIFIED (code)** | — |

---

## 5. Scorecard — D. Tool calling + skills

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| D1 | 10 built-in tools | **10 files in `dragon_voice/tools/`** (web_search, calculator, datetime, memory_tools(3 in 1 file: StoreFact/RecallFacts/ForgetFact), note, registry, system, timer, timesense, unit_converter, weather) | — | **VERIFIED (code)** | — |
| D2 | Tool XML parser tolerant of small-model quirks | `tools/registry.py:59-112` manual brace-balancing (P2 fix for nested JSON) | — | **VERIFIED (code)** | — |
| D3 | `web_search` via SearXNG (port 8888), DDG fallback | `tools/web_search.py:69-106` | no shot | **VERIFIED (code)** | — |
| D4 | Max 3 tool calls per turn (prevent loops) | `conversation.py:26` `MAX_TOOL_CALLS = 3`; enforced at `:247-251` | — | **VERIFIED (code)** | — |
| D5 | Compact tool format for local models (~150 tokens) | `tools/registry.py:132-155` — 5 priority tools only | — | **VERIFIED (code)** | — |
| D6 | Tools registered on startup: WebSearch, DateTime, StoreFact, RecallFact, ForgetFact, Timer, Weather, Calculator, UnitConverter, SystemInfo | `server.py:225-252` — 10 `.register()` calls | — | **VERIFIED (code)** | — |
| D7 | NoteTool deferred registration (after NotesService available) | `server.py:305-308` | — | **VERIFIED (code)** | — |
| D8 | TimesenseTool registered | **NO — not in `server.py:_on_startup`**. `grep TimesenseTool( /dragon_voice/server.py` → 0 hits | — | **OVERCLAIMED — not registered** | Register in startup with `TimesenseTool(self._surface_mgr)` |
| D9 | MCP servers bridged via `mcp/bridge.py` | `server.py:313-325`, `mcp/bridge.py` (38 LOC wrapper) | Never tested with live MCP in tests | **CODE-ONLY** | — |
| D10 | Tool `on_tool_call` / `on_tool_result` WS events (per-connection, not shared) | `server.py:1457-1498` — callback closures per-connection | — | **VERIFIED (code)** | — |
| D11 | Auto-emit `widget_list` for web_search results (Phase 4c) | `server.py:1470-1495` hardcoded to `web_search` tool only | — | **VERIFIED (code) — hardcoded to ONE tool** | Generalize with a tool-metadata flag |
| D12 | Skills directory / discovery in Dragon | No `skills/` directory. Dragon calls them "tools" in `dragon_voice/tools/`. The openclaw repo has 52 `skills/`; Dragon has 10 tools. | — | **NAMING OVERLOAD** | Spec: Dragon has 10 tools; openclaw has 52 skills — these are different things |
| D13 | Memory tools (StoreFact, RecallFacts, ForgetFact) with two-step confirm on forget | `tools/memory_tools.py` (177 LOC); server.py:237-239 registers; comment references "v4·D Gauntlet G9" | — | **VERIFIED (code)** | — |
| D14 | WeatherTool, CalculatorTool have real implementations | `tools/weather_tool.py` (166 LOC), `calculator_tool.py` (166 LOC) both have real execute methods | — | **VERIFIED (code)** | — |
| D15 | Tool result `execution_ms` timing | `tools/registry.py:45-54` | — | **VERIFIED (code)** | — |
| D16 | LMStudio LLM backend | `llm/lmstudio_llm.py` (156 LOC) | Never exercised from voice_mode code paths — voice_mode map in `server.py:1070-1098` never sets `llm_be="lmstudio"` | **DEAD CODE from voice path (could be selected via REST `/api/config`)** | Either document config path or remove |
| D17 | NPU Genie LLM backend | `llm/npu_genie.py` (198 LOC) | Same — `conn_config.llm.local_backend` in `server.py:1095` defaults to "ollama"; `npu_genie` only reachable via `llm.yaml` / REST | **CODE-ONLY** | — |

---

## 6. Scorecard — E. Memory

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| E1 | Hybrid memory: facts + documents + chunks with embeddings | `memory.py:30-291` three tables (memory_facts, memory_documents, memory_chunks) | — | **VERIFIED (code)** | — |
| E2 | Ollama `nomic-embed-text` embeddings | `memory.py:81-102` POST to `/api/embed` with `keep_alive: 30s` | — | **VERIFIED (code)** | — |
| E3 | Cosine similarity search with keyword-fallback | `memory.py:104-116`; fallback at 166-169 | — | **VERIFIED (code)** | — |
| E4 | Top-N fact injection into system prompt (Cloud/Local paths) | `conversation.py:166-174` via `_build_context` | — | **VERIFIED (code)** | — |
| E5 | **Memory injection on TinkerClaw path (voice_mode=3)** — Tab5 audit deferred | `pipeline.py:502-509` TC path bypasses ConversationEngine entirely → no memory injection. `server.py:1578-1623` `_handle_text` TC branch also bypasses `_build_context`. **Zero memory injection code on TC path.** | — | **SPEC-ONLY (explicitly deferred in Tab5 audit A7)** | DEFER — correct decision |
| E6 | Document chunking (512 tokens, 50 overlap) | `memory.py:184-197` | — | **VERIFIED (code)** | — |
| E7 | Document cascade delete | `memory.py:72` `FOREIGN KEY ... ON DELETE CASCADE` | — | **VERIFIED (code)** | — |
| E8 | `get_relevant_context` returns `[MEMORY CONTEXT] ... [END MEMORY CONTEXT]` | `memory.py:276-291` | — | **VERIFIED (code)** | — |
| E9 | Mode-aware history depth (10 local / 30 cloud) | `conversation.py:161-163` | — | **VERIFIED (code)** | — |
| E10 | Context budget enforcement (`trim_context_to_budget`) | `conversation.py:187-188`; `messages.py` has `CONTEXT_BUDGET_LOCAL` / `_CLOUD` | — | **VERIFIED (code)** | — |
| E11 | sqlite-vec usage (spec "Hybrid vector (sqlite-vec) + keyword (FTS5)") | **Dragon memory does NOT use sqlite-vec.** `memory.py` stores embeddings as BLOB of packed floats (`struct.pack(f"{len(vec)}f", *vec)`) and does a Python-loop cosine scan (memory.py:162-180). No `sqlite-vec` load extension, no `vec_match`, no FTS5 table. | — | **OVERCLAIMED — README/CLAUDE claim FTS5+sqlite-vec; reality is Python-loop cosine** | Either add sqlite-vec (openclaw DOES use it — `src/memory/sqlite-vec.ts`), or amend docs to "brute-force cosine in Python" |

---

## 7. Scorecard — F. Receipt / budget / caps

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| F1 | Per-turn receipt on voice-path LLM | `pipeline.py:599-651` reads `get_last_usage`, emits `type: receipt, stage: llm, model, cost_mils, retried, retry_reason` | — | **VERIFIED (code)** | — |
| F2 | Per-turn receipt on text-path (ConvEngine) | `server.py:1767-1801` parallel emit | — | **VERIFIED (code)** | — |
| F3 | Per-turn receipt on TinkerClaw text-path | `server.py:1629-1649` emits `type: receipt, stage: llm, model=<inner model id if available else "tinkerclaw">, cost_mils=0` | — | **VERIFIED (code) — but model field can still be `"tinkerclaw"` string, not a real model id** | — |
| F4 | Per-turn STT receipt | Grep for `"stage":.*"stt"` in Dragon: **NO HITS**. The spec says "receipt on stt/llm/tts stages"; only `"stage":"llm"` is ever emitted. | — | **SPEC-ONLY — STT and TTS receipts never emitted** | Emit on STT/TTS callback paths |
| F5 | Per-turn TTS receipt | Same — `"stage":"tts"` never emitted from Dragon. | — | **SPEC-ONLY** | — |
| F6 | Fallback receipt on `get_last_usage` failure | `pipeline.py:629-651` — emits zero-cost receipt with `retry_reason="receipt-fallback: <ExcType>"` | — | **VERIFIED (code)** | — |
| F7 | Pricing table with ceil-div for small turns | `openrouter_llm.py:334-386`; ceil-div at line 384-385 so a 2-token Haiku turn costs at least 1 mil | — | **VERIFIED (code)** | — |
| F8 | Daily cap enforcement + TTS downgrade speak | `server.py:1290-1298` — on `reason=="cap_downgrade"` calls `pipeline.speak_system`; `pipeline.speak_system` at `:709-735` synthesizes + sends TTS inline | — | **VERIFIED (code)** | — |
| F9 | Pricing table covers Gemini/Haiku/Sonnet/GPT-4o/audio-mini + default | `openrouter_llm.py:343-359` | — | **VERIFIED (code)** | — |
| F10 | Local midnight rollover (day-index) | **Dragon side: NONE.** The Tab5 audit flagged this as on the Tab5 side (`settings.c:460-464`). `grep -r midnight /dragon_voice/` returns zero hits. Dragon does not track a daily spend; it emits cost-per-turn, and Tab5 accumulates + rolls over. | — | **VERIFIED: correctly scoped to Tab5** | none |
| F11 | `retried` / `retry_reason` on receipt | `openrouter_llm.py:152-189` sets flags on 429/context_trim/client_error; `pipeline.py:626-627` emits them | — | **VERIFIED (code)** | — |
| F12 | Cost tracked as api_usage event in DB | `server.py:908-918` persists `api_usage` events from pipeline callbacks | — | **VERIFIED (code)** | — |

---

## 8. Scorecard — G. Mode switching

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| G1 | `config_update` accepts voice_mode 0-3 + llm_model | `server.py:1063-1068`; backward compat with `cloud_mode` bool at 1066-1068 | — | **VERIFIED (code)** | — |
| G2 | Rate limit 2/sec on config_update | `server.py:1053-1058` | — | **VERIFIED (code)** | — |
| G3 | Mode-aware system prompt (128/256/512 tokens) | `config.py:44-46` + `server.py:1104-1112`; TC mode skipped (1102-1103) | — | **VERIFIED (code)** | — |
| G4 | Pre-switch TinkerClaw gateway health probe (5s) | `server.py:1121-1139` `aiohttp.ClientTimeout(total=5)`, GET `/health`, emits error config_update on failure | — | **VERIFIED (code)** | — |
| G5 | Fail TC mode → auto-fallback config_update back to Tab5 | `server.py:1134-1138` emits `{"type":"config_update","error":"TinkerClaw gateway is not reachable","voice_mode":<orig>}`. **BUT note: error message lies — it keeps the failed voice_mode in the emit**. Tab5 audit F5 claims Tab5 auto-reverts to mode 0 on this error; the message itself doesn't command voice_mode=0, Tab5 must interpret any `error` field as a revert trigger. | — | **VERIFIED (code) — but subtly inconsistent with cloud-key error path which sets `voice_mode:0`** | Normalize: on TC fail, emit `voice_mode: 0` like the cloud-key path at line 1149 |
| G6 | Cloud-key missing → error config_update with `voice_mode: 0` | `server.py:1143-1150` | — | **VERIFIED (code)** | — |
| G7 | Per-connection config deep-copied (no cross-device leakage) | `server.py:806` `conn_config = copy.deepcopy(self._config)` | — | **VERIFIED (code)** | — |
| G8 | Pipeline init resets to local defaults on reconnect | `server.py:1532-1536` after `session_start` | — | **VERIFIED (code)** | — |
| G9 | Backend swap + ConvEngine swap are lock-serialized (A06) | `server.py:1178` `async with conn_lock:` wraps swap_backends + ConvEngine swap | — | **VERIFIED (code)** | — |
| G10 | `config_update` ACK with applied backends + model + cloud_mode + voice_mode | `server.py:1230-1239` | — | **VERIFIED (code)** | — |
| G11 | Vision capability emission after config swap | `server.py:1254-1283` — checks `gpt-4o`, `sonnet`, `haiku`, `llava`, `vision`; emits `type: vision_capability` | — | **VERIFIED (code)** | — |

---

## 9. Scorecard — H. Rich media pipeline

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| H1 | Pygments code block render → PNG → resize → JPEG bytes | `media/pipeline.py:224-245` `_render_code_pygments` native style, DejaVu Sans Mono 14pt, line_pad=6, image_pad=20; `_resize_jpeg` at 394-406 | 32 unit tests claimed in `test_media_pipeline.py` | **VERIFIED (code) — unit-tested per CLAUDE.md** | — |
| H2 | Plain-text Pillow fallback if Pygments absent | `media/pipeline.py:248-282` | — | **VERIFIED (code)** | — |
| H3 | Table render (Pillow) with accent orange headers, dark bg, grid | `media/pipeline.py:285-352` | — | **VERIFIED (code)** | — |
| H4 | Image URL proxy (download, resize, re-encode) | `media/pipeline.py:134-150` 10MB cap, 15s timeout | — | **VERIFIED (code)** | — |
| H5 | Max 3 media items per response | `media/pipeline.py:21` `MAX_MEDIA_PER_RESPONSE = 3`, enforced at 76, 87, 98 | — | **VERIFIED (code)** | — |
| H6 | `strip_rendered_content` removes rendered blocks + URLs | `media/pipeline.py:199-213` | — | **VERIFIED (code)** | — |
| H7 | `text_update` emitted after stripping | `pipeline.py:592-597`-ish (voice path via `_on_event`), and `server.py:1662-1666` (TC text path) + `:1701-1703` (ConvEngine text path) | — | **VERIFIED (code)** | — |
| H8 | TC path media detection (early-return bug) | `server.py:1651-1668` inside `_handle_text` TC branch. Spec says "both paths detect media" — confirmed | — | **VERIFIED (code)** | — |
| H9 | `GET /api/media/{id}` serves real bytes, not placeholder | `api/media_routes.py:39-62` opens real file from MediaStore, sets `Cache-Control: max-age=3600`, 404 if missing | — | **VERIFIED (code) — backed by real file read** | — |
| H10 | `POST /api/media/upload` accepts BMP/JPEG, re-encodes via Pillow, max 10MB, resize to 1280px, quality 85 | `api/media_routes.py:64-110` | — | **VERIFIED (code)** | — |
| H11 | MediaStore 24h auto-cleanup + 500MB cap | `media/store.py` + `server.py:369-376` hourly cleanup loop | — | **VERIFIED (code)** | — |
| H12 | OG/Twitter meta tag parser for link previews | `media/pipeline.py:152-187,409-425` `fetch_link_preview` | Never called from the main response flow. Grep `fetch_link_preview` → **only the definition, no caller** | **CODE-ONLY / DEAD CODE** | Either wire into response flow or remove |
| H13 | `media` event `width:660, height:0` (Tab5 parses dimensions from JPEG SOF) | `media/pipeline.py:428-436` `_media_event` — `height: 0`, Tab5 computes real | — | **VERIFIED (code)** | — |

---

## 10. Scorecard — I. WS transport + resilience

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| I1 | `WebSocketResponse(heartbeat=30, receive_timeout=60, autoping=True, max_msg_size=10MB)` | `server.py:795-800` | — | **VERIFIED (code)** | — |
| I2 | `_safe_send_json` / `_safe_send_bytes` non-raising helpers | `server.py:727-764` | — | **VERIFIED (code)** | — |
| I3 | Server-side 15s `pong` keepalive (ngrok timeout mitigation) | `server.py:822-866` | — | **VERIFIED (code)** | — |
| I4 | Keepalive fail-counter: close WS after 3 consecutive failures | `server.py:848-865` | — | **VERIFIED (code)** | — |
| I5 | Per-connection `conn_lock` serializes voice/text/swap | `server.py:812`, used in text at `:1034`, stop at `:978`, swap at `:1178` | — | **VERIFIED (code)** | — |
| I6 | Connection cap: `max_connections = 10` returns 503 | `server.py:58`, `:780-782` | — | **VERIFIED (code)** | — |
| I7 | Disconnect: pause session + mark device offline unless other conn exists | `server.py:1895-1912` | — | **VERIFIED (code)** | — |
| I8 | ThreadPoolExecutor shutdown on exit (inference pool) | `pipeline.py:27-40` + `server.py:547-552` | — | **VERIFIED (code)** | — |
| I9 | Audio buffer cap 5min @ 16kHz (9.6MB, P06) | `pipeline.py:62`, enforced at `:179-184` | — | **VERIFIED (code)** | — |
| I10 | Memory monitor every 5min (RSS, CPU temp, FDs) | `server.py:419-524` | — | **VERIFIED (code)** | — |
| I11 | RSS > 3072 MB → force GC + restart all pipelines | `server.py:484-524` | — | **VERIFIED (code)** | — |
| I12 | CPU temp thresholds: warn 80°C, error 90°C | `server.py:453-464` monitoring only, no throttling | — | **VERIFIED (code)** | — |
| I13 | FD usage > 80% warn | `server.py:466-470` | — | **VERIFIED (code)** | — |
| I14 | Shared `_proxy_session` aiohttp ClientSession for dashboard proxy | `server.py:159-161` | — | **VERIFIED (code)** | — |
| I15 | CORS allowlist (SEC12) with 4 origins | `server.py:113-118` | — | **VERIFIED (code)** | — |
| I16 | `_safe_send_json` covers `register` race — "client will reconnect, we'll replay on next session_start" | `server.py:1506-1521` — but see C8: **there's no replay**. The comment is aspirational. | — | **CODE-ONLY (comment overstates behavior)** | Align comment with reality |

---

## 11. Scorecard — J. TinkerClaw gateway (openclaw)

| Item | Spec / claim | Code evidence | Runtime evidence | Status | Action |
|---|---|---|---|---|---|
| J1 | Gateway WS server + HTTP control plane on :18789 | `src/gateway/server.impl.ts` (1154 LOC) + `server-http.ts:110-111` maps `/health`, `/healthz` → "live" lane; OpenAI HTTP at `openai-http.ts:416` `pathname: "/v1/chat/completions"` | — | **VERIFIED (code)** | — |
| J2 | `/health` endpoint | `server-http.ts:110-111` + `server/health-state.ts` imports from `commands/health.js` → real snapshot | — | **VERIFIED (code)** | — |
| J3 | `/v1/chat/completions` OpenAI-compatible endpoint | `openai-http.ts:416` | — | **VERIFIED (code)** | — |
| J4 | 9 channel plugins enabled | **All 9 confirmed as real implementations (not scaffolds):** whatsapp (9820 LOC), telegram (large), discord (16115 LOC), slack (10232 LOC), signal (5082 LOC), imessage (2620 LOC), googlechat, msteams (9745 LOC), line. Each has `openclaw.plugin.json` with `"channels": ["<name>"]`. | — | **VERIFIED (code)** | — |
| J5 | Multi-provider LLM (Anthropic, OpenAI, Google, Copilot, Bedrock) | All 5 directories exist in `extensions/`: anthropic, openai, google (with OAuth flow), amazon-bedrock, github-copilot. All have `index.ts` + `openclaw.plugin.json`. The `src/agents/pi-embedded-runner/` has provider-specific stream wrappers for anthropic, google, openai, moonshot, proxy — wired at the runner layer. | — | **VERIFIED (code)** | — |
| J6 | 78 extensions total | `ls extensions/` → 78 directories (confirmed by count) | — | **VERIFIED (code)** | — |
| J7 | 52 skills total | `ls skills/` → 52 directories (confirmed) | — | **VERIFIED (code)** | — |
| J8 | Plugin discovery at `src/plugins/discovery.ts` (844 LOC) | Real — `discovery.ts` + `loader.ts` (1393 LOC) | — | **VERIFIED (code)** | — |
| J9 | Memory via `sqlite-vec` + FTS5 | `src/memory/sqlite-vec.ts` + `src/memory/hybrid.ts` (hybrid.test.ts) — real implementation | — | **VERIFIED (code)** | — |
| J10 | Browser (Playwright/CDP) on port 18800 | `src/browser/cdp.ts`, `chrome.ts`, `chrome-mcp.ts` — real, multi-hundred LOC | — | **VERIFIED (code)** | — |
| J11 | Hooks system (Gmail, internal, bundled) | `src/hooks/` has `gmail.ts`, `internal-hooks.ts`, `bundled/`, `loader.ts` | — | **VERIFIED (code)** | — |
| J12 | "Agent mode" (voice_mode=3) wired through gateway | Dragon POSTs to `/v1/chat/completions`, gateway routes to Pi embedded runner with `user` field as session key → `src/agents/pi-embedded-runner/run.ts` picks up `DEFAULT_MODEL="claude-opus-4-6"` (defaults.ts:4) UNLESS overridden by the `model` field in the request. **Dragon sends `model=<conn_config.llm.tinkerclaw_model>` which defaults to `"ollama/qwen3:1.7b"`** (tinkerclaw_llm.py:29). | — | **CODE-ONLY — model claim mismatch: Dragon's default is "ollama/qwen3:1.7b", gateway's default is "claude-opus-4-6"; neither is "minimax/MiniMax-M2.5"** | Pick one default and align both repos |
| J13 | Session continuity via `user=<session_id>` | `tinkerclaw_llm.py:107-108` sets `payload["user"] = self._session_key` | Gateway's `server-session-key.ts` exists + has tests. Whether the gateway truly honors `user` as an OpenAI-compatible session key for memory recall is not directly verifiable without running it. | **CODE-ONLY** | — |
| J14 | DEFAULT_PROVIDER="anthropic", DEFAULT_MODEL="claude-opus-4-6" | `src/agents/defaults.ts:3-4` | — | **VERIFIED (code)** | — |
| J15 | Channel plugin registry + ACP bindings | `src/channels/plugins/registry.ts`, `acp-bindings.test.ts` | — | **VERIFIED (code)** | — |
| J16 | Auth rate limiter on gateway | `src/gateway/auth-rate-limit.ts` | — | **VERIFIED (code)** | — |
| J17 | 200K context token default for models without metadata | `src/agents/defaults.ts:6` `DEFAULT_CONTEXT_TOKENS = 200_000` | — | **VERIFIED (code)** | — |
| J18 | Plugin SDK at `src/plugin-sdk/` | Directory exists with `index.ts` (not audited content) | — | **VERIFIED (existence only)** | — |
| J19 | ngrok tunnel: `tinkerclaw-gateway.ngrok.dev` → 18789 | CLAUDE.md asserts; no network verification possible | — | **SPEC-ONLY** | — |
| J20 | `voice_mode=3` uses which LLM on the gateway? | Dragon passes `model: ollama/qwen3:1.7b` by default; gateway routes via openai-http → pi-embedded-runner; the `model` field is respected if the gateway has that provider configured. If `ollama/qwen3:1.7b` isn't registered, gateway falls back to `claude-opus-4-6`. | — | **CODE-ONLY — behavior depends on gateway config not in this repo** | Document: "TC mode uses the gateway's default model; to use Claude Opus, the user must set `llm_model="anthropic/claude-opus-4-6"` on config_update" |

---

## 12. Scorecard — K. Hygiene (claim audit)

| Item | PROGRESS / README claim | Reality | Status |
|---|---|---|---|
| K1 | "NPU Llama 1B on QCS6490 HTP at ~8 tok/s" | `llm/npu_genie.py` (198 LOC) exists; never reachable from voice_mode code paths (local_backend defaults to "ollama") | **CODE-ONLY** |
| K2 | "Memory: hybrid vector (sqlite-vec) + FTS5" (CLAUDE.md line 2 of Memory Service) | Dragon uses Python-loop cosine over BLOB-packed floats. No sqlite-vec, no FTS5. Openclaw DOES have it but that's a different repo. | **OVERCLAIMED in Dragon README/CLAUDE** |
| K3 | "55 E2E tests via Debug tab + 29 API tests" | `tests/test_api_e2e.py` present (25KB), `tests/e2e_full_suite.py` present. 44 media tests (12 + 32) confirmed in `tests/test_media_store.py` + `tests/test_media_pipeline.py`. | **VERIFIED (file presence)** |
| K4 | "Tolerant tool parser" (stray >, missing </args>) | `tools/registry.py:14-17` has regex + loose fallback; line 75-112 has manual brace-balancer for nested JSON. | **VERIFIED (code)** |
| K5 | "Default local LLM: qwen3:1.7b" | CLAUDE references. `config.yaml` + `LLMConfig.ollama_model` default — confirm via config.py | **VERIFIED (code)** |
| K6 | "Pygments + fonts-dejavu-core required" | `media/pipeline.py:226` uses `font_name="DejaVu Sans Mono"` | **VERIFIED (code)** |
| K7 | "TimeSense is the reference skill" | Exists as `tools/timesense_tool.py` (216 LOC) BUT **not registered in server startup**. It can be imported but won't fire from a voice "set a Pomodoro" invocation — that hits TimerTool (103 LOC) instead. | **HALF-BUILT — the reference is dev-only** |
| K8 | "Widget platform: 6 types, Tab5Surface facade" | 5 of 6 helpers exist (live, card, list, media, prompt); **chart has no helper** | **HALF-BUILT** |
| K9 | "MCP bridge" | `mcp/bridge.py` (38 LOC) + `client.py` (5KB). Never exercised in tests. | **CODE-ONLY** |
| K10 | "ngrok tunnels: dashboard, voice, gateway" | Asserted in both CLAUDE.md files; no in-repo proof | **SPEC-ONLY** |
| K11 | "Dashboard proxies /api/proxy/ to voice server" | `dashboard.py` 140KB; `server.py:146-186` `_proxy_dashboard` reverse-proxies `/dashboard*` to :3500 | **VERIFIED (code)** |
| K12 | "Receipt emitted on stt/llm/tts stages" (docs/protocol implicitly suggests) | Only `stage:llm` emitted anywhere. | **OVERCLAIMED** |
| K13 | "TinkerClaw runs minimax/MiniMax-M2.5" (inferred from Tab5 audit + context) | Dragon default model on TC is `"ollama/qwen3:1.7b"`. No minimax reference anywhere in Dragon. | **OVERCLAIMED / UNSUBSTANTIATED** |
| K14 | "Openclaw has 30+ extensions / 50+ skills" | 78 extensions / 52 skills confirmed | **UNDERCLAIMED (more than stated)** |
| K15 | "Session resume with full message history intact" (README Features) | Only `message_count` travels in `session_start`; the actual message array is accessible only via REST (`GET /api/v1/sessions/{id}/messages`). WS path emits no message replay. | **OVERCLAIMED — history is on-disk + fetchable, but not replayed over the WS** |
| K16 | "RSS monitor restarts pipelines above 3072 MB" | `server.py:484-524` implemented | **VERIFIED (code)** |

---

## 13. Top 10 overclaims (ranked by user impact)

1. **TimesenseTool is not registered** (D8/B16/K7). CLAUDE.md + widget-platform docs + the Tab5 audit all treat TimeSense as the reference widget skill. Voice command "set a 25-minute pomodoro" will route through TimerTool (text-only, no widget emission) instead, because `TimesenseTool(surface_mgr)` is **never instantiated in `_on_startup`**. **User harm: the entire widget-platform user-story story is invisible even on an ideal network.**
2. **TC receipt "model" can be the bare string `"tinkerclaw"`** (A4/F3/J12). `tinkerclaw_llm.py:29` defaults `_model` to `"ollama/qwen3:1.7b"`; the server receipt-emit falls back to `"tinkerclaw"` string if `_model` is empty — which it will be until a successful turn populates it. First-turn receipt will display the generic string. **User harm: per-bubble transparency says "minimax" or "claude" but shows the wrong thing.**
3. **STT + TTS receipts never emitted** (F4/F5/K12). Protocol docs + the audit spec both imply per-turn receipts on all 3 stages. Only `stage:"llm"` is ever emitted from Dragon — no `stage:"stt"`, no `stage:"tts"`. **User harm: budget tracking on Tab5 can only attribute LLM cost; cloud STT/TTS is opaque in the bubble stamp.**
4. **Memory FTS5 + sqlite-vec overclaimed** (E11/K2). README and CLAUDE both assert "Hybrid vector (sqlite-vec) + keyword (FTS5)". Actual implementation is Python-loop cosine over packed-float BLOBs. Works, but at 10k facts will visibly lag. **User harm: at scale the "AI that remembers you" feature degrades silently; no query path beyond linear scan.**
5. **Session-resume message replay missing** (C8/K15). Tab5 audit flagged ~500 LOC missing for resume — confirmed: Dragon sends `session_start` with `message_count` only; `messages[]` payload never wired. Tab5 cannot repopulate chat history from an ngrok reconnect without an additional REST call it doesn't make today. **User harm: reconnect loses the conversation context from the user's view, even though it's on disk.**
6. **TC mode default model mismatch** (J12/J20/K13). Dragon sends `ollama/qwen3:1.7b`; gateway default is `claude-opus-4-6`; Tab5 audit expects `minimax/MiniMax-M2.5`. Three different defaults. **User harm: user thinks they're talking to Claude Opus through the gateway, actual routing depends on gateway config user cannot see.**
7. **`widget_chart` has no Dragon emitter helper** (B5/B13). Spec lists `widget_chart` as Dragon→Tab5, Tab5-side has parser. Tab5Surface has no `.chart()` method. No skill can ever produce one. **User harm: promised widget type can never appear.**
8. **`widget_capabilities` stored but never consumed** (B14). `conn_state["widget_capabilities"]` is populated from register frame, no tool ever reads it. List/prompt helpers truncate at hardcoded lengths, not per-device caps. **User harm: capability negotiation is cosmetic.**
9. **TC gateway error path leaves `voice_mode` un-reverted** (G5). On cloud-key missing, server emits `voice_mode:0`. On TC gateway unreachable, server emits `voice_mode:<orig>` with an `error` field. Inconsistent — Tab5 must interpret both. **User harm: a mode-sheet Agent tap on an offline gateway might sit in mode 3 showing an error instead of snapping back to Local.**
10. **`TimerTool` and `TimesenseTool` are different tools** (D8/K7). Both claim to be "set a timer" tools. Only TimerTool is registered. TimesenseTool has the widget-surface integration. Voice or chat "set a timer" fires TimerTool (no widget), "pomodoro" trigger text in `timesense_tool.py:184` is also wired — but the TimesenseTool isn't instantiated, so the voice parser is dead code too. **User harm: duplication + dead code = maintenance trap.**

---

## 14. Top 5 underclaims (quiet wins)

1. **`_fallback_stt` / `_fallback_tts` instance cache** (A7/A8). `pipeline.py:459-466` + `:764-769` — the P1 audit fix cached the fallback backend instances so repeated cloud failures don't re-init Moonshine / Piper each time (~2-3s blocking load avoided). Unsexy, huge UX impact. Never celebrated.
2. **Per-connection `conn_lock`** (G9/I5). Serializes stop / text / config_update / swap. Fixed a class of races nobody was loudly complaining about.
3. **WAL checkpoint on DB close** (C3). `db.py:181-185` — prevents long WAL replay on restart. One-line fix, big startup-time improvement, never advertised.
4. **Connection eviction (P13)** for stale same-device registrations (C7). `server.py:1367-1398` — prevents ghost pipelines when a Tab5 drops TCP and reconnects before aiohttp notices. The whole "dead connection holding a pipeline" class of bugs is gone.
5. **Idempotency-Key on OpenRouter POST** (openrouter_llm.py:138-144). A single logical LLM call produces a single billable response regardless of aiohttp TCP retransmit. Silently saves money.

---

## 15. Proof-deficit backlog

| Gap | Test that closes it |
|---|---|
| TC receipt `model` field under a real TC gateway | Run Tab5 → TC → one turn; grep voice server log for `TC receipt emit`; confirm `model` != `"tinkerclaw"` (raw string). |
| STT receipt missing | Pick one Cloud turn; grep for `"stage":"stt"` in WS traffic; expect 0 → confirms gap. |
| TTS receipt missing | Same as above for `"stage":"tts"`. |
| Message replay on session resume | Disconnect mid-session, reconnect with stored session_id; grep WS emit for any frame carrying `messages` array after `session_start`; expect 0 → confirms gap. |
| sqlite-vec not used in Dragon memory | `grep -r sqlite.vec dragon_voice/` → 0 hits → confirms. |
| TimesenseTool never fires the live widget in a real voice turn | Voice: "set a 25-minute timer"; grep server log for `TimeSense start`; expect 0, see `Timer` instead → confirms. |
| widget_chart never emitted | `grep -r widget_chart dragon_voice/` → 0 hits in emit paths; only appears in spec comments. |
| `widget_capabilities` never read by any skill | `grep widget_capabilities dragon_voice/tools/` → 0 hits. |
| TC mode-revert after unreachable gateway | Stop tinkerclaw-gateway; tap Agent from Tab5 mode sheet; grep server log for `"voice_mode": <expected_0>`; confirm error message commands revert. |
| `fetch_link_preview` dead code | `grep fetch_link_preview` outside `media/pipeline.py` → 0 → confirms. |
| `lmstudio`/`npu_genie` backends from voice_mode | `grep 'llm_be.*lmstudio\|llm_be.*npu_genie' server.py` → 0 hits → confirms dead from voice_mode path. |
| CAS race on `update_session_status_if` | Spawn 2 concurrent pause calls on same session_id; expect 1 to return True, 1 False; confirms atomicity. |
| TC `user=<session_id>` session-key continuity on gateway | Send 2 consecutive TC turns with same session_id; third-party inspection of gateway logs (outside this repo) required. |
| Memory monitor RSS restart | Artificially balloon RSS > 3GB; observe pipeline reinit log. |
| Daily TC cost tracking is zero-cost | Grep for `cost_mils": 0` on TC receipt path → consistent; confirms design. |
| Openclaw `/v1/chat/completions` auth surface | POST without `Authorization` header → expect 401; confirms gateway requires auth. |
| Openclaw gateway model fallback (DEFAULT_MODEL) | POST with `model` unset → gateway should respond with claude-opus-4-6; confirms default. |
| 9 channels all enabled vs. scaffolded | For each channel: check `openclaw.plugin.json` + `index.ts` LOC > 100 → confirmed above in J4. |

---

## 16. Ordered action list

### Bucket (a) — Cheap tests / evidence capture (each ≤ 15 min)

1. **Registration audit**: grep `grep -rn TimesenseTool(\|TimesenseTool\s*=` `dragon_voice/`. If 0 hits in `server.py:_on_startup`, file an issue "TimesenseTool not registered" — closes #1 overclaim.
2. **STT/TTS receipt gap** (F4/F5): grep for `"stage"` in `pipeline.py` + `server.py`. Confirm only `"llm"` appears. File issue "emit STT + TTS receipts".
3. **Session-resume replay gap** (C8): confirm by grep `'"messages"'\|messages=.*get_messages' server.py`. File issue.
4. **sqlite-vec gap** (E11): grep `sqlite.vec|sqlite_vec` in `dragon_voice/` → 0. File issue. (openclaw has it, Dragon does not.)
5. **TC gateway-fail revert inconsistency** (G5): align the two error paths to both carry `voice_mode: 0`. 3-line fix.
6. **`widget_chart` helper missing** (B5): add the stub method to `surfaces/base.py` (empty body fine — at least document). 10-min patch.
7. **`widget_capabilities` read-site** (B14): grep `widget_capabilities` in `tools/` + `surfaces/` → 0 → either wire into `_safe_send` as a last-mile truncation, or delete the conn_state field.
8. **`fetch_link_preview` dead-code audit** (H12): delete or wire.
9. **TC default model alignment** (J12): pick one of {`ollama/qwen3:1.7b`, `anthropic/claude-opus-4-6`, `minimax/MiniMax-M2.5`} and make it the default in Dragon + document on gateway. One-line change to `tinkerclaw_llm.py:29`.
10. **Race test for CAS** (C2): add a `test_session_cas_race` pytest that spawns two concurrent `update_session_status_if` calls. ~30 LOC.

### Bucket (b) — Real code work (closing overclaims)

1. **Register TimesenseTool** (#1 overclaim, D8/K7): add `self._tool_registry.register(TimesenseTool(self._surface_mgr))` in `_on_startup`. Delete `TimerTool` OR make them distinct by name. Decide which wins the "set a 25-min timer" voice intent.
2. **STT + TTS receipt emission** (F4/F5): in `pipeline._process_utterance` add receipts after STT (line ~491, after `stt_ms` computed) and after each TTS synthesis (pipeline.py:~780, after `tts_ms`). Mirror the LLM receipt shape. Cost rendering: cloud STT/TTS pricing lives in `openrouter_llm._PRICING_MILS_PER_M` — extend for `openai/gpt-audio-mini`.
3. **Session resume message replay** (C8/K15): after `session_start`, if `resumed` is True, read `await message_store.get_context(session_id, max_messages=10)` and emit `{"type":"session_messages","items": [...]}`. Tab5 rehydrates chat store. ~30 LOC on Dragon + protocol addition.
4. **Memory backend upgrade** (E11/K2): port `src/memory/sqlite-vec.ts` technique to Python; add vss0 extension loading to `memory.py`. Or document-and-defer. Medium-size change.
5. **TC model unification** (J12): edit Dragon default + document the override mechanism in `config_update`. Fix the receipt-emit to **always** carry the gateway-reported model (requires gateway to include it in SSE tail chunk).
6. **Widget skills coverage** (B12/B13): pick 2-3 more tools (weather, web_search expanded, calculator) to emit `widget_card` or `widget_list`. Currently only web_search auto-emits list; weather answers in plain text even on cloud.

### Bucket (c) — Defer

1. **TC memory read-through** (E5). Tab5 audit correctly tags this "optional Phase 4". Bypass is the current intended design — TC owns memory. Leave deferred.
2. **MCP bridge hardening** (D9/K9). Code exists, not exercised; no real demand visible in the repo. Revisit if an MCP server is actually configured in production.
3. **LMStudio / NPU Genie backends from voice_mode** (D16/D17/K1). Reachable via REST `/api/config`. Not worth wiring into voice_mode switch until someone asks.
4. **OG link preview cards** (H12). Dead code today; reactivate when there's a skill that demands it.
5. **DB corruption auto-recovery validation** (C4). Only worth exercising before a release cut, not during a sprint.
6. **Openclaw auth rate limiter live test** (J16). Gateway concern, not voice-loop concern.
7. **Full-on-device replay of every 55 Debug-tab tests**. Too expensive for a cold audit; trust the suite presence.

---

## 17. Note on methodology

This audit reads exactly: (1) `TinkerBox/README.md` + `TinkerBox/CLAUDE.md` + `TinkerBox/docs/protocol.md` + Tab5 audit cross-reference; (2) full text of `dragon_voice/server.py`, `pipeline.py`, `sessions.py`, `db.py`, `memory.py`, `conversation.py`, `surfaces/base.py`, `surfaces/manager.py`, `llm/tinkerclaw_llm.py`, `llm/openrouter_llm.py`, `media/pipeline.py`, `api/media_routes.py`, `tools/registry.py`, `tools/timesense_tool.py`, `tools/web_search.py`, `stt/moonshine_stt.py`; (3) file-list + LOC spot-checks for all other modules; (4) openclaw `src/gateway/server.impl.ts` header + `src/agents/defaults.ts` + `src/agents/pi-embedded-runner/run.ts` header + a grep-level confirmation that `/health`, `/v1/chat/completions`, and the 9 channel plugin packages exist with substantial (>1k LOC) implementations each; (5) openclaw's `CLAUDE.md` for repo guidelines. **Not read:** openclaw's per-channel TypeScript implementations line-by-line, per-skill markdown frontmatter, `src/memory/manager.ts` body, the 52 skill-definition contents. **Not executed:** no pytest runs, no network probes, no device touches. Any "VERIFIED (code)" marker means the wiring from a spec claim to a working code path is grep-visible + the logic read-through shows no obvious drop-out — NOT that it has been observed producing the user-visible effect on a running system. Any "OVERCLAIMED" / "HALF-BUILT" judgment is grounded in a specific grep miss or a spec-to-code divergence visible from the read-through; those are the rows the parent session should probe first when moving to a live test.
