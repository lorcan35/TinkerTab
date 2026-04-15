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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "debug_srv";

#define DEBUG_PORT 8080

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
    ESP_LOGI(TAG, "Debug server auth token: %s", s_auth_token);
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
    if (strncmp(auth_header, "Bearer ", 7) != 0 || strcmp(auth_header + 7, s_auth_token) != 0) {
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

    size_t fb_size = FB_W * FB_H * FB_BPP;

    /* Allocate a temporary copy buffer in PSRAM so we don't hold the LVGL
     * lock during the (slow) HTTP chunked streaming. */
    uint8_t *fb_copy = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!fb_copy) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "PSRAM alloc failed for screenshot");
        return ESP_FAIL;
    }

    /* HW03: Lock LVGL to prevent tearing during framebuffer copy.
     * If the lock times out, return HTTP 503 instead of copying without
     * the lock — an unlocked copy races with LVGL rendering and produces
     * torn frames that are worse than no screenshot at all. */
    if (!tab5_ui_try_lock(2000)) {
        ESP_LOGW(TAG, "Screenshot: LVGL lock timeout (2s) — returning 503");
        heap_caps_free(fb_copy);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"display busy — LVGL lock timeout\"}");
        return ESP_OK;
    }

    /* Invalidate CPU cache so we read the latest PSRAM contents.
     * LVGL flush callback does C2M (write-back) on Core 0; this core's
     * cache may hold stale lines from a previous screenshot read. */
    esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(fb_copy, fb, fb_size);
    tab5_ui_unlock();

    /* Build BMP header */
    uint8_t hdr[BMP_HEADER_SIZE];
    build_bmp_header(hdr, FB_W, FB_H);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    /* Send header */
    ret = httpd_resp_send_chunk(req, (const char *)hdr, BMP_HEADER_SIZE);
    if (ret != ESP_OK) {
        free(fb_copy);
        return ret;
    }

    /* Send pixel rows bottom-up (BMP convention) from the copy. */
    uint32_t row_bytes = FB_W * FB_BPP;

    for (int y = FB_H - 1; y >= 0; y--) {
        ret = httpd_resp_send_chunk(req, (const char *)(fb_copy + y * row_bytes), row_bytes);
        if (ret != ESP_OK) {
            free(fb_copy);
            return ret;
        }
    }

    free(fb_copy);

    /* End chunked response */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
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

    /* AFE / wake word */
    cJSON_AddBoolToObject(root, "afe_active", tab5_afe_is_active());
    cJSON_AddBoolToObject(root, "wake_listening", voice_is_always_listening());
    if (tab5_afe_is_active()) {
        cJSON_AddStringToObject(root, "wake_word", tab5_afe_wake_word_name());
    }

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

    cJSON *jx = cJSON_GetObjectItem(root, "x");
    cJSON *jy = cJSON_GetObjectItem(root, "y");
    cJSON *jaction = cJSON_GetObjectItem(root, "action");

    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Need x and y");
        return ESP_FAIL;
    }

    int x = jx->valueint;
    int y = jy->valueint;
    const char *action = (jaction && cJSON_IsString(jaction)) ? jaction->valuestring : "tap";

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
"    <img id='screen' src='/screenshot.bmp' alt='screenshot' draggable='false'>"
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
"  img.src='/screenshot.bmp?t='+Date.now();"
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

/* ── Wake word toggle ────────────────────────────────────────────────── */

static esp_err_t wake_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    if (voice_is_always_listening()) {
        voice_stop_always_listening();
    } else {
        voice_start_always_listening();
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "afe_active", tab5_afe_is_active());
    cJSON_AddBoolToObject(root, "wake_listening", voice_is_always_listening());
    if (tab5_afe_is_active()) {
        cJSON_AddStringToObject(root, "wake_word", tab5_afe_wake_word_name());
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
    ESP_LOGI("ota", "Applying OTA from: %s (sha256=%s)", args->url,
             args->sha256[0] ? args->sha256 : "none");
    esp_err_t err = tab5_ota_apply(args->url, args->sha256[0] ? args->sha256 : NULL);
    /* If we get here, it failed (success reboots) */
    ESP_LOGE("ota", "OTA apply failed: %s", esp_err_to_name(err));
    free(args->url);
    free(args);
    vTaskDelete(NULL);
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
    cJSON_AddNumberToObject(root, "wake_word", tab5_settings_get_wake_word());
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
    ui_chat_hide();
    ui_settings_hide();
    ui_notes_hide();
    ui_keyboard_hide();

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
    }
    s_navigating = false;
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

    const char *state_names[] = {"IDLE", "CONNECTING", "READY", "LISTENING", "PROCESSING", "SPEAKING", "DICTATING"};
    int st = (int)voice_get_state();
    cJSON_AddStringToObject(root, "state_name", (st >= 0 && st <= 6) ? state_names[st] : "UNKNOWN");

    /* Include last LLM text if available */
    const char *llm_text = voice_get_llm_text();
    if (llm_text && llm_text[0]) {
        cJSON_AddStringToObject(root, "last_llm_text", llm_text);
    }

    /* Include last STT text */
    const char *stt_text = voice_get_stt_text();
    if (stt_text && stt_text[0]) {
        cJSON_AddStringToObject(root, "last_stt_text", stt_text);
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

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = DEBUG_PORT;
    config.stack_size  = 12288;
    config.max_uri_handlers = 28;
    config.lru_purge_enable = true;
    config.max_open_sockets = 16;         /* Needs headroom for rapid API calls (nav+info pairs) */
    config.recv_wait_timeout = 5;         /* 5s recv timeout (default 5) */
    config.send_wait_timeout = 5;         /* 5s send timeout (default 5) */
    config.close_fn = NULL;               /* Use default close */
    /* Run httpd on Core 1 so it doesn't starve when LVGL is busy on Core 0.
     * Settings screen creates 55 objects (~500ms) which blocks Core 0 entirely.
     * Without this, /info requests time out during heavy LVGL rendering. */
    config.core_id = 1;
    config.task_priority = tskIDLE_PRIORITY + 6;  /* Above LVGL (prio 5) */

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start debug server: %s", esp_err_to_name(ret));
        return ret;
    }

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
    const httpd_uri_t uri_mode_set = {
        .uri = "/mode", .method = HTTP_POST, .handler = mode_set_handler
    };
    const httpd_uri_t uri_navigate = {
        .uri = "/navigate", .method = HTTP_POST, .handler = navigate_handler
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

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_screenshot);
    httpd_register_uri_handler(server, &uri_screenshot_bmp);
    httpd_register_uri_handler(server, &uri_info);
    httpd_register_uri_handler(server, &uri_touch);
    httpd_register_uri_handler(server, &uri_reboot);
    httpd_register_uri_handler(server, &uri_log);
    httpd_register_uri_handler(server, &uri_open);
    httpd_register_uri_handler(server, &uri_crashlog);
    httpd_register_uri_handler(server, &uri_sdcard);
    httpd_register_uri_handler(server, &uri_wake);
    httpd_register_uri_handler(server, &uri_settings_get);
    httpd_register_uri_handler(server, &uri_mode_set);
    httpd_register_uri_handler(server, &uri_navigate);
    httpd_register_uri_handler(server, &uri_camera);
    httpd_register_uri_handler(server, &uri_ota_check);
    httpd_register_uri_handler(server, &uri_ota_apply);
    httpd_register_uri_handler(server, &uri_chat);
    httpd_register_uri_handler(server, &uri_voice_state);
    httpd_register_uri_handler(server, &uri_voice_reconnect);
    httpd_register_uri_handler(server, &uri_selftest);
    httpd_register_uri_handler(server, &uri_navtouch);

    /* Log the URL */
    char ip[20];
    get_wifi_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "Debug server: http://%s:%d/", ip, DEBUG_PORT);

    return ESP_OK;
}
