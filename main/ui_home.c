/*
 * ui_home.c — TinkerTab home screen / launcher
 * 720x1280 portrait, LVGL v9, dark theme
 */

#include "ui_home.h"
#include "battery.h"
#include "rtc.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_home";

/* ── Palette ─────────────────────────────────────────────────── */
#define COL_BG          0x0A0A0F
#define COL_CARD        0x16182D
#define COL_CARD_HOVER  0x1E2140
#define COL_STATUSBAR   0x0D0D18
#define COL_DOCK        0x0D0D18
#define COL_WHITE       0xE8ECF1
#define COL_DIM         0x6B7280
#define COL_ACCENT      0x6366F1   /* indigo */
#define COL_ACCENT2     0x818CF8   /* lighter indigo */
#define COL_GREEN       0x34D399
#define COL_RED         0xF87171
#define COL_AMBER       0xFBBF24
#define COL_CYAN        0x22D3EE
#define COL_PINK        0xF472B6
#define COL_ORANGE      0xFB923C
#define COL_TEAL        0x2DD4BF

/* ── Layout (720 × 1280) ────────────────────────────────────── */
#define SW  720
#define SH  1280

#define STATUSBAR_H   44
#define CLOCK_Y       70
#define GRID_Y        300
#define GRID_COLS     3
#define GRID_ROWS     3
#define TILE_W        200
#define TILE_H        160
#define TILE_GAP      16
#define DOCK_H        72
#define DOCK_Y        (SH - DOCK_H)

/* ── App descriptors ─────────────────────────────────────────── */
typedef struct {
    const char *icon;
    const char *label;
    uint32_t    color;
} app_t;

static const app_t apps[] = {
    { LV_SYMBOL_IMAGE,     "Camera",   0x6366F1 },  /* indigo   */
    { LV_SYMBOL_DIRECTORY, "Files",    0xFB923C },  /* orange   */
    { LV_SYMBOL_AUDIO,     "Music",    0xF472B6 },  /* pink     */
    { LV_SYMBOL_SETTINGS,  "Settings", 0x6B7280 },  /* gray     */
    { LV_SYMBOL_KEYBOARD,  "Chat",     0x22D3EE },  /* cyan     */
    { LV_SYMBOL_WIFI,      "Network",  0x34D399 },  /* green    */
    { LV_SYMBOL_VOLUME_MID,"Voice",    0xFBBF24 },  /* amber    */
    { LV_SYMBOL_REFRESH,   "IMU",      0x2DD4BF },  /* teal     */
    { LV_SYMBOL_CHARGE,    "Battery",  0x34D399 },  /* green    */
};

static const char *dock_icons[]  = { LV_SYMBOL_HOME, LV_SYMBOL_KEYBOARD, LV_SYMBOL_IMAGE, LV_SYMBOL_SETTINGS };
static const char *dock_labels[] = { "Home", "Chat", "Camera", "Settings" };
#define DOCK_N 4

/* ── State ───────────────────────────────────────────────────── */
static lv_obj_t *scr       = NULL;
static lv_obj_t *lbl_time_sb = NULL;
static lv_obj_t *lbl_batt_sb = NULL;
static lv_obj_t *lbl_wifi_sb = NULL;
static lv_obj_t *lbl_clock  = NULL;
static lv_obj_t *lbl_date   = NULL;
static lv_obj_t *lbl_greet  = NULL;
static lv_timer_t *tmr_update = NULL;

/* ── Helpers ─────────────────────────────────────────────────── */
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* ── Forward decl ────────────────────────────────────────────── */
static void tile_click_cb(lv_event_t *e);
static void update_timer_cb(lv_timer_t *t);

/* ================================================================
 * Build the home screen
 * ================================================================ */
lv_obj_t *ui_home_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SW, SH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Status bar ──────────────────────────────────────────── */
    {
        lv_obj_t *bar = make_card(scr, 0, 0, SW, STATUSBAR_H, COL_STATUSBAR);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

        lbl_time_sb = lv_label_create(bar);
        lv_label_set_text(lbl_time_sb, "00:00");
        lv_obj_set_style_text_color(lbl_time_sb, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_time_sb, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl_time_sb, LV_ALIGN_LEFT_MID, 20, 0);

        /* WiFi icon */
        lbl_wifi_sb = lv_label_create(bar);
        lv_label_set_text(lbl_wifi_sb, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(lbl_wifi_sb, lv_color_hex(COL_RED), 0);
        lv_obj_set_style_text_font(lbl_wifi_sb, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl_wifi_sb, LV_ALIGN_RIGHT_MID, -80, 0);

        /* Battery */
        lbl_batt_sb = lv_label_create(bar);
        lv_label_set_text(lbl_batt_sb, LV_SYMBOL_BATTERY_FULL " 100%");
        lv_obj_set_style_text_color(lbl_batt_sb, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_batt_sb, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_batt_sb, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    /* ── Greeting + clock area ───────────────────────────────── */
    {
        lbl_greet = lv_label_create(scr);
        lv_label_set_text(lbl_greet, "Good evening");
        lv_obj_set_style_text_color(lbl_greet, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(lbl_greet, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl_greet, LV_ALIGN_TOP_MID, 0, CLOCK_Y);

        lbl_clock = lv_label_create(scr);
        lv_label_set_text(lbl_clock, "00:00");
        lv_obj_set_style_text_color(lbl_clock, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_letter_space(lbl_clock, 6, 0);
        lv_obj_align(lbl_clock, LV_ALIGN_TOP_MID, 0, CLOCK_Y + 32);

        lbl_date = lv_label_create(scr);
        lv_label_set_text(lbl_date, "Wednesday, March 26");
        lv_obj_set_style_text_color(lbl_date, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, 0, CLOCK_Y + 100);

        /* Subtle divider line */
        lv_obj_t *line = lv_obj_create(scr);
        lv_obj_set_size(line, 120, 2);
        lv_obj_align(line, LV_ALIGN_TOP_MID, 0, CLOCK_Y + 140);
        lv_obj_set_style_bg_color(line, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_60, 0);
        lv_obj_set_style_radius(line, 1, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ── App grid ────────────────────────────────────────────── */
    {
        int grid_w = GRID_COLS * TILE_W + (GRID_COLS - 1) * TILE_GAP;
        int x0 = (SW - grid_w) / 2;

        for (int r = 0; r < GRID_ROWS; r++) {
            for (int c = 0; c < GRID_COLS; c++) {
                int i = r * GRID_COLS + c;
                int x = x0 + c * (TILE_W + TILE_GAP);
                int y = GRID_Y + r * (TILE_H + TILE_GAP);

                lv_obj_t *tile = make_card(scr, x, y, TILE_W, TILE_H, COL_CARD);
                lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_user_data(tile, (void *)(intptr_t)i);
                lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, NULL);

                /* Pressed style */
                lv_obj_set_style_bg_color(tile, lv_color_hex(COL_CARD_HOVER), LV_STATE_PRESSED);

                /* Icon circle */
                lv_obj_t *circle = lv_obj_create(tile);
                lv_obj_set_size(circle, 56, 56);
                lv_obj_align(circle, LV_ALIGN_TOP_MID, 0, 24);
                lv_obj_set_style_bg_color(circle, lv_color_hex(apps[i].color), 0);
                lv_obj_set_style_bg_opa(circle, LV_OPA_20, 0);
                lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_border_width(circle, 0, 0);
                lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

                /* Icon */
                lv_obj_t *icon = lv_label_create(circle);
                lv_label_set_text(icon, apps[i].icon);
                lv_obj_set_style_text_color(icon, lv_color_hex(apps[i].color), 0);
                lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
                lv_obj_center(icon);

                /* Label */
                lv_obj_t *lbl = lv_label_create(tile);
                lv_label_set_text(lbl, apps[i].label);
                lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
                lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -16);
            }
        }
    }

    /* ── Dock ────────────────────────────────────────────────── */
    {
        lv_obj_t *dock = make_card(scr, 0, DOCK_Y, SW, DOCK_H, COL_DOCK);
        lv_obj_set_style_radius(dock, 0, 0);
        lv_obj_clear_flag(dock, LV_OBJ_FLAG_CLICKABLE);

        /* Thin top accent line */
        lv_obj_t *accent = lv_obj_create(dock);
        lv_obj_set_size(accent, SW, 1);
        lv_obj_set_pos(accent, 0, 0);
        lv_obj_set_style_bg_color(accent, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_30, 0);
        lv_obj_set_style_border_width(accent, 0, 0);
        lv_obj_set_style_radius(accent, 0, 0);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        int slot = SW / DOCK_N;
        for (int i = 0; i < DOCK_N; i++) {
            int cx = slot / 2 + i * slot;
            bool active = (i == 0);

            lv_obj_t *ic = lv_label_create(dock);
            lv_label_set_text(ic, dock_icons[i]);
            lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(ic,
                active ? lv_color_hex(COL_ACCENT2) : lv_color_hex(COL_DIM), 0);
            lv_obj_align(ic, LV_ALIGN_TOP_MID, cx - SW / 2, 12);

            lv_obj_t *lb = lv_label_create(dock);
            lv_label_set_text(lb, dock_labels[i]);
            lv_obj_set_style_text_font(lb, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lb,
                active ? lv_color_hex(COL_ACCENT2) : lv_color_hex(COL_DIM), 0);
            lv_obj_align(lb, LV_ALIGN_TOP_MID, cx - SW / 2, 42);
        }
    }

    /* Initial data fill */
    ui_home_update_status();

    /* Auto-update every 1 second */
    tmr_update = lv_timer_create(update_timer_cb, 1000, NULL);

    lv_screen_load(scr);
    ESP_LOGI(TAG, "Home screen created");
    return scr;
}

/* ── Tile click ──────────────────────────────────────────────── */
static void tile_click_cb(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(t);
    if (idx >= 0 && idx < (int)(sizeof(apps) / sizeof(apps[0]))) {
        ESP_LOGI(TAG, "App tapped: %s", apps[idx].label);
    }
}

/* ── Timer callback to auto-refresh status ───────────────────── */
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
#if __has_include("battery.h")
    {
        uint8_t pct = tab5_battery_percent();
        const char *sym = LV_SYMBOL_BATTERY_FULL;
        if (pct < 15)      sym = LV_SYMBOL_BATTERY_EMPTY;
        else if (pct < 40)  sym = LV_SYMBOL_BATTERY_1;
        else if (pct < 60)  sym = LV_SYMBOL_BATTERY_2;
        else if (pct < 80)  sym = LV_SYMBOL_BATTERY_3;

        char buf[24];
        snprintf(buf, sizeof(buf), "%s %u%%", sym, pct);
        lv_label_set_text(lbl_batt_sb, buf);
    }
#endif

    /* WiFi status — green if connected, red if not */
#if __has_include("wifi.h")
    {
        extern bool tab5_wifi_connected(void);
        if (tab5_wifi_connected()) {
            lv_obj_set_style_text_color(lbl_wifi_sb, lv_color_hex(COL_GREEN), 0);
        } else {
            lv_obj_set_style_text_color(lbl_wifi_sb, lv_color_hex(COL_RED), 0);
        }
    }
#else
    lv_obj_set_style_text_color(lbl_wifi_sb, lv_color_hex(COL_RED), 0);
#endif

    /* RTC time */
#if __has_include("rtc.h")
    {
        tab5_rtc_time_t rtc = {0};
        if (tab5_rtc_get_time(&rtc) == ESP_OK) {
            char tbuf[8];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", rtc.hour, rtc.minute);
            lv_label_set_text(lbl_time_sb, tbuf);
            lv_label_set_text(lbl_clock, tbuf);

            /* Greeting based on hour */
            const char *greet = "Good evening";
            if (rtc.hour < 12)      greet = "Good morning";
            else if (rtc.hour < 17) greet = "Good afternoon";
            lv_label_set_text(lbl_greet, greet);

            /* Date */
            static const char *wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
            static const char *mn[] = {"January","February","March","April","May","June",
                                       "July","August","September","October","November","December"};
            const char *w = (rtc.weekday < 7) ? wd[rtc.weekday] : "???";
            const char *m = (rtc.month >= 1 && rtc.month <= 12) ? mn[rtc.month - 1] : "???";
            char dbuf[40];
            snprintf(dbuf, sizeof(dbuf), "%s, %s %d", w, m, rtc.day);
            lv_label_set_text(lbl_date, dbuf);
        }
    }
#endif
}

/* ── Destroy ─────────────────────────────────────────────────── */
void ui_home_destroy(void)
{
    if (tmr_update) {
        lv_timer_delete(tmr_update);
        tmr_update = NULL;
    }
    if (scr) {
        lv_obj_delete(scr);
        scr = NULL;
        lbl_time_sb = lbl_batt_sb = lbl_wifi_sb = NULL;
        lbl_clock = lbl_date = lbl_greet = NULL;
        ESP_LOGI(TAG, "Home screen destroyed");
    }
}
