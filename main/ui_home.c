/*
 * ui_home.c — TinkerTab home screen / launcher
 * 720x1280 portrait, LVGL v9, dark theme
 */

#include "ui_home.h"
#include "battery.h"
#include "rtc.h"
#include "wifi.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_home";

/* ── Palette ─────────────────────────────────────────────────── */
#define COL_BG          0x000000
#define COL_STATUS_BG   0x1A1A2E
#define COL_TILE_BG     0x1E293B
#define COL_DOCK_BG     0x0F172A
#define COL_WHITE       0xFFFFFF
#define COL_GRAY        0x94A3B8
#define COL_ACCENT      0x3B82F6
#define COL_GREEN       0x22C55E
#define COL_RED         0xEF4444

/* ── Layout constants (720 × 1280) ───────────────────────────── */
#define SCREEN_W        720
#define SCREEN_H        1280

#define STATUSBAR_H     48
#define CLOCK_AREA_Y    80          /* below status bar             */
#define CLOCK_AREA_H    200

#define GRID_COLS       3
#define GRID_ROWS       3
#define TILE_SIZE       200
#define TILE_GAP        20
#define GRID_TOP_Y      (STATUSBAR_H + CLOCK_AREA_H + 40)

#define DOCK_H          80
#define DOCK_Y          (SCREEN_H - DOCK_H)
#define DOCK_ITEMS      4

/* ── App descriptor ──────────────────────────────────────────── */
typedef struct {
    const char *icon;   /* single emoji / letter shown large      */
    const char *label;  /* short name                              */
} app_entry_t;

static const app_entry_t apps[GRID_ROWS * GRID_COLS] = {
    { "C",  "Camera"   },
    { "F",  "Files"    },
    { "M",  "Music"    },
    { "S",  "Settings" },
    { "CH", "Chat"     },
    { "B",  "Browser"  },
    { "V",  "Voice"    },
    { "I",  "IMU"      },
    { "BT", "Battery"  },
};

/* Dock items */
typedef struct {
    const char *icon;
    const char *label;
    bool        active;
} dock_entry_t;

static const dock_entry_t dock_items[DOCK_ITEMS] = {
    { "H",  "Home",     true  },
    { "CH", "Chat",     false },
    { "C",  "Camera",   false },
    { "S",  "Settings", false },
};

/* ── Module state ────────────────────────────────────────────── */
static lv_obj_t *scr_home       = NULL;
static lv_obj_t *lbl_clock_sb   = NULL;   /* status-bar clock    */
static lv_obj_t *lbl_wifi       = NULL;
static lv_obj_t *lbl_batt       = NULL;
static lv_obj_t *lbl_clock_big  = NULL;   /* centre clock        */
static lv_obj_t *lbl_date       = NULL;

/* ── Forward declarations ────────────────────────────────────── */
static void build_status_bar(lv_obj_t *parent);
static void build_clock_widget(lv_obj_t *parent);
static void build_app_grid(lv_obj_t *parent);
static void build_dock(lv_obj_t *parent);
static void app_tile_click_cb(lv_event_t *e);

/* ================================================================
 * ui_home_create
 * ================================================================ */
lv_obj_t *ui_home_create(void)
{
    scr_home = lv_obj_create(NULL);
    lv_obj_set_size(scr_home, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr_home, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_home, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_home, LV_OBJ_FLAG_SCROLLABLE);

    build_status_bar(scr_home);
    build_clock_widget(scr_home);
    build_app_grid(scr_home);
    build_dock(scr_home);

    /* Initial data fill */
    ui_home_update_status();

    lv_screen_load(scr_home);
    ESP_LOGI(TAG, "Home screen created");

    return scr_home;
}

/* ================================================================
 * Status bar — top 48 px
 * ================================================================ */
static void build_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCREEN_W, STATUSBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_STATUS_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_left(bar, 20, 0);
    lv_obj_set_style_pad_right(bar, 20, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Clock (left) */
    lbl_clock_sb = lv_label_create(bar);
    lv_label_set_text(lbl_clock_sb, "12:34");
    lv_obj_set_style_text_color(lbl_clock_sb, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_clock_sb, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_clock_sb, LV_ALIGN_LEFT_MID, 0, 0);

    /* Battery % (right) */
    lbl_batt = lv_label_create(bar);
    lv_label_set_text(lbl_batt, "100%");
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_batt, LV_ALIGN_RIGHT_MID, 0, 0);

    /* WiFi indicator (just left of battery) */
    lbl_wifi = lv_label_create(bar);
    lv_label_set_text(lbl_wifi, "W");
    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_20, 0);
    lv_obj_align_to(lbl_wifi, lbl_batt, LV_ALIGN_OUT_LEFT_MID, -16, 0);
}

/* ================================================================
 * Clock widget — large centred time + date
 * ================================================================ */
static void build_clock_widget(lv_obj_t *parent)
{
    /* Container (no visible background) */
    lv_obj_t *cw = lv_obj_create(parent);
    lv_obj_set_size(cw, SCREEN_W, CLOCK_AREA_H);
    lv_obj_align(cw, LV_ALIGN_TOP_MID, 0, CLOCK_AREA_Y);
    lv_obj_set_style_bg_opa(cw, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cw, 0, 0);
    lv_obj_set_style_pad_all(cw, 0, 0);
    lv_obj_clear_flag(cw, LV_OBJ_FLAG_SCROLLABLE);

    /* Big time */
    lbl_clock_big = lv_label_create(cw);
    lv_label_set_text(lbl_clock_big, "12:34");
    lv_obj_set_style_text_color(lbl_clock_big, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_clock_big, &lv_font_montserrat_48, 0);
    /* montserrat_48 is the largest built-in; scale perception via letter spacing */
    lv_obj_set_style_text_letter_space(lbl_clock_big, 4, 0);
    lv_obj_align(lbl_clock_big, LV_ALIGN_TOP_MID, 0, 20);

    /* Date */
    lbl_date = lv_label_create(cw);
    lv_label_set_text(lbl_date, "Wed, Mar 26");
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_20, 0);
    lv_obj_align_to(lbl_date, lbl_clock_big, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
}

/* ================================================================
 * App grid — 3×3 tiles
 * ================================================================ */
static void build_app_grid(lv_obj_t *parent)
{
    /* Total grid width = 3*200 + 2*20 = 640  →  left margin = (720-640)/2 = 40 */
    const int grid_w  = GRID_COLS * TILE_SIZE + (GRID_COLS - 1) * TILE_GAP;
    const int x_start = (SCREEN_W - grid_w) / 2;

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int idx = row * GRID_COLS + col;

            int x = x_start + col * (TILE_SIZE + TILE_GAP);
            int y = GRID_TOP_Y + row * (TILE_SIZE + TILE_GAP);

            /* Tile container */
            lv_obj_t *tile = lv_obj_create(parent);
            lv_obj_set_size(tile, TILE_SIZE, TILE_SIZE);
            lv_obj_set_pos(tile, x, y);
            lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE_BG), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(tile, 20, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_pad_all(tile, 0, 0);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

            /* Store app index in user data for the click callback */
            lv_obj_set_user_data(tile, (void *)(intptr_t)idx);
            lv_obj_add_event_cb(tile, app_tile_click_cb, LV_EVENT_CLICKED, NULL);

            /* Icon text (top half) */
            lv_obj_t *lbl_icon = lv_label_create(tile);
            lv_label_set_text(lbl_icon, apps[idx].icon);
            lv_obj_set_style_text_color(lbl_icon, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_text_font(lbl_icon, &lv_font_montserrat_48, 0);
            lv_obj_align(lbl_icon, LV_ALIGN_TOP_MID, 0, 36);

            /* Label text (bottom) */
            lv_obj_t *lbl_name = lv_label_create(tile);
            lv_label_set_text(lbl_name, apps[idx].label);
            lv_obj_set_style_text_color(lbl_name, lv_color_hex(COL_WHITE), 0);
            lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_14, 0);
            lv_obj_align(lbl_name, LV_ALIGN_BOTTOM_MID, 0, -20);
        }
    }
}

/* ================================================================
 * Bottom dock — 4 items, 80 px tall
 * ================================================================ */
static void build_dock(lv_obj_t *parent)
{
    lv_obj_t *dock = lv_obj_create(parent);
    lv_obj_set_size(dock, SCREEN_W, DOCK_H);
    lv_obj_set_pos(dock, 0, DOCK_Y);
    lv_obj_set_style_bg_color(dock, lv_color_hex(COL_DOCK_BG), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dock, 0, 0);
    lv_obj_set_style_radius(dock, 0, 0);
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);

    const int slot_w = SCREEN_W / DOCK_ITEMS;   /* 180 px each */

    for (int i = 0; i < DOCK_ITEMS; i++) {
        int cx = slot_w / 2 + i * slot_w;

        /* Icon */
        lv_obj_t *icon = lv_label_create(dock);
        lv_label_set_text(icon, dock_items[i].icon);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(icon,
            dock_items[i].active ? lv_color_hex(COL_ACCENT)
                                 : lv_color_hex(COL_GRAY), 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, cx - SCREEN_W / 2, 8);

        /* Label */
        lv_obj_t *lbl = lv_label_create(dock);
        lv_label_set_text(lbl, dock_items[i].label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl,
            dock_items[i].active ? lv_color_hex(COL_ACCENT)
                                 : lv_color_hex(COL_GRAY), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, cx - SCREEN_W / 2, 50);
    }
}

/* ================================================================
 * App tile click handler
 * ================================================================ */
static void app_tile_click_cb(lv_event_t *e)
{
    lv_obj_t *tile = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(tile);
    if (idx >= 0 && idx < GRID_ROWS * GRID_COLS) {
        ESP_LOGI(TAG, "App tapped: %s", apps[idx].label);
    }
}

/* ================================================================
 * ui_home_update_status — refresh status bar with live data
 * ================================================================ */
void ui_home_update_status(void)
{
    if (!scr_home) return;

    /* ── Battery ─────────────────────────────────────────────── */
#if __has_include("battery.h")
    {
        uint8_t pct = tab5_battery_percent();
        char buf[8];
        snprintf(buf, sizeof(buf), "%u%%", pct);
        lv_label_set_text(lbl_batt, buf);
    }
#endif

    /* ── WiFi ────────────────────────────────────────────────── */
#if __has_include("wifi.h")
    {
        /* Zero-timeout check: returns ESP_OK if already connected */
        bool connected = (tab5_wifi_wait_connected(0) == ESP_OK);
        lv_obj_set_style_text_color(lbl_wifi,
            connected ? lv_color_hex(COL_GREEN) : lv_color_hex(COL_RED), 0);
    }
#endif

    /* ── RTC time ────────────────────────────────────────────── */
#if __has_include("rtc.h")
    {
        tab5_rtc_time_t rtc = {0};
        if (tab5_rtc_get_time(&rtc) == ESP_OK) {
            /* Time */
            char tbuf[8];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", rtc.hour, rtc.minute);
            lv_label_set_text(lbl_clock_sb, tbuf);
            lv_label_set_text(lbl_clock_big, tbuf);

            /* Date */
            static const char *wday_names[] = {
                "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
            };
            static const char *month_names[] = {
                "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
            };
            const char *wd = (rtc.weekday < 7)
                             ? wday_names[rtc.weekday] : "???";
            const char *mn = (rtc.month >= 1 && rtc.month <= 12)
                             ? month_names[rtc.month - 1] : "???";
            char dbuf[24];
            snprintf(dbuf, sizeof(dbuf), "%s, %s %d", wd, mn, rtc.day);
            lv_label_set_text(lbl_date, dbuf);
        }
    }
#endif
}

/* ================================================================
 * ui_home_destroy
 * ================================================================ */
void ui_home_destroy(void)
{
    if (scr_home) {
        lv_obj_delete(scr_home);
        scr_home      = NULL;
        lbl_clock_sb  = NULL;
        lbl_wifi      = NULL;
        lbl_batt      = NULL;
        lbl_clock_big = NULL;
        lbl_date      = NULL;
        ESP_LOGI(TAG, "Home screen destroyed");
    }
}
