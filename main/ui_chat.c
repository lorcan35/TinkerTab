/*
 * ui_chat.c — TinkerOS Chat Screen (minimal safe version)
 * 720x1280 portrait, LVGL v9
 *
 * Creates only the skeleton on open: top bar + empty scroll + input bar.
 * Message bubbles added one at a time via ui_chat_add_message().
 * No history fetch on create. No heavy initial render.
 */

#include "ui_chat.h"
#include "ui_home.h"
#include "ui_keyboard.h"
#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ui_chat";

/* ── Layout ─────────────────────────────────────────────────── */
#define SW          720
#define SH          1280
#define TOPBAR_H    56
#define INPUT_H     64
#define MSG_AREA_H  (SH - TOPBAR_H - INPUT_H)
#define MAX_MSGS    20

/* ── Palette ─────────────────────────────────────────────────── */
#define COL_BG     lv_color_hex(0x000000)
#define COL_TOPBAR lv_color_hex(0x1C1C1E)
#define COL_INPUT  lv_color_hex(0x1C1C1E)
#define COL_USER   lv_color_hex(0x0A84FF)
#define COL_AI     lv_color_hex(0x2C2C2E)
#define COL_WHITE  lv_color_hex(0xEBEBF5)
#define COL_GREY   lv_color_hex(0x8E8E93)
#define COL_CYAN   lv_color_hex(0x00B4D8)

/* ── State ───────────────────────────────────────────────────── */
static lv_obj_t *s_screen    = NULL;
static lv_obj_t *s_msg_list  = NULL;
static lv_obj_t *s_textarea  = NULL;
static lv_obj_t *s_ai_label  = NULL;
static bool      s_active    = false;
static int       s_msg_count = 0;

/* ── Callbacks ───────────────────────────────────────────────── */

static void cb_back(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Back to home");
    s_active = false;
    lv_screen_load(ui_home_get_screen());
}

static void cb_send(lv_event_t *e)
{
    (void)e;
    if (!s_textarea) return;
    const char *txt = lv_textarea_get_text(s_textarea);
    if (!txt || !txt[0]) return;

    ESP_LOGI(TAG, "Send: %s", txt);

    /* Show the user's message in the chat */
    ui_chat_add_message(txt, true);

    /* TODO: wire up voice_send_text() once Dragon text API is in voice.h */

    lv_textarea_set_text(s_textarea, "");
    ui_keyboard_hide();
}

static void cb_ta_focused(lv_event_t *e)
{
    (void)e;
    if (s_textarea) {
        ui_keyboard_show(s_textarea);
    }
}

/* ── Public API ──────────────────────────────────────────────── */

lv_obj_t *ui_chat_create(void)
{
    if (s_active) return s_screen;

    ESP_LOGI(TAG, "[1] Creating chat screen");
    esp_task_wdt_reset();

    /* Screen — black background, no scroll */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, SW, SH);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    ESP_LOGI(TAG, "[2] Top bar");
    esp_task_wdt_reset();

    /* Top bar */
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SW, TOPBAR_H);
    lv_obj_set_style_bg_color(bar, COL_TOPBAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_back = lv_button_create(bar);
    lv_obj_remove_style_all(btn_back);
    lv_obj_set_size(btn_back, 48, TOPBAR_H);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn_back, cb_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ico = lv_label_create(btn_back);
    lv_label_set_text(ico, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(ico, COL_CYAN, 0);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_20, 0);
    lv_obj_center(ico);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "Tinker");
    lv_obj_set_style_text_color(title, COL_WHITE, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    ESP_LOGI(TAG, "[3] Message list");
    esp_task_wdt_reset();

    /* Scrollable message area */
    s_msg_list = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_msg_list);
    lv_obj_set_size(s_msg_list, SW, MSG_AREA_H);
    lv_obj_align(s_msg_list, LV_ALIGN_TOP_LEFT, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_msg_list, 8, 0);
    lv_obj_set_style_pad_gap(s_msg_list, 8, 0);
    lv_obj_add_flag(s_msg_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF);

    ESP_LOGI(TAG, "[4] Input bar");
    esp_task_wdt_reset();

    /* Input bar */
    lv_obj_t *input_bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(input_bar);
    lv_obj_set_size(input_bar, SW, INPUT_H);
    lv_obj_align(input_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(input_bar, COL_INPUT, 0);
    lv_obj_set_style_bg_opa(input_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(input_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Textarea */
    s_textarea = lv_textarea_create(input_bar);
    lv_obj_set_size(s_textarea, SW - 120, INPUT_H - 16);
    lv_obj_align(s_textarea, LV_ALIGN_LEFT_MID, 8, 0);
    lv_textarea_set_placeholder_text(s_textarea, "Type a message...");
    lv_textarea_set_one_line(s_textarea, true);
    lv_obj_set_style_bg_color(s_textarea, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(s_textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_textarea, COL_WHITE, 0);
    lv_obj_set_style_border_width(s_textarea, 0, 0);
    lv_obj_set_style_radius(s_textarea, 20, 0);
    lv_obj_set_style_pad_hor(s_textarea, 16, 0);
    lv_obj_set_style_pad_ver(s_textarea, 8, 0);
    lv_obj_add_event_cb(s_textarea, cb_ta_focused, LV_EVENT_FOCUSED, NULL);

    /* Send button */
    lv_obj_t *btn_send = lv_button_create(input_bar);
    lv_obj_set_size(btn_send, 48, 48);
    lv_obj_align(btn_send, LV_ALIGN_RIGHT_MID, -56, 0);
    lv_obj_set_style_bg_color(btn_send, COL_CYAN, 0);
    lv_obj_set_style_bg_opa(btn_send, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_send, 24, 0);
    lv_obj_set_style_shadow_width(btn_send, 0, 0);
    lv_obj_add_event_cb(btn_send, cb_send, LV_EVENT_CLICKED, NULL);

    lv_obj_t *send_ico = lv_label_create(btn_send);
    lv_label_set_text(send_ico, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(send_ico, COL_WHITE, 0);
    lv_obj_center(send_ico);

    /* Mic button (placeholder — wires to voice when ready) */
    lv_obj_t *btn_mic = lv_button_create(input_bar);
    lv_obj_set_size(btn_mic, 48, 48);
    lv_obj_align(btn_mic, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(btn_mic, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(btn_mic, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_mic, 24, 0);
    lv_obj_set_style_shadow_width(btn_mic, 0, 0);

    lv_obj_t *mic_ico = lv_label_create(btn_mic);
    lv_label_set_text(mic_ico, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(mic_ico, COL_WHITE, 0);
    lv_obj_center(mic_ico);

    ESP_LOGI(TAG, "[5] Loading screen");
    esp_task_wdt_reset();

    s_active    = true;
    s_msg_count = 0;
    s_ai_label  = NULL;

    lv_screen_load(s_screen);

    ESP_LOGI(TAG, "[6] Chat screen ready");
    return s_screen;
}

void ui_chat_add_message(const char *text, bool is_user)
{
    if (!s_msg_list || !text || !s_active) return;

    /* Prune oldest bubble if at limit */
    if (s_msg_count >= MAX_MSGS) {
        lv_obj_t *first = lv_obj_get_child(s_msg_list, 0);
        if (first) {
            lv_obj_delete(first);
            s_msg_count--;
        }
    }

    /* Bubble row — full width so we can align bubble inside it */
    lv_obj_t *row = lv_obj_create(s_msg_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, SW - 16);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Bubble */
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_style_max_width(bubble, SW - 80, 0);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, is_user ? COL_USER : COL_AI, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (is_user) {
        lv_obj_align(bubble, LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        lv_obj_align(bubble, LV_ALIGN_LEFT_MID, 0, 0);
    }

    lv_obj_t *lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(lbl, SW - 104, 0);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(lbl, COL_WHITE, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);

    if (!is_user) {
        s_ai_label = lbl;
    }

    s_msg_count++;
    lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
}

void ui_chat_stream_token(const char *token)
{
    if (!s_ai_label || !token || !s_active) return;

    const char *cur = lv_label_get_text(s_ai_label);
    size_t cur_len  = strlen(cur);
    size_t tok_len  = strlen(token);

    char *buf = heap_caps_malloc(cur_len + tok_len + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;

    memcpy(buf, cur, cur_len);
    memcpy(buf + cur_len, token, tok_len);
    buf[cur_len + tok_len] = '\0';

    lv_label_set_text(s_ai_label, buf);
    heap_caps_free(buf);

    lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
}

void ui_chat_destroy(void)
{
    if (!s_active) return;
    ESP_LOGI(TAG, "Destroying chat screen");

    s_active    = false;
    s_msg_list  = NULL;
    s_textarea  = NULL;
    s_ai_label  = NULL;
    s_msg_count = 0;

    /* Screen is deleted when a new screen is loaded — do not delete while
     * active to avoid LVGL use-after-free from ongoing event processing. */
    s_screen = NULL;
}

bool ui_chat_is_active(void)
{
    return s_active;
}
