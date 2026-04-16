# TinkerTab UI Overhaul — Design Spec

## Goal

Replace the current 4-page tileview home screen and unify the visual language across all screens. The home screen becomes a single-page dashboard with a living orb, agent orchestration card, and activity feed. All screens share a cohesive design system: consistent card styles, spacing, typography, and color language. Tool calls become visible in chat. Agent orchestration gets its own overlay.

## Architecture

Kill the tileview. Home is ONE scrollable page. Notes, Chat, Settings remain as overlays (existing hide/show pattern). Add a new Agents overlay accessible from the home agent summary card. Shared design system defined in `ui_theme.h/c` — static styles, color constants, spacing constants. All screens import the theme.

## Design System Constants

### Colors

```
Background:      #08080E  (not pure black — subtle blue tint)
Card surface:    #111119  (visible against bg)
Card elevated:   #13131F  (agent card, modals)
Card border:     rgba(255,255,255, 0.04)  = #0A0A0E blended
Card border hover: rgba(255,255,255, 0.07)

Text primary:    #E8E8EF  (titles, headings)
Text body:       #AAAAAA  (body content)
Text secondary:  #666666  (metadata, dates)
Text dim:        #444444  (timestamps, placeholders)

Accent amber:    #F59E0B  (orb, active states, user bubbles)
Accent amber dark: #D97706 (gradient end, pressed states)
Accent amber glow: rgba(245,166,35, 0.2) (shadows, ambient)

Mode Local:      #22C55E  (green)
Mode Hybrid:     #EAB308  (yellow)
Mode Cloud:      #3B82F6  (blue)
Mode TinkerClaw: #F43F5E  (rose)

Status green:    #22C55E  (connected, done)
Status red:      #EF4444  (disconnected, error)
```

### Spacing (real device pixels, use DPI_SCALE)

```
Side margins:         24px   (DPI_SCALE(18))
Card gap:             12px   (DPI_SCALE(9))
Card padding:         20px   (DPI_SCALE(15))
Card radius:          20px   (DPI_SCALE(15))
Info card radius:     16px   (DPI_SCALE(12))
Section gap:          20px   (DPI_SCALE(15))
Task row gap:         8px    (DPI_SCALE(6))
Input pill height:    52px   (DPI_SCALE(38))
Input pill radius:    26px   (DPI_SCALE(20))
Nav bar height:       72px   (DPI_SCALE(54))
Status bar height:    40px   (DPI_SCALE(30))
```

### Typography (maps to existing FONT_* defines)

```
Clock:    FONT_CLOCK  (Montserrat 48px) — home screen time
Time big: lv_font_montserrat_40 — time beside orb (add to build if needed)
Title:    FONT_TITLE  (Montserrat 28px) — agent card titles
Heading:  FONT_HEADING (Montserrat 24px) — screen headers, card titles
Body:     FONT_BODY   (Montserrat 20px) — body text, task labels, chat bubbles
Secondary: FONT_SECONDARY (Montserrat 18px) — subtitles, metadata
Caption:  FONT_CAPTION (Montserrat 16px) — timestamps, badges
Small:    FONT_SMALL  (Montserrat 14px) — agent type labels, tool names
Mono:     Use FONT_SMALL for mono-like elements (no actual monospace in LVGL build)
```

### Touch Targets

All interactive elements: minimum `TOUCH_MIN` = `DPI_SCALE(44)` = 60px on Tab5.
- Nav items: 180px × 72px (720/4 wide × nav height)
- Info cards: full width × ~72px
- Agent card: full width × content height (always > 60px)
- Quick action buttons: ~220px × 64px
- Input pill: full width × 52px
- Orb: 88px diameter + 20px ext click area = 128px effective

### Shared Static Styles (defined once in ui_theme.c)

```
s_style_card          — Card surface (#111119), radius 16, pad 16, border 1px
s_style_card_elevated — Card elevated (#13131F), radius 20, pad 20, border 1px, top gradient line
s_style_info_card     — Info card with flex row, pad 16, radius 16
s_style_text_title    — FONT_TITLE, color #E8E8EF
s_style_text_body     — FONT_BODY, color #AAAAAA
s_style_text_meta     — FONT_SMALL, color #666666
s_style_text_dim      — FONT_CAPTION, color #444444
s_style_input_pill    — bg #111119, border, radius 26, pad_left 20
s_style_nav_item      — text center, FONT_SMALL, color #3A3A3A
s_style_nav_active    — color #F59E0B
s_style_bubble_user   — amber gradient bg, black text, radius 22/22/6/22
s_style_bubble_ai     — #111119 bg, #BBBBBB text, radius 22/22/22/6
s_style_tool_card     — #12121E bg, left border amber 3px, radius 4/16/16/4
s_style_toggle_off    — track #111119, knob white
s_style_toggle_on     — track amber 20%, knob white
s_style_slider        — track #111119, fill amber gradient, knob white 11px
```

## Screen 1: Home

### Layout (flex column)

```
┌─────────────────────────────┐
│ Status Bar (40px)           │  Time left, mode pill center, battery right
├─────────────────────────────┤
│                             │
│ Orb Row (100px)             │  [Orb+arc 88px] [Time + date + greeting]
│                             │
│ Agent Card (~200px)         │  Orchestration: tasks, progress, sub-agents
│                             │
│ Info Card (72px)            │  Last note preview
│                             │
│ Info Card (72px)            │  Last chat preview
│                             │
│ Quick Actions (64px)        │  [Camera] [Files] [Timer]
│                             │
├─────────────────────────────┤
│ Input Pill (52px)           │  "Ask anything..." [mic button]
├─────────────────────────────┤
│ Nav Bar (72px)              │  Home | Notes | Chat | Settings
└─────────────────────────────┘
```

### Orb + Arc Ring

- lv_arc widget: outer ring showing agent activity level
  - Track: rgba(255,255,255,0.04)
  - Fill: amber gradient (use lv_arc with #F59E0B indicator color)
  - Range: 0-100, value maps to agent task completion %
  - Arc rotated -90° (starts from top)
  - Animation: smooth value transition on update
- lv_obj circle: inner orb
  - 62px diameter, amber bg (#F59E0B)
  - Shadow: 0 0 40px amber glow (use lv_obj_set_style_shadow_*)
  - NOTE: Shadows are expensive. Use shadow_width=20, shadow_spread=8, shadow_color=amber, shadow_opa=50. Only on the orb — nowhere else.
  - Child: LV_SYMBOL_AUDIO label centered, black text
  - Floating animation: lv_anim on Y pos, ±3px, 4s, ease-in-out, infinite
- Click: tap → voice_start_listening() + ui_voice_show()
- Long press: → voice_start_dictation()

### Agent Orchestration Card

- Elevated card style (s_style_card_elevated)
- Top accent: 1px line across top, mode color (rose for TinkerClaw), use lv_obj border_side TOP
- Header row: [pulsing dot 7px] [HEARTBEAT label FONT_SMALL #888] ... [2 min ago FONT_CAPTION #555]
- Title: "Morning check-in" FONT_HEADING #E8E8EF
- Task list: flex column, gap 8px
  - Each task: [check circle 20px] [label FONT_BODY #AAA] ... [meta FONT_SMALL]
  - Check states: done (green bg 12%, ✓), wip (amber bg 12%, ◐), todo (white bg 4%, ○)
  - Progress bar for WIP tasks: lv_bar, 36px wide, 4px tall, amber fill
- Sub-agents divider: 1px line rgba(255,255,255,0.04)
- Sub-agent rows: [emoji] [name FONT_SECONDARY #777] ... [status FONT_CAPTION #555]

**Data source:** TinkerClaw heartbeat events come via WebSocket as tool_call/tool_result messages. Dragon needs to aggregate these into an `agent_status` message (new protocol addition). If no agent is active, show the card in idle state or hide it.

**When no agent activity:** Replace agent card with a "quiet" card: "All quiet — your agent is standing by" in dim text. Or hide entirely and let info cards take the space.

### Info Cards (Recent Activity)

- Flex row: [icon 40px rounded-12] [body flex-grow] [timestamp]
- Icon backgrounds: amber 8% for notes, blue 8% for chat, purple 8% for camera
- Title: FONT_BODY #D0D0D8, ellipsis overflow
- Subtitle: FONT_CAPTION #555
- Timestamp: FONT_SMALL #444, monospace-like
- Tap: navigates to respective overlay (notes, chat)
- Max 3 recent items shown

### Quick Actions Row

- 3 buttons, flex row, equal width, gap 10px
- bg: #0E0E18, border 1px rgba(255,255,255,0.04), radius 14px
- Icon: FONT_HEADING size, centered above label
- Label: FONT_CAPTION #666
- Height: 64px (above TOUCH_MIN)
- Tap: Camera → ui_camera_create(), Files → ui_files_create(), Timer → voice_send_text("Set a timer")

### Input Pill

- Full width - margins, 52px height, radius 26px (pill shape)
- bg: #111119, border 1px rgba(255,255,255,0.06)
- Placeholder text: "Ask anything..." FONT_BODY #444
- Right: amber mic circle button 40px, gradient bg, shadow
- Tap text area → show keyboard, submit sends via voice_send_text()
- Tap mic → voice_start_listening() + ui_voice_show()

### Nav Bar

- 4 items: Home | Notes | Chat | Settings
- Height: 72px, bg gradient from transparent to #08080E
- Each item: icon (FONT_HEADING size) + label (FONT_SMALL)
- Active: amber color (#F59E0B) + top indicator bar (20px × 3px, amber, rounded, with amber glow shadow)
- Inactive: #3A3A3A
- Position: on lv_layer_top() (same as current — floats above overlays)
- Debounce: 300ms (same as current)
- Icons: use LV_SYMBOL_HOME, LV_SYMBOL_LIST, LV_SYMBOL_NEW_LINE, LV_SYMBOL_SETTINGS

### Status Bar

- Height: 40px
- Left: time (FONT_SECONDARY, white)
- Center: mode pill (rounded 20px, mode color border + bg 8% + text, pulsing dot)
  - Tap: cycles voice mode (same as current privacy_tap_cb)
- Right: battery % (FONT_CAPTION #888), connection dot (6px, green/red)

### Object Count Budget

```
Status bar:     5  (container, time, mode pill, dot, battery)
Orb row:        5  (container, arc, orb, icon, time group)
Time labels:    3  (big time, date, greeting)
Agent card:    16  (container, header×3, title, tasks×4 (icon+label+meta=12 but reuse), divider, sub×2)
Info cards:     8  (2 cards × (container, icon, title, subtitle))
Quick row:      7  (container, 3 buttons × (btn + label))
Input pill:     3  (container, placeholder label, mic button)
Nav bar:        9  (container, 4 items × (icon + label))
                ──
Total:        ~56 objects
```

This is at the limit. Mitigation: Agent card tasks use manual Y positioning inside the card (not individual containers per task row). This saves 4 containers = 4 objects. Revised: ~52 objects.

## Screen 2: Agents Overlay

Opened by tapping the agent summary card on Home. Fullscreen overlay (same hide/show pattern as Chat/Settings).

### Layout

```
┌─────────────────────────────┐
│ Status Bar (40px)           │
├─────────────────────────────┤
│ ← Back    Agents    2 active│
├─────────────────────────────┤
│                             │
│ Agent Card (expanded)       │  Live agent with tasks
│                             │
│ Agent Card (compact)        │  Active agent, fewer details
│                             │
│ Agent Card (dimmed)         │  Idle agent
│                             │
│ Agent Card (faded)          │  Completed agent
│                             │
├─────────────────────────────┤
│ Input Pill (52px)           │
├─────────────────────────────┤
│ Nav Bar (72px)              │
└─────────────────────────────┘
```

### Object Recycling

Same pool pattern as chat_msg_view. Max 3 agent cards rendered, scroll to see more. Each card: ~12 objects expanded, ~5 collapsed.

### Agent Card States

- **Live** (active): full opacity, pulsing rose dot, expanded with tasks
- **Active** (running): full opacity, pulsing dot, may be compact
- **Idle** (waiting): 45% opacity, grey dot, one-line "Waiting for tasks"
- **Completed** (past): 30% opacity, green dot, one-line summary "Done · 4 articles · 12s"

### Data Source

New WebSocket message type from Dragon:
```json
{
  "type": "agent_status",
  "agents": [
    {
      "name": "heartbeat",
      "status": "active",
      "title": "Morning check-in",
      "tasks": [
        {"name": "Check emails", "status": "done", "result": "3 found"},
        {"name": "Draft reply", "status": "wip", "progress": 70}
      ],
      "sub_agents": [
        {"name": "Email Agent", "status": "3 unread scanned"},
        {"name": "Writer Agent", "status": "Drafting..."}
      ]
    }
  ]
}
```

Dragon sends this periodically (every 5s while agents are active, every 60s when idle). Tab5 stores latest state and renders on screen.

**Phase 1 (MVP):** Show a static agent card from the heartbeat tool_call/tool_result messages we already receive. Parse tool names and display as tasks. No sub-agent nesting — just flat task list.

**Phase 2 (future):** Full agent_status protocol from Dragon with real orchestration data.

## Screen 3: Chat (Enhancement)

The chat redesign (Tasks 1-8 from the previous sprint) is already done. This spec adds:

### Tool Call Cards

New message type: `MSG_TOOL_CALL` in chat_msg_store.

Rendering in chat_msg_view:
- Left amber border: 3px, #F59E0B at 40% opacity
- Background: #12121E (slightly elevated)
- Border-radius: 4px left, 16px right (asymmetric — visually distinctive)
- Header row: [tool emoji] [TOOL_NAME in FONT_SMALL #F59E0B at 60%] ... [✓ 234ms in green]
- Body: query/args text, FONT_CAPTION, dim color
- Result: below divider line, FONT_CAPTION, slightly brighter

Data: Already available. Dragon sends `tool_call` and `tool_result` WebSocket messages. voice.c already handles them (show_tool_indicator). Instead of just showing "Searching the web..." text, create a proper tool card.

### Visual Polish

- Update bubble styles to match design system:
  - User: amber gradient bg (#F59E0B → #D97706), black text, border-radius 22/22/6/22
  - AI: #111119 bg, #BBBBBB text, border-radius 22/22/22/6, 1px border
- Timestamps: FONT_SMALL #333, right-aligned for user, left for AI

## Screen 4: Notes (Visual Polish)

Existing ui_notes.c stays. Apply design system:
- Card backgrounds: #111119 (from current COL_CARD #1A1A2E)
- Card radius: 16px
- Card padding: 16px
- Title: FONT_BODY #D0D0D8
- Preview: FONT_CAPTION #666, 2-line clamp
- Badge: voice=amber 6%, typed=blue 6%
- Metadata: FONT_SMALL #555
- Header: "Notes" FONT_HEADING + "+ Record" button (amber bg 8%, amber text)

## Screen 5: Settings (Visual Polish)

Existing ui_settings.c stays. Apply design system:
- Section headers: FONT_SMALL #555, letter-spacing 1.5px, uppercase
- Row height: minimum 52px (above TOUCH_MIN)
- Row text: FONT_BODY #AAAAAA left, value FONT_CAPTION #555 right
- Active values: amber color
- Sliders: lv_slider with amber gradient fill, white 11px knob, shadow
- Toggles: lv_switch styled with amber track when on
- Row dividers: 1px rgba(255,255,255,0.02) — barely visible

## File Structure

### New Files
| File | Responsibility |
|------|---------------|
| `main/ui_theme.h` | Design system constants, color defines, spacing defines |
| `main/ui_theme.c` | Shared static styles init, style accessor functions |
| `main/ui_agents.h` | Agents overlay API |
| `main/ui_agents.c` | Agents overlay with recycled card pool |

### Modified Files
| File | Change |
|------|--------|
| `main/ui_home.c` | Complete rewrite — single page, no tileview |
| `main/ui_chat.c` | Add tool call card rendering, update bubble styles |
| `main/chat_msg_store.h` | Add MSG_TOOL_CALL type |
| `main/chat_msg_view.c` | Add tool card slot rendering |
| `main/ui_notes.c` | Apply theme styles (colors, spacing, radii) |
| `main/ui_settings.c` | Apply theme styles |
| `main/config.h` | Add any new font defines |
| `main/CMakeLists.txt` | Add new .c files |
| `main/voice.c` | Parse agent_status WS messages, feed to ui_agents |

### Unchanged
| File | Why |
|------|-----|
| `main/chat_msg_store.c` | Data layer unchanged (add MSG_TOOL_CALL enum only) |
| `main/chat_header.c` | Reusable, may update colors to match theme |
| `main/chat_input_bar.c` | Reusable, may update colors to match theme |
| `main/chat_suggestions.c` | Reusable, colors update via theme |
| `bsp/tab5/bsp_config.h` | No hardware changes |

## LVGL Best Practices Applied (from Deep Research Report)

1. **Shared static styles** — all styles in ui_theme.c, initialized once at boot (Section 3.4 of report)
2. **Hide/show pattern** — all overlays use LV_OBJ_FLAG_HIDDEN, never create/destroy (Section 1.3)
3. **Object count < 55 per screen** — home ~52, agents uses recycling pool (Section 3.6)
4. **Manual Y positioning** inside agent cards — avoids nested flex overhead (Section 3.5)
5. **DPI_SCALE for all sizes** — portable across display densities (PINGOS guide)
6. **Touch targets ≥ TOUCH_MIN** — all interactive elements (PINGOS guide)
7. **No shadows except orb** — shadow rendering is expensive on ESP32-P4 (Section 9.2)
8. **lv_arc for orb ring** — single widget for animated circular progress (Section 4.1)
9. **Pre-allocated screen objects** — home screen created once at boot (Section 1.3)
10. **Timer management** — pause on hide, resume on show, one-shot with repeat_count=1 (Section 4.5)
11. **Thread safety** — all WS callbacks use lv_async_call (Section 4.4)
12. **WDT feeding** — esp_task_wdt_reset() between heavy UI sections (Section 8.2)
13. **Flex for simple layouts, manual Y for performance** — flex for card list, manual Y inside cards (Section 3.5)
14. **Const styles where possible** — consider LV_STYLE_CONST_INIT for immutable styles (Section 3.4)

## Implementation Order

1. **ui_theme** — design system constants + shared styles (foundation for everything)
2. **ui_home rewrite** — the biggest change, uses all theme styles
3. **ui_agents** — new overlay, uses recycled card pool
4. **Chat tool cards** — enhance existing chat with MSG_TOOL_CALL rendering
5. **Notes/Settings polish** — apply theme colors and spacing
6. **Integration test** — flash, verify all screens, heap stability

## Gap Resolutions (from 50 User Story Validation)

### Empty states
- **Info cards empty:** When no notes exist, show "No notes yet — tap to record" in FONT_BODY #555. When no chat history, show "Start a conversation" in FONT_BODY #555. Same card style but dim text.
- **Agent card empty:** When no agent is active and no recent activity, show: "All quiet" FONT_SECONDARY #444 with a dim orb icon. Card stays visible (not hidden) so layout is stable.

### Agent card limits
- **Max 4 tasks on home card.** If agent has more, show 4 + "and N more..." link in FONT_CAPTION #F59E0B. Tapping the card opens Agents overlay with full list.
- **Primary agent selection:** When multiple agents active, the home card shows the most recently updated one. Other active agents shown as a small "+ 1 more agent" badge below the card.
- **Incremental updates:** Agents overlay diffs incoming agent_status against current state. Only update changed task rows (text + icon swap), don't rebuild the full card. Use lv_label_set_text on existing labels.
- **Long-press on agent card:** Shows a 2-option popup: "View details" (opens Agents overlay) or "Cancel agent" (sends cancel to Dragon). Only for active agents.

### Notes recycling
- **Note card recycling pool:** Max 8 visible note cards, same scroll-recycle pattern as chat_msg_view. Notes data stored in a separate ring buffer or loaded from SD card metadata on demand. This is REQUIRED for 100+ notes.
- **Delete gesture:** Long-press on a note card → slide-right reveal "Delete" button (60px, red bg). Tap to confirm. No swipe-to-delete (too easy to trigger accidentally during scroll).

### Disconnect handling
- **Disconnect banner:** "Dragon disconnected — reconnecting..." slides down from top, 300ms ease-out. Auto-hides when connection restored (slide up 300ms). Banner is red (#EF4444) bg, white text.
- **Session recovery:** When Dragon reconnects, voice.c sends `register` with saved session_id. Dragon resumes conversation. Partial STT text from disconnected session is lost (acceptable — voice overlay shows "Connection lost" and user re-speaks).

### Voice overlay error
- **Error orb:** Orb turns red (#EF4444) with red glow for 2s, then fades back to amber. Shows error text below: "Connection lost" / "Dragon unavailable" / "Voice busy".
- **TTS interruption:** If user taps orb during SPEAKING, TTS stops (existing behavior). The full LLM text is already in the chat store — partial TTS doesn't affect chat display.

### Mode switching safety
- **Deferred mode switch:** If user taps mode pill during PROCESSING/SPEAKING, the switch is queued and applied after the current voice cycle completes (transition to READY). Toast shows "Mode will switch after current response."
- **TinkerClaw agent persistence:** Switching away from mode 3 doesn't cancel active TinkerClaw agents — they run server-side independently. Switching back shows their updated status.

### Battery & power
- **Low battery warnings:** Already implemented (10% toast, 5% critical toast). No changes needed.
- **OTA battery check:** OTA apply blocked if battery < 20%. Show toast: "Charge to 20% before updating." Already handled in ota.c.
- **Auto-sleep:** Not in this sprint. Future: dim backlight after 30s inactivity, off after 2min. Wake on touch. Defined in CLAUDE.md Phase 3 priorities.

### Tool cards in chat
- **Spacing between tool cards:** Standard card_gap (12px) between consecutive tool cards. Same as between any message type.
- **Tool card → agent card connection:** Tool cards in chat show individual tool invocations. The Agents overlay aggregates them into a task-level view. No explicit link between them (would over-complicate the UI).

## What This Does NOT Include

- Desktop SDL2 simulator (separate project)
- esp_lvgl_port migration (too risky to change LVGL integration mid-sprint)
- Custom fonts (stick with built-in Montserrat)
- Image assets (stick with LV_SYMBOL_* icons)
- Dragon-side agent_status protocol (Phase 1 uses existing tool_call messages)
- Screen transitions/animations between screens (keep current instant show/hide)
- Wake word integration
- Camera/Files screen redesign (future sprint)

## Animations & Transitions

All animations use LVGL v9's `lv_anim` API. Durations are in milliseconds. Easing functions reference `lv_anim_path_*` callbacks. "Infinite" means `lv_anim_set_repeat_count(LV_ANIM_REPEAT_INFINITE)`. "Ping-pong" means `lv_anim_set_playback_time()` is set so the animation reverses.

### Home Screen

| Element | Property | From | To | Duration | Easing | Repeat | Notes |
|---------|----------|------|----|----------|--------|--------|-------|
| Orb body | Y position | y-3 | y+3 | 4000ms | ease_in_out | Infinite ping-pong | Floating effect. Use `lv_anim_set_values(orb_y - DPI_SCALE(2), orb_y + DPI_SCALE(2))`. Start on home create, pause on hide. |
| Orb arc ring | Arc value | current | target | 600ms | ease_out | One-shot | Triggered when `agent_status` updates task completion %. Use `lv_anim_set_values(old_pct, new_pct)` with `lv_arc_set_value` as exec_cb. |
| Orb ambient glow | border_opa | 10 (LV_OPA_10) | 90 (LV_OPA_90) | 3000ms | ease_in_out | Infinite ping-pong | Breathing glow around orb circle. Use `lv_obj_set_style_border_opa` as exec_cb. Border color = amber glow. Pause on hide. |
| Agent card | Y position + opacity | y=screen_h, opa=0 | y=target, opa=255 | 300ms | ease_out | One-shot | Entry animation on first home load. Slide up from bottom. |
| Info card (1st) | X position + opacity | x=screen_w, opa=0 | x=target, opa=255 | 200ms | ease_out | One-shot | Slide from right. Starts 350ms after home load (after agent card finishes). |
| Info card (2nd) | X position + opacity | x=screen_w, opa=0 | x=target, opa=255 | 200ms | ease_out | One-shot | Starts 400ms after home load (50ms stagger after 1st info card). |
| Info card (3rd) | X position + opacity | x=screen_w, opa=0 | x=target, opa=255 | 200ms | ease_out | One-shot | Starts 450ms after home load (50ms stagger after 2nd info card). |
| Mode dot (status bar) | opacity | LV_OPA_50 | LV_OPA_COVER | 2000ms | ease_in_out | Infinite ping-pong | Pulsing indicator beside mode pill. Pause when home is hidden. |
| Status text (greeting, time) | opacity | 255 → 0 → 255 | — | 150ms out + 150ms in | ease_out | One-shot per update | Cross-fade on text change. Sequence: animate opa to 0 (150ms), set new text in `ready_cb`, animate opa to 255 (150ms). |

**Implementation pattern for agent card staggered entry:**
```c
// In ui_home_show() — only on first load, not every show
static bool s_entry_done = false;
if (!s_entry_done) {
    s_entry_done = true;
    // Agent card: delay 0
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, agent_card);
    lv_anim_set_values(&a, lv_obj_get_y(agent_card) + DPI_SCALE(40), lv_obj_get_y(agent_card));
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_start(&a);
    // Opacity
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa_inline);
    lv_anim_start(&a);
    // Info cards: delay 350, 400, 450 — similar pattern with lv_anim_set_delay()
}
```

**Pause/resume pattern:**
```c
void ui_home_hide(void) {
    lv_anim_del(s_orb, NULL);       // Stop all orb anims
    lv_anim_del(s_mode_dot, NULL);  // Stop mode dot pulse
}
void ui_home_show(void) {
    start_orb_float_anim();         // Restart
    start_mode_dot_pulse();         // Restart
    start_orb_glow_anim();          // Restart
}
```

### Chat Screen

| Element | Property | From | To | Duration | Easing | Repeat | Notes |
|---------|----------|------|----|----------|--------|--------|-------|
| New message (any) | Y position | y + DPI_SCALE(20) | target y | 200ms | ease_out | One-shot | Slide up from slightly below. Applied in `chat_msg_view` when a new slot is populated. |
| User bubble | X position | screen_w | target x | 250ms | ease_out | One-shot | Slide from right edge. Combined with Y slide for a diagonal entrance. |
| AI bubble | opacity | 0 | 255 | 300ms | ease_in | One-shot | Fade in only, no position change. Keeps the AI reply feeling calm/deliberate. |
| Tool card | height | 0 | content height | 300ms | ease_out | One-shot | Expand from zero height. Use `lv_anim_set_exec_cb` with `lv_obj_set_height`. Content inside is clipped by `LV_OBJ_FLAG_OVERFLOW_VISIBLE=false`. |
| Streaming text | — | — | — | — | — | — | **No animation.** Direct `lv_label_set_text()` on each token. Animation would cause jank at streaming speeds. |
| Scroll to bottom | scroll_y | current | max | 300ms | ease_out | One-shot | Use `lv_obj_scroll_to_y(container, LV_COORD_MAX, LV_ANIM_ON)`. LVGL uses 300ms default for `LV_ANIM_ON`. |

**Tool card expand implementation:**
```c
// When tool_result arrives, animate the card height
lv_obj_set_height(tool_card, 0);  // Start collapsed
lv_obj_remove_flag(tool_card, LV_OBJ_FLAG_HIDDEN);
lv_anim_t a;
lv_anim_init(&a);
lv_anim_set_var(&a, tool_card);
lv_anim_set_values(&a, 0, target_height);
lv_anim_set_time(&a, 300);
lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_height);
lv_anim_start(&a);
```

### Notes Screen

| Element | Property | From | To | Duration | Easing | Repeat | Notes |
|---------|----------|------|----|----------|--------|--------|-------|
| Note card (Nth) | opacity | 0 | 255 | 200ms | ease_out | One-shot | Staggered: delay = N * 100ms. Max stagger 10 cards (1s total). Cards beyond 10 appear instantly. |
| Note delete | X position + opacity | x=0, opa=255 | x=-screen_w, opa=0 | 250ms | ease_out | One-shot | Slide left off screen + fade. In `ready_cb`, call `lv_obj_add_flag(LV_OBJ_FLAG_HIDDEN)` and refresh the list. |

**Stagger cap:** If there are 100 notes, only the first 10 visible cards get staggered animation. The rest are created at full opacity immediately. This prevents a 10-second stagger waterfall.

### Settings Screen

| Element | Property | From | To | Duration | Easing | Repeat | Notes |
|---------|----------|------|----|----------|--------|--------|-------|
| Slider thumb | X position | — | — | — | — | — | LVGL's built-in slider drag handles this. No custom animation needed. |
| Toggle switch knob | X position | off_x | on_x | 200ms | ease_out | One-shot | LVGL `lv_switch` animates the knob internally when `LV_USE_ANIMATION=1`. |
| Toggle track color | bg_color | #111119 | amber 20% | 200ms | ease_out | One-shot | LVGL handles this via transition style on `LV_STATE_CHECKED`. Set `lv_style_transition_dsc_t` with 200ms. |
| Section headers | — | — | — | — | — | — | **No animation.** Static text, no entry effect. |

**Toggle transition setup:**
```c
static const lv_style_prop_t toggle_props[] = {LV_STYLE_BG_COLOR, LV_STYLE_PROP_INV};
static lv_style_transition_dsc_t toggle_trans;
lv_style_transition_dsc_init(&toggle_trans, toggle_props, lv_anim_path_ease_out, 200, 0, NULL);
lv_obj_add_style(toggle, &style_with_transition, LV_PART_INDICATOR | LV_STATE_CHECKED);
```

### Navigation

| Element | Property | From | To | Duration | Easing | Repeat | Notes |
|---------|----------|------|----|----------|--------|--------|-------|
| Screen transitions | — | — | — | — | — | — | **NO animation.** Instant `lv_obj_add/remove_flag(LV_OBJ_FLAG_HIDDEN)`. Screen load animations (`lv_screen_load_anim`) cause frame drops on ESP32-P4 at 720x1280. See LEARNINGS.md entry on screen transition performance. |
| Nav bar active indicator | width | 0 | DPI_SCALE(15) (20px) | 200ms | ease_out | One-shot | The small amber bar above the active nav item. Animate width from 0 to 20px on tab switch. |
| Nav bar icon color | opacity | 0 → 255 on new, 255 → 0 on old | — | 150ms | ease_out | One-shot | Cross-fade: old item fades to #3A3A3A, new item fades to #F59E0B. Use `lv_style_transition_dsc_t` on `LV_STYLE_TEXT_COLOR`. |

**Nav bar transition setup:**
```c
static const lv_style_prop_t nav_props[] = {LV_STYLE_TEXT_COLOR, LV_STYLE_PROP_INV};
static lv_style_transition_dsc_t nav_trans;
lv_style_transition_dsc_init(&nav_trans, nav_props, lv_anim_path_ease_out, 150, 0, NULL);
// Apply to each nav item's style
```

### Global / Shared

| Element | Property | From | To | Duration | Easing | Repeat | Notes |
|---------|----------|------|----|----------|--------|--------|-------|
| Touch feedback (all) | various | — | — | 100ms | ease_out | One-shot | Already implemented in `ui_feedback.c`. Buttons darken, cards lighten border, icons dim. |
| Toast notification | opacity | 0 → 255 | — | 200ms in | ease_out | One-shot | Fade in on show. Auto-dismiss after 3000ms via `lv_timer_create`. Then fade out 300ms, hide in `ready_cb`. |
| Toast dismiss | opacity | 255 → 0 | — | 300ms out | ease_in | One-shot | Triggered by auto-dismiss timer or swipe-up gesture. |
| Disconnect banner | Y position | -DPI_SCALE(40) | 0 | 300ms | ease_out | One-shot | Slides down from above the status bar. Shows "Disconnected" with red bg. Reverse animation (slide up) on reconnect. |

**Toast implementation:**
```c
void ui_toast_show(const char *msg, uint32_t duration_ms) {
    lv_label_set_text(s_toast_label, msg);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    // Fade in
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa_inline);
    lv_anim_start(&a);
    // Auto-dismiss timer
    lv_timer_create(toast_dismiss_cb, duration_ms ? duration_ms : 3000, s_toast);
}

static void toast_dismiss_cb(lv_timer_t *t) {
    lv_timer_del(t);
    // Fade out
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa_inline);
    lv_anim_set_ready_cb(&a, toast_hide_cb);  // lv_obj_add_flag(HIDDEN) in callback
    lv_anim_start(&a);
}
```

### Performance Rules

1. **Never animate opacity on large objects.** Opacity animation on objects larger than ~200x200px forces LVGL to create a temporary draw layer (framebuffer-sized), which exhausts the draw buffer on ESP32-P4. See LVGL report Section 2.4. The orb (62px) and mode dot (7px) are small enough. Toast (full-width) is acceptable because it is only ~40px tall.

2. **Never animate width/height on flex children.** Changing the size of a flex child triggers `lv_obj_update_layout()` on the parent, which recalculates positions for ALL siblings. This causes visible jank with more than 3-4 children. The tool card height animation is safe because tool cards are inside the chat scroll container using manual Y positioning (not flex).

3. **Prefer animating position (x, y) over size.** Position changes are a simple coordinate update with no layout side effects. Size changes may trigger layout, clipping recalculation, and child repositioning.

4. **Keep animations under 400ms.** Longer animations feel sluggish on a device you hold in your hand. The only exceptions are the infinite ambient animations (orb float 4s, orb glow 3s, mode dot pulse 2s) which are intentionally slow for a calm, living feel.

5. **Use ease_out for entrances, ease_in for exits.** Entrances should decelerate into their final position (fast start, gentle landing). Exits should accelerate away (gentle start, fast departure). This matches natural physics expectations.

6. **Only the orb gets a shadow.** Animated shadows are extremely expensive (multiple blur passes per frame). The orb shadow is static (set once, not animated). No other element has a shadow. See LVGL report Section 9.2.

7. **Pause animations on screen hide.** Call `lv_anim_del(obj, NULL)` when hiding a screen. Restart animations in the show function. Running invisible animations wastes CPU cycles and can cause issues if the animated object is accessed during a layout pass on a hidden tree.

8. **Cap staggered animations.** Never stagger more than 10 items. If a list has 100 items, animate the first 10 and show the rest instantly. Total stagger duration should not exceed 1 second.

9. **No animation during two-pass creation.** Settings and other screens using the two-pass creation pattern (with WDT feeding between sections) must not start animations during creation. Start animations only after the screen is fully built and shown.

10. **Use lv_anim_del before lv_anim_start.** Always delete any existing animation on the same object+property before starting a new one. Stacking duplicate animations causes memory leaks and erratic behavior.

## User Story Validation (50 Stories)

### First Boot / Onboarding

**US-01: First boot with no WiFi configured**
Scenario: User powers on Tab5 for the first time, no WiFi credentials in NVS.
Action: Device boots, splash screen shows, then home screen loads.
Expected: Home screen renders with all elements. Mode pill shows "Local". Connection dot is red. Orb floats normally. Agent card shows "All quiet" idle state. Input pill is tappable but tapping mic shows toast "No connection -- recording locally". Info cards show empty state ("No notes yet", "No chats yet").
Gap: Spec does not define empty state text for info cards when no notes/chats exist. Need placeholder text and styling.

**US-02: First boot connects to WiFi successfully**
Scenario: WiFi credentials are pre-configured (flashed or set via serial). Device boots and connects.
Action: Device auto-connects WiFi, then auto-connects Dragon WebSocket.
Expected: Connection dot transitions from red to green. Mode pill updates to reflect current voice mode from NVS (default: Local). No toast or banner -- silent connection is the happy path.
Gap: None.

**US-03: First boot with WiFi but Dragon unreachable**
Scenario: WiFi connects but Dragon server at 192.168.1.91 is offline.
Action: Device boots, WiFi connects, WS connection fails.
Expected: Connection dot stays red. Disconnect banner slides down from top: "Dragon offline -- local only". Orb still floats. Home screen fully functional. Tapping mic records to SD card (offline fallback). Reconnect watchdog retries every 10s with exponential backoff.
Gap: Spec does not mention the disconnect banner text content or behavior when Dragon comes back online (banner should slide up and disappear).

**US-04: User taps every element on home screen after boot**
Scenario: Fresh boot, everything connected, user explores the UI.
Action: User taps: orb, mode pill, agent card, info card (notes), info card (chat), Camera quick action, Files quick action, Timer quick action, input pill text area, input pill mic button, each nav bar item.
Expected: Each element responds with touch feedback (100ms ease-out from ui_feedback.c). Orb opens voice overlay. Mode pill cycles voice mode. Agent card opens Agents overlay. Info cards navigate to respective screens. Quick actions trigger their handlers. Input pill shows keyboard or starts voice. Nav items switch screens instantly.
Gap: None.

**US-05: Home screen renders within object budget after boot**
Scenario: Device boots with 2 active agents, 3 info cards, all UI elements.
Action: Home screen loads.
Expected: Total LVGL object count is <=55. Verify with `lv_obj_get_child_count()` recursive check. No heap allocation failures. Largest free internal SRAM block > 30KB.
Gap: None -- object budget is specified (52 objects). But spec should note how to verify at runtime (add a debug log on home create showing object count).

### Voice Interaction from Home

**US-06: Tap orb to start voice**
Scenario: Home screen visible, Dragon connected, idle state.
Action: User taps the orb.
Expected: Voice overlay appears (instant show, no screen transition animation). Mic starts streaming. Orb in voice overlay animates. Home orb animations are paused (deleted via `lv_anim_del`).
Gap: None.

**US-07: Tap orb while AI is speaking (interrupt)**
Scenario: AI TTS audio is playing through speaker after a voice query.
Action: User taps the orb on the home screen (or voice overlay).
Expected: TTS playback stops immediately (`voice_cancel()`). Voice overlay shows, mic starts listening for new input. Previous response is preserved in chat history.
Gap: Spec does not explicitly state what happens to the interrupted TTS response in chat. Should the partial response be kept? The chat should show the full LLM text (which arrived before TTS started) regardless of playback interruption.

**US-08: Long press orb for dictation**
Scenario: Home screen, Dragon connected.
Action: User long-presses the orb (>500ms hold).
Expected: Voice overlay opens in dictation mode. Label shows "DICTATION". Waveform visualization active. Auto-stop after 5s silence. Dragon sends `dictation_summary` with title and summary.
Gap: None.

**US-09: Voice query with tool calls**
Scenario: User asks "What's the weather in Dublin?"
Action: User taps orb, speaks query, Dragon processes with web_search tool.
Expected: Voice overlay shows listening, then processing. Tool call indicator shows "Searching the web...". After response, TTS plays. Chat history shows: user bubble with transcription, tool card with web_search details (query, execution time, result summary), AI bubble with spoken response.
Gap: None.

**US-10: Multiple tool calls in one turn**
Scenario: User asks "Search for ESP32-P4 specs and also check my emails."
Action: Dragon invokes web_search then email_check sequentially.
Expected: Chat shows two tool cards stacked vertically, each with their own expand animation (300ms ease-out). First card appears, then second card appears 300ms later. Both show tool name, args, execution time, and result. AI response bubble appears after both tools complete.
Gap: Spec does not address vertical spacing between consecutive tool cards. Should use the standard `card_gap` (12px) between tool cards.

**US-11: Voice query fails (Dragon timeout)**
Scenario: User taps orb, speaks, but Dragon does not respond within timeout (5min local, 1min cloud).
Action: Timeout fires.
Expected: Voice overlay shows error state. Toast appears: "Response timed out". Voice overlay auto-hides after 3s. Chat shows user bubble with transcription but no AI response. Status returns to idle.
Gap: Spec does not define error state visuals for the voice overlay orb. Should the orb turn red? Flash? The voice overlay error state needs visual definition.

**US-12: Dragon disconnects mid-conversation**
Scenario: User is mid-voice-query, WS connection drops (Dragon restart, network glitch).
Action: WebSocket `on_close` fires during active voice session.
Expected: Voice overlay shows "Connection lost". Mic stops streaming. Disconnect banner slides down on home screen. Reconnect watchdog activates (10s→20s→40s→60s backoff). If reconnection succeeds within the session, resume. If not, show toast "Session ended -- Dragon reconnected" when connection restores.
Gap: Spec does not define what happens to the in-progress voice session data. Should the partial audio be saved to SD? Should the STT text accumulated so far be preserved? Need a "session recovery" strategy.

**US-13: Voice input with no WiFi**
Scenario: WiFi is disconnected or unreachable. User taps orb.
Action: User taps orb.
Expected: Offline fallback activates: recording starts to SD card via Notes module. Voice overlay shows "Recording offline" label. No streaming to Dragon. When WiFi restores, queued recordings could be synced (future feature).
Gap: None -- offline fallback is documented in voice pipeline spec.

### Chat Interaction

**US-14: Open chat from nav bar**
Scenario: Home screen visible.
Action: User taps Chat nav item.
Expected: Chat overlay shows instantly (no animation). Nav bar Chat icon turns amber, active indicator bar animates width from 0 to 20px (200ms ease-out). Home icon fades to #3A3A3A (150ms). Previous chat messages are visible (persistence across close/open). Scroll position preserved.
Gap: None.

**US-15: Send text message in chat**
Scenario: Chat screen open, keyboard visible.
Action: User types "Hello" and taps Done.
Expected: Keyboard submits (Done key fires `LV_EVENT_READY`). User bubble slides in from right (250ms ease-out) with amber gradient background and black text. Chat auto-scrolls to bottom (300ms `LV_ANIM_ON`). Dragon processes and AI bubble fades in (300ms ease-in) with response.
Gap: None.

**US-16: Receive streaming AI response**
Scenario: User sent a query, Dragon is streaming LLM tokens.
Action: Tokens arrive via WebSocket `llm` messages.
Expected: AI bubble appears with fade-in animation (300ms). Text updates directly via `lv_label_set_text()` with no animation per token (performance rule). Chat auto-scrolls as text grows. Status bar shows "Processing..." or "Speaking..." as appropriate.
Gap: None.

**US-17: View tool card in chat**
Scenario: AI used a tool during response generation.
Action: `tool_call` and `tool_result` WebSocket messages arrive.
Expected: Tool card appears in chat between user and AI bubbles. Card expands from 0 height (300ms ease-out). Shows tool emoji, tool name in amber, execution time in green, query/args in dim text, result below divider. Left amber border 3px.
Gap: None.

**US-18: Scroll through long chat history**
Scenario: 50+ messages in chat history.
Action: User scrolls up through history.
Expected: Smooth scrolling via LVGL scroll. Object recycling via `chat_msg_view` pool pattern keeps rendered objects manageable. Older messages are loaded as user scrolls up. No jank or frame drops.
Gap: None -- existing chat_msg_view handles recycling.

**US-19: New Chat button clears history**
Scenario: Chat has existing conversation.
Action: User taps "New Chat" button in chat header.
Expected: Chat messages are cleared. `clear_history` message sent to Dragon. New session starts. Chat shows empty state. Input pill is focused.
Gap: None.

**US-20: Rich media message (code block image)**
Scenario: AI response contains a code block. Dragon renders it as JPEG and sends `media` WS message.
Action: Media message arrives with image URL.
Expected: Placeholder bubble appears. Background task downloads JPEG from Dragon. Once loaded, placeholder swaps to `lv_image` via `lv_async_call`. Image displays inline in chat. `text_update` message follows to strip code from text bubble.
Gap: None.

**US-21: Chat with keyboard obscuring input**
Scenario: User taps input area, on-screen keyboard appears.
Action: Keyboard slides up.
Expected: Chat content area shrinks to accommodate keyboard. Input field remains visible above keyboard (fix from April 2026). Text being typed is always visible. Scroll position adjusts so latest messages remain visible.
Gap: None.

### Agent Orchestration Viewing

**US-22: View agent card on home screen (active agent)**
Scenario: TinkerClaw has an active heartbeat agent running a morning check-in.
Action: Home screen loads with `agent_status` data.
Expected: Agent card shows elevated style. Top accent line in rose (TinkerClaw mode). Pulsing dot (7px, opacity 50-100%, 2s ping-pong). "HEARTBEAT" label. Title "Morning check-in". Task list with check states (done=green, wip=amber, todo=white). Progress bar for WIP tasks. Sub-agent rows below divider.
Gap: None.

**US-23: Tap agent card to open Agents overlay**
Scenario: Agent card visible on home.
Action: User taps the agent card.
Expected: Agents overlay appears (instant show). Back button visible. Header shows "Agents" and count of active agents. Expanded view of current agent with full task list. Other agents shown in compact/dimmed/faded states.
Gap: None.

**US-24: Agent card with 10+ tasks**
Scenario: A complex agent has 10 tasks in its task list.
Action: Home screen renders agent card.
Expected: Agent card on home screen shows max 4 tasks with a "+6 more" label at the bottom (truncation). Tapping the card opens the Agents overlay where all 10 tasks are visible in a scrollable list. Card height on home is capped to prevent it from pushing info cards off screen.
Gap: Spec does not define a max visible task count for the home agent card or truncation behavior. Need to add: max 4 tasks on home card, "+N more" link, full list in Agents overlay.

**US-25: Agent transitions from active to completed**
Scenario: Heartbeat agent finishes all tasks.
Action: `agent_status` update arrives with status "completed".
Expected: Pulsing dot stops, changes to static green. Task items all show green checkmarks. Card opacity reduces to 30%. Title shows completion summary "Done -- 4 tasks -- 12s". Card moves to bottom of list in Agents overlay.
Gap: None.

**US-26: No active agents (idle state)**
Scenario: No TinkerClaw agents are running.
Action: Home screen loads.
Expected: Agent card area shows idle card: "All quiet -- your agent is standing by" in dim text (#444). No pulsing dot. No task list. Card uses standard surface color (not elevated). Tapping still opens Agents overlay showing completed agents history.
Gap: None.

**US-27: Agent status updates while viewing Agents overlay**
Scenario: User has Agents overlay open. New `agent_status` arrives via WebSocket.
Action: WebSocket callback fires with updated task data.
Expected: Agent cards in overlay update in real-time via `lv_async_call`. Task statuses change (todo->wip->done). Progress bars animate. New tasks appear. No full refresh -- incremental update of changed elements only.
Gap: Spec does not detail incremental update strategy for the Agents overlay. Full redraw on every `agent_status` message could cause flicker. Need to specify diffing: update only changed task rows, add new rows, remove completed ones.

**US-28: Multiple agents running simultaneously**
Scenario: Two TinkerClaw agents active: heartbeat (checking emails) and writer (drafting a blog post).
Action: Home screen and Agents overlay render.
Expected: Home screen shows the primary (most recent) agent card. Agents overlay shows both: heartbeat as "Live" expanded, writer as "Active" compact. Object recycling limits to 3 agent cards rendered. Scrolling reveals additional agents.
Gap: Spec does not define which agent is "primary" for the home card when multiple are active. Suggest: most recently updated agent gets the home card.

### Notes

**US-29: View notes list with 5 notes**
Scenario: User has 5 saved notes.
Action: User taps Notes nav item.
Expected: Notes screen shows instantly. 5 note cards appear with staggered fade-in (100ms delay between cards, total 500ms). Cards show title, 2-line preview, badge (voice/typed), metadata. "+ Record" button in header.
Gap: None.

**US-30: View notes list with 100 notes**
Scenario: Power user has 100 notes saved on SD card.
Action: User taps Notes nav item.
Expected: First 10 notes fade in with stagger (1s total). Remaining 90 appear instantly at full opacity. List is scrollable. Object recycling pattern should be used (similar to chat_msg_view) to keep object count manageable. Max ~20 note card objects in the pool, recycled on scroll.
Gap: Spec does not mention object recycling for notes. With 100 notes at ~4 objects each = 400 objects, far exceeding budget. Need to add: note card recycling pool, max 15-20 visible cards, recycle on scroll like chat.

**US-31: Delete a note**
Scenario: User wants to delete a note.
Action: User performs delete action (swipe left or long-press + confirm).
Expected: Note card slides left + fades out (250ms ease-out). After animation completes (`ready_cb`), card is hidden and list reflows. Note is deleted from SD card. Remaining cards shift up smoothly.
Gap: Spec does not define the delete trigger gesture. Is it swipe-left? Long-press context menu? Dedicated delete button? Need to specify the interaction pattern.

**US-32: Create note via voice recording**
Scenario: User taps "+ Record" in notes header.
Action: Recording starts via dictation mode.
Expected: Voice overlay opens in dictation mode. Recording streams to Dragon. On stop, `dictation_summary` arrives with title and summary. New note card appears at top of list with slide-down animation. Badge shows "voice" with amber tint.
Gap: None.

**US-33: View note detail / edit**
Scenario: User taps on an existing note card.
Action: Note edit overlay opens.
Expected: Edit overlay appears (hide/show pattern, not create/destroy). Full note text displayed. Keyboard available for editing. Back button to return to list. Changes auto-save to SD card.
Gap: None.

### Settings

**US-34: Open settings and adjust brightness**
Scenario: User navigates to Settings.
Action: User taps Settings nav item, then drags brightness slider.
Expected: Settings overlay shows instantly. Brightness slider has amber gradient fill. White 11px knob follows finger smoothly (LVGL built-in drag). Display brightness updates in real-time via NVS write. Value label updates as slider moves.
Gap: None.

**US-35: Toggle wake word on/off**
Scenario: Settings open, wake word toggle visible.
Action: User taps the wake word toggle.
Expected: Toggle knob slides from off to on position (200ms ease-out). Track color fades from #111119 to amber 20% (200ms). NVS key `wake` updates to 1. AFE pipeline activates (if implemented).
Gap: None.

**US-36: Change voice mode in settings**
Scenario: User wants to switch from Local to Cloud mode.
Action: User taps voice mode dropdown, selects "Full Cloud".
Expected: Dropdown shows 4 options: Local, Hybrid, Full Cloud, TinkerClaw. Selecting "Full Cloud" sends `config_update` to Dragon. Mode pill on status bar updates color to blue. LLM model picker becomes enabled (was disabled in Local mode). NVS `vmode` updates to 2.
Gap: None.

**US-37: Check for OTA update**
Scenario: Settings open, user scrolls to OTA section.
Action: User taps "Check Update" button.
Expected: Button shows loading state. HTTP request to Dragon `/api/ota/check`. If update available: version number displayed, "Apply Update" green button appears. If current: "Up to date" text shown.
Gap: None.

**US-38: Settings with all sections visible**
Scenario: User scrolls through full settings page.
Action: User scrolls down through Display, Network, Voice, Storage, Battery, OTA, About sections.
Expected: All sections render with correct typography (section headers: FONT_SMALL #555 uppercase). Row heights >= 52px (TOUCH_MIN). Dividers barely visible (1px rgba(255,255,255,0.02)). No WDT crash (two-pass creation with `esp_task_wdt_reset()` between sections). Total objects ~55.
Gap: None.

### Mode Switching

**US-39: Switch mode via status bar pill tap**
Scenario: Home screen, currently in Local mode (green pill).
Action: User taps mode pill on status bar.
Expected: Mode cycles: Local(green) -> Hybrid(yellow) -> Cloud(blue) -> TinkerClaw(rose) -> Local. Pill border and text color update. Dot color changes. `config_update` sent to Dragon. Agent card accent line color updates if TinkerClaw mode entered/exited.
Gap: None.

**US-40: Switch mode while agent is running**
Scenario: TinkerClaw agent is actively running tasks. User switches to Local mode.
Action: User taps mode pill to switch away from TinkerClaw.
Expected: Mode switches to Local. Agent card enters a "paused" or "disconnected" state since TinkerClaw gateway is no longer the LLM route. Show toast: "Agent paused -- TinkerClaw mode required". Agent card pulsing dot stops, shows grey. Tasks remain visible but marked as interrupted.
Gap: Spec does not define what happens to active TinkerClaw agents when the user switches away from mode 3. Need to define: agent paused state, visual indication, and whether switching back resumes.

**US-41: Switch mode while voice is active**
Scenario: User is mid-voice-query (mic streaming). User somehow switches mode (e.g., via debug endpoint).
Action: Mode change arrives during active voice session.
Expected: Current voice session continues with the previous mode's pipeline (don't switch mid-stream). Mode change takes effect for the NEXT voice interaction. Toast: "Mode will change after current query". Mode pill updates immediately but pipeline switch is deferred.
Gap: Spec does not address mid-session mode switching. The voice pipeline should not switch modes during an active STT->LLM->TTS cycle. Need to specify deferred mode application.

**US-42: TinkerClaw mode with no gateway connection**
Scenario: User selects TinkerClaw mode but the TinkerClaw Gateway (port 18789) is unreachable.
Action: Mode set to 3, next voice query attempted.
Expected: Voice query fails. Toast: "TinkerClaw gateway unreachable". Auto-fallback to Local mode. NVS reverts to `vmode=0`. Mode pill changes back to green. Dragon sends `config_update` with error field.
Gap: None -- auto-fallback behavior is documented in voice pipeline spec.

### Error States / Edge Cases

**US-43: Battery dies while agent is working**
Scenario: Battery drops to critical level (<5%) during an active TinkerClaw agent session.
Action: ESP32-P4 initiates shutdown sequence.
Expected: Before shutdown: send `cancel` to Dragon to stop agent cleanly. Save current session_id to NVS. On next boot: reconnect with saved session_id. Dragon resumes or reports agent status. If agent completed while device was off, completion results are available on reconnect.
Gap: Spec does not address graceful shutdown behavior. Need to define: low battery warning banner (at 10%?), critical shutdown sequence (save state, notify Dragon), and session resume on reboot.

**US-44: WiFi drops and reconnects**
Scenario: WiFi connection lost for 30 seconds, then restores.
Action: WiFi event handler detects disconnect, then reconnect.
Expected: Connection dot turns red. Disconnect banner slides down (300ms ease-out). Reconnect watchdog activates. When WiFi restores: banner slides up, dot turns green, WS reconnects with backoff. Active session resumes if session_id matches.
Gap: None.

**US-45: Dragon restarts mid-TTS-playback**
Scenario: AI response TTS audio is playing. Dragon process restarts.
Action: WS connection drops during TTS binary stream.
Expected: TTS playback stops (incomplete audio). Disconnect banner appears. Partial audio does not cause speaker artifacts (playback drain task handles incomplete buffers gracefully). On reconnect, the LLM text is already in chat history -- user can read the full response even though TTS was interrupted.
Gap: None.

**US-46: OTA update initiated while on battery**
Scenario: User taps "Apply Update" in Settings while on battery power.
Action: OTA download starts.
Expected: Progress indication in Settings (could be a progress bar or percentage text). If battery drops below 15% during download, abort OTA and show toast "Battery too low for update -- please charge". OTA writes to inactive partition. On completion, device reboots. Auto-rollback protects against bad firmware.
Gap: Spec does not mention battery level check before OTA. Flashing a bad update on low battery could brick the device if it reboots into new firmware and crashes before marking valid, then battery dies before rollback reboot.

**US-47: 100+ notes cause object count explosion**
Scenario: User has accumulated 120 notes over months of use.
Action: User opens Notes screen.
Expected: Object recycling pool limits rendered cards to 15-20. Scrolling recycles off-screen cards for newly visible ones. Total object count stays under 80 (notes pool + chrome). No heap fragmentation from create/destroy -- pool objects are hidden/shown, not created/destroyed.
Gap: Notes screen currently does not implement object recycling (only chat does). This is a real risk. Need to add recycling pool to notes spec, similar to `chat_msg_view` pattern.

**US-48: Long press on agent card**
Scenario: Home screen, agent card visible with active agent.
Action: User long-presses the agent card (>500ms).
Expected: Two options: (1) open Agents overlay (same as tap), or (2) show a context menu with "Cancel agent" / "View details" / "Copy summary". If context menu: modal appears with options, amber border, dark background. Tapping outside dismisses.
Gap: Spec does not define long-press behavior on agent card. Tap opens Agents overlay, but long-press is undefined. Recommend: long-press shows "Cancel agent" option for active agents.

**US-49: Screen auto-lock / sleep**
Scenario: Device sits idle for 2 minutes with no touch input.
Action: Inactivity timer fires.
Expected: Display brightness fades to 0 over 1 second. Display backlight turns off (power save). Orb animations are paused. Touch anywhere wakes: brightness fades back in over 500ms, animations resume. Voice WS remains connected during sleep (keepalive pings continue). Wake word (if enabled) still listens during sleep.
Gap: Spec does not mention auto-lock/sleep behavior at all. For a battery-powered device, this is critical. Need to define: idle timeout, sleep behavior, wake gesture, and which subsystems stay active during sleep.

**US-50: Rapid nav bar tapping (stress test)**
Scenario: User rapidly taps between Home, Notes, Chat, Settings.
Action: User taps nav items faster than the 300ms debounce.
Expected: Nav debounce (300ms) prevents processing rapid taps. Only one screen transition per 300ms window. No animation race conditions (this was a previous bug fix). No crash from overlapping show/hide calls. Nav bar indicator animation completes or is cancelled cleanly before starting a new one (use `lv_anim_del` before `lv_anim_start`).
Gap: None -- 300ms debounce is already implemented and tested.
