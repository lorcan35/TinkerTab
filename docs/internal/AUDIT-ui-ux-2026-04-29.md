# Tab5 UI/UX Audit (2026-04-29)

> **Source:** Six parallel audits commissioned after the K144 chain-hardening sweep (PR #326) shipped, in response to user feedback that "navigation feels disjointed, touch interactions are off, the menu doesn't swipe to scroll, overlays appear unexpectedly."  Read-only audits; no code modified during audit.
>
> **Tracking:** TT #327 (chain hardening — closed) + new UI/UX hardening tracker (TBD)
>
> **Plan:** `docs/PLAN-ui-ux-hardening.md` operationalises findings into wave-style PRs.

## Six perspectives

1. **Navigation flow + screen inventory** — paths between every screen, every overlay, every gesture
2. **Touch interaction discipline** — tap targets, debounce, hitbox overlap, race conditions
3. **Visual consistency + UX uniformity** — typography, color tokens, spacing, animation, header conventions
4. **Accessibility (a11y)** — contrast, legibility, motor / vision / cognitive support
5. **Information architecture + mental models** — does system structure match user expectations
6. **Error handling + recovery UX** — failure modes, communication, recovery paths

## Verification status (2026-04-30)

All 16 cross-audit P0s independently verified by reading the cited file:line refs and computing values where claimed.  Code-level confirmations recorded inline below; runtime confirmations spot-checked on hardware (see `/tmp/verify-navsheet.png` for #4 visual proof — Focus tile is clearly clipped 134 px below the sheet edge).  No agent claims invalidated.

| # | Code-level | Runtime-level | Notes |
|---|---|---|---|
| 1 | ✓ verified `ui_theme.c:14`, `ui_sessions.c:97-103`, `chat_session_drawer.c:38-41` | (would need vmode=4 session) | Stale `[4]` arrays + `default → 0x5C5C68 grey` switch confirmed |
| 2 | ✓ verified `ui_keyboard.c:247` unconditional `lv_obj_clear_flag(mic_btn, HIDDEN)` | (would need: keyboard close → home → screenshot) | mic-btn parents to lv_layer_top |
| 3 | ✓ verified `debug_server.c:163-175` 4 separate `volatile` globals, no atomicity | (already symptomatic via story_onboard step-9 flake) | |
| 4 | ✓ verified math: 4 rows × 184 + 3 × 16 + 170 = 954 > 820 sheet height | ✓ confirmed via `/tmp/verify-navsheet.png` — Focus tile bottom row clipped | Comment at L197 stale ("3 rows" — was true at 9 tiles) |
| 5 | ✓ Settings `TAB_HYBRID = 0xF59E0B` ≠ theme `TH_MODE_HYBRID = 0xEAB308`; `TAB_ONBOARD = 0x8B5CF6` ≠ `TH_MODE_ONBOARD = 0x8E5BFF`; `COL_CYAN = COL_AMBER = 0xF59E0B` (alias bug) in `ui_notes.c:44-45` | (visible if user toggles modes) | |
| 6 | ✓ verified by `grep -rln ui_fb_ main/*.c` — only `ui_camera.c` (5 sites) + `ui_feedback.c` itself | (visible — taps without pressed state feel dead) | |
| 7 | ✓ contrast computed: TH_TEXT_DIM 0x444444 on TH_BG 0x08080E ≈ 2.11:1.  WCAG 1.4.3 needs 4.5:1.  FAIL | (visible in low light) | |
| 8 | ✓ comments at `ui_voice.c:49-61` openly admit `VO_CYAN/PURPLE/GREEN` are all amber variants | (visible — screenshots indistinguishable) | |
| 9 | ✓ verified `ui_home.c:1562-1577 orb_long_press_cb` writes NVS + sends WS update, no undo | (testable: long-press orb on home → state flips immediately) | |
| 10 | ✓ verified two paths: orb LP → `tab5_settings_set_voice_mode()` (`ui_home.c:1568`) AND dial sheet → `tab5_settings_set_int_tier/voi_tier/aut_tier` + derived voice_mode (`ui_mode_sheet.c:478-485`) | | |
| 11 | ✓ verified `ui_onboarding.c` mentions network only in copy text; never collects credentials | (confirmable via reflashed factory device) | |
| 12 | ✓ verified `voice_onboard.c:95/112/155` — `M5_FAIL_UNAVAILABLE` set with **no** toast/banner/obs surface | (testable: unplug K144 + boot → check /m5 vs UI) | |
| 13 | ✓ verified `voice.c:2318` "WS send failed — continuing SD recording" is `ESP_LOGW` only; `ui_notes.c:1343/1464` likewise | (testable: dictate, drop Wi-Fi mid-flight) | |
| 14 | ✓ verified `main.c:603` `tab5_ota_mark_valid()` runs early; `ota.c:344` only logs.  No NVS persistence of attempted-version.  No on-rollback-boot UI surface | | |
| 15 | ✓ verified `voice.c:2104-2107` increments + logs only; `voice.c:723-724` `auth_failed worker` logs only | | |
| 16 | ✓ verified `voice.c:986` writes `voice_set_state(IDLE, err_buf)`; `ui_voice.c` is the ONLY consumer of the detail string and renders only when overlay visible | | |

**All 16 P0s stand.**  No false positives.  Plan is sound.

---

## Headline P0 findings (cross-audit cluster)

| # | Issue | Audits | Severity |
|---|---|---|---|
| 1 | **Stale `[4]` mode-tint arrays leak Onboard.**  `ui_theme.c:14-17`, `ui_sessions.c:97-103`, `chat_session_drawer.c:38-41`.  vmode=4 sessions show grey/UB tag today. | Visual | P0 |
| 2 | **Floating mic button leaks across screens.**  `ui_keyboard.c:247` un-hides the mic regardless of current screen → shadows home menu chip after any keyboard close. | Nav, Touch | P0 |
| 3 | **Touch-after-navigation race.**  `s_inject_*` globals not atomic; real-finger overlap with inject window drops first tap or lands wrong target. | Touch | P0 |
| 4 | **Nav-sheet 10-tile grid overflows sheet height.**  Bottom-row "Focus" tile partially clipped; sheet not scrollable.  Users cannot physically reach Focus. | Nav | P0 |
| 5 | **Color tokens duplicated + drifting** across 5 screens.  Settings `TAB_HYBRID` is amber, theme `TH_MODE_HYBRID` is yellow — different colours for the same mode.  `COL_CYAN = COL_AMBER` literal alias bug in `ui_notes.c`. | Visual | P0 |
| 6 | **`ui_feedback` adoption is 5%** (only camera).  Every other tap has no pressed state → "dead" feel. | Touch, Visual | P0 |
| 7 | **`TH_TEXT_DIM` 0x444444 fails WCAG 1.4.3** (1.98:1 vs 4.5:1 needed).  Half the orientation copy on home is illegible in low light.  Single-token edit fixes. | A11y | P0 |
| 8 | **Voice-state ball is 4 amber shades** — colorblind users + screenshots can't distinguish LISTENING/PROCESSING/SPEAKING/READY.  WCAG 1.4.1 violation. | A11y | P0 |
| 9 | **Orb long-press = irreversible 5-mode cycle, no undo.**  Cost-bearing change ($0.03-0.08/turn for Cloud) on accidental hold. | A11y, IA | P0 |
| 10 | **Two parallel mode-control systems coexist** — orb long-press cycle (`vmode 0-4`) AND dial sheet (`int_tier × voi_tier × aut_tier`).  Both write same NVS via different math; user can't tell source of truth.  K144 mode unreachable from dial sheet. | IA | P0 |
| 11 | **Brand-new user cannot reach Wi-Fi setup before hitting Home.**  Onboarding never collects credentials.  CLAUDE.md "5-min conversation" goal structurally unreachable on factory hardware. | IA | P0 |
| 12 | **K144 failover silently flips UNAVAILABLE for rest of session** on warm-up fail.  User in vmode=4 only learns when next turn fails with generic toast. | Error | P0 |
| 13 | **Mid-dictation Wi-Fi drop has no UI surface.**  SD fallback works but user only sees waveform freeze; recording sits anonymous. | Error | P0 |
| 14 | **OTA rollback is mute on next boot.**  User boots into old version with no banner / toast / explanation. | Error | P0 |
| 15 | **WS auth 401 (5x) stops retry silently** — `voice.c:2105` log-only.  User has no way to know without `/voice` debug. | Error | P0 |
| 16 | **Device-evicted only visible if voice overlay is open.**  Otherwise user discovers it 20 min later when nothing works. | Error | P0 |

## Headline P1 findings

| # | Issue | Audits | Where |
|---|---|---|---|
| 7 | Chat overlay doesn't `clear_flag(GESTURE_BUBBLE)` — swipes inside chat leak to home's gesture handler | Nav | `ui_chat.c:538` |
| 8 | 67 direct `lv_font_montserrat_NN` references across 10 files bypass the `FONT_*` scale | Visual | wifi/files/audio/camera/voice/sessions/memory/agents/focus/notes |
| 9 | `DPI_SCALE` is dead — only `ui_theme.h` uses it; every screen hardcodes raw 720×1280 px | Visual, Touch | system-wide |
| 10 | 5 different overlay-header conventions — Chat 96-px hero / Settings 48-px / Sessions/Memory/Agents no header | Visual | system-wide |
| 11 | Long-press orb on home has no swallow-flag — borderline 380-450 ms taps fire both CLICKED and LONG_PRESSED, accidentally cycling voice mode | Touch | `ui_home.c:514-515` |
| 12 | Pill says `OR SAY "DRAGON"` — wake-word was deleted in #162, label is a lie | Nav, Visual | `ui_home.c:690` |
| 13 | Component duplication — say-pill in `ui_home.c:648` + `chat_input_bar.c:182` are two implementations of "byte-identical" | Visual | both files |
| 14 | Toast vocabulary inconsistent — `Connected to Dragon` / `Dragon reconnected`; `Mic muted -- unmute to use Onboard` / `Mic is muted -- unmute in Settings` | Visual | 20 strings |
| 15 | Camera REC vs Capture — same shape, only color differs; thumb-rest accidentally triggers REC | Touch | `ui_camera.c:537` |
| 16 | Touch debouncing exists at only 2 sites (orb + `/navigate`); chat orb / mode chip / suggestions / settings rows / capture / REC unprotected | Touch | system-wide |
| 17 | `ui_home_get_tileview` / `ui_home_get_tile` are dead API; CLAUDE.md still says "4-page tileview" | Nav | `ui_home.c:1895-1900` |
| 18 | Multiple buttons below `TOUCH_MIN=60` — camera back 80×48, files back 56×48, settings back 140×48 | Visual, Touch | various |
| 19 | 5 different `SIDE_PAD` values across screens (8/20/24/40) — visual jank when swiping between screens | Visual | system-wide |
| 20 | Keyboard slide cadence is now smooth (Wave 6) but the keyboard trigger still occupies a hit-zone class with the home menu chip | Nav, Touch | `ui_keyboard.c:818-826` |

## Audit reports — full content

### Audit 1: Navigation flow + screen inventory

(See `docs/AUDIT-ui-ux-nav-2026-04-29.md` if split out for size.)

#### Screen inventory (27 surfaces total, hub-and-spoke topology)

Home is the only `lv_screen` — every other surface is an overlay, sheet, drawer, or `lv_layer_top` floater.

#### Top findings

- **Discovery dead-ends**: vmode=4 only reachable via Settings → Voice radio (or chat-chip cycle post-Wave-2).  Memory / Agents / Sessions / Focus / Files / Camera reachable only via the nav-sheet, which itself is reached via the 4-dot menu chip on home.
- **Chat overlay missing `GESTURE_BUBBLE` clear** (`ui_chat.c:538`) — swipes inside chat propagate to home's gesture handler and accidentally trigger Settings/Notes/Focus open.
- **Nav-sheet bottom row clipped** — 10 tiles, 3-col grid, 4 rows, sheet height 820 — last row partially below viewport.
- **`ui_keyboard_hide()` un-hides the floating mic button on EVERY call** — leaves a redundant orb floating on home after any keyboard close.
- **Five back-affordance vocabularies** — chat top-left chevron only, settings X + chevron + swipe-RIGHT, sessions/memory/agents back-button + swipe-RIGHT-OR-BOTTOM, focus back-button + swipe-DOWN, camera/files back-button only.

### Audit 2: Touch interaction discipline

#### Top findings

- **Touch-after-nav race** — `s_inject_*` globals not atomic; on a `/navigate`, the 100 ms RELEASED inject window can shadow a real finger press.  Fixes: atomic-CAS the inject state into a single struct read with sequence counter; refuse the override when `tab5_touch_read()` reports a real touch.
- **Hitbox overlap K144 keyboard trigger ↔ home menu chip** at `(640, 1136)` vs `(572-628, 1188-1244)`.  Mitigated by hide-on-home but the floating-mic-button leak (P0 #2) re-arms it.
- **Long-press orb** has no swallow-flag — adopt the `s_now_card_long_pressed` pattern from `ui_home.c:1594-1620`.
- **Touch debouncing** only on home orb + `/navigate` HTTP; missing everywhere else.  Universal `ui_tap_gate(site, ms)` helper recommended.
- **`ui_feedback` adoption** is 5% (camera only); module exists but is unused.
- **Tap targets below `TOUCH_MIN`**: chat header back/chev/plus all `HDR_TOUCH=44` (= 44 px raw, below 60).  Camera back 80×48, files back 56×48, settings back 140×48.

### Audit 3: Visual consistency + UX uniformity

#### Top findings

- **Stale 4-entry mode arrays**: `ui_theme.c:14-17`, `ui_sessions.c:97-103`, `chat_session_drawer.c:38-41` — Onboard mode (vmode=4) regression that ships TODAY.
- **67 direct `lv_font_montserrat_NN` refs** across 10 files bypass `FONT_*` scale.
- **Color tokens duplicated in every overlay's prologue** with drift — `ui_files.c:30-39`, `ui_notes.c:41-53`, `ui_camera.c:33-39`, `ui_settings.c:51-82`, `ui_voice.c:50-68`.  `COL_CYAN = 0xF59E0B = COL_AMBER` is a literal alias bug in `ui_notes.c`.
- **`DPI_SCALE` is dead** — only `ui_theme.h` itself uses it.  Every screen sets sizes/positions in raw 720×1280 px.  The "portable" abstraction is fiction.
- **Five header conventions** across overlays.  Chat 96-px hero / Settings 48-px / Camera 48-px translucent / Files 48-px topbar / Sessions/Memory/Agents no header bar.  Same app, five different OS feels.
- **20 distinct toast strings** with vocabulary drift — different verbs and tenses for same events; long reconnect string overflows the 32-char target.
- **Mode-chip duplicated three times** — home chip, chat chip, drawer chip — three implementations of the same `8×8 dot + label + sub` recipe.
- **Animation cadence inconsistent** — spring-engine math right but adopted at only 3 sites; `lv_anim` linear paths at 9 sites; voice-overlay loops 1500-2000 ms (5× over the spec'd 150-300).

### Audit 4: Accessibility (a11y)

#### Top findings

- **P0 — `TH_TEXT_DIM` (#444444) on `TH_BG` is 1.98:1**, fails WCAG 1.4.3 (needs 4.5:1).  Used in mode-chip subtitle, chat-input ghost ("Hold to speak · or type"), chevron glyph in chat header, OFFLINE hero body.  Half the orientation copy on home is illegible in low light.  Fix: replace with `#9A9AA3` (~6.3:1) — single token edit in `ui_theme.h:22`.
- **P0 — Voice-state ball is 4 amber shades.**  LISTENING/PROCESSING/SPEAKING/READY all live in the amber family; comment at `ui_voice.c:43-44` openly says "state is conveyed by motion, not hue".  WCAG 1.4.1 (Use of Color) violation.  8% of M users colorblind; motion-only states don't survive screenshots.
- **P1 — No "reduce motion" toggle.**  Boot orb breathe (1500 ms loop), mode-dot SPRING_BOUNCY pulse with undershoot below baseline, toast SPRING_SNAPPY 50-px slide-in, orb size morphs 240↔340 px on every state change.  WCAG 2.3.3 violation.
- **P0 — Orb long-press = irreversible 5-mode cycle with no undo.**  `ui_home.c:1562 orb_long_press_cb` writes NVS, sends `voice_send_config_update`, shows a 2.2 s toast that says only "Mode: Cloud".  Cost-bearing change ($0.03–0.08/turn for Cloud) with no confirmation.  WCAG 3.3.4 (Error Prevention).
- **P1 — Touch target violations**: voice-overlay close 56×56 (under Tab5 60 px floor), mic dot 12×12 (decoration but state-cue), 30+ uses of `FONT_SMALL` 14 px below ISO 9241 minimum at arm's length.
- **P1 — Toast auto-dismiss 2.2 s** is too short for slow readers reading the 30-char "Reconnecting to Dragon… try again in a moment." string.  WCAG 2.2.1 (Timing Adjustable).  Voice-overlay timer scales by length (4-15 s) — that's the right pattern; copy it.
- **P1 — Nav bar is NOT always visible** (CLAUDE.md says it is; reality is `ui_nav_sheet` is opened on demand via the home menu chip).  On Camera/Files/Notes, no visible "home" affordance.  Tired user has to find the chip OR power-cycle.
- **P1 — Errors aren't actionable.**  "Disconnected", "Response timed out" don't say "Tap to retry".

#### Top a11y fixes

1. Replace `TH_TEXT_DIM` 0x444444 with `#9A9AA3` (single token edit, lifts ~10 site-wide labels above WCAG)
2. Add per-state icon glyph + state word to voice orb (mic / hourglass / speaker / check)
3. Pin a persistent home button on `lv_layer_top()` for the lifetime of the app

### Audit 5: Information architecture + mental models

#### Top findings

The disjointed-navigation feeling is **structural, not visual**.  IA is the residue of three overlapping design eras (rail-era → menu-chip era → Sovereign-Halo era), and three orthogonal conceptual systems are compressed onto one display without clarifying which is dominant:

- **P0 — Two parallel mode-control systems coexist.**  `vmode 0–4` long-press cycle (`ui_home.c:1562`) vs. the 3×3×2 Sovereign-Halo dial sheet (`ui_mode_sheet.c:114-135`).  They write the same NVS state via different math; user has no idea which is source of truth.  K144 mode (4) only reachable via Settings radio — the dial sheet *cannot* express it.  Pick one; suggest dial-sheet-only with K144 as `aut_tier=2`.
- **P0 — The orb is overloaded across three locations and four behaviours**: home tap, home long-press, chat ball tap, voice-overlay states.  Same shape, four contracts, branching invisibly on `voice_mode` / `chain_active`.
- **P0 — A brand-new user cannot reach Wi-Fi setup before hitting Home.**  Onboarding (`ui_onboarding.c:46-80`) is a 3-card carousel that NEVER collects credentials.  Wi-Fi defaults are compile-time from `config.h:35-40`.  CLAUDE.md's "5-minute conversation" goal is unreachable on factory hardware.
- **P1 — Navigation surface split across 4 entry points** (4-dot menu chip → nav sheet, edge swipes left/right/up, orb on home, debug `/navigate`) with overlapping but non-identical destinations.
- **P1 — Mode/model coupling undocumented in UI.**  `llm_mdl` only matters in vmode=2; in 0/1/3/4 the picker either does nothing or is silently overridden.  Settings exposes both as siblings with no conditional disclosure.
- **P1 — Hierarchy unclear.**  "Send a message" has 4 paths; "see what AI just did" has 3 paths; "switch model" has 4 paths.
- **P1 — 6 hidden gestures users must memorize.**  Swipe up/right/left on home, long-press orb, long-press now-card, long-press mode chip.  Not discoverable.
- **P1 — "Memory", "Agents", "Focus" tile names lack information scent.**  Engineers know what they mean; users don't.

#### Top IA fixes

1. **Collapse the dual mode system** — delete orb long-press cycling, make mode-chip long-press → dial sheet the only entry point, add K144 to dial sheet
2. **Insert Wi-Fi credential step in onboarding** before the welcome carousel; gate `set_onboarded(true)` on Dragon round-trip success
3. **Group nav sheet into Do / See / Tune sections** + merge Sessions into Chat (chevron access only)

### Audit 6: Error handling + recovery UX

#### Top findings

- **P0 — K144 failover gate flips UNAVAILABLE silently for the rest of the session** when boot warm-up fails.  `voice_onboard.c:94/108` logs `ESP_LOGW` and sets `M5_FAIL_UNAVAILABLE`.  The user is told nothing.  Per CLAUDE.md: "failover restored on reboot" — invisible in UI.
- **P0 — Mid-dictation Wi-Fi drop has no UI surface.**  `voice.c:2318` logs `WS send failed — continuing SD recording`, but the user sees only the live waveform freeze.  CLAUDE.md "offline queue" promise — Notes still save to SD — is buried in the OFFLINE hero.  Recording sits anonymously under `/recordings/`.
- **P1 — All toasts render amber-on-card regardless of severity.**  `ui_home.c:1801-1810` paints every toast with `TH_CARD_ELEVATED` bg, `TH_CARD_BORDER`, `TH_TEXT_PRIMARY`.  25+ sites mix info ("Sent"), warnings ("Mic muted"), and hard errors ("K144 not responding") into one undifferentiated visual class.
- **P1 — OTA rollback is mute on next boot.**  `main.c:600-603` calls `tab5_ota_mark_valid` early; `ota.c:330-346` only logs.  After a real auto-rollback, user boots into the previous firmware with no banner, no toast, no Settings indicator.
- **P1 — 75+ `ESP_LOGW`/`ESP_LOGE` sites have no UI surface.**  Every PSRAM alloc fail, every WS send drop, every parse failure, every recovery escalation goes serial-only.  Users experience these as "the device just rebooted" or "the toast didn't show up."
- **P0 — Wi-Fi wrong-password indistinguishable from transient drop.**  `wifi.c:43-53` logs the reason (WIFI_REASON_AUTH_FAIL=2, NO_AP_FOUND=201, etc.) but never surfaces it.  Tab5 retries forever; user has no diagnostic.
- **P0 — WS auth 401 (5 in a row) stops retry silently.**  `voice.c:2105` — log only.  User has no way to know without `/voice` debug endpoint.
- **P0 — Device-evicted only visible if voice overlay open.**  `voice.c:962-1005` writes the message into `voice_set_state(IDLE, err_buf)` callback's `detail`, which `ui_voice.c` paints only when overlay is up.

#### Best-in-codebase examples (worth copying)

- **K144 chain setup error mapping** (`voice_onboard.c:367-389`) — five distinct esp_err codes → five distinct user-actionable toasts.  This is the bar.
- **Budget cap UX** (`voice.c:1188-1209` + home pill `ui_home.c:983`) — auto-downgrade + toast + persistent pill + Dragon-spoken alert.  All four channels coordinated.
- **Wi-Fi OFFLINE hero** (`ui_home.c:907`) — 20-s debounce, full-screen, plain English, mentions SD-card fallback.
- **SD missing on Files** (`ui_files.c:436`) — full-screen panel + Retry button.

#### Top error-UX fixes

1. Add `tone` arg to `ui_home_show_toast` (info / warn / error → border + accent dot color); wire all sites
2. Add `error.*` obs-event class + a `ui_home_show_error_banner` persistent surface fed by it.  10 call sites cover 80% of silent-failure inventory.
3. OTA rollback acknowledgement on next boot — persist last-attempted version, compare against running, surface one-shot toast

## Highest-leverage moves (consolidated, ranked by user-impact)

These are the moves the 6 audits converge on as highest-impact:

1. **Token system pass + a11y contrast lift** — bump `TH_TEXT_DIM 0x444444 → ~#9A9AA3` (closes WCAG fail #7); promote 3 stale `[4]` mode arrays to `VOICE_MODE_COUNT` (closes visible Onboard regression #1); delete per-file `COL_*` blocks (closes drift #5); add `TH_HAIRLINE`/`TH_BG_TOPBAR`/`TH_ACCENT_BLUE`/`TH_DESTRUCTIVE`/`FONT_DISPLAY (48)`; standardize `SIDE_PAD = 40`.
2. **Toast tone + error obs class** — add `tone` arg to `ui_home_show_toast` (info/warn/error → border + accent dot color, scale dismiss timer to text length); add `error.*` obs-event class; build a `ui_home_show_error_banner` persistent surface for "K144 unavailable", "OTA rollback fired", "device evicted", "auth 401 lock", "SD-write fail" silent failures (#12-#16).
3. **Single source of truth for floating chrome** — drive mic button + keyboard trigger visibility from current screen, not from keyboard's lifecycle.  Pin a persistent home button on `lv_layer_top()` (closes a11y discoverability + #2).
4. **Atomic inject state + drain real touches around `/navigate`** — fixes the touch-after-nav race (#3) that's been the e2e flake all along.
5. **Voice-state orb gets icon + state word** — closes WCAG 1.4.1 (#8) and the affordance-vocabulary IA finding by adding non-color cue.
6. **Universal `ui_tap_gate(site, ms)` helper** + apply to every CLICKED handler that triggers async work / overlay / screen-load (closes touch-debounce gap).
7. **Promote `ui_feedback` adoption** — module exists with one exemplary user (camera); sweep through every clickable element (#6).
8. **Onboarding step 0 = Wi-Fi credentials** — gate `set_onboarded(true)` on Dragon round-trip success.  Closes IA #11 (5-min goal unreachable).
9. **Extract shared widgets** — `widget_say_pill`, `widget_mode_chip`, `widget_overlay_header`, `widget_empty_state`, `widget_loading_pill`, `widget_toast(info/warn/error)`.  Closes component-duplication + 5-different-headers findings.
10. **Collapse dual mode system to one knob** — delete orb long-press cycle (closes accidental-cost-bearing-cycle a11y #9 + IA #10); make mode-chip long-press → dial sheet the only entry point; add K144 to dial sheet as `aut_tier=2`.
11. **Standardize back-gesture vocabulary** — every full-screen overlay accepts swipe-RIGHT as back; clear `GESTURE_BUBBLE` on chat (closes Nav #6 + ergonomic findings).
12. **Swap nav-sheet to 3×3+1 (or scrollable)** + group into Do/See/Tune sections (closes #4 + IA scent).
13. **OTA rollback acknowledgement** — persist last-attempted version, compare on boot, surface one-shot toast (closes #14).
14. **Drop the `OR SAY DRAGON` pill copy** until wake-word is restored.
15. **Kill stale APIs** — `ui_home_get_tileview` / `ui_home_get_tile` (dead post-v4·C); CLAUDE.md "4-page tileview" string.

## What's solid (preserve)

- **Home v4·C / chat v4·C surfaces are well-designed** — mode tints flow, the say-pill is the same pixel-spec across both, the orb is a single visual language.  This is the bar.
- **Hide/show pattern over destroy/create** (CLAUDE.md "Heap Fragmentation Fix") — materially reduces touch-during-destroy race surface.
- **Hub-and-spoke topology** — home is the only `lv_screen`; consistent `if (lv_screen_active() != home) lv_screen_load(home)` guard.
- **Toast lifecycle is robust** — `toast_ctx_destroy` correctly tears down spring + label + timer atomically.
- **Async navigation discipline** — every nav goes through `tab5_lv_async_call` → `async_navigate` with `s_navigating` re-entrancy lock + `screen.navigate` obs event.
- **Now-card long-press / click discrimination pattern** — should be copied to other long-press sites (orb).
- **Voice overlay's instant show/hide** — three crash classes the fade refactor closed; comments document why.
- **Files no-SD empty state** is textbook — icon + title + body + action.  Use as template.
- **`config.h` typography scale + `ui_theme.h` token exports** — well-named, well-documented.  Infrastructure is right; discipline is what's missing.

---

*This doc is the durable record of the audit; the wave-by-wave plan to ship the fixes lives in `docs/PLAN-ui-ux-hardening.md`.*
