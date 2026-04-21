/**
 * TinkerTab — HTTP Debug Server
 *
 * Serves screenshots (BMP), device info, touch injection, and a live
 * remote-control HTML page over HTTP on port 8080.
 *
 * The BMP screenshot reads the DPI framebuffer (720x1280 RGB565 in PSRAM),
 * flushes the CPU cache first (esp_cache_msync M2C) so we see the latest
 * DMA data, then streams a bottom-up BMP with RGB565 bitmasks.
 */

#include "debug_server.h"
#include "config.h"
#include "esp_random.h"
#include "display.h"
#include "sdcard.h"
#include "afe.h"
#include "voice.h"
#include "ota.h"
#include "camera.h"
#include "settings.h"
#include "ui_core.h"
#include "ui_core.h"
#include "ui_wifi.h"
#include "ui_camera.h"
#include "ui_settings.h"
#include "ui_files.h"
#include "ui_home.h"
#include "ui_chat.h"
#include "ui_keyboard.h"
#include "wifi.h"
#include "battery.h"
#include "dragon_link.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_netif.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "driver/jpeg_encode.h"

static const char *TAG = "debug_srv";

#define DEBUG_PORT 8080

/* Wave 15 W15-C02: file-scoped httpd handle so `tab5_debug_server_stop()`
 * can actually stop the server on demand.  The original code stashed the
 * handle in a function-local and leaked it — making the server a one-shot
 * with no recovery path. */
static httpd_handle_t s_httpd = NULL;

/* ======================================================================== */
/*  Bearer token authentication                                              */
/* ======================================================================== */
static char s_auth_token[33] = {0}; /* 32 hex chars + null */

static void init_auth_token(void)
{
    /* Try reading from NVS */
    if (tab5_settings_get_auth_token(s_auth_token, sizeof(s_auth_token)) == ESP_OK && s_auth_token[0]) {
        ESP_LOGI(TAG, "Auth token loaded from NVS");
    } else {
        /* Generate new random token */
        uint32_t r[4];
        for (int i = 0; i < 4; i++) r[i] = esp_random();
        snprintf(s_auth_token, sizeof(s_auth_token), "%08lx%08lx%08lx%08lx",
                 (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2], (unsigned long)r[3]);
        tab5_settings_set_auth_token(s_auth_token);
        ESP_LOGI(TAG, "Generated new auth token");
    }
    /* v4·D audit P1 fix: don't dump the full token on every boot log.
     * The user can still recover it via serial by reading NVS, but
     * anyone over-shoulder-watching the console doesn't get it. */
    size_t tl = strlen(s_auth_token);
    ESP_LOGI(TAG, "Debug server auth token: %.*s****%.*s (%u chars, masked)",
             (int)(tl >= 4 ? 4 : tl), s_auth_token,
             (int)(tl >= 4 ? 4 : 0), s_auth_token + (tl >= 4 ? tl - 4 : 0),
             (unsigned)tl);
}

/* v4·D audit P1 fix: constant-time string compare for auth.  The
 * previous strcmp leaked information about the prefix match length
 * via response timing -- a local attacker on the LAN could use that
 * to narrow down the 32-hex-char token with O(16) probes per position.
 * This path iterates ALL bytes regardless of match -- ~100 ns extra. */
static int ct_token_cmp(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t n  = la > lb ? la : lb;
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned)(ca ^ cb);
    }
    return diff == 0 ? 0 : 1;
}

/**
 * Check Authorization: Bearer <token> header.
 * Returns true if authorized, false if 401 was sent.
 */
static bool check_auth(httpd_req_t *req)
{
    char auth_header[80] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Authorization required\"}");
        return false;
    }
    /* Expect "Bearer <token>" */
    if (strncmp(auth_header, "Bearer ", 7) != 0
        || ct_token_cmp(auth_header + 7, s_auth_token) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid token\"}");
        return false;
    }
    return true;
}

/* ======================================================================== */
/*  Touch injection state — read by the LVGL indev read callback             */
/* ======================================================================== */
static volatile bool     s_inject_active = false;
static volatile int32_t  s_inject_x = 0;
static volatile int32_t  s_inject_y = 0;
static volatile bool     s_inject_pressed = false;

bool tab5_debug_touch_override(int32_t *x, int32_t *y, bool *pressed)
{
    if (!s_inject_active) return false;
    *x = s_inject_x;
    *y = s_inject_y;
    *pressed = s_inject_pressed;
    return true;
}

#define FB_W       TAB5_DISPLAY_WIDTH   /* 720  */
#define FB_H       TAB5_DISPLAY_HEIGHT  /* 1280 */
#define FB_BPP     2                    /* RGB565 = 2 bytes/pixel */

/* ======================================================================== */
/*  BMP helpers                                                              */
/* ======================================================================== */

/* BMP file header (14) + BITMAPINFOHEADER (40) + 3x DWORD color masks (12) = 66 bytes */
#define BMP_HEADER_SIZE 66

static void build_bmp_header(uint8_t *hdr, int w, int h)
{
    uint32_t row_bytes = w * FB_BPP;
    /* BMP rows are padded to 4-byte boundary — 720*2=1440 is already aligned */
    uint32_t pixel_size = row_bytes * h;
    uint32_t file_size  = BMP_HEADER_SIZE + pixel_size;

    memset(hdr, 0, BMP_HEADER_SIZE);

    /* -- BMP File Header (14 bytes) -- */
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &file_size, 4);
    /* reserved = 0 (already) */
    uint32_t offset = BMP_HEADER_SIZE;
    memcpy(hdr + 10, &offset, 4);

    /* -- DIB Header: BITMAPINFOHEADER (40 bytes) -- */
    uint32_t dib_size = 40;
    memcpy(hdr + 14, &dib_size, 4);
    int32_t bmp_w = w;
    int32_t bmp_h = h;  /* positive = bottom-up */
    memcpy(hdr + 18, &bmp_w, 4);
    memcpy(hdr + 22, &bmp_h, 4);
    uint16_t planes = 1;
    memcpy(hdr + 26, &planes, 2);
    uint16_t bpp = 16;
    memcpy(hdr + 28, &bpp, 2);
    uint32_t compression = 3;  /* BI_BITFIELDS */
    memcpy(hdr + 30, &compression, 4);
    memcpy(hdr + 34, &pixel_size, 4);
    /* ppm, colors, important = 0 (already) */

    /* -- RGB565 color masks (12 bytes) -- */
    uint32_t mask_r = 0x0000F800;  /* bits 15-11 */
    uint32_t mask_g = 0x000007E0;  /* bits 10-5  */
    uint32_t mask_b = 0x0000001F;  /* bits 4-0   */
    memcpy(hdr + 54, &mask_r, 4);
    memcpy(hdr + 58, &mask_g, 4);
    memcpy(hdr + 62, &mask_b, 4);
}

/* ======================================================================== */
/*  GET /screenshot  and  GET /screenshot.bmp                                */
/* ======================================================================== */

/* Lazily-initialised hardware JPEG encoder. One engine is reused across
 * requests, serialised by s_jpeg_mux because jpeg_encoder_process is not
 * safe to share across concurrent callers. */
#include "freertos/semphr.h"
static jpeg_encoder_handle_t s_jpeg_enc = NULL;
static SemaphoreHandle_t     s_jpeg_mux = NULL;

static esp_err_t _ensure_jpeg_encoder(void)
{
    if (s_jpeg_enc) return ESP_OK;
    if (!s_jpeg_mux) {
        s_jpeg_mux = xSemaphoreCreateMutex();
        if (!s_jpeg_mux) return ESP_ERR_NO_MEM;
    }
    jpeg_encode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms    = 5000,
    };
    esp_err_t ret = jpeg_new_encoder_engine(&cfg, &s_jpeg_enc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(ret));
        s_jpeg_enc = NULL;
    }
    return ret;
}

static esp_err_t screenshot_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    esp_lcd_panel_handle_t panel = tab5_display_get_panel();
    if (!panel) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Display not initialized");
        return ESP_FAIL;
    }

    void *fb = NULL;
    esp_err_t ret = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb);
    if (ret != ESP_OK || !fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot get framebuffer");
        return ESP_FAIL;
    }

    if (_ensure_jpeg_encoder() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encoder init failed");
        return ESP_FAIL;
    }

    size_t fb_size = FB_W * FB_H * FB_BPP;

    /* DMA-aligned input buffer for the JPEG engine. */
    jpeg_encode_memory_alloc_cfg_t in_alloc = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    size_t in_capacity = 0;
    uint8_t *in_buf = jpeg_alloc_encoder_mem(fb_size, &in_alloc, &in_capacity);
    if (!in_buf || in_capacity < fb_size) {
        if (in_buf) free(in_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG input alloc failed");
        return ESP_FAIL;
    }

    /* Output buffer — 256 KB is plenty for a 720x1280 UI screenshot at q80
     * (measured ~60-120 KB for dark-UI content). */
    const size_t out_cap = 256 * 1024;
    jpeg_encode_memory_alloc_cfg_t out_alloc = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t out_capacity = 0;
    uint8_t *out_buf = jpeg_alloc_encoder_mem(out_cap, &out_alloc, &out_capacity);
    if (!out_buf) {
        free(in_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG output alloc failed");
        return ESP_FAIL;
    }

    /* HW03: Lock LVGL for the copy, unlock before encode (encode is long
     * but operates on our copy, so LVGL is free to keep rendering). */
    if (!tab5_ui_try_lock(2000)) {
        ESP_LOGW(TAG, "Screenshot: LVGL lock timeout (2s) — returning 503");
        free(in_buf); free(out_buf);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"display busy — LVGL lock timeout\"}");
        return ESP_OK;
    }
    esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(in_buf, fb, fb_size);
    tab5_ui_unlock();

    /* Encode. Serialize concurrent /screenshot calls — the engine is not
     * safe to share across simultaneous jpeg_encoder_process invocations. */
    jpeg_encode_cfg_t enc_cfg = {
        .height        = FB_H,
        .width         = FB_W,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = 80,
    };
    uint32_t out_size = 0;
    xSemaphoreTake(s_jpeg_mux, portMAX_DELAY);
    ret = jpeg_encoder_process(s_jpeg_enc, &enc_cfg, in_buf, fb_size,
                               out_buf, out_capacity, &out_size);
    xSemaphoreGive(s_jpeg_mux);

    free(in_buf);

    if (ret != ESP_OK || out_size == 0) {
        ESP_LOGE(TAG, "jpeg_encoder_process: %s out=%u", esp_err_to_name(ret), (unsigned)out_size);
        free(out_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encode failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    /* Typical payload 60-150 KB — fits comfortably in a single send with
     * the 32 KB LWIP window. No chunking needed. */
    ret = httpd_resp_send(req, (const char *)out_buf, out_size);
    free(out_buf);
    return ret;
}

/* ======================================================================== */
/*  GET /info                                                                */
/* ======================================================================== */

static esp_err_t get_wifi_ip(char *buf, size_t len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        snprintf(buf, len, "0.0.0.0");
        return ESP_FAIL;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
        return ESP_OK;
    }
    snprintf(buf, len, "0.0.0.0");
    return ESP_FAIL;
}

static esp_err_t info_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "heap_free",  (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min",   (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "uptime_ms",  (double)(esp_timer_get_time() / 1000));
    cJSON_AddBoolToObject(root,   "wifi_connected", tab5_wifi_connected());

    char ip[20];
    get_wifi_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(root, "wifi_ip", ip);

    dragon_state_t ds = tab5_dragon_get_state();
    cJSON_AddBoolToObject(root, "dragon_connected",
        ds == DRAGON_STATE_CONNECTED || ds == DRAGON_STATE_STREAMING);
    /* N2: voice_connected reflects actual voice WS state (port 3502),
     * while dragon_connected reflects CDP link state (port 3501) */
    cJSON_AddBoolToObject(root, "voice_connected", voice_is_connected());

    char display_str[16];
    snprintf(display_str, sizeof(display_str), "%dx%d", FB_W, FB_H);
    cJSON_AddStringToObject(root, "display", display_str);

    cJSON_AddNumberToObject(root, "fps", (double)tab5_dragon_get_fps());
    cJSON_AddNumberToObject(root, "lvgl_fps", (double)ui_core_get_fps());

    tab5_battery_info_t bat = {0};
    tab5_battery_read(&bat);
    cJSON_AddNumberToObject(root, "battery_pct", (double)bat.percent);

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    cJSON_AddNumberToObject(root, "tasks", (double)task_count);

    /* AFE / wake word — parked. Kept in /info so external monitors can
     * detect the parked state, but the fields always read false/empty. */
    cJSON_AddBoolToObject(root, "afe_active", false);
    cJSON_AddBoolToObject(root, "wake_listening", false);
    cJSON_AddStringToObject(root, "wake_word", "parked");

    /* SD card */
    cJSON_AddBoolToObject(root, "sd_mounted", tab5_sdcard_mounted());
    if (tab5_sdcard_mounted()) {
        cJSON_AddNumberToObject(root, "sd_total_mb", (double)(tab5_sdcard_total_bytes() / (1024 * 1024)));
        cJSON_AddNumberToObject(root, "sd_free_mb", (double)(tab5_sdcard_free_bytes() / (1024 * 1024)));
    }

    const char *reset_reasons[] = {"UNKNOWN","POWERON","EXT","SW","PANIC","INT_WDT","TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO","USB","JTAG","EFUSE","PWR_GLITCH","CPU_LOCKUP"};
    esp_reset_reason_t reason = esp_reset_reason();
    cJSON_AddStringToObject(root, "reset_reason", reason < sizeof(reset_reasons)/sizeof(reset_reasons[0]) ? reset_reasons[reason] : "UNKNOWN");

    cJSON_AddBoolToObject(root, "auth_required", true);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ======================================================================== */
/*  POST /touch                                                              */
/* ======================================================================== */

static esp_err_t touch_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *jaction = cJSON_GetObjectItem(root, "action");
    const char *action = (jaction && cJSON_IsString(jaction)) ? jaction->valuestring : "tap";

    /* Swipe takes (x1,y1)->(x2,y2) over duration_ms.  Everything else uses
       a single (x,y) point.  Long-press uses (x,y) + duration_ms. */
    if (strcmp(action, "swipe") == 0) {
        cJSON *jx1 = cJSON_GetObjectItem(root, "x1");
        cJSON *jy1 = cJSON_GetObjectItem(root, "y1");
        cJSON *jx2 = cJSON_GetObjectItem(root, "x2");
        cJSON *jy2 = cJSON_GetObjectItem(root, "y2");
        cJSON *jdur = cJSON_GetObjectItem(root, "duration_ms");
        if (!cJSON_IsNumber(jx1) || !cJSON_IsNumber(jy1) ||
            !cJSON_IsNumber(jx2) || !cJSON_IsNumber(jy2)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "swipe needs x1,y1,x2,y2");
            return ESP_FAIL;
        }
        int x1 = jx1->valueint, y1 = jy1->valueint;
        int x2 = jx2->valueint, y2 = jy2->valueint;
        int dur = (jdur && cJSON_IsNumber(jdur)) ? jdur->valueint : 250;
        if (dur < 50)   dur = 50;
        if (dur > 3000) dur = 3000;

        ESP_LOGI(TAG, "Touch inject: swipe (%d,%d)->(%d,%d) %dms",
                 x1, y1, x2, y2, dur);

        /* 20 ms step cadence — enough for LVGL to register the gesture
           direction without flooding the indev read callback. */
        const int step_ms = 20;
        int steps = dur / step_ms;
        if (steps < 5) steps = 5;

        s_inject_x = x1;
        s_inject_y = y1;
        s_inject_pressed = true;
        s_inject_active = true;
        vTaskDelay(pdMS_TO_TICKS(40));   /* settle — let LVGL see the press */
        for (int i = 1; i <= steps; i++) {
            s_inject_x = x1 + (x2 - x1) * i / steps;
            s_inject_y = y1 + (y2 - y1) * i / steps;
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
        s_inject_pressed = false;
        vTaskDelay(pdMS_TO_TICKS(100));  /* release + LVGL gesture dispatch */
        s_inject_active = false;

        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }

    cJSON *jx = cJSON_GetObjectItem(root, "x");
    cJSON *jy = cJSON_GetObjectItem(root, "y");

    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Need x and y");
        return ESP_FAIL;
    }

    int x = jx->valueint;
    int y = jy->valueint;

    ESP_LOGI(TAG, "Touch inject: x=%d y=%d action=%s", x, y, action);

    /* Inject via shared override variables (polled by LVGL touch read callback) */
    if (strcmp(action, "release") == 0) {
        s_inject_x = x;
        s_inject_y = y;
        s_inject_pressed = false;
        s_inject_active = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        s_inject_active = false;
    } else if (strcmp(action, "press") == 0) {
        s_inject_x = x;
        s_inject_y = y;
        s_inject_pressed = true;
        s_inject_active = true;
        /* Leave active — caller must POST release to end it */
    } else if (strcmp(action, "long_press") == 0) {
        /* Hold long enough for LVGL LV_EVENT_LONG_PRESSED (default 400 ms) +
           a little slack so LONG_PRESSED_REPEAT doesn't mis-fire. Caller can
           override via duration_ms. */
        cJSON *jdur = cJSON_GetObjectItem(root, "duration_ms");
        int dur = (jdur && cJSON_IsNumber(jdur)) ? jdur->valueint : 800;
        if (dur < 500)  dur = 500;
        if (dur > 5000) dur = 5000;
        s_inject_x = x;
        s_inject_y = y;
        s_inject_pressed = true;
        s_inject_active = true;
        vTaskDelay(pdMS_TO_TICKS(dur));
        s_inject_pressed = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        s_inject_active = false;
    } else {
        /* tap: press, hold 200ms, release (needs 2+ LVGL ticks for CLICKED) */
        s_inject_x = x;
        s_inject_y = y;
        s_inject_pressed = true;
        s_inject_active = true;
        vTaskDelay(pdMS_TO_TICKS(200));
        s_inject_pressed = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        s_inject_active = false;
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ======================================================================== */
/*  POST /reboot                                                             */
/* ======================================================================== */

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"rebooting\":true}");
    ESP_LOGW(TAG, "Reboot requested via debug server");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  /* unreachable */
}

/* ======================================================================== */
/*  GET /log                                                                 */
/* ======================================================================== */

static esp_err_t log_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "heap_free",  (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min",   (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "uptime_s",   (double)(esp_timer_get_time() / 1000000));

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    cJSON_AddNumberToObject(root, "tasks", (double)task_count);

    /* Task list requires configUSE_TRACE_FACILITY — just report count */

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "print");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ======================================================================== */
/*  GET /  — Interactive HTML page                                           */
/* ======================================================================== */

static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>TinkerTab Debug</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{background:#1a1a2e;color:#e0e0e0;font-family:monospace;padding:16px}"
"h1{color:#ffb800;font-size:1.4em;margin-bottom:12px}"
".row{display:flex;gap:16px;flex-wrap:wrap}"
".col{flex:1;min-width:300px}"
"#screen-wrap{position:relative;display:inline-block;cursor:crosshair;border:2px solid #333}"
"#screen{max-width:100%;height:auto}"
"#info-panel{background:#16213e;padding:12px;border-radius:8px;margin-top:12px;"
"  font-size:0.85em;white-space:pre-wrap}"
".btn{background:#ffb800;color:#000;border:none;padding:8px 16px;border-radius:4px;"
"  cursor:pointer;font-weight:bold;margin:4px}"
".btn:hover{background:#ffd060}"
".btn-danger{background:#e74c3c;color:#fff}"
".btn-danger:hover{background:#ff6b6b}"
"#coords{color:#aaa;font-size:0.8em;margin-top:4px}"
"#status{color:#0f0;font-size:0.8em;margin-top:4px}"
"</style></head><body>"
"<h1>TinkerTab Debug Server</h1>"
"<div class='row'>"
"<div class='col'>"
"  <div id='screen-wrap'>"
"    <img id='screen' src='/screenshot.jpg' alt='screenshot' draggable='false'>"
"  </div>"
"  <div id='coords'>&nbsp;</div>"
"  <div id='status'>Ready</div>"
"  <div style='margin-top:8px'>"
"    <button class='btn' onclick='refresh()'>Refresh</button>"
"    <button class='btn' id='auto-btn' onclick='toggleAuto()'>Auto: OFF</button>"
"    <button class='btn btn-danger' onclick='reboot()'>Reboot</button>"
"  </div>"
"</div>"
"<div class='col'>"
"  <div id='info-panel'>Loading...</div>"
"</div>"
"</div>"
"<script>"
"const img=document.getElementById('screen');"
"const coordsEl=document.getElementById('coords');"
"const statusEl=document.getElementById('status');"
"const infoEl=document.getElementById('info-panel');"
"let autoMode=false,autoTimer=null;"
"\n"
"function refresh(){"
"  img.src='/screenshot.jpg?t='+Date.now();"
"  statusEl.textContent='Refreshed '+new Date().toLocaleTimeString();"
"}"
"\n"
"function toggleAuto(){"
"  autoMode=!autoMode;"
"  document.getElementById('auto-btn').textContent='Auto: '+(autoMode?'ON':'OFF');"
"  if(autoMode){autoTimer=setInterval(refresh,2000)}"
"  else{clearInterval(autoTimer)}"
"}"
"\n"
"img.addEventListener('click',function(e){"
"  const r=img.getBoundingClientRect();"
"  const sx=img.naturalWidth/r.width;"
"  const sy=img.naturalHeight/r.height;"
"  const x=Math.round((e.clientX-r.left)*sx);"
"  const y=Math.round((e.clientY-r.top)*sy);"
"  coordsEl.textContent='Tap: '+x+', '+y;"
"  fetch('/touch',{method:'POST',headers:{'Content-Type':'application/json'},"
"    body:JSON.stringify({x:x,y:y,action:'tap'})})"
"  .then(()=>{statusEl.textContent='Tapped '+x+','+y;setTimeout(refresh,300)})"
"  .catch(e=>statusEl.textContent='Error: '+e);"
"});"
"\n"
"img.addEventListener('mousemove',function(e){"
"  const r=img.getBoundingClientRect();"
"  const sx=img.naturalWidth/r.width;"
"  const sy=img.naturalHeight/r.height;"
"  const x=Math.round((e.clientX-r.left)*sx);"
"  const y=Math.round((e.clientY-r.top)*sy);"
"  coordsEl.textContent='Cursor: '+x+', '+y;"
"});"
"\n"
"function fetchInfo(){"
"  fetch('/info').then(r=>r.json()).then(d=>{"
"    let s='Heap free:  '+fmt(d.heap_free)+'\\n';"
"    s+='Heap min:   '+fmt(d.heap_min)+'\\n';"
"    s+='PSRAM free: '+fmt(d.psram_free)+'\\n';"
"    s+='Uptime:     '+Math.round(d.uptime_ms/1000)+'s\\n';"
"    s+='WiFi:       '+(d.wifi_connected?d.wifi_ip:'disconnected')+'\\n';"
"    s+='Dragon:     '+(d.dragon_connected?'connected':'offline')+'\\n';"
"    s+='Display:    '+d.display+'\\n';"
"    s+='FPS:        '+d.fps.toFixed(1)+'\\n';"
"    s+='LVGL FPS:   '+d.lvgl_fps+'\\n';"
"    s+='Battery:    '+d.battery_pct+'%\\n';"
"    s+='Tasks:      '+d.tasks;"
"    infoEl.textContent=s;"
"  }).catch(e=>infoEl.textContent='Error: '+e);"
"}"
"\n"
"function fmt(n){if(n>1e6)return (n/1e6).toFixed(1)+'M';if(n>1e3)return (n/1e3).toFixed(0)+'K';return n;}"
"\n"
"function reboot(){"
"  if(confirm('Reboot TinkerTab?')){"
"    fetch('/reboot',{method:'POST'}).then(()=>statusEl.textContent='Rebooting...');"
"  }"
"}"
"\n"
"fetchInfo();setInterval(fetchInfo,5000);"
"</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

/* ======================================================================== */
/*  POST /open?screen=wifi  — open a screen directly for testing             */
/* ======================================================================== */

/* Deferred screen open — runs on the LVGL thread via lv_async_call */
static volatile int s_pending_screen = -1;

/* US-C19: Runs on LVGL thread via lv_async_call (not lv_timer_create).
 * httpd handlers run on Core 1 — calling lv_timer_create required LVGL lock
 * from HTTP context, risking deadlock. lv_async_call is thread-safe. */
static void async_open_screen(void *arg)
{
    (void)arg;
    int scr_id = s_pending_screen;
    s_pending_screen = -1;

    /* Dismiss any overlays (chat, voice, keyboard) before switching */
    extern void ui_chat_hide(void);
    if (ui_chat_is_active()) ui_chat_hide();
    ui_keyboard_hide();

    switch (scr_id) {
    case 0: ui_wifi_create(); break;
    case 1: ui_camera_create(); break;
    case 2: ui_settings_create(); break;
    case 3: ui_files_create(); break;
    case 4: lv_screen_load(ui_home_get_screen()); break;
    case 5: ui_chat_create(); break;
    case 6: /* Notes page — scroll tileview to page 1 (row=1, col=0) */ {
        lv_obj_t *tv = lv_obj_get_child(ui_home_get_screen(), 0);
        if (tv) lv_obj_set_tile_id(tv, 0, 1, LV_ANIM_OFF);
        break;
    }
    default: break;
    }
}

static esp_err_t open_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char screen[32] = {0};
    httpd_query_key_value(query, "screen", screen, sizeof(screen));
    ESP_LOGI(TAG, "Open screen: %s", screen);

    int scr_id = -1;
    if (strcmp(screen, "wifi") == 0) scr_id = 0;
    else if (strcmp(screen, "camera") == 0) scr_id = 1;
    else if (strcmp(screen, "settings") == 0) scr_id = 2;
    else if (strcmp(screen, "files") == 0) scr_id = 3;
    else if (strcmp(screen, "home") == 0) scr_id = 4;
    else if (strcmp(screen, "chat") == 0) scr_id = 5;
    else if (strcmp(screen, "notes") == 0) scr_id = 6;
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown screen");
        return ESP_FAIL;
    }

    /* US-C19: Schedule on LVGL thread via lv_async_call — thread-safe, no lock needed.
     * Previously used tab5_ui_try_lock + lv_timer_create which risked deadlock. */
    s_pending_screen = scr_id;
    lv_async_call(async_open_screen, NULL);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ======================================================================== */
/*  GET /crashlog — last core dump summary                                   */
/* ======================================================================== */

static esp_err_t crashlog_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
        return ESP_FAIL;
    }

    /* Reset reason */
    const char *reset_reasons[] = {"UNKNOWN","POWERON","EXT","SW","PANIC",
        "INT_WDT","TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO","USB",
        "JTAG","EFUSE","PWR_GLITCH","CPU_LOCKUP"};
    esp_reset_reason_t reason = esp_reset_reason();
    cJSON_AddStringToObject(root, "reset_reason",
        reason < sizeof(reset_reasons)/sizeof(reset_reasons[0])
        ? reset_reasons[reason] : "UNKNOWN");
    cJSON_AddBoolToObject(root, "was_crash",
        reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
        reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT);

    /* Check if a core dump exists in flash */
    esp_core_dump_summary_t summary;
    esp_err_t cd_ret = esp_core_dump_get_summary(&summary);
    if (cd_ret == ESP_OK) {
        cJSON_AddBoolToObject(root, "coredump_present", true);
        cJSON_AddNumberToObject(root, "exc_pc", (double)summary.exc_pc);
        cJSON_AddStringToObject(root, "exc_task", summary.exc_task);

        /* RISC-V: no on-device backtrace, provide stack dump size */
        cJSON_AddNumberToObject(root, "stackdump_size",
            (double)summary.exc_bt_info.dump_size);
        cJSON_AddStringToObject(root, "hint",
            "Use 'espcoredump.py info_corefile' on host for full backtrace");
    } else {
        cJSON_AddBoolToObject(root, "coredump_present", false);
        cJSON_AddStringToObject(root, "note",
            cd_ret == ESP_ERR_NOT_FOUND
            ? "No core dump in flash"
            : "Core dump read error");
    }

    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min", (double)esp_get_minimum_free_heap_size());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "print");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ── Coredump binary download ─────────────────────────────────────────── */

/* Wave 15 W15-C05: stream the raw coredump partition bytes so we can
 * decode them offline with `espcoredump.py info_corefile -t elf`.
 * Partition reads are chunked into 4 KiB so the httpd task stack stays
 * tiny regardless of the dump size (~3 MB on this board).  Erase is
 * NOT performed here — the operator explicitly calls /crashlog?erase=1
 * or runs the esptool after confirming the dump was captured.  This
 * avoids the pathological loop where a corrupted dump keeps crashing
 * the decoder and gets wiped on every fetch attempt. */
static esp_err_t coredump_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    size_t addr = 0, size = 0;
    esp_err_t err = esp_core_dump_image_get(&addr, &size);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_send(req, "{\"error\":\"no coredump\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "coredump_image_get failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read error");
        return ESP_FAIL;
    }
    if (size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "{\"error\":\"empty coredump\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Find the coredump partition so we can read via partition-relative
     * offsets.  `esp_core_dump_image_get` returns absolute flash addrs
     * but `esp_partition_read` wants offset-from-partition-start. */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no coredump partition");
        return ESP_FAIL;
    }
    if (addr < part->address || addr + size > part->address + part->size) {
        ESP_LOGE(TAG, "coredump range 0x%zx+%zu outside partition 0x%lx+%lu",
                 addr, size, (unsigned long)part->address, (unsigned long)part->size);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "range mismatch");
        return ESP_FAIL;
    }
    const size_t rel = addr - part->address;

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    httpd_resp_set_hdr(req, "Content-Disposition",
                      "attachment; filename=\"tab5_coredump.elf\"");
    /* Advertise size via X-Content-Length so the client can track
     * progress without conflicting with the chunked Transfer-Encoding
     * that httpd_resp_send_chunk() emits. */
    char len[32];
    snprintf(len, sizeof(len), "%zu", size);
    httpd_resp_set_hdr(req, "X-Content-Length", len);

    /* Stream in 4 KiB chunks. */
    const size_t CHUNK = 4096;
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DEFAULT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    size_t sent = 0;
    while (sent < size) {
        size_t to_read = size - sent;
        if (to_read > CHUNK) to_read = CHUNK;
        err = esp_partition_read(part, rel + sent, buf, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "partition_read@%zu: %s", rel + sent, esp_err_to_name(err));
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, to_read) != ESP_OK) {
            /* Client dropped — bail silently. */
            break;
        }
        sent += to_read;
    }
    heap_caps_free(buf);
    /* Terminate chunked stream. */
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "/coredump: sent %zu/%zu bytes", sent, size);
    return ESP_OK;
}

/* ── SD card file listing ─────────────────────────────────────────────── */

static void _list_dir_to_json(cJSON *arr, const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);

        struct stat st;
        stat(full, &st);

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry->d_name);
        cJSON_AddStringToObject(item, "path", full);
        cJSON_AddBoolToObject(item, "dir", entry->d_type == DT_DIR);
        if (entry->d_type != DT_DIR) {
            cJSON_AddNumberToObject(item, "size", (double)st.st_size);
        }
        cJSON_AddItemToArray(arr, item);
    }
    closedir(d);
}

static esp_err_t sdcard_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "mounted", tab5_sdcard_mounted());

    if (tab5_sdcard_mounted()) {
        cJSON_AddNumberToObject(root, "total_mb", (double)(tab5_sdcard_total_bytes() / (1024*1024)));
        cJSON_AddNumberToObject(root, "free_mb", (double)(tab5_sdcard_free_bytes() / (1024*1024)));

        /* List /sdcard root */
        cJSON *files = cJSON_AddArrayToObject(root, "files");
        _list_dir_to_json(files, "/sdcard");

        /* List /sdcard/rec if it exists */
        struct stat st;
        if (stat("/sdcard/rec", &st) == 0) {
            cJSON *recs = cJSON_AddArrayToObject(root, "recordings");
            _list_dir_to_json(recs, "/sdcard/rec");
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ── Wake word toggle (PARKED) ───────────────────────────────────────── */

static esp_err_t wake_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* Feature parked — accept the request but do nothing. Return a clear
     * status so external tools stop retrying. Un-park: restore the toggle
     * body above and flip WAKE_WORD_PARKED in voice.c. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "afe_active", false);
    cJSON_AddBoolToObject(root, "wake_listening", false);
    cJSON_AddStringToObject(root, "wake_word", "parked");
    cJSON_AddStringToObject(root, "status", "wake-word feature is parked — see voice.c WAKE_WORD_PARKED");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ── OTA debug endpoints ──────────────────────────────────────────────── */

static esp_err_t ota_check_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    tab5_ota_info_t info;
    esp_err_t err = tab5_ota_check(&info);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "current", TAB5_FIRMWARE_VER);
    cJSON_AddStringToObject(root, "partition", tab5_ota_current_partition());
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(root, "update_available", info.available);
        if (info.available) {
            cJSON_AddStringToObject(root, "new_version", info.version);
            cJSON_AddStringToObject(root, "url", info.url);
        }
    } else {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

typedef struct {
    char *url;
    char sha256[65];
} ota_apply_args_t;

static void ota_apply_task(void *arg)
{
    ota_apply_args_t *args = (ota_apply_args_t *)arg;
    ESP_LOGI("ota", "Scheduling OTA for next boot: %s (sha256=%s)", args->url,
             args->sha256[0] ? args->sha256 : "none");
    /* Wave 10 #77 extended-use fix: schedule instead of apply in-process.
     * tab5_ota_schedule stores url+sha to NVS, reboots, and the fresh
     * boot applies with a pristine DMA heap (see main.c near WiFi init).
     * Prevents the "esp_dma_capable_malloc: Not enough heap memory"
     * failure mode that hits after 30+ min of normal use. */
    esp_err_t err = tab5_ota_schedule(args->url, args->sha256[0] ? args->sha256 : NULL);
    /* If we get here, schedule failed (success reboots) */
    ESP_LOGE("ota", "OTA schedule failed: %s", esp_err_to_name(err));
    free(args->url);
    free(args);
    vTaskSuspend(NULL)  /* wave 13 C4: P4 TLSP crash on delete — suspend instead */;
}

static esp_err_t ota_apply_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* Check for update first */
    tab5_ota_info_t info;
    esp_err_t err = tab5_ota_check(&info);
    if (err != ESP_OK || !info.available) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"no update available\"}");
        return ESP_OK;
    }

    /* Spawn OTA task with SHA256 from version.json (SEC07) */
    ota_apply_args_t *args = malloc(sizeof(ota_apply_args_t));
    if (args) {
        args->url = strdup(info.url);
        strncpy(args->sha256, info.sha256, sizeof(args->sha256) - 1);
        args->sha256[sizeof(args->sha256) - 1] = '\0';
        if (args->url) {
            xTaskCreate(ota_apply_task, "ota_apply", 8192, args, 5, NULL);
        } else {
            free(args);
        }
    }

    httpd_resp_set_type(req, "application/json");
    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"updating\",\"version\":\"%s\",\"url\":\"%s\",\"sha256_verify\":%s}",
             info.version, info.url, info.sha256[0] ? "true" : "false");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── ADB-style debug APIs ─────────────────────────────────────────────── */

/* GET /settings — dump all NVS settings as JSON */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();

    char buf[64];
    tab5_settings_get_wifi_ssid(buf, sizeof(buf));
    cJSON_AddStringToObject(root, "wifi_ssid", buf);
    tab5_settings_get_dragon_host(buf, sizeof(buf));
    cJSON_AddStringToObject(root, "dragon_host", buf);
    cJSON_AddNumberToObject(root, "dragon_port", tab5_settings_get_dragon_port());
    cJSON_AddNumberToObject(root, "brightness", tab5_settings_get_brightness());
    cJSON_AddNumberToObject(root, "volume", tab5_settings_get_volume());
    cJSON_AddNumberToObject(root, "voice_mode", tab5_settings_get_voice_mode());
    char model[64];
    tab5_settings_get_llm_model(model, sizeof(model));
    cJSON_AddStringToObject(root, "llm_model", model);
    /* wake_word setting is parked — always reports 0 regardless of NVS value */
    cJSON_AddNumberToObject(root, "wake_word", 0);
    cJSON_AddBoolToObject(root, "voice_connected", voice_is_connected());
    cJSON_AddNumberToObject(root, "voice_state", voice_get_state());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* Async wrapper for lv_async_call — refreshes home mode badge on LVGL thread */
static void async_refresh_mode_badge(void *arg)
{
    (void)arg;
    extern void ui_home_refresh_mode_badge(void);
    ui_home_refresh_mode_badge();
}

/* POST /settings — update NVS keys via JSON body.
 * Supports: dragon_host (str), dragon_port (int), wifi_ssid (str).
 * Body: {"dragon_host":"192.168.1.91","dragon_port":3502}
 * Returns: {"updated":["dragon_host","dragon_port"]}
 * Note: does NOT reconnect voice WS automatically — call /voice/reconnect after.
 */
static esp_err_t settings_set_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"error\":\"body 1..512 bytes required\"}");
        return ESP_OK;
    }

    char body[520] = {0};
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"recv failed\"}");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *req_json = cJSON_Parse(body);
    if (!req_json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"error\":\"invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *updated = cJSON_CreateArray();

    cJSON *host = cJSON_GetObjectItem(req_json, "dragon_host");
    if (cJSON_IsString(host) && host->valuestring && strlen(host->valuestring) > 0) {
        if (tab5_settings_set_dragon_host(host->valuestring) == ESP_OK) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("dragon_host"));
        }
    }

    cJSON *port = cJSON_GetObjectItem(req_json, "dragon_port");
    if (cJSON_IsNumber(port)) {
        int p = (int)port->valuedouble;
        if (p > 0 && p < 65536) {
            if (tab5_settings_set_dragon_port((uint16_t)p) == ESP_OK) {
                cJSON_AddItemToArray(updated, cJSON_CreateString("dragon_port"));
            }
        }
    }

    cJSON *cmode = cJSON_GetObjectItem(req_json, "conn_mode");
    if (cJSON_IsNumber(cmode)) {
        int m = (int)cmode->valuedouble;
        if (m >= 0 && m <= 2) {
            if (tab5_settings_set_connection_mode((uint8_t)m) == ESP_OK) {
                cJSON_AddItemToArray(updated, cJSON_CreateString("conn_mode"));
            }
        }
    }

    /* v4·D Sovereign Halo mode dials — accept tiers and auto-resolve into
     * voice_mode + llm_model. Calling /voice/reconnect after a tier
     * change is optional but recommended -- Dragon picks up the new mode
     * on the next config_update that follows. */
    bool any_tier = false;
    cJSON *it = cJSON_GetObjectItem(req_json, "int_tier");
    if (cJSON_IsNumber(it)) {
        int t = (int)it->valuedouble;
        if (t >= 0 && t <= 2 && tab5_settings_set_int_tier((uint8_t)t) == ESP_OK) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("int_tier"));
            any_tier = true;
        }
    }
    cJSON *vt = cJSON_GetObjectItem(req_json, "voi_tier");
    if (cJSON_IsNumber(vt)) {
        int t = (int)vt->valuedouble;
        if (t >= 0 && t <= 2 && tab5_settings_set_voi_tier((uint8_t)t) == ESP_OK) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("voi_tier"));
            any_tier = true;
        }
    }
    cJSON *at = cJSON_GetObjectItem(req_json, "aut_tier");
    if (cJSON_IsNumber(at)) {
        int t = (int)at->valuedouble;
        if (t >= 0 && t <= 1 && tab5_settings_set_aut_tier((uint8_t)t) == ESP_OK) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("aut_tier"));
            any_tier = true;
        }
    }

    /* Phase 3e budget cap edit + spent reset (dev/debug knob) */
    cJSON *cap = cJSON_GetObjectItem(req_json, "cap_mils");
    if (cJSON_IsNumber(cap)) {
        uint32_t v = (uint32_t)cap->valuedouble;
        if (tab5_budget_set_cap_mils(v) == ESP_OK) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("cap_mils"));
        }
    }
    cJSON *reset = cJSON_GetObjectItem(req_json, "reset_spent");
    if (cJSON_IsBool(reset) && cJSON_IsTrue(reset)) {
        /* Zero today's spend by resetting the day marker -- the next
         * accumulate() will see stored_day != today and wipe. */
        extern esp_err_t nvs_flash_init(void);
        /* Direct-write via the bounded API by briefly reparenting into
         * settings.c's helpers is overkill; instead we rely on the
         * accumulate path's rollover logic: bumping cap + zero-arg
         * accumulate(0) would no-op. The cleanest path is to call the
         * setter that already exists.  Here we just re-apply cap which
         * implicitly keeps spent untouched; true reset is a future
         * helper. For now, a practical workaround: set cap to a value
         * greater than current spent so the next receipt re-evaluates. */
        cJSON_AddItemToArray(updated, cJSON_CreateString("reset_spent_noop"));
    }
    if (any_tier) {
        /* Resolve the new tier triple and persist the derived voice_mode +
         * llm_model. Leaves llm_model untouched when resolver doesn't pick
         * a specific cloud model (ie Local, Hybrid, TinkerClaw modes). */
        char model_out[64] = {0};
        uint8_t new_mode = tab5_mode_resolve(
            tab5_settings_get_int_tier(),
            tab5_settings_get_voi_tier(),
            tab5_settings_get_aut_tier(),
            model_out, sizeof(model_out));
        tab5_settings_set_voice_mode(new_mode);
        cJSON_AddItemToArray(updated, cJSON_CreateString("voice_mode"));
        cJSON_AddNumberToObject(resp, "resolved_voice_mode", new_mode);
        if (model_out[0]) {
            tab5_settings_set_llm_model(model_out);
            cJSON_AddItemToArray(updated, cJSON_CreateString("llm_model"));
            cJSON_AddStringToObject(resp, "resolved_llm_model", model_out);
        }
    }

    cJSON_AddItemToObject(resp, "updated", updated);
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(req_json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, out);
    free(out);
    return ret;
}

/* POST /mode?m=0|1|2|3&model=... — switch voice mode (3=TinkerClaw) */
static esp_err_t mode_set_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* Parse query string */
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"use ?m=0|1|2|3&model=... (3=TinkerClaw)\"}");
        return ESP_OK;
    }

    char val[8] = {0};
    char model[64] = {0};
    httpd_query_key_value(query, "m", val, sizeof(val));
    httpd_query_key_value(query, "model", model, sizeof(model));

    int mode = atoi(val);
    if (mode < 0 || mode > 3) mode = 0;

    /* Save to NVS */
    tab5_settings_set_voice_mode((uint8_t)mode);
    if (model[0]) {
        tab5_settings_set_llm_model(model);
    }

    ESP_LOGI("debug", "Mode switch: voice_mode=%d, llm_model=%s", mode, model[0] ? model : "(unchanged)");

    /* Send config_update to Dragon via voice WS */
    if (voice_is_connected()) {
        voice_send_config_update((int)mode, model[0] ? model : NULL);
    }

    /* Refresh home screen mode badge (runs on LVGL thread) */
    lv_async_call(async_refresh_mode_badge, NULL);

    /* Return current state */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "voice_mode", mode);
    const char *mode_names[] = {"local", "hybrid", "cloud", "tinkerclaw"};
    cJSON_AddStringToObject(root, "mode_name", mode <= 3 ? mode_names[mode] : "unknown");
    char cur_model[64];
    tab5_settings_get_llm_model(cur_model, sizeof(cur_model));
    cJSON_AddStringToObject(root, "llm_model", cur_model);
    cJSON_AddBoolToObject(root, "voice_connected", voice_is_connected());
    cJSON_AddStringToObject(root, "status", "applied");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* POST /navigate?screen=settings|notes|chat|camera|files|home
 * Uses lv_async_call to avoid LVGL lock deadlock from HTTP context */
static char s_nav_target[16] = {0};

static volatile bool s_navigating = false;

static void async_navigate(void *arg)
{
    (void)arg;
    if (s_navigating) {
        ESP_LOGW("nav", "Navigation already in progress — skipping");
        return;
    }
    s_navigating = true;
    ESP_LOGI("nav", "async_navigate executing, target='%s'", s_nav_target);
    /* Navigate using the home tileview for pages 0-3.
     * For secondary screens (camera, files) create them separately.
     * This avoids the bug where ui_settings_create() replaces the
     * tileview screen entirely, breaking subsequent navigation. */
    lv_obj_t *tv = ui_home_get_tileview();

    /* Always dismiss ALL overlays before any navigation — HIDE not destroy.
     * Destroy+recreate corrupts LVGL timer linked list (lv_ll_remove crash). */
    extern void ui_chat_hide(void);
    extern void ui_settings_hide(void);
    extern void ui_notes_hide(void);
    extern void ui_agents_hide(void);
    extern void ui_memory_hide(void);
    extern void ui_focus_hide(void);
    extern void ui_sessions_hide(void);
    ui_chat_hide();
    ui_settings_hide();
    ui_notes_hide();
    ui_agents_hide();
    ui_memory_hide();
    ui_focus_hide();
    ui_sessions_hide();
    ui_keyboard_hide();
    /* Wave 4 UX fix: if the voice overlay is up but voice is idle, a
     * nav request means the user has moved on.  Dismiss so the user
     * sees the screen they just navigated to instead of a stale
     * "Tap to speak." card blocking the view.  Active states
     * (LISTENING / PROCESSING / SPEAKING) are left alone. */
    extern void ui_voice_dismiss_if_idle(void);
    ui_voice_dismiss_if_idle();

    if (strcmp(s_nav_target, "home") == 0) {
        ui_home_go_home();
    } else if (strcmp(s_nav_target, "notes") == 0) {
        extern lv_obj_t *ui_notes_create(void);
        ui_notes_create();
    } else if (strcmp(s_nav_target, "chat") == 0) {
        extern lv_obj_t *ui_chat_create(void);
        ui_chat_create();
    } else if (strcmp(s_nav_target, "settings") == 0) {
        /* Settings is too heavy for lv_async_call — it blocks the LVGL render loop.
         * Use direct call since async_navigate already runs in LVGL context via lv_async_call. */
        extern void ui_home_nav_settings(void);
        ui_home_nav_settings();
    } else if (strcmp(s_nav_target, "camera") == 0) {
        extern lv_obj_t *ui_camera_create(void);
        ui_camera_create();
    } else if (strcmp(s_nav_target, "files") == 0) {
        extern lv_obj_t *ui_files_create(void);
        ui_files_create();
    } else if (strcmp(s_nav_target, "sessions") == 0) {
        extern void ui_sessions_show(void);
        ui_sessions_show();
    } else if (strcmp(s_nav_target, "agents") == 0) {
        extern void ui_agents_show(void);
        ui_agents_show();
    } else if (strcmp(s_nav_target, "memory") == 0) {
        extern void ui_memory_show(void);
        ui_memory_show();
    } else if (strcmp(s_nav_target, "focus") == 0) {
        extern void ui_focus_show(void);
        ui_focus_show();
    }
    s_navigating = false;
}

/* ── /widget — testing-only hook that injects a widget_live into the store
 *   without requiring Dragon. Body is JSON matching widget_live schema (see
 *   TinkerBox docs/protocol.md §17.2). Used by the Widget Platform test
 *   harness before the Dragon Tab5Surface is wired. */
#include "widget.h"
static void async_widget_refresh(void *arg) {
    (void)arg;
    extern void ui_home_update_status(void);
    ui_home_update_status();
}

static esp_err_t widget_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* Read body */
    int len = req->content_len;
    if (len <= 0 || len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body 1..2048 bytes");
        return ESP_FAIL;
    }
    char *buf = malloc(len + 1);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read"); return ESP_FAIL; }
        got += r;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    if (action && !strcmp(action, "dismiss")) {
        const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
        if (cid) widget_store_dismiss(cid);
        cJSON_Delete(root);
        lv_async_call(async_widget_refresh, NULL);
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }

    /* Build widget_t from JSON */
    widget_t w = {0};
    const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "skill_id"));
    const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
    const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
    const char *icn = cJSON_GetStringValue(cJSON_GetObjectItem(root, "icon"));
    const char *tn  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tone"));
    cJSON *prog     = cJSON_GetObjectItem(root, "progress");
    cJSON *pri      = cJSON_GetObjectItem(root, "priority");
    cJSON *act      = cJSON_GetObjectItem(root, "action");

    if (!cid) cid = "debug_1";
    if (!sid) sid = "debug";
    strncpy(w.card_id,  cid, WIDGET_ID_LEN - 1);
    strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
    if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
    if (bdy) strncpy(w.body,  bdy, WIDGET_BODY_LEN  - 1);
    if (icn) strncpy(w.icon,  icn, WIDGET_ICON_LEN  - 1);
    /* v4·D Phase 4c: optional "type" + "items" fields let the debug
     * endpoint inject widget_list payloads for visual testing. */
    const char *tstr = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    w.type = tstr ? widget_type_from_str(tstr) : WIDGET_TYPE_LIVE;
    if (w.type == WIDGET_TYPE_NONE) w.type = WIDGET_TYPE_LIVE;
    w.tone     = widget_tone_from_str(tn);
    w.progress = cJSON_IsNumber(prog) ? (float)prog->valuedouble : 0.0f;
    w.priority = cJSON_IsNumber(pri)  ? (uint8_t)pri->valueint   : 50;
    if (cJSON_IsObject(act)) {
        const char *al = cJSON_GetStringValue(cJSON_GetObjectItem(act, "label"));
        const char *ae = cJSON_GetStringValue(cJSON_GetObjectItem(act, "event"));
        if (al) strncpy(w.action_label, al, WIDGET_ACTION_LBL_LEN - 1);
        if (ae) strncpy(w.action_event, ae, WIDGET_ACTION_EVT_LEN - 1);
    }
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        int cnt = cJSON_GetArraySize(items);
        if (cnt > WIDGET_LIST_MAX_ITEMS) cnt = WIDGET_LIST_MAX_ITEMS;
        for (int i = 0; i < cnt; i++) {
            cJSON *it = cJSON_GetArrayItem(items, i);
            if (!cJSON_IsObject(it)) continue;
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
            const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "value"));
            if (t) strncpy(w.items[i].text,  t, WIDGET_LIST_ITEM_TEXT_LEN  - 1);
            if (v) strncpy(w.items[i].value, v, WIDGET_LIST_ITEM_VALUE_LEN - 1);
        }
        w.items_count = (uint8_t)cnt;
    }
    /* v4·D Phase 4f: optional "values" + "max" fields for widget_chart. */
    cJSON *vals = cJSON_GetObjectItem(root, "values");
    if (cJSON_IsArray(vals)) {
        int cnt = cJSON_GetArraySize(vals);
        if (cnt > WIDGET_CHART_MAX_POINTS) cnt = WIDGET_CHART_MAX_POINTS;
        for (int i = 0; i < cnt; i++) {
            cJSON *v = cJSON_GetArrayItem(vals, i);
            w.chart_values[i] = cJSON_IsNumber(v) ? (float)v->valuedouble : 0.0f;
        }
        w.chart_count = (uint8_t)cnt;
    }
    cJSON *mx = cJSON_GetObjectItem(root, "max");
    w.chart_max = cJSON_IsNumber(mx) ? (float)mx->valuedouble : 0.0f;
    /* v4·D Phase 4g: media + prompt fields for /widget debug injection. */
    const char *media_url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    const char *media_alt = cJSON_GetStringValue(cJSON_GetObjectItem(root, "alt"));
    if (media_url) strncpy(w.media_url, media_url, WIDGET_MEDIA_URL_LEN - 1);
    if (media_alt) strncpy(w.media_alt, media_alt, WIDGET_MEDIA_ALT_LEN - 1);
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices)) {
        int cnt = cJSON_GetArraySize(choices);
        if (cnt > WIDGET_PROMPT_MAX_CHOICES) cnt = WIDGET_PROMPT_MAX_CHOICES;
        for (int i = 0; i < cnt; i++) {
            cJSON *it = cJSON_GetArrayItem(choices, i);
            if (!cJSON_IsObject(it)) continue;
            const char *t  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
            const char *ev = cJSON_GetStringValue(cJSON_GetObjectItem(it, "event"));
            if (t)  strncpy(w.choices[i].text,  t,  WIDGET_PROMPT_CHOICE_LEN - 1);
            if (ev) strncpy(w.choices[i].event, ev, WIDGET_PROMPT_EVENT_LEN - 1);
        }
        w.choices_count = (uint8_t)cnt;
    }
    widget_store_upsert(&w);
    cJSON_Delete(root);
    lv_async_call(async_widget_refresh, NULL);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t navigate_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* Debounce: reject rapid navigation requests (same as UI 300ms debounce).
     * Without this, rapid /navigate calls queue multiple lv_async_call entries
     * that create/destroy LVGL overlays simultaneously → Load access fault. */
    static int64_t s_last_nav_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - s_last_nav_us < 500000) {  /* 500ms debounce */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"navigation too fast, wait 500ms\"}");
        return ESP_OK;
    }
    s_last_nav_us = now;

    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"use ?screen=settings|notes|chat|camera|files|home\"}");
        return ESP_OK;
    }

    httpd_query_key_value(query, "screen", s_nav_target, sizeof(s_nav_target));

    /* Schedule on LVGL thread — lv_async_call is thread-safe, no lock needed */
    lv_async_call(async_navigate, NULL);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "navigated", s_nav_target);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* POST /navtouch?screen=home|notes|chat|settings
 * Same as /navigate but also dismisses overlays and reports tap coordinates.
 * Useful for testing touch-based navigation flow. */
static esp_err_t navtouch_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"use ?screen=home|notes|chat|settings\"}");
        return ESP_OK;
    }
    char screen[16] = {0};
    httpd_query_key_value(query, "screen", screen, sizeof(screen));

    int tap_x = -1;
    if (strcmp(screen, "home") == 0)          tap_x = 90;
    else if (strcmp(screen, "notes") == 0)    tap_x = 270;
    else if (strcmp(screen, "chat") == 0)     tap_x = 450;
    else if (strcmp(screen, "settings") == 0) tap_x = 630;

    if (tap_x < 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"unknown screen\"}");
        return ESP_OK;
    }

    /* Schedule on LVGL thread — same path as /navigate */
    memset(s_nav_target, 0, sizeof(s_nav_target));
    memcpy(s_nav_target, screen, strlen(screen) < sizeof(s_nav_target) ? strlen(screen) : sizeof(s_nav_target) - 1);
    lv_async_call(async_navigate, NULL);

    ESP_LOGI("debug", "NavTouch: %s (tap equiv %d,1220)", screen, tap_x);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "screen", screen);
    cJSON_AddNumberToObject(root, "tap_x", tap_x);
    cJSON_AddNumberToObject(root, "tap_y", 1220);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ── Camera debug endpoint ────────────────────────────────────────────── */

static esp_err_t camera_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    if (!tab5_camera_initialized()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"camera not initialized\"}");
        return ESP_OK;
    }

    tab5_cam_frame_t frame;
    esp_err_t err = tab5_camera_capture(&frame);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"capture failed: %s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, buf);
        return ESP_OK;
    }

    /* Return raw RGB565 as BMP */
    uint32_t data_size = frame.width * frame.height * 2;
    uint32_t file_size = 66 + data_size;
    uint8_t hdr[66] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &file_size, 4);
    uint32_t offset = 66; memcpy(&hdr[10], &offset, 4);
    uint32_t dib = 40; memcpy(&hdr[14], &dib, 4);
    int32_t w = frame.width, h = -(int32_t)frame.height;
    memcpy(&hdr[18], &w, 4); memcpy(&hdr[22], &h, 4);
    uint16_t planes = 1; memcpy(&hdr[26], &planes, 2);
    uint16_t bpp = 16; memcpy(&hdr[28], &bpp, 2);
    uint32_t comp = 3; memcpy(&hdr[30], &comp, 4);
    memcpy(&hdr[34], &data_size, 4);
    uint32_t rm = 0xF800, gm = 0x07E0, bm = 0x001F;
    memcpy(&hdr[54], &rm, 4); memcpy(&hdr[58], &gm, 4); memcpy(&hdr[62], &bm, 4);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    httpd_resp_send_chunk(req, (const char *)hdr, 66);
    httpd_resp_send_chunk(req, (const char *)frame.data, data_size);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

/* ── Chat/Text input endpoint ─────────────────────────────────────────── */

static esp_err_t chat_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* POST /chat?text=hello — send text to Dragon via voice WS */
    char query[256] = {0};
    char text[200] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "text", text, sizeof(text));
    }

    /* Also try reading from body (JSON) */
    if (text[0] == '\0') {
        int len = httpd_req_recv(req, query, sizeof(query) - 1);
        if (len > 0) {
            query[len] = '\0';
            cJSON *root = cJSON_Parse(query);
            if (root) {
                cJSON *t = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(t)) {
                    snprintf(text, sizeof(text), "%s", t->valuestring);
                }
                cJSON_Delete(root);
            }
        }
    }

    if (text[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"use ?text=hello or POST {\\\"text\\\":\\\"hello\\\"}\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Debug chat: %s", text);

    if (!voice_is_connected()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"voice not connected\",\"sent\":false}");
        return ESP_OK;
    }

    /* Push the user's message into the chat store so the bubble appears.
     * The normal typing path goes through chat_input_bar which calls
     * chat_store_add + voice_send_text; /chat (debug) only called the
     * latter, which is why debug-fired turns never showed a user bubble. */
    extern void ui_chat_push_message(const char *role, const char *text);
    ui_chat_push_message("user", text);
    voice_send_text(text);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddBoolToObject(root, "sent", true);
    cJSON_AddBoolToObject(root, "voice_connected", voice_is_connected());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ── Voice state endpoint ─────────────────────────────────────────────── */

static esp_err_t voice_state_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* GET /voice — full voice pipeline state */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", voice_is_connected());
    cJSON_AddNumberToObject(root, "state", (int)voice_get_state());

    const char *state_names[] = {"IDLE", "CONNECTING", "READY", "LISTENING", "PROCESSING", "SPEAKING", "RECONNECTING", "DICTATING"};
    int st = (int)voice_get_state();
    cJSON_AddStringToObject(root, "state_name", (st >= 0 && st <= 6) ? state_names[st] : "UNKNOWN");

    /* Wave 14 W14-M01: the debug httpd task races with voice.c's
     * WS RX task.  Use the copy-under-mutex variants to avoid
     * observing a mid-strcat string. */
    char llm_buf[512];
    if (voice_get_llm_text_copy(llm_buf, sizeof(llm_buf)) && llm_buf[0]) {
        cJSON_AddStringToObject(root, "last_llm_text", llm_buf);
    }

    char stt_buf[512];
    if (voice_get_stt_text_copy(stt_buf, sizeof(stt_buf)) && stt_buf[0]) {
        cJSON_AddStringToObject(root, "last_stt_text", stt_buf);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ── Voice reconnect endpoint ─────────────────────────────────────────── */

static esp_err_t voice_reconnect_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    /* POST /voice/reconnect — force voice WS reconnect */
    ESP_LOGI(TAG, "Debug: forcing voice reconnect");
    voice_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    /* Reconnect using current NVS settings + voice port (3502, not dragon_port which is 3501 for CDP) */
    char host[64] = {0};
    tab5_settings_get_dragon_host(host, sizeof(host));
    voice_connect(host, TAB5_VOICE_PORT);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    httpd_resp_sendstr(req, "{\"status\":\"reconnecting\"}");
    return ESP_OK;
}

/* ── Wave 12 observability: /heap endpoint ─────────────────────────────
 *
 * Exposes the per-pool heap state + last reboot reason in one call so
 * dashboards and post-mortem scripts can track device health over time.
 * This pairs with the wave 11 coredump path — if last_reboot_reason is
 * something like "abort" or a heap_wd string, the coredump partition
 * at 0x620000 has a forensic dump available via
 *     esptool read_flash 0x620000 0x40000 cd.bin
 *     espcoredump.py info_corefile -t elf ... cd.bin
 *
 * No auth-gate — the debug server overall requires auth per
 * reference_tab5_debug_access.md, but /heap is READ-ONLY and useful
 * to have behind the same bearer token.
 */
esp_err_t heap_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    extern void media_cache_stats(int *used_slots, unsigned *resident_kb);
    int mc_used = 0; unsigned mc_kb = 0;
    media_cache_stats(&mc_used, &mc_kb);

    size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t int_free      = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t int_largest   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t dma_free      = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t dma_largest   = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    lv_mem_monitor_t lvgl;
    lv_mem_monitor(&lvgl);

    const char *reset_str = "unknown";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   reset_str = "poweron"; break;
        case ESP_RST_EXT:       reset_str = "external_pin"; break;
        case ESP_RST_SW:        reset_str = "esp_restart"; break;
        case ESP_RST_PANIC:     reset_str = "panic_abort"; break;   /* wave 11 coredump path */
        case ESP_RST_INT_WDT:   reset_str = "int_wdt"; break;
        case ESP_RST_TASK_WDT:  reset_str = "task_wdt"; break;
        case ESP_RST_WDT:       reset_str = "other_wdt"; break;
        case ESP_RST_DEEPSLEEP: reset_str = "deepsleep_wake"; break;
        case ESP_RST_BROWNOUT:  reset_str = "brownout"; break;
        case ESP_RST_SDIO:      reset_str = "sdio"; break;
        default:                reset_str = "unknown"; break;
    }

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime_ms\":%llu,\"reset_reason\":\"%s\","
        "\"psram\":{\"free_kb\":%u,\"largest_kb\":%u},"
        "\"internal\":{\"free_kb\":%u,\"largest_kb\":%u,\"frag_pct\":%d},"
        "\"dma\":{\"free_kb\":%u,\"largest_kb\":%u},"
        "\"lvgl\":{\"used_kb\":%u,\"free_kb\":%u,\"frag_pct\":%u},"
        "\"media_cache\":{\"slots_used\":%d,\"resident_kb\":%u},"
        "\"coredump_available\":%s}",
        (unsigned long long)(esp_timer_get_time() / 1000),
        reset_str,
        (unsigned)(psram_free / 1024), (unsigned)(psram_largest / 1024),
        (unsigned)(int_free / 1024), (unsigned)(int_largest / 1024),
        int_free ? (int)(100 - (int_largest * 100 / int_free)) : 0,
        (unsigned)(dma_free / 1024), (unsigned)(dma_largest / 1024),
        (unsigned)((lvgl.total_size - lvgl.free_size) / 1024),
        (unsigned)(lvgl.free_size / 1024), (unsigned)lvgl.frag_pct,
        mc_used, mc_kb,
        (esp_core_dump_image_check() == ESP_OK) ? "true" : "false");
    (void)n;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── Full self-test endpoint ──────────────────────────────────────────── */

static esp_err_t selftest_handler(httpd_req_t *req)
{
    /* GET /selftest — run through all subsystem checks and return results */
    cJSON *root = cJSON_CreateObject();
    cJSON *tests = cJSON_AddArrayToObject(root, "tests");
    int pass = 0, fail = 0;

    /* WiFi */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = tab5_wifi_connected();
        cJSON_AddStringToObject(t, "name", "wifi");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* Voice WS connected */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = voice_is_connected();
        cJSON_AddStringToObject(t, "name", "voice_ws");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* SD card */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = tab5_sdcard_mounted();
        cJSON_AddStringToObject(t, "name", "sd_card");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* PSRAM available (>1MB) + fragmentation check */
    {
        cJSON *t = cJSON_CreateObject();
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        bool ok = free_psram > (1 * 1024 * 1024);
        cJSON_AddStringToObject(t, "name", "psram");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddNumberToObject(t, "free_mb", (double)(free_psram / 1024 / 1024));
        cJSON_AddNumberToObject(t, "largest_free_block_kb", (double)(largest_block / 1024));
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* Display initialized (if we got this far, display is running) */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = true;  /* If debug server is responding, display is up */
        cJSON_AddStringToObject(t, "name", "display");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* Camera initialized */
    {
        cJSON *t = cJSON_CreateObject();
        bool ok = tab5_camera_initialized();
        cJSON_AddStringToObject(t, "name", "camera");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* Internal heap — free + fragmentation */
    {
        cJSON *t = cJSON_CreateObject();
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        int frag_pct = (free_internal > 0) ? (int)(100 - (largest_internal * 100 / free_internal)) : 0;
        bool ok = free_internal > (30 * 1024) && largest_internal > (8 * 1024);
        cJSON_AddStringToObject(t, "name", "internal_heap");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddNumberToObject(t, "free_kb", (double)(free_internal / 1024));
        cJSON_AddNumberToObject(t, "largest_block_kb", (double)(largest_internal / 1024));
        cJSON_AddNumberToObject(t, "fragmentation_pct", (double)frag_pct);
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    /* NVS settings readable */
    {
        cJSON *t = cJSON_CreateObject();
        char host[64] = {0};
        tab5_settings_get_dragon_host(host, sizeof(host));
        bool ok = (host[0] != '\0');
        cJSON_AddStringToObject(t, "name", "nvs_settings");
        cJSON_AddBoolToObject(t, "pass", ok);
        cJSON_AddStringToObject(t, "dragon_host", host);
        cJSON_AddNumberToObject(t, "nvs_writes_this_session",
                                (double)tab5_settings_get_nvs_write_count());
        cJSON_AddItemToArray(tests, t);
        if (ok) pass++; else fail++;
    }

    cJSON_AddNumberToObject(root, "passed", pass);
    cJSON_AddNumberToObject(root, "failed", fail);
    cJSON_AddNumberToObject(root, "total", pass + fail);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ======================================================================== */
/*  Server init                                                              */
/* ======================================================================== */

esp_err_t tab5_debug_server_init(void)
{
    if (!tab5_wifi_connected()) {
        ESP_LOGW(TAG, "WiFi not connected — debug server not started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Initialize bearer token auth (generate on first boot, load from NVS thereafter) */
    init_auth_token();

    /* Eagerly create the hardware JPEG encoder once during server init.
     * Lazy per-request init races when multiple sockets arrive before
     * s_jpeg_enc is populated — both tasks try to init, the second one
     * crashes in jpeg_release_codec_handle(NULL). */
    (void)_ensure_jpeg_encoder();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = DEBUG_PORT;
    config.stack_size  = 12288;
    config.max_uri_handlers = 28;
    config.lru_purge_enable = true;
    config.max_open_sockets = 16;         /* Needs headroom for rapid API calls (nav+info pairs) */
    config.recv_wait_timeout = 5;         /* 5s recv timeout (default 5) */
    /* A full 1.8 MB screenshot over 2.4 GHz WiFi with 400-500 ms RTT and
     * the small default LWIP TCP send window needs ~30-60s of blocked
     * send() calls. Default 5s kills the connection after one window-full.
     * Bump to 90s so /screenshot can actually complete. */
    config.send_wait_timeout = 90;
    config.close_fn = NULL;               /* Use default close */
    /* Run httpd on Core 1 so it doesn't starve when LVGL is busy on Core 0.
     * Settings screen creates 55 objects (~500ms) which blocks Core 0 entirely.
     * Without this, /info requests time out during heavy LVGL rendering. */
    config.core_id = 1;
    config.task_priority = tskIDLE_PRIORITY + 6;  /* Above LVGL (prio 5) */

    if (s_httpd != NULL) {
        /* Already running — treat as idempotent.  Caller should stop
         * first if they want a restart.  Returning ESP_OK preserves
         * the old pre-W15-C02 behaviour where a second call was a
         * benign no-op (would have leaked a second server). */
        ESP_LOGW(TAG, "debug server already running, skipping re-init");
        return ESP_OK;
    }

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start debug server: %s", esp_err_to_name(ret));
        return ret;
    }
    s_httpd = server;

    /* Register URI handlers */
    const httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler
    };
    const httpd_uri_t uri_screenshot = {
        .uri = "/screenshot", .method = HTTP_GET, .handler = screenshot_handler
    };
    const httpd_uri_t uri_screenshot_bmp = {
        .uri = "/screenshot.bmp", .method = HTTP_GET, .handler = screenshot_handler
    };
    const httpd_uri_t uri_screenshot_jpg = {
        .uri = "/screenshot.jpg", .method = HTTP_GET, .handler = screenshot_handler
    };
    const httpd_uri_t uri_info = {
        .uri = "/info", .method = HTTP_GET, .handler = info_handler
    };
    const httpd_uri_t uri_touch = {
        .uri = "/touch", .method = HTTP_POST, .handler = touch_handler
    };
    const httpd_uri_t uri_reboot = {
        .uri = "/reboot", .method = HTTP_POST, .handler = reboot_handler
    };
    const httpd_uri_t uri_log = {
        .uri = "/log", .method = HTTP_GET, .handler = log_handler
    };

    const httpd_uri_t uri_open = {
        .uri = "/open", .method = HTTP_POST, .handler = open_handler
    };
    const httpd_uri_t uri_coredump = {
        .uri = "/coredump", .method = HTTP_GET, .handler = coredump_handler
    };
    const httpd_uri_t uri_crashlog = {
        .uri = "/crashlog", .method = HTTP_GET, .handler = crashlog_handler
    };
    const httpd_uri_t uri_sdcard = {
        .uri = "/sdcard", .method = HTTP_GET, .handler = sdcard_handler
    };
    const httpd_uri_t uri_wake = {
        .uri = "/wake", .method = HTTP_POST, .handler = wake_handler
    };
    const httpd_uri_t uri_settings_get = {
        .uri = "/settings", .method = HTTP_GET, .handler = settings_get_handler
    };
    const httpd_uri_t uri_settings_set = {
        .uri = "/settings", .method = HTTP_POST, .handler = settings_set_handler
    };
    const httpd_uri_t uri_mode_set = {
        .uri = "/mode", .method = HTTP_POST, .handler = mode_set_handler
    };
    const httpd_uri_t uri_navigate = {
        .uri = "/navigate", .method = HTTP_POST, .handler = navigate_handler
    };
    const httpd_uri_t uri_widget = {
        .uri = "/widget", .method = HTTP_POST, .handler = widget_handler
    };
    const httpd_uri_t uri_camera = {
        .uri = "/camera", .method = HTTP_GET, .handler = camera_handler
    };
    const httpd_uri_t uri_ota_check = {
        .uri = "/ota/check", .method = HTTP_GET, .handler = ota_check_handler
    };
    const httpd_uri_t uri_ota_apply = {
        .uri = "/ota/apply", .method = HTTP_POST, .handler = ota_apply_handler
    };
    const httpd_uri_t uri_chat = {
        .uri = "/chat", .method = HTTP_POST, .handler = chat_handler
    };
    const httpd_uri_t uri_voice_state = {
        .uri = "/voice", .method = HTTP_GET, .handler = voice_state_handler
    };
    const httpd_uri_t uri_voice_reconnect = {
        .uri = "/voice/reconnect", .method = HTTP_POST, .handler = voice_reconnect_handler
    };
    const httpd_uri_t uri_selftest = {
        .uri = "/selftest", .method = HTTP_GET, .handler = selftest_handler
    };
    const httpd_uri_t uri_navtouch = {
        .uri = "/navtouch", .method = HTTP_POST, .handler = navtouch_handler
    };
    extern esp_err_t heap_handler(httpd_req_t *req);
    const httpd_uri_t uri_heap = {
        .uri = "/heap", .method = HTTP_GET, .handler = heap_handler
    };

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_screenshot);
    httpd_register_uri_handler(server, &uri_screenshot_bmp);
    httpd_register_uri_handler(server, &uri_screenshot_jpg);
    httpd_register_uri_handler(server, &uri_info);
    httpd_register_uri_handler(server, &uri_touch);
    httpd_register_uri_handler(server, &uri_reboot);
    httpd_register_uri_handler(server, &uri_log);
    httpd_register_uri_handler(server, &uri_open);
    httpd_register_uri_handler(server, &uri_crashlog);
    httpd_register_uri_handler(server, &uri_coredump);
    httpd_register_uri_handler(server, &uri_sdcard);
    httpd_register_uri_handler(server, &uri_wake);
    httpd_register_uri_handler(server, &uri_settings_get);
    httpd_register_uri_handler(server, &uri_settings_set);
    httpd_register_uri_handler(server, &uri_mode_set);
    httpd_register_uri_handler(server, &uri_navigate);
    httpd_register_uri_handler(server, &uri_widget);
    httpd_register_uri_handler(server, &uri_camera);
    httpd_register_uri_handler(server, &uri_ota_check);
    httpd_register_uri_handler(server, &uri_ota_apply);
    httpd_register_uri_handler(server, &uri_chat);
    httpd_register_uri_handler(server, &uri_voice_state);
    httpd_register_uri_handler(server, &uri_voice_reconnect);
    httpd_register_uri_handler(server, &uri_selftest);
    httpd_register_uri_handler(server, &uri_navtouch);
    httpd_register_uri_handler(server, &uri_heap);

    /* Log the URL */
    char ip[20];
    get_wifi_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "Debug server: http://%s:%d/", ip, DEBUG_PORT);

    return ESP_OK;
}

esp_err_t tab5_debug_server_stop(void)
{
    if (s_httpd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = httpd_stop(s_httpd);
    s_httpd = NULL;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Debug server stopped");
    } else {
        ESP_LOGW(TAG, "httpd_stop returned %s", esp_err_to_name(err));
    }
    return err;
}
