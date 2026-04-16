# Chat Redesign — Modular, Recycled, Mode-Aware

**Date:** 2026-04-16
**Repo:** TinkerTab (firmware)
**Goal:** Reduce chat object count from 154 to ~54, decompose 2500-line monolith into reusable modules, support per-mode conversation history, and make the architecture portable across screen sizes.

## Architecture

Delete Chat Home panel entirely (39 objects). Go straight to conversation on Chat tap. Implement message object recycling with a data-backed message store. Decompose ui_chat.c into 6 focused modules. Use LVGL best practices throughout: flex layout for infrastructure, `lv_pct()` sizing, `DPI_SCALE()` for touch targets, shared static styles, `lv_obj_null_on_delete()` for pool pointers.

---

## 1. Module Structure

### 1.1 File Decomposition

| File | Lines (est) | Responsibility | LVGL dependency |
|------|-------------|---------------|-----------------|
| `chat_msg_store.c/h` | ~150 | Ring buffer per mode, message CRUD, search | **None** — pure C |
| `chat_msg_view.c/h` | ~400 | Recycled object pool, scroll handling, slot reconfiguration | Yes |
| `chat_input_bar.c/h` | ~120 | Composite widget: mic + textarea + send button | Yes |
| `chat_header.c/h` | ~80 | Composite widget: back + title + status + mode badge | Yes |
| `chat_suggestions.c/h` | ~100 | Mode-specific suggestion cards, empty state | Yes |
| `ui_chat.c` | ~300 | Orchestrator: wires modules, handles nav, session drawer | Yes |

**Total: ~1150 lines** (down from 2500+, plus reusable across screens)

### 1.2 Dependency Graph

```
ui_chat.c (orchestrator)
  ├── chat_msg_store.c   (data — no LVGL)
  ├── chat_msg_view.c    (rendering — uses store)
  ├── chat_input_bar.c   (widget — standalone)
  ├── chat_header.c      (widget — standalone)
  └── chat_suggestions.c (widget — uses store for mode)
```

`chat_input_bar.c` and `chat_header.c` are reusable by `ui_notes.c` and `ui_voice.c`.

---

## 2. Message Data Store (`chat_msg_store.c`)

### 2.1 Data Model

```c
typedef enum {
    MSG_TEXT,           // Plain text (user or AI)
    MSG_IMAGE,          // Rendered code/table/photo (JPEG URL)
    MSG_CARD,           // Link preview / search result
    MSG_AUDIO_CLIP,     // Pronunciation / music preview
    MSG_TOOL_STATUS,    // "Searching the web..." (ephemeral)
    MSG_SYSTEM,         // "Clearing..." / errors (centered)
} msg_type_t;

typedef struct {
    msg_type_t  type;
    bool        is_user;          // true = right-aligned, false = left-aligned
    char        text[512];        // message content or alt text
    char        media_url[256];   // for MSG_IMAGE/MSG_CARD/MSG_AUDIO_CLIP
    char        subtitle[128];    // for MSG_CARD
    uint32_t    timestamp;        // epoch seconds
    int16_t     height_px;        // measured bubble height (cached after first render)
    bool        active;           // slot in use
} chat_msg_t;
```

### 2.2 Per-Mode Ring Buffers

```c
#define CHAT_MODE_COUNT     4
static chat_msg_t s_messages[CHAT_MODE_COUNT][BSP_CHAT_MAX_MESSAGES];
static int        s_msg_count[CHAT_MODE_COUNT];
static int        s_write_idx[CHAT_MODE_COUNT];  // ring buffer write head
```

BSP constants:
```c
// bsp/tab5/bsp_config.h
#define BSP_CHAT_MAX_MESSAGES   100   // per-mode history depth
#define BSP_CHAT_POOL_SIZE      12    // visible message slots

// bsp/waveshare_s3/bsp_config.h (future)
#define BSP_CHAT_MAX_MESSAGES   30
#define BSP_CHAT_POOL_SIZE      6
```

### 2.3 API

```c
void     chat_store_init(void);
int      chat_store_add(uint8_t mode, const chat_msg_t *msg);      // returns index
int      chat_store_count(uint8_t mode);
const chat_msg_t *chat_store_get(uint8_t mode, int index);         // 0=oldest
void     chat_store_clear(uint8_t mode);                           // New Chat
void     chat_store_clear_all(void);                               // Factory reset
```

No LVGL dependency. Runs on Linux simulator. Testable standalone.

---

## 3. Message View with Object Recycling (`chat_msg_view.c`)

### 3.1 Pool Architecture

Pre-allocate `BSP_CHAT_POOL_SIZE` message slots at init. Each slot has every possible child widget, most hidden:

```c
typedef struct {
    lv_obj_t *container;      // bubble outer container
    lv_obj_t *text_label;     // for MSG_TEXT/MSG_SYSTEM/MSG_TOOL_STATUS
    lv_obj_t *image_widget;   // for MSG_IMAGE (hidden when text)
    lv_obj_t *title_label;    // for MSG_CARD title (hidden otherwise)
    lv_obj_t *subtitle_label; // for MSG_CARD subtitle (hidden otherwise)
    lv_obj_t *timestamp;      // always visible
    int       data_index;     // which chat_msg_t this slot displays (-1 = empty)
} msg_slot_t;

static msg_slot_t s_slots[BSP_CHAT_POOL_SIZE];
```

Objects per slot: 6 (container + text_label + image_widget + title_label + subtitle_label + timestamp).
Most are hidden at any time. Only text_label OR image_widget is visible per slot, never both.

**Total pool objects: BSP_CHAT_POOL_SIZE × 6 = 72 on Tab5 (12 slots), 36 on Waveshare (6 slots).**

Wait — 72 is too many. Let me optimize.

**Revised: 3 objects per slot.** Container + content label + timestamp. When an image needs to show, the label is hidden and an image widget is created inside the container. Image widgets are rare (1-3 per conversation) so creating them on demand is acceptable — they're not pooled.

```c
typedef struct {
    lv_obj_t *container;      // bubble outer
    lv_obj_t *content;        // text label (always created, hidden for images)
    lv_obj_t *timestamp;      // always visible
    lv_obj_t *media;          // NULL normally; created on demand for images, deleted when slot recycled
    int       data_index;     // which chat_msg_t this slot displays
} msg_slot_t;
```

Objects per slot: 3 base + 1 optional = **3 normally, 4 for image slots**.
Total pool: 12 × 3 = **36 objects** (Tab5). Media objects created/deleted only when scrolling past image messages (rare, 1 object at a time).

### 3.2 Scroll Handling

The scroll container (`s_msg_scroll`) has a virtual content height equal to the sum of all message heights in the store. The pool slots are positioned within this virtual space to cover the visible window.

```
Virtual scroll content:
┌──────────────────────────┐ ← total_height (sum of all msg heights)
│  msg[0] (off-screen)     │
│  msg[1] (off-screen)     │
│  ...                     │
│ ─── visible window ────  │ ← scroll_y
│  msg[15] ← slot[0]      │   Pool slots map to these
│  msg[16] ← slot[1]      │
│  msg[17] ← slot[2]      │
│  ...                     │
│  msg[26] ← slot[11]     │
│ ─── end visible ───────  │ ← scroll_y + MSG_AREA_H
│  msg[27] (off-screen)    │
│  ...                     │
│  msg[99] (off-screen)    │
└──────────────────────────┘
```

On scroll event:
1. Calculate which data indices are visible: `first_visible = scroll_y_to_index(scroll_y)`
2. For each pool slot, assign to a data index in the visible range
3. Update slot content (text, style, position) if data index changed
4. Recycle slots that scrolled off-screen to newly visible messages

### 3.3 Height Calculation

Each message's height must be known to calculate scroll positions. On first render, measure the slot after setting text (`lv_obj_update_layout`, then `lv_obj_get_height`). Cache in `chat_msg_t.height_px`.

For unrendered messages (scrolled far off-screen), estimate: `estimated_height = 60 + (text_len / 40) * 20`. Refined when the message is actually rendered.

### 3.4 Streaming Support

When AI is responding (tokens arriving), the last slot is in "streaming mode":
- New tokens append to the text label: `lv_label_set_text(slot->content, accumulated_text)`
- Height recalculated after each append
- Auto-scroll to bottom
- On `llm_done`, finalize the message in the store with full text + measured height

### 3.5 API

```c
void chat_view_init(lv_obj_t *parent);                // create pool inside parent
void chat_view_set_mode(uint8_t mode);                // switch to mode's message history
void chat_view_refresh(void);                          // rebuild visible window from store
void chat_view_scroll_to_bottom(void);                 // auto-scroll
void chat_view_append_streaming(const char *token);    // streaming AI response
void chat_view_finalize_streaming(void);               // end streaming, commit to store
```

---

## 4. Composite Widgets

### 4.1 Input Bar (`chat_input_bar.c`)

Reusable across chat, voice, notes.

```c
typedef struct {
    lv_obj_t *container;     // horizontal flex row
    lv_obj_t *mic_btn;       // microphone button (left)
    lv_obj_t *textarea;      // text input (center, flex-grow)
    lv_obj_t *send_btn;      // send button (right)
} chat_input_bar_t;

chat_input_bar_t *chat_input_bar_create(lv_obj_t *parent, uint32_t accent_color);
void chat_input_bar_set_callbacks(chat_input_bar_t *bar,
                                  lv_event_cb_t on_send,
                                  lv_event_cb_t on_mic,
                                  lv_event_cb_t on_ta_click);
const char *chat_input_bar_get_text(chat_input_bar_t *bar);
void chat_input_bar_clear(chat_input_bar_t *bar);
```

**Layout:** Flex row, `lv_pct(100)` width, `DPI_SCALE(52)` height.
**Touch targets:** Mic button `DPI_SCALE(44)` × `DPI_SCALE(44)`, Send button `DPI_SCALE(44)` height.
**Objects: 4** (container + mic + textarea + send)

### 4.2 Header (`chat_header.c`)

Reusable across chat, notes, settings.

```c
typedef struct {
    lv_obj_t *container;     // horizontal flex row
    lv_obj_t *back_btn;      // back arrow (left)
    lv_obj_t *title;         // screen title (flex-grow)
    lv_obj_t *status_dot;    // connection indicator
    lv_obj_t *status_label;  // "Ready" / "Processing..."
    lv_obj_t *action_btn;    // optional right button (+ for new chat)
    lv_obj_t *mode_badge;    // mode indicator (right)
} chat_header_t;

chat_header_t *chat_header_create(lv_obj_t *parent, const char *title,
                                   uint32_t accent_color);
void chat_header_set_status(chat_header_t *hdr, const char *text, bool connected);
void chat_header_set_mode(chat_header_t *hdr, const char *mode_name, uint32_t color);
```

**Layout:** Flex row, `lv_pct(100)` width, `DPI_SCALE(48)` height.
**Objects: 7** (container + back + title + dot + label + action + badge)

### 4.3 Suggestions (`chat_suggestions.c`)

Mode-specific empty state cards.

```c
void chat_suggestions_create(lv_obj_t *parent, uint8_t mode, lv_event_cb_t on_tap);
void chat_suggestions_show(void);
void chat_suggestions_hide(void);
```

4 suggestion cards per mode, hidden when messages exist. Uses flex column layout.
**Objects: 5** (container + 4 cards)

---

## 5. Styling (LVGL Best Practices)

### 5.1 Shared Static Styles

```c
// In chat_msg_view.c — created once, shared across all slots
static lv_style_t s_style_user_bubble;    // amber, right-aligned
static lv_style_t s_style_ai_bubble;      // dark, left-aligned
static lv_style_t s_style_tool_bubble;    // orange accent, ephemeral
static lv_style_t s_style_system_msg;     // centered, muted
static lv_style_t s_style_timestamp;      // small, right-aligned, gray
static lv_style_t s_style_card;           // left orange border

static bool s_styles_initialized = false;

static void init_styles(void) {
    if (s_styles_initialized) return;
    lv_style_init(&s_style_user_bubble);
    lv_style_set_bg_color(&s_style_user_bubble, lv_color_hex(0xF5A623));
    lv_style_set_bg_opa(&s_style_user_bubble, LV_OPA_20);
    // ... etc
    s_styles_initialized = true;
}
```

### 5.2 DPI-Aware Sizing

```c
// In config.h (app-level, not BSP)
#define DPI_SCALE(px)  ((px) * BSP_DISPLAY_DPI / 160)

// Usage:
lv_obj_set_height(btn, DPI_SCALE(48));       // 48px at 160 DPI, scales up/down
lv_style_set_pad_all(&style, DPI_SCALE(12)); // 12px padding, scaled
lv_style_set_radius(&style, DPI_SCALE(16));  // 16px radius, scaled
```

BSP defines:
```c
// bsp/tab5/bsp_config.h
#define BSP_DISPLAY_DPI   218    // 720px / 3.3 inches

// bsp/waveshare_s3/bsp_config.h (future)
#define BSP_DISPLAY_DPI   145    // 320px / 2.2 inches
```

### 5.3 Touch Targets

All interactive elements use `DPI_SCALE(44)` minimum dimension (7mm at 160 DPI):

```c
#define TOUCH_MIN   DPI_SCALE(44)

// Mic button
lv_obj_set_size(mic_btn, TOUCH_MIN, TOUCH_MIN);

// Send button
lv_obj_set_height(send_btn, TOUCH_MIN);

// Suggestion cards
lv_obj_set_height(card, DPI_SCALE(48));  // slightly larger for readability

// Back button
lv_obj_set_size(back_btn, TOUCH_MIN, TOUCH_MIN);
```

### 5.4 Layout System

| Component | Layout | Why |
|-----------|--------|-----|
| Header | Flex row | <10 objects, horizontal, auto-spacing |
| Input bar | Flex row | 4 objects, textarea grows to fill |
| Suggestions | Flex column | 4-5 cards, centered, auto-gap |
| Message pool | Manual Y positioning | Virtual scroll requires manual placement |
| Pool slot internals | Flex column | Label + timestamp stacked vertically |

### 5.5 Pointer Safety

All pool slot pointers use `lv_obj_null_on_delete()` where available in LVGL v9.2.2. If not available, explicit NULL on recycle:

```c
static void recycle_slot(msg_slot_t *slot) {
    if (slot->media) {
        lv_obj_del(slot->media);
        slot->media = NULL;
    }
    slot->data_index = -1;
    lv_obj_add_flag(slot->container, LV_OBJ_FLAG_HIDDEN);
}
```

---

## 6. Session Drawer

Pull-down overlay created on demand, destroyed on dismiss. Not persistent — acceptable because it's rare and short-lived.

### 6.1 Trigger

Swipe down from header, or tap a "▾" chevron in the header. Creates a semi-transparent overlay + session list.

### 6.2 Data Source

Fetches from Dragon REST API: `GET /api/v1/sessions?limit=10`
Response includes session_id, mode, last_message_preview, timestamp.

### 6.3 Object Budget

~5 objects per visible session row × 5 rows = 25 objects. Created on open, destroyed on close. Only exists while drawer is visible.

### 6.4 Interaction

Tap a session → loads that session's messages from Dragon into the current mode's store → refreshes view. Tap outside or swipe up → closes drawer.

---

## 7. Object Budget

### 7.1 Tab5 (720×1280)

| Component | Objects | Notes |
|-----------|---------|-------|
| Header | 7 | Flex row, persistent |
| Accent bar | 1 | Thin mode-colored bar |
| Message pool (12 × 3) | 36 | Recycled, persistent |
| Suggestions | 5 | Hidden when messages exist |
| Input bar | 4 | Flex row, persistent |
| Scroll container | 1 | |
| **Total** | **54** | **65% reduction from 154** |

### 7.2 Waveshare 320×480 (future)

| Component | Objects | Notes |
|-----------|---------|-------|
| Header | 6 | Compact (no action button) |
| Accent bar | 1 | |
| Message pool (6 × 3) | 18 | Fewer visible slots |
| Suggestions | 4 | Fewer cards |
| Input bar | 4 | Same widget, smaller |
| Scroll container | 1 | |
| **Total** | **34** | |

---

## 8. Migration Strategy

### 8.1 What Gets Deleted

- `build_home_panel()` — entire function (~440 lines)
- All Chat Home static variables (s_home_panel, s_home_textarea, s_model_lbl, etc.)
- `show_chat_home()` / `enter_conversation()` panel switching logic
- Manual `s_next_y` tracking
- `MAX_MESSAGES` eviction loop with Y recalculation

### 8.2 What Gets Preserved

- `ui_chat_push_message()` — public API stays (backed by store now)
- `ui_chat_push_media/card/audio_clip()` — public API stays (stored as MSG_IMAGE/CARD/AUDIO)
- `ui_chat_update_last_message()` — updates store, refreshes view
- `ui_chat_hide()` / `ui_chat_create()` — hide/show pattern preserved
- Mode badge cycling, keyboard integration, voice state polling

### 8.3 What's New

- `chat_msg_store.c/h` — new file
- `chat_msg_view.c/h` — new file
- `chat_input_bar.c/h` — new file (replaces 3 copies across chat/voice/notes)
- `chat_header.c/h` — new file (replaces duplicated header patterns)
- `chat_suggestions.c/h` — new file
- `BSP_CHAT_*` constants in `bsp/tab5/bsp_config.h`
- `DPI_SCALE()` macro in `config.h`
- `BSP_DISPLAY_DPI` in `bsp/tab5/bsp_config.h`

---

## 9. Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| Scroll position jumps during recycling | High | Pre-measure heights, smooth transitions |
| Streaming breaks with pool recycling | High | Last slot stays pinned during streaming, never recycled |
| Rich media in recycled slots | Medium | Media widgets created on demand per slot, deleted on recycle |
| Height estimation errors | Medium | Conservative estimate, refine on first render |
| Session drawer fetch latency | Low | Show "Loading..." immediately, populate async |
| Regression in existing chat features | High | Preserve all public API signatures, test each feature |

---

## 10. Testing

### 10.1 Unit Tests (Linux simulator)

- `chat_msg_store`: add/get/clear per mode, ring buffer overflow, boundary conditions
- No LVGL needed — pure C with assert

### 10.2 Integration Tests (on device)

- Navigation: Home → Chat → verify conversation loads in 1 tap
- Mode cycling: tap badge → suggestions change, history switches
- Scrolling: send 30+ messages → scroll up/down → verify no visual glitches
- Rich media: code block → image renders in recycled slot
- New Chat: tap + → messages clear, welcome state shows
- Session drawer: swipe down → sessions list → tap to load
- Keyboard: tap textarea → keyboard opens, input visible above keyboard
- Memory stability: 10 rounds of open/close/navigate → heap stable

### 10.3 Cross-Device (future)

- Build for Linux with `BSP_CHAT_POOL_SIZE=6` → verify layout adapts
- Verify `DPI_SCALE()` produces correct sizes at different DPI values
