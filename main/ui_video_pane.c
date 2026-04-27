/*
 * ui_video_pane.c — see header.
 *
 * Implementation: a single full-screen lv_image centred on a black
 * lv_obj parented to ui_home's screen.  Tap-to-dismiss closes the
 * pane.  No animations, no ornaments — keeps the LVGL pool footprint
 * tiny so the pane is cheap to open/close mid-call.
 */
#include "ui_video_pane.h"

#include "esp_log.h"
#include "ui_home.h"

static const char *TAG = "ui_video_pane";

#define VP_W   720
#define VP_H   1280

static lv_obj_t *s_root  = NULL;
static lv_obj_t *s_image = NULL;

static void on_tap(lv_event_t *e)
{
    (void)e;
    ui_video_pane_hide();
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
    /* Camera native is 1280x720 landscape on a 720x1280 portrait pane.
     * No rotation here — TJPGD-decoded RAW images don't compose well
     * with lv_image rotation in LVGL 9 (renders blank).  The user can
     * rotate via NVS cam_rot which already affects the encoded bytes
     * upstream when the local Tab5 is the sender (Phase 3A); for
     * downlink frames from a peer, just letterbox the landscape feed
     * and let the user tilt the device. */
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
}

void ui_video_pane_hide(void)
{
    if (!s_root) return;
    lv_obj_delete(s_root);
    s_root  = NULL;
    s_image = NULL;
}

bool ui_video_pane_is_visible(void)
{
    return s_root != NULL;
}

void ui_video_pane_set_dsc(const lv_image_dsc_t *dsc)
{
    if (!s_image || !dsc) return;
    lv_image_set_src(s_image, dsc);
    lv_obj_invalidate(s_image);
}
