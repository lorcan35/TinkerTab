/*
 * ui_splash.c — TinkerOS boot splash
 * 720x1280 portrait, LVGL v9
 *
 * Amber-on-black aesthetic. Shows "GLYPH" wordmark with
 * system check items appearing as subsystems init.
 */

#include "ui_splash.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_splash";

/* ── Tinker palette ───────────────────────────────────────────── */
#define COL_BG          0x0D0D0D
#define COL_SURFACE     0x1A1A1A
#define COL_AMBER       0xFFB800
#define COL_AMBER_DIM   0x8A6400
#define COL_WHITE       0xE8E8E8
#define COL_GRAY        0x555555
#define COL_MINT        0x00E5A0
#define COL_BAR_BG      0x1A1A1A

/* ── Layout (720 x 1280) ────────────────────────────────────── */
#define SCREEN_W  720
#define SCREEN_H  1280

#define LOGO_Y    380
#define SUB_Y     470
#define CHECK_Y0  620
#define CHECK_GAP 44
#define BAR_W     300
#define BAR_H     4
#define BAR_Y     1050

/* ── State ───────────────────────────────────────────────────── */
static lv_obj_t   *scr_splash   = NULL;
static lv_obj_t   *lbl_status   = NULL;
static lv_obj_t   *bar_progress = NULL;
static lv_obj_t   *lbl_checks[8];
static int         check_count  = 0;

/* ================================================================ */
lv_obj_t *ui_splash_create(void)
{
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_size(scr_splash, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr_splash, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_splash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_splash, LV_OBJ_FLAG_SCROLLABLE);

    /* ── "TINKEROS" wordmark ──────────────────────────────────── */
    lv_obj_t *lbl_logo = lv_label_create(scr_splash);
    lv_label_set_text(lbl_logo, "T i n k e r O S");
    lv_obj_set_style_text_color(lbl_logo, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_text_font(lbl_logo, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_letter_space(lbl_logo, 10, 0);
    lv_obj_align(lbl_logo, LV_ALIGN_TOP_MID, 0, LOGO_Y);

    /* ── Subtitle ──────────────────────────────────────────── */
    lv_obj_t *lbl_sub = lv_label_create(scr_splash);
    lv_label_set_text(lbl_sub, "TinkerTab Firmware");
    lv_obj_set_style_text_color(lbl_sub, lv_color_hex(COL_AMBER_DIM), 0);
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_letter_space(lbl_sub, 4, 0);
    lv_obj_align(lbl_sub, LV_ALIGN_TOP_MID, 0, SUB_Y);

    /* ── Thin amber accent line ────────────────────────────── */
    lv_obj_t *line = lv_obj_create(scr_splash);
    lv_obj_set_size(line, 80, 2);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, SUB_Y + 40);
    lv_obj_set_style_bg_color(line, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
    lv_obj_set_style_radius(line, 1, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* ── System check labels (pre-create, hidden) ──────────── */
    check_count = 0;
    memset(lbl_checks, 0, sizeof(lbl_checks));

    /* ── Status text ───────────────────────────────────────── */
    lbl_status = lv_label_create(scr_splash);
    lv_label_set_text(lbl_status, "Initializing...");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, BAR_Y - 40);

    /* ── Progress bar ──────────────────────────────────────── */
    bar_progress = lv_bar_create(scr_splash);
    lv_obj_set_size(bar_progress, BAR_W, BAR_H);
    lv_obj_align(bar_progress, LV_ALIGN_TOP_MID, 0, BAR_Y);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(COL_BAR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_progress, BAR_H / 2, LV_PART_MAIN);

    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(COL_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, BAR_H / 2, LV_PART_INDICATOR);

    /* ── Version at bottom ─────────────────────────────────── */
    lv_obj_t *lbl_ver = lv_label_create(scr_splash);
    lv_label_set_text(lbl_ver, "v1.0.0");
    lv_obj_set_style_text_color(lbl_ver, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_ver, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_ver, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_screen_load(scr_splash);
    ESP_LOGI(TAG, "TinkerOS splash created");
    return scr_splash;
}

/* ================================================================ */
void ui_splash_set_status(const char *text)
{
    if (!lbl_status || !text) return;
    lv_label_set_text(lbl_status, text);

    /* Add as a check item if we have room */
    if (check_count < 8 && scr_splash) {
        lv_obj_t *lbl = lv_label_create(scr_splash);
        char buf[64];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  %s", text);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MINT), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, CHECK_Y0 + check_count * CHECK_GAP);
        lbl_checks[check_count] = lbl;
        check_count++;
    }
}

/* ================================================================ */
void ui_splash_set_progress(int percent)
{
    if (!bar_progress) return;
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    lv_bar_set_value(bar_progress, percent, LV_ANIM_ON);
}

/* ================================================================ */
void ui_splash_destroy(void)
{
    if (scr_splash) {
        lv_obj_delete(scr_splash);
        scr_splash   = NULL;
        lbl_status   = NULL;
        bar_progress = NULL;
        check_count  = 0;
        memset(lbl_checks, 0, sizeof(lbl_checks));
        ESP_LOGI(TAG, "Splash destroyed");
    }
}
