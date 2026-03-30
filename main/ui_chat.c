#include "ui_chat.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "ui_chat";
static lv_obj_t *s_overlay = NULL;
static bool s_active = false;

static void cb_close(lv_event_t *e) {
    (void)e;
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_active = false;
    ESP_LOGI(TAG, "Chat closed");
}

lv_obj_t *ui_chat_create(void) {
    if (s_active) return s_overlay;
    ESP_LOGI(TAG, "Opening chat overlay");

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 720, 1280);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(s_overlay);
    lv_label_set_text(t, "Tinker Chat");
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *b = lv_button_create(s_overlay);
    lv_obj_set_size(b, 100, 48);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0A84FF), 0);
    lv_obj_add_event_cb(b, cb_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    lv_obj_t *m = lv_label_create(s_overlay);
    lv_label_set_text(m, "Hi! I'm Tinker.\nAsk me anything.");
    lv_obj_set_style_text_color(m, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_20, 0);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, 0);

    s_active = true;
    ESP_LOGI(TAG, "Chat overlay open");
    return s_overlay;
}

void ui_chat_destroy(void) { cb_close(NULL); }
bool ui_chat_is_active(void) { return s_active; }
