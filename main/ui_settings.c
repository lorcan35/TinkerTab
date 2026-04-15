/**
 * TinkerTab — Settings Screen (Material Dark Design)
 *
 * Fullscreen overlay on home screen for M5Stack Tab5 (720x1280 portrait).
 * LVGL v9.2.2, manual Y positioning, max 55 objects (128KB pool).
 *
 * Sections: Display, Network, Voice Mode, Storage, Battery, About
 * Each section has a unique accent color. Spacing used instead of dividers.
 */

#include "esp_task_wdt.h"
#include "ui_core.h"
#include "ui_settings.h"
#include "ui_home.h"
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

static inline void feed_wdt(void) {
    esp_task_wdt_reset();
    /* Yield to let LWIP TCP/IP task process packets on Core 0.
     * Settings creates 55 LVGL objects in one lv_async_call callback,
     * monopolizing Core 0. LWIP needs Core 0 cycles to deliver HTTP
     * requests to the debug server (running on Core 1).
     * 20ms per yield × 7 sections = 140ms total overhead — acceptable. */
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ── Material Dark Constants ──────────────────────────────────────────── */
#define BG_COLOR     0x0A0A0F
#define CARD_COLOR   0x1A1A2E
#define TEXT_PRIMARY 0xFFFFFF
#define TEXT_SUBTLE  0x555555
#define TEXT_DIM     0x888888
#define SIDE_PAD     20
#define RIGHT_X      380
#define ROW_H        44
#define HDR_H        28
#define TOPBAR_H     48
#define OVERLAY_H    1280
#define NAV_BAR_H    80
#define USABLE_H     (OVERLAY_H - NAV_BAR_H)  /* Full screen — nav bar on lv_layer_top() */
#define CONTENT_W    680

/* Section accent colors */
#define ACC_DISPLAY  0xF5A623
#define ACC_NETWORK  0x00E5FF
#define ACC_VOICE    0xA855F7
#define ACC_STORAGE  0xF59E0B
#define ACC_BATTERY  0xEF4444
#define ACC_ABOUT    0x8B5CF6

/* Voice tab colors */
#define TAB_LOCAL    0x22C55E
#define TAB_HYBRID   0xEAB308
#define TAB_CLOUD    0x3B82F6
#define TAB_TINKERCLAW 0xE11D48

/* ── Screen-lifetime state ──────────────────────────────────────────── */
static lv_obj_t *s_screen          = NULL;
static lv_obj_t *s_scroll          = NULL;

/* Live-updated labels */
static lv_obj_t *s_lbl_wifi       = NULL;
static lv_obj_t *s_lbl_bat_status = NULL;   /* primary: "XX% • Charging" or "USB Powered" */
static lv_obj_t *s_lbl_bat_volt   = NULL;  /* debug: small dim voltage */
static lv_obj_t *s_bar_bat_level  = NULL;
static lv_obj_t *s_lbl_bat_pct    = NULL;
static lv_obj_t *s_lbl_sd_info    = NULL;
static lv_obj_t *s_lbl_heap       = NULL;
static lv_obj_t *s_lbl_orient     = NULL;

/* NTP sync */
static lv_obj_t *s_ntp_spinner    = NULL;
static lv_obj_t *s_ntp_btn_label  = NULL;

/* Dragon host text input */
static lv_obj_t *s_dragon_ta      = NULL;

/* OTA */
static lv_obj_t *s_ota_btn_label  = NULL;
static lv_obj_t *s_ota_apply_btn  = NULL;
static char      s_ota_url[256]   = {0};
static char      s_ota_sha256[65] = {0};

/* Sliders */
static lv_obj_t *s_slider_bright  = NULL;
static lv_obj_t *s_slider_volume  = NULL;

/* Slider value labels */
static lv_obj_t *s_lbl_bright_val = NULL;
static lv_obj_t *s_lbl_vol_val    = NULL;

/* Auto-rotate switch */
static lv_obj_t *s_sw_autorot     = NULL;

/* Guard flag for background tasks during destroy */
static volatile bool s_destroying = false;
/* Guard flag to prevent double-creation from overlapping navigate calls */
static volatile bool s_creating = false;
static lv_timer_t *s_refresh_timer = NULL;

/* Two-pass creation: Phase 2 deferred via lv_timer_create */
static volatile bool s_phase2_pending = false;
static lv_timer_t   *s_phase2_timer  = NULL;
static int            s_phase2_y     = 0;       /* Y position carried from Phase 1 */

/* NVS write debounce timers — prevent flash wear from slider drag (US-HW17) */
static lv_timer_t *s_bright_save_timer = NULL;
static lv_timer_t *s_vol_save_timer    = NULL;

/* Voice tab system */
static lv_obj_t *s_tab_local      = NULL;
static lv_obj_t *s_tab_hybrid     = NULL;
static lv_obj_t *s_tab_cloud      = NULL;
static lv_obj_t *s_tab_tinkerclaw = NULL;
static lv_obj_t *s_local_card     = NULL;
static lv_obj_t *s_hybrid_card    = NULL;
static lv_obj_t *s_cloud_card     = NULL;
static lv_obj_t *s_local_content[4]  = {NULL};
static lv_obj_t *s_hybrid_content[4] = {NULL};
static lv_obj_t *s_cloud_content[4]  = {NULL};
static lv_obj_t *s_tinkerclaw_card   = NULL;
static lv_obj_t *s_tinkerclaw_content[4] = {NULL};
static uint8_t   s_active_tab     = 0;

/* Cloud model IDs matching dropdown order */
static const char *s_cloud_model_ids[] = {
    "anthropic/claude-3.5-haiku",
    "anthropic/claude-sonnet-4-20250514",
    "openai/gpt-4o-mini",
    "openai/gpt-4o",
};

/* Local model names matching dropdown order */
static const char *s_local_model_names[] = {
    "qwen3:1.7b",
    "qwen3:4b",
    "qwen3:0.6b",
};

/* ══════════════════════════════════════════════════════════════════════
 *  Material Dark Helper Functions
 * ══════════════════════════════════════════════════════════════════════ */

/** Section header with accent-colored label.
 *  Returns the Y position after the header (y + HDR_H). */
static int mk_section(lv_obj_t *parent, const char *text, lv_color_t accent, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, accent, 0);
    lv_obj_set_style_text_font(lbl, FONT_CAPTION, 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    lv_obj_set_pos(lbl, SIDE_PAD, y);

    return y + HDR_H;
}

/** Row with label (single line, vertically centered in ROW_H). */
static void mk_row_label(lv_obj_t *parent, const char *label, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
    lv_obj_set_pos(lbl, SIDE_PAD, y + (ROW_H - 20) / 2);
}

/** Right-aligned value text. Returns the label object. */
static lv_obj_t *mk_row_value(lv_obj_t *parent, const char *text, lv_color_t color, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
    lv_obj_set_pos(lbl, RIGHT_X, y + (ROW_H - 20) / 2);
    return lbl;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Refresh timer
 * ══════════════════════════════════════════════════════════════════════ */

static void settings_refresh_cb(lv_timer_t *t) {
    (void)t;
    feed_wdt();
    if (s_destroying) return;
    if (s_screen && lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN)) return;
    ui_settings_update();
}

/* ── Forward declarations ───────────────────────────────────────────── */
static void ntp_sync_task(void *arg);
void ui_settings_hide(void);  /* needed by cb_back_btn before definition */

/* ══════════════════════════════════════════════════════════════════════
 *  Event Callbacks
 * ══════════════════════════════════════════════════════════════════════ */

static void cb_back_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;
    }
    /* Hide instead of destroy — destroy+recreate of 55 objects causes
     * internal SRAM heap fragmentation from LVGL alloc/free churn.
     * The overlay stays allocated (hidden) and is re-shown on next open.
     * ~11KB from LVGL expand pool (PSRAM), zero internal SRAM impact. */
    ui_settings_hide();
    ui_home_go_home();
}

/* ── NVS debounce timer callbacks (US-HW17) ───────────────────────────
 * Slider drag fires VALUE_CHANGED 10-30x/sec.  Hardware updates are
 * instant, but NVS writes need flash sector erases.  We defer the NVS
 * commit until 500ms after the last drag event (one-shot timer).       */

static void brightness_save_cb(lv_timer_t *t)
{
    int val = (int)(intptr_t)lv_timer_get_user_data(t);
    tab5_settings_set_brightness((uint8_t)val);
    ESP_LOGI(TAG, "Brightness %d%% saved to NVS (debounced)", val);
    s_bright_save_timer = NULL;
}

static void volume_save_cb(lv_timer_t *t)
{
    int val = (int)(intptr_t)lv_timer_get_user_data(t);
    tab5_settings_set_volume((uint8_t)val);
    ESP_LOGI(TAG, "Volume %d%% saved to NVS (debounced)", val);
    s_vol_save_timer = NULL;
}

static void cb_brightness(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    tab5_display_set_brightness(val);                       /* instant HW */
    if (s_lbl_bright_val) lv_label_set_text_fmt(s_lbl_bright_val, "%d%%", val);

    /* Restart 500ms debounce for NVS write */
    if (s_bright_save_timer) lv_timer_delete(s_bright_save_timer);
    s_bright_save_timer = lv_timer_create(brightness_save_cb, 500,
                                          (void *)(intptr_t)val);
    lv_timer_set_repeat_count(s_bright_save_timer, 1);
}

static void cb_volume(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    tab5_audio_set_volume((uint8_t)val);                    /* instant HW */
    if (s_lbl_vol_val) lv_label_set_text_fmt(s_lbl_vol_val, "%d%%", val);

    /* Restart 500ms debounce for NVS write */
    if (s_vol_save_timer) lv_timer_delete(s_vol_save_timer);
    s_vol_save_timer = lv_timer_create(volume_save_cb, 500,
                                       (void *)(intptr_t)val);
    lv_timer_set_repeat_count(s_vol_save_timer, 1);
}

/* Play a short preview tone on slider release so user hears the new volume (US-PR12) */
static void cb_volume_released(lv_event_t *e)
{
    (void)e;
    tab5_audio_test_tone(440, 200);  /* 440Hz A4 for 200ms */
}

static void cb_autorotate(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Auto-rotate %s", on ? "enabled" : "disabled");
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

/* ── Voice mode logic ───────────────────────────────────────────────── */

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

    esp_err_t err = voice_send_config_update((int)mode, model);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send voice config: %s", esp_err_to_name(err));
    }
}

static void voice_tab_switch(uint8_t new_tab)
{
    if (new_tab == s_active_tab) return;

    lv_obj_t *tabs[] = { s_tab_local, s_tab_hybrid, s_tab_cloud, s_tab_tinkerclaw };
    lv_obj_t *cards[] = { s_local_card, s_hybrid_card, s_cloud_card, s_tinkerclaw_card };

    /* Style old tab as inactive */
    if (tabs[s_active_tab]) {
        lv_obj_set_style_bg_color(tabs[s_active_tab], lv_color_hex(CARD_COLOR), 0);
        lv_obj_set_style_text_color(tabs[s_active_tab], lv_color_hex(0x666666), 0);
    }
    /* Hide old card */
    if (cards[s_active_tab])
        lv_obj_add_flag(cards[s_active_tab], LV_OBJ_FLAG_HIDDEN);

    /* Style new tab as active */
    uint32_t tab_colors[] = { TAB_LOCAL, TAB_HYBRID, TAB_CLOUD, TAB_TINKERCLAW };
    if (tabs[new_tab]) {
        lv_obj_set_style_bg_color(tabs[new_tab], lv_color_hex(tab_colors[new_tab]), 0);
        lv_obj_set_style_text_color(tabs[new_tab], lv_color_hex(0x000000), 0);
    }
    /* Show new card */
    if (cards[new_tab])
        lv_obj_clear_flag(cards[new_tab], LV_OBJ_FLAG_HIDDEN);

    s_active_tab = new_tab;

    tab5_settings_set_voice_mode(new_tab);
    ESP_LOGI(TAG, "Voice tab: %d (%s)", new_tab,
             new_tab == 0 ? "local" : new_tab == 1 ? "hybrid" :
             new_tab == 2 ? "cloud" : "tinkerclaw");
    send_voice_config();
}

static void cb_tab_local(lv_event_t *e)      { (void)e; voice_tab_switch(0); }
static void cb_tab_hybrid(lv_event_t *e)     { (void)e; voice_tab_switch(1); }
static void cb_tab_cloud(lv_event_t *e)      { (void)e; voice_tab_switch(2); }
static void cb_tab_tinkerclaw(lv_event_t *e) { (void)e; voice_tab_switch(3); }

static void cb_local_model(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dd);
    if (sel < sizeof(s_local_model_names)/sizeof(s_local_model_names[0])) {
        tab5_settings_set_llm_model(s_local_model_names[sel]);
        ESP_LOGI(TAG, "Local LLM model: %s", s_local_model_names[sel]);
        send_voice_config();
    }
}

static void cb_cloud_model(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dd);
    if (sel < sizeof(s_cloud_model_ids)/sizeof(s_cloud_model_ids[0])) {
        tab5_settings_set_llm_model(s_cloud_model_ids[sel]);
        ESP_LOGI(TAG, "Cloud LLM model: %s", s_cloud_model_ids[sel]);
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
        lv_obj_set_style_text_color(hint, on ? lv_color_hex(TAB_LOCAL) : lv_color_hex(TEXT_DIM), 0);
    }
    if (on) {
        voice_start_always_listening();
    } else {
        voice_stop_always_listening();
    }
}

/* ── WiFi picker ────────────────────────────────────────────────────── */

static void cb_wifi_setup(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Launching WiFi setup screen");
    extern lv_obj_t *ui_wifi_create(void);
    ui_wifi_create();
}

/* ── Connection mode ────────────────────────────────────────────────── */

static void cb_conn_mode(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dd);
    tab5_settings_set_connection_mode((uint8_t)sel);
    ESP_LOGI(TAG, "Connection mode: %d (%s)",
             (int)sel, sel == 0 ? "auto" : sel == 1 ? "local" : "remote");
}

/* ── Dragon host ────────────────────────────────────────────────────── */

static lv_obj_t *s_dragon_status_lbl = NULL;  /* reconnect feedback label (US-PR09) */

static void cb_dragon_host_done(lv_event_t *e)
{
    (void)e;
    if (!s_dragon_ta) return;
    const char *txt = lv_textarea_get_text(s_dragon_ta);
    if (txt && txt[0]) {
        tab5_settings_set_dragon_host(txt);
        ESP_LOGI(TAG, "Dragon host saved: %s", txt);
        if (voice_is_connected()) {
            ESP_LOGI(TAG, "Disconnecting voice for host change -> watchdog will reconnect");
            voice_disconnect();
        }
        /* US-PR09: Show reconnect feedback below Dragon host field */
        if (s_dragon_status_lbl) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Reconnecting to %s...", txt);
            lv_label_set_text(s_dragon_status_lbl, msg);
            lv_obj_set_style_text_color(s_dragon_status_lbl, lv_color_hex(ACC_NETWORK), 0);
            lv_obj_clear_flag(s_dragon_status_lbl, LV_OBJ_FLAG_HIDDEN);
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

/* ── Keyboard layout callback — shrink scroll area above keyboard ── */
static void settings_keyboard_layout_cb(bool visible, int kb_height)
{
    if (!s_scroll) return;

    if (visible) {
        /* Shrink scroll container so it ends above keyboard */
        lv_obj_set_height(s_scroll, USABLE_H - TOPBAR_H - kb_height);

        /* Scroll to make dragon_ta visible if it exists */
        if (s_dragon_ta) {
            lv_obj_scroll_to_view(s_dragon_ta, LV_ANIM_ON);
        }
    } else {
        /* Restore full height */
        lv_obj_set_height(s_scroll, OVERLAY_H - TOPBAR_H);
    }
}

/* ── NTP sync ───────────────────────────────────────────────────────── */

static void cb_ntp_sync(lv_event_t *e)
{
    (void)e;
    if (s_ntp_spinner) return;
    if (s_ntp_btn_label) {
        lv_label_set_text(s_ntp_btn_label, "Syncing...");
    }
    xTaskCreate(ntp_sync_task, "ntp_sync", 4096, NULL, 5, NULL);
}

static void ntp_sync_task(void *arg)
{
    (void)arg;
    esp_err_t ret = tab5_rtc_sync_from_ntp();

    if (s_destroying) { vTaskDelete(NULL); return; }

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

/* ── OTA ────────────────────────────────────────────────────────────── */

/* OTA progress update via lv_async_call (thread-safe LVGL access) */
typedef struct {
    int percent;
    char phase[16];
} ota_progress_msg_t;

static void ota_progress_async_cb(void *arg)
{
    ota_progress_msg_t *msg = (ota_progress_msg_t *)arg;
    if (!msg) return;
    if (s_destroying || !s_ota_btn_label) { free(msg); return; }

    if (strcmp(msg->phase, "download") == 0) {
        lv_label_set_text_fmt(s_ota_btn_label, "Downloading... %d%%", msg->percent);
    } else if (strcmp(msg->phase, "verify") == 0) {
        lv_label_set_text(s_ota_btn_label, "Verifying...");
    } else if (strcmp(msg->phase, "reboot") == 0) {
        lv_label_set_text(s_ota_btn_label, "Rebooting...");
    }
    free(msg);
}

static void ota_progress_cb(int percent, const char *phase)
{
    if (s_destroying) return;
    ota_progress_msg_t *msg = malloc(sizeof(ota_progress_msg_t));
    if (!msg) return;
    msg->percent = percent;
    strncpy(msg->phase, phase, sizeof(msg->phase) - 1);
    msg->phase[sizeof(msg->phase) - 1] = '\0';
    lv_async_call(ota_progress_async_cb, msg);
}

static void ota_apply_task(void *arg)
{
    ESP_LOGI(TAG, "OTA apply: downloading from %s", s_ota_url);
    tab5_ota_set_progress_cb(ota_progress_cb);
    esp_err_t err = tab5_ota_apply(s_ota_url, s_ota_sha256[0] ? s_ota_sha256 : NULL);
    tab5_ota_set_progress_cb(NULL);
    ESP_LOGE(TAG, "OTA apply failed: %s", esp_err_to_name(err));
    if (s_destroying) { vTaskDelete(NULL); return; }
    tab5_ui_lock();
    if (s_ota_btn_label) lv_label_set_text(s_ota_btn_label, "Update failed!");
    if (s_ota_apply_btn) lv_obj_clear_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    tab5_ui_unlock();
    vTaskDelete(NULL);
}

/* OTA confirmation dialog callbacks (US-PR19) */
static void cb_ota_cancel(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_user_data(e);
    if (mbox) lv_msgbox_close(mbox);
}

static void cb_ota_confirm(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_user_data(e);
    if (mbox) lv_msgbox_close(mbox);

    /* Proceed with actual OTA apply */
    if (!s_ota_url[0]) return;
    if (s_ota_btn_label) lv_label_set_text(s_ota_btn_label, "Updating...");
    if (s_ota_apply_btn) lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    xTaskCreate(ota_apply_task, "ota_apply", 8192, NULL, 5, NULL);
}

static void cb_ota_apply(lv_event_t *e)
{
    (void)e;
    if (!s_ota_url[0]) return;

    /* Show confirmation dialog instead of applying immediately (US-PR19) */
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "Update Firmware?");
    lv_msgbox_add_text(mbox, "Device will restart after update.\nDo not unplug during update.");
    lv_obj_t *btn_cancel = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_t *btn_update = lv_msgbox_add_footer_button(mbox, "Update");
    lv_obj_add_event_cb(btn_cancel, cb_ota_cancel, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(btn_update, cb_ota_confirm, LV_EVENT_CLICKED, mbox);

    /* Style the dialog for Material Dark theme */
    lv_obj_set_style_bg_color(mbox, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(mbox, 16, 0);
    lv_obj_set_style_border_width(mbox, 1, 0);
    lv_obj_set_style_border_color(mbox, lv_color_hex(0x333333), 0);
    lv_obj_set_width(mbox, 400);

    /* Style title */
    lv_obj_t *title = lv_msgbox_get_title(mbox);
    if (title) {
        lv_obj_set_style_text_color(title, lv_color_hex(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(title, FONT_BODY, 0);
    }

    /* Style content area */
    lv_obj_t *content = lv_msgbox_get_content(mbox);
    if (content) {
        lv_obj_set_style_text_color(content, lv_color_hex(TEXT_DIM), 0);
        lv_obj_set_style_text_font(content, FONT_CAPTION, 0);
    }

    /* Style header */
    lv_obj_t *header = lv_msgbox_get_header(mbox);
    if (header) {
        lv_obj_set_style_bg_color(header, lv_color_hex(CARD_COLOR), 0);
    }

    /* Style footer */
    lv_obj_t *footer = lv_msgbox_get_footer(mbox);
    if (footer) {
        lv_obj_set_style_bg_color(footer, lv_color_hex(CARD_COLOR), 0);
    }
}

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
        snprintf(s_ota_url, sizeof(s_ota_url), "%s", info.url);
        strncpy(s_ota_sha256, info.sha256, sizeof(s_ota_sha256) - 1);
        s_ota_sha256[sizeof(s_ota_sha256) - 1] = '\0';
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

/* ══════════════════════════════════════════════════════════════════════
 *  Inline style helpers
 * ══════════════════════════════════════════════════════════════════════ */

/** Create a Material Dark slider: 200px wide, 4px track, 16px accent knob. */
static lv_obj_t *mk_slider(lv_obj_t *parent, lv_color_t accent, int x, int y,
                            int min, int max, int val, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_pos(s, x, y + (ROW_H - 4) / 2 - 2);
    lv_obj_set_size(s, 200, 4);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(CARD_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, accent, LV_PART_KNOB);
    lv_obj_set_style_radius(s, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 2, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 8, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

/** Create a Material Dark toggle switch: 44x24px. */
static lv_obj_t *mk_switch(lv_obj_t *parent, lv_color_t accent, int x, int y,
                            bool checked, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y + (ROW_H - 24) / 2);
    lv_obj_set_size(sw, 44, 24);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(TEXT_PRIMARY), LV_PART_KNOB);
    lv_obj_set_style_radius(sw, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 12, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sw, 10, LV_PART_KNOB);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return sw;
}

/** Create a pill-shaped button with centered text. */
static lv_obj_t *mk_pill_btn(lv_obj_t *parent, const char *text, lv_color_t bg,
                              lv_color_t text_color, int x, int y, int w, int h,
                              int radius, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, text_color, 0);
    lv_obj_set_style_text_font(lbl, FONT_CAPTION, 0);
    lv_obj_center(lbl);

    return btn;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Phase 2 timer callback: create remaining sections ──────────────── */
static void phase2_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_phase2_timer = NULL;

    /* Guard: screen destroyed/hidden before Phase 2 fires */
    if (!s_phase2_pending || s_destroying || !s_scroll) {
        ESP_LOGW(TAG, "Phase 2 skipped (screen gone)");
        s_phase2_pending = false;
        s_creating = false;
        return;
    }

    ESP_LOGI(TAG, "Phase 2: creating Voice + Storage + Battery + About...");
    int y = s_phase2_y;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: VOICE MODE (purple #A855F7)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Section: Voice Mode");
    lv_color_t acc_voice = lv_color_hex(ACC_VOICE);

    y = mk_section(s_scroll, "VOICE MODE", acc_voice, y);

    /* Four pill-shaped tab buttons */
    s_active_tab = tab5_settings_get_voice_mode();
    if (s_active_tab > 3) s_active_tab = 0;

    int tab_w = 158;
    int tab_h = 40;
    int tab_gap = 10;
    int tab_x0 = SIDE_PAD;

    {
        /* Local tab */
        s_tab_local = lv_button_create(s_scroll);
        lv_obj_remove_style_all(s_tab_local);
        lv_obj_set_pos(s_tab_local, tab_x0, y);
        lv_obj_set_size(s_tab_local, tab_w, tab_h);
        lv_obj_set_style_radius(s_tab_local, 20, 0);
        lv_obj_set_style_bg_opa(s_tab_local, LV_OPA_COVER, 0);
        if (s_active_tab == 0) {
            lv_obj_set_style_bg_color(s_tab_local, lv_color_hex(TAB_LOCAL), 0);
            lv_obj_set_style_text_color(s_tab_local, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(s_tab_local, lv_color_hex(CARD_COLOR), 0);
            lv_obj_set_style_text_color(s_tab_local, lv_color_hex(0x666666), 0);
        }
        lv_obj_add_event_cb(s_tab_local, cb_tab_local, LV_EVENT_CLICKED, NULL);
        lv_obj_t *tl = lv_label_create(s_tab_local);
        lv_label_set_text(tl, "Local");
        lv_obj_set_style_text_font(tl, FONT_SECONDARY, 0);
        lv_obj_center(tl);

        feed_wdt(); /* mid-section yield — Voice Mode is the heaviest section */

        /* Hybrid tab */
        s_tab_hybrid = lv_button_create(s_scroll);
        lv_obj_remove_style_all(s_tab_hybrid);
        lv_obj_set_pos(s_tab_hybrid, tab_x0 + tab_w + tab_gap, y);
        lv_obj_set_size(s_tab_hybrid, tab_w, tab_h);
        lv_obj_set_style_radius(s_tab_hybrid, 20, 0);
        lv_obj_set_style_bg_opa(s_tab_hybrid, LV_OPA_COVER, 0);
        if (s_active_tab == 1) {
            lv_obj_set_style_bg_color(s_tab_hybrid, lv_color_hex(TAB_HYBRID), 0);
            lv_obj_set_style_text_color(s_tab_hybrid, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(s_tab_hybrid, lv_color_hex(CARD_COLOR), 0);
            lv_obj_set_style_text_color(s_tab_hybrid, lv_color_hex(0x666666), 0);
        }
        lv_obj_add_event_cb(s_tab_hybrid, cb_tab_hybrid, LV_EVENT_CLICKED, NULL);
        lv_obj_t *th = lv_label_create(s_tab_hybrid);
        lv_label_set_text(th, "Hybrid");
        lv_obj_set_style_text_font(th, FONT_SECONDARY, 0);
        lv_obj_center(th);

        feed_wdt(); /* mid-section yield */

        /* Cloud tab */
        s_tab_cloud = lv_button_create(s_scroll);
        lv_obj_remove_style_all(s_tab_cloud);
        lv_obj_set_pos(s_tab_cloud, tab_x0 + 2 * (tab_w + tab_gap), y);
        lv_obj_set_size(s_tab_cloud, tab_w, tab_h);
        lv_obj_set_style_radius(s_tab_cloud, 20, 0);
        lv_obj_set_style_bg_opa(s_tab_cloud, LV_OPA_COVER, 0);
        if (s_active_tab == 2) {
            lv_obj_set_style_bg_color(s_tab_cloud, lv_color_hex(TAB_CLOUD), 0);
            lv_obj_set_style_text_color(s_tab_cloud, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(s_tab_cloud, lv_color_hex(CARD_COLOR), 0);
            lv_obj_set_style_text_color(s_tab_cloud, lv_color_hex(0x666666), 0);
        }
        lv_obj_add_event_cb(s_tab_cloud, cb_tab_cloud, LV_EVENT_CLICKED, NULL);
        lv_obj_t *tc = lv_label_create(s_tab_cloud);
        lv_label_set_text(tc, "Cloud");
        lv_obj_set_style_text_font(tc, FONT_SECONDARY, 0);
        lv_obj_center(tc);

        feed_wdt(); /* mid-section yield — before TinkerClaw tab */

        /* TinkerClaw tab */
        s_tab_tinkerclaw = lv_button_create(s_scroll);
        lv_obj_remove_style_all(s_tab_tinkerclaw);
        lv_obj_set_pos(s_tab_tinkerclaw, tab_x0 + 3 * (tab_w + tab_gap), y);
        lv_obj_set_size(s_tab_tinkerclaw, tab_w, tab_h);
        lv_obj_set_style_radius(s_tab_tinkerclaw, 20, 0);
        lv_obj_set_style_bg_opa(s_tab_tinkerclaw, LV_OPA_COVER, 0);
        if (s_active_tab == 3) {
            lv_obj_set_style_bg_color(s_tab_tinkerclaw, lv_color_hex(TAB_TINKERCLAW), 0);
            lv_obj_set_style_text_color(s_tab_tinkerclaw, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(s_tab_tinkerclaw, lv_color_hex(CARD_COLOR), 0);
            lv_obj_set_style_text_color(s_tab_tinkerclaw, lv_color_hex(0x666666), 0);
        }
        lv_obj_add_event_cb(s_tab_tinkerclaw, cb_tab_tinkerclaw, LV_EVENT_CLICKED, NULL);
        lv_obj_t *tt = lv_label_create(s_tab_tinkerclaw);
        lv_label_set_text(tt, "TinkerClaw");
        lv_obj_set_style_text_font(tt, FONT_SECONDARY, 0);
        lv_obj_center(tt);
    }
    y += tab_h + 10;

    /* ── LOCAL card (green tint) ─────────────────────────────────────── */
    int card_y = y;
    int card_h = 110;
    {
        s_local_card = lv_obj_create(s_scroll);
        lv_obj_remove_style_all(s_local_card);
        lv_obj_set_pos(s_local_card, SIDE_PAD, card_y);
        lv_obj_set_size(s_local_card, CONTENT_W, card_h);
        lv_obj_set_style_bg_color(s_local_card, lv_color_hex(0x0D1A0D), 0);
        lv_obj_set_style_bg_opa(s_local_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_local_card, 1, 0);
        lv_obj_set_style_border_color(s_local_card, lv_color_hex(0x22C55E33), 0);
        lv_obj_set_style_border_opa(s_local_card, (lv_opa_t)0x33, 0);
        lv_obj_set_style_radius(s_local_card, 8, 0);
        lv_obj_clear_flag(s_local_card, LV_OBJ_FLAG_SCROLLABLE);

        /* LLM Model row */
        lv_obj_t *k1 = lv_label_create(s_local_card);
        lv_label_set_text(k1, "LLM Model");
        lv_obj_set_style_text_color(k1, lv_color_hex(TAB_LOCAL), 0);
        lv_obj_set_style_text_font(k1, FONT_SECONDARY, 0);
        lv_obj_set_pos(k1, 16, 14);
        s_local_content[0] = k1;

        lv_obj_t *dd = lv_dropdown_create(s_local_card);
        lv_obj_set_pos(dd, 340, 8);
        lv_obj_set_size(dd, 200, 36);
        lv_dropdown_set_options(dd, "qwen3:1.7b\nqwen3:4b\nqwen3:0.6b");
        lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), 0);
        lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(dd, FONT_SECONDARY, 0);
        lv_obj_set_style_border_width(dd, 1, 0);
        lv_obj_set_style_border_color(dd, lv_color_hex(TAB_LOCAL), 0);
        lv_obj_set_style_radius(dd, 6, 0);
        {
            char saved[64] = {0};
            tab5_settings_get_llm_model(saved, sizeof(saved));
            for (int i = 0; i < 3; i++) {
                if (strcmp(s_local_model_names[i], saved) == 0) {
                    lv_dropdown_set_selected(dd, i);
                    break;
                }
            }
        }
        lv_obj_add_event_cb(dd, cb_local_model, LV_EVENT_VALUE_CHANGED, NULL);
        s_local_content[1] = dd;

        /* Temperature row (combined key:value) */
        lv_obj_t *k2 = lv_label_create(s_local_card);
        lv_label_set_text(k2, "Temperature: 0.7");
        lv_obj_set_style_text_color(k2, lv_color_hex(TAB_LOCAL), 0);
        lv_obj_set_style_text_font(k2, FONT_SECONDARY, 0);
        lv_obj_set_pos(k2, 16, 60);
        s_local_content[2] = k2;
        s_local_content[3] = NULL;

        if (s_active_tab != 0) lv_obj_add_flag(s_local_card, LV_OBJ_FLAG_HIDDEN);
    }

    /* ── HYBRID card (amber tint) ────────────────────────────────────── */
    {
        s_hybrid_card = lv_obj_create(s_scroll);
        lv_obj_remove_style_all(s_hybrid_card);
        lv_obj_set_pos(s_hybrid_card, SIDE_PAD, card_y);
        lv_obj_set_size(s_hybrid_card, CONTENT_W, card_h);
        lv_obj_set_style_bg_color(s_hybrid_card, lv_color_hex(0x1A170D), 0);
        lv_obj_set_style_bg_opa(s_hybrid_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_hybrid_card, 1, 0);
        lv_obj_set_style_border_color(s_hybrid_card, lv_color_hex(0xEAB30833), 0);
        lv_obj_set_style_border_opa(s_hybrid_card, (lv_opa_t)0x33, 0);
        lv_obj_set_style_radius(s_hybrid_card, 8, 0);
        lv_obj_clear_flag(s_hybrid_card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *k1 = lv_label_create(s_hybrid_card);
        lv_label_set_text(k1, "STT / TTS: Cloud (OpenRouter)");
        lv_obj_set_style_text_color(k1, lv_color_hex(TAB_HYBRID), 0);
        lv_obj_set_style_text_font(k1, FONT_SECONDARY, 0);
        lv_obj_set_pos(k1, 16, 20);
        s_hybrid_content[0] = k1;
        s_hybrid_content[1] = NULL;

        lv_obj_t *k2 = lv_label_create(s_hybrid_card);
        lv_label_set_text(k2, "LLM: Local (Ollama)");
        lv_obj_set_style_text_color(k2, lv_color_hex(TAB_HYBRID), 0);
        lv_obj_set_style_text_font(k2, FONT_SECONDARY, 0);
        lv_obj_set_pos(k2, 16, 60);
        s_hybrid_content[2] = k2;
        s_hybrid_content[3] = NULL;

        if (s_active_tab != 1) lv_obj_add_flag(s_hybrid_card, LV_OBJ_FLAG_HIDDEN);
    }

    /* ── CLOUD card (blue tint) ──────────────────────────────────────── */
    {
        s_cloud_card = lv_obj_create(s_scroll);
        lv_obj_remove_style_all(s_cloud_card);
        lv_obj_set_pos(s_cloud_card, SIDE_PAD, card_y);
        lv_obj_set_size(s_cloud_card, CONTENT_W, card_h);
        lv_obj_set_style_bg_color(s_cloud_card, lv_color_hex(0x0D0D1A), 0);
        lv_obj_set_style_bg_opa(s_cloud_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_cloud_card, 1, 0);
        lv_obj_set_style_border_color(s_cloud_card, lv_color_hex(0x3B82F633), 0);
        lv_obj_set_style_border_opa(s_cloud_card, (lv_opa_t)0x33, 0);
        lv_obj_set_style_radius(s_cloud_card, 8, 0);
        lv_obj_clear_flag(s_cloud_card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *k1 = lv_label_create(s_cloud_card);
        lv_label_set_text(k1, "Cloud Model");
        lv_obj_set_style_text_color(k1, lv_color_hex(TAB_CLOUD), 0);
        lv_obj_set_style_text_font(k1, FONT_SECONDARY, 0);
        lv_obj_set_pos(k1, 16, 14);
        s_cloud_content[0] = k1;

        lv_obj_t *dd = lv_dropdown_create(s_cloud_card);
        lv_obj_set_pos(dd, 340, 8);
        lv_obj_set_size(dd, 200, 36);
        lv_dropdown_set_options(dd, "Claude 3.5 Haiku\nClaude Sonnet\nGPT-4o mini\nGPT-4o");
        lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), 0);
        lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(dd, FONT_SECONDARY, 0);
        lv_obj_set_style_border_width(dd, 1, 0);
        lv_obj_set_style_border_color(dd, lv_color_hex(TAB_CLOUD), 0);
        lv_obj_set_style_radius(dd, 6, 0);
        {
            char saved[64] = {0};
            tab5_settings_get_llm_model(saved, sizeof(saved));
            for (int i = 0; i < 4; i++) {
                if (strcmp(s_cloud_model_ids[i], saved) == 0) {
                    lv_dropdown_set_selected(dd, i);
                    break;
                }
            }
        }
        lv_obj_add_event_cb(dd, cb_cloud_model, LV_EVENT_VALUE_CHANGED, NULL);
        s_cloud_content[1] = dd;

        lv_obj_t *k2 = lv_label_create(s_cloud_card);
        lv_label_set_text(k2, "API Key: Set");
        lv_obj_set_style_text_color(k2, lv_color_hex(TAB_CLOUD), 0);
        lv_obj_set_style_text_font(k2, FONT_SECONDARY, 0);
        lv_obj_set_pos(k2, 16, 60);
        s_cloud_content[2] = k2;
        s_cloud_content[3] = NULL;

        if (s_active_tab != 2) lv_obj_add_flag(s_cloud_card, LV_OBJ_FLAG_HIDDEN);
    }

    /* ── TINKERCLAW card (rose tint) ────────────────────────────────────── */
    {
        s_tinkerclaw_card = lv_obj_create(s_scroll);
        lv_obj_remove_style_all(s_tinkerclaw_card);
        lv_obj_set_pos(s_tinkerclaw_card, SIDE_PAD, card_y);
        lv_obj_set_size(s_tinkerclaw_card, CONTENT_W, card_h);
        lv_obj_set_style_bg_color(s_tinkerclaw_card, lv_color_hex(0x1A0D12), 0);
        lv_obj_set_style_bg_opa(s_tinkerclaw_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_tinkerclaw_card, 1, 0);
        lv_obj_set_style_border_color(s_tinkerclaw_card, lv_color_hex(0xE11D4833), 0);
        lv_obj_set_style_border_opa(s_tinkerclaw_card, (lv_opa_t)0x33, 0);
        lv_obj_set_style_radius(s_tinkerclaw_card, 8, 0);
        lv_obj_clear_flag(s_tinkerclaw_card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *k1 = lv_label_create(s_tinkerclaw_card);
        lv_label_set_text(k1, "Agent mode \xe2\x80\x94 TinkerClaw gateway");
        lv_obj_set_style_text_color(k1, lv_color_hex(TAB_TINKERCLAW), 0);
        lv_obj_set_style_text_font(k1, FONT_SECONDARY, 0);
        lv_obj_set_pos(k1, 16, 20);
        s_tinkerclaw_content[0] = k1;

        lv_obj_t *k2 = lv_label_create(s_tinkerclaw_card);
        lv_label_set_text(k2, "handles skills, memory, and model selection");
        lv_obj_set_style_text_color(k2, lv_color_hex(TAB_TINKERCLAW), 0);
        lv_obj_set_style_text_font(k2, FONT_SECONDARY, 0);
        lv_obj_set_pos(k2, 16, 60);
        s_tinkerclaw_content[1] = k2;
        s_tinkerclaw_content[2] = NULL;
        s_tinkerclaw_content[3] = NULL;

        if (s_active_tab != 3) lv_obj_add_flag(s_tinkerclaw_card, LV_OBJ_FLAG_HIDDEN);
    }

    y = card_y + card_h + 10;

    /* Wake Word toggle */
    mk_row_label(s_scroll, "Wake Word", y);
    mk_switch(s_scroll, acc_voice, 660, y, tab5_settings_get_wake_word() != 0,
              cb_wake_word, NULL);
    y += ROW_H + 16;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: STORAGE (amber #F59E0B)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Section: Storage");
    lv_color_t acc_storage = lv_color_hex(ACC_STORAGE);

    y = mk_section(s_scroll, "STORAGE", acc_storage, y);

    /* SD info shown directly under section header */
    s_lbl_sd_info = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_sd_info, "SD: Checking...");
    lv_obj_set_style_text_color(s_lbl_sd_info, lv_color_hex(TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_sd_info, FONT_BODY, 0);
    lv_obj_set_pos(s_lbl_sd_info, SIDE_PAD, y + (ROW_H - 18) / 2);
    y += ROW_H + 16;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: BATTERY (red #EF4444)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Section: Battery");
    lv_color_t acc_battery = lv_color_hex(ACC_BATTERY);

    y = mk_section(s_scroll, "BATTERY", acc_battery, y);

    /* Primary battery status: "XX% . Charging" or "USB Powered" */
    s_lbl_bat_status = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_bat_status, "Checking...");
    lv_obj_set_style_text_color(s_lbl_bat_status, lv_color_hex(TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_bat_status, FONT_BODY, 0);
    lv_obj_set_pos(s_lbl_bat_status, SIDE_PAD, y + (ROW_H - 18) / 2);
    y += ROW_H + 4;

    /* Level with bar + percentage */
    s_bar_bat_level = lv_bar_create(s_scroll);
    lv_obj_set_pos(s_bar_bat_level, SIDE_PAD, y + (ROW_H - 12) / 2);
    lv_obj_set_size(s_bar_bat_level, 400, 12);
    lv_bar_set_range(s_bar_bat_level, 0, 100);
    lv_bar_set_value(s_bar_bat_level, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_bat_level, lv_color_hex(CARD_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_bat_level, acc_battery, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_bat_level, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_bat_level, 6, LV_PART_INDICATOR);
    s_lbl_bat_pct = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_bat_pct, "0%");
    lv_obj_set_style_text_color(s_lbl_bat_pct, lv_color_hex(TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_bat_pct, FONT_BODY, 0);
    lv_obj_set_pos(s_lbl_bat_pct, SIDE_PAD + 415, y + (ROW_H - 18) / 2);
    y += ROW_H + 2;

    /* Debug voltage label -- small and dim for developers */
    s_lbl_bat_volt = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_bat_volt, "");
    lv_obj_set_style_text_color(s_lbl_bat_volt, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(s_lbl_bat_volt, FONT_SMALL, 0);
    lv_obj_set_pos(s_lbl_bat_volt, SIDE_PAD, y);
    y += 20 + 16;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: ABOUT (violet #8B5CF6)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Section: About");
    lv_color_t acc_about = lv_color_hex(ACC_ABOUT);

    y = mk_section(s_scroll, "ABOUT", acc_about, y);

    /* Device info -- Line 1: product name + version (prominent) */
    {
        char ver_str[64];
        snprintf(ver_str, sizeof(ver_str), "TinkerTab v%s", TAB5_FIRMWARE_VER);
        lv_obj_t *ver_lbl = lv_label_create(s_scroll);
        lv_label_set_text(ver_lbl, ver_str);
        lv_obj_set_style_text_color(ver_lbl, lv_color_hex(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(ver_lbl, FONT_HEADING, 0);
        lv_obj_set_pos(ver_lbl, SIDE_PAD, y + 2);
    }
    y += 28;
    /* Device info -- Line 2: hardware details (smaller, gray) */
    {
        char hw_str[96];
        snprintf(hw_str, sizeof(hw_str),
                 "M5Stack Tab5 \xc2\xb7 ESP32-P4 \xc2\xb7 %s", tab5_ota_current_partition());
        lv_obj_t *hw_lbl = lv_label_create(s_scroll);
        lv_label_set_text(hw_lbl, hw_str);
        lv_obj_set_style_text_color(hw_lbl, lv_color_hex(TEXT_DIM), 0);
        lv_obj_set_style_text_font(hw_lbl, FONT_SMALL, 0);
        lv_obj_set_pos(hw_lbl, SIDE_PAD, y);
    }
    y += 24;

    /* OTA Update -- full-width Check button */
    {
        lv_obj_t *ota_btn = mk_pill_btn(s_scroll, LV_SYMBOL_DOWNLOAD " Check Update",
                                        acc_about, lv_color_hex(TEXT_PRIMARY),
                                        SIDE_PAD, y + 3, CONTENT_W, 42, 8, cb_ota_check);
        s_ota_btn_label = lv_obj_get_child(ota_btn, 0);
    }
    y += ROW_H + 8;
    {
        s_ota_apply_btn = mk_pill_btn(s_scroll, LV_SYMBOL_OK " Apply Update",
                                      lv_color_hex(TAB_LOCAL), lv_color_hex(TEXT_PRIMARY),
                                      SIDE_PAD, y + 3, CONTENT_W, 42, 8, cb_ota_apply);
        lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    }
    y += ROW_H + 8;

    /* Free Heap + PSRAM (debug info -- small and dim) */
    s_lbl_heap = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_heap, "Heap: -- KB  |  PSRAM: -- MB");
    lv_obj_set_style_text_color(s_lbl_heap, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(s_lbl_heap, FONT_SMALL, 0);
    lv_obj_set_pos(s_lbl_heap, SIDE_PAD, y + (ROW_H - 14) / 2);
    y += ROW_H;

    /* ── Bottom padding ──────────────────────────────────────────────── */
    y += 40;

    s_phase2_pending = false;
    ESP_LOGI(TAG, "Phase 2 complete (%lu objects total)", (unsigned long)lv_obj_get_child_count(s_scroll));

    /* Trigger immediate data refresh for the newly created labels */
    ui_settings_update();

    /* Mark creation complete — touch is already enabled from Phase 1 */
    s_creating = false;
}

lv_obj_t *ui_settings_create(void)
{
    if (s_creating) {
        ESP_LOGW(TAG, "Settings creation already in progress — skipping");
        return s_screen;
    }

    if (s_screen) {
        /* Overlay already exists — just unhide and resume refresh */
        ESP_LOGI(TAG, "Settings screen resumed");
        s_destroying = false;
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(s_screen);
        /* Recreate refresh timer (deleted on hide to prevent race conditions) */
        if (!s_refresh_timer) s_refresh_timer = lv_timer_create(settings_refresh_cb, 2000, NULL);
        else lv_timer_resume(s_refresh_timer);
        ui_settings_update();
        ui_keyboard_set_layout_cb(settings_keyboard_layout_cb);

        /* If Phase 2 was cancelled (hidden before it fired), re-schedule it */
        if (!s_lbl_heap && s_scroll && !s_phase2_pending) {
            ESP_LOGI(TAG, "Phase 2 missing — re-scheduling");
            s_creating = true;
            s_phase2_pending = true;
            s_phase2_timer = lv_timer_create(phase2_timer_cb, 50, NULL);
            lv_timer_set_repeat_count(s_phase2_timer, 1);
        }
        return s_screen;
    }
    ESP_LOGI(TAG, "Creating settings screen (two-pass)...");
    s_creating = true;
    s_destroying = false;

    /* ═══════════════════════════════════════════════════════════════════
     *  PHASE 1: Container + Display + Network (~15 objects, <100ms)
     *  User sees the screen immediately after this returns.
     * ═══════════════════════════════════════════════════════════════════ */

    /* ── Fullscreen overlay on home screen ───────────────────────────── */
    lv_obj_t *home = ui_home_get_screen();
    s_screen = lv_obj_create(home);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, 720, OVERLAY_H);
    lv_obj_set_pos(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_screen);

    /* Swipe-right to go back */
    lv_obj_add_event_cb(s_screen, cb_back_btn, LV_EVENT_GESTURE, NULL);

    /* ── Top Bar (48px, sticky) ──────────────────────────────────────── */
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Back arrow button (36x36, rounded 8px, bg #1A1A2E) */
    lv_obj_t *back_btn = lv_button_create(bar);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_set_pos(back_btn, SIDE_PAD, (TOPBAR_H - 36) / 2);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_add_event_cb(back_btn, cb_back_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_label_create(back_btn);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(arrow, FONT_SECONDARY, 0);
    lv_obj_center(arrow);

    /* "Settings" title centered */
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title, FONT_HEADING, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* ── Scrollable content area ─────────────────────────────────────── */
    s_scroll = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, 720, USABLE_H - TOPBAR_H);
    lv_obj_set_pos(s_scroll, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_scroll, 0, 0);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);

    int y = 12;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: DISPLAY (amber #F5A623)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Phase 1 — Section: Display");
    lv_color_t acc_display = lv_color_hex(ACC_DISPLAY);

    y = mk_section(s_scroll, "DISPLAY", acc_display, y);

    /* Brightness */
    mk_row_label(s_scroll, "Brightness", y);
    s_slider_bright = mk_slider(s_scroll, acc_display, RIGHT_X, y,
                                0, 100, tab5_settings_get_brightness(), cb_brightness);
    s_lbl_bright_val = lv_label_create(s_scroll);
    lv_obj_set_pos(s_lbl_bright_val, RIGHT_X + 208, y + (ROW_H - 14) / 2);
    lv_label_set_text_fmt(s_lbl_bright_val, "%d%%", tab5_settings_get_brightness());
    lv_obj_set_style_text_color(s_lbl_bright_val, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_text_font(s_lbl_bright_val, FONT_SECONDARY, 0);
    y += ROW_H + 4;

    /* Volume */
    mk_row_label(s_scroll, "Volume", y);
    s_slider_volume = mk_slider(s_scroll, acc_display, RIGHT_X, y,
                                0, 100, tab5_settings_get_volume(), cb_volume);
    lv_obj_add_event_cb(s_slider_volume, cb_volume_released, LV_EVENT_RELEASED, NULL);
    s_lbl_vol_val = lv_label_create(s_scroll);
    lv_obj_set_pos(s_lbl_vol_val, RIGHT_X + 208, y + (ROW_H - 14) / 2);
    lv_label_set_text_fmt(s_lbl_vol_val, "%d%%", tab5_settings_get_volume());
    lv_obj_set_style_text_color(s_lbl_vol_val, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_text_font(s_lbl_vol_val, FONT_SECONDARY, 0);
    y += ROW_H + 4;

    /* Auto-rotate */
    mk_row_label(s_scroll, "Auto-rotate", y);
    s_lbl_orient = mk_row_value(s_scroll, "Off", lv_color_hex(TEXT_DIM), y);
    s_sw_autorot = mk_switch(s_scroll, acc_display, 660, y, false, cb_autorotate, NULL);
    y += ROW_H + 16;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: NETWORK (cyan #00E5FF)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Phase 1 — Section: Network");
    lv_color_t acc_network = lv_color_hex(ACC_NETWORK);

    y = mk_section(s_scroll, "NETWORK", acc_network, y);

    /* WiFi status */
    mk_row_label(s_scroll, "WiFi", y);
    s_lbl_wifi = mk_row_value(s_scroll, "Checking...", lv_color_hex(TEXT_DIM), y);
    y += ROW_H + 4;

    /* WiFi Setup */
    {
        lv_obj_t *btn = mk_pill_btn(s_scroll, LV_SYMBOL_WIFI " Configure",
                                    acc_network, lv_color_hex(0x000000),
                                    RIGHT_X, y + 3, 180, 38, 8, cb_wifi_setup);
        (void)btn;
    }
    /* NTP Sync */
    {
        lv_obj_t *ntp_btn = mk_pill_btn(s_scroll, "Sync NTP",
                                        acc_network, lv_color_hex(0x000000),
                                        SIDE_PAD, y + 3, 140, 38, 8, cb_ntp_sync);
        s_ntp_btn_label = lv_obj_get_child(ntp_btn, 0);
    }
    y += ROW_H + 4;

    feed_wdt(); /* mid-section yield — Network section */

    /* Dragon host input (placeholder text serves as label) */
    s_dragon_ta = lv_textarea_create(s_scroll);
    lv_obj_set_pos(s_dragon_ta, SIDE_PAD, y + 4);
    lv_obj_set_size(s_dragon_ta, CONTENT_W, 36);
    lv_textarea_set_one_line(s_dragon_ta, true);
    lv_textarea_set_max_length(s_dragon_ta, 63);
    lv_obj_set_style_bg_color(s_dragon_ta, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_bg_opa(s_dragon_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_dragon_ta, lv_color_hex(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_dragon_ta, FONT_CAPTION, 0);
    lv_obj_set_style_border_width(s_dragon_ta, 1, 0);
    lv_obj_set_style_border_color(s_dragon_ta, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(s_dragon_ta, 6, 0);
    lv_obj_set_style_pad_left(s_dragon_ta, 8, 0);
    lv_textarea_set_placeholder_text(s_dragon_ta, "Dragon Host (e.g. 192.168.1.89)");
    {
        char dhost[64];
        tab5_settings_get_dragon_host(dhost, sizeof(dhost));
        if (dhost[0]) {
            lv_textarea_set_text(s_dragon_ta, dhost);
        }
    }
    lv_obj_add_flag(s_dragon_ta, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_done, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_done, LV_EVENT_READY, NULL);
    y += ROW_H + 4;

    /* US-PR09: Reconnect status label (hidden until host change) */
    s_dragon_status_lbl = lv_label_create(s_scroll);
    lv_label_set_text(s_dragon_status_lbl, "");
    lv_obj_set_pos(s_dragon_status_lbl, SIDE_PAD, y);
    lv_obj_set_style_text_color(s_dragon_status_lbl, lv_color_hex(ACC_NETWORK), 0);
    lv_obj_set_style_text_font(s_dragon_status_lbl, FONT_SMALL, 0);
    lv_obj_add_flag(s_dragon_status_lbl, LV_OBJ_FLAG_HIDDEN);
    y += 4;

    /* Connection mode: Auto / Local / Remote */
    {
        lv_obj_t *conn_lbl = lv_label_create(s_scroll);
        lv_label_set_text(conn_lbl, "Connection");
        lv_obj_set_pos(conn_lbl, SIDE_PAD, y + 8);
        lv_obj_set_style_text_color(conn_lbl, lv_color_hex(TEXT_DIM), 0);
        lv_obj_set_style_text_font(conn_lbl, FONT_CAPTION, 0);

        static lv_obj_t *s_conn_dd = NULL;
        s_conn_dd = lv_dropdown_create(s_scroll);
        lv_dropdown_set_options(s_conn_dd, "Automatic\nHome Network\nInternet Only");
        lv_dropdown_set_selected(s_conn_dd, tab5_settings_get_connection_mode());
        lv_obj_set_pos(s_conn_dd, 340, y);
        lv_obj_set_size(s_conn_dd, 340, 36);
        lv_obj_set_style_bg_color(s_conn_dd, lv_color_hex(CARD_COLOR), 0);
        lv_obj_set_style_bg_opa(s_conn_dd, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_conn_dd, lv_color_hex(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(s_conn_dd, FONT_SECONDARY, 0);
        lv_obj_set_style_border_width(s_conn_dd, 1, 0);
        lv_obj_set_style_border_color(s_conn_dd, lv_color_hex(0x333333), 0);
        lv_obj_set_style_radius(s_conn_dd, 6, 0);
        /* Dropdown list styling */
        lv_obj_set_style_bg_color(s_conn_dd, lv_color_hex(CARD_COLOR), LV_PART_ITEMS);
        lv_obj_set_style_text_color(s_conn_dd, lv_color_hex(TEXT_PRIMARY), LV_PART_ITEMS);
        lv_obj_add_event_cb(s_conn_dd, cb_conn_mode, LV_EVENT_VALUE_CHANGED, NULL);
    }
    y += ROW_H + 16;

    /* ═══════════════════════════════════════════════════════════════════
     *  END PHASE 1 — Screen is visible now with Display + Network.
     *  Schedule Phase 2 to create Voice + Storage + Battery + About
     *  after a 50ms delay so LVGL renders Phase 1 first.
     * ═══════════════════════════════════════════════════════════════════ */
    ESP_LOGI(TAG, "Phase 1 complete — scheduling Phase 2 in 50ms");

    s_phase2_y = y;
    s_phase2_pending = true;
    s_phase2_timer = lv_timer_create(phase2_timer_cb, 50, NULL);
    lv_timer_set_repeat_count(s_phase2_timer, 1);

    /* Start refresh timer + initial data update for Phase 1 labels */
    s_refresh_timer = lv_timer_create(settings_refresh_cb, 2000, NULL);
    lv_timer_t *init_timer = lv_timer_create(settings_refresh_cb, 200, NULL);
    lv_timer_set_repeat_count(init_timer, 1);

    ui_keyboard_set_layout_cb(settings_keyboard_layout_cb);

    ESP_LOGI(TAG, "Phase 1 visible — user sees Display + Network immediately");
    return s_screen;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Update live values
 * ══════════════════════════════════════════════════════════════════════ */

void ui_settings_update(void)
{
    if (!s_screen) return;

    /* WiFi status */
    if (s_lbl_wifi) {
        extern bool tab5_wifi_connected(void);
        if (tab5_wifi_connected()) {
            lv_label_set_text(s_lbl_wifi, "Connected");
            lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(TAB_LOCAL), 0);
        } else {
            lv_label_set_text(s_lbl_wifi, "Not connected");
            lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(ACC_BATTERY), 0);
        }
    }

    /* US-PR09: Hide reconnect label once voice is back up */
    if (s_dragon_status_lbl && !lv_obj_has_flag(s_dragon_status_lbl, LV_OBJ_FLAG_HIDDEN)) {
        if (voice_is_connected()) {
            lv_label_set_text(s_dragon_status_lbl, "Connected");
            lv_obj_set_style_text_color(s_dragon_status_lbl, lv_color_hex(TAB_LOCAL), 0);
        }
    }

    /* SD card info */
    if (s_lbl_sd_info) {
        if (tab5_sdcard_mounted()) {
            uint64_t total = tab5_sdcard_total_bytes();
            uint64_t free_b = tab5_sdcard_free_bytes();
            uint64_t used_b = (total > free_b) ? (total - free_b) : 0;
            int total_gb = (int)((total + 536870912ULL) / 1073741824ULL);  /* round */
            int free_gb  = (int)((free_b + 536870912ULL) / 1073741824ULL);
            int used_gb  = (int)((used_b + 536870912ULL) / 1073741824ULL);
            char sd_buf[64];
            if (used_gb == 0) {
                snprintf(sd_buf, sizeof(sd_buf), "SD Card: %d GB available", free_gb);
            } else {
                snprintf(sd_buf, sizeof(sd_buf), "SD Card: %d / %d GB used", used_gb, total_gb);
            }
            lv_label_set_text(s_lbl_sd_info, sd_buf);
            lv_obj_set_style_text_color(s_lbl_sd_info, lv_color_hex(TAB_LOCAL), 0);
        } else {
            lv_label_set_text(s_lbl_sd_info, "SD Card: Not mounted");
            lv_obj_set_style_text_color(s_lbl_sd_info, lv_color_hex(TEXT_DIM), 0);
        }
    }

    /* Battery */
    {
        tab5_battery_info_t bi;
        if (tab5_battery_read(&bi) == ESP_OK) {
            bool no_battery = (bi.voltage < 2.0f && bi.percent == 0);

            /* Primary status label */
            if (s_lbl_bat_status) {
                char sbuf[48];
                if (no_battery) {
                    snprintf(sbuf, sizeof(sbuf), "USB Powered");
                    lv_obj_set_style_text_color(s_lbl_bat_status,
                        lv_color_hex(TAB_LOCAL), 0);
                } else {
                    snprintf(sbuf, sizeof(sbuf), "%d%% \xc2\xb7 %s", bi.percent,
                             bi.charging ? "Charging" : "Discharging");
                    lv_obj_set_style_text_color(s_lbl_bat_status,
                        bi.charging ? lv_color_hex(TAB_LOCAL) : lv_color_hex(TEXT_DIM), 0);
                }
                lv_label_set_text(s_lbl_bat_status, sbuf);
            }
            /* Debug voltage (small dim text) */
            if (s_lbl_bat_volt) {
                char vbuf[32];
                snprintf(vbuf, sizeof(vbuf), "%.2fV", bi.voltage);
                lv_label_set_text(s_lbl_bat_volt, vbuf);
            }
            /* Bar + percentage */
            if (s_bar_bat_level) {
                int bar_pct = no_battery ? 0 : bi.percent;
                lv_bar_set_value(s_bar_bat_level, bar_pct, LV_ANIM_ON);
                lv_color_t bar_col = bar_pct > 20 ? lv_color_hex(TAB_LOCAL) :
                                     bar_pct > 10 ? lv_color_hex(TAB_HYBRID) :
                                     lv_color_hex(ACC_BATTERY);
                lv_obj_set_style_bg_color(s_bar_bat_level, bar_col, LV_PART_INDICATOR);
                /* Hide bar when USB powered (no battery) */
                if (no_battery) {
                    lv_obj_add_flag(s_bar_bat_level, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(s_bar_bat_level, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (s_lbl_bat_pct) {
                if (no_battery) {
                    lv_label_set_text(s_lbl_bat_pct, "");
                } else {
                    char pbuf[8];
                    snprintf(pbuf, sizeof(pbuf), "%d%%", bi.percent);
                    lv_label_set_text(s_lbl_bat_pct, pbuf);
                }
            }
        }
    }

    /* Heap + PSRAM (combined label) */
    if (s_lbl_heap) {
        uint32_t heap_kb = (uint32_t)(esp_get_free_heap_size() / 1024);
        uint32_t psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        char hbuf[64];
        if (psram > 1048576) {
            snprintf(hbuf, sizeof(hbuf), "Heap: %lu KB  |  PSRAM: %.1f MB",
                     (unsigned long)heap_kb, psram / 1048576.0f);
        } else {
            snprintf(hbuf, sizeof(hbuf), "Heap: %lu KB  |  PSRAM: %lu KB",
                     (unsigned long)heap_kb, (unsigned long)(psram / 1024));
        }
        lv_label_set_text(s_lbl_heap, hbuf);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Destroy / Hide / Get
 * ══════════════════════════════════════════════════════════════════════ */

void ui_settings_destroy(void)
{
    if (!s_screen) return;

    ui_keyboard_set_layout_cb(NULL);
    s_destroying = true;
    s_creating = false;
    s_phase2_pending = false;  /* Cancel Phase 2 if it hasn't fired yet */

    if (s_phase2_timer) { lv_timer_delete(s_phase2_timer); s_phase2_timer = NULL; }
    if (s_refresh_timer) { lv_timer_delete(s_refresh_timer); s_refresh_timer = NULL; }

    /* Flush pending NVS debounce timers — save current value before destroy */
    if (s_bright_save_timer) {
        brightness_save_cb(s_bright_save_timer);   /* commit now */
        lv_timer_delete(s_bright_save_timer);
        s_bright_save_timer = NULL;
    }
    if (s_vol_save_timer) {
        volume_save_cb(s_vol_save_timer);           /* commit now */
        lv_timer_delete(s_vol_save_timer);
        s_vol_save_timer = NULL;
    }

    lv_obj_delete(s_screen);

    s_screen        = NULL;
    s_scroll        = NULL;
    s_lbl_wifi      = NULL;
    s_lbl_bat_status = NULL;
    s_lbl_bat_volt  = NULL;
    s_bar_bat_level = NULL;
    s_lbl_bat_pct   = NULL;
    s_lbl_sd_info   = NULL;
    s_lbl_heap      = NULL;
    s_lbl_orient    = NULL;
    s_ntp_spinner   = NULL;
    s_ntp_btn_label = NULL;
    s_slider_bright = NULL;
    s_slider_volume = NULL;
    s_lbl_bright_val = NULL;
    s_lbl_vol_val    = NULL;
    s_sw_autorot    = NULL;
    s_tab_local     = NULL;
    s_tab_hybrid    = NULL;
    s_tab_cloud     = NULL;
    s_tab_tinkerclaw = NULL;
    s_local_card    = NULL;
    s_hybrid_card   = NULL;
    s_cloud_card    = NULL;
    s_tinkerclaw_card = NULL;
    memset(s_local_content,  0, sizeof(s_local_content));
    memset(s_hybrid_content, 0, sizeof(s_hybrid_content));
    memset(s_cloud_content,  0, sizeof(s_cloud_content));
    memset(s_tinkerclaw_content, 0, sizeof(s_tinkerclaw_content));
    s_dragon_ta     = NULL;
    s_dragon_status_lbl = NULL;
    s_ota_btn_label = NULL;
    s_ota_apply_btn = NULL;

    ESP_LOGI(TAG, "Settings screen destroyed");
}

lv_obj_t *ui_settings_get_screen(void) { return s_screen; }

void ui_settings_hide(void)
{
    s_creating = false;
    s_phase2_pending = false;  /* Cancel Phase 2 if it hasn't fired yet */
    ui_keyboard_set_layout_cb(NULL);

    if (s_phase2_timer) { lv_timer_delete(s_phase2_timer); s_phase2_timer = NULL; }

    /* Hide instead of destroy — rapid open/close cycles exhaust LVGL pool.
     * PAUSE refresh timer to prevent it updating hidden objects during
     * other overlay creation (Settings timer + Notes creation = WDT). */
    if (s_refresh_timer) { lv_timer_delete(s_refresh_timer); s_refresh_timer = NULL; }

    /* Flush pending NVS debounce timers before hiding */
    if (s_bright_save_timer) {
        brightness_save_cb(s_bright_save_timer);
        lv_timer_delete(s_bright_save_timer);
        s_bright_save_timer = NULL;
    }
    if (s_vol_save_timer) {
        volume_save_cb(s_vol_save_timer);
        lv_timer_delete(s_vol_save_timer);
        s_vol_save_timer = NULL;
    }

    if (s_screen) {
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    }
    s_destroying = false;
}
