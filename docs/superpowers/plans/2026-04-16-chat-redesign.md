# Chat Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decompose the 2747-line ui_chat.c monolith into 6 focused modules with object recycling, reducing LVGL object count from 154 to ~54, with per-mode conversation history and portable sizing.

**Architecture:** Delete Chat Home panel (39 objects). Go straight to conversation. Message data backed by a pure-C ring buffer per voice mode. LVGL objects recycled from a fixed pool of 12 slots. Composite widgets (header, input bar, suggestions) reusable across screens. DPI_SCALE macro for portable touch targets.

**Tech Stack:** ESP-IDF 5.4.3 / LVGL v9.2.2 / C11 / FreeRTOS

**Spec:** `docs/superpowers/specs/2026-04-16-chat-redesign.md`

---

## File Structure

### New Files
| File | Responsibility |
|------|---------------|
| `main/chat_msg_store.h` | Message data model + ring buffer API |
| `main/chat_msg_store.c` | Per-mode ring buffers, CRUD, no LVGL dependency |
| `main/chat_msg_view.h` | Recycled message pool API |
| `main/chat_msg_view.c` | Pool slots, scroll handling, style management |
| `main/chat_input_bar.h` | Composite input widget API |
| `main/chat_input_bar.c` | Mic + textarea + send, flex layout |
| `main/chat_header.h` | Composite header widget API |
| `main/chat_header.c` | Back + title + status + mode badge, flex layout |
| `main/chat_suggestions.h` | Mode-specific suggestion cards API |
| `main/chat_suggestions.c` | 4 cards per mode, empty state |

### Modified Files
| File | Change |
|------|--------|
| `main/ui_chat.c` | Complete rewrite (~300 lines, orchestrator only) |
| `main/ui_chat.h` | Same public API, no changes needed |
| `main/config.h` | Add DPI_SCALE macro + TOUCH_MIN |
| `bsp/tab5/bsp_config.h` | Add BSP_CHAT_MAX_MESSAGES, BSP_CHAT_POOL_SIZE, BSP_DISPLAY_DPI |
| `main/CMakeLists.txt` | Add new .c files to UI_SRCS |

### Unchanged (callers preserved)
| File | Why unchanged |
|------|--------------|
| `main/voice.c` | Calls ui_chat_push_message/media/card/audio_clip — API preserved |
| `main/ui_home.c` | Calls ui_chat_create/hide — API preserved |
| `main/debug_server.c` | Calls ui_chat_create via navigate — API preserved |

---

## Task 1: BSP Constants + DPI Macro

**Files:**
- Modify: `bsp/tab5/bsp_config.h`
- Modify: `main/config.h`

- [ ] **Step 1: Add BSP chat + DPI constants**

Append to `bsp/tab5/bsp_config.h` after the ES7210 section:

```c
// ---------------------------------------------------------------------------
// Chat UI — pool and history sizing
// ---------------------------------------------------------------------------
#define BSP_CHAT_MAX_MESSAGES   100   /* Per-mode message history depth */
#define BSP_CHAT_POOL_SIZE      12    /* Visible recycled message slots */

// ---------------------------------------------------------------------------
// Display DPI — for portable touch target sizing
// ---------------------------------------------------------------------------
#define BSP_DISPLAY_DPI         218   /* 720px / 3.3 inches */
```

- [ ] **Step 2: Add DPI_SCALE macro to config.h**

Add after the font defines in `main/config.h` (after the FONT_NAV line):

```c
// ---------------------------------------------------------------------------
// DPI-Aware Sizing — scales pixel values for different display densities
// Base: 160 DPI (standard). Tab5 at 218 DPI scales up ~36%.
// Usage: DPI_SCALE(44) = 60px on Tab5 (7mm touch target)
// ---------------------------------------------------------------------------
#define DPI_SCALE(px)   ((px) * BSP_DISPLAY_DPI / 160)
#define TOUCH_MIN       DPI_SCALE(44)   /* Minimum touch target (7mm at 160 DPI) */
```

- [ ] **Step 3: Build to verify**

```bash
cd /home/rebelforce/projects/TinkerTab
export IDF_PATH=/home/rebelforce/esp/esp-idf-v5.4.3
source $IDF_PATH/export.sh 2>/dev/null
python3 $IDF_PATH/tools/idf.py build
```

Expected: Build succeeds. No functional changes yet.

- [ ] **Step 4: Commit**

```bash
git add bsp/tab5/bsp_config.h main/config.h
git commit -m "feat: add BSP_CHAT constants + DPI_SCALE macro for portable sizing"
```

---

## Task 2: Message Data Store (chat_msg_store)

**Files:**
- Create: `main/chat_msg_store.h`
- Create: `main/chat_msg_store.c`

This is the pure-C data layer. No LVGL dependency. Ring buffer per voice mode.

- [ ] **Step 1: Create chat_msg_store.h**

```c
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "bsp_config.h"  /* BSP_CHAT_MAX_MESSAGES */

/* Voice modes — matches config.h VOICE_MODE_* */
#define CHAT_MODE_COUNT  4

typedef enum {
    MSG_TEXT,           /* Plain text (user or AI) */
    MSG_IMAGE,          /* Rendered code/table/photo (JPEG URL from Dragon) */
    MSG_CARD,           /* Link preview / search result */
    MSG_AUDIO_CLIP,     /* Pronunciation / music preview */
    MSG_TOOL_STATUS,    /* "Searching the web..." (ephemeral) */
    MSG_SYSTEM,         /* "Clearing..." / errors (centered) */
} msg_type_t;

typedef struct {
    msg_type_t  type;
    bool        is_user;          /* true = right-aligned user bubble */
    char        text[512];        /* message content or alt text for media */
    char        media_url[256];   /* relative URL for MSG_IMAGE/CARD/AUDIO */
    char        subtitle[128];    /* for MSG_CARD only */
    uint32_t    timestamp;        /* epoch seconds (from RTC) */
    int16_t     height_px;        /* measured bubble height, 0 = unmeasured */
    bool        active;           /* slot is in use */
} chat_msg_t;

/** Initialize the store (zeroes all buffers). Call once at boot. */
void chat_store_init(void);

/** Add a message to the specified mode's ring buffer. Returns index or -1 on error. */
int chat_store_add(uint8_t mode, const chat_msg_t *msg);

/** Get message count for a mode. */
int chat_store_count(uint8_t mode);

/** Get message by index (0 = oldest). Returns NULL if out of range. */
const chat_msg_t *chat_store_get(uint8_t mode, int index);

/** Get mutable pointer (for caching height_px after render). */
chat_msg_t *chat_store_get_mut(uint8_t mode, int index);

/** Clear all messages for a mode (New Chat). */
void chat_store_clear(uint8_t mode);

/** Clear all modes (factory reset). */
void chat_store_clear_all(void);

/** Get the last message for a mode (for streaming append). NULL if empty. */
chat_msg_t *chat_store_last(uint8_t mode);
```

- [ ] **Step 2: Create chat_msg_store.c**

```c
/**
 * Chat Message Store — per-mode ring buffers for conversation history.
 *
 * Pure C, no LVGL dependency. Each voice mode (Local, Hybrid, Cloud, TinkerClaw)
 * has its own ring buffer of BSP_CHAT_MAX_MESSAGES entries. Messages are stored
 * as structs with text, media URLs, timestamps, and cached render heights.
 *
 * Thread safety: NOT thread-safe. All access must be from the LVGL thread
 * (Core 0) or protected by the LVGL lock. The push functions in ui_chat.c
 * use lv_async_call to ensure this.
 */

#include "chat_msg_store.h"
#include <string.h>

static chat_msg_t s_messages[CHAT_MODE_COUNT][BSP_CHAT_MAX_MESSAGES];
static int        s_count[CHAT_MODE_COUNT];
static int        s_write_idx[CHAT_MODE_COUNT];

void chat_store_init(void)
{
    memset(s_messages, 0, sizeof(s_messages));
    memset(s_count, 0, sizeof(s_count));
    memset(s_write_idx, 0, sizeof(s_write_idx));
}

int chat_store_add(uint8_t mode, const chat_msg_t *msg)
{
    if (mode >= CHAT_MODE_COUNT || !msg) return -1;

    int idx = s_write_idx[mode];
    s_messages[mode][idx] = *msg;
    s_messages[mode][idx].active = true;
    s_messages[mode][idx].height_px = 0;  /* uncached — measure on first render */

    s_write_idx[mode] = (idx + 1) % BSP_CHAT_MAX_MESSAGES;
    if (s_count[mode] < BSP_CHAT_MAX_MESSAGES) {
        s_count[mode]++;
    }
    /* If count == MAX, oldest message is overwritten (ring buffer) */

    return s_count[mode] - 1;  /* return logical index of new message */
}

int chat_store_count(uint8_t mode)
{
    if (mode >= CHAT_MODE_COUNT) return 0;
    return s_count[mode];
}

const chat_msg_t *chat_store_get(uint8_t mode, int index)
{
    if (mode >= CHAT_MODE_COUNT || index < 0 || index >= s_count[mode]) return NULL;

    /* Ring buffer: oldest message is at (write_idx - count) wrapped */
    int start = (s_write_idx[mode] - s_count[mode] + BSP_CHAT_MAX_MESSAGES) % BSP_CHAT_MAX_MESSAGES;
    int real_idx = (start + index) % BSP_CHAT_MAX_MESSAGES;
    return &s_messages[mode][real_idx];
}

chat_msg_t *chat_store_get_mut(uint8_t mode, int index)
{
    return (chat_msg_t *)chat_store_get(mode, index);
}

void chat_store_clear(uint8_t mode)
{
    if (mode >= CHAT_MODE_COUNT) return;
    memset(s_messages[mode], 0, sizeof(s_messages[mode]));
    s_count[mode] = 0;
    s_write_idx[mode] = 0;
}

void chat_store_clear_all(void)
{
    for (int i = 0; i < CHAT_MODE_COUNT; i++) {
        chat_store_clear(i);
    }
}

chat_msg_t *chat_store_last(uint8_t mode)
{
    if (mode >= CHAT_MODE_COUNT || s_count[mode] == 0) return NULL;
    int idx = (s_write_idx[mode] - 1 + BSP_CHAT_MAX_MESSAGES) % BSP_CHAT_MAX_MESSAGES;
    return &s_messages[mode][idx];
}
```

- [ ] **Step 3: Add to CMakeLists.txt**

In `main/CMakeLists.txt`, add `"chat_msg_store.c"` to the UI_SRCS list:

```cmake
set(UI_SRCS
    "ui_splash.c" "ui_home.c" "ui_settings.c"
    "ui_camera.c" "ui_files.c" "ui_audio.c" "ui_keyboard.c" "ui_voice.c" "ui_wifi.c"
    "ui_chat.c" "ui_notes.c" "ui_feedback.c"
    "chat_msg_store.c"
)
```

- [ ] **Step 4: Build to verify**

```bash
python3 $IDF_PATH/tools/idf.py build
```

Expected: Compiles clean. chat_msg_store.c has no LVGL dependency.

- [ ] **Step 5: Commit**

```bash
git add main/chat_msg_store.h main/chat_msg_store.c main/CMakeLists.txt
git commit -m "feat: chat_msg_store — per-mode ring buffer message data layer (pure C, no LVGL)"
```

---

## Task 3: Chat Header Widget (chat_header)

**Files:**
- Create: `main/chat_header.h`
- Create: `main/chat_header.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create chat_header.h**

```c
#pragma once

#include "lvgl.h"
#include <stdint.h>

typedef struct {
    lv_obj_t *container;     /* horizontal flex row */
    lv_obj_t *back_btn;      /* back arrow (left) */
    lv_obj_t *title;         /* screen title (flex-grow) */
    lv_obj_t *status_dot;    /* connection indicator dot */
    lv_obj_t *status_label;  /* "Ready" / "Processing..." */
    lv_obj_t *action_btn;    /* optional right button (+ for new chat) */
    lv_obj_t *mode_badge;    /* mode indicator (right) */
} chat_header_t;

/**
 * Create a reusable header widget. Returns heap-allocated struct (caller frees on destroy).
 * @param parent      LVGL parent object
 * @param title       Screen title ("Chat", "Notes", etc.)
 * @param accent_color Mode accent color (hex)
 * @param show_action true to show + button
 */
chat_header_t *chat_header_create(lv_obj_t *parent, const char *title,
                                   uint32_t accent_color, bool show_action);

void chat_header_set_status(chat_header_t *hdr, const char *text, bool connected);
void chat_header_set_mode(chat_header_t *hdr, const char *mode_name, uint32_t color);
void chat_header_set_back_cb(chat_header_t *hdr, lv_event_cb_t cb, void *user_data);
void chat_header_set_action_cb(chat_header_t *hdr, lv_event_cb_t cb, void *user_data);
```

- [ ] **Step 2: Create chat_header.c**

```c
/**
 * Chat Header — reusable composite widget.
 * Flex row: [back] [title (grow)] [status dot] [status text] [+action] [mode badge]
 * Uses DPI_SCALE for portable sizing. TOUCH_MIN for button hit areas.
 */

#include "chat_header.h"
#include "config.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "chat_hdr";

chat_header_t *chat_header_create(lv_obj_t *parent, const char *title,
                                   uint32_t accent_color, bool show_action)
{
    chat_header_t *hdr = calloc(1, sizeof(chat_header_t));
    if (!hdr) { ESP_LOGE(TAG, "OOM"); return NULL; }

    /* Container: flex row, full width, fixed height */
    hdr->container = lv_obj_create(parent);
    if (!hdr->container) { free(hdr); return NULL; }
    lv_obj_remove_style_all(hdr->container);
    lv_obj_set_size(hdr->container, lv_pct(100), DPI_SCALE(48));
    lv_obj_set_style_bg_color(hdr->container, lv_color_hex(0x0F0F1A), 0);
    lv_obj_set_style_bg_opa(hdr->container, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(hdr->container, DPI_SCALE(8), 0);
    lv_obj_set_style_pad_ver(hdr->container, DPI_SCALE(4), 0);
    lv_obj_set_flex_flow(hdr->container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr->container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr->container, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    hdr->back_btn = lv_button_create(hdr->container);
    lv_obj_set_size(hdr->back_btn, TOUCH_MIN, TOUCH_MIN);
    lv_obj_set_style_bg_opa(hdr->back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(hdr->back_btn, 0, 0);
    lv_obj_set_style_border_width(hdr->back_btn, 0, 0);
    lv_obj_t *back_lbl = lv_label_create(hdr->back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, FONT_HEADING, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(back_lbl);

    /* Title — flex-grow fills remaining space */
    hdr->title = lv_label_create(hdr->container);
    lv_label_set_text(hdr->title, title);
    lv_obj_set_style_text_font(hdr->title, FONT_HEADING, 0);
    lv_obj_set_style_text_color(hdr->title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_flex_grow(hdr->title, 1);

    /* Status dot */
    hdr->status_dot = lv_obj_create(hdr->container);
    lv_obj_remove_style_all(hdr->status_dot);
    lv_obj_set_size(hdr->status_dot, DPI_SCALE(8), DPI_SCALE(8));
    lv_obj_set_style_radius(hdr->status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hdr->status_dot, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_bg_opa(hdr->status_dot, LV_OPA_COVER, 0);

    /* Status label */
    hdr->status_label = lv_label_create(hdr->container);
    lv_label_set_text(hdr->status_label, "Ready");
    lv_obj_set_style_text_font(hdr->status_label, FONT_SMALL, 0);
    lv_obj_set_style_text_color(hdr->status_label, lv_color_hex(0x22C55E), 0);

    /* Action button (+ for new chat) — optional */
    if (show_action) {
        hdr->action_btn = lv_button_create(hdr->container);
        lv_obj_set_size(hdr->action_btn, TOUCH_MIN, DPI_SCALE(32));
        lv_obj_set_style_bg_color(hdr->action_btn, lv_color_hex(0x222233), 0);
        lv_obj_set_style_bg_opa(hdr->action_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hdr->action_btn, DPI_SCALE(8), 0);
        lv_obj_set_style_shadow_width(hdr->action_btn, 0, 0);
        lv_obj_set_style_border_width(hdr->action_btn, 0, 0);
        lv_obj_t *plus_lbl = lv_label_create(hdr->action_btn);
        lv_label_set_text(plus_lbl, "+");
        lv_obj_set_style_text_font(plus_lbl, FONT_HEADING, 0);
        lv_obj_set_style_text_color(plus_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(plus_lbl);
    }

    /* Mode badge */
    hdr->mode_badge = lv_label_create(hdr->container);
    lv_label_set_text(hdr->mode_badge, "Local");
    lv_obj_set_style_text_font(hdr->mode_badge, FONT_SMALL, 0);
    lv_obj_set_style_text_color(hdr->mode_badge, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_color(hdr->mode_badge, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_opa(hdr->mode_badge, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(hdr->mode_badge, DPI_SCALE(8), 0);
    lv_obj_set_style_pad_ver(hdr->mode_badge, DPI_SCALE(2), 0);
    lv_obj_set_style_radius(hdr->mode_badge, DPI_SCALE(6), 0);

    return hdr;
}

void chat_header_set_status(chat_header_t *hdr, const char *text, bool connected)
{
    if (!hdr) return;
    if (hdr->status_label) lv_label_set_text(hdr->status_label, text);
    uint32_t col = connected ? 0x22C55E : 0xFF453A;
    if (hdr->status_dot) lv_obj_set_style_bg_color(hdr->status_dot, lv_color_hex(col), 0);
    if (hdr->status_label) lv_obj_set_style_text_color(hdr->status_label, lv_color_hex(col), 0);
}

void chat_header_set_mode(chat_header_t *hdr, const char *mode_name, uint32_t color)
{
    if (!hdr || !hdr->mode_badge) return;
    lv_label_set_text(hdr->mode_badge, mode_name);
    lv_obj_set_style_text_color(hdr->mode_badge, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(hdr->mode_badge, lv_color_hex(color), 0);
}

void chat_header_set_back_cb(chat_header_t *hdr, lv_event_cb_t cb, void *user_data)
{
    if (hdr && hdr->back_btn) lv_obj_add_event_cb(hdr->back_btn, cb, LV_EVENT_CLICKED, user_data);
}

void chat_header_set_action_cb(chat_header_t *hdr, lv_event_cb_t cb, void *user_data)
{
    if (hdr && hdr->action_btn) lv_obj_add_event_cb(hdr->action_btn, cb, LV_EVENT_CLICKED, user_data);
}
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `"chat_header.c"` to UI_SRCS.

- [ ] **Step 4: Build**

```bash
python3 $IDF_PATH/tools/idf.py build
```

- [ ] **Step 5: Commit**

```bash
git add main/chat_header.h main/chat_header.c main/CMakeLists.txt
git commit -m "feat: chat_header — reusable composite header widget with flex layout + DPI_SCALE"
```

---

## Task 4: Chat Input Bar Widget (chat_input_bar)

**Files:**
- Create: `main/chat_input_bar.h`
- Create: `main/chat_input_bar.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create chat_input_bar.h**

```c
#pragma once

#include "lvgl.h"
#include <stdint.h>

typedef struct {
    lv_obj_t *container;     /* horizontal flex row */
    lv_obj_t *mic_btn;       /* microphone button (left) */
    lv_obj_t *textarea;      /* text input (center, flex-grow) */
    lv_obj_t *send_btn;      /* send button (right) */
} chat_input_bar_t;

/**
 * Create a reusable input bar widget. Returns heap-allocated struct.
 * Uses flex row layout. Textarea fills available space.
 * Touch targets use DPI_SCALE for portability.
 */
chat_input_bar_t *chat_input_bar_create(lv_obj_t *parent, uint32_t accent_color);

void chat_input_bar_set_callbacks(chat_input_bar_t *bar,
                                  lv_event_cb_t on_send,
                                  lv_event_cb_t on_mic,
                                  lv_event_cb_t on_ta_click);
const char *chat_input_bar_get_text(chat_input_bar_t *bar);
void chat_input_bar_clear(chat_input_bar_t *bar);
void chat_input_bar_set_text(chat_input_bar_t *bar, const char *text);
lv_obj_t *chat_input_bar_get_textarea(chat_input_bar_t *bar);
```

- [ ] **Step 2: Create chat_input_bar.c**

```c
/**
 * Chat Input Bar — reusable composite widget.
 * Flex row: [mic button] [textarea (grow)] [send button]
 * Used by chat, voice overlay, and notes screens.
 */

#include "chat_input_bar.h"
#include "config.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "input_bar";

chat_input_bar_t *chat_input_bar_create(lv_obj_t *parent, uint32_t accent_color)
{
    chat_input_bar_t *bar = calloc(1, sizeof(chat_input_bar_t));
    if (!bar) { ESP_LOGE(TAG, "OOM"); return NULL; }

    /* Container: flex row, full width */
    bar->container = lv_obj_create(parent);
    if (!bar->container) { free(bar); return NULL; }
    lv_obj_remove_style_all(bar->container);
    lv_obj_set_size(bar->container, lv_pct(100), DPI_SCALE(56));
    lv_obj_set_style_bg_color(bar->container, lv_color_hex(0x0F0F1A), 0);
    lv_obj_set_style_bg_opa(bar->container, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar->container, DPI_SCALE(8), 0);
    lv_obj_set_style_pad_ver(bar->container, DPI_SCALE(6), 0);
    lv_obj_set_style_pad_gap(bar->container, DPI_SCALE(8), 0);
    lv_obj_set_flex_flow(bar->container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar->container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(bar->container, 1, 0);
    lv_obj_set_style_border_color(bar->container, lv_color_hex(0x222233), 0);
    lv_obj_set_style_border_side(bar->container, LV_BORDER_SIDE_TOP, 0);

    /* Mic button */
    bar->mic_btn = lv_button_create(bar->container);
    lv_obj_set_size(bar->mic_btn, TOUCH_MIN, TOUCH_MIN);
    lv_obj_set_style_radius(bar->mic_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar->mic_btn, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_opa(bar->mic_btn, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(bar->mic_btn, 0, 0);
    lv_obj_set_style_border_width(bar->mic_btn, 0, 0);
    lv_obj_t *mic_icon = lv_label_create(bar->mic_btn);
    lv_label_set_text(mic_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(mic_icon, FONT_BODY, 0);
    lv_obj_set_style_text_color(mic_icon, lv_color_hex(accent_color), 0);
    lv_obj_center(mic_icon);

    /* Textarea — flex-grow fills remaining space */
    bar->textarea = lv_textarea_create(bar->container);
    lv_obj_set_flex_grow(bar->textarea, 1);
    lv_obj_set_height(bar->textarea, DPI_SCALE(40));
    lv_textarea_set_placeholder_text(bar->textarea, "Type a message...");
    lv_textarea_set_one_line(bar->textarea, true);
    lv_obj_set_style_bg_color(bar->textarea, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(bar->textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar->textarea, 1, 0);
    lv_obj_set_style_border_color(bar->textarea, lv_color_hex(0x333344), 0);
    lv_obj_set_style_radius(bar->textarea, DPI_SCALE(20), 0);
    lv_obj_set_style_text_font(bar->textarea, FONT_BODY, 0);
    lv_obj_set_style_text_color(bar->textarea, lv_color_hex(0xE0E0E8), 0);
    lv_obj_set_style_pad_hor(bar->textarea, DPI_SCALE(12), 0);

    /* Send button */
    bar->send_btn = lv_button_create(bar->container);
    lv_obj_set_size(bar->send_btn, DPI_SCALE(64), TOUCH_MIN);
    lv_obj_set_style_bg_color(bar->send_btn, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_bg_opa(bar->send_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar->send_btn, DPI_SCALE(20), 0);
    lv_obj_set_style_shadow_width(bar->send_btn, 0, 0);
    lv_obj_set_style_border_width(bar->send_btn, 0, 0);
    lv_obj_t *send_lbl = lv_label_create(bar->send_btn);
    lv_label_set_text(send_lbl, "Send");
    lv_obj_set_style_text_font(send_lbl, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(send_lbl);

    return bar;
}

void chat_input_bar_set_callbacks(chat_input_bar_t *bar,
                                  lv_event_cb_t on_send,
                                  lv_event_cb_t on_mic,
                                  lv_event_cb_t on_ta_click)
{
    if (!bar) return;
    if (on_send && bar->send_btn) lv_obj_add_event_cb(bar->send_btn, on_send, LV_EVENT_CLICKED, bar);
    if (on_mic && bar->mic_btn)   lv_obj_add_event_cb(bar->mic_btn, on_mic, LV_EVENT_CLICKED, bar);
    if (on_ta_click && bar->textarea) lv_obj_add_event_cb(bar->textarea, on_ta_click, LV_EVENT_CLICKED, bar);
    /* LV_EVENT_READY = Done key on keyboard → same as send */
    if (on_send && bar->textarea) lv_obj_add_event_cb(bar->textarea, on_send, LV_EVENT_READY, bar);
}

const char *chat_input_bar_get_text(chat_input_bar_t *bar)
{
    if (!bar || !bar->textarea) return "";
    return lv_textarea_get_text(bar->textarea);
}

void chat_input_bar_clear(chat_input_bar_t *bar)
{
    if (bar && bar->textarea) lv_textarea_set_text(bar->textarea, "");
}

void chat_input_bar_set_text(chat_input_bar_t *bar, const char *text)
{
    if (bar && bar->textarea) lv_textarea_set_text(bar->textarea, text ? text : "");
}

lv_obj_t *chat_input_bar_get_textarea(chat_input_bar_t *bar)
{
    return bar ? bar->textarea : NULL;
}
```

- [ ] **Step 3: Add to CMakeLists.txt, build, commit**

Add `"chat_input_bar.c"` to UI_SRCS. Build. Commit:

```bash
git add main/chat_input_bar.h main/chat_input_bar.c main/CMakeLists.txt
git commit -m "feat: chat_input_bar — reusable composite input widget with flex layout"
```

---

## Task 5: Chat Suggestions (chat_suggestions)

**Files:**
- Create: `main/chat_suggestions.h`
- Create: `main/chat_suggestions.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create chat_suggestions.h**

```c
#pragma once

#include "lvgl.h"
#include <stdint.h>

/** Create mode-specific suggestion cards as children of parent. */
void chat_suggestions_create(lv_obj_t *parent, uint8_t mode, lv_event_cb_t on_tap);

/** Show suggestion cards (empty conversation state). */
void chat_suggestions_show(void);

/** Hide suggestion cards (messages exist). */
void chat_suggestions_hide(void);

/** Check if suggestions are currently visible. */
bool chat_suggestions_visible(void);
```

- [ ] **Step 2: Create chat_suggestions.c**

```c
/**
 * Mode-specific suggestion cards for empty chat state.
 * Shows 4 tappable prompt suggestions based on current voice mode.
 * Uses flex column layout, DPI_SCALE sizing.
 */

#include "chat_suggestions.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "suggestions";

static lv_obj_t *s_container = NULL;

/* Suggestion texts per mode */
static const char *s_suggestions[4][4] = {
    /* Local */
    { "\"Tell me a joke\"",
      "\"What time is it?\"",
      "\"Set a 5 minute timer\"",
      "\"How's the weather?\"" },
    /* Hybrid */
    { "\"Tell me a joke\"",
      "\"What time is it?\"",
      "\"Set a 5 minute timer\"",
      "\"Translate hello to French\"" },
    /* Cloud */
    { "\"Explain quantum computing\"",
      "\"Write a Python function\"",
      "\"Compare AirPods vs Sony XM5\"",
      "\"Summarize this article...\"" },
    /* TinkerClaw */
    { "\"What do you know about me?\"",
      "\"Search the web for...\"",
      "\"Remember that I like sushi\"",
      "\"What can you do?\"" },
};

/* Mode accent colors */
static const uint32_t s_mode_colors[4] = {
    0x22C55E,  /* Local: green */
    0xEAB308,  /* Hybrid: yellow */
    0x3B82F6,  /* Cloud: blue */
    0xE8457A,  /* TinkerClaw: rose */
};

/* Mode hints */
static const char *s_mode_hints[4] = {
    "Ask me anything!\nFast local AI, private.",
    "Ask me anything!\nLocal AI, cloud audio.",
    "Ask me anything!\nPowered by Claude / GPT-4o.",
    "I'm your AI agent.\nMemory, web search, tools.",
};

void chat_suggestions_create(lv_obj_t *parent, uint8_t mode, lv_event_cb_t on_tap)
{
    if (mode >= 4) mode = 0;

    /* Destroy old container if mode changed */
    if (s_container) {
        lv_obj_del(s_container);
        s_container = NULL;
    }

    s_container = lv_obj_create(parent);
    lv_obj_remove_style_all(s_container);
    lv_obj_set_size(s_container, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_align(s_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(s_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_container, DPI_SCALE(8), 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Hint text */
    lv_obj_t *hint = lv_label_create(s_container);
    lv_label_set_text(hint, s_mode_hints[mode]);
    lv_obj_set_style_text_font(hint, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(hint, DPI_SCALE(12), 0);

    /* Suggestion cards */
    uint32_t color = s_mode_colors[mode];
    for (int i = 0; i < 4; i++) {
        lv_obj_t *card = lv_obj_create(s_container);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, lv_pct(100), DPI_SCALE(48));
        lv_obj_set_style_bg_color(card, lv_color_hex(0x111122), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, DPI_SCALE(10), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x222233), 0);
        lv_obj_set_style_pad_hor(card, DPI_SCALE(14), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, s_suggestions[mode][i]);
        lv_obj_set_style_text_font(lbl, FONT_SECONDARY, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        if (on_tap) {
            lv_obj_add_event_cb(card, on_tap, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
        }
    }
}

void chat_suggestions_show(void)
{
    if (s_container) lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
}

void chat_suggestions_hide(void)
{
    if (s_container) lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
}

bool chat_suggestions_visible(void)
{
    return s_container && !lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN);
}
```

- [ ] **Step 3: Add to CMakeLists.txt, build, commit**

Add `"chat_suggestions.c"` to UI_SRCS. Build. Commit:

```bash
git add main/chat_suggestions.h main/chat_suggestions.c main/CMakeLists.txt
git commit -m "feat: chat_suggestions — mode-specific suggestion cards with DPI scaling"
```

---

## Task 6: Message View with Object Recycling (chat_msg_view)

This is the core module — the recycled object pool and scroll handling. This is the most complex task.

**Files:**
- Create: `main/chat_msg_view.h`
- Create: `main/chat_msg_view.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create chat_msg_view.h**

```c
#pragma once

#include "lvgl.h"
#include <stdint.h>

/** Initialize the recycled message view inside a parent container. */
void chat_view_init(lv_obj_t *parent);

/** Switch to a different mode's message history. Refreshes the view. */
void chat_view_set_mode(uint8_t mode);

/** Get current mode. */
uint8_t chat_view_get_mode(void);

/** Refresh the visible window from the store (call after adding messages). */
void chat_view_refresh(void);

/** Scroll to the bottom (latest message). */
void chat_view_scroll_to_bottom(void);

/** Append streaming token to the last AI message. Auto-scrolls. */
void chat_view_append_streaming(const char *token);

/** Finalize streaming — commit accumulated text to store, measure height. */
void chat_view_finalize_streaming(void);

/** Check if currently streaming. */
bool chat_view_is_streaming(void);

/** Get the scroll container (for keyboard layout callbacks). */
lv_obj_t *chat_view_get_scroll(void);
```

- [ ] **Step 2: Create chat_msg_view.c**

This is the largest new file. I'll provide the complete implementation with shared styles, pool management, and scroll handling.

The implementer should create this file with:
1. Static shared styles (user bubble, AI bubble, tool, system, timestamp, card) — initialized once via `init_styles()`
2. Pool of `BSP_CHAT_POOL_SIZE` msg_slot_t structs, each with container + content label + timestamp
3. `configure_slot()` function that sets a slot's content from a `chat_msg_t` (text, style, position)
4. `refresh_visible()` function that maps data indices to pool slots based on scroll position
5. Scroll event handler that triggers `refresh_visible()` on scroll
6. Streaming mode: last slot pinned, text appended, height remeasured
7. Media handling: when slot shows MSG_IMAGE, create lv_image on demand (download via media_cache_fetch in background task)

Key implementation details:
- Pool slots are children of `s_scroll` container
- Virtual content height = sum of all message heights in store
- `lv_obj_set_content_height(s_scroll, total_height)` sets virtual scrollable area
- Each slot positioned with `lv_obj_set_pos(slot->container, x, y)` based on message position in virtual space
- Heights cached in `chat_msg_t.height_px` — estimated on first pass, refined on render
- Default height estimate: `60 + (strlen(text) / 40) * 20` pixels

The implementer MUST read the existing `ui_chat.c` to understand:
- How `add_message()` currently works (lines 1959-2056)
- How bubble styling is applied (colors, padding, radius)
- How timestamps are formatted
- How rich media bubbles differ from text bubbles
- How the streaming accumulation works (s_assist_bubble, s_assist_label)

- [ ] **Step 3: Add to CMakeLists.txt, build, commit**

Add `"chat_msg_view.c"` to UI_SRCS. Build. Commit:

```bash
git add main/chat_msg_view.h main/chat_msg_view.c main/CMakeLists.txt
git commit -m "feat: chat_msg_view — recycled object pool with virtual scroll + shared styles"
```

---

## Task 7: Rewrite ui_chat.c (Orchestrator)

This is the integration task. Replace the 2747-line monolith with a ~300-line orchestrator.

**Files:**
- Rewrite: `main/ui_chat.c`

The new ui_chat.c:
1. Preserves ALL public API signatures from ui_chat.h (no changes to voice.c callers)
2. Creates overlay with: header + accent bar + scroll area (msg_view) + input bar
3. No Chat Home panel — go straight to conversation
4. Delegates to modules: chat_header for header, chat_input_bar for input, chat_msg_view for messages, chat_suggestions for empty state
5. Handles: mode cycling (tap badge), new chat (tap +), back (tap ‹), keyboard integration, voice state polling

The push functions (push_message, push_media, push_card, push_audio_clip, update_last_message) convert their args into `chat_msg_t` structs, add to the store, and call `chat_view_refresh()`.

- [ ] **Step 1: Rewrite ui_chat.c**

The implementer must:
1. Read the current ui_chat.c thoroughly to understand ALL features
2. Keep all #include statements needed
3. Keep the `s_overlay`, `s_active` state pattern
4. Keep `ui_chat_create()` / `ui_chat_hide()` / `ui_chat_destroy()` lifecycle
5. Keep `poll_voice_cb` timer for status updates
6. Keep keyboard layout callback integration
7. Replace build_home_panel() with nothing (deleted)
8. Replace build_conversation_ui() with module creation calls
9. Replace add_message() with store_add + view_refresh
10. Replace push_media/card/audio callbacks with store_add + view_refresh
11. Keep thread-safe push pattern (malloc + strdup + lv_async_call)

- [ ] **Step 2: Build and fix any compilation errors**

```bash
python3 $IDF_PATH/tools/idf.py build
```

- [ ] **Step 3: Commit**

```bash
git add main/ui_chat.c
git commit -m "feat: ui_chat.c rewrite — thin orchestrator using modular chat components"
```

---

## Task 8: Flash + Integration Test

- [ ] **Step 1: Full clean build**

```bash
python3 $IDF_PATH/tools/idf.py fullclean
python3 $IDF_PATH/tools/idf.py set-target esp32p4
python3 $IDF_PATH/tools/idf.py build
```

- [ ] **Step 2: Flash**

```bash
python3 $IDF_PATH/tools/idf.py -p /dev/ttyACM0 flash
```

- [ ] **Step 3: Verify boot + selftest**

```bash
TOKEN=dd9cc86de572e4a69b28456048c7f1a9
curl -s http://192.168.1.90:8080/selftest | python3 -m json.tool
```

Expected: 8/8 pass.

- [ ] **Step 4: Test chat navigation**

```bash
# Tap Chat nav
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=chat"
sleep 3
curl -s -H "Authorization: Bearer $TOKEN" -o /tmp/chat_new.bmp http://192.168.1.90:8080/screenshot
```

Expected: Conversation view opens directly (no Chat Home panel). Mode-specific suggestions visible.

- [ ] **Step 5: Test message send**

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/chat -d '{"text":"Hello Tinker"}'
sleep 15
curl -s -H "Authorization: Bearer $TOKEN" -o /tmp/chat_msg.bmp http://192.168.1.90:8080/screenshot
```

Expected: User message + AI response visible. Suggestions hidden.

- [ ] **Step 6: Heap stability test**

```bash
for round in 1 2 3 4 5; do
  for screen in home settings chat notes home; do
    curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=$screen" >/dev/null 2>&1
    sleep 2
  done
  curl -s http://192.168.1.90:8080/selftest | python3 -c "import sys,json; [print(f'Round $round: free={t[\"free_kb\"]}KB frag={t[\"fragmentation_pct\"]}%') for t in json.load(sys.stdin)['tests'] if t['name']=='internal_heap']"
done
```

Expected: Heap stable across all rounds. No fragmentation growth.

- [ ] **Step 7: Commit final**

```bash
git add -A
git commit -m "test: chat redesign verified — 54 objects, stable heap, all modes working"
git push
```
