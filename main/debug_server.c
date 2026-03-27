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
#include "display.h"
#include "ui_core.h"
#include "ui_wifi.h"
#include "ui_camera.h"
#include "ui_settings.h"
#include "ui_files.h"
#include "ui_home.h"
#include "wifi.h"
#include "battery.h"
#include "dragon_link.h"

#include <string.h>
#include <stdio.h>
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

    /* Try to lock LVGL with timeout — don't block the HTTP server forever */
    if (!tab5_ui_try_lock(2000)) {
        ESP_LOGW(TAG, "Screenshot: LVGL lock timeout (2s) — copying without lock");
        /* Still copy the framebuffer but without lock (may have tearing) */
        esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        memcpy(fb_copy, fb, fb_size);
    } else {
        esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        memcpy(fb_copy, fb, fb_size);
        tab5_ui_unlock();
    }

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

    char display_str[16];
    snprintf(display_str, sizeof(display_str), "%dx%d", FB_W, FB_H);
    cJSON_AddStringToObject(root, "display", display_str);

    cJSON_AddNumberToObject(root, "fps", (double)tab5_dragon_get_fps());

    tab5_battery_info_t bat = {0};
    tab5_battery_read(&bat);
    cJSON_AddNumberToObject(root, "battery_pct", (double)bat.percent);

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    cJSON_AddNumberToObject(root, "tasks", (double)task_count);

    const char *reset_reasons[] = {"UNKNOWN","POWERON","EXT","SW","PANIC","INT_WDT","TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO","USB","JTAG","EFUSE","PWR_GLITCH","CPU_LOCKUP"};
    esp_reset_reason_t reason = esp_reset_reason();
    cJSON_AddStringToObject(root, "reset_reason", reason < sizeof(reset_reasons)/sizeof(reset_reasons[0]) ? reset_reasons[reason] : "UNKNOWN");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* ======================================================================== */
/*  POST /touch                                                              */
/* ======================================================================== */

static esp_err_t touch_handler(httpd_req_t *req)
{
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
        /* tap: press, hold 100ms, release */
        s_inject_x = x;
        s_inject_y = y;
        s_inject_pressed = true;
        s_inject_active = true;
        vTaskDelay(pdMS_TO_TICKS(100));
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
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

/* ======================================================================== */
/*  POST /open?screen=wifi  — open a screen directly for testing             */
/* ======================================================================== */

/* Deferred screen open — runs on the LVGL timer task */
static volatile int s_pending_screen = -1;

static void deferred_open_cb(lv_timer_t *t)
{
    int scr_id = s_pending_screen;
    s_pending_screen = -1;
    lv_timer_delete(t);

    switch (scr_id) {
    case 0: ui_wifi_create(); break;
    case 1: ui_camera_create(); break;
    case 2: ui_settings_create(); break;
    case 3: ui_files_create(); break;
    case 4: lv_screen_load(ui_home_get_screen()); break;
    default: break;
    }
}

static esp_err_t open_handler(httpd_req_t *req)
{
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
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown screen");
        return ESP_FAIL;
    }

    /* Defer to LVGL timer task — animations require LVGL thread context */
    s_pending_screen = scr_id;
    if (!tab5_ui_try_lock(2000)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LVGL lock timeout");
        return ESP_FAIL;
    }
    lv_timer_create(deferred_open_cb, 10, NULL);
    tab5_ui_unlock();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ======================================================================== */
/*  GET /crashlog — last core dump summary                                   */
/* ======================================================================== */

static esp_err_t crashlog_handler(httpd_req_t *req)
{
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

        cJSON *bt = cJSON_CreateArray();
        if (bt) {
            for (int i = 0; i < summary.exc_bt_info.depth && i < 32; i++) {
                cJSON_AddItemToArray(bt,
                    cJSON_CreateNumber((double)summary.exc_bt_info.bt[i]));
            }
            cJSON_AddItemToObject(root, "backtrace", bt);
        }
        cJSON_AddStringToObject(root, "hint",
            "Use 'espcoredump.py info_corefile' on host for full decode");
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

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = DEBUG_PORT;
    config.stack_size  = 12288;  /* 12 KB — was 8 KB, tight with concurrent WiFi scan */
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;

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

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_screenshot);
    httpd_register_uri_handler(server, &uri_screenshot_bmp);
    httpd_register_uri_handler(server, &uri_info);
    httpd_register_uri_handler(server, &uri_touch);
    httpd_register_uri_handler(server, &uri_reboot);
    httpd_register_uri_handler(server, &uri_log);
    httpd_register_uri_handler(server, &uri_open);
    httpd_register_uri_handler(server, &uri_crashlog);

    /* Log the URL */
    char ip[20];
    get_wifi_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "Debug server: http://%s:%d/", ip, DEBUG_PORT);

    return ESP_OK;
}
