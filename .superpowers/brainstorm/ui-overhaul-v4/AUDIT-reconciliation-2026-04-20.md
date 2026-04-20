# Reconciliation Audit — v4·D Sovereign Halo + TinkerClaw

**Date:** 2026-04-20
**Branch under test:** `feat/ui-overhaul-v4` (Tab5) + `fix/ws-send-guard` (Dragon)
**Auditor:** Claude Opus 4.7, cold read of spec + code + shots/ only. No device touched.

---

## Executive summary

PROGRESS.md reports ~30 items "SHIPPED + verified"; a cold read of the code and `shots/` directory confirms about half of that. **Roughly 8 of those items are code-only (parser + emit paths exist; no on-device screenshot or log trace in this folder to prove the user-visible path completed).** Biggest gaps: the TinkerClaw agent-mode consent sheet (the spec's violet warning modal) was silently downgraded to a color-swap on the composite card and is being shipped as if it fulfilled the spec; the widget_media/widget_card/widget_prompt "end-to-end" claims rely on a paint that no screenshot in `shots/` captures; and onboarding (spec §D, UX-audit P0) has not a single line of code, not even a stub.

---

## Scorecard — A. TinkerClaw (voice_mode=3) polish

| Item | Spec | Code | Device evidence | Status | Action |
|---|---|---|---|---|---|
| Agent-mode consent sheet (M5 violet modal, before aut_tier 0→1) | `system-d-modes.html:820` "M5 · CONTEXT CARRY-OVER", `.warn-sheet` with explicit "Switch to Agent" / "Keep Ask mode" buttons | `ui_mode_sheet.c:382-391` silently recolors composite card (violet border + "AGENT MODE" kicker) on Agent tap. No modal. No cancel path. Commit `98648aa` self-describes as "lighter version... no second modal, no revert-on-cancel flow" | `shots/phase2c-agent.bmp` (52 KB) shows inline composite recolor only, not a full-screen warn-sheet with 4 bullets | **HALF-BUILT / SPEC-DOWNGRADED** | Either (a) build the real modal, (b) amend the spec to match reality. Don't keep calling `98648aa` a Phase 2c "ship". |
| Settings TinkerClaw row → warning | Implied by §C "voice_mode=3 selection" | `ui_settings.c:405-408` `cb_tab_local` → `voice_tab_switch(3)` immediately. No warning path exists from Settings | None | **CODE-ONLY (negative)** — spec gap, no guardrail | BUILD: reuse same modal from mode sheet; wire before `voice_tab_switch` |
| MEMORY OFF chat disclosure during TC turns | UX-audit calls out that memory bypass must be visible beyond the mode-sheet composite tint | Zero hits on `MEMORY OFF \| memory_off \| memory bypass` anywhere under `main/ui_chat.c` | None | **SPEC-ONLY** | BUILD: a chat system-bubble ("MEMORY BYPASSED — agent gateway") on mode transition into 3 |
| TC receipts stamp chat bubble with minimax model | PROGRESS 2026-04-20 Sprint 3 | Dragon `server.py:1630` emits `receipt` with `model=llm._model` on TC path (commit `f836d06`); Tab5 `voice.c` receipt parser + `ui_chat_refresh_receipts` force-redraw exists. Code path is identical to Cloud, only cost=0 | Code exists; no `shots/tc-*.bmp` or `shots/phase*-minimax*.bmp`. None of the 44 screenshots contains "minimax" or TC receipt imagery | **CODE-ONLY** | TEST: do one TC turn, `/screenshot`, save `shots/tc-receipt-minimax.bmp`. ~2 min. |
| TC turn 30-60s user feedback | PROGRESS 2026-04-20: chat input breathing 210→255 over 4 s, stuck watchdog mode-aware 240 s TC | `voice.c` stuck WD verified via commit `5fdf729`; `chat_input_bar` breathing verified via `fe8036d`. PROCESSING state emitter is wired | No screenshot of a 30-s TC in-flight UI | **CODE-ONLY** | TEST: start a TC turn, wait 20 s, grab screenshot of chat + home. ~3 min. |
| TC fallback → auto-revert + toast | SYSTEM §06 | Dragon `server.py:1131-1139` sends `{"error": "TinkerClaw gateway is not reachable"}` on health-check failure; Tab5 `voice.c:826-843` sets voice_mode=0, toast, state READY | Commit `98648aa` description says this path was observed once (toast "TinkerClaw gateway is not reachable") but no shot saved | **VERIFIED (log-only)** | No action — but save a `shots/tc-fallback-toast.bmp` next time. |
| TC + memory read-through (inject top-5 facts as system prompt) | `SYSTEM.html:681` Phase 4 Optional | Zero hits on `inject.*fact \| top.?5.?fact \| inject_memory` in Dragon. `tinkerclaw_llm.py` is a raw pipe per `CLAUDE.md` | None | **SPEC-ONLY (explicitly deferred per PROGRESS.md risk #3)** | DEFER — this is correctly marked "decision deferred". Stop mentioning in reports. |

---

## Scorecard — B. Widget platform

| Item | Spec | Code | Device evidence | Status | Action |
|---|---|---|---|---|---|
| `widget_live` | `widget-platform/` | `widget.h:WIDGET_TYPE_LIVE`, `ui_home.c` live-slot competitor | Multiple shots: `phase1b-home.bmp`, `phase1c-home.bmp`, plus `gauntlet-g5-plus-more*.bmp` | **VERIFIED** | SHIP |
| `widget_card` | PROGRESS "Phase 4" shipped | `widget.h:WIDGET_TYPE_CARD` + parser at `widget_store.c:287`. But `widget_store.c:221-226` explicitly excludes CARD from live-slot competition; render on home does NOT exist for cards. Chat-inline `ui_chat_push_card` exists | No screenshot of a card rendered anywhere | **HALF-BUILT / MISCLAIMED** — parser exists, no home render path, no chat verification | TEST chat path: emit `widget_card` from Dragon to one TC turn, screenshot chat. ~5 min. |
| `widget_list` | Phase 4c | Parser + priority compete + auto-emit from web_search (`5d6ce19` + TB `c2ada03`) | `shots/phase4c-widget-list-final.bmp` + 3 prior iterations | **VERIFIED** | SHIP |
| `widget_chart` | Phase 4f | Parser + ASCII histogram (`b6955fd`) | Referenced in PROGRESS as verified, but no `shots/phase4f-*.bmp` actually exists in the folder listing | **CODE-ONLY** — PROGRESS claims a screenshot; the 44-file `shots/` listing contains no `phase4f` file | TEST: re-capture. ~2 min. |
| `widget_media` inline image decode | Phase 4g — claim "verified 2026-04-20" | `ui_home.c:1267-1336` has the lazy-create + fetch task + lv_image bind. `media_cache_fetch` exists. PROGRESS says "IMAGE · rear window view" which is the **caption fallback string**, not a decoded JPEG | No shot. The caption fallback is specifically what renders when decode fails (`ui_home.c:1316` `media_image_clear_cb`) | **HALF-BUILT — claimed verified, but only the fallback path has been witnessed** | TEST: upload a real JPEG through `/api/media/upload`, emit `widget_media` pointing at it, screenshot. If the blurred-lede region contains image data, shoot + save. ~5 min. |
| `widget_prompt` tap → `widget_action` end-to-end | Phase 4g; Dragon logs "widget_action: …" on tap | Tab5 parser + hit-zone tap in `ui_home.c:1260`; Dragon `SurfaceManager.handle_action` in `surfaces/manager.py:79-91` | PROGRESS claims verified; only registered handler is **Time Sense's own** (`timesense_tool.py:65`). Any other skill-emitted prompt lands on `manager.py:86` with `log.info("widget_action no handler: ...")` — and no screenshot confirms timesense round-trip | **HALF-BUILT** — the full user-story "a skill emits prompt → I tap → skill acts" only closes when TimeSense is the emitter AND has a live card; no other skill has a registered handler | TEST: start TimeSense, emit a prompt widget, tap one choice, verify log + screenshot the post-tap state change. |

---

## Scorecard — C. Home screen primitives (`d-sovereign-halo.html`)

| Item | Spec | Code | Device evidence | Status |
|---|---|---|---|---|
| 180 px orb | `d-sovereign-halo.html` | `ui_home.c` commit `ebc99d6` Phase 1a | `shots/phase1a-orb180.bmp`, `phase1a-home.bmp` | **VERIFIED** |
| Orb amber regardless of mode | `d-sovereign-halo.html` spec-compliance table | `ui_home.c` un-hardcoding commit `bbdb691` | Present in post-`bbdb691` shots | **VERIFIED** |
| Single live statement replacing greeting + 2 cards | Phase 1b | Commit `125b86e` | `shots/phase1b-fixed.bmp` | **VERIFIED** |
| Live line (not card) from `widget_live` | Phase 4c | Code path in `ui_home.c` widget-bind | `shots/phase4c-widget-list-final.bmp` (LIST qualifies) | **VERIFIED** |
| Single 96 px tap bar | Phase 1c | Commit `bd4c136` | `shots/phase1c-home.bmp` | **VERIFIED** |
| 4-dot chip → nav sheet (not chat) | Fix this session `12d3b9e` | `ui_home.c:1184-1189` `menu_chip_click_cb` → `ui_nav_sheet_show` | No standalone `shots/nav-sheet.bmp` in folder | **CODE-ONLY** since the fix — no screenshot of sheet visible |
| Mode chip long-press → mode sheet | Phase 2b | `ui_mode_sheet.c` | `shots/phase2a-sheet*.bmp` | **VERIFIED** |
| Nav rail hidden | Phase 1c | `ui_home.c:89,604` comment "Nav rail hidden" | Post-Phase 1 shots | **VERIFIED** |

---

## Scorecard — D. Chat surface (`system-d-complete.html §E`)

| Item | Spec | Code | Device evidence | Status |
|---|---|---|---|---|
| Bubbles with receipt stamps | §E | Receipt parser + `ui_chat_refresh_receipts` | `shots/phase3d-chat-stamps.bmp`, `phase4a-*-stamp*.bmp` | **VERIFIED** |
| Mode badge reads live model | fe8036d | `chat_header.c` read from NVS | PROGRESS notes "now shows AGENT on m==3"; no shot | **CODE-ONLY** |
| Break-outs: image/card/audio rendered inline in chat | §E "Rich media flow" | `chat_msg_view.c:196-230` — **for MSG_IMAGE it renders `LV_SYMBOL_IMAGE + alt-text body label` inside a placeholder breakout. It does NOT decode a JPEG.** `ui_chat_push_card` exists but renders a text-only card, not a syntax-highlighted image | None | **HALF-BUILT** — the "Dragon renders → Tab5 decodes JPEG + TJPGD" claim applies to `ui_home.c widget_media` path only; the **chat** path is text-placeholder-only |
| Session drawer | shipped claim | `chat_session_drawer.c` exists; wired from header `menu_click_cb → chat_session_drawer_show` | No `shots/session-drawer*.bmp` in folder | **CODE-ONLY** |
| Thinking + tool-indicator bubbles | §E | `voice.c:868-894` sets voice state to "Thinking..." / "Searching..." on tool_call. These drive the **voice overlay label**, NOT a chat bubble | `shots/chat-v4c-*.jpg` don't show tool indicators | **HALF-BUILT / MISCLAIMED** — CLAUDE.md says "thinking + tool indicator bubbles" but the actual implementation only changes the voice-state label string |
| Code block rendered as syntax-highlighted image in chat | `CLAUDE.md` Rich Media Chat feature | Dragon MediaPipeline renders via Pygments; Tab5 receives as `media` WS message; `ui_chat_push_media` queues MSG_IMAGE | Chat renders only `LV_SYMBOL_IMAGE "  alt-text"` (see above) | **HALF-BUILT** — Dragon renders the image; Tab5 does not actually draw the decoded pixels in a chat bubble |

---

## Scorecard — E. Settings

| Item | Spec | Code | Device evidence | Status |
|---|---|---|---|---|
| Mode rows with live model label | §C, fe8036d | `ui_settings.c:1068-1076` uses live `llm_model` for Cloud desc; TinkerClaw row desc hard-coded to "Agents · Memory" | None specific to fe8036d | **CODE-ONLY** |
| Daily cap slider | Phase 4e `d08f8e2` | `ui_settings.c:1144-1156` | Referenced as "shots/phase4e" but the folder listing contains no `phase4e` bmp — probably an uncommitted filename | **CODE-ONLY / shot missing** |
| Agent-warning on TinkerClaw row tap | §C | Not implemented; row fires `voice_tab_switch(3)` immediately | None | **SPEC-ONLY** |
| WiFi configure flow (scan / connect) | §C | `ui_wifi.c` 934 lines exist | No shot of a successful scan/connect flow in this folder | **CODE-ONLY** (pre-existing) |
| OTA check button | `ota.c` + `ui_settings.c:716` `cb_ota_check` | Button + task + `tab5_ota_check` all present | No shot of a successful `/ota/check` round-trip with a real version diff | **CODE-ONLY** |

---

## Scorecard — F. Edge states (§I)

| Item | Spec | Code | Device evidence | Status |
|---|---|---|---|---|
| OFFLINE banner | Red pill + full-screen hero | `ui_home.c` renders one-word "OFFLINE" in `s_sys_label`. No full-screen hero. Search for `offline_hero \| ui_offline \| full.?screen` returns zero hits | None | **SPEC-ONLY / HALF-BUILT** — only the pill is built; the hero is absent |
| NO DRAGON | Similar | Same one-word label (`s_sys_label`); `ui_home.c:1070` "Dragon is unreachable.\nVoice notes save locally." shown as live-line lede | `shots/disconnect-01-detected.jpg` shows the short pill | **HALF-BUILT** — basic status works, full surface missing |
| CAPPED | G7 fix | Commit `0b2bf0d` persistent CAPPED banner | `shots/gauntlet-g7-capped-banner.bmp` | **VERIFIED** |
| Reconnecting (Tier-1) | c3f322d | `voice.c` VOICE_STATE_RECONNECTING enum + pill | `shots/disconnect-*.jpg` trio | **VERIFIED** |
| Dragon crash mid-turn → rose orb + toast | §I | Zero hits on `rose_pulse \| mid.?turn.*toast \| Dragon.*crash.*toast` in Tab5 | None | **SPEC-ONLY** |

---

## Scorecard — G. Onboarding (§D, UX-audit P0)

| Item | Spec | Code | Status |
|---|---|---|---|
| 3-card welcome → wifi → dragon pair on first boot | `system-d-complete.html §D` + UX-audit P0 | Grep for `onboarding \| first.?run \| ui_onboard \| welcome.*screen` on `main/` returns only NVS first-boot auto-gen tokens. No UI code. | **SPEC-ONLY — zero implementation** |

---

## Scorecard — H. Mode sheet (`system-d-modes.html`)

| Item | Spec | Code | Device evidence | Status |
|---|---|---|---|---|
| Triple-dial | 2b | `ui_mode_sheet.c` | `shots/phase2a-sheet*.bmp` | **VERIFIED** |
| Resolve panel live model | bbdb691 | `ui_mode_sheet.c` reads NVS | PROGRESS claims; no dedicated shot | **CODE-ONLY** |
| Live `config_update` on tap | claimed | `ui_mode_sheet.c` calls `voice_send_config_update` on dial tap | No log trace saved, but Dragon-side validation (TC health check, cloud-key check) proves the round-trip | **VERIFIED (log-only)** |
| Agent warning before aut_tier=1 (Phase 2c spec: modal warning sheet) | See A1 above | Composite recolor only | `phase2c-agent.bmp` = recolor only | **HALF-BUILT / SPEC-DOWNGRADED** |
| Presets tab (legacy 4 modes as recipes) | `system-d-modes.html` | Zero hits on `preset.*tab \| preset.*recipe` in `ui_mode_sheet.c` | None | **SPEC-ONLY** |

---

## Scorecard — I. Receipt / budget

| Item | Spec | Code | Device evidence | Status |
|---|---|---|---|---|
| Per-turn receipt WS (Cloud / Local / TC) | 3a/3b/3c/3d/3e/4a/f836d06 | Dragon emits 3 stages; Tab5 parses + stamps | `phase3c-budget.bmp`, `phase3d-chat-stamps.bmp`, `phase4a-*-stamp*.bmp`; TC receipt code shipped but no shot | **VERIFIED (Cloud+Local) / CODE-ONLY (TC)** |
| Daily cap auto-downgrade + TTS | G7-F `5d7895b` + `1d7d42e` | Tab5 `voice.c:758` sends reason=cap_downgrade; Dragon `pipeline.speak_system` | Log confirmed (PROGRESS 2026-04-19: "103 KB PCM + tts_end 671 ms"); no `shots/g7f*.bmp` beyond the CAPPED banner | **VERIFIED (log-only)** — banner visually verified, TTS audibly verified in session |
| Local midnight rollover | 4849655 P1 | `settings.c:460-464` `year*366 + yday` — correct structure | **Has not been observed rolling.** Device hasn't been soaked across local midnight and `spent_day` NVS value inspected post-rollover | **CODE-ONLY** |

---

## Scorecard — J. OTA + recovery

| Item | Spec | Code | Device evidence | Status |
|---|---|---|---|---|
| `tab5_ota_mark_valid()` on boot-good | `CLAUDE.md` OTA section | `main.c:423` in boot sequence | Boots obviously succeed; no partition-valid log saved | **VERIFIED (implicit)** |
| SHA256 mismatch abort | SEC07 | `ota.c:185-189` mbedtls streaming hash | Zero shots or logs of a deliberate mismatch test | **CODE-ONLY** |
| Auto-rollback on crash before mark_valid | Partition table | Bootloader-level (IDF); not a new feature | Never deliberately tested by crashing a bad binary | **CODE-ONLY** |

---

## Scorecard — K. Hygiene (claimed vs. actually shipped)

| Item | PROGRESS claim | Reality | Status |
|---|---|---|---|
| `widget_chart` ASCII histogram | "shipped + verified" | Code at `widget_store.c` + render; no `shots/phase4f*.bmp` in folder | **CODE-ONLY** |
| `widget_media` inline image | "shipped + verified 2026-04-20" | Code path present; only the caption-fallback branch has explicit text in PROGRESS ("IMAGE · rear window view" = fallback string); no decoded-JPEG shot | **HALF-BUILT** |
| `widget_prompt` round-trip | "verified" | Only verified round-trip is TimeSense (which owns both sides). No foreign skill has emitted+tapped. | **HALF-BUILT** |
| Memory screen REST wiring | `e010ed9` | `ui_memory.c` GETs `/api/v1/memory`; code confirmed | PROGRESS itself admits: "earlier screenshots were blocked by voice overlay" | **CODE-ONLY** |

---

## Top 10 items where we are overclaiming (ordered by user-impact)

1. **Agent-mode consent sheet (A1).** Spec promises a modal with 4 bullets + Cancel/Confirm; ship is an inline color-swap. High-trust surface, and the commit itself admits the downgrade. **User harm: consent theater.**
2. **Chat rich-media image decode (D3/D6).** CLAUDE.md headline feature "Dragon renders code → Tab5 decodes JPEG inline"; actual chat only paints `LV_SYMBOL_IMAGE + alt-text`. User sees "IMAGE · code: python" and no image. **User harm: feature invisible.**
3. **widget_media on home decoded-JPEG (B5).** Claimed verified; only the caption fallback has a known render. No shot of a real image in the live slot. **User harm: fallback path is shipped as the feature.**
4. **Tool-indicator bubbles in chat (D5).** CLAUDE.md says "thinking + tool indicator bubbles"; actual implementation changes a voice-overlay label string. No bubble in chat. **User harm: no context on what the LLM is doing during 30-s tool chains.**
5. **Agent-warning from Settings row (E3).** Tapping TinkerClaw row in Settings hot-switches to voice_mode=3 with zero guardrails. The mode sheet path at least recolors the composite; Settings has nothing. **User harm: easiest path to the bypass has weakest guardrail.**
6. **Onboarding (G).** Called out P0 in UX audit. Zero lines of code. Not even a stub file. Any factory-reset / first-boot user walks into a dark device. **User harm: new-user experience.**
7. **widget_prompt round-trip "end-to-end" (B6/K3).** The claim is the platform works for any skill. Reality: only TimeSense's own handler closes the loop. Skills without `register_action` silently log "no handler". **User harm: misleading capability.**
8. **Dragon crash mid-turn surface (F5).** Spec wants a toast + rose orb pulse. Not implemented at all. **User harm: during crashes user sees silence or generic "Connection lost".**
9. **Full-screen OFFLINE hero (F1/F2).** Only the one-word pill exists. Spec wanted a full hero. **User harm: low — the pill is readable — but overclaim nonetheless.**
10. **Presets tab in mode sheet (H5).** Spec mentioned legacy 4 modes as recipe presets; never built. **User harm: low; but PROGRESS implies the sheet is "done".**

---

## Top 5 where we are underclaiming

1. **TC fallback auto-revert + toast.** Wired cleanly (Dragon 5-s health probe → error message → Tab5 auto-sets voice_mode=0 + toast + READY). PROGRESS buries it. Worth a named bullet.
2. **WS socket `SO_KEEPALIVE` + aiohttp `heartbeat=30`.** 43caa60 + a1ccf5f is the quietest fix that had the biggest stability impact; the pair of commits solved the entire "why does Tab5 drop mid-soak" class of bugs. Barely named.
3. **widget_store priority-weighted eviction + `voice_active` reboot grace.** `e48a588` round 2 P1s. Unsexy, never-going-to-shoot-a-screenshot, but raises the floor.
4. **Local midnight rollover code structure** (`year*366 + tm_yday`). Correct timezone-aware canonical day index. Needs observation to promote from CODE-ONLY to VERIFIED, but the code itself is sound.
5. **`mode_manager.c:144-148` "voice WS persists across mode switches".** Called out as a non-negotiable in PROGRESS resume checklist; rarely celebrated as the reason mode swaps feel instant.

---

## Proof-deficit backlog — one-line test per gap

| Gap | Test that closes it |
|---|---|
| TC receipt visible on chat bubble | Do one TC turn; `/screenshot`; expect "…· minimax · FREE" subtitle. |
| TC fallback toast visible | Stop tinkerclaw-gateway; tap Agent from mode sheet; screenshot toast + home pill. |
| widget_media decoded JPEG visible on home | `POST /api/v1/media/upload` a 200×100 jpg; emit `widget_media` pointing at it; screenshot live-slot. |
| widget_card visible in chat | Emit `widget_card` to an open chat session; screenshot chat bubble. |
| widget_card on home (missing render) | Either build the home render path, or mark CARD as chat-only in spec. |
| widget_prompt round-trip for a NON-TimeSense skill | Wire one simple skill that calls `.prompt(...)` + `register_action`; emit; tap; `journalctl -u tinkerclaw-voice` shows dispatch + state change. |
| widget_chart shot | Emit one chart widget; screenshot; save `shots/phase4f-chart.bmp`. |
| Chat session drawer | Open chat → tap header menu → screenshot drawer. |
| Memory screen list | Pre-seed 3 facts via REST; open Memory overlay directly from nav sheet (not via voice); screenshot list. |
| Tool-indicator chat bubble | Build one. Currently only a voice-state string, not a bubble. |
| Dragon crash mid-turn surface | Kill `tinkerclaw-voice` during a TTS; observe + screenshot. Build if absent. |
| Onboarding | No test — build first. |
| Agent-warning on Settings row | No test — build first (reuse mode-sheet modal). |
| Daily cap slider shot | Drag slider; screenshot; save `shots/phase4e-cap-slider.bmp`. |
| Nav sheet shot | Tap 4-dot chip; screenshot; save `shots/nav-sheet.bmp`. |
| Mode-sheet reverse-derive on open | Set `vmode=2 llm_model=gemini-...`; open sheet; screenshot; confirm dials reflect reality. |
| OTA check happy path | Publish v0.8.1 bin; tap Check; screenshot "Apply Update" revealed. |
| Local midnight rollover | Set RTC to 23:59:55 local; observe rollover; dump NVS `spent_day` before/after. |

---

## Ordered action list

### Test today (each ≤5 min, closes a live overclaim)
1. **TC receipt minimax bubble** — one TC turn, screenshot. Closes A4 + K TC parity.
2. **widget_media decoded JPEG** — upload a real image, emit widget, screenshot. Closes B5 + top-3 overclaim.
3. **widget_card in chat** — emit `widget_card` on a session, screenshot. Closes B2.
4. **Nav sheet screenshot** — tap 4-dot chip, capture. Closes C6.
5. **Mode-sheet reverse-derive** — set vmode=2 with gemini, open sheet, screenshot. Closes H2 claim.
6. **Daily-cap slider shot** — screenshot the slider at a non-zero value. Closes E2 evidence gap.

### Build tomorrow (each ≤½ day, closes top overclaims)
1. **Real Agent-mode modal** (A1 + H4 + E3). Reuse from spec; insert before `voice_send_config_update(3, …)` in mode sheet AND Settings row.
2. **Chat image decode** (D3 + D6). Extend `chat_msg_view.c` MSG_IMAGE branch to call `media_cache_fetch` + `lv_image_set_src` like `ui_home.c` does for widget_media.
3. **Tool-indicator chat bubble** (D5). On `tool_call`, push a MSG_SYSTEM bubble ("Searching the web…") into chat store; on `tool_result`, replace with a MSG_SYSTEM bubble bearing elapsed ms.
4. **Onboarding carousel** (G). Even a 3-screen MVP closes a P0.

### Defer
1. **TC memory read-through** (top-5 facts as system prompt). Correctly marked optional. Re-evaluate after consent sheet lands.
2. **Dragon crash rose-pulse surface** (F5). Nice-to-have; the existing "Connection lost" pill is serviceable.
3. **Full-screen OFFLINE hero** (F1/F2). Pill is fine for now.
4. **Presets tab in mode sheet** (H5). Triple-dial subsumes this; amend spec.
5. **Auto-rollback crash test** (J3). Only worth the time before a firmware release, not during design sprint.

---

## Note on methodology

This audit reads exactly: (1) the spec files named, (2) `shots/` filenames, (3) code greps on `main/` and `dragon_voice/`, (4) git log of both repos since 2026-04-19. No device was touched. Any item marked VERIFIED is verified **by a screenshot in this folder** or **by a log trace the session transcript captured**. "CODE-ONLY" is not an accusation — it just means the evidence required to promote it to VERIFIED wasn't saved alongside the code. Most of today's ordered action list is exactly that: capture the evidence for the commits you already shipped.
