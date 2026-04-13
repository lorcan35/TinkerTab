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
#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "ui_camera";

/* ── Palette ─────────────────────────────────────────────────── */
#define COL_BG          0x000000
#define COL_BAR_BG      0x0F172A
#define COL_WHITE       0xFFFFFF
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
    bool cam_ok = tab5_camera_initialized();

    /* Find highest existing IMG_NNNN.jpg to avoid overwriting */
    if (!capture_counter_init && tab5_sdcard_mounted()) {
        char path[32];
        for (uint32_t i = 9999; i > 0; i--) {
            snprintf(path, sizeof(path), "/sdcard/IMG_%04"PRIu32".jpg", i);
            struct stat st;
            if (stat(path, &st) == 0) {
                capture_counter = i + 1;
                ESP_LOGI(TAG, "Resuming capture counter at %"PRIu32, capture_counter);
                break;
            }
        }
        capture_counter_init = true;
    }

    /* ── Screen ──────────────────────────────────────────────── */
    scr_camera = lv_obj_create(NULL);
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
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x333333), 0);
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
        alloc_canvas_buffer(w, h);

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

    /* Only blit if the frame matches our canvas dimensions and is RGB565 */
    if (frame.format != TAB5_CAM_FMT_RGB565) return;
    if (frame.width != canvas_w || frame.height != canvas_h) return;

    /* Copy frame data into the canvas buffer */
    uint32_t copy_size = (uint32_t)frame.width * frame.height * 2;
    if (copy_size > canvas_buf_size) copy_size = canvas_buf_size;
    memcpy(canvas_buf, frame.data, copy_size);

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

    /* Build filename with incrementing counter */
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/IMG_%04"PRIu32".jpg",
             capture_counter);

    err = tab5_camera_save_jpeg(&frame, path);
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

    toast_show("Photo saved!");
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
