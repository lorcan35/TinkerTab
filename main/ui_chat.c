#include "ui_chat.h"
#include "voice.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "ui_chat";
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_msg_list = NULL;
static lv_obj_t *s_textarea = NULL;
static lv_obj_t *s_assistant_bubble = NULL;  /* current streaming response bubble */
static lv_obj_t *s_assistant_label = NULL;
static lv_timer_t *s_poll_timer = NULL;
static bool s_active = false;
static int s_msg_count = 0;
static voice_state_t s_last_state = VOICE_STATE_IDLE;

void ui_chat_add_message(const char *text, bool is_user);

/* Poll voice state for streaming LLM updates */
static void poll_voice_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    voice_state_t st = voice_get_state();

    /* Update assistant bubble while LLM tokens stream in.
     * Check PROCESSING and SPEAKING — tokens continue arriving
     * during TTS synthesis. */
    if (st == VOICE_STATE_PROCESSING || st == VOICE_STATE_SPEAKING) {
        const char *llm = voice_get_llm_text();
        if (llm && llm[0]) {
            if (!s_assistant_bubble) {
                ui_chat_add_message("...", false);
                int count = lv_obj_get_child_count(s_msg_list);
                if (count > 0) {
                    s_assistant_bubble = lv_obj_get_child(s_msg_list, count - 1);
                    s_assistant_label = lv_obj_get_child(s_assistant_bubble, 0);
                }
            }
            if (s_assistant_label) {
                lv_label_set_text(s_assistant_label, llm);
                lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
            }
        }
    }

    /* Transition back to READY → finalize the bubble */
    if (s_last_state == VOICE_STATE_PROCESSING || s_last_state == VOICE_STATE_SPEAKING) {
        if (st == VOICE_STATE_READY || st == VOICE_STATE_IDLE) {
            /* Final update with complete LLM text */
            const char *llm = voice_get_llm_text();
            if (llm && llm[0] && s_assistant_label) {
                lv_label_set_text(s_assistant_label, llm);
            }
            s_assistant_bubble = NULL;
            s_assistant_label = NULL;
        }
    }

    s_last_state = st;
}

static void cb_close(lv_event_t *e) {
    (void)e;
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_msg_list = NULL;
    s_textarea = NULL;
    s_assistant_bubble = NULL;
    s_assistant_label = NULL;
    s_active = false;
    s_msg_count = 0;
    ESP_LOGI(TAG, "Chat closed");
}

static void cb_send(lv_event_t *e) {
    (void)e;
    if (!s_textarea) return;
    const char *txt = lv_textarea_get_text(s_textarea);
    if (!txt || !txt[0]) return;

    /* Show user bubble immediately */
    ui_chat_add_message(txt, true);

    /* Send to Dragon via voice text path */
    esp_err_t ret = voice_send_text(txt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "voice_send_text failed: %s", esp_err_to_name(ret));
        ui_chat_add_message("(Not connected to Dragon)", false);
    }

    lv_textarea_set_text(s_textarea, "");
}

lv_obj_t *ui_chat_create(void) {
    if (s_active) return s_overlay;
    ESP_LOGI(TAG, "Creating chat screen");
    esp_task_wdt_reset();

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 720, 1280);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t *btn = lv_button_create(s_overlay);
    lv_obj_set_size(btn, 80, 44);
    lv_obj_set_pos(btn, 10, 10);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, cb_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* Title */
    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, "Chat with Tinker");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    /* Message scroll area */
    s_msg_list = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_msg_list);
    lv_obj_set_size(s_msg_list, 700, 1280 - 66 - 72);
    lv_obj_set_pos(s_msg_list, 10, 62);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_msg_list, 4, 0);
    lv_obj_set_style_pad_gap(s_msg_list, 10, 0);
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF);

    /* Input bar */
    lv_obj_t *bar = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, 72);
    lv_obj_set_pos(bar, 0, 1280 - 72);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Text input */
    s_textarea = lv_textarea_create(bar);
    lv_obj_set_size(s_textarea, 540, 52);
    lv_obj_set_pos(s_textarea, 12, 10);
    lv_textarea_set_placeholder_text(s_textarea, "Type a message...");
    lv_textarea_set_one_line(s_textarea, true);
    lv_obj_set_style_bg_color(s_textarea, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_text_color(s_textarea, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_textarea, 0, 0);
    lv_obj_set_style_radius(s_textarea, 24, 0);
    lv_obj_set_style_pad_left(s_textarea, 16, 0);

    /* Send button */
    lv_obj_t *send = lv_button_create(bar);
    lv_obj_set_size(send, 100, 52);
    lv_obj_set_pos(send, 564, 10);
    lv_obj_set_style_bg_color(send, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_radius(send, 24, 0);
    lv_obj_add_event_cb(send, cb_send, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(send);
    lv_label_set_text(sl, "Send");
    lv_obj_set_style_text_color(sl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);

    s_active = true;
    s_last_state = voice_get_state();

    /* Welcome message */
    ui_chat_add_message("Hi! I'm Tinker. Type anything to chat.", false);

    /* Start polling for streaming LLM responses */
    s_poll_timer = lv_timer_create(poll_voice_cb, 200, NULL);

    ESP_LOGI(TAG, "Chat screen ready");
    return s_overlay;
}

void ui_chat_add_message(const char *text, bool is_user) {
    if (!s_msg_list || !text) return;
    if (s_msg_count >= 50) {
        lv_obj_t *first = lv_obj_get_child(s_msg_list, 0);
        if (first) lv_obj_del(first);
        s_msg_count--;
    }
    lv_obj_t *bubble = lv_obj_create(s_msg_list);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, 580);
    lv_obj_set_style_bg_color(bubble, is_user ? lv_color_hex(0xF5A623) : lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_all(bubble, 14, 0);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    if (is_user) {
        lv_obj_set_style_translate_x(bubble, 100, 0);  /* Right-align user bubbles */
    }

    lv_obj_t *lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 552);
    lv_obj_set_style_text_color(lbl, is_user ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF), 0);
    s_msg_count++;
    lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_ON);
}

void ui_chat_destroy(void) { cb_close(NULL); }
bool ui_chat_is_active(void) { return s_active; }
