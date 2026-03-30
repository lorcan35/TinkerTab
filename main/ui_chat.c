/**
 * TinkerOS — Chat Screen (ui_chat.c)
 *
 * iMessage-style chat interface with:
 *   - Scrollable message list (user right/blue, Tinker left/grey)
 *   - Text input bar with send button
 *   - Hold-to-talk mic button for voice input
 *   - Streaming LLM response (bubble grows as tokens arrive)
 *   - Session history fetch from Dragon REST API on load
 *   - Auto-scroll to newest message
 */

#include "ui_chat.h"
#include "ui_home.h"
#include "ui_core.h"
#include "ui_keyboard.h"
#include "voice.h"
#include "config.h"
#include "settings.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_chat";

/* ── Palette ────────────────────────────────────────────────────────────── */
#define COL_BG          lv_color_hex(0x000000)
#define COL_TOPBAR      lv_color_hex(0x1C1C1E)
#define COL_USER_BG     lv_color_hex(0x0A84FF)
#define COL_AI_BG       lv_color_hex(0x2C2C2E)
#define COL_USER_TEXT   lv_color_hex(0xFFFFFF)
#define COL_AI_TEXT     lv_color_hex(0xEBEBF5)
#define COL_INPUT_BG    lv_color_hex(0x1C1C1E)
#define COL_SEP         lv_color_hex(0x38383A)
#define COL_CYAN        lv_color_hex(0x00B4D8)
#define COL_LABEL2      lv_color_hex(0x8E8E93)
#define COL_LABEL3      lv_color_hex(0x48484A)
#define COL_SEND_BG     lv_color_hex(0x0A84FF)
#define COL_MIC_BG      lv_color_hex(0x2C2C2E)
#define COL_MIC_ACTIVE  lv_color_hex(0xFF453A)

/* ── Layout ─────────────────────────────────────────────────────────────── */
#define SW              720
#define SH              1280
#define TOPBAR_H        56
#define INPUT_BAR_H     64
#define BUBBLE_MAX_W    540
#define BUBBLE_RAD      18
#define BUBBLE_PAD_H    12
#define BUBBLE_PAD_V    10
#define MSG_PAD_X       16
#define MSG_GAP_Y       6
#define BTN_SZ          44
#define TA_H            40

/* ── Streaming buffer ───────────────────────────────────────────────────── */
#define STREAM_BUF_INIT 256
#define STREAM_BUF_MAX  4096
#define MAX_MESSAGES     80    /* max bubbles on screen */
#define HISTORY_LIMIT    30    /* messages to fetch from REST */
#define HISTORY_BUF_SZ   8192 /* HTTP response buffer */

/* ── State ──────────────────────────────────────────────────────────────── */
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_msg_list = NULL;     /* scrollable message container */
static lv_obj_t *s_textarea = NULL;     /* text input field */
static lv_obj_t *s_btn_send = NULL;
static lv_obj_t *s_btn_mic = NULL;
static lv_obj_t *s_input_bar = NULL;
static lv_obj_t *s_connecting_lbl = NULL;

static bool s_active = false;
static bool s_mic_held = false;
static int  s_msg_count = 0;

/* Streaming AI bubble state */
static lv_obj_t *s_ai_bubble = NULL;    /* current AI bubble being streamed */
static lv_obj_t *s_ai_label = NULL;     /* label inside the streaming bubble */
static char     *s_stream_buf = NULL;   /* accumulated LLM text (PSRAM) */
static size_t    s_stream_len = 0;
static size_t    s_stream_cap = 0;

/* ── Forward declarations ───────────────────────────────────────────────── */
static void chat_event_cb(const char *event, const char *data);
static void cb_back(lv_event_t *e);
static void cb_send(lv_event_t *e);
static void cb_mic_pressed(lv_event_t *e);
static void cb_mic_released(lv_event_t *e);
static void cb_ta_focused(lv_event_t *e);
static void add_user_bubble(const char *text);
static void add_ai_bubble(const char *text);
static void begin_ai_stream(void);
static void append_ai_stream(const char *token);
static void finish_ai_stream(void);
static void scroll_to_bottom(void);
static void prune_messages(void);
static void history_fetch_task(void *arg);

/* ══════════════════════════════════════════════════════════════════════════
 *  Bubble helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static lv_obj_t *make_bubble(lv_obj_t *parent, bool is_user, const char *text)
{
    /* Outer row — aligns bubble left or right */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, SW - MSG_PAD_X * 2);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Bubble container */
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, BUBBLE_MAX_W, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bubble, is_user ? COL_USER_BG : COL_AI_BG, 0);
    lv_obj_set_style_radius(bubble, BUBBLE_RAD, 0);
    lv_obj_set_style_pad_hor(bubble, BUBBLE_PAD_H, 0);
    lv_obj_set_style_pad_ver(bubble, BUBBLE_PAD_V, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (is_user) {
        lv_obj_align(bubble, LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        lv_obj_align(bubble, LV_ALIGN_LEFT_MID, 0, 0);
    }

    /* Message text */
    lv_obj_t *lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, is_user ? COL_USER_TEXT : COL_AI_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lbl, BUBBLE_MAX_W - BUBBLE_PAD_H * 2, 0);

    s_msg_count++;
    return lbl;  /* return the label for streaming updates */
}

static void add_user_bubble(const char *text)
{
    if (!s_msg_list || !text || !text[0]) return;
    prune_messages();
    make_bubble(s_msg_list, true, text);
    scroll_to_bottom();
}

static void add_ai_bubble(const char *text)
{
    if (!s_msg_list || !text || !text[0]) return;
    prune_messages();
    make_bubble(s_msg_list, false, text);
    scroll_to_bottom();
}

static void begin_ai_stream(void)
{
    if (!s_msg_list) return;
    if (s_ai_bubble) return;  /* already streaming */

    prune_messages();

    /* Allocate stream buffer in PSRAM */
    s_stream_cap = STREAM_BUF_INIT;
    s_stream_buf = (char *)heap_caps_malloc(s_stream_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stream_buf) {
        ESP_LOGE(TAG, "Failed to allocate stream buffer");
        return;
    }
    s_stream_buf[0] = '\0';
    s_stream_len = 0;

    /* Create the bubble + label (empty initially) */
    lv_obj_t *row = lv_obj_create(s_msg_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, SW - MSG_PAD_X * 2);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_ai_bubble = lv_obj_create(row);
    lv_obj_remove_style_all(s_ai_bubble);
    lv_obj_set_width(s_ai_bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(s_ai_bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_ai_bubble, BUBBLE_MAX_W, 0);
    lv_obj_set_style_bg_opa(s_ai_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ai_bubble, COL_AI_BG, 0);
    lv_obj_set_style_radius(s_ai_bubble, BUBBLE_RAD, 0);
    lv_obj_set_style_pad_hor(s_ai_bubble, BUBBLE_PAD_H, 0);
    lv_obj_set_style_pad_ver(s_ai_bubble, BUBBLE_PAD_V, 0);
    lv_obj_clear_flag(s_ai_bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_ai_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    s_ai_label = lv_label_create(s_ai_bubble);
    lv_label_set_text(s_ai_label, "...");
    lv_obj_set_style_text_color(s_ai_label, COL_AI_TEXT, 0);
    lv_obj_set_style_text_font(s_ai_label, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(s_ai_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ai_label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_ai_label, BUBBLE_MAX_W - BUBBLE_PAD_H * 2, 0);

    s_msg_count++;
    scroll_to_bottom();
}

static void append_ai_stream(const char *token)
{
    if (!s_ai_label || !s_stream_buf || !token) return;

    size_t tlen = strlen(token);
    /* Grow buffer if needed */
    while (s_stream_len + tlen + 1 > s_stream_cap && s_stream_cap < STREAM_BUF_MAX) {
        size_t new_cap = s_stream_cap * 2;
        if (new_cap > STREAM_BUF_MAX) new_cap = STREAM_BUF_MAX;
        char *nb = (char *)heap_caps_realloc(s_stream_buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) break;
        s_stream_buf = nb;
        s_stream_cap = new_cap;
    }
    if (s_stream_len + tlen < s_stream_cap) {
        memcpy(s_stream_buf + s_stream_len, token, tlen);
        s_stream_len += tlen;
        s_stream_buf[s_stream_len] = '\0';
    }

    lv_label_set_text(s_ai_label, s_stream_buf);
    scroll_to_bottom();
}

static void finish_ai_stream(void)
{
    s_ai_bubble = NULL;
    s_ai_label = NULL;
    if (s_stream_buf) {
        heap_caps_free(s_stream_buf);
        s_stream_buf = NULL;
    }
    s_stream_len = 0;
    s_stream_cap = 0;
}

static void scroll_to_bottom(void)
{
    if (s_msg_list) {
        lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void prune_messages(void)
{
    /* Remove oldest messages if we exceed MAX_MESSAGES */
    while (s_msg_count >= MAX_MESSAGES && s_msg_list) {
        uint32_t cnt = lv_obj_get_child_count(s_msg_list);
        if (cnt == 0) break;
        lv_obj_t *oldest = lv_obj_get_child(s_msg_list, 0);
        if (oldest) {
            lv_obj_delete(oldest);
            s_msg_count--;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Chat event callback (from voice.c — called under tab5_ui_lock)
 * ══════════════════════════════════════════════════════════════════════════ */

static void chat_event_cb(const char *event, const char *data)
{
    if (!s_active) return;

    if (strcmp(event, "stt") == 0) {
        /* User's message — only add if not from voice_send_text (which adds it inline) */
        /* voice_send_text dispatches stt after sending, so we always add here */
        add_user_bubble(data);
        /* Start streaming container for upcoming AI response */
        begin_ai_stream();
    } else if (strcmp(event, "llm_token") == 0) {
        if (!s_ai_bubble) begin_ai_stream();
        append_ai_stream(data);
    } else if (strcmp(event, "llm_done") == 0) {
        finish_ai_stream();
    } else if (strcmp(event, "error") == 0) {
        finish_ai_stream();
        add_ai_bubble(data ? data : "Error");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Event handlers
 * ══════════════════════════════════════════════════════════════════════════ */

static void cb_back(lv_event_t *e)
{
    (void)e;
    ui_chat_destroy();
    lv_screen_load(ui_home_get_screen());
}

static void cb_send(lv_event_t *e)
{
    (void)e;
    if (!s_textarea) return;
    const char *text = lv_textarea_get_text(s_textarea);
    if (!text || !text[0]) return;

    /* Send text to Dragon — voice_send_text triggers chat_event_cb("stt") */
    esp_err_t err = voice_send_text(text);
    if (err == ESP_OK) {
        lv_textarea_set_text(s_textarea, "");
        ui_keyboard_hide();
    } else {
        ESP_LOGW(TAG, "Failed to send text: %s", esp_err_to_name(err));
        add_ai_bubble("Not connected to Dragon");
    }
}

static void cb_mic_pressed(lv_event_t *e)
{
    (void)e;
    s_mic_held = true;
    if (s_btn_mic) {
        lv_obj_set_style_bg_color(s_btn_mic, COL_MIC_ACTIVE, 0);
    }
    ui_keyboard_hide();
    voice_start_listening();
}

static void cb_mic_released(lv_event_t *e)
{
    (void)e;
    if (!s_mic_held) return;
    s_mic_held = false;
    if (s_btn_mic) {
        lv_obj_set_style_bg_color(s_btn_mic, COL_MIC_BG, 0);
    }
    voice_stop_listening();
}

static void cb_ta_focused(lv_event_t *e)
{
    (void)e;
    if (s_textarea) {
        ui_keyboard_show(s_textarea);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  History fetch (runs in a FreeRTOS task — HTTP GET to Dragon REST API)
 * ══════════════════════════════════════════════════════════════════════════ */

static void history_fetch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Fetching chat history...");

    char session_id[32] = {0};
    tab5_settings_get_session_id(session_id, sizeof(session_id));
    if (!session_id[0]) {
        ESP_LOGI(TAG, "No session ID — skipping history fetch");
        vTaskDelete(NULL);
        return;
    }

    char dragon_host[64] = {0};
    tab5_settings_get_dragon_host(dragon_host, sizeof(dragon_host));
    if (!dragon_host[0]) {
        ESP_LOGI(TAG, "No Dragon host — skipping history fetch");
        vTaskDelete(NULL);
        return;
    }

    /* Build URL: http://<dragon>:3502/api/v1/sessions/<sid>/messages?limit=30 */
    char url[256];
    snprintf(url, sizeof(url),
             "http://%s:%d/api/v1/sessions/%s/messages?limit=%d",
             dragon_host,
             TAB5_VOICE_PORT,
             session_id,
             HISTORY_LIMIT);

    char *buf = (char *)heap_caps_malloc(HISTORY_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "History fetch: no memory");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        heap_caps_free(buf);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "History fetch failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        heap_caps_free(buf);
        vTaskDelete(NULL);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int read_len = esp_http_client_read(client, buf,
                       (content_len > 0 && content_len < HISTORY_BUF_SZ - 1)
                       ? content_len : HISTORY_BUF_SZ - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        ESP_LOGW(TAG, "History fetch: empty response");
        heap_caps_free(buf);
        vTaskDelete(NULL);
        return;
    }
    buf[read_len] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    if (!root) {
        ESP_LOGW(TAG, "History fetch: JSON parse error");
        vTaskDelete(NULL);
        return;
    }

    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        vTaskDelete(NULL);
        return;
    }

    /* Add messages to chat under LVGL lock */
    tab5_ui_lock();
    if (s_active && s_msg_list) {
        int count = cJSON_GetArraySize(items);
        ESP_LOGI(TAG, "Loading %d history messages", count);
        for (int i = 0; i < count; i++) {
            cJSON *msg = cJSON_GetArrayItem(items, i);
            cJSON *role = cJSON_GetObjectItem(msg, "role");
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            if (!cJSON_IsString(role) || !cJSON_IsString(content)) continue;
            if (!content->valuestring[0]) continue;

            if (strcmp(role->valuestring, "user") == 0) {
                add_user_bubble(content->valuestring);
            } else if (strcmp(role->valuestring, "assistant") == 0) {
                add_ai_bubble(content->valuestring);
            }
        }
        scroll_to_bottom();

        /* Remove "connecting" label if present */
        if (s_connecting_lbl) {
            lv_obj_delete(s_connecting_lbl);
            s_connecting_lbl = NULL;
        }
    }
    tab5_ui_unlock();

    cJSON_Delete(root);
    ESP_LOGI(TAG, "History loaded");
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════════ */

lv_obj_t *ui_chat_create(void)
{
    if (s_active) {
        ESP_LOGW(TAG, "Chat screen already active");
        return s_screen;
    }

    ESP_LOGI(TAG, "Creating chat screen");

    /* ── Screen ─────────────────────────────────────────────────── */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, SW, SH);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    /* ── Top bar ────────────────────────────────────────────────── */
    {
        lv_obj_t *bar = lv_obj_create(s_screen);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, SW, TOPBAR_H);
        lv_obj_set_style_bg_color(bar, COL_TOPBAR, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(bar, 8, 0);

        /* Back button */
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 48, 48);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(btn, cb_back, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ico = lv_label_create(btn);
        lv_label_set_text(ico, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(ico, COL_CYAN, 0);
        lv_obj_set_style_text_font(ico, &lv_font_montserrat_20, 0);
        lv_obj_center(ico);

        /* Title */
        lv_obj_t *title = lv_label_create(bar);
        lv_label_set_text(title, "Tinker");
        lv_obj_set_style_text_color(title, lv_color_hex(0xEBEBF5), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_flex_grow(title, 1);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

        /* Spacer */
        lv_obj_t *sp = lv_obj_create(bar);
        lv_obj_remove_style_all(sp);
        lv_obj_set_size(sp, 48, 1);
    }

    /* ── Message list (scrollable) ──────────────────────────────── */
    {
        s_msg_list = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_msg_list);
        lv_obj_set_size(s_msg_list, SW, SH - TOPBAR_H - INPUT_BAR_H);
        lv_obj_align(s_msg_list, LV_ALIGN_TOP_LEFT, 0, TOPBAR_H);
        lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_msg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_hor(s_msg_list, MSG_PAD_X, 0);
        lv_obj_set_style_pad_ver(s_msg_list, 12, 0);
        lv_obj_set_style_pad_gap(s_msg_list, MSG_GAP_Y, 0);
        lv_obj_add_flag(s_msg_list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_AUTO);

        /* Initial "connecting" indicator */
        s_connecting_lbl = lv_label_create(s_msg_list);
        lv_label_set_text(s_connecting_lbl, "Loading conversation...");
        lv_obj_set_style_text_color(s_connecting_lbl, COL_LABEL2, 0);
        lv_obj_set_style_text_font(s_connecting_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_width(s_connecting_lbl, SW - MSG_PAD_X * 2);
        lv_obj_set_style_text_align(s_connecting_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* ── Separator line ─────────────────────────────────────────── */
    {
        lv_obj_t *sep = lv_obj_create(s_screen);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, SW, 1);
        lv_obj_set_style_bg_color(sep, COL_SEP, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_align(sep, LV_ALIGN_BOTTOM_LEFT, 0, -INPUT_BAR_H);
    }

    /* ── Input bar ──────────────────────────────────────────────── */
    {
        s_input_bar = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_input_bar);
        lv_obj_set_size(s_input_bar, SW, INPUT_BAR_H);
        lv_obj_set_style_bg_color(s_input_bar, COL_INPUT_BG, 0);
        lv_obj_set_style_bg_opa(s_input_bar, LV_OPA_COVER, 0);
        lv_obj_align(s_input_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_clear_flag(s_input_bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(s_input_bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_input_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(s_input_bar, 10, 0);
        lv_obj_set_style_pad_gap(s_input_bar, 8, 0);

        /* Mic button (hold-to-talk) */
        s_btn_mic = lv_button_create(s_input_bar);
        lv_obj_set_size(s_btn_mic, BTN_SZ, BTN_SZ);
        lv_obj_set_style_bg_color(s_btn_mic, COL_MIC_BG, 0);
        lv_obj_set_style_bg_opa(s_btn_mic, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_btn_mic, BTN_SZ / 2, 0);
        lv_obj_set_style_shadow_width(s_btn_mic, 0, 0);
        lv_obj_add_event_cb(s_btn_mic, cb_mic_pressed, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_btn_mic, cb_mic_released, LV_EVENT_RELEASED, NULL);
        lv_obj_t *mic_ico = lv_label_create(s_btn_mic);
        lv_label_set_text(mic_ico, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(mic_ico, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(mic_ico, &lv_font_montserrat_18, 0);
        lv_obj_center(mic_ico);

        /* Text input field */
        s_textarea = lv_textarea_create(s_input_bar);
        lv_obj_set_height(s_textarea, TA_H);
        lv_obj_set_flex_grow(s_textarea, 1);
        lv_textarea_set_placeholder_text(s_textarea, "Message Tinker...");
        lv_textarea_set_one_line(s_textarea, true);
        lv_obj_set_style_bg_color(s_textarea, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_opa(s_textarea, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_textarea, COL_SEP, 0);
        lv_obj_set_style_border_width(s_textarea, 1, 0);
        lv_obj_set_style_radius(s_textarea, TA_H / 2, 0);
        lv_obj_set_style_text_color(s_textarea, lv_color_hex(0xEBEBF5), 0);
        lv_obj_set_style_text_font(s_textarea, &lv_font_montserrat_16, 0);
        lv_obj_set_style_pad_hor(s_textarea, 16, 0);
        lv_obj_set_style_pad_ver(s_textarea, 8, 0);
        lv_obj_add_event_cb(s_textarea, cb_ta_focused, LV_EVENT_FOCUSED, NULL);

        /* Send button */
        s_btn_send = lv_button_create(s_input_bar);
        lv_obj_set_size(s_btn_send, BTN_SZ, BTN_SZ);
        lv_obj_set_style_bg_color(s_btn_send, COL_SEND_BG, 0);
        lv_obj_set_style_bg_opa(s_btn_send, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_btn_send, BTN_SZ / 2, 0);
        lv_obj_set_style_shadow_width(s_btn_send, 0, 0);
        lv_obj_add_event_cb(s_btn_send, cb_send, LV_EVENT_CLICKED, NULL);
        lv_obj_t *send_ico = lv_label_create(s_btn_send);
        lv_label_set_text(send_ico, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(send_ico, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(send_ico, &lv_font_montserrat_18, 0);
        lv_obj_center(send_ico);
    }

    /* ── Register chat callback ─────────────────────────────────── */
    voice_set_chat_cb(chat_event_cb);

    /* ── Ensure voice is connected ──────────────────────────────── */
    if (voice_get_state() == VOICE_STATE_IDLE) {
        char host[64] = {0};
        tab5_settings_get_dragon_host(host, sizeof(host));
        if (host[0]) {
            voice_connect_async(host, TAB5_VOICE_PORT, false);
        }
    }

    /* ── Fetch history from Dragon REST API ─────────────────────── */
    xTaskCreatePinnedToCore(history_fetch_task, "chat_hist", 4096,
                            NULL, 3, NULL, 1);

    /* ── Load screen ────────────────────────────────────────────── */
    s_active = true;
    lv_screen_load(s_screen);
    ESP_LOGI(TAG, "Chat screen created");
    return s_screen;
}

void ui_chat_destroy(void)
{
    if (!s_active) return;
    ESP_LOGI(TAG, "Destroying chat screen");

    s_active = false;
    voice_set_chat_cb(NULL);
    ui_keyboard_hide();
    finish_ai_stream();

    s_screen = NULL;
    s_msg_list = NULL;
    s_textarea = NULL;
    s_btn_send = NULL;
    s_btn_mic = NULL;
    s_input_bar = NULL;
    s_connecting_lbl = NULL;
    s_msg_count = 0;
    s_mic_held = false;
}

bool ui_chat_is_active(void)
{
    return s_active;
}
