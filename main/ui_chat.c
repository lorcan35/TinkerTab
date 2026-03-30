#include "ui_chat.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "ui_chat";
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_msg_list = NULL;
static lv_obj_t *s_textarea = NULL;
static bool s_active = false;
static int s_msg_count = 0;

void ui_chat_add_message(const char *text, bool is_user);

static void cb_close(lv_event_t *e) {
    (void)e;
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_msg_list = NULL;
    s_textarea = NULL;
    s_active = false;
    s_msg_count = 0;
    ESP_LOGI(TAG, "Chat closed");
}

static void cb_send(lv_event_t *e) {
    (void)e;
    if (!s_textarea) return;
    const char *txt = lv_textarea_get_text(s_textarea);
    if (!txt || !txt[0]) return;
    ESP_LOGI(TAG, "Send: %s", txt);
    ui_chat_add_message(txt, true);
    lv_textarea_set_text(s_textarea, "");
}

lv_obj_t *ui_chat_create(void) {
    if (s_active) return s_overlay;
    ESP_LOGI(TAG, "[1] Creating overlay");
    esp_task_wdt_reset();

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 720, 1280);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    ESP_LOGI(TAG, "[2] Top bar");
    esp_task_wdt_reset();

    /* Back button */
    lv_obj_t *btn = lv_button_create(s_overlay);
    lv_obj_set_size(btn, 80, 40);
    lv_obj_set_pos(btn, 10, 10);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0A84FF), 0);
    lv_obj_add_event_cb(btn, cb_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* Title */
    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, "Tinker Chat");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    ESP_LOGI(TAG, "[3] Message area");
    esp_task_wdt_reset();

    /* Message scroll area */
    s_msg_list = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_msg_list);
    lv_obj_set_size(s_msg_list, 720, 1280 - 56 - 64);
    lv_obj_set_pos(s_msg_list, 0, 56);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_msg_list, 8, 0);
    lv_obj_set_style_pad_gap(s_msg_list, 8, 0);
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF);

    ESP_LOGI(TAG, "[4] Input bar");
    esp_task_wdt_reset();

    /* Input bar */
    lv_obj_t *bar = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, 64);
    lv_obj_set_pos(bar, 0, 1280 - 64);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Text input */
    s_textarea = lv_textarea_create(bar);
    lv_obj_set_size(s_textarea, 540, 48);
    lv_obj_set_pos(s_textarea, 8, 8);
    lv_textarea_set_placeholder_text(s_textarea, "Type a message...");
    lv_textarea_set_one_line(s_textarea, true);
    lv_obj_set_style_bg_color(s_textarea, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_text_color(s_textarea, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_textarea, 0, 0);
    lv_obj_set_style_radius(s_textarea, 20, 0);

    /* Send button */
    lv_obj_t *send = lv_button_create(bar);
    lv_obj_set_size(send, 80, 48);
    lv_obj_set_pos(send, 560, 8);
    lv_obj_set_style_bg_color(send, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_radius(send, 20, 0);
    lv_obj_add_event_cb(send, cb_send, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(send);
    lv_label_set_text(sl, "Send");
    lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(sl);

    ESP_LOGI(TAG, "[5] Done");
    s_active = true;

    ui_chat_add_message("Hi! I'm Tinker. Ask me anything.", false);

    ESP_LOGI(TAG, "[6] Chat ready");
    return s_overlay;
}

void ui_chat_add_message(const char *text, bool is_user) {
    if (!s_msg_list || !text) return;
    if (s_msg_count >= 30) {
        lv_obj_t *first = lv_obj_get_child(s_msg_list, 0);
        if (first) lv_obj_del(first);
        s_msg_count--;
    }
    lv_obj_t *bubble = lv_obj_create(s_msg_list);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, 640);
    lv_obj_set_style_bg_color(bubble, is_user ? lv_color_hex(0x0A84FF) : lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 616);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    s_msg_count++;
    lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_ON);
}

void ui_chat_destroy(void) { cb_close(NULL); }
bool ui_chat_is_active(void) { return s_active; }
