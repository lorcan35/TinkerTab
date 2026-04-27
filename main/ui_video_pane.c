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
#include "esp_timer.h"

#include "ui_home.h"
#include "camera.h"
#include "settings.h"
#include "voice.h"          /* #280: mute toggle */
#include "voice_video.h"    /* #280: downlink frame stats for status */

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

/* #280: mute pill top-right + status badge top-center. */
#define MUTE_W      96
#define MUTE_H      48
#define MUTE_PAD    16
#define STATUS_FPS_MS  1000  /* 1 Hz status refresh */

static lv_obj_t *s_root      = NULL;
static lv_obj_t *s_image     = NULL;
static lv_obj_t *s_pip_canvas = NULL;
static lv_obj_t *s_end_btn   = NULL;
/* #278 incoming-call chrome */
static lv_obj_t *s_accept_btn  = NULL;
static lv_obj_t *s_decline_btn = NULL;
static lv_obj_t *s_incoming_lbl = NULL;
/* #280 in-call chrome */
static lv_obj_t *s_mute_btn   = NULL;
static lv_obj_t *s_mute_lbl   = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_timer_t *s_status_timer = NULL;
static uint32_t s_last_recv_count_seen = 0;
static int64_t  s_last_recv_change_us  = 0;

static uint8_t *s_pip_buf = NULL;   /* RGB565 PIP_W*PIP_H*2 bytes in PSRAM */

static lv_timer_t *s_pip_timer = NULL;
static bool s_in_call    = false;
static bool s_incoming   = false;     /* #278: ringing / awaiting accept */

static ui_video_pane_end_call_cb_t s_end_cb     = NULL;
static ui_video_pane_accept_cb_t   s_accept_cb  = NULL;
static ui_video_pane_decline_cb_t  s_decline_cb = NULL;

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

/* #278 incoming-call: tear down the incoming chrome and call the
 * caller's accept hook (which typically upgrades to full call). */
static void on_accept_btn(lv_event_t *e)
{
    (void)e;
    ui_video_pane_accept_cb_t cb = s_accept_cb;
    /* Don't hide — caller will upgrade us to call mode in place. */
    s_incoming = false;
    if (s_accept_btn)   { lv_obj_delete(s_accept_btn);   s_accept_btn   = NULL; }
    if (s_decline_btn)  { lv_obj_delete(s_decline_btn);  s_decline_btn  = NULL; }
    if (s_incoming_lbl) { lv_obj_delete(s_incoming_lbl); s_incoming_lbl = NULL; }
    if (cb) cb();
}

static void on_decline_btn(lv_event_t *e)
{
    (void)e;
    ui_video_pane_decline_cb_t cb = s_decline_cb;
    ui_video_pane_hide();
    if (cb) cb();
}

/* #280 mute toggle.  Flips voice's call-mute flag and updates the
 * pill's label so the user gets immediate visual feedback. */
static void on_mute_btn(lv_event_t *e)
{
    (void)e;
    bool now = !voice_call_audio_is_muted();
    voice_call_audio_set_muted(now);
    if (s_mute_lbl) {
        lv_label_set_text(s_mute_lbl, now ? "Unmute" : "Mute");
    }
    if (s_mute_btn) {
        lv_obj_set_style_bg_color(s_mute_btn,
            now ? lv_color_hex(0xEF4444) : lv_color_hex(0x1F2937), 0);
    }
}

/* #280 status timer.  Picks one of CALLING / CONNECTED / PEER LEFT
 * based on whether downlink frames are arriving, then appends MUTED
 * if the local mic is suppressed.  Runs at 1 Hz. */
static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_status_lbl) return;

    voice_video_stats_t v;
    voice_video_get_stats(&v);

    if (v.frames_recv != s_last_recv_count_seen) {
        s_last_recv_count_seen = v.frames_recv;
        s_last_recv_change_us  = esp_timer_get_time();
    }

    int64_t now = esp_timer_get_time();
    bool peer_active = v.frames_recv > 0 &&
                       (now - s_last_recv_change_us) < (5LL * 1000 * 1000);

    const char *label;
    uint32_t    color;
    if (peer_active) {
        label = "CONNECTED";
        color = 0x22C55E;          /* green */
    } else if (v.frames_recv > 0) {
        label = "PEER LEFT";
        color = 0xF59E0B;          /* amber */
    } else {
        label = "CALLING...";   /* "CALLING…" */
        color = 0xF59E0B;
    }

    /* Append MUTED when the local mic is suppressed. */
    if (voice_call_audio_is_muted()) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s  \xE2\x80\xA2  MUTED", label);
        lv_label_set_text(s_status_lbl, buf);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xEF4444), 0);
    } else {
        lv_label_set_text(s_status_lbl, label);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(color), 0);
    }
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

    /* #280: Mute pill (top-right) — toggles voice_call_audio_set_muted. */
    if (!s_mute_btn) {
        s_mute_btn = lv_button_create(s_root);
        lv_obj_remove_style_all(s_mute_btn);
        lv_obj_set_size(s_mute_btn, MUTE_W, MUTE_H);
        lv_obj_set_pos(s_mute_btn, VP_W - MUTE_W - MUTE_PAD, MUTE_PAD);
        lv_obj_set_style_radius(s_mute_btn, MUTE_H / 2, 0);
        lv_obj_set_style_bg_color(s_mute_btn,
            voice_call_audio_is_muted() ? lv_color_hex(0xEF4444) : lv_color_hex(0x1F2937), 0);
        lv_obj_set_style_bg_opa(s_mute_btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(s_mute_btn, on_mute_btn, LV_EVENT_CLICKED, NULL);
        s_mute_lbl = lv_label_create(s_mute_btn);
        lv_label_set_text(s_mute_lbl, voice_call_audio_is_muted() ? "Unmute" : "Mute");
        lv_obj_set_style_text_color(s_mute_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(s_mute_lbl);
    }

    /* #280: status badge (top-center) — flips between CALLING…/CONNECTED/PEER LEFT
     * driven by a 1 Hz lv_timer that polls voice_video_get_stats. */
    if (!s_status_lbl) {
        s_status_lbl = lv_label_create(s_root);
        lv_label_set_text(s_status_lbl, "CALLING...");
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xF59E0B), 0);
        lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 32);
    }
    s_last_recv_count_seen = 0;
    s_last_recv_change_us  = esp_timer_get_time();
    if (!s_status_timer) {
        s_status_timer = lv_timer_create(status_timer_cb, STATUS_FPS_MS, NULL);
        status_timer_cb(NULL);   /* immediate first paint */
    }
}

void ui_video_pane_show(void)
{
    if (s_root) {
        lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_root);
        return;
    }

    /* #276: parent to lv_layer_top() instead of the home screen so the
     * pane stays on top when the user navigates to Settings / Chat /
     * Notes / etc.  Without this, the call keeps running while a new
     * overlay covers the pane — privacy bug + UX confusion (user
     * thinks they ended the call). */
    lv_obj_t *parent = lv_layer_top();
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
    /* Coming from incoming-call mode? Tear that chrome down — the
     * call-mode chrome (PIP + End Call) replaces it.  No-op if we
     * weren't in incoming mode. */
    if (s_incoming) {
        s_incoming = false;
        if (s_accept_btn)   { lv_obj_delete(s_accept_btn);   s_accept_btn   = NULL; }
        if (s_decline_btn)  { lv_obj_delete(s_decline_btn);  s_decline_btn  = NULL; }
        if (s_incoming_lbl) { lv_obj_delete(s_incoming_lbl); s_incoming_lbl = NULL; }
    }
    build_call_chrome();
}

/* #278: build the incoming-call chrome (Accept + Decline buttons +
 * "Incoming call" badge).  Idempotent. */
static void build_incoming_chrome(void)
{
    if (s_incoming && s_accept_btn && s_decline_btn) return;
    s_incoming = true;

    /* "Incoming call" badge top-center */
    if (!s_incoming_lbl) {
        s_incoming_lbl = lv_label_create(s_root);
        lv_label_set_text(s_incoming_lbl, "Incoming call");
        lv_obj_set_style_text_color(s_incoming_lbl, lv_color_hex(0xF59E0B), 0);
        lv_obj_align(s_incoming_lbl, LV_ALIGN_TOP_MID, 0, 32);
    }

    /* Accept (green) bottom-right where End Call would be in call mode. */
    if (!s_accept_btn) {
        s_accept_btn = lv_button_create(s_root);
        lv_obj_remove_style_all(s_accept_btn);
        lv_obj_set_size(s_accept_btn, END_W, END_H);
        lv_obj_set_pos(s_accept_btn,
                       VP_W - END_W - END_PAD,
                       VP_H - END_H - END_PAD);
        lv_obj_set_style_radius(s_accept_btn, END_H / 2, 0);
        lv_obj_set_style_bg_color(s_accept_btn, lv_color_hex(0x22C55E), 0);
        lv_obj_set_style_bg_opa(s_accept_btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(s_accept_btn, on_accept_btn, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(s_accept_btn);
        lv_label_set_text(lbl, "Accept");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }

    /* Decline (red) bottom-left */
    if (!s_decline_btn) {
        s_decline_btn = lv_button_create(s_root);
        lv_obj_remove_style_all(s_decline_btn);
        lv_obj_set_size(s_decline_btn, END_W, END_H);
        lv_obj_set_pos(s_decline_btn, END_PAD, VP_H - END_H - END_PAD);
        lv_obj_set_style_radius(s_decline_btn, END_H / 2, 0);
        lv_obj_set_style_bg_color(s_decline_btn, lv_color_hex(0xEF4444), 0);
        lv_obj_set_style_bg_opa(s_decline_btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(s_decline_btn, on_decline_btn, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(s_decline_btn);
        lv_label_set_text(lbl, "Decline");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }
}

void ui_video_pane_show_incoming(void)
{
    if (!s_root) ui_video_pane_show();
    if (!s_root) return;
    /* If we're already in active-call mode, don't downgrade — the
     * peer who triggered this is just streaming to an established
     * call. */
    if (s_in_call) return;
    build_incoming_chrome();
}

bool ui_video_pane_is_incoming(void)
{
    return s_incoming;
}

void ui_video_pane_hide(void)
{
    if (s_pip_timer) {
        lv_timer_delete(s_pip_timer);
        s_pip_timer = NULL;
    }
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = NULL;
    }
    if (s_root) {
        /* lv_obj_delete recurses to children — no need to delete each
         * child first.  Just NULL the static handles so we don't keep
         * stale pointers. */
        lv_obj_delete(s_root);
        s_root          = NULL;
        s_image         = NULL;
        s_pip_canvas    = NULL;
        s_end_btn       = NULL;
        s_accept_btn    = NULL;
        s_decline_btn   = NULL;
        s_incoming_lbl  = NULL;
        s_mute_btn      = NULL;
        s_mute_lbl      = NULL;
        s_status_lbl    = NULL;
    }
    if (s_pip_buf) {
        heap_caps_free(s_pip_buf);
        s_pip_buf = NULL;
    }
    s_in_call  = false;
    s_incoming = false;
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

void ui_video_pane_set_accept_cb(ui_video_pane_accept_cb_t cb)
{
    s_accept_cb = cb;
}

void ui_video_pane_set_decline_cb(ui_video_pane_decline_cb_t cb)
{
    s_decline_cb = cb;
}
