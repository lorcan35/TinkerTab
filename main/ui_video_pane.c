/*
 * ui_video_pane.c — see header.
 *
 * Two open modes:
 *   _show        — full-screen lv_image, tap to dismiss.  Phase 3B.
 *   _show_call   — same plus a local-camera PIP in the bottom-right
 *                  + End Call button bottom-left.  Phase 3D.
 *
 * The PIP runs its own 5 fps lv_timer — own consumer of
 * tab5_camera_capture, serialised by the camera's internal busy flag
 * with the streaming task.  Local-frame downscale is naive nearest-
 * neighbour (1280x720 -> 240x135) into a PSRAM RGB565 canvas; ~33 K
 * pixels per frame at 5 fps fits easily in the LVGL render budget.
 */
#include "ui_video_pane.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "ui_home.h"
#include "camera.h"
#include "settings.h"

static const char *TAG = "ui_video_pane";

#define VP_W   720
#define VP_H   1280

/* PIP geometry — bottom-right with a small margin. */
#define PIP_W   240
#define PIP_H   135   /* 16:9 */
#define PIP_PAD 16

/* End Call pill bottom-left. */
#define END_W   200
#define END_H   56
#define END_PAD 24

#define PIP_FPS_MS 200   /* 5 fps */

static lv_obj_t *s_root      = NULL;
static lv_obj_t *s_image     = NULL;
static lv_obj_t *s_pip_canvas = NULL;
static lv_obj_t *s_end_btn   = NULL;

static uint8_t *s_pip_buf = NULL;   /* RGB565 PIP_W*PIP_H*2 bytes in PSRAM */

static lv_timer_t *s_pip_timer = NULL;
static bool s_in_call = false;

static ui_video_pane_end_call_cb_t s_end_cb = NULL;

static void on_tap(lv_event_t *e)
{
    (void)e;
    if (s_in_call) return;       /* call mode: only End Call closes */
    ui_video_pane_hide();
}

static void on_end_btn(lv_event_t *e)
{
    (void)e;
    ui_video_pane_end_call_cb_t cb = s_end_cb;
    /* Hide first so the End Call cb can spawn another pane without
     * racing with our own teardown. */
    ui_video_pane_hide();
    if (cb) cb();
}

/* Naive RGB565 nearest-neighbour downscale src(sw x sh) -> dst(dw x dh).
 * dst stride is dw pixels.  Used only for the small PIP, so the per-
 * pixel cost is negligible — runs at 5 fps inside the LVGL timer. */
static void downscale_rgb565(const uint16_t *src, int sw, int sh,
                             uint16_t *dst, int dw, int dh)
{
    int x_step = (sw << 16) / dw;   /* 16.16 fixed */
    int y_step = (sh << 16) / dh;
    int sy = 0;
    for (int y = 0; y < dh; y++) {
        const uint16_t *srow = src + (sy >> 16) * sw;
        int sx = 0;
        for (int x = 0; x < dw; x++) {
            dst[y * dw + x] = srow[sx >> 16];
            sx += x_step;
        }
        sy += y_step;
    }
}

static void pip_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_pip_canvas || !s_pip_buf) return;
    if (!tab5_camera_initialized()) return;

    tab5_cam_frame_t frame;
    if (tab5_camera_capture(&frame) != ESP_OK) return;
    if (frame.format != TAB5_CAM_FMT_RGB565) return;

    /* Downscale (no rotation in the PIP — keep it simple, the user
     * already sees their feed.  cam_rot only matters for the remote
     * who'll see Tab5's outbound stream rotated by voice_video). */
    downscale_rgb565((const uint16_t *)frame.data, frame.width, frame.height,
                     (uint16_t *)s_pip_buf, PIP_W, PIP_H);
    lv_obj_invalidate(s_pip_canvas);
}

/* Internal: build the call-mode chrome (PIP + End Call) on top of the
 * already-created s_root.  Idempotent. */
static void build_call_chrome(void)
{
    if (s_in_call && s_pip_canvas && s_end_btn) return;
    s_in_call = true;

    /* PIP canvas */
    if (!s_pip_buf) {
        s_pip_buf = heap_caps_malloc((size_t)PIP_W * PIP_H * 2,
                                     MALLOC_CAP_SPIRAM);
    }
    if (s_pip_buf && !s_pip_canvas) {
        s_pip_canvas = lv_canvas_create(s_root);
        lv_canvas_set_buffer(s_pip_canvas, s_pip_buf, PIP_W, PIP_H,
                             LV_COLOR_FORMAT_RGB565);
        memset(s_pip_buf, 0, (size_t)PIP_W * PIP_H * 2);
        lv_obj_set_pos(s_pip_canvas, VP_W - PIP_W - PIP_PAD,
                                     VP_H - PIP_H - PIP_PAD);
        lv_obj_set_style_radius(s_pip_canvas, 12, 0);
        lv_obj_set_style_clip_corner(s_pip_canvas, true, 0);
        lv_obj_set_style_border_color(s_pip_canvas, lv_color_hex(0xF59E0B), 0);
        lv_obj_set_style_border_width(s_pip_canvas, 2, 0);
    }

    /* End Call pill */
    if (!s_end_btn) {
        s_end_btn = lv_button_create(s_root);
        lv_obj_remove_style_all(s_end_btn);
        lv_obj_set_size(s_end_btn, END_W, END_H);
        lv_obj_set_pos(s_end_btn, END_PAD, VP_H - END_H - END_PAD);
        lv_obj_set_style_radius(s_end_btn, END_H / 2, 0);
        lv_obj_set_style_bg_color(s_end_btn, lv_color_hex(0xEF4444), 0);
        lv_obj_set_style_bg_opa(s_end_btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(s_end_btn, on_end_btn, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(s_end_btn);
        lv_label_set_text(lbl, "End Call");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }

    if (!s_pip_timer) {
        s_pip_timer = lv_timer_create(pip_timer_cb, PIP_FPS_MS, NULL);
    }
}

void ui_video_pane_show(void)
{
    if (s_root) {
        lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_root);
        return;
    }

    lv_obj_t *parent = ui_home_get_screen();
    if (!parent) parent = lv_screen_active();
    if (!parent) return;

    s_root = lv_obj_create(parent);
    if (!s_root) {
        ESP_LOGE(TAG, "OOM creating root");
        return;
    }
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, VP_W, VP_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, on_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_root);

    s_image = lv_image_create(s_root);
    lv_obj_center(s_image);
}

void ui_video_pane_show_call(void)
{
    if (!s_root) ui_video_pane_show();
    if (!s_root) return;
    build_call_chrome();
}

void ui_video_pane_hide(void)
{
    if (s_pip_timer) {
        lv_timer_delete(s_pip_timer);
        s_pip_timer = NULL;
    }
    if (s_root) {
        lv_obj_delete(s_root);
        s_root = NULL;
        s_image = NULL;
        s_pip_canvas = NULL;
        s_end_btn = NULL;
    }
    if (s_pip_buf) {
        heap_caps_free(s_pip_buf);
        s_pip_buf = NULL;
    }
    s_in_call = false;
}

bool ui_video_pane_is_visible(void)
{
    return s_root != NULL;
}

bool ui_video_pane_is_in_call(void)
{
    return s_in_call;
}

void ui_video_pane_set_dsc(const lv_image_dsc_t *dsc)
{
    if (!s_image || !dsc) return;
    lv_image_set_src(s_image, dsc);
    lv_obj_invalidate(s_image);
}

void ui_video_pane_set_end_call_cb(ui_video_pane_end_call_cb_t cb)
{
    s_end_cb = cb;
}
