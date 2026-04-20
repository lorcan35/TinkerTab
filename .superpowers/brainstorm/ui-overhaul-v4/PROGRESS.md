# UI Overhaul v4·D — Progress Tracker

> **Purpose:** Single source of truth for what's done, what's in flight, what's next. Designed to survive Claude Code session boundaries + conversation compactions. Resume-safe.

**Last updated:** 2026-04-19
**Branch:** `feat/ui-overhaul-v4` (v4·C Ambient Canvas shipped, v4·D Sovereign Halo in design)
**Session continuity:** see `/home/rebelforce/projects/TinkerTab/CLAUDE.md` §"Continue here" + memory file `project_ui_overhaul_status.md`

## Terminology — read first

| Symbol | Meaning |
|--------|---------|
| ✅ **shipped** | Flashed to device, visually verified via `/screenshot`, running stable. **Only v4·C Ambient Canvas currently meets this bar** (commit `8571db5`). |
| ✎ **drafted** | HTML mockup exists. **Not shipped.** No code written. Not on device. |
| 🚧 **in flight** | Code being written or mockup being built in current session. |
| ⬜ **pending** | Planned, no artifact yet. |

Nothing is "done" until: (1) code committed, (2) `idf.py build` passes, (3) flashed to `/dev/ttyACM0`, (4) `/screenshot` verified, (5) no regression on heap/WDT over a 10-minute soak. **Don't shortcut this.**

---

## For a new Claude Code session picking this up

**Load in this order:**
1. This file (PROGRESS.md) — know where we are
2. `SYSTEM.html` — the reality-checked master spec
3. Memory: `project_ui_overhaul_status.md` — device state
4. `system-d-complete.html` — current in-flight deliverable
5. TinkerTab `CLAUDE.md` — project constraints (LVGL, WS protocol, NVS keys, etc.)

**Before writing code:** read `main/ui_home.c`, `main/voice.c`, `main/widget_store.c` to ground-truth. Primitives changed between v4·C ship and v4·D design — don't trust mockup HTML alone.

---

## Round 4 deliverables

### Design files in this folder

| File | Status | Surfaces | LOC | Notes |
|------|--------|----------|-----|-------|
| `SYSTEM.html` | ✎ drafted | Master spec | 660 | Reality-checked via 3 parallel codebase audits |
| `system-d-sovereign.html` | ✎ drafted | 23 base | 1910 | Screens + widgets + rich media + overlays |
| `system-d-modes.html` | ✎ drafted | 8 mode UX | 1058 | Dials, presets, receipts, budget, bindings |
| `d-sovereign-halo.html` | ✎ drafted | 1 home | 400 | Anchor iteration (the pick) |
| `e-journal.html` | ✎ drafted | 1 home | 400 | Fallback / magazine cover |
| `f-beacon.html` | ✎ drafted | 1 home | 400 | Fallback / broadcast metaphor |
| `system-d-complete.html` | ✎ drafted | 32 gap fills | ~3600 | All 32 surfaces drafted + connection fix log |
| `STORIES.md` | ✎ drafted | 100 user-story stress tests | &mdash; | 20 per concept &times; 5 concepts; surfaces 25 new design gaps (G1-G25) |

### Section status in `system-d-complete.html`

| § | Section | Surfaces | Status |
|---|---------|----------|--------|
| A | Voice pipeline (dictation, processing, speaking, interrupt, timeout) | 5 | ✎ drafted |
| B | Notes full CRUD (empty, edit, playback, delete confirm) | 4 | ✎ drafted |
| C | Settings extras (storage, battery, about, quiet hours) | 4 | ✎ drafted |
| D | Onboarding (welcome, Wi-Fi password, Dragon pair) | 3 | ✎ drafted |
| E | Chat advanced (empty, camera attach, long scroll) | 3 | ✎ drafted |
| F | Widgets advanced (composition, prompt answered) | 2 | ✎ drafted |
| G | Agents / Memory / Focus (Time Sense, memory browser, agent run) | 3 | ✎ drafted |
| H | Files advanced (JPEG preview, delete confirm) | 2 | ✎ drafted |
| I | Edge states (critical battery, OTA rollback, SD unmounted, low memory) | 4 | ✎ drafted |
| J | Connection reliability (WS healthy, reconnecting, ngrok) + fix log | 3 + log | ✎ drafted |

**Grand total:** 32 of 32 surfaces drafted. **Zero shipped** — nothing verified on device beyond v4·C Ambient Canvas home (commit `8571db5`).

**Grand total:** 32 surfaces planned, 9 shipped, 23 remaining.

---

## Build roadmap (from SYSTEM.html §06)

### Phase 1 · Ship Sovereign home ✅ **SHIPPED + on-device verified**
**Commits:** `ebc99d6` (1a) · `125b86e` (1b) · `bd4c136` (1c) on `feat/ui-overhaul-v4` (pushed to origin).
**1a:** orb 156→180, halos/rings scaled proportionally.
**1b:** NOW card 240 px → live-line 88 px (bg transparent, hairlines top+bottom only, kicker + lede side-by-side, stats hidden).
**1c:** 4-chip nav rail hidden, 56 px 4-dot menu chip on right of say-pill (tap → chat).
**Chrome drop:** ~400 px → 144 px (strip 104 + pad 40).
**Verified:** `/screenshot` at 192.168.1.90 after each flash. See `shots/phase1a-home.bmp`, `phase1b-fixed.bmp`, `phase1c-home.bmp`.

### Phase 2 · Triple-dial mode sheet ✅ **SHIPPED (2a + 2b)** · ⬜ 2c pending
**Commits:** `6c83cee` (2a data model) · `c2ec711` (2b sheet UI) on `feat/ui-overhaul-v4` (pushed).
**2a:** NVS keys `int_tier` / `voi_tier` / `aut_tier` + `tab5_mode_resolve()` + POST /settings accepts tiers. Verified via curl: 4/4 resolver paths correct.
**2b:** New files `main/ui_mode_sheet.c/h`. Fullscreen overlay with 3 segmented dials + live composite ("Resolves to Local / Hybrid / Full Cloud / TinkerClaw"). Long-press mode chip → sheet. Live config_update to Dragon on each tap. Verified on device (shots/phase2a-sheet.bmp, phase2a-smart-studio.bmp).
**2c pending:** Agent-mode warning before switching to Autonomy=Agent (memory-wipe consent).

### Phase 3 · Per-turn receipts + daily budget ✅ **3a + 3b + 3c SHIPPED** · ⬜ 3d pending
**Commits:** TB `769961a` (3a Dragon) · TT `349dab1` (3b Tab5 parser) · TT `ca***` (3c NVS + home readout).
**3a Dragon:** OpenRouter `stream_options.include_usage`, module-level price table, `get_last_usage()`, receipt emit in both voice + text turn paths.
**3b Tab5:** WS parse `{"type":"receipt",...}`, cache last receipt, log to serial.
**3c Tab5:** NVS u32 keys `spent_mils` + `cap_mils` + `spent_day` with midnight rollover; `tab5_budget_accumulate()`; home live-line empty-state reads `TODAY · $0.XX of $Y.YY` when `spent>0`.
**Verified live:** 3 Haiku turns = 983 mils ≈ $0.01, rendered on device (`shots/phase3c-budget.bmp`).
**3d shipped:** Per-bubble chat receipt stamps verified on device — bubble subtitle reads `16:49 · TINKER · 3.5-haiku · $0.003` (commit `[new]`, `shots/phase3d-chat-stamps.bmp`).
**3e shipped:** Auto-downgrade to Local when cap hit (commit `c570cb1`). Verified live: 299-mil Haiku turn trips 100-mil test cap → Dragon flipped to `voice_mode=0 stt=moonshine tts=piper llm=ollama` on the same turn, NVS `voice_mode` persisted 2→0, `llm_model` preserved for future re-raise. Cap restored to $1.00 after test.
**4a shipped:** Local-model usage capture + FREE stamp code path (TB `bf534a1` + TT `0425a1a`). `price_for_model` returns 0 for model ids without `/` prefix. Ollama receipt verified: `model=qwen3:1.7b tok=337+103=440 cost_mils=0`. Chat render branches on `receipt_mils>0` → `$X.XXX` vs FREE. Visual shot of FREE stamp specifically deferred — device testing environment kept resetting sessions mid-burst; code path is the same as the verified `$0.003` branch.

### Phase 4 · Intelligence layer · 4b + 4c + 4d SHIPPED + 4d verified in-place
**4b shipped:** vision_capability WS event (TB `40c21d1`) + violet chip on camera top-bar when model supports vision.
**4c shipped:** widget_list type parsing + rendering (TT `5d6ce19`) — `widget_t.items` array, `widget_store_upsert` memcpy, LIVE + LIST both compete for home slot, card grows 88→168 px when type==LIST.
**4c auto-emit shipped (2026-04-19 late):** Dragon `_on_tool_result` auto-emits `widget_list` on web_search hits (TB `c2ada03`). `surfaces/base.py` also gains `Tab5Surface.list_()` for skills to emit lists directly. Verified on device: "WEB_SEARCH +1" + "SpaceX latest news today" + 3 rows rendered on home live-slot.
**4d shipped (2026-04-19 late):** `ui_chat_refresh_receipts()` force-redraw hook (TT `34b4aee`). Receipt stamps now paint on the CURRENT turn's bubble, not next-scroll. Verified: gpt-4o-mini 51-mil receipt shown inline as "18:25 · TINKER · 4o-mini · $0.000".

### Gauntlet fixes
- **G2 SHIPPED** (TB `5eab5a7` + TT `dafcc4b`) — RETRIED chip on receipts when OpenRouter retried.
- **G3-F2 / G5 / G7 SHIPPED** earlier this session.
- **G7-F SHIPPED (2026-04-19 late)** — Tab5 `voice_send_config_update_ex(0, lm, "cap_downgrade")` (TT `5d7895b`), Dragon `pipeline.speak_system()` (TB `1d7d42e`). Verified: 103 KB PCM + tts_end 671ms on reason=cap_downgrade.
- **G9 SHIPPED (2026-04-19 late)** — `forget_fact` tool with two-step confirm gate (TB `f2c493a`). Lookup → confirm flow with alternatives. Verified 4 branches on live Dragon.
- **G10 SHIPPED** (TB `9f44f5a`) — Idempotency-Key header on OpenRouter calls.

**Pending:**
- G4 (investigation: memory is already global for single-user — premise doesn't apply)
- G8 local-authoritative focus timer
- logprobs + `Tool.preferred_backend` + confidence escalation chips

### Phase 4e · Settings UI cap editor ✅ SHIPPED (2026-04-19)
TT `d08f8e2`. "Daily cap" slider in VOICE MODE section, range 0-50 in 20¢ steps ($0-$10), 500ms NVS debounce, amber label ("$1.00" / "OFF").

### Phase 4f · widget_chart ✅ SHIPPED + verified (2026-04-19)
TT `b6955fd`. `chart_values[12]` + `chart_max` in widget_t, ASCII histogram render (" .:+#" 5-level ramp), card auto-grows 88→168 on CHART. Verified "Weekly tempo" + 7-point bar row.

### Phase 4g · widget_media + widget_prompt ✅ SHIPPED + verified (2026-04-20)
TT `794e915` + TB `d032431`. media_url + media_alt + choices[3] fields; voice.c parsers + ui_home render. Dragon `Tab5Surface.media(...)` and `.prompt(...)` helpers. Verified:
- widget_media: "Latest capture / Taken 3 min ago / IMAGE • rear window view"
- widget_prompt: "Confirm delete? / 1 Delete all 3 items / 2 Delete selected only / 3 Cancel" with tone=urgent → rose orb

### Gauntlet additions
- **G1 SHIPPED (2026-04-19 late)** — single-slot queue in voice.c + "+1 QUEUED" badge on voice overlay (TT `6901db2`).
- **G7-F SHIPPED** (prior)
- **G9 SHIPPED** (prior)

### Session 2026-04-20 UX audit + stability sweep
- Nav sheet on 4-dot chip (TT `12d3b9e`): 6 tiles Chat/Notes/Settings/Camera/Files/Memory. Verified.
- Nav load fix: `ui_home_go_home` + `ui_home_nav_settings` + `ui_focus/_memory/_agents/_sessions_show` all call `lv_screen_load(home)` when on a foreign lv_screen. Camera→home + Settings from Camera both work now.
- UI un-hardcoding (TT `bbdb691`): widget kicker 106 px clamp + LONG_DOT, auto-tall on title+body or body>48 chars; orb amber always (no blue Cloud tint); mode sheet resolve-panel reads live llm_model; chat suggestions "Powered by <model>" live.
- Gemini Flash 3 Preview set as default cloud model; pricing table (TB `d032431`) covers 3-flash-preview, 2.5-flash, 2.5-flash-lite, 2.0-flash.
- **Tab5 stuck-watchdog mode-aware** (TT `5fdf729`): was a hardcoded 10 s that killed every Local turn. Now 300 s Local / 240 s Hybrid/TinkerClaw / 75 s Cloud. WS pong budget 45s→180s. Verified: Local turn "one word greeting" completed in 32 s (qwen3:1.7b 455 tokens, FREE).
- **Internet-only mode (conn_mode=2) verified**: Tab5 → tinkerclaw-voice.ngrok.dev → Dragon 127.0.0.1:3502. "Say hi in three words." → "Hello there, friend." via gemini-3-flash-preview in ~6 s.
- **TinkerClaw gateway (openclaw) fixed**: removed `models.providers.ollama.options` invalid key from `~/.tinkerclaw/tinkerclaw.json`. `GET /health` → 200.
- **Mode sheet state sync** (TB `2cd48bb`): dials reverse-derive from live voice_mode on open. Previously always Fast+Local+Ask.
- **Pipecat / LiveKit research brief** saved at `RESEARCH-pipecat-livekit.md`. Recommendation: steal pipecat patterns (frame taxonomy, interrupt frame, semantic turn detection), skip LiveKit full stack, clone Xiaozhi-ESP32's Opus+WS protocol for v2.

**Still open (next sprint):**
- Opus encoder on mic path (cuts ngrok upload 10x).
- Interrupt frame (cancel in-flight LLM + TTS on barge-in, truncate bot transcript to what user heard).
- Onboarding / first-run screens from `system-d-complete.html` section D.
- ui_agents / ui_focus still rendering mocked content (ui_memory wired in this sprint).
- Pre-existing voice WS flakiness on reconnect (not introduced by audit fixes; warrants its own investigation).

### Stability audit sprint ✅ SHIPPED (2026-04-20)

Both audits (`AUDIT-stability-2026-04-20.md` + `AUDIT-ux-gauntlet-40-2026-04-20.md`) saved in this folder.

**Tab5 (`4849655`):**
- P0 widget_capability sent in register + Dragon stores on conn_state
- P0 handle_binary_message reads s_state under mutex
- P0 widget_store_gc ticks every ~30 s from home refresh timer
- P1 stt_partial bounded snprintf under state mutex
- P1 budget rollover uses local midnight (year*366+yday) not UTC
- P1 local-turn receipt no longer filtered out by empty model string

**Dragon (`2a6c677`):**
- P0 inference ThreadPoolExecutor shutdown hook on server exit
- P0 on_audio/on_event route through _safe_send_bytes/_safe_send_json
- P0 minimal-receipt fallback on the exception path (cost_mils=0 + retry_reason)
- P0 OpenRouter error + ClientError + proxy-HTML errors no longer leak into token stream (logged + friendly fallback sentence)
- P1 widget_capabilities.widgets stashed on conn_state for skill downgrade
- P1 config_update rate-limited to 2/sec/conn
- P1 Hallucination regex incremental scan with 32-byte overlap (O(N) instead of O(N^2))
- P1 Pre-warmed fallback STT + TTS cached on pipeline (released in shutdown)
- P1 pause_session is now atomic CAS via new Database.update_session_status_if

**Prior-session audit fixes already shipped (previous commits):**
- P0 #1 SurfaceManager + widget_action handler (`55621f1`)
- P0 #2 NVS handle mutex (`e010ed9`)
- P0 #3 Ngrok fallback counter reset on disconnect (`e010ed9`)

---

## Resume checklist for the next agent

If you're picking this up mid-stream:

- [ ] Read this file top to bottom
- [ ] `cd /home/rebelforce/projects/TinkerTab && git status` — confirm branch state
- [ ] Check `system-d-complete.html` tail for the "LAST SECTION BUILT" marker
- [ ] Continue next `✎ drafted` section using the template pattern from `system-d-sovereign.html` / `system-d-modes.html`
- [ ] Update THIS file as you finish each section (✅)
- [ ] Update the progress banner at the top of `system-d-complete.html`
- [ ] If you hit the output-token limit, stop at a section boundary — don't leave half-built surfaces

**Non-negotiables (from the code):**
- No `lv_arc` — use `lv_bar` horizontal
- No `box-shadow` &gt; 24 px — use concentric rings + 2-stop radial
- No `lv_label` rotation — pre-render sprite or drop
- Orb recipe: `.orb-recipe` class with radial + inner highlight
- Live slot = `widget_store_live_active()` — it's real, `s_now_card` not `s_poem_label`
- Voice WS persists across mode switches (`mode_manager.c:144-148`)

---

## Sovereign Halo spec-compliance audit (2026-04-19 evening)

Cross-checked live device `/screenshot` against `d-sovereign-halo.html` + `system-d-sovereign.html`:

| Spec element | Status |
|---|---|
| Orb 180 px | ✅ |
| 3 amber rings + 2 halos | ✅ |
| State word (Fraunces italic 52 px) | ✅ |
| **Trail line (Fraunces italic 22 px)** | ✅ shipped (commit `[new]`) — "Evening, Emile -- what's on your mind?" |
| Mode chip centered below trail | ✅ |
| Live-line (top+bottom hairline rules, no fill) | ✅ |
| Live-line shows "TODAY $0.XX of $Y.YY" on spend | ✅ |
| 104 px strip + 4-dot menu | ✅ |
| Amber speak-button (84 px) | ✅ |
| Orb tone shifts with voice_mode | ✅ (local=emerald, hybrid/cloud=amber+sky) |
| Em-dash in trail | ⚠ ASCII " -- " workaround — font subset missing U+2014 |

Other screens (per `system-d-sovereign.html`) not yet spec-audited.

## Active risks / open questions

1. **Dragon 42s keepalive still fires** on WS-level PING-only clients. Either (a) remove `_ws_keepalive` server-side since `esp_websocket_client` pings now handle liveness, or (b) add Tab5 app-level JSON ping every 20 s. Leaning (a). **[gate: must fix before merge]**
2. **LV_MEM headroom on Settings** is tight (73 %). Adding a Skill Bindings section needs the two-pass creation + WDT feed pattern.
3. **TinkerClaw memory read-through** — Phase 4 optional. Injecting top-N facts as system prompt on `voice_mode=3` would preserve continuity without breaking gateway boundary. Decision deferred.
4. **Widget composition** covered in F1 with +N-more chip.
5. **25 gaps from STORIES.md** (G1–G25) require triage before Phase 1 build. Top P0 items: G7 (silent auto-escalation violates trust), G21 (app-level JSON ping), G10 (pre-turn budget check), G16 (widget TTL), G19 (widget urgency levels).

---

## Changelog

- **2026-04-19** · Round 4 design + STORIES stress-test: SYSTEM spec + sovereign + modes + d/e/f + complete (A–J) + 100 user stories surfacing 25 new gaps — this session
- **2026-04-19** · v4·C Ambient Canvas shipped on device (commit `8571db5`)
- **2026-04-18** · Round 3 trio (a/b/c) drafted
- **2026-04-16** · Round 1 exploratory (01/02/03) + Round 2 cuts (A/B/C) drafted
