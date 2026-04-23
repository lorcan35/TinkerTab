/*
 * ui_nav_sheet.c -- v4·D Sovereign Halo nav sheet.
 *
 * The spec (d-sovereign-halo.html) calls for "menu hides inside 4-dot chip".
 * Before this file, tapping the 4-dot chip jumped straight to the Chat
 * overlay -- leaving the user with no in-UI path to Settings, Notes,
 * Camera, Files, Memory.  This sheet replaces that with a 2x3 tile grid
 * covering every destination the home screen doesn't host inline.
 */
#include "ui_nav_sheet.h"
#include "ui_core.h"
#include "ui_home.h"
#include "ui_chat.h"
#include "ui_notes.h"
#include "ui_settings.h"
#include "ui_camera.h"
#include "ui_files.h"
#include "ui_memory.h"
#include "ui_focus.h"
#include "ui_agents.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>

#ifndef FONT_BODY
#  define FONT_BODY         LV_FONT_DEFAULT
#endif
#ifndef FONT_CAPTION
#  define FONT_CAPTION      LV_FONT_DEFAULT
#endif
#ifndef FONT_TITLE
#  define FONT_TITLE        LV_FONT_DEFAULT
#endif

static const char *TAG = "ui_nav_sheet";

#define SHEET_BG        0x0C0C14
#define SHEET_TILE      0x13131F
#define SHEET_BORDER    0x1E1E2A
#define SHEET_AMBER     0xF59E0B
#define SHEET_TEXT1     0xF5F5F7
#define SHEET_TEXT2     0x8A8A95

static lv_obj_t *s_sheet   = NULL;
static lv_obj_t *s_scrim   = NULL;
static bool      s_visible = false;

typedef struct {
    const char *title;
    const char *sub;
    void (*action)(void);
} nav_tile_t;

static void go_chat(void)     { extern lv_obj_t *ui_chat_create(void);  ui_chat_create();  }
static void go_notes(void)    { extern lv_obj_t *ui_notes_create(void); ui_notes_create(); }
static void go_settings(void) { extern lv_obj_t *ui_settings_create(void); ui_settings_create(); }
static void go_camera(void)   { extern lv_obj_t *ui_camera_create(void); ui_camera_create(); }
static void go_files(void)    { extern lv_obj_t *ui_files_create(void);  ui_files_create();  }
static void go_memory(void)   { extern void ui_memory_show(void);        ui_memory_show();   }

static const nav_tile_t s_tiles[] = {
    { "Chat",     "Threads \xE2\x80\xA2 history",  go_chat     },
    { "Notes",    "Voice & text",                  go_notes    },
    { "Settings", "Mode \xE2\x80\xA2 cap \xE2\x80\xA2 WiFi",  go_settings },
    { "Camera",   "Viewfinder",                    go_camera   },
    { "Files",    "SD card",                       go_files    },
    { "Memory",   "Facts \xE2\x80\xA2 recall",      go_memory   },
};
#define NAV_TILE_COUNT (sizeof(s_tiles) / sizeof(s_tiles[0]))

static void scrim_click_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_sheet_hide();
}

static void tile_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)NAV_TILE_COUNT) return;
    ESP_LOGI(TAG, "tile %d (%s) tapped", idx, s_tiles[idx].title);
    ui_nav_sheet_hide();
    /* Dismiss stale idle voice overlay so the user actually sees where
     * they're navigating to. */
    extern void ui_voice_dismiss_if_idle(void);
    ui_voice_dismiss_if_idle();
    /* #167: same reason as debug_server.c async_navigate — camera and
     * files own PSRAM canvas buffers and full LVGL screen trees; if the
     * user leaves them by tapping any OTHER tile, we have to destroy
     * explicitly or ~1.8 MB / screen tree / preview timer leak per
     * round-trip.  Both functions are NULL-guarded. */
    extern void ui_camera_destroy(void);
    extern void ui_files_destroy(void);
    ui_camera_destroy();
    ui_files_destroy();
    if (s_tiles[idx].action) s_tiles[idx].action();
}

static void close_btn_click_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_sheet_hide();
}

static void build_sheet(void)
{
    lv_obj_t *top = lv_layer_top();

    /* Scrim behind the sheet.  Full-screen, translucent, clickable to
     * dismiss.  Opa 70% keeps the home orb visible behind the sheet so
     * the transition feels contextual rather than total. */
    s_scrim = lv_obj_create(top);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, 720, 1280);
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_70, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, scrim_click_cb, LV_EVENT_CLICKED, NULL);

    /* Sheet: bottom 70% of the display.  Rounded top, flat bottom. */
    const int SHEET_H = 820;
    s_sheet = lv_obj_create(top);
    lv_obj_remove_style_all(s_sheet);
    lv_obj_set_size(s_sheet, 720, SHEET_H);
    lv_obj_set_pos(s_sheet, 0, 1280 - SHEET_H);
    lv_obj_set_style_bg_color(s_sheet, lv_color_hex(SHEET_BG), 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_sheet, 28, 0);
    lv_obj_set_style_border_width(s_sheet, 1, 0);
    lv_obj_set_style_border_color(s_sheet, lv_color_hex(SHEET_BORDER), 0);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);

    /* Drag handle (visual only) */
    lv_obj_t *grip = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(grip);
    lv_obj_set_size(grip, 56, 5);
    lv_obj_set_pos(grip, (720 - 56) / 2, 14);
    lv_obj_set_style_bg_color(grip, lv_color_hex(0x3A3A44), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(grip, 3, 0);

    /* Kicker + title */
    lv_obj_t *kicker = lv_label_create(s_sheet);
    lv_label_set_text(kicker, "\xE2\x80\xA2 MENU");
    lv_obj_set_style_text_font(kicker, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(kicker, lv_color_hex(SHEET_AMBER), 0);
    lv_obj_set_style_text_letter_space(kicker, 4, 0);
    lv_obj_set_pos(kicker, 40, 52);

    lv_obj_t *title = lv_label_create(s_sheet);
    lv_label_set_text(title, "Where to?");
    lv_obj_set_style_text_font(title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(SHEET_TEXT1), 0);
    lv_obj_set_pos(title, 40, 86);

    /* Close button top-right */
    lv_obj_t *close_btn = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(close_btn);
    lv_obj_set_size(close_btn, 52, 52);
    lv_obj_set_pos(close_btn, 720 - 52 - 40, 80);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(SHEET_TILE), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(close_btn, 26, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(SHEET_BORDER), 0);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(close_btn, close_btn_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(close_btn);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_hex(SHEET_TEXT2), 0);
    lv_obj_center(xl);

    /* 2x3 grid of tiles.  Tile = 304 x 184, gap = 16.
     * Grid starts at x=40, y=168. */
    const int TW = 304, TH = 184, GAP = 16;
    const int GRID_X = 40, GRID_Y = 170;
    for (int i = 0; i < (int)NAV_TILE_COUNT; i++) {
        int col = i % 2;
        int row = i / 2;
        lv_obj_t *tile = lv_obj_create(s_sheet);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, TW, TH);
        lv_obj_set_pos(tile,
                       GRID_X + col * (TW + GAP),
                       GRID_Y + row * (TH + GAP));
        lv_obj_set_style_bg_color(tile, lv_color_hex(SHEET_TILE), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, 22, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(SHEET_BORDER), 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        /* Amber dot + title */
        lv_obj_t *dot = lv_obj_create(tile);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_pos(dot, 24, 28);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(SHEET_AMBER), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

        lv_obj_t *tl = lv_label_create(tile);
        lv_label_set_text(tl, s_tiles[i].title);
        lv_obj_set_style_text_font(tl, FONT_TITLE, 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(SHEET_TEXT1), 0);
        lv_obj_set_pos(tl, 24, 56);

        lv_obj_t *sub = lv_label_create(tile);
        lv_label_set_text(sub, s_tiles[i].sub);
        lv_obj_set_style_text_font(sub, FONT_CAPTION, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(SHEET_TEXT2), 0);
        lv_obj_set_style_text_letter_space(sub, 3, 0);
        lv_obj_set_pos(sub, 24, TH - 34);
    }
}

void ui_nav_sheet_show(void)
{
    if (s_visible) return;
    if (!s_sheet) build_sheet();
    if (!s_sheet) return;
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_scrim);
    lv_obj_move_foreground(s_sheet);
    s_visible = true;
    ESP_LOGI(TAG, "nav sheet shown");
}

void ui_nav_sheet_hide(void)
{
    if (!s_visible) return;
    if (s_scrim) lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    if (s_sheet) lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ESP_LOGI(TAG, "nav sheet hidden");
}

bool ui_nav_sheet_is_visible(void)
{
    return s_visible;
}
