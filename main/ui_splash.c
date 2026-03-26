/*
 * ui_splash.c — TinkerTab boot splash screen
 * 720x1280 portrait, LVGL v9, dark theme
 */

#include "ui_splash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_splash";

/* ── Palette ─────────────────────────────────────────────────── */
#define COL_BG          0x000000
#define COL_MONO_BG     0x2A2A2A
#define COL_WHITE       0xFFFFFF
#define COL_GRAY        0x666666
#define COL_ACCENT      0x3B82F6
#define COL_BAR_BG      0x1E293B

/* ── Layout constants (720 x 1280) ───────────────────────────── */
#define SCREEN_W        720
#define SCREEN_H        1280

#define MONO_SIZE       120          /* monogram rounded-rect       */
#define MONO_Y          400          /* centre of monogram (approx) */
#define TITLE_GAP       28           /* gap below monogram          */
#define STATUS_Y        900          /* status text y                */
#define BAR_W           240
#define BAR_H           6
#define BAR_Y           940          /* progress bar y               */

/* ── Module state ────────────────────────────────────────────── */
static lv_obj_t *scr_splash   = NULL;
static lv_obj_t *lbl_status   = NULL;
static lv_obj_t *bar_progress = NULL;

/* ================================================================
 * ui_splash_create
 * ================================================================ */
lv_obj_t *ui_splash_create(void)
{
    /* ── Screen ──────────────────────────────────────────────── */
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_size(scr_splash, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr_splash, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_splash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_splash, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Monogram container (rounded rect) ───────────────────── */
    lv_obj_t *mono_box = lv_obj_create(scr_splash);
    lv_obj_set_size(mono_box, MONO_SIZE, MONO_SIZE);
    lv_obj_align(mono_box, LV_ALIGN_TOP_MID, 0, MONO_Y - MONO_SIZE / 2);
    lv_obj_set_style_bg_color(mono_box, lv_color_hex(COL_MONO_BG), 0);
    lv_obj_set_style_bg_opa(mono_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mono_box, 24, 0);
    lv_obj_set_style_border_width(mono_box, 0, 0);
    lv_obj_set_style_pad_all(mono_box, 0, 0);
    lv_obj_clear_flag(mono_box, LV_OBJ_FLAG_SCROLLABLE);

    /* "TC" text inside monogram */
    lv_obj_t *lbl_tc = lv_label_create(mono_box);
    lv_label_set_text(lbl_tc, "TC");
    lv_obj_set_style_text_color(lbl_tc, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_tc, &lv_font_montserrat_48, 0);
    lv_obj_center(lbl_tc);

    /* ── "TINKERTAB" title ───────────────────────────────────── */
    lv_obj_t *lbl_title = lv_label_create(scr_splash);
    lv_label_set_text(lbl_title, "T I N K E R T A B");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(lbl_title, 6, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0,
                 MONO_Y + MONO_SIZE / 2 + TITLE_GAP);

    /* ── Status text ─────────────────────────────────────────── */
    lbl_status = lv_label_create(scr_splash);
    lv_label_set_text(lbl_status, "Starting...");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, STATUS_Y);

    /* ── Progress bar ────────────────────────────────────────── */
    bar_progress = lv_bar_create(scr_splash);
    lv_obj_set_size(bar_progress, BAR_W, BAR_H);
    lv_obj_align(bar_progress, LV_ALIGN_TOP_MID, 0, BAR_Y);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);

    /* bar track (background) */
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(COL_BAR_BG),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_progress, BAR_H / 2, LV_PART_MAIN);

    /* bar indicator (filled portion) */
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(COL_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, BAR_H / 2, LV_PART_INDICATOR);

    /* ── Load the screen ─────────────────────────────────────── */
    lv_screen_load(scr_splash);
    ESP_LOGI(TAG, "Splash screen created");

    return scr_splash;
}

/* ================================================================
 * ui_splash_set_status
 * ================================================================ */
void ui_splash_set_status(const char *text)
{
    if (lbl_status && text) {
        lv_label_set_text(lbl_status, text);
    }
}

/* ================================================================
 * ui_splash_set_progress
 * ================================================================ */
void ui_splash_set_progress(int percent)
{
    if (bar_progress) {
        if (percent < 0)   percent = 0;
        if (percent > 100) percent = 100;
        lv_bar_set_value(bar_progress, percent, LV_ANIM_ON);
    }
}

/* ================================================================
 * ui_splash_destroy
 * ================================================================ */
void ui_splash_destroy(void)
{
    if (scr_splash) {
        lv_obj_delete(scr_splash);
        scr_splash   = NULL;
        lbl_status   = NULL;
        bar_progress = NULL;
        ESP_LOGI(TAG, "Splash screen destroyed");
    }
}
