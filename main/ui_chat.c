#include "ui_chat.h"
#include "voice.h"
#include "ui_keyboard.h"
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
    /* L5: Support swipe-right gesture as back action */
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;
    }
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
    /* F1: HIDE overlay instead of deleting — preserves message history.
     * Messages live in the LVGL tree. Reopening just unhides. */
    ui_keyboard_hide();  /* MUST dismiss keyboard before hiding overlay */
    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);  /* stop intercepting taps */
    }
    s_assistant_bubble = NULL;
    s_assistant_label = NULL;
    s_active = false;
    ESP_LOGI(TAG, "Chat hidden (messages preserved)");
}

static void cb_textarea_click(lv_event_t *e) {
    (void)e;
    if (s_textarea) ui_keyboard_show(s_textarea);
}

/* S5: Mic button — start voice recording for speech-to-text input */
static void cb_mic(lv_event_t *e) {
    (void)e;
    voice_state_t st = voice_get_state();
    if (st == VOICE_STATE_READY) {
        ESP_LOGI(TAG, "Chat mic: starting voice input");
        voice_start_listening();
    } else if (st == VOICE_STATE_IDLE) {
        ESP_LOGW(TAG, "Chat mic: not connected to Dragon");
        ui_chat_add_message("(Not connected — tap orb on Home to connect)", false);
    } else if (st == VOICE_STATE_LISTENING) {
        /* Already listening — stop and send */
        voice_stop_listening();
    } else {
        ESP_LOGI(TAG, "Chat mic: voice busy (state=%d)", st);
    }
}

static void cb_send(lv_event_t *e) {
    (void)e;
    if (!s_textarea) return;
    ui_keyboard_hide();
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

    /* F1: If overlay exists but is hidden, just unhide it (preserves messages) */
    if (s_overlay) {
        ESP_LOGI(TAG, "Restoring chat (%d messages)", s_msg_count);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);  /* re-enable tap capture */
        s_active = true;
        s_last_state = voice_get_state();
        s_poll_timer = lv_timer_create(poll_voice_cb, 200, NULL);
        return s_overlay;
    }

    ESP_LOGI(TAG, "Creating chat screen");
    esp_task_wdt_reset();

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 720, 1280);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* L5: Swipe-right to close chat */
    lv_obj_add_event_cb(s_overlay, cb_close, LV_EVENT_GESTURE, NULL);

    /* Top bar — 80px with back button + title */
    lv_obj_t *topbar = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(topbar);
    lv_obj_set_size(topbar, 720, 80);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_style_bg_color(topbar, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button — larger touch target */
    lv_obj_t *btn = lv_button_create(topbar);
    lv_obj_set_size(btn, 100, 56);
    lv_obj_set_pos(btn, 12, 12);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_add_event_cb(btn, cb_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
    lv_obj_center(bl);

    /* Title */
    lv_obj_t *title = lv_label_create(topbar);
    lv_label_set_text(title, "Chat with Tinker");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 30, 0);

    /* Message scroll area — between topbar (80px) and input bar (80px) */
    s_msg_list = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_msg_list);
    lv_obj_set_size(s_msg_list, 700, 1280 - 80 - 80);
    lv_obj_set_pos(s_msg_list, 10, 80);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_msg_list, 4, 0);
    lv_obj_set_style_pad_gap(s_msg_list, 10, 0);
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF);

    /* Input bar — 80px tall, proper spacing */
    lv_obj_t *bar = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, 80);
    lv_obj_set_pos(bar, 0, 1280 - 80);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* S5: Mic button — 60px circle, proper touch target */
    lv_obj_t *mic = lv_button_create(bar);
    lv_obj_set_size(mic, 60, 60);
    lv_obj_set_pos(mic, 10, 10);
    lv_obj_set_style_bg_color(mic, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_radius(mic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mic, 0, 0);
    lv_obj_add_event_cb(mic, cb_mic, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mic_lbl = lv_label_create(mic);
    lv_label_set_text(mic_lbl, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(mic_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(mic_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(mic_lbl);

    /* Text input — between mic and send */
    s_textarea = lv_textarea_create(bar);
    lv_obj_set_size(s_textarea, 440, 60);
    lv_obj_set_pos(s_textarea, 80, 10);
    lv_textarea_set_placeholder_text(s_textarea, "Type a message...");
    lv_textarea_set_one_line(s_textarea, true);
    lv_obj_set_style_bg_color(s_textarea, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_text_color(s_textarea, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_textarea, &lv_font_montserrat_20, 0);
    lv_obj_set_style_border_width(s_textarea, 0, 0);
    lv_obj_set_style_radius(s_textarea, 28, 0);
    lv_obj_set_style_pad_left(s_textarea, 20, 0);
    lv_obj_add_event_cb(s_textarea, cb_textarea_click, LV_EVENT_CLICKED, NULL);

    /* Send button — matches input height */
    lv_obj_t *send = lv_button_create(bar);
    lv_obj_set_size(send, 108, 60);
    lv_obj_set_pos(send, 530, 10);
    lv_obj_set_style_bg_color(send, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_radius(send, 28, 0);
    lv_obj_set_style_border_width(send, 0, 0);
    lv_obj_add_event_cb(send, cb_send, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(send);
    lv_label_set_text(sl, "Send");
    lv_obj_set_style_text_color(sl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_20, 0);
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
    lv_obj_set_width(bubble, 560);
    lv_obj_set_style_bg_color(bubble, is_user ? lv_color_hex(0xF5A623) : lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 20, 0);
    lv_obj_set_style_pad_all(bubble, 16, 0);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    if (is_user) {
        lv_obj_set_style_translate_x(bubble, 120, 0);  /* Right-align user bubbles */
    }

    lv_obj_t *lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 528);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);  /* readable size */
    lv_obj_set_style_text_color(lbl, is_user ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF), 0);
    s_msg_count++;
    lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_ON);
}

void ui_chat_destroy(void) { cb_close(NULL); }
bool ui_chat_is_active(void) { return s_active; }
