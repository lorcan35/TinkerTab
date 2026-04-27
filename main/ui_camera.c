/*
 * ui_camera.c — TinkerTab camera viewfinder screen
 * 720x1280 portrait, LVGL v9, dark theme
 *
 * Layout:
 *   Top 960px   — viewfinder canvas (camera preview)
 *   Bottom 320px — control bar (capture, resolution, gallery)
 */

#include "ui_camera.h"
#include "ui_home.h"
#include "ui_files.h"
#include "ui_feedback.h"
#include "camera.h"
#include "sdcard.h"
#include "voice.h"      /* U11 follow-up: voice_upload_chat_image() */
#include "settings.h"   /* #260: cam_rot NVS key */
#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "ui_camera";

/* ── Palette ─────────────────────────────────────────────────── */
#define COL_BG          0x08080E
#define COL_BAR_BG      0x0F172A
#define COL_WHITE       0xE8E8EF
#define COL_GRAY        0x94A3B8
#define COL_ACCENT      0x3B82F6
#define COL_GREEN       0x22C55E
#define COL_RED         0xEF4444

/* ── Layout (720 x 1280) ────────────────────────────────────── */
#define SCREEN_W        720
#define SCREEN_H        1280
#define VIEWFINDER_H    960
#define CONTROL_BAR_H   320

/* ── Preview timer ───────────────────────────────────────────── */
#define PREVIEW_FPS_MS  100   /* ~10 fps */

/* U11 photo-share follow-up: armed by ui_chat's camera button before
 * launching the camera screen.  capture_btn_cb consumes + clears it on
 * the next save so a regular Camera-from-nav capture isn't auto-shared. */
static bool s_chat_share_armed = false;

void ui_camera_arm_chat_share(void)
{
    s_chat_share_armed = true;
}

/* ── Toast duration ──────────────────────────────────────────── */
#define TOAST_DURATION_MS  2000

/* ── Module state ────────────────────────────────────────────── */
static lv_obj_t   *scr_camera      = NULL;
static lv_obj_t   *canvas_preview  = NULL;
static lv_obj_t   *lbl_no_camera   = NULL;
static lv_obj_t   *btn_capture     = NULL;
static lv_obj_t   *lbl_no_sd       = NULL;
static lv_obj_t   *dd_resolution   = NULL;
static lv_obj_t   *btn_gallery     = NULL;
static lv_obj_t   *lbl_gallery     = NULL;
static lv_obj_t   *toast_obj       = NULL;

static lv_timer_t *preview_timer   = NULL;
static lv_timer_t *toast_timer     = NULL;

static uint8_t    *canvas_buf      = NULL;
static uint32_t    canvas_buf_size = 0;

static uint16_t    canvas_w        = 0;
static uint16_t    canvas_h        = 0;

/* #260: snapshot of NVS cam_rot at screen-create time.
 * 0 = 0deg (raw memcpy, fastest path), 1 = 90 CW, 2 = 180, 3 = 270 CW.
 * Snapshotted (not re-read each frame) so the buffer dimensions stay
 * consistent for the lifetime of the canvas widget. */
static uint8_t     s_cam_rot       = 0;

static uint32_t    capture_counter = 0;
static bool        capture_counter_init = false;

/* Currently selected resolution (default VGA for smooth preview) */
static tab5_cam_resolution_t current_res = TAB5_CAM_RES_HD;  /* SC202CS outputs 1280x720 */

/* ── Forward declarations ────────────────────────────────────── */
static void cb_back_btn(lv_event_t *e);
static void preview_timer_cb(lv_timer_t *t);
static void capture_btn_cb(lv_event_t *e);
static void resolution_dd_cb(lv_event_t *e);
static void toast_show(const char *text);
static void toast_timer_cb(lv_timer_t *t);
static void update_sd_state(void);
static void alloc_canvas_buffer(uint16_t w, uint16_t h);
static void free_canvas_buffer(void);

/* ================================================================
 * Resolution helpers
 * ================================================================ */
static void res_to_dimensions(tab5_cam_resolution_t res,
                              uint16_t *w, uint16_t *h)
{
    switch (res) {
    case TAB5_CAM_RES_QVGA: *w = 320;  *h = 240;  break;
    case TAB5_CAM_RES_VGA:  *w = 640;  *h = 480;  break;
    case TAB5_CAM_RES_HD:   *w = 1280; *h = 720;  break;
    case TAB5_CAM_RES_FULL: *w = 1920; *h = 1080; break;
    default:                *w = 640;  *h = 480;  break;
    }
}

/* ================================================================
 * Canvas buffer management (PSRAM)
 * ================================================================ */
static void alloc_canvas_buffer(uint16_t w, uint16_t h)
{
    free_canvas_buffer();

    /* RGB565: 2 bytes per pixel */
    canvas_w = w;
    canvas_h = h;
    canvas_buf_size = (uint32_t)w * h * 2;

    canvas_buf = heap_caps_malloc(canvas_buf_size, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer (%"PRIu32" bytes)",
                 canvas_buf_size);
        canvas_buf_size = 0;
        canvas_w = 0;
        canvas_h = 0;
        return;
    }
    memset(canvas_buf, 0, canvas_buf_size);
    ESP_LOGI(TAG, "Canvas buffer: %ux%u (%"PRIu32" bytes in PSRAM)",
             w, h, canvas_buf_size);
}

static void free_canvas_buffer(void)
{
    if (canvas_buf) {
        heap_caps_free(canvas_buf);
        canvas_buf = NULL;
        canvas_buf_size = 0;
        canvas_w = 0;
        canvas_h = 0;
    }
}

/* ================================================================
 * #260: RGB565 in-place rotation helpers.
 *
 * Source frame is sw x sh.  Destination is the canvas buffer,
 * sized appropriately by the caller (90/270 = sh x sw, 180 = sw x sh).
 *
 * Naive transpose loops; ~30-40 ms per 1280x720 frame on PSRAM.
 * Acceptable for the 10 FPS preview (100 ms budget).  If we ever
 * raise preview FPS, swap in tile-based rotation (32x32 cache lines)
 * or the ESP32-P4 PPA hardware path.
 * ================================================================ */
static void rotate_rgb565_180(const uint16_t *src, uint16_t *dst,
                              int sw, int sh)
{
    /* dst[(sh-1-y), (sw-1-x)] = src[y, x] */
    int npix = sw * sh;
    for (int i = 0; i < npix; i++) {
        dst[npix - 1 - i] = src[i];
    }
}

static void rotate_rgb565_90cw(const uint16_t *src, uint16_t *dst,
                               int sw, int sh)
{
    /* dst is sh wide, sw tall.  dst[(sh-1-y) + x*sh] = src[x + y*sw] */
    for (int y = 0; y < sh; y++) {
        const uint16_t *srow = src + y * sw;
        for (int x = 0; x < sw; x++) {
            dst[(sh - 1 - y) + x * sh] = srow[x];
        }
    }
}

static void rotate_rgb565_270cw(const uint16_t *src, uint16_t *dst,
                                int sw, int sh)
{
    /* dst is sh wide, sw tall.  dst[y + (sw-1-x)*sh] = src[x + y*sw] */
    for (int y = 0; y < sh; y++) {
        const uint16_t *srow = src + y * sw;
        for (int x = 0; x < sw; x++) {
            dst[y + (sw - 1 - x) * sh] = srow[x];
        }
    }
}

/* ── Gallery button — open file browser ───────────────────────── */
static void gallery_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Gallery tapped — opening file browser");
    ui_camera_destroy();
    ui_files_create();
}

/* ── Back button callback ────────────────────────────────────── */
static void cb_back_btn(lv_event_t *e)
{
    (void)e;
    ui_camera_destroy();
    lv_screen_load(ui_home_get_screen());
}

/* ================================================================
 * ui_camera_create
 * ================================================================ */
lv_obj_t *ui_camera_create(void)
{
    /* #172: idempotent re-entry.  Camera owns a ~1.8 MB PSRAM canvas
     * buffer + a preview timer + a full LVGL screen tree.  If the
     * create path is hit twice without an intervening destroy (debug
     * /navigate racing with a tile tap, duplicate async navigation
     * during screen churn), we'd overwrite \`scr_camera\` / \`canvas_buf\`
     * and leak the previous instance + leave its preview timer firing
     * on a dangling canvas.  Short-circuit when we're already live. */
    if (scr_camera) {
        lv_screen_load(scr_camera);
        return scr_camera;
    }

    bool cam_ok = tab5_camera_initialized();

    /* U18 (#206): resume the capture counter from existing IMG_NNNN.jpg
     * filenames on the SD card.
     *
     * Pre-fix: 9999 stat() calls in a loop, run once on first camera
     * open.  Two failure modes:
     *   - Slow SD or large card → 9999 stats can take seconds and
     *     trip the LVGL/IDLE WDT before the screen is even built.
     *   - SD inserted AFTER first camera open → the init flag was
     *     set permanently when the previous open ran with no SD,
     *     so the counter stayed at 0 and the next capture overwrote
     *     IMG_0000.jpg (or worse, clobbered an existing photo).
     *
     * Post-fix: single opendir+readdir scan (one pass over directory
     * entries, not a 0..9999 brute force), and gate on actual SD mount
     * state so a late insert re-scans on the next ui_camera_create. */
    if (tab5_sdcard_mounted()) {
        if (!capture_counter_init) {
            DIR *d = opendir("/sdcard");
            uint32_t highest = 0;
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    /* Match IMG_NNNN.jpg / IMG_NNNN.JPG (case-insensitive
                     * because FATFS short-name mode upcases everything). */
                    if (strncasecmp(de->d_name, "IMG_", 4) != 0) continue;
                    const char *dot = strrchr(de->d_name, '.');
                    if (!dot || strcasecmp(dot, ".jpg") != 0) continue;
                    /* Parse the 4-digit number between IMG_ and .jpg. */
                    char num_buf[8] = {0};
                    size_t num_len = (size_t)(dot - (de->d_name + 4));
                    if (num_len == 0 || num_len >= sizeof(num_buf)) continue;
                    memcpy(num_buf, de->d_name + 4, num_len);
                    char *end = NULL;
                    unsigned long n = strtoul(num_buf, &end, 10);
                    if (end != num_buf + num_len) continue;
                    if (n > highest) highest = (uint32_t)n;
                }
                closedir(d);
            }
            capture_counter = highest + 1;
            capture_counter_init = true;
            ESP_LOGI(TAG, "Capture counter resumed at %"PRIu32 " (scanned existing IMG_*.jpg)",
                     capture_counter);
        }
    } else {
        /* SD not mounted (yet).  Don't latch capture_counter_init —
         * next open after the user inserts the card will re-scan. */
        capture_counter_init = false;
    }

    /* ── Screen ──────────────────────────────────────────────── */
    scr_camera = lv_obj_create(NULL);
    if (!scr_camera) {
        ESP_LOGE(TAG, "OOM: failed to create camera screen");
        return NULL;
    }
    lv_obj_set_size(scr_camera, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr_camera, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_camera, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_camera, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr_camera, 0, 0);

    /* ── Top bar with back button ────────────────────────────── */
    {
        lv_obj_t *topbar = lv_obj_create(scr_camera);
        lv_obj_remove_style_all(topbar);
        lv_obj_set_size(topbar, SCREEN_W, 48);
        lv_obj_set_style_bg_color(topbar, lv_color_hex(COL_BAR_BG), 0);
        lv_obj_set_style_bg_opa(topbar, LV_OPA_80, 0);
        lv_obj_align(topbar, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *btn_back = lv_button_create(topbar);
        lv_obj_remove_style_all(btn_back);
        lv_obj_set_size(btn_back, 80, 48);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x1A1A24), 0);
        lv_obj_set_style_bg_opa(btn_back, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn_back, 12, 0);
        lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_add_event_cb(btn_back, cb_back_btn, LV_EVENT_CLICKED, NULL);
        ui_fb_button(btn_back);

        lv_obj_t *arrow = lv_label_create(btn_back);
        lv_label_set_text(arrow, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(arrow, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(arrow, &lv_font_montserrat_24, 0);
        lv_obj_center(arrow);

        /* v4·D Phase 4b vision chip — right-aligned violet pill that reads
         * "VISION · <MODEL>  READY" when Dragon has advertised a
         * vision-capable LLM backend.  When not capable (local-only
         * ollama without "vision" in name), the chip is hidden so the
         * top bar stays clean.  See system-d-modes.html M8 mockup +
         * system-d-sovereign.html camera screen. */
        {
            extern bool voice_get_vision_capability(char *, size_t, int *);
            char vm[64] = {0};
            int per_frame_mils = 0;
            bool vcap = voice_get_vision_capability(vm, sizeof(vm), &per_frame_mils);
            if (vcap && vm[0]) {
                /* Shorten model ID for chip: "anthropic/claude-3.5-haiku"
                 * -> "haiku-3.5".  Same recipe as chat receipt stamp. */
                char short_vm[24] = {0};
                const char *slash = strchr(vm, '/');
                const char *tail  = slash ? slash + 1 : vm;
                const char *hy    = strchr(tail, '-');
                const char *start = hy ? hy + 1 : tail;
                snprintf(short_vm, sizeof(short_vm), "%s", start);

                lv_obj_t *vchip = lv_obj_create(topbar);
                lv_obj_remove_style_all(vchip);
                lv_obj_set_size(vchip, 280, 40);
                lv_obj_set_style_bg_color(vchip, lv_color_hex(0x1A1020), 0);
                lv_obj_set_style_bg_opa(vchip, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(vchip, 20, 0);
                lv_obj_set_style_border_width(vchip, 1, 0);
                lv_obj_set_style_border_color(vchip, lv_color_hex(0xA78BFA), 0);
                lv_obj_align(vchip, LV_ALIGN_RIGHT_MID, -8, 0);
                lv_obj_clear_flag(vchip, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t *vlbl = lv_label_create(vchip);
                char buf[64];
                snprintf(buf, sizeof(buf), "\xe2\x80\xa2 VISION  %s  READY", short_vm);
                lv_label_set_text(vlbl, buf);
                lv_obj_set_style_text_font(vlbl, FONT_CHAT_MONO, 0);
                lv_obj_set_style_text_color(vlbl, lv_color_hex(0xA78BFA), 0);
                lv_obj_set_style_text_letter_space(vlbl, 2, 0);
                lv_obj_center(vlbl);
            }
        }
    }

    /* ── Viewfinder area (below topbar) ─────────────────────────── */
    lv_obj_t *vf_area = lv_obj_create(scr_camera);
    lv_obj_set_size(vf_area, SCREEN_W, VIEWFINDER_H - 48);
    lv_obj_align(vf_area, LV_ALIGN_TOP_LEFT, 0, 48);
    lv_obj_set_style_bg_color(vf_area, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(vf_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vf_area, 0, 0);
    lv_obj_set_style_pad_all(vf_area, 0, 0);
    lv_obj_set_style_radius(vf_area, 0, 0);
    lv_obj_clear_flag(vf_area, LV_OBJ_FLAG_SCROLLABLE);

    if (cam_ok) {
        /* Set initial resolution — feed watchdog as camera init can be slow */
        esp_task_wdt_reset();
        tab5_camera_set_resolution(current_res);

        uint16_t w, h;
        res_to_dimensions(current_res, &w, &h);
        esp_task_wdt_reset();
        /* #260: snapshot rotation pref + allocate canvas with the post-
         * rotation dimensions so 90/270 give a portrait-fit viewfinder. */
        s_cam_rot = tab5_settings_get_cam_rotation() & 0x03;
        if (s_cam_rot == 1 || s_cam_rot == 3) {
            alloc_canvas_buffer(h, w);  /* dimensions swapped */
        } else {
            alloc_canvas_buffer(w, h);
        }

        if (canvas_buf) {
            canvas_preview = lv_canvas_create(vf_area);
            lv_canvas_set_buffer(canvas_preview, canvas_buf,
                                 canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
            lv_obj_center(canvas_preview);

            /* Start the preview timer */
            preview_timer = lv_timer_create(preview_timer_cb, PREVIEW_FPS_MS,
                                            NULL);
            ESP_LOGI(TAG, "Preview started at ~%d fps",
                     1000 / PREVIEW_FPS_MS);
        }
    } else {
        /* Camera unavailable — show placeholder text */
        lbl_no_camera = lv_label_create(vf_area);
        lv_label_set_text(lbl_no_camera, "Camera not available");
        lv_obj_set_style_text_color(lbl_no_camera,
                                    lv_color_hex(COL_GRAY), 0);
        lv_obj_set_style_text_font(lbl_no_camera,
                                   &lv_font_montserrat_24, 0);
        lv_obj_center(lbl_no_camera);
    }

    /* ── Control bar (bottom 320px) ──────────────────────────── */
    lv_obj_t *bar = lv_obj_create(scr_camera);
    lv_obj_set_size(bar, SCREEN_W, CONTROL_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BAR_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Capture button (center, 80px diameter) ──────────────── */
    btn_capture = lv_obj_create(bar);
    lv_obj_set_size(btn_capture, 80, 80);
    lv_obj_align(btn_capture, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_radius(btn_capture, 40, 0);
    lv_obj_set_style_bg_color(btn_capture, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_bg_opa(btn_capture, LV_OPA_30, 0);
    lv_obj_set_style_border_color(btn_capture, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_border_width(btn_capture, 4, 0);
    lv_obj_set_style_border_opa(btn_capture, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(btn_capture, 0, 0);
    lv_obj_clear_flag(btn_capture, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_capture, LV_OBJ_FLAG_CLICKABLE);

    /* Inner solid circle for the shutter button look */
    lv_obj_t *btn_inner = lv_obj_create(btn_capture);
    lv_obj_set_size(btn_inner, 60, 60);
    lv_obj_center(btn_inner);
    lv_obj_set_style_radius(btn_inner, 30, 0);
    lv_obj_set_style_bg_color(btn_inner, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_bg_opa(btn_inner, LV_OPA_90, 0);
    lv_obj_set_style_border_width(btn_inner, 0, 0);
    lv_obj_clear_flag(btn_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn_inner, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn_capture, capture_btn_cb, LV_EVENT_CLICKED, NULL);
    ui_fb_button_colored(btn_capture, 0xCCCCCC);  /* Darken white on press */

    /* ── "No SD" label below capture button (hidden by default) ── */
    lbl_no_sd = lv_label_create(bar);
    lv_label_set_text(lbl_no_sd, "No SD");
    lv_obj_set_style_text_color(lbl_no_sd, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(lbl_no_sd, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_no_sd, LV_ALIGN_CENTER, 0, 40);

    /* ── Resolution dropdown (left side) ─────────────────────── */
    dd_resolution = lv_dropdown_create(bar);
    lv_dropdown_set_options(dd_resolution,
                            "QVGA 320x240\n"
                            "VGA 640x480\n"
                            "HD 1280x720\n"
                            "Full 1920x1080");
    lv_dropdown_set_selected(dd_resolution, (uint32_t)current_res);
    lv_obj_set_size(dd_resolution, 200, 56);
    lv_obj_align(dd_resolution, LV_ALIGN_LEFT_MID, 24, -20);

    /* Dropdown styling */
    lv_obj_set_style_bg_color(dd_resolution, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_bg_opa(dd_resolution, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(dd_resolution, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(dd_resolution, &lv_font_montserrat_18, 0);
    lv_obj_set_style_border_color(dd_resolution, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(dd_resolution, 1, 0);
    lv_obj_set_style_radius(dd_resolution, 8, 0);
    lv_obj_set_style_pad_hor(dd_resolution, 12, 0);

    /* Dropdown list (popup) styling */
    lv_obj_t *dd_list = lv_dropdown_get_list(dd_resolution);
    if (dd_list) {
        lv_obj_set_style_bg_color(dd_list, lv_color_hex(0x1E293B),   0);
        lv_obj_set_style_bg_opa(dd_list, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(dd_list, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_18,  0);
        lv_obj_set_style_border_color(dd_list,
                                      lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_border_width(dd_list, 1, 0);
    }

    lv_obj_add_event_cb(dd_resolution, resolution_dd_cb,
                         LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Gallery button (right side) ─────────────────────────── */
    btn_gallery = lv_obj_create(bar);
    lv_obj_set_size(btn_gallery, 140, 56);
    lv_obj_align(btn_gallery, LV_ALIGN_RIGHT_MID, -24, -20);
    lv_obj_set_style_bg_color(btn_gallery, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_bg_opa(btn_gallery, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_gallery, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(btn_gallery, 1, 0);
    lv_obj_set_style_radius(btn_gallery, 8, 0);
    lv_obj_set_style_pad_all(btn_gallery, 0, 0);
    lv_obj_clear_flag(btn_gallery, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_gallery, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn_gallery, gallery_btn_cb, LV_EVENT_CLICKED, NULL);
    ui_fb_card(btn_gallery);

    lbl_gallery = lv_label_create(btn_gallery);
    lv_label_set_text(lbl_gallery, "Gallery");
    lv_obj_set_style_text_color(lbl_gallery, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_gallery, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_gallery);

    /* ── Update SD card state ────────────────────────────────── */
    update_sd_state();

    /* ── Load the screen ─────────────────────────────────────── */
    lv_screen_load(scr_camera);
    ESP_LOGI(TAG, "Camera screen created");

    return scr_camera;
}

/* ================================================================
 * Preview timer — captures frames and updates canvas
 * ================================================================ */
static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (!canvas_preview || !canvas_buf) return;

    tab5_cam_frame_t frame;
    esp_err_t err = tab5_camera_capture(&frame);
    if (err != ESP_OK) return;

    if (frame.format != TAB5_CAM_FMT_RGB565) return;

    /* #260: rotate the captured frame into the canvas buffer.
     * For rotation 0/180 the frame and canvas share dimensions
     * (sw x sh).  For 90/270 the canvas is allocated swapped
     * (sh x sw) — see alloc_canvas_buffer in ui_camera_create. */
    const uint16_t *src = (const uint16_t *)frame.data;
    uint16_t *dst = (uint16_t *)canvas_buf;
    int sw = frame.width;
    int sh = frame.height;
    uint32_t expected = (uint32_t)sw * sh * 2;
    if (expected > canvas_buf_size) return;

    switch (s_cam_rot) {
    case 1:  /* 90 CW: source w/h swapped on output */
        if (canvas_w != sh || canvas_h != sw) return;
        rotate_rgb565_90cw(src, dst, sw, sh);
        break;
    case 2:  /* 180: same dimensions */
        if (canvas_w != sw || canvas_h != sh) return;
        rotate_rgb565_180(src, dst, sw, sh);
        break;
    case 3:  /* 270 CW (= 90 CCW): swapped */
        if (canvas_w != sh || canvas_h != sw) return;
        rotate_rgb565_270cw(src, dst, sw, sh);
        break;
    case 0:
    default:
        if (canvas_w != sw || canvas_h != sh) return;
        memcpy(dst, src, expected);
        break;
    }

    /* Tell LVGL the canvas content changed */
    lv_obj_invalidate(canvas_preview);
}

/* ================================================================
 * Capture button callback
 * ================================================================ */
static void capture_btn_cb(lv_event_t *e)
{
    (void)e;

    if (!tab5_sdcard_mounted()) {
        toast_show("No SD card!");
        return;
    }
    if (!tab5_camera_initialized()) {
        toast_show("Camera error");
        return;
    }

    tab5_cam_frame_t frame;
    esp_err_t err = tab5_camera_capture(&frame);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Capture failed: %s", esp_err_to_name(err));
        toast_show("Capture failed");
        return;
    }

    /* #260: rotate the saved photo to match what the user saw on the
     * viewfinder.  Allocate a temp PSRAM buffer; for 0deg we just save
     * the frame as-is (skip the alloc). */
    tab5_cam_frame_t saved = frame;
    uint8_t *rot_buf = NULL;
    if (s_cam_rot != 0 && frame.format == TAB5_CAM_FMT_RGB565) {
        size_t bytes = (size_t)frame.width * frame.height * 2;
        rot_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (rot_buf) {
            const uint16_t *src = (const uint16_t *)frame.data;
            uint16_t *dst = (uint16_t *)rot_buf;
            switch (s_cam_rot) {
            case 1:
                rotate_rgb565_90cw(src, dst, frame.width, frame.height);
                saved.width  = frame.height;
                saved.height = frame.width;
                break;
            case 2:
                rotate_rgb565_180(src, dst, frame.width, frame.height);
                break;
            case 3:
                rotate_rgb565_270cw(src, dst, frame.width, frame.height);
                saved.width  = frame.height;
                saved.height = frame.width;
                break;
            }
            saved.data = rot_buf;
            saved.size = bytes;
        }
    }

    /* Build filename with incrementing counter */
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/IMG_%04"PRIu32".jpg",
             capture_counter);

    err = tab5_camera_save_jpeg(&saved, path);
    if (rot_buf) heap_caps_free(rot_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
        toast_show("Save failed");
        return;
    }

    capture_counter++;
    ESP_LOGI(TAG, "Photo saved: %s", path);

    /* Update gallery button text */
    if (lbl_gallery) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%"PRIu32" photos", capture_counter);
        lv_label_set_text(lbl_gallery, buf);
    }

    /* U11 photo-share follow-up: if the camera was launched from chat
     * (s_chat_share_armed is set in ui_chat::on_camera_tap), upload the
     * fresh BMP to Dragon and let the WS broadcast back the signed
     * `media` event so the chat overlay shows it as an image bubble. */
    if (s_chat_share_armed) {
        s_chat_share_armed = false;
        voice_upload_chat_image(path);
        toast_show("Sending to chat...");
    } else {
        toast_show("Photo saved!");
    }
}

/* ================================================================
 * Resolution dropdown callback
 * ================================================================ */
static void resolution_dd_cb(lv_event_t *e)
{
    (void)e;

    uint32_t sel = lv_dropdown_get_selected(dd_resolution);
    tab5_cam_resolution_t new_res;

    switch (sel) {
    case 0:  new_res = TAB5_CAM_RES_QVGA; break;
    case 1:  new_res = TAB5_CAM_RES_VGA;  break;
    case 2:  new_res = TAB5_CAM_RES_HD;   break;
    case 3:  new_res = TAB5_CAM_RES_FULL; break;
    default: new_res = TAB5_CAM_RES_VGA;  break;
    }

    if (new_res == current_res) return;

    /* Pause preview while changing resolution */
    if (preview_timer) {
        lv_timer_pause(preview_timer);
    }

    esp_err_t err = tab5_camera_set_resolution(new_res);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set resolution failed: %s", esp_err_to_name(err));
        toast_show("Resolution error");
        if (preview_timer) lv_timer_resume(preview_timer);
        return;
    }

    current_res = new_res;

    /* Reallocate canvas for new resolution */
    uint16_t w, h;
    res_to_dimensions(current_res, &w, &h);
    alloc_canvas_buffer(w, h);

    if (canvas_preview && canvas_buf) {
        lv_canvas_set_buffer(canvas_preview, canvas_buf,
                             canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
        /* Re-center in case dimensions changed */
        lv_obj_center(canvas_preview);
    }

    if (preview_timer) {
        lv_timer_resume(preview_timer);
    }

    ESP_LOGI(TAG, "Resolution changed to %ux%u", w, h);
}

/* ================================================================
 * SD card state check
 * ================================================================ */
static void update_sd_state(void)
{
    bool mounted = tab5_sdcard_mounted();

    if (mounted) {
        lv_obj_add_flag(lbl_no_sd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(btn_capture, LV_OPA_30, 0);
        lv_obj_clear_state(btn_capture, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_flag(lbl_no_sd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(btn_capture, LV_OPA_10, 0);
        lv_obj_add_state(btn_capture, LV_STATE_DISABLED);
    }
}

/* ================================================================
 * Toast notification
 * ================================================================ */
static void toast_show(const char *text)
{
    /* Remove existing toast if any */
    if (toast_obj) {
        lv_obj_delete(toast_obj);
        toast_obj = NULL;
    }
    if (toast_timer) {
        lv_timer_delete(toast_timer);
        toast_timer = NULL;
    }

    if (!scr_camera) return;

    /* Create toast container */
    toast_obj = lv_obj_create(scr_camera);
    lv_obj_set_size(toast_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(toast_obj, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(toast_obj, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_bg_opa(toast_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast_obj, 12, 0);
    lv_obj_set_style_border_width(toast_obj, 0, 0);
    lv_obj_set_style_pad_hor(toast_obj, 24, 0);
    lv_obj_set_style_pad_ver(toast_obj, 12, 0);
    lv_obj_clear_flag(toast_obj, LV_OBJ_FLAG_SCROLLABLE);

    /* Toast text */
    lv_obj_t *lbl = lv_label_create(toast_obj);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl);

    /* Auto-dismiss timer */
    toast_timer = lv_timer_create(toast_timer_cb, TOAST_DURATION_MS, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}

static void toast_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (toast_obj) {
        /* Fade out by simply deleting */
        lv_obj_delete(toast_obj);
        toast_obj = NULL;
    }
    toast_timer = NULL;
}

/* ================================================================
 * ui_camera_destroy
 * ================================================================ */
void ui_camera_destroy(void)
{
    /* Stop preview timer first */
    if (preview_timer) {
        lv_timer_delete(preview_timer);
        preview_timer = NULL;
    }

    /* Stop toast timer */
    if (toast_timer) {
        lv_timer_delete(toast_timer);
        toast_timer = NULL;
    }

    /* Delete the screen (frees all child objects) */
    if (scr_camera) {
        lv_obj_delete(scr_camera);
        scr_camera     = NULL;
        canvas_preview = NULL;
        lbl_no_camera  = NULL;
        btn_capture    = NULL;
        lbl_no_sd      = NULL;
        dd_resolution  = NULL;
        btn_gallery    = NULL;
        lbl_gallery    = NULL;
        toast_obj      = NULL;
        ESP_LOGI(TAG, "Camera screen destroyed");
    }

    /* Free PSRAM canvas buffer */
    free_canvas_buffer();
}
