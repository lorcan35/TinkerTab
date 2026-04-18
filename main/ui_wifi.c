/**
 * TinkerTab -- WiFi Configuration Screen
 *
 * Dark-themed WiFi settings for M5Stack Tab5 (720x1280 portrait).
 * LVGL v9 API, accent color #3B82F6.
 *
 * Features:
 *   - Current connection status (SSID, signal, IP)
 *   - Scan for available networks
 *   - Tap network -> password dialog with on-screen keyboard
 *   - Save credentials to NVS on successful connection
 */

#include "ui_wifi.h"
#include "ui_home.h"
#include "ui_core.h"
#include "ui_keyboard.h"
#include "wifi.h"
#include "settings.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_wifi";

/* ── Colors (match ui_settings.c palette) ──────────────────────────── */
#define COL_BG          lv_color_hex(0x0F0F23)
#define COL_TOPBAR      lv_color_hex(0x1A1A2E)
#define COL_ACCENT      lv_color_hex(0x3B82F6)
#define COL_ROW_EVEN    lv_color_hex(0x0F172A)
#define COL_ROW_ODD     lv_color_hex(0x1E293B)
#define COL_CARD        lv_color_hex(0x1C1C2E)
#define COL_TEXT        lv_color_hex(0xE2E8F0)
#define COL_TEXT_DIM    lv_color_hex(0x94A3B8)
#define COL_GREEN       lv_color_hex(0x22C55E)
#define COL_RED         lv_color_hex(0xEF4444)
#define COL_YELLOW      lv_color_hex(0xF59E0B)
#define COL_CYAN        lv_color_hex(0x00B4D8)
#define COL_WHITE       lv_color_hex(0xE8E8EF)
#define COL_OVERLAY     lv_color_hex(0x08080E)

/* ── Layout constants ──────────────────────────────────────────────── */
#define TOPBAR_H        48
#define STATUS_CARD_H   120
#define SIDE_PAD        16
#define CORNER_R        8
#define ROW_H           60
#define MAX_SCAN_APS    20

/* NVS persistence uses settings.h API (tab5_settings_set_wifi_ssid/pass) */

/* ── Screen state ──────────────────────────────────────────────────── */
static lv_obj_t *s_screen          = NULL;
static lv_obj_t *s_scroll          = NULL;
static lv_obj_t *s_lbl_status      = NULL;
static lv_obj_t *s_lbl_ssid        = NULL;
static lv_obj_t *s_lbl_signal      = NULL;
static lv_obj_t *s_lbl_ip          = NULL;
static lv_obj_t *s_scan_list       = NULL;
static lv_obj_t *s_scan_btn_label  = NULL;
static lv_obj_t *s_spinner         = NULL;

/* Password dialog */
static lv_obj_t *s_pwd_overlay     = NULL;
static lv_obj_t *s_pwd_dialog      = NULL;
static lv_obj_t *s_pwd_title       = NULL;
static lv_obj_t *s_pwd_ta          = NULL;
static lv_obj_t *s_pwd_connect_btn = NULL;
static lv_obj_t *s_pwd_status_lbl  = NULL;

/* Scan results storage */
static wifi_ap_record_t s_ap_records[MAX_SCAN_APS];
static uint16_t s_ap_count = 0;

/* Currently selected network for connection */
static char s_selected_ssid[33];
static int  s_row_idx = 0;

/* Guard flag for background tasks during destroy */
static volatile bool s_destroying = false;

/* Event handler for non-blocking scan completion */
static esp_event_handler_instance_t s_scan_done_handler = NULL;

/* ── Forward declarations ──────────────────────────────────────────── */
static lv_obj_t *make_topbar(lv_obj_t *parent);
static void      create_status_card(lv_obj_t *parent);
static void      create_scan_section(lv_obj_t *parent);
static void      populate_scan_list(void);
static void      show_password_dialog(const char *ssid);
static void      hide_password_dialog(void);
static void      scan_done_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data);
static void      wifi_connect_task(void *arg);
static const char *rssi_to_bars(int rssi);
static const char *auth_to_str(wifi_auth_mode_t auth);

/* NVS save uses the centralized settings API */
static esp_err_t wifi_nvs_save(const char *ssid, const char *pass)
{
    esp_err_t ret = tab5_settings_set_wifi_ssid(ssid);
    if (ret == ESP_OK) {
        ret = tab5_settings_set_wifi_pass(pass);
    }
    ESP_LOGI(TAG, "Settings save %s: %s", ssid, esp_err_to_name(ret));
    return ret;
}

/* ── Event callbacks ───────────────────────────────────────────────── */

static void cb_back_btn(lv_event_t *e)
{
    (void)e;
    ui_wifi_destroy();
    lv_screen_load(ui_home_get_screen());
}

static void cb_scan_btn(lv_event_t *e)
{
    (void)e;
    if (s_spinner) return; /* scan already in progress */

    if (s_scan_btn_label) {
        lv_label_set_text(s_scan_btn_label, "Scanning...");
    }

    /* Non-blocking scan — results arrive via WIFI_EVENT_SCAN_DONE */
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);  /* non-blocking */
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        if (s_scan_btn_label) lv_label_set_text(s_scan_btn_label, "Scan Failed");
    } else {
        ESP_LOGI(TAG, "WiFi scan started (non-blocking)");
    }
}

static void cb_network_row(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ap_count) return;

    const char *ssid = (const char *)s_ap_records[idx].ssid;
    ESP_LOGI(TAG, "Selected network: %s", ssid);

    /* Open networks: connect directly. Secured: show password dialog. */
    if (s_ap_records[idx].authmode == WIFI_AUTH_OPEN) {
        strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
        s_selected_ssid[sizeof(s_selected_ssid) - 1] = '\0';
        /* Connect with empty password */
        xTaskCreate(wifi_connect_task, "wifi_conn", 8192, strdup(""), 5, NULL);
    } else {
        show_password_dialog(ssid);
    }
}

static void cb_pwd_connect(lv_event_t *e)
{
    (void)e;
    if (!s_pwd_ta) return;

    const char *pwd = lv_textarea_get_text(s_pwd_ta);
    if (strlen(pwd) < 8) {
        if (s_pwd_status_lbl) {
            lv_label_set_text(s_pwd_status_lbl, "Password must be 8+ characters");
            lv_obj_set_style_text_color(s_pwd_status_lbl, COL_RED, 0);
        }
        return;
    }

    if (s_pwd_status_lbl) {
        lv_label_set_text(s_pwd_status_lbl, "Connecting...");
        lv_obj_set_style_text_color(s_pwd_status_lbl, COL_CYAN, 0);
    }

    /* Disable button while connecting */
    if (s_pwd_connect_btn) {
        lv_obj_add_state(s_pwd_connect_btn, LV_STATE_DISABLED);
    }

    ui_keyboard_hide();

    char *pwd_copy = strdup(pwd);
    xTaskCreate(wifi_connect_task, "wifi_conn", 8192, pwd_copy, 5, NULL);
}

static void cb_pwd_cancel(lv_event_t *e)
{
    (void)e;
    ui_keyboard_hide();
    hide_password_dialog();
}

static void cb_pwd_ta_clicked(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    ui_keyboard_show(ta);
}

/* ── WiFi scan background task ─────────────────────────────────────── */

/* Deferred UI update — runs in LVGL timer context where the lock is safe */
static void scan_done_update_ui(void *arg)
{
    (void)arg;
    if (s_destroying || !s_screen) return;

    populate_scan_list();
    if (s_scan_btn_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Scan (%d found)", s_ap_count);
        lv_label_set_text(s_scan_btn_label, buf);
    }
}

/* Called on the system event loop task when scan completes.
 *
 * CRITICAL FIX (issue #8): Must NOT call tab5_ui_lock() (portMAX_DELAY)
 * here — that blocks the event loop forever if LVGL holds the mutex,
 * which starves the HTTP debug server and WiFi driver, causing the
 * permanent hang reported in #8.
 *
 * Fix: try_lock with a short timeout, fall back to lv_async_call. */
static void scan_done_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg; (void)event_base; (void)event_id; (void)event_data;

    if (s_destroying || !s_screen) return;

    /* Fetch scan results — safe on event loop, no LVGL involved */
    uint16_t count = MAX_SCAN_APS;
    esp_err_t ret = esp_wifi_scan_get_ap_records(&count, s_ap_records);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Scan get records failed: %s", esp_err_to_name(ret));
        count = 0;
    }
    s_ap_count = count;
    ESP_LOGI(TAG, "Scan found %d networks", s_ap_count);

    /* Try short lock; on failure defer to LVGL task via async call */
    if (tab5_ui_try_lock(200)) {
        if (s_destroying || !s_screen) { tab5_ui_unlock(); return; }
        populate_scan_list();
        if (s_scan_btn_label) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Scan (%d found)", s_ap_count);
            lv_label_set_text(s_scan_btn_label, buf);
        }
        tab5_ui_unlock();
    } else {
        ESP_LOGW(TAG, "LVGL lock busy in scan_done — deferring to LVGL task");
        lv_async_call(scan_done_update_ui, NULL);
    }
}

/* ── WiFi connect background task ──────────────────────────────────── */

static void wifi_connect_task(void *arg)
{
    char *password = (char *)arg;

    if (s_destroying) { free(password); vTaskSuspend(NULL); return; }

    ESP_LOGI(TAG, "Connecting to '%s'...", s_selected_ssid);

    /* Disconnect current connection */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Configure new connection */
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memset(wifi_cfg.sta.ssid, 0, sizeof(wifi_cfg.sta.ssid));
    memcpy(wifi_cfg.sta.ssid, s_selected_ssid,
           strlen(s_selected_ssid) < sizeof(wifi_cfg.sta.ssid) ? strlen(s_selected_ssid) : sizeof(wifi_cfg.sta.ssid) - 1);
    if (password && strlen(password) > 0) {
        memset(wifi_cfg.sta.password, 0, sizeof(wifi_cfg.sta.password));
        memcpy(wifi_cfg.sta.password, password,
               strlen(password) < sizeof(wifi_cfg.sta.password) ? strlen(password) : sizeof(wifi_cfg.sta.password) - 1);
    } else {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set config failed: %s", esp_err_to_name(ret));
        if (tab5_ui_try_lock(2000)) {
            if (s_pwd_status_lbl) {
                lv_label_set_text(s_pwd_status_lbl, "Config error");
                lv_obj_set_style_text_color(s_pwd_status_lbl, COL_RED, 0);
            }
            if (s_pwd_connect_btn) lv_obj_clear_state(s_pwd_connect_btn, LV_STATE_DISABLED);
            tab5_ui_unlock();
        }
        free(password);
        vTaskSuspend(NULL);
        return;
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Connect failed: %s", esp_err_to_name(ret));
        if (tab5_ui_try_lock(2000)) {
            if (s_pwd_status_lbl) {
                lv_label_set_text(s_pwd_status_lbl, "Connection failed");
                lv_obj_set_style_text_color(s_pwd_status_lbl, COL_RED, 0);
            }
            if (s_pwd_connect_btn) lv_obj_clear_state(s_pwd_connect_btn, LV_STATE_DISABLED);
            tab5_ui_unlock();
        }
        free(password);
        vTaskSuspend(NULL);
        return;
    }

    /* Wait for connection (up to 10 seconds) */
    bool connected = false;
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (tab5_wifi_connected()) {
            connected = true;
            break;
        }
    }

    if (connected) {
        ESP_LOGI(TAG, "Connected to %s!", s_selected_ssid);
        /* Save to NVS — no lock needed */
        wifi_nvs_save(s_selected_ssid, password ? password : "");
    } else {
        ESP_LOGW(TAG, "Connection to %s timed out", s_selected_ssid);
    }

    /* Update password dialog UI — use try_lock to avoid deadlock */
    if (tab5_ui_try_lock(2000)) {
        if (!s_destroying && s_screen) {
            if (connected) {
                if (s_pwd_status_lbl) {
                    lv_label_set_text(s_pwd_status_lbl, "Connected!");
                    lv_obj_set_style_text_color(s_pwd_status_lbl, COL_GREEN, 0);
                }
            } else {
                if (s_pwd_status_lbl) {
                    lv_label_set_text(s_pwd_status_lbl, "Connection timed out");
                    lv_obj_set_style_text_color(s_pwd_status_lbl, COL_RED, 0);
                }
            }
            if (s_pwd_connect_btn) lv_obj_clear_state(s_pwd_connect_btn, LV_STATE_DISABLED);
        }
        tab5_ui_unlock();
    } else {
        ESP_LOGW(TAG, "wifi_connect_task: LVGL lock timeout, skipping dialog update");
    }

    /* Update status card — ui_wifi_update manages its own lock */
    if (!s_destroying && s_screen) {
        ui_wifi_update();
    }

    free(password);
    vTaskSuspend(NULL);
}

/* ── RSSI to signal bars ───────────────────────────────────────────── */

static const char *rssi_to_bars(int rssi)
{
    if (rssi >= -50) return LV_SYMBOL_WIFI "  ";     /* excellent */
    if (rssi >= -60) return LV_SYMBOL_WIFI " ";      /* good */
    if (rssi >= -70) return LV_SYMBOL_WIFI;          /* fair */
    return LV_SYMBOL_WIFI;                           /* weak */
}

static const char *auth_to_str(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:            return "Open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    default:                        return "Secured";
    }
}

/* ── Helper: top bar ───────────────────────────────────────────────── */

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
    lv_label_set_text(title, "WiFi Settings");
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

/* ── Helper: connection status card ────────────────────────────────── */

static void create_status_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 720 - 2 * SIDE_PAD, STATUS_CARD_H);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Status line: icon + "Connected" / "Disconnected" */
    lv_obj_t *status_row = lv_obj_create(card);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 8, 0);

    lv_obj_t *wifi_icon = lv_label_create(status_row);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_icon, COL_ACCENT, 0);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_24, 0);

    s_lbl_status = lv_label_create(status_row);
    lv_label_set_text(s_lbl_status, "Checking...");
    lv_obj_set_style_text_color(s_lbl_status, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_20, 0);

    /* SSID */
    s_lbl_ssid = lv_label_create(card);
    lv_label_set_text(s_lbl_ssid, "SSID: --");
    lv_obj_set_style_text_color(s_lbl_ssid, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_lbl_ssid, &lv_font_montserrat_18, 0);

    /* Signal + IP row */
    lv_obj_t *info_row = lv_obj_create(card);
    lv_obj_remove_style_all(info_row);
    lv_obj_set_size(info_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lbl_signal = lv_label_create(info_row);
    lv_label_set_text(s_lbl_signal, "Signal: --");
    lv_obj_set_style_text_color(s_lbl_signal, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_lbl_signal, &lv_font_montserrat_18, 0);

    s_lbl_ip = lv_label_create(info_row);
    lv_label_set_text(s_lbl_ip, "IP: --");
    lv_obj_set_style_text_color(s_lbl_ip, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_lbl_ip, &lv_font_montserrat_18, 0);
}

/* ── Helper: scan section ──────────────────────────────────────────── */

static void create_scan_section(lv_obj_t *parent)
{
    /* Section header row */
    lv_obj_t *hdr_row = lv_obj_create(parent);
    lv_obj_remove_style_all(hdr_row);
    lv_obj_set_size(hdr_row, 720 - 2 * SIDE_PAD, 48);
    lv_obj_set_flex_flow(hdr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hdr_lbl = lv_label_create(hdr_row);
    lv_label_set_text(hdr_lbl, "Available Networks");
    lv_obj_set_style_text_color(hdr_lbl, COL_ACCENT, 0);
    lv_obj_set_style_text_font(hdr_lbl, &lv_font_montserrat_20, 0);

    /* Scan button */
    lv_obj_t *scan_btn = lv_button_create(hdr_row);
    lv_obj_set_size(scan_btn, 160, 48);
    lv_obj_set_style_bg_color(scan_btn, COL_ACCENT, 0);
    lv_obj_set_style_radius(scan_btn, 6, 0);
    lv_obj_add_event_cb(scan_btn, cb_scan_btn, LV_EVENT_CLICKED, NULL);

    s_scan_btn_label = lv_label_create(scan_btn);
    lv_label_set_text(s_scan_btn_label, "Scan");
    lv_obj_set_style_text_color(s_scan_btn_label, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_scan_btn_label, &lv_font_montserrat_18, 0);
    lv_obj_center(s_scan_btn_label);

    /* Scan results list container */
    s_scan_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scan_list);
    lv_obj_set_size(s_scan_list, 720 - 2 * SIDE_PAD, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scan_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_scan_list, 2, 0);

    /* Placeholder */
    lv_obj_t *placeholder = lv_label_create(s_scan_list);
    lv_label_set_text(placeholder, "Tap Scan to find networks");
    lv_obj_set_style_text_color(placeholder, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_18, 0);
    lv_obj_set_style_pad_top(placeholder, 20, 0);
}

/* ── Populate scan list from s_ap_records ──────────────────────────── */

static void populate_scan_list(void)
{
    if (!s_scan_list) return;

    /* Clear existing children */
    lv_obj_clean(s_scan_list);
    s_row_idx = 0;

    if (s_ap_count == 0) {
        lv_obj_t *empty = lv_label_create(s_scan_list);
        lv_label_set_text(empty, "No networks found");
        lv_obj_set_style_text_color(empty, COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, 0);
        lv_obj_set_style_pad_top(empty, 20, 0);
        return;
    }

    for (int i = 0; i < s_ap_count; i++) {
        wifi_ap_record_t *ap = &s_ap_records[i];

        /* Skip hidden SSIDs */
        if (strlen((char *)ap->ssid) == 0) continue;

        lv_obj_t *row = lv_obj_create(s_scan_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 720 - 2 * SIDE_PAD, ROW_H);
        lv_obj_set_style_bg_color(row, (s_row_idx % 2 == 0) ? COL_ROW_EVEN : COL_ROW_ODD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, CORNER_R, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb_network_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* Left: signal icon + SSID */
        lv_obj_t *left = lv_obj_create(row);
        lv_obj_remove_style_all(left);
        lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left, 10, 0);

        /* Signal strength indicator */
        lv_obj_t *sig = lv_label_create(left);
        lv_label_set_text(sig, rssi_to_bars(ap->rssi));
        lv_color_t sig_col = (ap->rssi >= -50) ? COL_GREEN :
                             (ap->rssi >= -70) ? COL_YELLOW : COL_RED;
        lv_obj_set_style_text_color(sig, sig_col, 0);
        lv_obj_set_style_text_font(sig, &lv_font_montserrat_18, 0);

        /* SSID */
        lv_obj_t *ssid_lbl = lv_label_create(left);
        lv_label_set_text(ssid_lbl, (char *)ap->ssid);
        lv_obj_set_style_text_color(ssid_lbl, COL_TEXT, 0);
        lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_18, 0);
        lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(ssid_lbl, 360);

        /* Right: auth + RSSI */
        lv_obj_t *right = lv_obj_create(row);
        lv_obj_remove_style_all(right);
        lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(right, 10, 0);

        char rssi_buf[16];
        snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", ap->rssi);
        lv_obj_t *rssi_lbl = lv_label_create(right);
        lv_label_set_text(rssi_lbl, rssi_buf);
        lv_obj_set_style_text_color(rssi_lbl, COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_16, 0);

        if (ap->authmode != WIFI_AUTH_OPEN) {
            lv_obj_t *auth_lbl = lv_label_create(right);
            lv_label_set_text(auth_lbl, auth_to_str(ap->authmode));
            lv_obj_set_style_text_color(auth_lbl, COL_TEXT_DIM, 0);
            lv_obj_set_style_text_font(auth_lbl, &lv_font_montserrat_16, 0);

            lv_obj_t *lock_lbl = lv_label_create(right);
            lv_label_set_text(lock_lbl, LV_SYMBOL_EYE_CLOSE);
            lv_obj_set_style_text_color(lock_lbl, COL_TEXT_DIM, 0);
            lv_obj_set_style_text_font(lock_lbl, &lv_font_montserrat_16, 0);
        }

        s_row_idx++;
    }
}

/* ── Password dialog ───────────────────────────────────────────────── */

static void show_password_dialog(const char *ssid)
{
    if (s_pwd_overlay) return; /* already showing */

    strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
    s_selected_ssid[sizeof(s_selected_ssid) - 1] = '\0';

    /* Semi-transparent overlay */
    s_pwd_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_pwd_overlay);
    lv_obj_set_size(s_pwd_overlay, 720, 1280);
    lv_obj_set_style_bg_color(s_pwd_overlay, COL_OVERLAY, 0);
    lv_obj_set_style_bg_opa(s_pwd_overlay, LV_OPA_70, 0);
    lv_obj_align(s_pwd_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_pwd_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pwd_overlay, cb_pwd_cancel, LV_EVENT_CLICKED, NULL);

    /* Dialog card */
    s_pwd_dialog = lv_obj_create(s_pwd_overlay);
    lv_obj_remove_style_all(s_pwd_dialog);
    lv_obj_set_size(s_pwd_dialog, 640, 320);
    lv_obj_set_style_bg_color(s_pwd_dialog, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(s_pwd_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pwd_dialog, 16, 0);
    lv_obj_set_style_pad_all(s_pwd_dialog, 24, 0);
    lv_obj_align(s_pwd_dialog, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_set_flex_flow(s_pwd_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pwd_dialog, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_pwd_dialog, 12, 0);
    lv_obj_clear_flag(s_pwd_dialog, LV_OBJ_FLAG_SCROLLABLE);
    /* Prevent clicks from passing through to overlay */
    lv_obj_add_flag(s_pwd_dialog, LV_OBJ_FLAG_CLICKABLE);

    /* Title */
    s_pwd_title = lv_label_create(s_pwd_dialog);
    char title_buf[64];
    snprintf(title_buf, sizeof(title_buf), "Connect to %s", ssid);
    lv_label_set_text(s_pwd_title, title_buf);
    lv_obj_set_style_text_color(s_pwd_title, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_pwd_title, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(s_pwd_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_pwd_title, 580);

    /* Password textarea */
    s_pwd_ta = lv_textarea_create(s_pwd_dialog);
    lv_obj_set_size(s_pwd_ta, 580, 48);
    lv_textarea_set_placeholder_text(s_pwd_ta, "Enter password");
    lv_textarea_set_password_mode(s_pwd_ta, true);
    lv_textarea_set_max_length(s_pwd_ta, 63);
    lv_textarea_set_one_line(s_pwd_ta, true);
    lv_obj_set_style_bg_color(s_pwd_ta, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(s_pwd_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_pwd_ta, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_pwd_ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_border_color(s_pwd_ta, COL_ACCENT, 0);
    lv_obj_set_style_border_width(s_pwd_ta, 2, 0);
    lv_obj_set_style_radius(s_pwd_ta, 8, 0);
    lv_obj_set_style_pad_all(s_pwd_ta, 10, 0);
    lv_obj_add_event_cb(s_pwd_ta, cb_pwd_ta_clicked, LV_EVENT_CLICKED, NULL);

    /* Button row */
    lv_obj_t *btn_row = lv_obj_create(s_pwd_dialog);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, 580, 48);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 16, 0);

    /* Cancel button */
    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_set_size(cancel_btn, 140, 48);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x334155), 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, cb_pwd_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(cancel_lbl);

    /* Connect button */
    s_pwd_connect_btn = lv_button_create(btn_row);
    lv_obj_set_size(s_pwd_connect_btn, 160, 48);
    lv_obj_set_style_bg_color(s_pwd_connect_btn, COL_ACCENT, 0);
    lv_obj_set_style_radius(s_pwd_connect_btn, 8, 0);
    lv_obj_add_event_cb(s_pwd_connect_btn, cb_pwd_connect, LV_EVENT_CLICKED, NULL);

    lv_obj_t *connect_lbl = lv_label_create(s_pwd_connect_btn);
    lv_label_set_text(connect_lbl, "Connect");
    lv_obj_set_style_text_color(connect_lbl, COL_WHITE, 0);
    lv_obj_set_style_text_font(connect_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(connect_lbl);

    /* Status label */
    s_pwd_status_lbl = lv_label_create(s_pwd_dialog);
    lv_label_set_text(s_pwd_status_lbl, "");
    lv_obj_set_style_text_color(s_pwd_status_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_pwd_status_lbl, &lv_font_montserrat_16, 0);

    /* Auto-open keyboard targeting the textarea */
    ui_keyboard_show(s_pwd_ta);
}

static void hide_password_dialog(void)
{
    if (s_pwd_overlay) {
        lv_obj_delete(s_pwd_overlay);
        s_pwd_overlay     = NULL;
        s_pwd_dialog      = NULL;
        s_pwd_title       = NULL;
        s_pwd_ta          = NULL;
        s_pwd_connect_btn = NULL;
        s_pwd_status_lbl  = NULL;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *ui_wifi_create(void)
{
    if (s_screen) {
        ESP_LOGW(TAG, "WiFi screen already exists");
        return s_screen;
    }

    /* ── Screen ────────────────────────────────────────────────────── */
    s_screen = lv_obj_create(NULL);
    if (!s_screen) {
        ESP_LOGE(TAG, "OOM: failed to create WiFi screen");
        return NULL;
    }
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Top bar ───────────────────────────────────────────────────── */
    make_topbar(s_screen);

    /* ── Scrollable body ───────────────────────────────────────────── */
    s_scroll = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, 720, 1280 - TOPBAR_H);
    lv_obj_align(s_scroll, LV_ALIGN_TOP_LEFT, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(s_scroll, SIDE_PAD, 0);
    lv_obj_set_style_pad_top(s_scroll, 12, 0);
    lv_obj_set_style_pad_bottom(s_scroll, 32, 0);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_style_pad_row(s_scroll, 8, 0);

    /* ── Connection status card ────────────────────────────────────── */
    create_status_card(s_scroll);

    /* ── Scan section ──────────────────────────────────────────────── */
    create_scan_section(s_scroll);

    /* ── Initial data refresh ──────────────────────────────────────── */
    ui_wifi_update();

    s_destroying = false;

    /* Register scan-done event handler (non-blocking scan) */
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                        scan_done_handler, NULL,
                                        &s_scan_done_handler);

    /* ── Load the screen ───────────────────────────────────────────── */
    /* NOTE: lv_screen_load_anim(MOVE_LEFT) causes infinite loop in LVGL
     * when transitioning from tileview. Use direct load instead. */
    lv_screen_load(s_screen);

    ESP_LOGI(TAG, "WiFi screen created");
    return s_screen;
}

void ui_wifi_update(void)
{
    if (!s_screen) return;

    /* Phase 1: Query WiFi state OUTSIDE the LVGL lock.
     * Calling esp_wifi_sta_get_ap_info / esp_netif_get_ip_info while
     * holding the LVGL mutex deadlocks: the WiFi driver may post to
     * the system event loop, which can be blocked in scan_done_handler
     * waiting for the same mutex.  This was the root cause of #8. */
    bool connected = tab5_wifi_connected();
    wifi_ap_record_t ap_info = {0};
    bool have_ap_info = false;
    char ip_buf[32] = "IP: --";

    if (connected) {
        have_ap_info = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                snprintf(ip_buf, sizeof(ip_buf), "IP: " IPSTR, IP2STR(&ip_info.ip));
            }
        }
    }

    /* Phase 2: Update LVGL widgets under the lock.
     * Recursive mutex so safe if caller already holds it (e.g. ui_wifi_create).
     * Uses try_lock so we never block forever. */
    if (!tab5_ui_try_lock(1000)) {
        ESP_LOGW(TAG, "ui_wifi_update: LVGL lock timeout — skipping UI refresh");
        return;
    }

    if (!s_screen) { tab5_ui_unlock(); return; }

    if (connected) {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, "Connected");
            lv_obj_set_style_text_color(s_lbl_status, COL_GREEN, 0);
        }

        if (have_ap_info) {
            if (s_lbl_ssid) {
                char buf[48];
                snprintf(buf, sizeof(buf), "SSID: %s", (char *)ap_info.ssid);
                lv_label_set_text(s_lbl_ssid, buf);
            }
            if (s_lbl_signal) {
                char buf[32];
                const char *quality = (ap_info.rssi >= -50) ? "Excellent" :
                                      (ap_info.rssi >= -60) ? "Good" :
                                      (ap_info.rssi >= -70) ? "Fair" : "Weak";
                snprintf(buf, sizeof(buf), "Signal: %s (%d dBm)", quality, ap_info.rssi);
                lv_label_set_text(s_lbl_signal, buf);
            }
        }

        if (s_lbl_ip) {
            lv_label_set_text(s_lbl_ip, ip_buf);
        }
    } else {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, "Disconnected");
            lv_obj_set_style_text_color(s_lbl_status, COL_RED, 0);
        }
        if (s_lbl_ssid)   lv_label_set_text(s_lbl_ssid, "SSID: --");
        if (s_lbl_signal)  lv_label_set_text(s_lbl_signal, "Signal: --");
        if (s_lbl_ip)      lv_label_set_text(s_lbl_ip, "IP: --");
    }

    tab5_ui_unlock();
}

void ui_wifi_destroy(void)
{
    if (!s_screen) return;

    s_destroying = true;

    /* Unregister scan-done event before destroying UI */
    if (s_scan_done_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              s_scan_done_handler);
        s_scan_done_handler = NULL;
    }

    ui_keyboard_hide();
    hide_password_dialog();

    lv_obj_delete(s_screen);

    s_screen         = NULL;
    s_scroll         = NULL;
    s_lbl_status     = NULL;
    s_lbl_ssid       = NULL;
    s_lbl_signal     = NULL;
    s_lbl_ip         = NULL;
    s_scan_list      = NULL;
    s_scan_btn_label = NULL;
    s_spinner        = NULL;

    ESP_LOGI(TAG, "WiFi screen destroyed");
}
