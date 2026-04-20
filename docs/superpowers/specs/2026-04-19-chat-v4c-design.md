# Chat v4·C Ambient — Design Spec

**Date:** 2026-04-19
**Repo:** TinkerTab (firmware) + TinkerBox (Dragon server)
**Supersedes:** `2026-04-16-chat-redesign.md` (kept as architectural baseline; visual language and mode-per-session are the new layers)
**Target branch:** `feat/chat-v4c` (off `feat/ui-overhaul-v4`)
**Visual reference:** `.superpowers/brainstorm/1141731-1776574617/content/09-chat-pixelperfect.html`

---

## 1. Goal

Rebuild chat so it is the same product as the v4·C Ambient Canvas home — same tokens, same typography, same say-pill — and fix the architectural conflation that had a per-mode ring buffer pretending to be per-session history.

### 1.1 What chat is (decisions from brainstorming 2026-04-19)

1. **Unified surface.** Voice turns, text turns, rich media, and tool side-effects live in one thread. No mode-switching UI inside chat.
2. **Fullscreen takeover.** Chat is its own screen, entered via the home rail ("chat" button). Home stays intact underneath.
3. **Moderate density.** Dialogue bubbles + rich media as first-class content. Tool-call status chatter is hidden (tool activity surfaces on the home Now-slot via the widget platform, not here).
4. **Hybrid breakout.** Bubbles for short dialogue. Code, images, tables, cards break the bubble rail and take full screen width with an amber kicker.
5. **Amber orb-ball input.** The 84 px amber disc from the v4·C home say-pill is the voice affordance inside chat's input pill — one brand gesture across surfaces.
6. **Session-scoped with mode baked in.** Each session carries its own `voice_mode` + `llm_model`. Tapping a past session atomically switches the backend and loads history.

### 1.2 Out of scope (for this spec)

- Voice overlay redesign (separate spec follows immediately after this one; chat ships with the existing voice overlay suppressed via in-pill recording — voice overlay still exists as fallback for home-orb taps).
- Settings / Notes / Camera / Files redesign (later).
- Wake-word surfacing — feature stays parked.
- New backend features on Dragon beyond the two new session fields + `PATCH` route for in-session mode updates.

---

## 2. Architecture

### 2.1 Modules (inherited from 2026-04-16 spec)

| File | LOC est | Responsibility | LVGL dep |
|------|---------|----------------|----------|
| `chat_msg_store.c/h` | ~150 | Session-scoped message CRUD, ring buffer bounded at `BSP_CHAT_MAX_MESSAGES` | No |
| `chat_msg_view.c/h` | ~400 | Recycled object pool, virtual scroll, streaming pin | Yes |
| `chat_header.c/h` | ~110 | Back / title / ▾ chevron / mode chip / + new-chat | Yes |
| `chat_input_bar.c/h` | ~140 | 108 px say-pill: 84 px amber ball + text entry + keyboard affordance | Yes |
| `chat_suggestions.c/h` | ~100 | Empty-state suggestion cards, mode-aware prompts | Yes |
| `chat_session_drawer.c/h` | ~160 | Pull-down drawer, session rows, mode-per-session tap → `config_update` | Yes |
| `ui_chat.c` | ~300 | Orchestrator: module wiring, nav, show/hide | Yes |

Total: ~1,360 LOC (down from the 2,500+ current monolith). `chat_header.c` and `chat_input_bar.c` are explicitly reusable by Notes and the upcoming voice overlay redesign.

### 2.2 Data model changes

**TinkerBox (Dragon) — schema addition:**

```sql
ALTER TABLE sessions ADD COLUMN voice_mode INTEGER NOT NULL DEFAULT 0;   -- 0..3
ALTER TABLE sessions ADD COLUMN llm_model  TEXT    NOT NULL DEFAULT '';  -- e.g. "anthropic/claude-3.5-haiku"
```

Both default safely — existing sessions behave as Local.

**TinkerBox — REST changes:**

- `POST /api/v1/sessions` — accept optional `voice_mode` + `llm_model` at create time. If omitted, fall back to the device's current config.
- `GET /api/v1/sessions` — responses include `voice_mode` + `llm_model` fields (already forward-compat; clients that ignore them aren't broken).
- `PATCH /api/v1/sessions/{id}` — allow updating `voice_mode` + `llm_model` so a mode switch inside a live session persists.
- `config_update` WS message — when Tab5 changes mode mid-session, Dragon also writes the new mode onto the active session.

**TinkerTab (Tab5) — NVS:** no change. Existing `vmode` + `llm_mdl` remain the *last-used* device defaults and are applied on boot; loading a session overrides them in-memory for the session's lifetime.

### 2.3 Session-scoped message store

The April 16 spec's per-mode ring buffer is replaced by a single session-scoped store. The store holds only the *active* session's messages. Switching sessions clears the store and refetches.

```c
typedef struct {
    char          session_id[40];
    uint8_t       voice_mode;        // 0..3, matches the session's mode on Dragon
    char          llm_model[64];
    char          title[80];         // Dragon-auto-titled
    uint32_t      updated_at;
} chat_session_t;

typedef enum {
    MSG_TEXT, MSG_IMAGE, MSG_CARD, MSG_AUDIO_CLIP,
    MSG_SYSTEM,
    // NOTE: no MSG_TOOL_STATUS here — tool activity is hidden from chat (Q3)
} msg_type_t;

typedef struct {
    msg_type_t  type;
    bool        is_user;
    char        text[512];
    char        media_url[256];
    char        subtitle[128];
    uint32_t    timestamp;
    int16_t     height_px;
    bool        active;
} chat_msg_t;

// Bounded ring buffer for the ACTIVE session only
static chat_session_t s_active;
static chat_msg_t     s_messages[BSP_CHAT_MAX_MESSAGES];  // 100
static int            s_msg_count;
static int            s_write_idx;
```

Why session-scoped (not per-mode): simpler to reason about, matches Dragon's data model (messages are keyed by `session_id`), and makes the "tapping a row resumes that session's mode" flow a single data-load rather than a buffer swap.

### 2.4 Streaming pin (preserved)

When the AI is streaming (`llm` tokens arriving), the last pool slot is pinned to the streaming message and never recycled until `llm_done`. Height recalculated after each `lv_label_set_text`; auto-scroll to bottom.

---

## 3. Visual language

All tokens pulled 1-to-1 from `main/ui_theme.h` and `main/ui_home.c`. No new palette entries.

### 3.1 Colors

| Token | Hex | Role in chat |
|-------|-----|--------------|
| `TH_BG` | `#08080E` | Screen background |
| `TH_CARD` | `#111119` | AI bubble fill, input pill fill |
| `TH_CARD_ELEVATED` | `#13131F` | Mode chip fill, "+" new-chat button, keyboard affordance |
| `TH_BORDER` (#1E1E2A) | `#1E1E2A` | 1 px border on AI bubble, pill, chip |
| `TH_AMBER` | `#F59E0B` | User bubble, orb-ball gradient end, accent bar, kicker, "+" icon |
| `TH_AMBER_HOT` | `#FBBF24` | Orb-ball gradient start |
| `TH_MODE_LOCAL` | `#22C55E` | Session dot + kicker when session mode = Local |
| `TH_MODE_HYBRID` | `#EAB308` | Session dot + kicker when session mode = Hybrid |
| `TH_MODE_CLOUD` | `#3B82F6` | Session dot + kicker when session mode = Cloud |
| `TH_MODE_CLAW` | `#F43F5E` | Session dot + kicker when session mode = Claw |
| `TH_TEXT_PRIMARY` | `#E8E8EF` | AI bubble body, mode-chip label, session row title |
| `TH_TEXT_BODY` | `#AAAAAA` | Lead paragraphs |
| `TH_TEXT_SECONDARY` | `#666666` | Session row timestamp, ghost hint strong portion |
| `TH_TEXT_DIM` | `#444444` | Bubble timestamps, kicker dim state |

### 3.2 Typography

| Define (from `config.h`) | Font | Size | Where in chat |
|--------------------------|------|------|---------------|
| `FONT_TITLE` | Montserrat Bold 28 | — | not used (chat uses Fraunces for title) |
| `FONT_HEADING` | Montserrat Bold 22 | 22 | Bubble body (default) |
| `FONT_BODY` | Montserrat 18 | 18 | Session row "preview" text |
| `FONT_SMALL` | Montserrat 14 | 14 | Timestamps, chip sublabel |
| New: `FONT_CHAT_TITLE` | **Fraunces italic 32** | 32 | Chat header title ("Chat"), drawer title ("Conversations") |
| New: `FONT_CHAT_EMPH` | **Fraunces italic 22** | 22 | AI bubble `<em>` emphasis spans |
| New: `FONT_CHAT_MONO` | **JetBrains Mono Medium 14** | 14 | Kickers, session row mode line |

Font additions go through the existing LVGL font converter pipeline. Fraunces italic 32 + JetBrains Mono Medium 14 subsets estimated at ~30 KB flash. Fraunces italic 22 reuses the same font family.

Fallback path: if the font converter can't ship new faces in time, fall back to Montserrat for body + Montserrat italic-emulated via `lv_obj_set_style_transform_skew` on the title label. Graceful degradation; brand shines brighter with Fraunces available.

### 3.3 Spacing — all from `ui_home.c` constants

```c
#define SIDE_PAD       40   /* same as home */
#define HDR_H          96   /* chat-specific; home status strip is 56 but chat needs more vertical for title + chip */
#define ACCENT_H       2    /* under header; home Now-slot bar is 3, kept at 2 here as chrome edge */
#define ACCENT_W       140  /* matches home Now-slot 140×3 accent */
#define BUBBLE_RADIUS  22
#define BUBBLE_TAIL_R  6
#define BUBBLE_PAD_H   22
#define BUBBLE_PAD_V   16
#define BUBBLE_GAP     20   /* between consecutive bubbles */

#define PILL_BOT_PAD   40   /* same as home STRIP_BOT_PAD */
#define PILL_H         108  /* same as home SAY_H */
#define PILL_R         54   /* = PILL_H / 2 */
#define PILL_BALL_SZ   84   /* same as home's amber disc */
#define PILL_KB_SZ     56   /* same as home's rail chip height */

#define BREAK_PAD_H    40   /* inner padding inside full-width break-out, matches SIDE_PAD */
#define BREAK_IMG_H    260
#define BREAK_IMG_R    10
```

### 3.4 Touch targets

All ≥ `DPI_SCALE(44)` (TOUCH_MIN) per WCAG / Apple HIG. Verified targets: back button, ▾ chevron, mode chip, "+" new-chat, orb-ball, keyboard affordance, session row (full row tappable at 64 px tall), "+ Start new conversation".

---

## 4. Scene specs

### 4.1 Scene 1 — Conversation

```
y=0         ┌─ HEADER (96 h) ────────────────────────┐
y=52        │  ← [back]  Chat ▾       [mode] [+]    │   mode chip + "+" right
y=96        ├──── AMBER ACCENT 140×2 ────            │
y=96        │                                        │
            │  ┌─ AI bubble ──────────────────┐      │
            │  │ body text (Fraunces italic   │      │
            │  │ accents in amber)            │      │
            │  └──────────────────────────────┘      │
            │        TIMESTAMP (JetBrains Mono)      │
            │                                        │
            │      ┌─ USER bubble (amber) ──────┐    │   right-aligned
            │      │ body text                  │    │
            │      └────────────────────────────┘    │
            │                     TIMESTAMP          │
            │                                        │
            │── BREAK-OUT ──────────────────────── │   full-bleed, ±40 margin
            │ PYTHON · util/time.py (amber kick)   │   3 px amber bar top-left
            │ def iso(dt):                         │   JetBrains Mono 17
            │     return dt.isoformat()            │
            │── /BREAK-OUT ──────────────────────── │
            │                                        │
y=1076      ├─ SAY-PILL (108 h) ────────────────────┤
y=1128      │  [●]  │  ▌Hold to speak · or type…  [⌨] │   84 ball + cursor + ghost + 56 kb
y=1184      └────────────────────────────────────────┘
y=1240-1280                   40 px bottom pad
```

### 4.2 Scene 2 — Drawer open

Header re-titles to "Conversations"; ▾ flips to ▴. A 720-wide 2 px gradient spine paints the four mode colors under the header (Local → Hybrid → Cloud → Claw). The drawer pushes down from the header, occupying roughly 800 px; the old thread fades to ~12% opacity with a 2 px blur beneath. Dismiss by tapping ▴ or the faded area.

Session row layout:

```
┌─ 40 pad ─┬─ dot (12) ─┬─ info ─────────┬─ time ─┬─ 40 pad ─┐
│          │ ●          │ MODE · MODEL    │ 5m    │          │
│          │            │ Session title   │        │          │
└──────────┴────────────┴─────────────────┴────────┴──────────┘
 18 pad top │ 3 px amber left border when row = active (current session)
 18 pad bot │
```

Rows are 64 px tall; 44 TOUCH_MIN is comfortably met.

### 4.3 Scene 3 — Media-rich

Identical chrome to Scene 1, but the accent bar + kicker inherit the session's mode color. A Cloud · Sonnet session paints the 140×2 bar and the kicker text in `TH_MODE_CLOUD`. Images use a 260 h placeholder until `media_cache_fetch` resolves the JPEG, then swap in via `lv_async_call`.

---

## 5. Interactions

### 5.1 Enter chat

From home rail "chat" button → `ui_chat_show()` → chat overlay fades in (120 ms). Overlay is created once at boot and shown/hidden (inherits the v5 hide/show fragmentation-safety pattern).

### 5.2 Speak from chat

Tap orb-ball in the say-pill → `voice_start_listening()` fires. The ball's gradient shifts with the **voice pipeline state** (idle amber → listening amber-hot → processing amber with breathing pulse → speaking amber-dark → done back to amber). This is independent of the widget-platform `tone` system (which paints the *home* orb based on active skill tone). STT text streams into a new user bubble as it arrives. On `llm`, the AI bubble streams in below; `llm_done` finalizes.

Voice overlay is **not opened** from chat — the conversation stays visible. This is the primary fix for the "interrupts and shit" complaint from 2026-04-19's audit: no overlay context-switch, no stale-handler stacking on orb tap during SPEAKING.

### 5.3 Type from chat

Tap anywhere in the input pill (outside the ball + keyboard) → keyboard overlay opens, focus on a hidden `lv_textarea` behind the pill. Done key → `voice_send_text()` → new user bubble → AI streams as usual.

### 5.4 Pull drawer

Tap ▾ chevron (44 TOUCH_MIN) → `chat_session_drawer_show()`:

1. Fetch `GET /api/v1/sessions?limit=10`
2. Paint rows with mode color per session
3. ▾ rotates to ▴
4. Thread fades to 12% + 2 px blur

Tap a session row:

```c
// chat_session_drawer.c
static void row_click_cb(lv_event_t *e) {
    const chat_session_t *s = lv_event_get_user_data(e);
    // 1. Atomic mode switch
    voice_send_config_update(s->voice_mode, s->llm_model);
    // 2. Load that session's messages from Dragon
    chat_view_load_session(s->session_id);
    // 3. Update the header mode chip
    chat_header_set_mode(s->voice_mode, s->llm_model);
    // 4. Close drawer
    chat_session_drawer_hide();
}
```

Tap "+ Start new conversation" → Dragon creates a session inheriting the currently-active mode → chat loads empty state → suggestion cards appear.

### 5.5 New chat (header "+")

Same as "+ Start new conversation" in the drawer but doesn't open the drawer. One-tap context reset.

### 5.6 Mode switch *during* a session

Long-press mode chip → toggles through Local → Hybrid → Cloud → Claw. Tab5 sends `config_update`; Dragon writes the new mode onto the active session so the next drawer fetch reflects it. Toast confirms: "Mode: Cloud · Haiku".

---

## 6. Object budget (Tab5)

| Component | Objects | Lifecycle |
|-----------|---------|-----------|
| Header | 8 | Persistent (hidden overlay) |
| Accent bar | 1 | Persistent |
| Message pool 12 × 3 | 36 | Persistent |
| Suggestions | 5 | Created when message count = 0, destroyed at first send |
| Input pill | 5 (pill + ball + cursor + ghost + kb) | Persistent |
| Drawer (on demand) | 25 (5 rows × 5 children) | Created on open, destroyed on close |
| **Total (chat closed)** | **55** | |
| **Peak (drawer open)** | **80** | |

Down from ~154 in the current monolith.

---

## 7. Migration

### 7.1 Delete

- `build_home_panel()` in `ui_chat.c` (~440 LOC) — replaced by direct conversation entry
- `s_next_y` manual Y tracking — replaced by pool + virtual scroll
- `MAX_MESSAGES` eviction loop — replaced by `chat_msg_store` ring buffer
- All `MSG_TOOL_CALL` / tool-indicator bubble creation — tool activity hidden (Q3 decision)

### 7.2 Preserve (public API intact)

- `ui_chat_push_message(role, text)` — backed by store now
- `ui_chat_push_media / push_card / push_audio_clip` — stored as typed messages
- `ui_chat_update_last_message(text)` — mutates the last store entry + refreshes view
- `ui_chat_hide() / ui_chat_create()` — hide/show pattern preserved

### 7.3 New

- All six new modules under `main/chat_*.c/h`
- Session drawer module `main/chat_session_drawer.c/h`
- Dragon schema additions (see §2.2)
- Fraunces italic + JetBrains Mono font subsets in LVGL font pipeline

### 7.4 Branch strategy

Branch `feat/chat-v4c` off `feat/ui-overhaul-v4`. Merge back to `feat/ui-overhaul-v4` when validated on-device. `main` merge batched with voice overlay redesign.

---

## 8. Risk assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| Fraunces / JetBrains Mono font converter pipeline delay | Medium | Ship with Montserrat fallback; add fonts in a follow-up commit |
| Session drawer fetch latency over ngrok | Medium | Show "Loading…" immediately, populate async; cache last response |
| Virtual scroll glitches during streaming | High | Streaming slot never recycled; height refined live |
| Mode switch races (tap row → config_update → WS round-trip while user taps another row) | Medium | Disable drawer rows during in-flight `config_update`; re-enable on ACK |
| Dragon schema migration on existing database | Low | `DEFAULT 0` + `DEFAULT ''` — existing rows remain valid |
| `chat_input_bar` / `chat_header` adoption by Notes/Voice — coupling risk | Low | Both modules take accent color as constructor param; no shared state |
| LV_MEM regression from Fraunces font blob | Medium | Measure flash + internal-SRAM impact; keep subset minimal (ASCII + basic punctuation + amber-highlighted emphasis glyphs) |

---

## 9. Testing

### 9.1 Linux simulator (unit)

- `chat_msg_store` — add/get/clear, ring buffer overflow, session swap wipes state
- Session record round-trip: set → persist → load → fields intact

### 9.2 Device (integration)

- Enter chat from home rail → session loads in ≤ 1 s on local LAN
- Send text message → bubble appears instantly, AI streams, auto-scroll to bottom
- Send 30 messages → scroll up/down, no glitches, heights stable
- Code block arrives via `media` WS → full-bleed break-out renders with amber kicker
- Drawer: pull ▾ → 10 sessions with correct mode dots
- Drawer: tap Cloud · Sonnet row → `config_update` fires → mode chip repaints blue → messages load → orb-ball color matches tone
- New Chat "+": messages clear, suggestions appear
- Mode switch during active session: long-press chip → toast shows new mode → drawer reflects on next fetch
- Voice-from-chat: tap orb-ball → recording starts → bubble streams → NO voice overlay opens
- Keyboard: tap pill → keyboard opens, input visible above keyboard, Done submits
- Heap: 50 chat open/close cycles → largest free block > 60 KB sustained
- Screenshot: pixel-diff against `09-chat-pixelperfect.html` scene mockups (< 5% diff target)

### 9.3 Cross-device (future)

- Build for Waveshare 320×480 simulator → `BSP_CHAT_POOL_SIZE=6`, smaller side-pad, verify layout adapts

---

## 10. Open questions (pre-plan)

None that block implementation. Answered during brainstorming:

- Q: What lives in the thread? → Dialogue + media only (Q3).
- Q: Fullscreen vs sheet? → Fullscreen (Q2).
- Q: Voice from chat? → In-pill orb-ball, no overlay (Q5).
- Q: History? → Session-scoped with mode-per-session (Q6 + user refinement).
- Q: Visual style? → Hybrid breakout, v4·C tokens (Q4).
