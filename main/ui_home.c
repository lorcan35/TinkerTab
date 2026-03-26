/*
 * ui_home.c — Glyph OS home screen
 * 720x1280 portrait, LVGL v9
 *
 * Uses lv_tileview for vertical page-snap navigation:
 *   Page 0 — AI (brain glyph with breathing pulse, status)
 *   Page 1 — Tools (2-column live tiles with accent bars)
 *   Page 2 — Dragon (MJPEG viewport, connection status)
 *   Page 3 — Settings (hardware info rows)
 *
 * Floating status bar overlays on top.
 * Page indicator dots on right edge follow scroll position.
 * Amber primary (#FFB800), shifts to cyan (#00B4D8) in Dragon mode.
 */

#include "ui_home.h"
#include "battery.h"
#include "rtc.h"
#include "dragon_link.h"
#include "wifi.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_home";

/* ── Glyph Palette ───────────────────────────────────────────── */
#define COL_BG          0x0D0D0D
#define COL_SURFACE     0x1A1A1A
#define COL_SURFACE2    0x242424
#define COL_AMBER       0xFFB800
#define COL_AMBER_DIM   0x8A6400
#define COL_CYAN        0x00B4D8
#define COL_MINT        0x00E5A0
#define COL_CORAL       0xFF6B6B
#define COL_WHITE       0xE8E8E8
#define COL_DIM         0x666666
#define COL_GREEN       0x34D399
#define COL_RED         0xF87171

/* ── Layout (720 x 1280) ────────────────────────────────────── */
#define SW  720
#define SH  1280
#define STATUSBAR_H  48
#define PAGE_PAD     24
#define TILE_W       310
#define TILE_H       140
#define TILE_GAP     14
#define ROW_H        56
#define NUM_PAGES    4
#define PAGE_DOT_SZ  8
#define PAGE_DOT_GAP 18

/* ── Forward decls ───────────────────────────────────────────── */
static void update_timer_cb(lv_timer_t *t);
static void tileview_scroll_cb(lv_event_t *e);
static void brain_pulse_anim_cb(void *obj, int32_t val);
static void nav_click_cb(lv_event_t *e);

/* ── State ───────────────────────────────────────────────────── */
static lv_obj_t   *scr           = NULL;
static lv_obj_t   *tileview      = NULL;

/* Status bar */
static lv_obj_t   *lbl_time      = NULL;
static lv_obj_t   *lbl_mode      = NULL;
static lv_obj_t   *lbl_wifi      = NULL;
static lv_obj_t   *lbl_batt      = NULL;
static lv_obj_t   *lbl_dragon_dot = NULL;

/* AI page */
static lv_obj_t   *brain_ring    = NULL;
static lv_obj_t   *lbl_ai_state  = NULL;
static lv_obj_t   *lbl_ai_hint   = NULL;

/* Tools page */
static lv_obj_t   *lbl_tool_wifi  = NULL;
static lv_obj_t   *lbl_tool_batt  = NULL;
static lv_obj_t   *lbl_tool_heap  = NULL;
static lv_obj_t   *lbl_tool_dragon = NULL;
static lv_obj_t   *lbl_tool_fps   = NULL;
static lv_obj_t   *lbl_tool_rtc   = NULL;

/* Dragon page */
static lv_obj_t   *lbl_dragon_state = NULL;
static lv_obj_t   *lbl_dragon_info  = NULL;

/* Settings page */
static lv_obj_t   *lbl_set_wifi   = NULL;
static lv_obj_t   *lbl_set_dragon = NULL;
static lv_obj_t   *lbl_set_heap   = NULL;

/* Page indicator */
static lv_obj_t   *page_dots[NUM_PAGES] = {NULL};
static int         current_page = 0;

/* Bottom nav */
#define NAV_H        56
static lv_obj_t   *nav_icons[NUM_PAGES] = {NULL};

/* Animations */
static lv_anim_t   brain_anim;
static bool        brain_anim_running = false;

/* Tile objects for navigation */
static lv_obj_t   *tiles[NUM_PAGES] = {NULL};

static lv_timer_t *tmr_update     = NULL;

/* ── Helper: page title ──────────────────────────────────────── */
static void add_page_title(lv_obj_t *page, const char *title, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(page);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, PAGE_PAD, STATUSBAR_H + 16);

    /* Accent line */
    lv_obj_t *line = lv_obj_create(page);
    lv_obj_set_size(line, SW - PAGE_PAD * 2, 1);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, PAGE_PAD, STATUSBAR_H + 50);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_20, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* ── Helper: tool tile (with accent bar at top) ─────────────── */
static lv_obj_t *make_tool_tile(lv_obj_t *parent, int x, int y,
                                const char *icon, const char *label, uint32_t accent)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, TILE_W, TILE_H);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_SURFACE2), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 16, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    /* Accent bar — thin colored strip at top of tile */
    lv_obj_t *bar = lv_obj_create(tile);
    lv_obj_set_size(bar, TILE_W - 64, 3);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, -12);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(tile);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lv_color_hex(accent), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 36);

    lv_obj_t *val = lv_label_create(tile);
    lv_label_set_text(val, "---");
    lv_obj_set_style_text_color(val, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_18, 0);
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    return val;
}

/* ── Helper: settings row ────────────────────────────────────── */
static lv_obj_t *make_setting_row(lv_obj_t *parent, int y,
                                  const char *icon, const char *label, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, SW - PAGE_PAD * 2, ROW_H);
    lv_obj_set_pos(row, PAGE_PAD, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SURFACE2), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_l = lv_label_create(row);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s  %s", icon, label);
    lv_label_set_text(lbl_l, buf);
    lv_obj_set_style_text_color(lbl_l, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_l, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *lbl_v = lv_label_create(row);
    lv_label_set_text(lbl_v, value);
    lv_obj_set_style_text_color(lbl_v, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl_v, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, 0, 0);

    return lbl_v;
}

/* ================================================================
 * Build Glyph OS home screen
 * ================================================================ */
lv_obj_t *ui_home_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SW, SH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ══════════════════════════════════════════════════════════
     * TILEVIEW — LVGL's built-in snap-scroll page container
     * ══════════════════════════════════════════════════════════ */
    tileview = lv_tileview_create(scr);
    lv_obj_set_size(tileview, SW, SH);
    lv_obj_set_pos(tileview, 0, 0);
    lv_obj_set_style_bg_color(tileview, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);

    /* Page 0: AI — can swipe down */
    lv_obj_t *pg_ai = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_BOTTOM);
    /* Page 1: Tools — can swipe up and down */
    lv_obj_t *pg_tools = lv_tileview_add_tile(tileview, 0, 1, LV_DIR_TOP | LV_DIR_BOTTOM);
    /* Page 2: Dragon — can swipe up and down */
    lv_obj_t *pg_dragon = lv_tileview_add_tile(tileview, 0, 2, LV_DIR_TOP | LV_DIR_BOTTOM);
    /* Page 3: Settings — can swipe up only */
    lv_obj_t *pg_settings = lv_tileview_add_tile(tileview, 0, 3, LV_DIR_TOP);

    tiles[0] = pg_ai;
    tiles[1] = pg_tools;
    tiles[2] = pg_dragon;
    tiles[3] = pg_settings;

    /* ── PAGE 0: AI ──────────────────────────────────────────── */
    {
        add_page_title(pg_ai, "A I", COL_AMBER);

        /* Outer breathing ring — animated opacity */
        brain_ring = lv_obj_create(pg_ai);
        lv_obj_set_size(brain_ring, 170, 170);
        lv_obj_align(brain_ring, LV_ALIGN_CENTER, 0, -120);
        lv_obj_set_style_bg_opa(brain_ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(brain_ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(brain_ring, 1, 0);
        lv_obj_set_style_border_color(brain_ring, lv_color_hex(COL_AMBER), 0);
        lv_obj_set_style_border_opa(brain_ring, LV_OPA_20, 0);
        lv_obj_clear_flag(brain_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* Brain glyph circle — solid amber fill (Stitch "Kinetic Monolith" style) */
        lv_obj_t *brain = lv_obj_create(pg_ai);
        lv_obj_set_size(brain, 150, 150);
        lv_obj_align(brain, LV_ALIGN_CENTER, 0, -120);
        lv_obj_set_style_bg_color(brain, lv_color_hex(COL_AMBER), 0);
        lv_obj_set_style_bg_opa(brain, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(brain, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(brain, 0, 0);
        lv_obj_clear_flag(brain, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *brain_icon = lv_label_create(brain);
        lv_label_set_text(brain_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(brain_icon, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_text_font(brain_icon, &lv_font_montserrat_48, 0);
        lv_obj_center(brain_icon);

        /* Start breathing animation on outer ring */
        lv_anim_init(&brain_anim);
        lv_anim_set_var(&brain_anim, brain_ring);
        lv_anim_set_values(&brain_anim, 10, 60);
        lv_anim_set_duration(&brain_anim, 2500);
        lv_anim_set_playback_duration(&brain_anim, 2500);
        lv_anim_set_repeat_count(&brain_anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&brain_anim, brain_pulse_anim_cb);
        lv_anim_start(&brain_anim);
        brain_anim_running = true;

        /* State text */
        lbl_ai_state = lv_label_create(pg_ai);
        lv_label_set_text(lbl_ai_state, "Glyph AI \xe2\x80\x94 Idle");
        lv_obj_set_style_text_color(lbl_ai_state, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_ai_state, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl_ai_state, LV_ALIGN_CENTER, 0, 40);

        /* Hint */
        lbl_ai_hint = lv_label_create(pg_ai);
        lv_label_set_text(lbl_ai_hint, "Connect Dragon to enable AI features");
        lv_obj_set_style_text_color(lbl_ai_hint, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(lbl_ai_hint, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_ai_hint, LV_ALIGN_CENTER, 0, 76);

        /* Separator line */
        lv_obj_t *sep = lv_obj_create(pg_ai);
        lv_obj_set_size(sep, 32, 1);
        lv_obj_align(sep, LV_ALIGN_CENTER, 0, 62);
        lv_obj_set_style_bg_color(sep, lv_color_hex(COL_AMBER), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
        lv_obj_set_style_border_width(sep, 0, 0);
        lv_obj_set_style_radius(sep, 0, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* Swipe hint */
        lv_obj_t *swipe = lv_label_create(pg_ai);
        lv_label_set_text(swipe, LV_SYMBOL_DOWN "  swipe to initialize");
        lv_obj_set_style_text_color(swipe, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(swipe, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(swipe, 2, 0);
        lv_obj_align(swipe, LV_ALIGN_BOTTOM_MID, 0, -80);
    }

    /* ── PAGE 1: TOOLS ───────────────────────────────────────── */
    {
        add_page_title(pg_tools, "T O O L S", COL_AMBER);

        int x0 = PAGE_PAD;
        int x1 = PAGE_PAD + TILE_W + TILE_GAP;
        int y0 = STATUSBAR_H + 70;

        lbl_tool_wifi   = make_tool_tile(pg_tools, x0, y0,                        LV_SYMBOL_WIFI,    "WiFi",    COL_GREEN);
        lbl_tool_batt   = make_tool_tile(pg_tools, x1, y0,                        LV_SYMBOL_CHARGE,  "Battery", COL_AMBER);
        lbl_tool_heap   = make_tool_tile(pg_tools, x0, y0 + TILE_H + TILE_GAP,    LV_SYMBOL_LIST,    "Memory",  COL_CYAN);
        lbl_tool_dragon = make_tool_tile(pg_tools, x1, y0 + TILE_H + TILE_GAP,    LV_SYMBOL_UPLOAD,  "Dragon",  COL_CYAN);
        lbl_tool_fps    = make_tool_tile(pg_tools, x0, y0 + 2*(TILE_H + TILE_GAP), LV_SYMBOL_IMAGE,  "FPS",     COL_MINT);
        lbl_tool_rtc    = make_tool_tile(pg_tools, x1, y0 + 2*(TILE_H + TILE_GAP), LV_SYMBOL_BELL,   "Clock",   COL_AMBER);
    }

    /* ── PAGE 2: DRAGON ──────────────────────────────────────── */
    {
        add_page_title(pg_dragon, "D R A G O N", COL_CYAN);

        /* Viewport placeholder */
        lv_obj_t *vp = lv_obj_create(pg_dragon);
        lv_obj_set_size(vp, SW - PAGE_PAD * 2, 600);
        lv_obj_align(vp, LV_ALIGN_TOP_LEFT, PAGE_PAD, STATUSBAR_H + 70);
        lv_obj_set_style_bg_color(vp, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(vp, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(vp, 16, 0);
        lv_obj_set_style_border_width(vp, 1, 0);
        lv_obj_set_style_border_color(vp, lv_color_hex(COL_CYAN), 0);
        lv_obj_set_style_border_opa(vp, LV_OPA_30, 0);
        lv_obj_clear_flag(vp, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *vp_text = lv_label_create(vp);
        lv_label_set_text(vp_text, LV_SYMBOL_PLAY "  MJPEG Stream");
        lv_obj_set_style_text_color(vp_text, lv_color_hex(COL_CYAN), 0);
        lv_obj_set_style_text_font(vp_text, &lv_font_montserrat_18, 0);
        lv_obj_center(vp_text);

        /* State */
        lbl_dragon_state = lv_label_create(pg_dragon);
        lv_label_set_text(lbl_dragon_state, "Disconnected");
        lv_obj_set_style_text_color(lbl_dragon_state, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(lbl_dragon_state, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl_dragon_state, LV_ALIGN_TOP_LEFT, PAGE_PAD, STATUSBAR_H + 690);

        /* Info */
        lbl_dragon_info = lv_label_create(pg_dragon);
        lv_label_set_text(lbl_dragon_info, "Waiting for Dragon Q6A...");
        lv_obj_set_style_text_color(lbl_dragon_info, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(lbl_dragon_info, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_dragon_info, LV_ALIGN_TOP_LEFT, PAGE_PAD, STATUSBAR_H + 724);
    }

    /* ── PAGE 3: SETTINGS ────────────────────────────────────── */
    {
        add_page_title(pg_settings, "S E T T I N G S", COL_AMBER);

        int y = STATUSBAR_H + 70;
        int gap = ROW_H + 10;
        lbl_set_wifi   = make_setting_row(pg_settings, y,           LV_SYMBOL_WIFI,     "WiFi",        "---");
        lbl_set_dragon = make_setting_row(pg_settings, y + gap,     LV_SYMBOL_UPLOAD,   "Dragon Link", "---");
        lbl_set_heap   = make_setting_row(pg_settings, y + 2*gap,   LV_SYMBOL_LIST,     "Free Memory", "---");
        make_setting_row(pg_settings, y + 3*gap, LV_SYMBOL_SETTINGS, "Display",    "720x1280 DSI");
        make_setting_row(pg_settings, y + 4*gap, LV_SYMBOL_AUDIO,    "Audio",      "ES8388");
        make_setting_row(pg_settings, y + 5*gap, LV_SYMBOL_IMAGE,    "Camera",     "SC202CS");
        make_setting_row(pg_settings, y + 6*gap, LV_SYMBOL_REFRESH,  "IMU",        "BMI270");

        /* About */
        lv_obj_t *about = lv_label_create(pg_settings);
        lv_label_set_text(about, "Glyph OS v1.0.0\nTinkerTab  \xC2\xB7  ESP32-P4  \xC2\xB7  M5Stack Tab5");
        lv_obj_set_style_text_color(about, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(about, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(about, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(about, SW - PAGE_PAD * 2);
        lv_obj_align(about, LV_ALIGN_BOTTOM_MID, 0, -40);
    }

    /* ══════════════════════════════════════════════════════════
     * STATUS BAR — floating overlay on top of tileview
     * ══════════════════════════════════════════════════════════ */
    {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_set_size(bar, SW, STATUSBAR_H);
        lv_obj_set_pos(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* Amber accent line */
        lv_obj_t *accent = lv_obj_create(bar);
        lv_obj_set_size(accent, SW, 1);
        lv_obj_set_pos(accent, 0, STATUSBAR_H - 1);
        lv_obj_set_style_bg_color(accent, lv_color_hex(COL_AMBER), 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_20, 0);
        lv_obj_set_style_border_width(accent, 0, 0);
        lv_obj_set_style_radius(accent, 0, 0);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* Time — left */
        lbl_time = lv_label_create(bar);
        lv_label_set_text(lbl_time, "00:00");
        lv_obj_set_style_text_color(lbl_time, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl_time, LV_ALIGN_LEFT_MID, 20, 0);

        /* Mode badge — center */
        lbl_mode = lv_label_create(bar);
        lv_label_set_text(lbl_mode, "GLYPH");
        lv_obj_set_style_text_color(lbl_mode, lv_color_hex(COL_AMBER), 0);
        lv_obj_set_style_text_font(lbl_mode, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(lbl_mode, 3, 0);
        lv_obj_align(lbl_mode, LV_ALIGN_CENTER, 0, 0);

        /* Dragon status dot */
        lbl_dragon_dot = lv_obj_create(bar);
        lv_obj_set_size(lbl_dragon_dot, 8, 8);
        lv_obj_set_style_bg_color(lbl_dragon_dot, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_bg_opa(lbl_dragon_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lbl_dragon_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(lbl_dragon_dot, 0, 0);
        lv_obj_clear_flag(lbl_dragon_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(lbl_dragon_dot, LV_ALIGN_RIGHT_MID, -120, 0);

        /* WiFi icon */
        lbl_wifi = lv_label_create(bar);
        lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_wifi, LV_ALIGN_RIGHT_MID, -80, 0);

        /* Battery */
        lbl_batt = lv_label_create(bar);
        lv_label_set_text(lbl_batt, LV_SYMBOL_BATTERY_FULL " 100%");
        lv_obj_set_style_text_color(lbl_batt, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl_batt, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    /* ══════════════════════════════════════════════════════════
     * PAGE INDICATOR DOTS — right edge, vertically centered
     * ══════════════════════════════════════════════════════════ */
    {
        int total_h = NUM_PAGES * PAGE_DOT_SZ + (NUM_PAGES - 1) * PAGE_DOT_GAP;
        int start_y = (SH - total_h) / 2;
        for (int i = 0; i < NUM_PAGES; i++) {
            page_dots[i] = lv_obj_create(scr);
            lv_obj_set_size(page_dots[i], PAGE_DOT_SZ, PAGE_DOT_SZ);
            lv_obj_set_pos(page_dots[i], SW - 20, start_y + i * (PAGE_DOT_SZ + PAGE_DOT_GAP));
            lv_obj_set_style_radius(page_dots[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(page_dots[i], 0, 0);
            lv_obj_set_style_bg_color(page_dots[i],
                lv_color_hex(i == 0 ? COL_AMBER : COL_DIM), 0);
            lv_obj_set_style_bg_opa(page_dots[i],
                i == 0 ? LV_OPA_COVER : LV_OPA_40, 0);
            lv_obj_clear_flag(page_dots[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    /* ══════════════════════════════════════════════════════════
     * BOTTOM NAV BAR — tap to navigate (Stitch "Kinetic" style)
     * ══════════════════════════════════════════════════════════ */
    {
        lv_obj_t *nav = lv_obj_create(scr);
        lv_obj_set_size(nav, SW, NAV_H);
        lv_obj_set_pos(nav, 0, SH - NAV_H);
        lv_obj_set_style_bg_color(nav, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(nav, LV_OPA_90, 0);
        lv_obj_set_style_radius(nav, 0, 0);
        lv_obj_set_style_border_width(nav, 0, 0);
        lv_obj_set_style_pad_all(nav, 0, 0);
        lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

        /* Top accent line */
        lv_obj_t *nav_line = lv_obj_create(nav);
        lv_obj_set_size(nav_line, SW, 1);
        lv_obj_set_pos(nav_line, 0, 0);
        lv_obj_set_style_bg_color(nav_line, lv_color_hex(COL_AMBER), 0);
        lv_obj_set_style_bg_opa(nav_line, LV_OPA_10, 0);
        lv_obj_set_style_border_width(nav_line, 0, 0);
        lv_obj_set_style_radius(nav_line, 0, 0);
        lv_obj_clear_flag(nav_line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        const char *nav_syms[NUM_PAGES] = {
            LV_SYMBOL_HOME, LV_SYMBOL_LIST, LV_SYMBOL_UPLOAD, LV_SYMBOL_SETTINGS
        };
        int slot_w = SW / NUM_PAGES;

        for (int i = 0; i < NUM_PAGES; i++) {
            nav_icons[i] = lv_label_create(nav);
            lv_label_set_text(nav_icons[i], nav_syms[i]);
            lv_obj_set_style_text_font(nav_icons[i], &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(nav_icons[i],
                lv_color_hex(i == 0 ? COL_AMBER : COL_DIM), 0);
            lv_obj_set_pos(nav_icons[i], slot_w * i + slot_w / 2 - 10, NAV_H / 2 - 10);
            lv_obj_add_flag(nav_icons[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(nav_icons[i], nav_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    /* Register scroll event to update page dots + nav */
    lv_obj_add_event_cb(tileview, tileview_scroll_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Initial data */
    ui_home_update_status();
    tmr_update = lv_timer_create(update_timer_cb, 1000, NULL);

    lv_screen_load(scr);
    ESP_LOGI(TAG, "Glyph home screen created (tileview + nav + animations)");
    return scr;
}

/* ── Brain pulse animation callback ──────────────────────────── */
static void brain_pulse_anim_cb(void *obj, int32_t val)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

/* ── Update all navigation indicators for a given page ────────── */
static void update_nav_state(int page)
{
    for (int i = 0; i < NUM_PAGES; i++) {
        /* Page dots */
        if (page_dots[i]) {
            if (i == page) {
                uint32_t col = (page == 2) ? COL_CYAN : COL_AMBER;
                lv_obj_set_style_bg_color(page_dots[i], lv_color_hex(col), 0);
                lv_obj_set_style_bg_opa(page_dots[i], LV_OPA_COVER, 0);
                lv_obj_set_size(page_dots[i], PAGE_DOT_SZ, PAGE_DOT_SZ + 8);
            } else {
                lv_obj_set_style_bg_color(page_dots[i], lv_color_hex(COL_DIM), 0);
                lv_obj_set_style_bg_opa(page_dots[i], LV_OPA_40, 0);
                lv_obj_set_size(page_dots[i], PAGE_DOT_SZ, PAGE_DOT_SZ);
            }
        }
        /* Nav bar icons */
        if (nav_icons[i]) {
            if (i == page) {
                uint32_t col = (page == 2) ? COL_CYAN : COL_AMBER;
                lv_obj_set_style_text_color(nav_icons[i], lv_color_hex(col), 0);
            } else {
                lv_obj_set_style_text_color(nav_icons[i], lv_color_hex(COL_DIM), 0);
            }
        }
    }
}

/* ── Nav bar tap — jump to page ──────────────────────────────── */
static void nav_click_cb(lv_event_t *e)
{
    int page = (int)(intptr_t)lv_event_get_user_data(e);
    if (page >= 0 && page < NUM_PAGES && tiles[page] && tileview) {
        lv_tileview_set_tile(tileview, tiles[page], LV_ANIM_ON);
    }
}

/* ── Tileview scroll — update page indicator dots + nav ──────── */
static void tileview_scroll_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t *active = lv_tileview_get_tile_active(tv);
    if (!active) return;

    int new_page = lv_obj_get_y(active) / SH;
    if (new_page < 0) new_page = 0;
    if (new_page >= NUM_PAGES) new_page = NUM_PAGES - 1;

    if (new_page != current_page) {
        current_page = new_page;
        update_nav_state(current_page);
        ESP_LOGI(TAG, "Page → %d", current_page);
    }
}

/* ── Timer callback ──────────────────────────────────────────── */
static void update_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_home_update_status();
}

/* ── Status update ───────────────────────────────────────────── */
void ui_home_update_status(void)
{
    if (!scr) return;

    /* Battery */
    {
        uint8_t pct = tab5_battery_percent();
        const char *sym = LV_SYMBOL_BATTERY_FULL;
        if (pct < 15)       sym = LV_SYMBOL_BATTERY_EMPTY;
        else if (pct < 40)  sym = LV_SYMBOL_BATTERY_1;
        else if (pct < 60)  sym = LV_SYMBOL_BATTERY_2;
        else if (pct < 80)  sym = LV_SYMBOL_BATTERY_3;

        char buf[24];
        snprintf(buf, sizeof(buf), "%s %u%%", sym, pct);
        if (lbl_batt) lv_label_set_text(lbl_batt, buf);
        snprintf(buf, sizeof(buf), "%u%%", pct);
        if (lbl_tool_batt) lv_label_set_text(lbl_tool_batt, buf);
    }

    /* WiFi */
    {
        bool connected = tab5_wifi_connected();
        if (lbl_wifi) lv_obj_set_style_text_color(lbl_wifi,
            lv_color_hex(connected ? COL_GREEN : COL_RED), 0);
        if (lbl_tool_wifi) lv_label_set_text(lbl_tool_wifi, connected ? "Connected" : "Offline");
        if (lbl_set_wifi) lv_label_set_text(lbl_set_wifi, connected ? "Connected" : "Disconnected");
    }

    /* Dragon */
    {
        dragon_state_t st = tab5_dragon_get_state();
        const char *state_str = tab5_dragon_state_str();
        bool streaming = tab5_dragon_is_streaming();
        float fps = tab5_dragon_get_fps();

        uint32_t dot_color = COL_DIM;
        if (streaming)                                    dot_color = COL_CYAN;
        else if (st == DRAGON_STATE_CONNECTED ||
                 st == DRAGON_STATE_HANDSHAKE)             dot_color = COL_AMBER;
        else if (st == DRAGON_STATE_RECONNECTING)          dot_color = COL_CORAL;
        if (lbl_dragon_dot) {
            lv_obj_set_style_bg_color(lbl_dragon_dot, lv_color_hex(dot_color), 0);
            /* Pulse the dot size when streaming */
            if (streaming) {
                static bool dot_big = false;
                lv_obj_set_size(lbl_dragon_dot, dot_big ? 8 : 10, dot_big ? 8 : 10);
                dot_big = !dot_big;
            } else {
                lv_obj_set_size(lbl_dragon_dot, 8, 8);
            }
        }

        if (lbl_mode) {
            if (streaming) {
                lv_label_set_text(lbl_mode, "DRAGON");
                lv_obj_set_style_text_color(lbl_mode, lv_color_hex(COL_CYAN), 0);
            } else {
                lv_label_set_text(lbl_mode, "GLYPH");
                lv_obj_set_style_text_color(lbl_mode, lv_color_hex(COL_AMBER), 0);
            }
        }

        if (lbl_tool_dragon) lv_label_set_text(lbl_tool_dragon, state_str);
        char fps_buf[16];
        snprintf(fps_buf, sizeof(fps_buf), "%.1f", fps);
        if (lbl_tool_fps) lv_label_set_text(lbl_tool_fps, streaming ? fps_buf : "---");

        if (lbl_dragon_state) {
            lv_label_set_text(lbl_dragon_state, state_str);
            lv_obj_set_style_text_color(lbl_dragon_state,
                lv_color_hex(streaming ? COL_CYAN : COL_DIM), 0);
        }
        if (lbl_dragon_info) {
            if (streaming) {
                char info[48];
                snprintf(info, sizeof(info), "Streaming %.1f FPS", fps);
                lv_label_set_text(lbl_dragon_info, info);
                lv_obj_set_style_text_color(lbl_dragon_info, lv_color_hex(COL_CYAN), 0);
            } else {
                lv_label_set_text(lbl_dragon_info, "Waiting for Dragon Q6A...");
                lv_obj_set_style_text_color(lbl_dragon_info, lv_color_hex(COL_DIM), 0);
            }
        }

        if (lbl_set_dragon) lv_label_set_text(lbl_set_dragon, state_str);

        if (lbl_ai_state) {
            if (streaming) {
                lv_label_set_text(lbl_ai_state, "Glyph AI \xe2\x80\x94 Connected");
                lv_obj_set_style_text_color(lbl_ai_state, lv_color_hex(COL_CYAN), 0);
            } else {
                lv_label_set_text(lbl_ai_state, "Glyph AI \xe2\x80\x94 Idle");
                lv_obj_set_style_text_color(lbl_ai_state, lv_color_hex(COL_WHITE), 0);
            }
        }
        if (lbl_ai_hint) {
            lv_label_set_text(lbl_ai_hint,
                streaming ? "Dragon link active" : "Connect Dragon to enable AI features");
        }
    }

    /* Heap */
    {
        char buf[32];
        uint32_t free_kb = (uint32_t)(esp_get_free_heap_size() / 1024);
        snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)free_kb);
        if (lbl_tool_heap) lv_label_set_text(lbl_tool_heap, buf);
        if (lbl_set_heap) lv_label_set_text(lbl_set_heap, buf);
    }

    /* RTC */
    {
        tab5_rtc_time_t rtc = {0};
        if (tab5_rtc_get_time(&rtc) == ESP_OK) {
            char tbuf[8];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", rtc.hour, rtc.minute);
            if (lbl_time) lv_label_set_text(lbl_time, tbuf);
            if (lbl_tool_rtc) lv_label_set_text(lbl_tool_rtc, tbuf);
        }
    }
}

/* ── Destroy ─────────────────────────────────────────────────── */
void ui_home_destroy(void)
{
    if (brain_anim_running) {
        lv_anim_delete(brain_ring, brain_pulse_anim_cb);
        brain_anim_running = false;
    }
    if (tmr_update) {
        lv_timer_delete(tmr_update);
        tmr_update = NULL;
    }
    if (scr) {
        lv_obj_delete(scr);
        scr = NULL;
        tileview = NULL;
        brain_ring = NULL;
        memset(nav_icons, 0, sizeof(nav_icons));
        memset(tiles, 0, sizeof(tiles));
        lbl_time = lbl_mode = lbl_wifi = lbl_batt = lbl_dragon_dot = NULL;
        lbl_ai_state = lbl_ai_hint = NULL;
        lbl_tool_wifi = lbl_tool_batt = lbl_tool_heap = NULL;
        lbl_tool_dragon = lbl_tool_fps = lbl_tool_rtc = NULL;
        lbl_dragon_state = lbl_dragon_info = NULL;
        lbl_set_wifi = lbl_set_dragon = lbl_set_heap = NULL;
        memset(page_dots, 0, sizeof(page_dots));
        current_page = 0;
        ESP_LOGI(TAG, "Home screen destroyed");
    }
}
