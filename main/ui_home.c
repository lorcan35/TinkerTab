/*
 * ui_home.c — Glyph OS home screen (v2 — refined OS aesthetic)
 * 720x1280 portrait, LVGL v9
 *
 * Inspired by iOS/Android — clean, soft, consumer-grade feel.
 * Tileview for vertical snap-scroll:
 *   Page 0 — Home (large clock, greeting, Glyph AI orb)
 *   Page 1 — Apps (rounded icon grid with labels)
 *   Page 2 — Dragon (full viewport, minimal chrome)
 *   Page 3 — Settings (iOS-style grouped list)
 *
 * Bottom nav bar + floating status bar.
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

/* ── Palette — soft dark ─────────────────────────────────────── */
#define COL_BG          0x000000
#define COL_CARD        0x1C1C1E   /* iOS dark card */
#define COL_CARD2       0x2C2C2E   /* lighter card */
#define COL_AMBER       0xFFB800
#define COL_AMBER_SOFT  0xFFD060
#define COL_CYAN        0x00B4D8
#define COL_MINT        0x30D158   /* iOS green */
#define COL_BLUE        0x0A84FF   /* iOS blue */
#define COL_PURPLE      0xBF5AF2   /* iOS purple */
#define COL_ORANGE      0xFF9F0A   /* iOS orange */
#define COL_RED         0xFF453A   /* iOS red */
#define COL_PINK        0xFF375F
#define COL_WHITE       0xFFFFFF
#define COL_LABEL       0xEBEBF5   /* primary label */
#define COL_LABEL2      0x8E8E93   /* secondary label */
#define COL_LABEL3      0x48484A   /* tertiary label */
#define COL_SEP         0x38383A   /* separator */

/* ── Layout ──────────────────────────────────────────────────── */
#define SW  720
#define SH  1280
#define SBAR_H    40
#define NAV_H     64
#define NUM_PAGES 4

/* App icon grid */
#define ICON_SZ   100
#define ICON_RAD  24
#define ICON_GAP  40
#define ICON_COLS 4
#define ICON_LBL_GAP 8

/* Settings */
#define SET_ROW_H   52
#define SET_RAD     14
#define SET_PAD     20

/* ── Forward decls ───────────────────────────────────────────── */
static void update_timer_cb(lv_timer_t *t);
static void tileview_scroll_cb(lv_event_t *e);
static void nav_click_cb(lv_event_t *e);
static void brain_pulse_cb(void *obj, int32_t val);

/* ── State ───────────────────────────────────────────────────── */
static lv_obj_t  *scr        = NULL;
static lv_obj_t  *tileview   = NULL;
static lv_obj_t  *tiles[NUM_PAGES] = {NULL};

/* Status bar */
static lv_obj_t  *lbl_sbar_time = NULL;
static lv_obj_t  *lbl_sbar_wifi = NULL;
static lv_obj_t  *lbl_sbar_batt = NULL;

/* Home page */
static lv_obj_t  *lbl_clock     = NULL;
static lv_obj_t  *lbl_date      = NULL;
static lv_obj_t  *lbl_greeting  = NULL;
static lv_obj_t  *orb_ring      = NULL;

/* Apps page */
static lv_obj_t  *lbl_app_wifi  = NULL;
static lv_obj_t  *lbl_app_mem   = NULL;

/* Dragon page */
static lv_obj_t  *lbl_dragon_st = NULL;
static lv_obj_t  *lbl_dragon_fps = NULL;

/* Settings page */
static lv_obj_t  *lbl_set_wifi  = NULL;
static lv_obj_t  *lbl_set_dragon = NULL;
static lv_obj_t  *lbl_set_mem   = NULL;

/* Nav bar */
static lv_obj_t  *nav_icons[NUM_PAGES] = {NULL};
static int        cur_page = 0;

/* Page dots */
static lv_obj_t  *page_dots[NUM_PAGES] = {NULL};

static lv_anim_t  orb_anim;
static bool        orb_anim_on = false;
static lv_timer_t *tmr_update  = NULL;

/* ================================================================
 * Helpers
 * ================================================================ */

/* Make a rounded-square app icon with symbol + label below */
static lv_obj_t *make_app_icon(lv_obj_t *parent, int col, int row, int y_off,
                               const char *sym, const char *name, uint32_t bg_col)
{
    int grid_w = ICON_COLS * ICON_SZ + (ICON_COLS - 1) * ICON_GAP;
    int x0 = (SW - grid_w) / 2;
    int x = x0 + col * (ICON_SZ + ICON_GAP);
    int y = y_off + row * (ICON_SZ + ICON_LBL_GAP + 20 + 24);

    /* Icon square */
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_set_size(icon, ICON_SZ, ICON_SZ);
    lv_obj_set_pos(icon, x, y);
    lv_obj_set_style_bg_color(icon, lv_color_hex(bg_col), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon, ICON_RAD, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    /* no shadows — ESP32-P4 draw budget */
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(icon);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_36, 0);
    lv_obj_center(ic);

    /* Label below */
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, ICON_SZ);
    lv_obj_set_pos(lbl, x, y + ICON_SZ + ICON_LBL_GAP);

    return ic;  /* return the symbol label for dynamic updates */
}

/* iOS-style settings row inside a group container */
static lv_obj_t *make_set_row(lv_obj_t *group, const char *icon,
                               uint32_t icon_bg, const char *label, const char *val)
{
    lv_obj_t *row = lv_obj_create(group);
    lv_obj_set_size(row, lv_pct(100), SET_ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Mini icon badge */
    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_set_size(badge, 30, 30);
    lv_obj_set_style_bg_color(badge, lv_color_hex(icon_bg), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *ic = lv_label_create(badge);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_14, 0);
    lv_obj_center(ic);

    /* Label */
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 40, 0);

    /* Value */
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val);
    lv_obj_set_style_text_color(v, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

    return v;
}

/* iOS-style grouped card container */
static lv_obj_t *make_set_group(lv_obj_t *parent, int y, int num_rows)
{
    lv_obj_t *grp = lv_obj_create(parent);
    int h = num_rows * SET_ROW_H + 8;
    lv_obj_set_size(grp, SW - SET_PAD * 2, h);
    lv_obj_set_pos(grp, SET_PAD, y);
    lv_obj_set_style_bg_color(grp, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(grp, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(grp, SET_RAD, 0);
    lv_obj_set_style_border_width(grp, 0, 0);
    lv_obj_set_style_pad_all(grp, 8, 0);
    lv_obj_set_style_pad_row(grp, 0, 0);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);
    return grp;
}

/* ================================================================
 * Build
 * ================================================================ */
lv_obj_t *ui_home_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SW, SH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── TILEVIEW ────────────────────────────────────────────── */
    tileview = lv_tileview_create(scr);
    lv_obj_set_size(tileview, SW, SH);
    lv_obj_set_pos(tileview, 0, 0);
    lv_obj_set_style_bg_color(tileview, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);

    tiles[0] = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_BOTTOM);
    tiles[1] = lv_tileview_add_tile(tileview, 0, 1, LV_DIR_TOP | LV_DIR_BOTTOM);
    tiles[2] = lv_tileview_add_tile(tileview, 0, 2, LV_DIR_TOP | LV_DIR_BOTTOM);
    tiles[3] = lv_tileview_add_tile(tileview, 0, 3, LV_DIR_TOP);

    /* ── PAGE 0: HOME (lock screen style) ────────────────────── */
    {
        lv_obj_t *pg = tiles[0];

        /* Large clock — center-top */
        lbl_clock = lv_label_create(pg);
        lv_label_set_text(lbl_clock, "00:00");
        lv_obj_set_style_text_color(lbl_clock, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_letter_space(lbl_clock, 2, 0);
        lv_obj_align(lbl_clock, LV_ALIGN_TOP_MID, 0, 180);

        /* Date */
        lbl_date = lv_label_create(pg);
        lv_label_set_text(lbl_date, "Wednesday, March 26");
        lv_obj_set_style_text_color(lbl_date, lv_color_hex(COL_LABEL2), 0);
        lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, 0, 245);

        /* Glyph AI orb — outer ring (breathing) */
        orb_ring = lv_obj_create(pg);
        lv_obj_set_size(orb_ring, 180, 180);
        lv_obj_align(orb_ring, LV_ALIGN_CENTER, 0, 40);
        lv_obj_set_style_bg_opa(orb_ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(orb_ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(orb_ring, 2, 0);
        lv_obj_set_style_border_color(orb_ring, lv_color_hex(COL_AMBER), 0);
        lv_obj_set_style_border_opa(orb_ring, LV_OPA_20, 0);
        lv_obj_clear_flag(orb_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* Inner orb */
        lv_obj_t *orb = lv_obj_create(pg);
        lv_obj_set_size(orb, 140, 140);
        lv_obj_align(orb, LV_ALIGN_CENTER, 0, 40);
        lv_obj_set_style_bg_color(orb, lv_color_hex(COL_AMBER), 0);
        lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(orb, 0, 0);
        /* no shadow — use border glow instead */
        lv_obj_set_style_border_width(orb, 2, 0);
        lv_obj_set_style_border_color(orb, lv_color_hex(COL_AMBER_SOFT), 0);
        lv_obj_set_style_border_opa(orb, LV_OPA_40, 0);
        lv_obj_clear_flag(orb, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *orb_icon = lv_label_create(orb);
        lv_label_set_text(orb_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(orb_icon, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_text_font(orb_icon, &lv_font_montserrat_48, 0);
        lv_obj_center(orb_icon);

        /* Breathing animation on ring */
        lv_anim_init(&orb_anim);
        lv_anim_set_var(&orb_anim, orb_ring);
        lv_anim_set_values(&orb_anim, 10, 80);
        lv_anim_set_duration(&orb_anim, 3000);
        lv_anim_set_playback_duration(&orb_anim, 3000);
        lv_anim_set_repeat_count(&orb_anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&orb_anim, brain_pulse_cb);
        lv_anim_start(&orb_anim);
        orb_anim_on = true;

        /* Greeting */
        lbl_greeting = lv_label_create(pg);
        lv_label_set_text(lbl_greeting, "Glyph AI");
        lv_obj_set_style_text_color(lbl_greeting, lv_color_hex(COL_LABEL), 0);
        lv_obj_set_style_text_font(lbl_greeting, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl_greeting, LV_ALIGN_CENTER, 0, 160);

        /* Swipe hint */
        lv_obj_t *hint = lv_label_create(pg);
        lv_label_set_text(hint, LV_SYMBOL_UP);
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_LABEL3), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -(NAV_H + 30));
    }

    /* ── PAGE 1: APPS (icon grid) ────────────────────────────── */
    {
        lv_obj_t *pg = tiles[1];
        int y0 = SBAR_H + 30;

        /* Section label */
        lv_obj_t *title = lv_label_create(pg);
        lv_label_set_text(title, "Apps");
        lv_obj_set_style_text_color(title, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        lv_obj_set_pos(title, 28, y0);

        int gy = y0 + 56;

        /* Row 0 */
        lbl_app_wifi = make_app_icon(pg, 0, 0, gy, LV_SYMBOL_WIFI,    "WiFi",    COL_BLUE);
        make_app_icon(pg, 1, 0, gy, LV_SYMBOL_UPLOAD,  "Dragon",  COL_CYAN);
        make_app_icon(pg, 2, 0, gy, LV_SYMBOL_IMAGE,   "Camera",  COL_PURPLE);
        make_app_icon(pg, 3, 0, gy, LV_SYMBOL_AUDIO,   "Audio",   COL_PINK);

        /* Row 1 */
        lbl_app_mem = make_app_icon(pg, 0, 1, gy, LV_SYMBOL_LIST,     "Files",   COL_ORANGE);
        make_app_icon(pg, 1, 1, gy, LV_SYMBOL_CHARGE,  "Battery", COL_MINT);
        make_app_icon(pg, 2, 1, gy, LV_SYMBOL_EYE_OPEN,"AI Chat", COL_AMBER);
        make_app_icon(pg, 3, 1, gy, LV_SYMBOL_SETTINGS,"Settings",COL_LABEL2);
    }

    /* ── PAGE 2: DRAGON (streaming viewport) ─────────────────── */
    {
        lv_obj_t *pg = tiles[2];

        /* Full-width viewport */
        lv_obj_t *vp = lv_obj_create(pg);
        lv_obj_set_size(vp, SW - 32, 700);
        lv_obj_align(vp, LV_ALIGN_TOP_MID, 0, SBAR_H + 20);
        lv_obj_set_style_bg_color(vp, lv_color_hex(0x0A0A0A), 0);
        lv_obj_set_style_bg_opa(vp, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(vp, 20, 0);
        lv_obj_set_style_border_width(vp, 0, 0);
        lv_obj_clear_flag(vp, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *vp_icon = lv_label_create(vp);
        lv_label_set_text(vp_icon, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(vp_icon, lv_color_hex(COL_LABEL3), 0);
        lv_obj_set_style_text_font(vp_icon, &lv_font_montserrat_48, 0);
        lv_obj_align(vp_icon, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t *vp_lbl = lv_label_create(vp);
        lv_label_set_text(vp_lbl, "Dragon Remote");
        lv_obj_set_style_text_color(vp_lbl, lv_color_hex(COL_LABEL2), 0);
        lv_obj_set_style_text_font(vp_lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(vp_lbl, LV_ALIGN_CENTER, 0, 30);

        /* Status card below */
        lv_obj_t *card = lv_obj_create(pg);
        lv_obj_set_size(card, SW - 32, 80);
        lv_obj_align(card, LV_ALIGN_TOP_MID, 0, SBAR_H + 740);
        lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_hor(card, 20, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lbl_dragon_st = lv_label_create(card);
        lv_label_set_text(lbl_dragon_st, "Disconnected");
        lv_obj_set_style_text_color(lbl_dragon_st, lv_color_hex(COL_LABEL2), 0);
        lv_obj_set_style_text_font(lbl_dragon_st, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl_dragon_st, LV_ALIGN_LEFT_MID, 0, 0);

        lbl_dragon_fps = lv_label_create(card);
        lv_label_set_text(lbl_dragon_fps, "---");
        lv_obj_set_style_text_color(lbl_dragon_fps, lv_color_hex(COL_LABEL3), 0);
        lv_obj_set_style_text_font(lbl_dragon_fps, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_dragon_fps, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    /* ── PAGE 3: SETTINGS (iOS grouped list) ─────────────────── */
    {
        lv_obj_t *pg = tiles[3];
        int y = SBAR_H + 20;

        lv_obj_t *title = lv_label_create(pg);
        lv_label_set_text(title, "Settings");
        lv_obj_set_style_text_color(title, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        lv_obj_set_pos(title, SET_PAD, y);
        y += 50;

        /* Group 1: Connection */
        lv_obj_t *g1 = make_set_group(pg, y, 2);
        lbl_set_wifi   = make_set_row(g1, LV_SYMBOL_WIFI,   COL_BLUE,  "WiFi",        "---");
        lbl_set_dragon = make_set_row(g1, LV_SYMBOL_UPLOAD,  COL_CYAN,  "Dragon Link", "---");
        y += 2 * SET_ROW_H + 8 + 16;

        /* Group 2: System */
        lv_obj_t *g2 = make_set_group(pg, y, 3);
        lbl_set_mem = make_set_row(g2, LV_SYMBOL_LIST,     COL_ORANGE, "Memory",  "---");
        make_set_row(g2, LV_SYMBOL_SETTINGS, COL_LABEL2,   "Display", "720x1280");
        make_set_row(g2, LV_SYMBOL_CHARGE,   COL_MINT,     "Battery", "---");
        y += 3 * SET_ROW_H + 8 + 16;

        /* Group 3: Hardware */
        lv_obj_t *g3 = make_set_group(pg, y, 3);
        make_set_row(g3, LV_SYMBOL_AUDIO,   COL_PINK,    "Audio",  "ES8388");
        make_set_row(g3, LV_SYMBOL_IMAGE,   COL_PURPLE,  "Camera", "SC202CS");
        make_set_row(g3, LV_SYMBOL_REFRESH, COL_RED,     "IMU",    "BMI270");
        y += 3 * SET_ROW_H + 8 + 16;

        /* About */
        lv_obj_t *about = lv_label_create(pg);
        lv_label_set_text(about, "Glyph OS v1.0.0\nTinkerTab \xC2\xB7 ESP32-P4");
        lv_obj_set_style_text_color(about, lv_color_hex(COL_LABEL3), 0);
        lv_obj_set_style_text_font(about, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(about, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(about, SW);
        lv_obj_set_pos(about, 0, y + 8);
    }

    /* ── STATUS BAR ──────────────────────────────────────────── */
    {
        lv_obj_t *sbar = lv_obj_create(scr);
        lv_obj_set_size(sbar, SW, SBAR_H);
        lv_obj_set_pos(sbar, 0, 0);
        lv_obj_set_style_bg_color(sbar, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(sbar, LV_OPA_80, 0);
        lv_obj_set_style_radius(sbar, 0, 0);
        lv_obj_set_style_border_width(sbar, 0, 0);
        lv_obj_clear_flag(sbar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lbl_sbar_time = lv_label_create(sbar);
        lv_label_set_text(lbl_sbar_time, "00:00");
        lv_obj_set_style_text_color(lbl_sbar_time, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_sbar_time, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_sbar_time, LV_ALIGN_LEFT_MID, 16, 0);

        lbl_sbar_wifi = lv_label_create(sbar);
        lv_label_set_text(lbl_sbar_wifi, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(lbl_sbar_wifi, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_sbar_wifi, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_sbar_wifi, LV_ALIGN_RIGHT_MID, -60, 0);

        lbl_sbar_batt = lv_label_create(sbar);
        lv_label_set_text(lbl_sbar_batt, "100%");
        lv_obj_set_style_text_color(lbl_sbar_batt, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_sbar_batt, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_sbar_batt, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    /* ── BOTTOM NAV ──────────────────────────────────────────── */
    {
        lv_obj_t *nav = lv_obj_create(scr);
        lv_obj_set_size(nav, SW, NAV_H);
        lv_obj_set_pos(nav, 0, SH - NAV_H);
        lv_obj_set_style_bg_color(nav, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(nav, LV_OPA_80, 0);
        lv_obj_set_style_radius(nav, 0, 0);
        lv_obj_set_style_border_width(nav, 0, 0);
        lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

        const char *syms[NUM_PAGES] = {
            LV_SYMBOL_HOME, LV_SYMBOL_LIST, LV_SYMBOL_PLAY, LV_SYMBOL_SETTINGS
        };
        int slot = SW / NUM_PAGES;
        for (int i = 0; i < NUM_PAGES; i++) {
            nav_icons[i] = lv_label_create(nav);
            lv_label_set_text(nav_icons[i], syms[i]);
            lv_obj_set_style_text_font(nav_icons[i], &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(nav_icons[i],
                lv_color_hex(i == 0 ? COL_WHITE : COL_LABEL3), 0);
            lv_obj_align(nav_icons[i], LV_ALIGN_LEFT_MID, slot * i + slot / 2 - 12, 0);
            lv_obj_add_flag(nav_icons[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(nav_icons[i], nav_click_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
        }
    }

    /* ── PAGE DOTS ───────────────────────────────────────────── */
    {
        int dot_sz = 6;
        int dot_gap = 10;
        int total = NUM_PAGES * dot_sz + (NUM_PAGES - 1) * dot_gap;
        int x0 = (SW - total) / 2;
        int y = SH - NAV_H - 20;
        for (int i = 0; i < NUM_PAGES; i++) {
            page_dots[i] = lv_obj_create(scr);
            lv_obj_set_size(page_dots[i], dot_sz, dot_sz);
            lv_obj_set_pos(page_dots[i], x0 + i * (dot_sz + dot_gap), y);
            lv_obj_set_style_radius(page_dots[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(page_dots[i], 0, 0);
            lv_obj_set_style_bg_color(page_dots[i],
                lv_color_hex(i == 0 ? COL_WHITE : COL_LABEL3), 0);
            lv_obj_set_style_bg_opa(page_dots[i], LV_OPA_COVER, 0);
            lv_obj_clear_flag(page_dots[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    lv_obj_add_event_cb(tileview, tileview_scroll_cb, LV_EVENT_VALUE_CHANGED, NULL);

    ui_home_update_status();
    tmr_update = lv_timer_create(update_timer_cb, 1000, NULL);

    lv_screen_load(scr);
    ESP_LOGI(TAG, "Glyph OS home (v2 refined) created");
    return scr;
}

/* ================================================================
 * Callbacks
 * ================================================================ */

static void brain_pulse_cb(void *obj, int32_t val)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

static void update_nav_ui(int page)
{
    for (int i = 0; i < NUM_PAGES; i++) {
        if (page_dots[i]) {
            lv_obj_set_style_bg_color(page_dots[i],
                lv_color_hex(i == page ? COL_WHITE : COL_LABEL3), 0);
        }
        if (nav_icons[i]) {
            lv_obj_set_style_text_color(nav_icons[i],
                lv_color_hex(i == page ? COL_WHITE : COL_LABEL3), 0);
        }
    }
}

static void nav_click_cb(lv_event_t *e)
{
    int pg = (int)(intptr_t)lv_event_get_user_data(e);
    if (pg >= 0 && pg < NUM_PAGES && tiles[pg] && tileview) {
        lv_tileview_set_tile(tileview, tiles[pg], LV_ANIM_ON);
    }
}

static void tileview_scroll_cb(lv_event_t *e)
{
    lv_obj_t *active = lv_tileview_get_tile_active(lv_event_get_target(e));
    if (!active) return;
    int pg = lv_obj_get_y(active) / SH;
    if (pg < 0) pg = 0;
    if (pg >= NUM_PAGES) pg = NUM_PAGES - 1;
    if (pg != cur_page) {
        cur_page = pg;
        update_nav_ui(cur_page);
    }
}

static void update_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_home_update_status();
}

/* ================================================================
 * Status update
 * ================================================================ */
void ui_home_update_status(void)
{
    if (!scr) return;

    /* Time */
    tab5_rtc_time_t rtc = {0};
    if (tab5_rtc_get_time(&rtc) == ESP_OK) {
        char tb[8];
        snprintf(tb, sizeof(tb), "%02d:%02d", rtc.hour, rtc.minute);
        if (lbl_clock) lv_label_set_text(lbl_clock, tb);
        if (lbl_sbar_time) lv_label_set_text(lbl_sbar_time, tb);
    }

    /* Battery */
    uint8_t bpct = tab5_battery_percent();
    {
        char bb[8];
        snprintf(bb, sizeof(bb), "%u%%", bpct);
        if (lbl_sbar_batt) lv_label_set_text(lbl_sbar_batt, bb);
    }

    /* WiFi */
    bool wifi_on = tab5_wifi_connected();
    if (lbl_sbar_wifi) {
        lv_obj_set_style_text_color(lbl_sbar_wifi,
            lv_color_hex(wifi_on ? COL_WHITE : COL_LABEL3), 0);
    }
    if (lbl_set_wifi) {
        lv_label_set_text(lbl_set_wifi, wifi_on ? "Connected" : "Off");
    }

    /* Dragon */
    {
        bool streaming = tab5_dragon_is_streaming();
        const char *st_str = tab5_dragon_state_str();
        float fps = tab5_dragon_get_fps();

        if (lbl_dragon_st) {
            lv_label_set_text(lbl_dragon_st, streaming ? "Streaming" : st_str);
            lv_obj_set_style_text_color(lbl_dragon_st,
                lv_color_hex(streaming ? COL_CYAN : COL_LABEL2), 0);
        }
        if (lbl_dragon_fps) {
            if (streaming) {
                char fb[16];
                snprintf(fb, sizeof(fb), "%.1f FPS", fps);
                lv_label_set_text(lbl_dragon_fps, fb);
            } else {
                lv_label_set_text(lbl_dragon_fps, "---");
            }
        }
        if (lbl_set_dragon) lv_label_set_text(lbl_set_dragon, st_str);

        /* Greeting changes with state */
        if (lbl_greeting) {
            if (streaming)
                lv_label_set_text(lbl_greeting, "Dragon Connected");
            else
                lv_label_set_text(lbl_greeting, "Glyph AI");
        }
    }

    /* Memory */
    {
        char mb[24];
        uint32_t fk = (uint32_t)(esp_get_free_heap_size() / 1024);
        snprintf(mb, sizeof(mb), "%lu KB", (unsigned long)fk);
        if (lbl_set_mem) lv_label_set_text(lbl_set_mem, mb);
    }
}

/* ================================================================
 * Destroy
 * ================================================================ */
void ui_home_destroy(void)
{
    if (orb_anim_on) {
        lv_anim_delete(orb_ring, brain_pulse_cb);
        orb_anim_on = false;
    }
    if (tmr_update) {
        lv_timer_delete(tmr_update);
        tmr_update = NULL;
    }
    if (scr) {
        lv_obj_delete(scr);
        scr = NULL;
        tileview = NULL;
        orb_ring = NULL;
        lbl_clock = lbl_date = lbl_greeting = NULL;
        lbl_sbar_time = lbl_sbar_wifi = lbl_sbar_batt = NULL;
        lbl_app_wifi = lbl_app_mem = NULL;
        lbl_dragon_st = lbl_dragon_fps = NULL;
        lbl_set_wifi = lbl_set_dragon = lbl_set_mem = NULL;
        memset(nav_icons, 0, sizeof(nav_icons));
        memset(page_dots, 0, sizeof(page_dots));
        memset(tiles, 0, sizeof(tiles));
        cur_page = 0;
        ESP_LOGI(TAG, "Home destroyed");
    }
}
