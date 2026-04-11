#include "esp_task_wdt.h"
#include "ui_core.h"

static inline void feed_wdt(void) {
    esp_task_wdt_reset();
}
/**
 * TinkerTab — Settings Screen (Low-Object Version)
 *
 * Dark-themed settings UI for M5Stack Tab5 (720x1280 portrait).
 * LVGL v9 API, accent color #3B82F6.
 *
 * MINIMAL OBJECTS: No row containers. All labels and controls placed
 * directly on s_scroll with manual (x, y) positioning.
 * Target: under 40 LVGL objects total.
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
#include "ota.h"
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
#define COL_TEXT        lv_color_hex(0xE2E8F0)
#define COL_TEXT_DIM    lv_color_hex(0x94A3B8)
#define COL_GREEN       lv_color_hex(0x22C55E)
#define COL_RED         lv_color_hex(0xEF4444)
#define COL_YELLOW      lv_color_hex(0xEAB308)

/* ── Layout constants ───────────────────────────────────────────────── */
#define TOPBAR_H        48
#define SIDE_PAD        16
#define LEFT_X          16
#define RIGHT_X         380
#define ROW_STEP        44
#define SECTION_GAP     12
#define HDR_STEP        36

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

/* OTA update button label (updated during check) */
static lv_obj_t *s_ota_btn_label  = NULL;
static lv_obj_t *s_ota_apply_btn  = NULL;
static char      s_ota_url[256]   = {0};

/* Brightness slider tracks the current value */
static lv_obj_t *s_slider_bright  = NULL;

/* Volume slider */
static lv_obj_t *s_slider_volume  = NULL;

/* Auto-rotate switch */
static lv_obj_t *s_sw_autorot     = NULL;

/* Guard flag for background tasks during destroy */
static volatile bool s_destroying = false;
static lv_timer_t *s_refresh_timer = NULL;

static void settings_refresh_cb(lv_timer_t *t) {
    (void)t;
    feed_wdt();
    if (s_destroying) return;
    if (s_screen && lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN)) return;
    ui_settings_update();
}

/* ── Forward declarations ───────────────────────────────────────────── */
static void ntp_sync_task(void *arg);

/* ── Event callbacks ────────────────────────────────────────────────── */

static void cb_back_btn(lv_event_t *e)
{
    /* L5: Support both click and swipe-right gesture */
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;
    }
    s_destroying = true;
    if (s_refresh_timer) { lv_timer_pause(s_refresh_timer); }

    /* Settings is an overlay on the home screen — just hide it */
    if (s_screen) {
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    }
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

/* Three-tier voice mode + LLM model picker */
static lv_obj_t *s_dd_voice_mode = NULL;
static lv_obj_t *s_dd_llm_model = NULL;

/* LLM model IDs matching dropdown order */
static const char *s_llm_model_ids[] = {
    "",                                    /* Local NPU — empty = use local */
    "",                                    /* Local Ollama — empty = use local */
    "anthropic/claude-3-haiku",
    "anthropic/claude-sonnet-4-20250514",
    "openai/gpt-4o-mini",
};

static void send_voice_config(void)
{
    uint8_t mode = tab5_settings_get_voice_mode();
    char model[64] = {0};
    tab5_settings_get_llm_model(model, sizeof(model));
    ESP_LOGI(TAG, "Sending voice config: mode=%d model=%s", mode, model);

    if (!voice_is_connected()) {
        ESP_LOGW(TAG, "Voice not connected — config will apply on reconnect");
        return;
    }

    /* Send full three-tier config update to Dragon */
    esp_err_t err = voice_send_config_update((int)mode, model);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send voice config: %s", esp_err_to_name(err));
    }
}

static void cb_voice_mode(lv_event_t *e)
{
    (void)e;
    if (!s_dd_voice_mode) return;
    uint8_t mode = lv_dropdown_get_selected(s_dd_voice_mode);
    tab5_settings_set_voice_mode(mode);
    ESP_LOGI(TAG, "Voice mode: %d (%s)", mode, mode == 0 ? "local" : mode == 1 ? "hybrid" : "cloud");

    /* Enable/disable model dropdown based on mode */
    if (s_dd_llm_model) {
        if (mode == 2) {
            lv_obj_clear_state(s_dd_llm_model, LV_STATE_DISABLED);
            lv_obj_set_style_opa(s_dd_llm_model, LV_OPA_COVER, 0);
        } else {
            lv_obj_add_state(s_dd_llm_model, LV_STATE_DISABLED);
            lv_obj_set_style_opa(s_dd_llm_model, LV_OPA_40, 0);
        }
    }

    send_voice_config();
}

static void cb_llm_model(lv_event_t *e)
{
    (void)e;
    if (!s_dd_llm_model) return;
    uint32_t sel = lv_dropdown_get_selected(s_dd_llm_model);
    if (sel < sizeof(s_llm_model_ids)/sizeof(s_llm_model_ids[0])) {
        tab5_settings_set_llm_model(s_llm_model_ids[sel]);
        ESP_LOGI(TAG, "LLM model: %s", s_llm_model_ids[sel]);
        send_voice_config();
    }
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

/* F2: OTA apply background task — downloads firmware, reboots */
static void ota_apply_task(void *arg)
{
    ESP_LOGI(TAG, "OTA apply: downloading from %s", s_ota_url);
    esp_err_t err = tab5_ota_apply(s_ota_url);  /* reboots on success */
    /* If we get here, it failed */
    ESP_LOGE(TAG, "OTA apply failed: %s", esp_err_to_name(err));
    if (s_destroying) { vTaskDelete(NULL); return; }
    tab5_ui_lock();
    if (s_ota_btn_label) lv_label_set_text(s_ota_btn_label, "Update failed!");
    if (s_ota_apply_btn) lv_obj_clear_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    tab5_ui_unlock();
    vTaskDelete(NULL);
}

static void cb_ota_apply(lv_event_t *e)
{
    (void)e;
    if (!s_ota_url[0]) return;
    if (s_ota_btn_label) lv_label_set_text(s_ota_btn_label, "Updating...");
    if (s_ota_apply_btn) lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    xTaskCreate(ota_apply_task, "ota_apply", 8192, NULL, 5, NULL);
}

/* OTA check background task */
static void ota_check_task(void *arg)
{
    tab5_ota_info_t info;
    esp_err_t err = tab5_ota_check(&info);

    if (s_destroying) { vTaskDelete(NULL); return; }

    tab5_ui_lock();
    if (s_destroying || !s_ota_btn_label) { tab5_ui_unlock(); vTaskDelete(NULL); return; }

    if (err != ESP_OK) {
        lv_label_set_text(s_ota_btn_label, "Check failed");
        if (s_ota_apply_btn) lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (info.available) {
        char buf[64];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK " v%s available!", info.version);
        lv_label_set_text(s_ota_btn_label, buf);
        /* F2: Store URL and show Apply button */
        snprintf(s_ota_url, sizeof(s_ota_url), "%s", info.url);
        if (s_ota_apply_btn) lv_obj_clear_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_ota_btn_label, LV_SYMBOL_OK " Up to date");
        if (s_ota_apply_btn) lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    }
    tab5_ui_unlock();
    vTaskDelete(NULL);
}

static void cb_ota_check(lv_event_t *e)
{
    (void)e;
    if (s_ota_btn_label) {
        lv_label_set_text(s_ota_btn_label, "Checking...");
    }
    if (s_ota_apply_btn) lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    xTaskCreate(ota_check_task, "ota_check", 8192, NULL, 5, NULL);
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
        /* F3: Disconnect voice so reconnect watchdog picks up new host */
        if (voice_is_connected()) {
            ESP_LOGI(TAG, "Disconnecting voice for host change -> watchdog will reconnect");
            voice_disconnect();
        }
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

/* ══════════════════════════════════════════════════════════════════════
 *  Inline helpers for minimal-object layout
 * ══════════════════════════════════════════════════════════════════════ */

/** Create a section header label (accent colored, font 20). Returns label. */
static lv_obj_t *mk_hdr(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_ACCENT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

/** Create a key label (left side, font 18, COL_TEXT). */
static lv_obj_t *mk_key(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

/** Create a value label (right side, font 18, COL_TEXT_DIM). */
static lv_obj_t *mk_val(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *ui_settings_create(void)
{
    if (s_screen) {
        /* Overlay already exists — just unhide and resume refresh */
        ESP_LOGI(TAG, "Settings screen resumed");
        s_destroying = false;
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(s_screen);
        if (s_refresh_timer) lv_timer_resume(s_refresh_timer);
        ui_settings_update();
        return s_screen;
    }
    ESP_LOGI(TAG, "Creating settings screen...");
    /* Temporarily remove the UI task from WDT — creation takes 1-5s with manual layout. */
    TaskHandle_t ui_task = xTaskGetHandle("ui_task");
    if (ui_task) esp_task_wdt_delete(ui_task);

    /* ── Fullscreen overlay on home screen (NOT a separate screen) ── */
    /* Using a separate lv_screen causes crashes on screen transitions
     * because the DPI+PSRAM draw pipeline can't handle two screens. */
    lv_obj_t *home = ui_home_get_screen();
    s_screen = lv_obj_create(home);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, 720, 1280);
    lv_obj_set_pos(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_screen);

    /* L5: Swipe-right to go back */
    lv_obj_add_event_cb(s_screen, cb_back_btn, LV_EVENT_GESTURE, NULL);

    /* ── Top bar — minimal: just 1 container + back button + title ── */
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_TOPBAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t *btn = lv_button_create(bar);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 48, TOPBAR_H);
    lv_obj_set_pos(btn, SIDE_PAD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn, cb_back_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(arrow, COL_TEXT, 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_24, 0);
    lv_obj_center(arrow);

    /* Title — centered in topbar */
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* ── Scrollable body (NO flex) ──────────────────────────────────── */
    s_scroll = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, 720, 1280 - TOPBAR_H);
    lv_obj_set_pos(s_scroll, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_scroll, 0, 0);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);

    /* Hide during creation — prevents rendering on every child add */
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_HIDDEN);

    /* Running Y position inside s_scroll */
    int y = 8;

    feed_wdt();
    ESP_LOGI(TAG, "Section: Display");

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: Display
     * ════════════════════════════════════════════════════════════════ */
    mk_hdr(s_scroll, "Display", LEFT_X, y);
    y += HDR_STEP;

    /* Brightness */
    mk_key(s_scroll, "Brightness", LEFT_X, y + 4);
    s_slider_bright = lv_slider_create(s_scroll);
    lv_obj_set_pos(s_slider_bright, RIGHT_X, y);
    lv_obj_set_size(s_slider_bright, 280, 30);
    lv_slider_set_range(s_slider_bright, 0, 100);
    lv_slider_set_value(s_slider_bright, tab5_settings_get_brightness(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider_bright, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_bright, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_bright, COL_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_bright, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider_bright, cb_brightness, LV_EVENT_VALUE_CHANGED, NULL);
    y += ROW_STEP;

    /* Volume */
    mk_key(s_scroll, "Volume", LEFT_X, y + 4);
    s_slider_volume = lv_slider_create(s_scroll);
    lv_obj_set_pos(s_slider_volume, RIGHT_X, y);
    lv_obj_set_size(s_slider_volume, 280, 30);
    lv_slider_set_range(s_slider_volume, 0, 100);
    lv_slider_set_value(s_slider_volume, tab5_settings_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider_volume, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_volume, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_volume, COL_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_volume, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider_volume, cb_volume, LV_EVENT_VALUE_CHANGED, NULL);
    y += ROW_STEP;

    /* Auto-rotate */
    mk_key(s_scroll, "Auto-rotate", LEFT_X, y + 6);
    s_lbl_orient = mk_val(s_scroll, "Off", RIGHT_X, y + 6);
    s_sw_autorot = lv_switch_create(s_scroll);
    lv_obj_set_pos(s_sw_autorot, 600, y);
    lv_obj_set_size(s_sw_autorot, 60, 36);
    lv_obj_set_style_bg_color(s_sw_autorot, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sw_autorot, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_sw_autorot, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_event_cb(s_sw_autorot, cb_autorotate, LV_EVENT_VALUE_CHANGED, NULL);
    y += ROW_STEP + SECTION_GAP;

    feed_wdt();
    ESP_LOGI(TAG, "Section: Network");

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: Network
     * ════════════════════════════════════════════════════════════════ */
    mk_hdr(s_scroll, "Network", LEFT_X, y);
    y += HDR_STEP;

    /* WiFi status */
    mk_key(s_scroll, "WiFi", LEFT_X, y + 4);
    s_lbl_wifi = mk_val(s_scroll, "Checking...", RIGHT_X, y + 4);
    y += ROW_STEP;

    /* WiFi Setup button */
    mk_key(s_scroll, "WiFi Setup", LEFT_X, y + 6);
    {
        lv_obj_t *wifi_btn = lv_button_create(s_scroll);
        lv_obj_set_pos(wifi_btn, RIGHT_X, y);
        lv_obj_set_size(wifi_btn, 180, 38);
        lv_obj_set_style_bg_color(wifi_btn, COL_ACCENT, 0);
        lv_obj_set_style_radius(wifi_btn, 6, 0);
        lv_obj_add_event_cb(wifi_btn, cb_wifi_setup, LV_EVENT_CLICKED, NULL);

        lv_obj_t *wifi_btn_lbl = lv_label_create(wifi_btn);
        lv_label_set_text(wifi_btn_lbl, LV_SYMBOL_WIFI " Configure");
        lv_obj_set_style_text_color(wifi_btn_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(wifi_btn_lbl, &lv_font_montserrat_18, 0);
        lv_obj_center(wifi_btn_lbl);
    }
    y += ROW_STEP;

    /* Bluetooth */
    mk_key(s_scroll, "Bluetooth", LEFT_X, y + 4);
    {
        lv_obj_t *bt_val = mk_val(s_scroll, "ESP-Hosted pending", RIGHT_X, y + 4);
        lv_obj_set_style_text_color(bt_val, COL_YELLOW, 0);
    }
    y += ROW_STEP;

    /* Sync Time button */
    mk_key(s_scroll, "Sync Time", LEFT_X, y + 6);
    {
        lv_obj_t *ntp_btn = lv_button_create(s_scroll);
        lv_obj_set_pos(ntp_btn, RIGHT_X, y);
        lv_obj_set_size(ntp_btn, 140, 38);
        lv_obj_set_style_bg_color(ntp_btn, COL_ACCENT, 0);
        lv_obj_set_style_radius(ntp_btn, 6, 0);
        lv_obj_add_event_cb(ntp_btn, cb_ntp_sync, LV_EVENT_CLICKED, NULL);

        s_ntp_btn_label = lv_label_create(ntp_btn);
        lv_label_set_text(s_ntp_btn_label, "Sync NTP");
        lv_obj_set_style_text_color(s_ntp_btn_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_ntp_btn_label, &lv_font_montserrat_18, 0);
        lv_obj_center(s_ntp_btn_label);
    }
    y += ROW_STEP;

    /* Dragon host input */
    mk_key(s_scroll, "Dragon Host", LEFT_X, y + 6);
    s_dragon_ta = lv_textarea_create(s_scroll);
    lv_obj_set_pos(s_dragon_ta, RIGHT_X, y);
    lv_obj_set_size(s_dragon_ta, 280, 38);
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
    {
        char dhost[64];
        tab5_settings_get_dragon_host(dhost, sizeof(dhost));
        if (dhost[0]) {
            lv_textarea_set_text(s_dragon_ta, dhost);
        }
    }
    lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_done, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_done, LV_EVENT_READY, NULL);
    y += ROW_STEP + SECTION_GAP;

    feed_wdt();
    ESP_LOGI(TAG, "Section: Voice");

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: Voice
     * ════════════════════════════════════════════════════════════════ */
    mk_hdr(s_scroll, "Voice", LEFT_X, y);
    y += HDR_STEP;

    /* Voice Mode dropdown */
    mk_key(s_scroll, "Mode", LEFT_X, y + 6);
    s_dd_voice_mode = lv_dropdown_create(s_scroll);
    lv_obj_set_pos(s_dd_voice_mode, RIGHT_X, y);
    lv_obj_set_size(s_dd_voice_mode, 200, 38);
    lv_dropdown_set_options(s_dd_voice_mode, "Local\nHybrid\nFull Cloud");
    lv_obj_set_style_bg_color(s_dd_voice_mode, lv_color_hex(0x1E1E2E), 0);
    lv_obj_set_style_text_color(s_dd_voice_mode, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_dd_voice_mode, &lv_font_montserrat_18, 0);
    lv_obj_set_style_border_width(s_dd_voice_mode, 1, 0);
    lv_obj_set_style_border_color(s_dd_voice_mode, COL_ACCENT, 0);
    lv_obj_set_style_radius(s_dd_voice_mode, 6, 0);
    lv_dropdown_set_selected(s_dd_voice_mode, tab5_settings_get_voice_mode());
    lv_obj_add_event_cb(s_dd_voice_mode, cb_voice_mode, LV_EVENT_VALUE_CHANGED, NULL);
    y += ROW_STEP;

    /* AI Model dropdown */
    mk_key(s_scroll, "AI Model", LEFT_X, y + 6);
    s_dd_llm_model = lv_dropdown_create(s_scroll);
    lv_obj_set_pos(s_dd_llm_model, RIGHT_X, y);
    lv_obj_set_size(s_dd_llm_model, 240, 38);
    lv_dropdown_set_options(s_dd_llm_model,
        "Local NPU\nLocal Ollama\nClaude Haiku\nClaude Sonnet\nGPT-4o mini");
    lv_obj_set_style_bg_color(s_dd_llm_model, lv_color_hex(0x1E1E2E), 0);
    lv_obj_set_style_text_color(s_dd_llm_model, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_dd_llm_model, &lv_font_montserrat_18, 0);
    lv_obj_set_style_border_width(s_dd_llm_model, 1, 0);
    lv_obj_set_style_border_color(s_dd_llm_model, COL_ACCENT, 0);
    lv_obj_set_style_radius(s_dd_llm_model, 6, 0);
    {
        char saved_model[64] = {0};
        tab5_settings_get_llm_model(saved_model, sizeof(saved_model));
        for (int i = 0; i < 5; i++) {
            if (s_llm_model_ids[i][0] && strcmp(s_llm_model_ids[i], saved_model) == 0) {
                lv_dropdown_set_selected(s_dd_llm_model, i);
                break;
            }
        }
    }
    lv_obj_add_event_cb(s_dd_llm_model, cb_llm_model, LV_EVENT_VALUE_CHANGED, NULL);
    if (tab5_settings_get_voice_mode() != 2) {
        lv_obj_add_state(s_dd_llm_model, LV_STATE_DISABLED);
        lv_obj_set_style_opa(s_dd_llm_model, LV_OPA_40, 0);
    }
    y += ROW_STEP;

    /* Wake Word toggle */
    mk_key(s_scroll, "Wake Word", LEFT_X, y + 6);
    lv_obj_t *wake_hint = mk_val(s_scroll,
        tab5_settings_get_wake_word() ? "On" : "Off", RIGHT_X, y + 6);
    lv_obj_set_style_text_color(wake_hint,
        tab5_settings_get_wake_word() ? COL_GREEN : COL_TEXT_DIM, 0);
    {
        lv_obj_t *sw_wake = lv_switch_create(s_scroll);
        lv_obj_set_pos(sw_wake, 600, y);
        lv_obj_set_size(sw_wake, 60, 36);
        lv_obj_set_style_bg_color(sw_wake, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw_wake, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw_wake, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        if (tab5_settings_get_wake_word()) {
            lv_obj_add_state(sw_wake, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_wake, cb_wake_word, LV_EVENT_VALUE_CHANGED, wake_hint);
    }
    y += ROW_STEP + SECTION_GAP;

    feed_wdt();
    ESP_LOGI(TAG, "Section: Storage");

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: Storage
     * ════════════════════════════════════════════════════════════════ */
    mk_hdr(s_scroll, "Storage", LEFT_X, y);
    y += HDR_STEP;

    mk_key(s_scroll, "SD Card", LEFT_X, y + 4);
    s_lbl_sd_info = mk_val(s_scroll, "Checking...", RIGHT_X, y + 4);
    y += ROW_STEP + SECTION_GAP;

    feed_wdt();
    ESP_LOGI(TAG, "Section: Battery");

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: Battery
     * ════════════════════════════════════════════════════════════════ */
    mk_hdr(s_scroll, "Battery", LEFT_X, y);
    y += HDR_STEP;

    /* Voltage */
    mk_key(s_scroll, "Voltage", LEFT_X, y + 4);
    s_lbl_bat_volt = mk_val(s_scroll, "-- V", RIGHT_X, y + 4);
    y += ROW_STEP;

    /* Status */
    mk_key(s_scroll, "Status", LEFT_X, y + 4);
    s_lbl_bat_status = mk_val(s_scroll, "--", RIGHT_X, y + 4);
    y += ROW_STEP;

    /* Level with bar + percentage */
    mk_key(s_scroll, "Level", LEFT_X, y + 4);
    s_bar_bat_level = lv_bar_create(s_scroll);
    lv_obj_set_pos(s_bar_bat_level, RIGHT_X, y + 6);
    lv_obj_set_size(s_bar_bat_level, 180, 16);
    lv_bar_set_range(s_bar_bat_level, 0, 100);
    lv_bar_set_value(s_bar_bat_level, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_bat_level, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_bat_level, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_bat_level, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_bat_level, 4, LV_PART_INDICATOR);
    s_lbl_bat_pct = mk_val(s_scroll, "0%", RIGHT_X + 200, y + 4);
    y += ROW_STEP + SECTION_GAP;

    feed_wdt();
    ESP_LOGI(TAG, "Section: About");

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: About
     * ════════════════════════════════════════════════════════════════ */
    mk_hdr(s_scroll, "About", LEFT_X, y);
    y += HDR_STEP;

    /* Device */
    mk_key(s_scroll, "Device", LEFT_X, y + 4);
    mk_val(s_scroll, "M5Stack Tab5", RIGHT_X, y + 4);
    y += ROW_STEP;

    /* SoC */
    mk_key(s_scroll, "SoC", LEFT_X, y + 4);
    mk_val(s_scroll, "ESP32-P4 (RISC-V 2x400MHz)", RIGHT_X, y + 4);
    y += ROW_STEP;

    /* Firmware */
    mk_key(s_scroll, "Firmware", LEFT_X, y + 4);
    {
        char fw_str[48];
        snprintf(fw_str, sizeof(fw_str), "TinkerTab v%s (%s)", TAB5_FIRMWARE_VER, tab5_ota_current_partition());
        mk_val(s_scroll, fw_str, RIGHT_X, y + 4);
    }
    y += ROW_STEP;

    /* OTA Update button */
    mk_key(s_scroll, "Updates", LEFT_X, y + 6);
    {
        lv_obj_t *ota_btn = lv_button_create(s_scroll);
        lv_obj_set_pos(ota_btn, RIGHT_X, y);
        lv_obj_set_size(ota_btn, 220, 38);
        lv_obj_set_style_bg_color(ota_btn, COL_ACCENT, 0);
        lv_obj_set_style_radius(ota_btn, 6, 0);
        lv_obj_add_event_cb(ota_btn, cb_ota_check, LV_EVENT_CLICKED, NULL);

        s_ota_btn_label = lv_label_create(ota_btn);
        lv_label_set_text(s_ota_btn_label, LV_SYMBOL_DOWNLOAD " Check Update");
        lv_obj_set_style_text_color(s_ota_btn_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_ota_btn_label, &lv_font_montserrat_16, 0);
        lv_obj_center(s_ota_btn_label);
    }
    y += ROW_STEP;

    /* OTA Apply button (hidden until update found) */
    s_ota_apply_btn = lv_button_create(s_scroll);
    lv_obj_set_pos(s_ota_apply_btn, RIGHT_X, y);
    lv_obj_set_size(s_ota_apply_btn, 220, 38);
    lv_obj_set_style_bg_color(s_ota_apply_btn, COL_GREEN, 0);
    lv_obj_set_style_radius(s_ota_apply_btn, 6, 0);
    lv_obj_add_event_cb(s_ota_apply_btn, cb_ota_apply, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *apply_lbl = lv_label_create(s_ota_apply_btn);
        lv_label_set_text(apply_lbl, LV_SYMBOL_OK " Apply Update");
        lv_obj_set_style_text_color(apply_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(apply_lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(apply_lbl);
    }
    lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    y += ROW_STEP;

    /* Free Heap */
    mk_key(s_scroll, "Free Heap", LEFT_X, y + 4);
    s_lbl_heap = mk_val(s_scroll, "-- KB", RIGHT_X, y + 4);
    y += ROW_STEP;

    /* Free PSRAM */
    mk_key(s_scroll, "Free PSRAM", LEFT_X, y + 4);
    s_lbl_psram = mk_val(s_scroll, "-- MB", RIGHT_X, y + 4);
    y += ROW_STEP;

    /* ── Set scroll content size based on total Y ──────────────────── */
    y += 32; /* bottom padding */
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);

    s_destroying = false;
    ESP_LOGI(TAG, "Widgets done, unhiding scroll...");
    feed_wdt();

    /* Unhide — triggers a single layout pass for all widgets at once */
    lv_obj_clear_flag(s_scroll, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Loading screen...");
    feed_wdt();

    /* Overlay is now visible (created on home screen, moved to foreground) */
    ESP_LOGI(TAG, "Settings overlay ready");

    s_refresh_timer = lv_timer_create(settings_refresh_cb, 2000, NULL);
    lv_timer_t *init_timer = lv_timer_create(settings_refresh_cb, 500, NULL);
    lv_timer_set_auto_delete(init_timer, true);

    ESP_LOGI(TAG, "Settings screen created");

    /* Re-subscribe UI task to WDT */
    if (ui_task) esp_task_wdt_add(ui_task);
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
    s_dd_voice_mode = NULL;
    s_dd_llm_model  = NULL;
    s_dragon_ta     = NULL;
    s_ota_btn_label = NULL;
    s_ota_apply_btn = NULL;

    ESP_LOGI(TAG, "Settings screen destroyed");
}

lv_obj_t *ui_settings_get_screen(void) { return s_screen; }

void ui_settings_hide(void)
{
    /* Destroy the entire settings overlay to free memory and ensure
     * no stale pixels remain. Next open re-creates from scratch. */
    ui_settings_destroy();
}
