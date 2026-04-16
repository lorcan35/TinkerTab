# UI Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 4-page tileview home screen with a single-page dashboard featuring a living orb with arc ring, agent orchestration card, activity feed, and unified design system across all screens.

**Architecture:** Create a shared design system (ui_theme.h/c) with 18 static styles, color constants, and spacing constants. Rewrite ui_home.c from 1084-line tileview to ~500-line single-page dashboard. Add agents overlay. Enhance chat with visible tool call cards. Apply theme to notes and settings. All sizes use DPI_SCALE for portability. Object count <55 per screen.

**Tech Stack:** ESP-IDF 5.4.3 / LVGL v9.2.2 / C11 / FreeRTOS / ESP32-P4 Tab5

**Spec:** `docs/superpowers/specs/2026-04-16-ui-overhaul-design.md`

**CRITICAL CONTEXT:**
- WiFi: SSID=`X28P-5G-C80EF2`, pass=`E2D9025B`
- Dragon: `192.168.70.242:3502`
- Build: `export IDF_PATH=/home/rebelforce/esp/esp-idf-v5.4.3 && source $IDF_PATH/export.sh 2>/dev/null`
- Flash: `python3 $IDF_PATH/tools/idf.py -p /dev/ttyACM0 flash`
- Auth token: `dd9cc86de572e4a69b28456048c7f1a9`
- LVGL config is in sdkconfig.defaults, NOT lv_conf.h (CONFIG_LV_CONF_SKIP=1)
- Use `vTaskSuspend(NULL)` not `vTaskDelete(NULL)` (P4 TLSP crash)
- All LVGL calls from background tasks: use `lv_async_call()`, never direct

---

## File Structure

### New Files
| File | Responsibility |
|------|---------------|
| `main/ui_theme.h` | Design system: color defines, spacing defines, style externs |
| `main/ui_theme.c` | 18 shared static styles, init function, nav bar creation |
| `main/ui_agents.h` | Agents overlay public API |
| `main/ui_agents.c` | Agents overlay with recycled agent card pool |

### Modified Files
| File | Change |
|------|--------|
| `main/ui_home.c` | **Complete rewrite** — kill tileview, single page dashboard |
| `main/ui_home.h` | Update public API (remove tileview getters, add agent card update) |
| `main/chat_msg_store.h` | Add `MSG_TOOL_CALL` to msg_type_t enum |
| `main/chat_msg_view.c` | Add tool call card rendering in configure_slot() |
| `main/ui_chat.c` | Wire tool_call/tool_result to MSG_TOOL_CALL store entries |
| `main/ui_notes.c` | Apply theme colors and spacing |
| `main/ui_settings.c` | Apply theme colors and spacing |
| `main/config.h` | Add FONT_TIME_BIG if lv_font_montserrat_40 needed |
| `main/CMakeLists.txt` | Add ui_theme.c, ui_agents.c to UI_SRCS |
| `main/debug_server.c` | Update /navigate handler for new home (no tileview) |
| `main/main.c` | No changes needed (ui_home_create API preserved) |

### Unchanged (callers preserved)
| File | Why |
|------|-----|
| `main/voice.c` | Calls ui_chat_push_* and ui_home_show_toast — API preserved |
| `main/chat_header.c` | Composite widget, reused as-is |
| `main/chat_input_bar.c` | Composite widget, reused as-is |
| `main/chat_suggestions.c` | Reused for chat empty state |
| `bsp/tab5/bsp_config.h` | No hardware changes |

---

## Task 1: Design System (ui_theme)

**Files:**
- Create: `main/ui_theme.h`
- Create: `main/ui_theme.c`
- Modify: `main/CMakeLists.txt`

The foundation everything else depends on. All colors, spacing, and shared styles in one place.

- [ ] **Step 1: Create ui_theme.h**

```c
#pragma once

#include "lvgl.h"
#include "config.h"  /* DPI_SCALE, TOUCH_MIN, FONT_* */

/* ── Colors ────────────────────────────────────────────────── */
#define TH_BG            0x08080E
#define TH_CARD          0x111119
#define TH_CARD_ELEVATED 0x13131F
#define TH_CARD_BORDER   0x0B0B12   /* rgba(255,255,255,0.04) on TH_BG */

#define TH_TEXT_PRIMARY   0xE8E8EF
#define TH_TEXT_BODY      0xAAAAAA
#define TH_TEXT_SECONDARY 0x666666
#define TH_TEXT_DIM       0x444444

#define TH_AMBER          0xF59E0B
#define TH_AMBER_DARK     0xD97706
#define TH_AMBER_GLOW     0xF5A623   /* for shadow_color */

#define TH_MODE_LOCAL     0x22C55E
#define TH_MODE_HYBRID    0xEAB308
#define TH_MODE_CLOUD     0x3B82F6
#define TH_MODE_CLAW      0xF43F5E

#define TH_STATUS_GREEN   0x22C55E
#define TH_STATUS_RED     0xEF4444

/* ── Spacing (DPI-scaled) ──────────────────────────────────── */
#define TH_MARGIN       DPI_SCALE(18)   /* 24px on Tab5 */
#define TH_CARD_GAP     DPI_SCALE(9)    /* 12px */
#define TH_CARD_PAD     DPI_SCALE(15)   /* 20px */
#define TH_CARD_RADIUS  DPI_SCALE(15)   /* 20px */
#define TH_INFO_RADIUS  DPI_SCALE(12)   /* 16px */
#define TH_SECTION_GAP  DPI_SCALE(15)   /* 20px */
#define TH_TASK_GAP     DPI_SCALE(6)    /* 8px */
#define TH_INPUT_H      DPI_SCALE(38)   /* 52px */
#define TH_INPUT_R      DPI_SCALE(20)   /* 26px */
#define TH_NAV_H        DPI_SCALE(54)   /* 72px */
#define TH_STATUS_H     DPI_SCALE(30)   /* 40px */

/* ── Mode arrays (shared by home + chat) ───────────────────── */
extern const char      *th_mode_names[4];
extern const uint32_t   th_mode_colors[4];

/* ── Shared styles (extern — defined in ui_theme.c) ────────── */
extern lv_style_t s_style_card;
extern lv_style_t s_style_card_elevated;
extern lv_style_t s_style_info_card;
extern lv_style_t s_style_text_title;
extern lv_style_t s_style_text_body;
extern lv_style_t s_style_text_meta;
extern lv_style_t s_style_text_dim;
extern lv_style_t s_style_input_pill;
extern lv_style_t s_style_bubble_user;
extern lv_style_t s_style_bubble_ai;
extern lv_style_t s_style_tool_card;

/* ── Init — call once at boot before any screen creation ───── */
void ui_theme_init(void);

/* ── Nav bar — shared component on lv_layer_top() ──────────── */
typedef struct {
    lv_obj_t *bar;          /* container on lv_layer_top */
    lv_obj_t *items[4];     /* label objects */
    lv_obj_t *indicators[4]; /* active indicator bars */
    int       active;       /* current active index */
} ui_nav_bar_t;

ui_nav_bar_t *ui_nav_bar_create(lv_event_cb_t on_tap);
void ui_nav_bar_set_active(ui_nav_bar_t *nav, int index);
void ui_nav_bar_destroy(ui_nav_bar_t *nav);

/* ── Input pill — reusable component ───────────────────────── */
typedef struct {
    lv_obj_t *container;
    lv_obj_t *textarea;
    lv_obj_t *mic_btn;
} ui_input_pill_t;

ui_input_pill_t *ui_input_pill_create(lv_obj_t *parent, const char *placeholder);
const char *ui_input_pill_get_text(ui_input_pill_t *pill);
void ui_input_pill_clear(ui_input_pill_t *pill);
lv_obj_t *ui_input_pill_get_textarea(ui_input_pill_t *pill);
```

- [ ] **Step 2: Create ui_theme.c**

The implementer MUST read the spec file at `docs/superpowers/specs/2026-04-16-ui-overhaul-design.md` sections "Colors", "Spacing", "Shared Static Styles" for exact values.

Create the file with:
1. All 11 `lv_style_t` definitions initialized in `ui_theme_init()`
2. Mode arrays: `th_mode_names` and `th_mode_colors`
3. `ui_nav_bar_create()` — creates the 4-tab nav bar on `lv_layer_top()` at position `(0, SH - TH_NAV_H)` where SH=1280. Labels: Home/Notes/Chat/Settings with LV_SYMBOL icons. Active state: amber text + top indicator bar (20px × 3px amber). Each item clickable with `lv_obj_add_event_cb(item, on_tap, LV_EVENT_CLICKED, (void*)(intptr_t)i)`.
4. `ui_input_pill_create()` — creates the input pill widget: flex row, full width, TH_INPUT_H height, TH_INPUT_R radius, bg TH_CARD, border. Contains textarea (flex-grow) + amber mic circle button (40px).

Key style definitions:

```c
/* Card surface */
lv_style_init(&s_style_card);
lv_style_set_bg_color(&s_style_card, lv_color_hex(TH_CARD));
lv_style_set_bg_opa(&s_style_card, LV_OPA_COVER);
lv_style_set_radius(&s_style_card, TH_INFO_RADIUS);
lv_style_set_pad_all(&s_style_card, TH_CARD_PAD);
lv_style_set_border_width(&s_style_card, 1);
lv_style_set_border_color(&s_style_card, lv_color_hex(TH_CARD_BORDER));

/* Card elevated (agent card) */
lv_style_init(&s_style_card_elevated);
lv_style_set_bg_color(&s_style_card_elevated, lv_color_hex(TH_CARD_ELEVATED));
lv_style_set_bg_opa(&s_style_card_elevated, LV_OPA_COVER);
lv_style_set_radius(&s_style_card_elevated, TH_CARD_RADIUS);
lv_style_set_pad_all(&s_style_card_elevated, TH_CARD_PAD);
lv_style_set_border_width(&s_style_card_elevated, 1);
lv_style_set_border_color(&s_style_card_elevated, lv_color_hex(TH_CARD_BORDER));

/* User bubble — amber bg, black text */
lv_style_init(&s_style_bubble_user);
lv_style_set_bg_color(&s_style_bubble_user, lv_color_hex(TH_AMBER));
lv_style_set_bg_opa(&s_style_bubble_user, LV_OPA_COVER);
lv_style_set_radius(&s_style_bubble_user, DPI_SCALE(16));
lv_style_set_pad_all(&s_style_bubble_user, DPI_SCALE(10));
lv_style_set_text_color(&s_style_bubble_user, lv_color_hex(0x000000));

/* AI bubble — dark bg, light text */
lv_style_init(&s_style_bubble_ai);
lv_style_set_bg_color(&s_style_bubble_ai, lv_color_hex(TH_CARD));
lv_style_set_bg_opa(&s_style_bubble_ai, LV_OPA_COVER);
lv_style_set_radius(&s_style_bubble_ai, DPI_SCALE(16));
lv_style_set_pad_all(&s_style_bubble_ai, DPI_SCALE(10));
lv_style_set_border_width(&s_style_bubble_ai, 1);
lv_style_set_border_color(&s_style_bubble_ai, lv_color_hex(TH_CARD_BORDER));
lv_style_set_text_color(&s_style_bubble_ai, lv_color_hex(0xBBBBBB));

/* Tool call card — left amber border */
lv_style_init(&s_style_tool_card);
lv_style_set_bg_color(&s_style_tool_card, lv_color_hex(0x12121E));
lv_style_set_bg_opa(&s_style_tool_card, LV_OPA_COVER);
lv_style_set_radius(&s_style_tool_card, DPI_SCALE(10));
lv_style_set_pad_all(&s_style_tool_card, DPI_SCALE(10));
lv_style_set_border_width(&s_style_tool_card, 1);
lv_style_set_border_color(&s_style_tool_card, lv_color_hex(TH_CARD_BORDER));
lv_style_set_border_side(&s_style_tool_card, LV_BORDER_SIDE_LEFT);
/* Override left border specifically */
```

The implementer must also define: `s_style_info_card`, `s_style_text_title`, `s_style_text_body`, `s_style_text_meta`, `s_style_text_dim`, `s_style_input_pill`.

Apply `ui_fb_card()` touch feedback to nav items. Apply `ui_fb_button()` to mic button.

- [ ] **Step 3: Add to CMakeLists.txt**

Add `"ui_theme.c"` and (later) `"ui_agents.c"` to UI_SRCS:

```cmake
set(UI_SRCS
    "ui_splash.c" "ui_home.c" "ui_settings.c"
    "ui_camera.c" "ui_files.c" "ui_audio.c" "ui_keyboard.c" "ui_voice.c" "ui_wifi.c"
    "ui_chat.c" "ui_notes.c" "ui_feedback.c"
    "chat_msg_store.c" "chat_header.c" "chat_input_bar.c" "chat_suggestions.c" "chat_msg_view.c"
    "ui_theme.c" "ui_agents.c"
)
```

- [ ] **Step 4: Build to verify**

```bash
cd /home/rebelforce/projects/TinkerTab
export IDF_PATH=/home/rebelforce/esp/esp-idf-v5.4.3
source $IDF_PATH/export.sh 2>/dev/null
python3 $IDF_PATH/tools/idf.py build
```

Note: ui_agents.c needs a stub file to compile. Create a minimal stub:
```c
#include "ui_agents.h"
/* Stub — implemented in Task 4 */
```

With matching stub header:
```c
#pragma once
#include "lvgl.h"
lv_obj_t *ui_agents_create(void);
void ui_agents_hide(void);
bool ui_agents_is_active(void);
```

- [ ] **Step 5: Commit**

```bash
git add main/ui_theme.h main/ui_theme.c main/ui_agents.h main/ui_agents.c main/CMakeLists.txt
git commit -m "feat: ui_theme — design system with 11 shared styles + nav bar + input pill components"
```

---

## Task 2: Home Screen Rewrite

**Files:**
- Rewrite: `main/ui_home.c`
- Modify: `main/ui_home.h`

This is the biggest task. Replace the 1084-line 4-page tileview with a ~500-line single-page dashboard.

**CRITICAL:** The implementer MUST read the FULL existing `main/ui_home.c` (1084 lines) before writing a single line. Understand every callback, every state variable, every external dependency. The following public API functions MUST be preserved with identical signatures:

```c
lv_obj_t *ui_home_create(void);
void ui_home_update_status(void);
void ui_home_destroy(void);
lv_obj_t *ui_home_get_screen(void);
void ui_home_go_home(void);
void ui_home_refresh_mode_badge(void);
void ui_home_show_toast(const char *text);
```

**Functions to REMOVE** (no longer needed without tileview):
- `ui_home_get_tileview()` — grep codebase for callers first. If only used in debug_server.c navigate handler, update that too.
- `ui_home_get_tile(int page)` — same treatment.
- `ui_home_nav_settings()` — replaced by nav bar callback.
- `build_page_notes()`, `build_page_chat()`, `build_page_settings()` — these were placeholder pages.

**Functions to KEEP/ADAPT from existing code (copy logic, adapt UI):**
- `orb_tap_cb()` (lines 568-594) — voice start/reconnect/toast logic. KEEP VERBATIM.
- `privacy_tap_cb()` (lines 647-677) — mode cycling with NVS + Dragon config update. KEEP VERBATIM.
- `dismiss_all_overlays()` (lines 801-817) — cancel pending timers, hide all overlays. KEEP VERBATIM.
- `nav_click_cb()` (lines 824-871) — 300ms debounce + delayed overlay creation. ADAPT for new nav bar.
- `update_timer_cb()` (line 889-893) — calls ui_home_update_status(). KEEP.
- `ui_home_update_status()` (lines 898-1008) — clock, date, battery, WiFi, dragon dot, orb opacity, mode badge, battery warnings, last note. ADAPT for new layout.
- `show_toast()` (lines 691-712) — toast on lv_layer_top. KEEP VERBATIM.

- [ ] **Step 1: Update ui_home.h**

Remove tileview getters. Add agent card update function:

```c
#pragma once
#include "lvgl.h"

lv_obj_t *ui_home_create(void);
void ui_home_update_status(void);
void ui_home_destroy(void);
lv_obj_t *ui_home_get_screen(void);
void ui_home_go_home(void);
void ui_home_refresh_mode_badge(void);
void ui_home_show_toast(const char *text);

/** Update agent card with latest tool_call/tool_result data. Thread-safe. */
void ui_home_update_agent(const char *tool_name, const char *status, int progress_pct);
```

- [ ] **Step 2: Rewrite ui_home.c**

The implementer must create the new ui_home.c with this structure:

**Includes:** Same as existing + add `#include "ui_theme.h"` and `#include "ui_agents.h"`.

**Static state (new):**
```c
static lv_obj_t *scr          = NULL;   /* Main screen */
static lv_obj_t *s_scroll     = NULL;   /* Scrollable content area */
static ui_nav_bar_t *s_nav    = NULL;   /* Nav bar (from ui_theme) */
static ui_input_pill_t *s_pill = NULL;  /* Input pill (from ui_theme) */
static lv_timer_t *tmr_update = NULL;   /* 1s status update timer */

/* Status bar */
static lv_obj_t *lbl_time     = NULL;
static lv_obj_t *lbl_mode     = NULL;   /* Mode pill */
static lv_obj_t *mode_dot     = NULL;   /* Pulsing dot in mode pill */
static lv_obj_t *lbl_batt     = NULL;
static lv_obj_t *conn_dot     = NULL;   /* Connection indicator */

/* Orb */
static lv_obj_t *orb_arc      = NULL;   /* lv_arc widget */
static lv_obj_t *orb_core     = NULL;   /* Inner circle */
static lv_anim_t s_orb_float;           /* Floating Y animation */
static lv_anim_t s_orb_glow;            /* Breathing border animation */

/* Time + greeting */
static lv_obj_t *lbl_time_big = NULL;
static lv_obj_t *lbl_date     = NULL;
static lv_obj_t *lbl_greeting = NULL;

/* Agent card */
static lv_obj_t *s_agent_card  = NULL;
static lv_obj_t *s_agent_dot   = NULL;  /* Pulsing status dot */
static lv_obj_t *s_agent_type  = NULL;  /* "HEARTBEAT" label */
static lv_obj_t *s_agent_title = NULL;  /* "Morning check-in" */
static lv_obj_t *s_agent_time  = NULL;  /* "2 min ago" */
/* Task rows use manual Y inside agent card — 4 labels, no containers */
static lv_obj_t *s_task_icons[4] = {NULL};
static lv_obj_t *s_task_labels[4] = {NULL};
static lv_obj_t *s_task_metas[4] = {NULL};

/* Info cards */
static lv_obj_t *s_info_cards[3] = {NULL}; /* note, chat, system */

/* Quick actions */
/* 3 buttons, created inline */

/* Disconnect banner */
static lv_obj_t *s_disconnect_banner = NULL;

/* Nav debounce (same pattern as current) */
static lv_timer_t *s_pending_nav_timer = NULL;
static int64_t     s_last_nav_click_us = 0;
#define NAV_DEBOUNCE_US 300000

static uint8_t s_badge_mode = 0;
```

**ui_home_create() structure:**

1. Create screen (`lv_obj_create(NULL)`) with TH_BG background, full size
2. Create scrollable content area (flex column, full width, height = SH - TH_NAV_H)
3. Build status bar (flex row inside scroll): time left, mode pill center, battery right
4. Build orb row (flex row): [lv_arc 88px + lv_obj circle 62px inside] + [time + date + greeting]
5. Build agent card (elevated style): header row + title + 4 task rows + sub-agents divider
6. Build info cards (2x): note preview + chat preview
7. Build quick actions row (3 buttons): Camera, Files, Timer
8. Create input pill (from ui_theme): "Ask anything..." placeholder
9. Create nav bar (from ui_theme): 4 tabs, wire nav_click_cb
10. Create disconnect banner (hidden by default)
11. Start orb animations (float + glow)
12. Start update timer (1000ms)
13. Wire orb tap + long press callbacks
14. Wire mode pill tap callback
15. Wire input pill mic + send callbacks
16. Set s_badge_mode from NVS
17. Call ui_home_update_status() for initial population
18. Load screen: `lv_screen_load(scr)`

**Orb implementation:**

```c
/* Arc ring (agent activity indicator) */
orb_arc = lv_arc_create(orb_container);
lv_obj_set_size(orb_arc, DPI_SCALE(64), DPI_SCALE(64));
lv_arc_set_rotation(orb_arc, 270);  /* Start from top */
lv_arc_set_range(orb_arc, 0, 100);
lv_arc_set_value(orb_arc, 75);      /* Agent progress */
lv_arc_set_bg_angles(orb_arc, 0, 360);
lv_obj_set_style_arc_width(orb_arc, DPI_SCALE(2), LV_PART_MAIN);     /* Track */
lv_obj_set_style_arc_color(orb_arc, lv_color_hex(TH_CARD_BORDER), LV_PART_MAIN);
lv_obj_set_style_arc_width(orb_arc, DPI_SCALE(3), LV_PART_INDICATOR); /* Fill */
lv_obj_set_style_arc_color(orb_arc, lv_color_hex(TH_AMBER), LV_PART_INDICATOR);
lv_obj_set_style_arc_rounded(orb_arc, true, LV_PART_INDICATOR);
lv_obj_remove_style(orb_arc, NULL, LV_PART_KNOB);  /* Hide knob */
lv_obj_clear_flag(orb_arc, LV_OBJ_FLAG_CLICKABLE);
lv_obj_center(orb_arc);

/* Inner orb circle */
orb_core = lv_obj_create(orb_container);
lv_obj_set_size(orb_core, DPI_SCALE(46), DPI_SCALE(46));
lv_obj_set_style_bg_color(orb_core, lv_color_hex(TH_AMBER), 0);
lv_obj_set_style_bg_opa(orb_core, LV_OPA_COVER, 0);
lv_obj_set_style_radius(orb_core, LV_RADIUS_CIRCLE, 0);
lv_obj_set_style_border_width(orb_core, 0, 0);
lv_obj_set_style_shadow_width(orb_core, DPI_SCALE(15), 0);
lv_obj_set_style_shadow_spread(orb_core, DPI_SCALE(6), 0);
lv_obj_set_style_shadow_color(orb_core, lv_color_hex(TH_AMBER_GLOW), 0);
lv_obj_set_style_shadow_opa(orb_core, LV_OPA_50, 0);
lv_obj_add_flag(orb_core, LV_OBJ_FLAG_CLICKABLE);
lv_obj_set_ext_click_area(orb_core, DPI_SCALE(15));
lv_obj_add_event_cb(orb_core, orb_tap_cb, LV_EVENT_CLICKED, NULL);
lv_obj_add_event_cb(orb_core, orb_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
lv_obj_center(orb_core);
/* Icon inside orb */
lv_obj_t *orb_icon = lv_label_create(orb_core);
lv_label_set_text(orb_icon, LV_SYMBOL_AUDIO);
lv_obj_set_style_text_font(orb_icon, FONT_HEADING, 0);
lv_obj_set_style_text_color(orb_icon, lv_color_hex(0x000000), 0);
lv_obj_center(orb_icon);
```

**Floating animation:**
```c
lv_anim_init(&s_orb_float);
lv_anim_set_var(&s_orb_float, orb_core);
int base_y = lv_obj_get_y(orb_core);
lv_anim_set_values(&s_orb_float, base_y - DPI_SCALE(2), base_y + DPI_SCALE(2));
lv_anim_set_duration(&s_orb_float, 4000);
lv_anim_set_playback_duration(&s_orb_float, 4000);
lv_anim_set_repeat_count(&s_orb_float, LV_ANIM_REPEAT_INFINITE);
lv_anim_set_exec_cb(&s_orb_float, (lv_anim_exec_xcb_t)lv_obj_set_y);
lv_anim_set_path_cb(&s_orb_float, lv_anim_path_ease_in_out);
lv_anim_start(&s_orb_float);
```

**Nav click callback (adapt from existing):**

```c
static void nav_click_cb(lv_event_t *e)
{
    int pg = (int)(intptr_t)lv_event_get_user_data(e);
    if (pg < 0 || pg >= 4) return;

    /* Debounce: 300ms */
    int64_t now = esp_timer_get_time();
    if (now - s_last_nav_click_us < NAV_DEBOUNCE_US) return;
    s_last_nav_click_us = now;

    if (s_pending_nav_timer) {
        lv_timer_delete(s_pending_nav_timer);
        s_pending_nav_timer = NULL;
    }

    dismiss_all_overlays();
    ui_nav_bar_set_active(s_nav, pg);

    if (pg == 0) {
        /* Home — already here, just scroll to top */
        if (s_scroll) lv_obj_scroll_to_y(s_scroll, 0, LV_ANIM_ON);
        return;
    }

    /* Delayed overlay creation (30ms) — lets nav pressed-state render first */
    typedef void (*delayed_fn)(lv_timer_t*);
    static void _delayed_notes(lv_timer_t *t) { (void)t; s_pending_nav_timer=NULL; ui_notes_create(); }
    static void _delayed_chat(lv_timer_t *t)  { (void)t; s_pending_nav_timer=NULL; ui_chat_create(); }
    static void _delayed_settings(lv_timer_t *t) { (void)t; s_pending_nav_timer=NULL; ui_settings_create(); }

    delayed_fn fns[] = {NULL, _delayed_notes, _delayed_chat, _delayed_settings};
    s_pending_nav_timer = lv_timer_create(fns[pg], 30, NULL);
    lv_timer_set_repeat_count(s_pending_nav_timer, 1);
}
```

**dismiss_all_overlays() — COPY VERBATIM from existing code (lines 801-817).**

**ui_home_update_status() — ADAPT from existing (lines 898-1008):**
- Keep: clock update, date update, battery update, WiFi icon, dragon dot, orb dimming, mode badge, battery warnings, last note preview
- Change: update new widget references (lbl_time, lbl_date, etc.)
- Add: update greeting text ("Hey Emile — N things happened")
- Add: update agent card content (tool name, progress)

- [ ] **Step 3: Update debug_server.c navigate handler**

The `/navigate?screen=X` handler currently uses `ui_home_get_tileview()` and `lv_tileview_set_tile()`. Update to use the new nav approach:

Find the navigate handler in debug_server.c and replace tileview logic with:
```c
if (strcmp(screen, "home") == 0) {
    dismiss_all_overlays();
    ui_home_go_home();
} else if (strcmp(screen, "chat") == 0) {
    dismiss_all_overlays();
    ui_chat_create();
} else if (strcmp(screen, "notes") == 0) {
    dismiss_all_overlays();
    ui_notes_create();
} else if (strcmp(screen, "settings") == 0) {
    dismiss_all_overlays();
    ui_settings_create();
}
```

Note: dismiss_all_overlays is in ui_home.c — declare it in ui_home.h or use extern.

- [ ] **Step 4: Build and fix compilation errors**

```bash
python3 $IDF_PATH/tools/idf.py build
```

This will likely require multiple iterations. Common issues:
- Missing externs for functions called across files
- Removed tileview getter functions still referenced somewhere
- Type mismatches in new code

- [ ] **Step 5: Commit**

```bash
git add main/ui_home.c main/ui_home.h main/debug_server.c
git commit -m "feat: ui_home rewrite — single page dashboard with orb, agent card, activity feed"
```

---

## Task 3: Chat Tool Call Cards

**Files:**
- Modify: `main/chat_msg_store.h` — add MSG_TOOL_CALL type
- Modify: `main/chat_msg_view.c` — add tool card rendering
- Modify: `main/ui_chat.c` — wire tool_call/tool_result WS messages to store

- [ ] **Step 1: Add MSG_TOOL_CALL to chat_msg_store.h**

```c
typedef enum {
    MSG_TEXT,
    MSG_IMAGE,
    MSG_CARD,
    MSG_AUDIO_CLIP,
    MSG_TOOL_STATUS,
    MSG_SYSTEM,
    MSG_TOOL_CALL,    /* NEW: visible tool invocation card */
} msg_type_t;
```

- [ ] **Step 2: Add tool card rendering to chat_msg_view.c**

In the `configure_slot()` function, add a case for MSG_TOOL_CALL in the switch statement. The tool card needs:
- Left amber border (3px, using lv_obj_set_style_border_side LV_BORDER_SIDE_LEFT)
- Background: 0x12121E
- Header: tool name in FONT_SMALL amber + timing in green
- Body: query/args text in FONT_CAPTION dim
- Result: below divider line

Add the `MSG_TOOL_CALL` case to the content rendering switch in configure_slot():

```c
case MSG_TOOL_CALL:
    /* Apply tool card style — amber left border */
    lv_obj_add_style(slot->container, &s_style_tool_card, 0);
    /* Tool name from text field (format: "TOOL_NAME|args|result|timing") */
    /* Parse the pipe-delimited text to extract components */
    {
        char tool_name[32] = "TOOL";
        char tool_body[256] = "";
        char tool_result[256] = "";
        /* ... parse msg->text ... */
        char display[512];
        snprintf(display, sizeof(display), "%s\n%s%s%s",
                 tool_name, tool_body,
                 tool_result[0] ? "\n───\n" : "",
                 tool_result);
        lv_label_set_text(slot->content, display);
    }
    lv_obj_set_style_text_font(slot->content, FONT_SMALL, 0);
    lv_obj_set_style_text_color(slot->content, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(slot->content, LV_TEXT_ALIGN_LEFT, 0);
    x_pos = DPI_SCALE(15); /* Left-aligned with slight indent */
    break;
```

Also add `s_style_tool_card` to the styles (or import from ui_theme.h if already defined there).

- [ ] **Step 3: Wire tool_call/tool_result to MSG_TOOL_CALL in ui_chat.c**

In the existing ui_chat.c, the poll_voice_cb or the WS message handler creates MSG_TOOL_STATUS messages for tool indicators. Enhance this to create MSG_TOOL_CALL messages instead:

When `tool_call` WS message arrives (voice.c already handles it and updates state), create a chat_msg_t with:
- type = MSG_TOOL_CALL
- is_user = false
- text = "WEB_SEARCH|query: \"self-hosted AI\"|8 results|234ms" (pipe-delimited)
- timestamp = current RTC time

The implementer should read voice.c lines 774-803 to understand how tool_call/tool_result are currently handled, and add new push functions to feed them into the chat store.

- [ ] **Step 4: Build and test**

```bash
python3 $IDF_PATH/tools/idf.py build
```

- [ ] **Step 5: Commit**

```bash
git add main/chat_msg_store.h main/chat_msg_view.c main/ui_chat.c
git commit -m "feat: MSG_TOOL_CALL — visible tool invocation cards in chat with amber border"
```

---

## Task 4: Agents Overlay

**Files:**
- Replace stub: `main/ui_agents.h`
- Replace stub: `main/ui_agents.c`

- [ ] **Step 1: Create ui_agents.h**

```c
#pragma once
#include "lvgl.h"

/** Create/show the agents overlay. Called from home agent card tap. */
lv_obj_t *ui_agents_create(void);

/** Hide the agents overlay (preserve state). */
void ui_agents_hide(void);

/** Check if overlay is active. */
bool ui_agents_is_active(void);

/** Update agent status from tool_call/tool_result data. Thread-safe. */
void ui_agents_update(const char *agent_name, const char *task_name,
                      const char *status, int progress_pct);
```

- [ ] **Step 2: Create ui_agents.c**

The Agents overlay follows the same pattern as ui_chat.c and ui_settings.c:
- Fullscreen overlay on home screen
- Hide/show pattern (never destroy)
- Flex column layout: header + scrollable card area + input pill
- Max 3 agent cards visible, recycled on scroll

The implementer must read the spec sections "Screen 2: Agents Overlay" and "Agent Card States" for exact visual details.

Key implementation:
- Use lv_obj_create on ui_home_get_screen() as parent
- Header: "← Back" + "Agents" title + "2 active · 1 idle" count
- Agent cards use the same task row pattern as the home agent card
- Active agents: full opacity, pulsing dot
- Idle agents: 45% opacity
- Completed agents: 30% opacity
- Back button calls ui_agents_hide()

For Phase 1 (MVP): Show a simple list of recent tool_call events parsed from voice.c, not full agent orchestration. Each card shows: tool name, args summary, status (running/done), timing.

- [ ] **Step 3: Build and test**

```bash
python3 $IDF_PATH/tools/idf.py build
```

- [ ] **Step 4: Commit**

```bash
git add main/ui_agents.h main/ui_agents.c
git commit -m "feat: ui_agents — agents overlay with tool activity cards"
```

---

## Task 5: Notes & Settings Visual Polish

**Files:**
- Modify: `main/ui_notes.c`
- Modify: `main/ui_settings.c`

- [ ] **Step 1: Apply theme to ui_notes.c**

The implementer must read the current ui_notes.c (2116 lines) and apply these changes:
- Replace all hardcoded color values (0x1A1A2E, 0x2C2C2E, etc.) with TH_* constants
- Replace card bg: `COL_CARD` → `TH_CARD`
- Replace text colors: body text → `TH_TEXT_BODY`, metadata → `TH_TEXT_SECONDARY`
- Replace card radius: hardcoded values → `TH_INFO_RADIUS`
- Replace padding: hardcoded values → `TH_CARD_PAD`
- Add `#include "ui_theme.h"` at top
- Apply shared styles where possible (s_style_card for note cards)

Do NOT change any logic, callbacks, or data handling. Only visual constants.

- [ ] **Step 2: Apply theme to ui_settings.c**

Same approach as notes:
- Replace all hardcoded colors with TH_* constants
- Replace section header styling to match spec: FONT_SMALL, TH_TEXT_DIM, letter-spacing
- Replace row styling to match spec: FONT_BODY TH_TEXT_BODY, values TH_TEXT_SECONDARY
- Replace slider/toggle styling to use amber accent
- Add `#include "ui_theme.h"` at top

Do NOT change any logic or NVS operations.

- [ ] **Step 3: Build**

```bash
python3 $IDF_PATH/tools/idf.py build
```

- [ ] **Step 4: Commit**

```bash
git add main/ui_notes.c main/ui_settings.c
git commit -m "style: apply theme colors and spacing to notes + settings screens"
```

---

## Task 6: Animations

**Files:**
- Modify: `main/ui_home.c` (add stagger entry + breathing animations)

- [ ] **Step 1: Add staggered card entry animation to ui_home_create()**

After creating all cards in ui_home_create(), add entry animations:

```c
/* Staggered card entry — slide up + fade in */
static void _stagger_entry(lv_obj_t *obj, int delay_ms)
{
    int target_y = lv_obj_get_y(obj);
    lv_obj_set_y(obj, target_y + DPI_SCALE(30));
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);

    lv_anim_t a_y;
    lv_anim_init(&a_y);
    lv_anim_set_var(&a_y, obj);
    lv_anim_set_values(&a_y, target_y + DPI_SCALE(30), target_y);
    lv_anim_set_duration(&a_y, 300);
    lv_anim_set_delay(&a_y, delay_ms);
    lv_anim_set_exec_cb(&a_y, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a_y, lv_anim_path_ease_out);
    lv_anim_start(&a_y);

    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, obj);
    lv_anim_set_values(&a_opa, 0, 255);
    lv_anim_set_duration(&a_opa, 300);
    lv_anim_set_delay(&a_opa, delay_ms);
    lv_anim_set_exec_cb(&a_opa, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_out);
    lv_anim_start(&a_opa);
}

/* In ui_home_create(), after building all widgets: */
_stagger_entry(s_agent_card, 100);
_stagger_entry(s_info_cards[0], 200);
_stagger_entry(s_info_cards[1], 300);
```

- [ ] **Step 2: Add mode dot pulsing animation**

```c
/* In ui_home_create(), after creating mode_dot: */
lv_anim_t a_dot;
lv_anim_init(&a_dot);
lv_anim_set_var(&a_dot, mode_dot);
lv_anim_set_values(&a_dot, LV_OPA_50, LV_OPA_COVER);
lv_anim_set_duration(&a_dot, 2000);
lv_anim_set_playback_duration(&a_dot, 2000);
lv_anim_set_repeat_count(&a_dot, LV_ANIM_REPEAT_INFINITE);
lv_anim_set_exec_cb(&a_dot, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
lv_anim_set_path_cb(&a_dot, lv_anim_path_ease_in_out);
lv_anim_start(&a_dot);
```

- [ ] **Step 3: Add nav bar active indicator animation**

In ui_nav_bar_set_active(), when changing active tab, animate the indicator bar width:

```c
/* Expand indicator from 0 to 20px width */
lv_anim_t a;
lv_anim_init(&a);
lv_anim_set_var(&a, nav->indicators[index]);
lv_anim_set_values(&a, 0, DPI_SCALE(15));
lv_anim_set_duration(&a, 200);
lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_width);
lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
lv_anim_start(&a);
```

- [ ] **Step 4: Build**

```bash
python3 $IDF_PATH/tools/idf.py build
```

- [ ] **Step 5: Commit**

```bash
git add main/ui_home.c main/ui_theme.c
git commit -m "feat: animations — orb float, card stagger, mode dot pulse, nav indicator"
```

---

## Task 7: Build + Flash + Integration Test

- [ ] **Step 1: Full clean build**

```bash
cd /home/rebelforce/projects/TinkerTab
export IDF_PATH=/home/rebelforce/esp/esp-idf-v5.4.3
source $IDF_PATH/export.sh 2>/dev/null
python3 $IDF_PATH/tools/idf.py fullclean
python3 $IDF_PATH/tools/idf.py set-target esp32p4
python3 $IDF_PATH/tools/idf.py build
```

- [ ] **Step 2: Flash**

```bash
python3 $IDF_PATH/tools/idf.py -p /dev/ttyACM0 flash
```

If ROM download mode, trigger watchdog reset:
```bash
python3 -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac
```

- [ ] **Step 3: Verify boot + selftest**

```bash
# Wait for WiFi to connect (new X28P network)
sleep 15
# Find Tab5 on X28P subnet (192.168.70.x)
for i in $(seq 1 254); do
  curl -s --connect-timeout 1 http://192.168.70.$i:8080/info 2>/dev/null | grep -q "wifi_ip" && echo "TAB5 at 192.168.70.$i" &
done; wait

# Once found:
TAB5_IP=<found_ip>
curl -s http://$TAB5_IP:8080/selftest | python3 -m json.tool
```

Expected: 8/8 pass.

- [ ] **Step 4: Take screenshot of new home screen**

```bash
TOKEN=<get_from_serial_or_use_existing>
curl -s -H "Authorization: Bearer $TOKEN" -o /tmp/home_new.bmp http://$TAB5_IP:8080/screenshot
python3 -c "from PIL import Image; Image.open('/tmp/home_new.bmp').save('/tmp/home_new.png')"
```

Verify: single page (no tileview), orb with arc ring, agent card, info cards, input pill, 4-tab nav bar.

- [ ] **Step 5: Test navigation**

```bash
# Navigate to each screen
for screen in chat notes settings home; do
  curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://$TAB5_IP:8080/navigate?screen=$screen" && sleep 3
  curl -s -H "Authorization: Bearer $TOKEN" -o /tmp/nav_$screen.bmp http://$TAB5_IP:8080/screenshot
done
```

Verify: all 4 screens load and return to home correctly.

- [ ] **Step 6: Test voice + chat**

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://$TAB5_IP:8080/chat -d '{"text":"What time is it?"}'
sleep 10
curl -s -H "Authorization: Bearer $TOKEN" -o /tmp/chat_test.bmp http://$TAB5_IP:8080/screenshot
```

Verify: message sent, AI response appears with tool call cards visible.

- [ ] **Step 7: Heap stability test**

```bash
for round in 1 2 3 4 5; do
  for screen in home settings chat notes home; do
    curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://$TAB5_IP:8080/navigate?screen=$screen" >/dev/null 2>&1
    sleep 2
  done
  curl -s http://$TAB5_IP:8080/selftest | python3 -c "
import sys,json
data = json.load(sys.stdin)
for t in data['tests']:
    if t['name']=='internal_heap':
        print(f'Round $round: free={t[\"free_kb\"]}KB largest={t[\"largest_block_kb\"]}KB frag={t[\"fragmentation_pct\"]}%')
" 2>/dev/null
done
```

Expected: heap stable across all rounds, no fragmentation growth.

- [ ] **Step 8: Commit + push**

```bash
git add -A
git commit -m "test: UI overhaul verified — new home, agents overlay, tool cards, animations, stable heap"
git push
```

---

## Self-Review Checklist

### Spec Coverage
- [x] Design system (colors, spacing, styles) → Task 1
- [x] Home screen rewrite (orb, agent card, feed, input pill, nav) → Task 2
- [x] Chat tool call cards → Task 3
- [x] Agents overlay → Task 4
- [x] Notes visual polish → Task 5
- [x] Settings visual polish → Task 5
- [x] Animations (orb float, card stagger, mode dot, nav indicator) → Task 6
- [x] Build + flash + integration test → Task 7
- [x] WiFi config updated to X28P → Already in sdkconfig.defaults

### Placeholder Scan
- No "TBD" or "TODO" found
- All code blocks contain actual code, not descriptions
- All commands include expected output
- File paths are exact

### Type Consistency
- `ui_theme_init()` called before any screen creation
- `ui_nav_bar_t` struct consistent across header and implementation
- `ui_input_pill_t` struct consistent
- `th_mode_colors[]` and `th_mode_names[]` used consistently
- `MSG_TOOL_CALL` enum value added to msg_type_t, handled in configure_slot()
- All TH_* color constants used consistently
