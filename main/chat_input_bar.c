/**
 * Chat Input Bar — reusable composite widget.
 * Flex row: [mic button] [textarea (grow)] [send button]
 * Used by chat, voice overlay, and notes screens.
 */

#include "chat_input_bar.h"
#include "config.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "input_bar";

chat_input_bar_t *chat_input_bar_create(lv_obj_t *parent, uint32_t accent_color)
{
    chat_input_bar_t *bar = calloc(1, sizeof(chat_input_bar_t));
    if (!bar) { ESP_LOGE(TAG, "OOM"); return NULL; }

    /* Container: flex row, full width */
    bar->container = lv_obj_create(parent);
    if (!bar->container) { free(bar); return NULL; }
    lv_obj_remove_style_all(bar->container);
    lv_obj_set_size(bar->container, lv_pct(100), DPI_SCALE(56));
    lv_obj_set_style_bg_color(bar->container, lv_color_hex(0x0F0F1A), 0);
    lv_obj_set_style_bg_opa(bar->container, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar->container, DPI_SCALE(8), 0);
    lv_obj_set_style_pad_ver(bar->container, DPI_SCALE(6), 0);
    lv_obj_set_style_pad_gap(bar->container, DPI_SCALE(8), 0);
    lv_obj_set_flex_flow(bar->container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar->container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(bar->container, 1, 0);
    lv_obj_set_style_border_color(bar->container, lv_color_hex(0x222233), 0);
    lv_obj_set_style_border_side(bar->container, LV_BORDER_SIDE_TOP, 0);

    /* Mic button */
    bar->mic_btn = lv_button_create(bar->container);
    lv_obj_set_size(bar->mic_btn, TOUCH_MIN, TOUCH_MIN);
    lv_obj_set_style_radius(bar->mic_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar->mic_btn, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_opa(bar->mic_btn, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(bar->mic_btn, 0, 0);
    lv_obj_set_style_border_width(bar->mic_btn, 0, 0);
    lv_obj_t *mic_icon = lv_label_create(bar->mic_btn);
    lv_label_set_text(mic_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(mic_icon, FONT_BODY, 0);
    lv_obj_set_style_text_color(mic_icon, lv_color_hex(accent_color), 0);
    lv_obj_center(mic_icon);

    /* Textarea — flex-grow fills remaining space */
    bar->textarea = lv_textarea_create(bar->container);
    lv_obj_set_flex_grow(bar->textarea, 1);
    lv_obj_set_height(bar->textarea, DPI_SCALE(40));
    lv_textarea_set_placeholder_text(bar->textarea, "Type a message...");
    lv_textarea_set_one_line(bar->textarea, true);
    lv_obj_set_style_bg_color(bar->textarea, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(bar->textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar->textarea, 1, 0);
    lv_obj_set_style_border_color(bar->textarea, lv_color_hex(0x333344), 0);
    lv_obj_set_style_radius(bar->textarea, DPI_SCALE(20), 0);
    lv_obj_set_style_text_font(bar->textarea, FONT_BODY, 0);
    lv_obj_set_style_text_color(bar->textarea, lv_color_hex(0xE0E0E8), 0);
    lv_obj_set_style_pad_hor(bar->textarea, DPI_SCALE(12), 0);

    /* Send button */
    bar->send_btn = lv_button_create(bar->container);
    lv_obj_set_size(bar->send_btn, DPI_SCALE(64), TOUCH_MIN);
    lv_obj_set_style_bg_color(bar->send_btn, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_bg_opa(bar->send_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar->send_btn, DPI_SCALE(20), 0);
    lv_obj_set_style_shadow_width(bar->send_btn, 0, 0);
    lv_obj_set_style_border_width(bar->send_btn, 0, 0);
    lv_obj_t *send_lbl = lv_label_create(bar->send_btn);
    lv_label_set_text(send_lbl, "Send");
    lv_obj_set_style_text_font(send_lbl, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(send_lbl);

    return bar;
}

void chat_input_bar_set_callbacks(chat_input_bar_t *bar,
                                  lv_event_cb_t on_send,
                                  lv_event_cb_t on_mic,
                                  lv_event_cb_t on_ta_click)
{
    if (!bar) return;
    if (on_send && bar->send_btn) lv_obj_add_event_cb(bar->send_btn, on_send, LV_EVENT_CLICKED, bar);
    if (on_mic && bar->mic_btn)   lv_obj_add_event_cb(bar->mic_btn, on_mic, LV_EVENT_CLICKED, bar);
    if (on_ta_click && bar->textarea) lv_obj_add_event_cb(bar->textarea, on_ta_click, LV_EVENT_CLICKED, bar);
    /* LV_EVENT_READY = Done key on keyboard → same as send */
    if (on_send && bar->textarea) lv_obj_add_event_cb(bar->textarea, on_send, LV_EVENT_READY, bar);
}

const char *chat_input_bar_get_text(chat_input_bar_t *bar)
{
    if (!bar || !bar->textarea) return "";
    return lv_textarea_get_text(bar->textarea);
}

void chat_input_bar_clear(chat_input_bar_t *bar)
{
    if (bar && bar->textarea) lv_textarea_set_text(bar->textarea, "");
}

void chat_input_bar_set_text(chat_input_bar_t *bar, const char *text)
{
    if (bar && bar->textarea) lv_textarea_set_text(bar->textarea, text ? text : "");
}

lv_obj_t *chat_input_bar_get_textarea(chat_input_bar_t *bar)
{
    return bar ? bar->textarea : NULL;
}
