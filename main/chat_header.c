/**
 * Chat Header — reusable composite widget.
 * Flex row: [back] [title (grow)] [status dot] [status text] [+action] [mode badge]
 * Uses DPI_SCALE for portable sizing. TOUCH_MIN for button hit areas.
 */

#include "chat_header.h"
#include "config.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "chat_hdr";

chat_header_t *chat_header_create(lv_obj_t *parent, const char *title,
                                   uint32_t accent_color, bool show_action)
{
    chat_header_t *hdr = calloc(1, sizeof(chat_header_t));
    if (!hdr) { ESP_LOGE(TAG, "OOM"); return NULL; }

    /* Container: flex row, full width, fixed height */
    hdr->container = lv_obj_create(parent);
    if (!hdr->container) { free(hdr); return NULL; }
    lv_obj_remove_style_all(hdr->container);
    lv_obj_set_size(hdr->container, lv_pct(100), DPI_SCALE(48));
    lv_obj_set_style_bg_color(hdr->container, lv_color_hex(0x0F0F1A), 0);
    lv_obj_set_style_bg_opa(hdr->container, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(hdr->container, DPI_SCALE(8), 0);
    lv_obj_set_style_pad_ver(hdr->container, DPI_SCALE(4), 0);
    lv_obj_set_flex_flow(hdr->container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr->container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr->container, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    hdr->back_btn = lv_button_create(hdr->container);
    lv_obj_set_size(hdr->back_btn, TOUCH_MIN, TOUCH_MIN);
    lv_obj_set_style_bg_opa(hdr->back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(hdr->back_btn, 0, 0);
    lv_obj_set_style_border_width(hdr->back_btn, 0, 0);
    lv_obj_t *back_lbl = lv_label_create(hdr->back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, FONT_HEADING, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(back_lbl);

    /* Title — flex-grow fills remaining space */
    hdr->title = lv_label_create(hdr->container);
    lv_label_set_text(hdr->title, title);
    lv_obj_set_style_text_font(hdr->title, FONT_HEADING, 0);
    lv_obj_set_style_text_color(hdr->title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_flex_grow(hdr->title, 1);

    /* Status dot */
    hdr->status_dot = lv_obj_create(hdr->container);
    lv_obj_remove_style_all(hdr->status_dot);
    lv_obj_set_size(hdr->status_dot, DPI_SCALE(8), DPI_SCALE(8));
    lv_obj_set_style_radius(hdr->status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hdr->status_dot, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_bg_opa(hdr->status_dot, LV_OPA_COVER, 0);

    /* Status label */
    hdr->status_label = lv_label_create(hdr->container);
    lv_label_set_text(hdr->status_label, "Ready");
    lv_obj_set_style_text_font(hdr->status_label, FONT_SMALL, 0);
    lv_obj_set_style_text_color(hdr->status_label, lv_color_hex(0x22C55E), 0);

    /* Action button (+ for new chat) — optional */
    if (show_action) {
        hdr->action_btn = lv_button_create(hdr->container);
        lv_obj_set_size(hdr->action_btn, TOUCH_MIN, DPI_SCALE(32));
        lv_obj_set_style_bg_color(hdr->action_btn, lv_color_hex(0x222233), 0);
        lv_obj_set_style_bg_opa(hdr->action_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hdr->action_btn, DPI_SCALE(8), 0);
        lv_obj_set_style_shadow_width(hdr->action_btn, 0, 0);
        lv_obj_set_style_border_width(hdr->action_btn, 0, 0);
        lv_obj_t *plus_lbl = lv_label_create(hdr->action_btn);
        lv_label_set_text(plus_lbl, "+");
        lv_obj_set_style_text_font(plus_lbl, FONT_HEADING, 0);
        lv_obj_set_style_text_color(plus_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(plus_lbl);
    }

    /* Mode badge */
    hdr->mode_badge = lv_label_create(hdr->container);
    lv_label_set_text(hdr->mode_badge, "Local");
    lv_obj_set_style_text_font(hdr->mode_badge, FONT_SMALL, 0);
    lv_obj_set_style_text_color(hdr->mode_badge, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_color(hdr->mode_badge, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_opa(hdr->mode_badge, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(hdr->mode_badge, DPI_SCALE(8), 0);
    lv_obj_set_style_pad_ver(hdr->mode_badge, DPI_SCALE(2), 0);
    lv_obj_set_style_radius(hdr->mode_badge, DPI_SCALE(6), 0);

    return hdr;
}

void chat_header_set_status(chat_header_t *hdr, const char *text, bool connected)
{
    if (!hdr) return;
    if (hdr->status_label) lv_label_set_text(hdr->status_label, text);
    uint32_t col = connected ? 0x22C55E : 0xFF453A;
    if (hdr->status_dot) lv_obj_set_style_bg_color(hdr->status_dot, lv_color_hex(col), 0);
    if (hdr->status_label) lv_obj_set_style_text_color(hdr->status_label, lv_color_hex(col), 0);
}

void chat_header_set_mode(chat_header_t *hdr, const char *mode_name, uint32_t color)
{
    if (!hdr || !hdr->mode_badge) return;
    lv_label_set_text(hdr->mode_badge, mode_name);
    lv_obj_set_style_text_color(hdr->mode_badge, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(hdr->mode_badge, lv_color_hex(color), 0);
}

void chat_header_set_back_cb(chat_header_t *hdr, lv_event_cb_t cb, void *user_data)
{
    if (hdr && hdr->back_btn) lv_obj_add_event_cb(hdr->back_btn, cb, LV_EVENT_CLICKED, user_data);
}

void chat_header_set_action_cb(chat_header_t *hdr, lv_event_cb_t cb, void *user_data)
{
    if (hdr && hdr->action_btn) lv_obj_add_event_cb(hdr->action_btn, cb, LV_EVENT_CLICKED, user_data);
}
