/**
 * TinkerTab — Settings Screen
 *
 * Dark-themed settings UI for M5Stack Tab5 (720x1280 portrait).
 * LVGL v9 API, accent color #3B82F6.
 */

#include "ui_settings.h"
#include "ui_home.h"
#include "ui_core.h"
#include "display.h"
#include "battery.h"
#include "wifi.h"
#include "bluetooth.h"
#include "rtc.h"
#include "sdcard.h"
#include "imu.h"
#include "config.h"
#include "settings.h"
#include "audio.h"
#include "voice.h"
#include "ui_keyboard.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_settings";

/* ── Colors ─────────────────────────────────────────────────────────── */
#define COL_BG          lv_color_hex(0x0F0F23)
#define COL_TOPBAR      lv_color_hex(0x1A1A2E)
#define COL_ACCENT      lv_color_hex(0x3B82F6)
#define COL_ROW_EVEN    lv_color_hex(0x0F172A)
#define COL_ROW_ODD     lv_color_hex(0x1E293B)
#define COL_TEXT        lv_color_hex(0xE2E8F0)
#define COL_TEXT_DIM    lv_color_hex(0x94A3B8)
#define COL_GREEN       lv_color_hex(0x22C55E)
#define COL_RED         lv_color_hex(0xEF4444)
#define COL_YELLOW      lv_color_hex(0xEAB308)

/* ── Layout constants ───────────────────────────────────────────────── */
#define TOPBAR_H        48
#define ROW_H           60
#define SECTION_PAD     12
#define SIDE_PAD        16
#define CORNER_R        8

/* ── Screen-lifetime state ──────────────────────────────────────────── */
static lv_obj_t *s_screen          = NULL;
static lv_obj_t *s_scroll          = NULL;

/* Live-updated labels */
static lv_obj_t *s_lbl_wifi       = NULL;
static lv_obj_t *s_lbl_bat_volt   = NULL;
static lv_obj_t *s_lbl_bat_status = NULL;
static lv_obj_t *s_bar_bat_level  = NULL;
static lv_obj_t *s_lbl_bat_pct    = NULL;
static lv_obj_t *s_lbl_sd_info    = NULL;
static lv_obj_t *s_lbl_heap       = NULL;
static lv_obj_t *s_lbl_psram      = NULL;
static lv_obj_t *s_lbl_orient     = NULL;

/* NTP sync spinner (created on demand, removed on completion) */
static lv_obj_t *s_ntp_spinner    = NULL;
static lv_obj_t *s_ntp_btn_label  = NULL;

/* H3: Dragon host text input */
static lv_obj_t *s_dragon_ta      = NULL;

/* Brightness slider tracks the current value */
static lv_obj_t *s_slider_bright  = NULL;

/* Volume slider */
static lv_obj_t *s_slider_volume  = NULL;

/* Auto-rotate switch */
static lv_obj_t *s_sw_autorot     = NULL;

/* Row index counter for alternating colours */
static int s_row_idx = 0;

/* Guard flag for background tasks during destroy */
static volatile bool s_destroying = false;
static lv_timer_t *s_refresh_timer = NULL;

static void settings_refresh_cb(lv_timer_t *t) {
    (void)t;
    if (!s_destroying) ui_settings_update();
}

/* ── Forward declarations ───────────────────────────────────────────── */
static lv_obj_t *make_topbar(lv_obj_t *parent);
static lv_obj_t *make_section(lv_obj_t *parent, const char *title);
static lv_obj_t *make_row(lv_obj_t *section);
static void      add_row_label(lv_obj_t *row, const char *text);
static lv_obj_t *add_row_value(lv_obj_t *row, const char *text);
static void      ntp_sync_task(void *arg);

/* ── Event callbacks ────────────────────────────────────────────────── */

static void cb_back_btn(lv_event_t *e)
{
    (void)e;
    ui_settings_destroy();
    ui_home_go_home();
    lv_screen_load(ui_home_get_screen());
}

static void cb_brightness(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    tab5_display_set_brightness(val);
    tab5_settings_set_brightness((uint8_t)val);
    ESP_LOGI(TAG, "Brightness set to %d%% (saved)", val);
}

static void cb_volume(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    tab5_audio_set_volume((uint8_t)val);
    tab5_settings_set_volume((uint8_t)val);
    ESP_LOGI(TAG, "Volume set to %d%% (saved)", val);
}

static void cb_autorotate(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Auto-rotate %s", on ? "enabled" : "disabled");
    /* Update orientation label immediately */
    if (s_lbl_orient) {
        if (on) {
            tab5_orientation_t o = tab5_imu_get_orientation();
            const char *names[] = {"Portrait", "Landscape", "Portrait Inv", "Landscape Inv"};
            lv_label_set_text(s_lbl_orient, names[o]);
        } else {
            lv_label_set_text(s_lbl_orient, "Off");
        }
    }
}

static void cb_cloud_mode(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    lv_obj_t *hint = lv_event_get_user_data(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    tab5_settings_set_cloud_mode(on ? 1 : 0);
    ESP_LOGI(TAG, "Cloud mode %s", on ? "ON" : "OFF");
    if (hint) {
        lv_label_set_text(hint, on ? "On" : "Off");
        lv_obj_set_style_text_color(hint, on ? COL_GREEN : COL_TEXT_DIM, 0);
    }
    voice_send_cloud_mode(on);
}

static void cb_wake_word(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    lv_obj_t *hint = lv_event_get_user_data(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    tab5_settings_set_wake_word(on ? 1 : 0);
    ESP_LOGI(TAG, "Wake word %s", on ? "ON" : "OFF");
    if (hint) {
        lv_label_set_text(hint, on ? "On" : "Off");
        lv_obj_set_style_text_color(hint, on ? COL_GREEN : COL_TEXT_DIM, 0);
    }
    if (on) {
        voice_start_always_listening();
    } else {
        voice_stop_always_listening();
    }
}

/* M3: WiFi setup — launches existing wifi picker screen */
static void cb_wifi_setup(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Launching WiFi setup screen");
    extern lv_obj_t *ui_wifi_create(void);
    ui_wifi_create();
}

/* H3: Dragon host — save on defocus/return key */
static void cb_dragon_host_done(lv_event_t *e)
{
    (void)e;
    if (!s_dragon_ta) return;
    const char *txt = lv_textarea_get_text(s_dragon_ta);
    if (txt && txt[0]) {
        tab5_settings_set_dragon_host(txt);
        ESP_LOGI(TAG, "Dragon host saved: %s", txt);
    }
    ui_keyboard_hide();
}

static void cb_dragon_host_click(lv_event_t *e)
{
    (void)e;
    if (s_dragon_ta) {
        ui_keyboard_show(s_dragon_ta);
    }
}

static void cb_ntp_sync(lv_event_t *e)
{
    (void)e;
    /* Avoid double-tap */
    if (s_ntp_spinner) return;

    /* Show spinner next to button label */
    if (s_ntp_btn_label) {
        lv_label_set_text(s_ntp_btn_label, "Syncing...");
    }

    /* Spawn a FreeRTOS task so we don't block LVGL */
    xTaskCreate(ntp_sync_task, "ntp_sync", 4096, NULL, 5, NULL);
}

/* ── NTP sync background task ───────────────────────────────────────── */

static void ntp_sync_task(void *arg)
{
    (void)arg;
    esp_err_t ret = tab5_rtc_sync_from_ntp();

    if (s_destroying) { vTaskDelete(NULL); return; }

    /* Thread-safe UI update via ui_core mutex */
    tab5_ui_lock();
    if (s_destroying || !s_ntp_btn_label) { tab5_ui_unlock(); vTaskDelete(NULL); return; }
    if (s_ntp_btn_label) {
        if (ret == ESP_OK) {
            lv_label_set_text(s_ntp_btn_label, "Synced!");
            ESP_LOGI(TAG, "NTP sync OK");
        } else {
            lv_label_set_text(s_ntp_btn_label, "Failed");
            ESP_LOGW(TAG, "NTP sync failed: %s", esp_err_to_name(ret));
        }
    }
    s_ntp_spinner = NULL;
    tab5_ui_unlock();
    vTaskDelete(NULL);
}

/* ── Helper: top bar ────────────────────────────────────────────────── */

static lv_obj_t *make_topbar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, TOPBAR_H);
    lv_obj_set_style_bg_color(bar, COL_TOPBAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, SIDE_PAD, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Back button */
    lv_obj_t *btn = lv_button_create(bar);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 48, TOPBAR_H);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn, cb_back_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(arrow, COL_TEXT, 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_24, 0);
    lv_obj_center(arrow);

    /* Title */
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* Spacer to balance the back button */
    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 48, 1);

    return bar;
}

/* ── Helper: section container with header ──────────────────────────── */

static lv_obj_t *make_section(lv_obj_t *parent, const char *title)
{
    /* Reset row counter for each section */
    s_row_idx = 0;

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_width(cont, 720 - 2 * SIDE_PAD);
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(cont, SECTION_PAD, 0);
    lv_obj_set_style_pad_bottom(cont, 4, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Section header */
    lv_obj_t *hdr = lv_label_create(cont);
    lv_label_set_text(hdr, title);
    lv_obj_set_style_text_color(hdr, COL_ACCENT, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_left(hdr, 4, 0);
    lv_obj_set_style_pad_bottom(hdr, 4, 0);

    return cont;
}

/* ── Helper: single row inside a section ────────────────────────────── */

static lv_obj_t *make_row(lv_obj_t *section)
{
    lv_obj_t *row = lv_obj_create(section);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 720 - 2 * SIDE_PAD, ROW_H);
    lv_obj_set_style_bg_color(row, (s_row_idx % 2 == 0) ? COL_ROW_EVEN : COL_ROW_ODD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, CORNER_R, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    s_row_idx++;
    return row;
}

/* ── Helper: add label on left side of a row ────────────────────────── */

static void add_row_label(lv_obj_t *row, const char *text)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
}

/* ── Helper: add value label on right side of a row ─────────────────── */

static lv_obj_t *add_row_value(lv_obj_t *row, const char *text)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    return lbl;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *ui_settings_create(void)
{
    if (s_screen) {
        ESP_LOGW(TAG, "Settings screen already exists");
        return s_screen;
    }

    /* ── Screen ─────────────────────────────────────────────────────── */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Top bar ────────────────────────────────────────────────────── */
    make_topbar(s_screen);

    /* ── Scrollable body ────────────────────────────────────────────── */
    s_scroll = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, 720, 1280 - TOPBAR_H);
    lv_obj_align(s_scroll, LV_ALIGN_TOP_LEFT, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(s_scroll, SIDE_PAD, 0);
    lv_obj_set_style_pad_top(s_scroll, 8, 0);
    lv_obj_set_style_pad_bottom(s_scroll, 32, 0);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);

    /* ────────────────────────────────────────────────────────────────
     *  SECTION: Display
     * ──────────────────────────────────────────────────────────────── */
    {
        lv_obj_t *sec = make_section(s_scroll, "Display");

        /* Brightness row */
        lv_obj_t *row_bright = make_row(sec);
        add_row_label(row_bright, "Brightness");

        s_slider_bright = lv_slider_create(row_bright);
        lv_obj_set_width(s_slider_bright, 280);
        lv_slider_set_range(s_slider_bright, 0, 100);
        lv_slider_set_value(s_slider_bright, tab5_settings_get_brightness(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_slider_bright, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider_bright, COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_slider_bright, COL_ACCENT, LV_PART_KNOB);
        lv_obj_set_style_pad_all(s_slider_bright, 4, LV_PART_KNOB);
        lv_obj_add_event_cb(s_slider_bright, cb_brightness, LV_EVENT_VALUE_CHANGED, NULL);

        /* Volume row */
        lv_obj_t *row_vol = make_row(sec);
        add_row_label(row_vol, "Volume");

        s_slider_volume = lv_slider_create(row_vol);
        lv_obj_set_width(s_slider_volume, 280);
        lv_slider_set_range(s_slider_volume, 0, 100);
        lv_slider_set_value(s_slider_volume, tab5_settings_get_volume(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_slider_volume, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider_volume, COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_slider_volume, COL_ACCENT, LV_PART_KNOB);
        lv_obj_set_style_pad_all(s_slider_volume, 4, LV_PART_KNOB);
        lv_obj_add_event_cb(s_slider_volume, cb_volume, LV_EVENT_VALUE_CHANGED, NULL);

        /* Auto-rotate row */
        lv_obj_t *row_rot = make_row(sec);
        add_row_label(row_rot, "Auto-rotate");

        /* Right side: orientation label + switch */
        lv_obj_t *rot_right = lv_obj_create(row_rot);
        lv_obj_remove_style_all(rot_right);
        lv_obj_set_size(rot_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rot_right, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rot_right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(rot_right, 12, 0);

        s_lbl_orient = lv_label_create(rot_right);
        lv_label_set_text(s_lbl_orient, "Off");
        lv_obj_set_style_text_color(s_lbl_orient, COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(s_lbl_orient, &lv_font_montserrat_18, 0);

        s_sw_autorot = lv_switch_create(rot_right);
        lv_obj_set_size(s_sw_autorot, 60, 36);
        lv_obj_set_style_bg_color(s_sw_autorot, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_sw_autorot, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_sw_autorot, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_add_event_cb(s_sw_autorot, cb_autorotate, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* ────────────────────────────────────────────────────────────────
     *  SECTION: Network
     * ──────────────────────────────────────────────────────────────── */
    {
        lv_obj_t *sec = make_section(s_scroll, "Network");

        /* WiFi row with configure button */
        lv_obj_t *row_wifi = make_row(sec);
        add_row_label(row_wifi, "WiFi");
        s_lbl_wifi = add_row_value(row_wifi, "Checking...");

        /* M3: WiFi configure button — launches full WiFi picker screen */
        lv_obj_t *row_wifi_cfg = make_row(sec);
        add_row_label(row_wifi_cfg, "WiFi Setup");

        lv_obj_t *wifi_btn = lv_button_create(row_wifi_cfg);
        lv_obj_set_size(wifi_btn, 180, 48);
        lv_obj_set_style_bg_color(wifi_btn, COL_ACCENT, 0);
        lv_obj_set_style_radius(wifi_btn, 6, 0);
        lv_obj_add_event_cb(wifi_btn, cb_wifi_setup, LV_EVENT_CLICKED, NULL);

        lv_obj_t *wifi_btn_lbl = lv_label_create(wifi_btn);
        lv_label_set_text(wifi_btn_lbl, LV_SYMBOL_WIFI " Configure");
        lv_obj_set_style_text_color(wifi_btn_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(wifi_btn_lbl, &lv_font_montserrat_18, 0);
        lv_obj_center(wifi_btn_lbl);

        /* Bluetooth row */
        lv_obj_t *row_bt = make_row(sec);
        add_row_label(row_bt, "Bluetooth");
        lv_obj_t *bt_val = add_row_value(row_bt, "Not supported");
        lv_obj_set_style_text_color(bt_val, COL_TEXT_DIM, 0);

        /* Sub-label for BT explanation */
        /* We'll just make the value text more descriptive */
        lv_label_set_text(bt_val, "ESP-Hosted pending");
        lv_obj_set_style_text_color(bt_val, COL_YELLOW, 0);

        /* Sync Time row */
        lv_obj_t *row_ntp = make_row(sec);
        add_row_label(row_ntp, "Sync Time");

        lv_obj_t *ntp_btn = lv_button_create(row_ntp);
        lv_obj_set_size(ntp_btn, 140, 48);
        lv_obj_set_style_bg_color(ntp_btn, COL_ACCENT, 0);
        lv_obj_set_style_radius(ntp_btn, 6, 0);
        lv_obj_add_event_cb(ntp_btn, cb_ntp_sync, LV_EVENT_CLICKED, NULL);

        s_ntp_btn_label = lv_label_create(ntp_btn);
        lv_label_set_text(s_ntp_btn_label, "Sync NTP");
        lv_obj_set_style_text_color(s_ntp_btn_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_ntp_btn_label, &lv_font_montserrat_18, 0);
        lv_obj_center(s_ntp_btn_label);

        /* H3: Dragon host input — editable text field */
        lv_obj_t *row_dragon = make_row(sec);
        add_row_label(row_dragon, "Dragon Host");

        s_dragon_ta = lv_textarea_create(row_dragon);
        lv_obj_set_size(s_dragon_ta, 280, 48);
        lv_textarea_set_one_line(s_dragon_ta, true);
        lv_textarea_set_max_length(s_dragon_ta, 63);
        lv_obj_set_style_bg_color(s_dragon_ta, lv_color_hex(0x1E1E2E), 0);
        lv_obj_set_style_text_color(s_dragon_ta, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_dragon_ta, &lv_font_montserrat_16, 0);
        lv_obj_set_style_border_width(s_dragon_ta, 1, 0);
        lv_obj_set_style_border_color(s_dragon_ta, COL_ACCENT, 0);
        lv_obj_set_style_radius(s_dragon_ta, 6, 0);
        lv_obj_set_style_pad_left(s_dragon_ta, 10, 0);
        lv_textarea_set_placeholder_text(s_dragon_ta, "192.168.1.89");

        /* Load current value from NVS */
        char dhost[64];
        tab5_settings_get_dragon_host(dhost, sizeof(dhost));
        if (dhost[0]) {
            lv_textarea_set_text(s_dragon_ta, dhost);
        }

        lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_click, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_done, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_done, LV_EVENT_READY, NULL);
    }

    /* ────────────────────────────────────────────────────────────────
     *  SECTION: Voice
     * ──────────────────────────────────────────────────────────────── */
    {
        lv_obj_t *sec = make_section(s_scroll, "Voice");

        /* Cloud Mode row */
        lv_obj_t *row_cloud = make_row(sec);
        add_row_label(row_cloud, "Cloud Mode");

        lv_obj_t *cloud_right = lv_obj_create(row_cloud);
        lv_obj_remove_style_all(cloud_right);
        lv_obj_set_size(cloud_right, LV_SIZE_CONTENT, 48);
        lv_obj_set_flex_flow(cloud_right, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(cloud_right, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_gap(cloud_right, 12, 0);

        lv_obj_t *cloud_hint = lv_label_create(cloud_right);
        lv_label_set_text(cloud_hint,
            tab5_settings_get_cloud_mode() ? "On" : "Off");
        lv_obj_set_style_text_color(cloud_hint,
            tab5_settings_get_cloud_mode() ? COL_GREEN : COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(cloud_hint, &lv_font_montserrat_18, 0);

        lv_obj_t *sw_cloud = lv_switch_create(cloud_right);
        lv_obj_set_size(sw_cloud, 60, 36);
        lv_obj_set_style_bg_color(sw_cloud, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw_cloud, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw_cloud, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        if (tab5_settings_get_cloud_mode()) {
            lv_obj_add_state(sw_cloud, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_cloud, cb_cloud_mode, LV_EVENT_VALUE_CHANGED, cloud_hint);

        /* -- Wake Word toggle -- */
        lv_obj_t *wake_row = make_row(sec);
        add_row_label(wake_row, "Wake Word");

        lv_obj_t *wake_right = lv_obj_create(wake_row);
        lv_obj_remove_style_all(wake_right);
        lv_obj_set_size(wake_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(wake_right, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(wake_right, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_gap(wake_right, 12, 0);

        lv_obj_t *wake_hint = lv_label_create(wake_right);
        lv_label_set_text(wake_hint,
            tab5_settings_get_wake_word() ? "On" : "Off");
        lv_obj_set_style_text_color(wake_hint,
            tab5_settings_get_wake_word() ? COL_GREEN : COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(wake_hint, &lv_font_montserrat_18, 0);

        lv_obj_t *sw_wake = lv_switch_create(wake_right);
        lv_obj_set_size(sw_wake, 60, 36);
        lv_obj_set_style_bg_color(sw_wake, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw_wake, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw_wake, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        if (tab5_settings_get_wake_word()) {
            lv_obj_add_state(sw_wake, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_wake, cb_wake_word, LV_EVENT_VALUE_CHANGED, wake_hint);
    }

    /* ────────────────────────────────────────────────────────────────
     *  SECTION: Storage
     * ──────────────────────────────────────────────────────────────── */
    {
        lv_obj_t *sec = make_section(s_scroll, "Storage");

        lv_obj_t *row_sd = make_row(sec);
        add_row_label(row_sd, "SD Card");
        s_lbl_sd_info = add_row_value(row_sd, "Checking...");
    }

    /* ────────────────────────────────────────────────────────────────
     *  SECTION: Battery
     * ──────────────────────────────────────────────────────────────── */
    {
        lv_obj_t *sec = make_section(s_scroll, "Battery");

        /* Voltage row */
        lv_obj_t *row_volt = make_row(sec);
        add_row_label(row_volt, "Voltage");
        s_lbl_bat_volt = add_row_value(row_volt, "-- V");

        /* Status row */
        lv_obj_t *row_stat = make_row(sec);
        add_row_label(row_stat, "Status");
        s_lbl_bat_status = add_row_value(row_stat, "--");

        /* Level row with bar */
        lv_obj_t *row_lvl = make_row(sec);
        add_row_label(row_lvl, "Level");

        /* Right side: bar + percentage label */
        lv_obj_t *lvl_right = lv_obj_create(row_lvl);
        lv_obj_remove_style_all(lvl_right);
        lv_obj_set_size(lvl_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(lvl_right, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(lvl_right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(lvl_right, 8, 0);

        s_bar_bat_level = lv_bar_create(lvl_right);
        lv_obj_set_size(s_bar_bat_level, 180, 16);
        lv_bar_set_range(s_bar_bat_level, 0, 100);
        lv_bar_set_value(s_bar_bat_level, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar_bat_level, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar_bat_level, COL_GREEN, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_bar_bat_level, 4, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bar_bat_level, 4, LV_PART_INDICATOR);

        s_lbl_bat_pct = lv_label_create(lvl_right);
        lv_label_set_text(s_lbl_bat_pct, "0%");
        lv_obj_set_style_text_color(s_lbl_bat_pct, COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(s_lbl_bat_pct, &lv_font_montserrat_18, 0);
    }

    /* ────────────────────────────────────────────────────────────────
     *  SECTION: About
     * ──────────────────────────────────────────────────────────────── */
    {
        lv_obj_t *sec = make_section(s_scroll, "About");

        /* Device */
        lv_obj_t *row_dev = make_row(sec);
        add_row_label(row_dev, "Device");
        add_row_value(row_dev, "M5Stack Tab5");

        /* SoC */
        lv_obj_t *row_soc = make_row(sec);
        add_row_label(row_soc, "SoC");
        add_row_value(row_soc, "ESP32-P4 (RISC-V 2x400MHz)");

        /* Firmware */
        lv_obj_t *row_fw = make_row(sec);
        add_row_label(row_fw, "Firmware");
        add_row_value(row_fw, "TinkerTab v1.0.0");

        /* Free Heap */
        lv_obj_t *row_heap = make_row(sec);
        add_row_label(row_heap, "Free Heap");
        s_lbl_heap = add_row_value(row_heap, "-- KB");

        /* Free PSRAM */
        lv_obj_t *row_psram = make_row(sec);
        add_row_label(row_psram, "Free PSRAM");
        s_lbl_psram = add_row_value(row_psram, "-- MB");
    }

    /* ── Initial data refresh + periodic timer ────────────────────── */
    ui_settings_update();
    s_refresh_timer = lv_timer_create(settings_refresh_cb, 2000, NULL);

    s_destroying = false;

    /* ── Load the screen ────────────────────────────────────────────── */
    lv_screen_load(s_screen);

    ESP_LOGI(TAG, "Settings screen created");
    return s_screen;
}

void ui_settings_update(void)
{
    if (!s_screen) return;

    /* WiFi status */
    if (s_lbl_wifi) {
        {
            extern bool tab5_wifi_connected(void);
            if (tab5_wifi_connected()) {
                lv_label_set_text(s_lbl_wifi, "Connected");
                lv_obj_set_style_text_color(s_lbl_wifi, COL_GREEN, 0);
            } else {
                lv_label_set_text(s_lbl_wifi, "Not connected");
                lv_obj_set_style_text_color(s_lbl_wifi, COL_RED, 0);
            }
        }
    }

    /* SD card info */
    if (s_lbl_sd_info) {
        if (tab5_sdcard_mounted()) {
            uint64_t total = tab5_sdcard_total_bytes();
            uint64_t free_b = tab5_sdcard_free_bytes();
            char sd_buf[64];
            snprintf(sd_buf, sizeof(sd_buf), "%.1f / %.1f GB free",
                     free_b / 1073741824.0, total / 1073741824.0);
            lv_label_set_text(s_lbl_sd_info, sd_buf);
            lv_obj_set_style_text_color(s_lbl_sd_info, COL_GREEN, 0);
        } else {
            lv_label_set_text(s_lbl_sd_info, "Not mounted");
            lv_obj_set_style_text_color(s_lbl_sd_info, COL_TEXT_DIM, 0);
        }
    }

    /* Battery */
    {
        tab5_battery_info_t bi;
        if (tab5_battery_read(&bi) == ESP_OK) {
            if (s_lbl_bat_volt) {
                char vbuf[32];
                snprintf(vbuf, sizeof(vbuf), "%.2f V", bi.voltage);
                lv_label_set_text(s_lbl_bat_volt, vbuf);
            }
            if (s_lbl_bat_status) {
                lv_label_set_text(s_lbl_bat_status, bi.charging ? "Charging" : "Discharging");
                lv_obj_set_style_text_color(s_lbl_bat_status,
                    bi.charging ? COL_GREEN : COL_TEXT_DIM, 0);
            }
            if (s_bar_bat_level) {
                lv_bar_set_value(s_bar_bat_level, bi.percent, LV_ANIM_ON);
                /* Color the bar based on level */
                lv_color_t bar_col = bi.percent > 20 ? COL_GREEN :
                                     bi.percent > 10 ? COL_YELLOW : COL_RED;
                lv_obj_set_style_bg_color(s_bar_bat_level, bar_col, LV_PART_INDICATOR);
            }
            if (s_lbl_bat_pct) {
                char pbuf[8];
                snprintf(pbuf, sizeof(pbuf), "%d%%", bi.percent);
                lv_label_set_text(s_lbl_bat_pct, pbuf);
            }
        }
    }

    /* Heap / PSRAM */
    if (s_lbl_heap) {
        char hbuf[32];
        snprintf(hbuf, sizeof(hbuf), "%lu KB",
                 (unsigned long)(esp_get_free_heap_size() / 1024));
        lv_label_set_text(s_lbl_heap, hbuf);
    }
    if (s_lbl_psram) {
        char pbuf[32];
        uint32_t psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (psram > 1048576) {
            snprintf(pbuf, sizeof(pbuf), "%.1f MB", psram / 1048576.0f);
        } else {
            snprintf(pbuf, sizeof(pbuf), "%lu KB", (unsigned long)(psram / 1024));
        }
        lv_label_set_text(s_lbl_psram, pbuf);
    }
}

void ui_settings_destroy(void)
{
    if (!s_screen) return;

    s_destroying = true;

    if (s_refresh_timer) { lv_timer_delete(s_refresh_timer); s_refresh_timer = NULL; }

    lv_obj_delete(s_screen);

    s_screen        = NULL;
    s_scroll        = NULL;
    s_lbl_wifi      = NULL;
    s_lbl_bat_volt  = NULL;
    s_lbl_bat_status = NULL;
    s_bar_bat_level = NULL;
    s_lbl_bat_pct   = NULL;
    s_lbl_sd_info   = NULL;
    s_lbl_heap      = NULL;
    s_lbl_psram     = NULL;
    s_lbl_orient    = NULL;
    s_ntp_spinner   = NULL;
    s_ntp_btn_label = NULL;
    s_slider_bright = NULL;
    s_slider_volume = NULL;
    s_sw_autorot    = NULL;

    ESP_LOGI(TAG, "Settings screen destroyed");
}
