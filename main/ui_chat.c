/**
 * TinkerTab — Chat Screen Orchestrator
 *
 * Thin orchestrator (~400 lines) delegating to 5 modules:
 *   - chat_header.h    — composite header widget
 *   - chat_input_bar.h — composite input bar widget
 *   - chat_msg_store.h — per-mode ring buffers
 *   - chat_msg_view.h  — recycled object pool + virtual scroll
 *   - chat_suggestions.h — mode-specific suggestion cards
 *
 * No Chat Home panel — goes straight to conversation.
 * Fullscreen overlay on home screen, show/hide pattern.
 */
#include "ui_chat.h"
#include "ui_feedback.h"
#include "voice.h"
#include "config.h"
#include "ui_keyboard.h"
#include "ui_sessions.h"
#include "settings.h"
#include "tab5_rtc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>
#include "media_cache.h"
#include "chat_msg_store.h"
#include "chat_msg_view.h"
#include "chat_header.h"
#include "chat_input_bar.h"
#include "chat_suggestions.h"

extern lv_obj_t *ui_home_get_screen(void);
extern void ui_home_show_toast(const char *text);

static const char *TAG = "ui_chat";

/* Yield to WDT during heavy UI creation */
static inline void feed_wdt_yield(void) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* Safe string copy — avoids -Wstringop-truncation and -Wformat-truncation */
static void safe_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t slen = strlen(src);
    if (slen >= dst_sz) slen = dst_sz - 1;
    memcpy(dst, src, slen);
    dst[slen] = '\0';
}

/* ── State ─────────────────────────────────────────────────────── */
static lv_obj_t       *s_overlay    = NULL;
static bool            s_active     = false;
static chat_header_t  *s_header     = NULL;
static chat_input_bar_t *s_input_bar = NULL;
static lv_timer_t     *s_poll_timer = NULL;
static bool            s_clear_guard = false;
static lv_timer_t     *s_clear_timer = NULL;
static voice_state_t   s_last_state = VOICE_STATE_IDLE;
static int32_t         s_touch_start_x = -1;
static bool            s_store_inited  = false;
static bool            s_streaming_active = false;  /* true while poll streams AI tokens */

/* Image download serialization */
static SemaphoreHandle_t s_img_dl_sem = NULL;
static volatile uint32_t s_img_load_gen = 0;

#define SWIPE_EDGE_PX  60
#define CLR_BG         0x08080E   /* TH_BG — v5 background */

/* ── Mode arrays — v5 th_mode_colors palette ──────────────────── */
static const char *s_mode_names[]  = {"Local", "Hybrid", "Cloud", "TinkerClaw"};
static const uint32_t s_mode_colors[] = {0x22C55E, 0xF59E0B, 0x3B82F6, 0xF43F5E};

static const char *s_local_models[]  = {"qwen3:1.7b", "qwen3:0.6b", "qwen3:4b"};
static const char *s_cloud_models[]  = {
    "anthropic/claude-3.5-haiku", "anthropic/claude-sonnet-4-20250514",
    "openai/gpt-4o-mini", "openai/gpt-4o",
};
static const char *s_tinkerclaw_models[] = {
    "ollama/qwen3:1.7b", "ollama/qwen3:4b",
    "anthropic/claude-3.5-haiku", "anthropic/claude-sonnet-4-20250514",
    "openai/gpt-4o-mini",
};
#define N_LOCAL_MODELS      (sizeof(s_local_models) / sizeof(s_local_models[0]))
#define N_CLOUD_MODELS      (sizeof(s_cloud_models) / sizeof(s_cloud_models[0]))
#define N_TINKERCLAW_MODELS (sizeof(s_tinkerclaw_models) / sizeof(s_tinkerclaw_models[0]))

/* ── Rich media async push types ─────────────────────────────── */
typedef struct { char url[256]; char alt[128]; int width; int height; } media_push_t;
typedef struct { char title[128]; char subtitle[256]; char image_url[256]; char description[256]; } card_push_t;
typedef struct { char url[256]; float duration_s; char label[128]; } audio_push_t;
typedef struct { char *role; char *text; } chat_push_msg_t;
typedef struct { char *text; } chat_update_msg_t;

/* ── Helpers (copied verbatim from previous version) ──────────── */

static bool __attribute__((unused)) get_time_str(char *buf, size_t buf_sz)
{
    tab5_rtc_time_t now;
    if (tab5_rtc_get_time(&now) == ESP_OK) {
        snprintf(buf, buf_sz, "%02d:%02d", now.hour, now.minute);
        return true;
    }
    return false;
}

static void strip_tool_tags(const char *src, char *out, size_t out_sz)
{
    if (!src || !out || out_sz == 0) return;
    size_t j = 0;
    const char *p = src;
    while (*p && j < out_sz - 1) {
        if (*p == '<') {
            if (strncmp(p, "<tool", 5) == 0) break;
        }
        out[j++] = *p++;
    }
    out[j] = '\0';
    while (j > 0 && (out[j-1] == ' ' || out[j-1] == '\n' || out[j-1] == '\r'))
        out[--j] = '\0';
}

static const char *detect_tool_name(const char *text)
{
    if (!text) return NULL;
    const char *t = strstr(text, "<tool");
    if (!t) return NULL;
    const char *n = strstr(t, "name=\"");
    if (n) { n += 6; return n; }
    return "a tool";
}

static const char *friendly_tool_name(const char *raw_name)
{
    if (!raw_name) return "Using a tool";
    static const struct { const char *raw; size_t len; const char *friendly; } map[] = {
        { "web_search",    10, "Searching the web" },
        { "remember",       8, "Saving to memory" },
        { "memory_store",  12, "Saving to memory" },
        { "recall",         6, "Checking memory" },
        { "memory_search", 13, "Checking memory" },
        { "datetime",       8, "Checking the time" },
        { "calculator",    10, "Calculating" },
        { "math",           4, "Calculating" },
        { "browser",        7, "Browsing a page" },
        { "browse",         6, "Browsing a page" },
        { "weather",        7, "Checking the weather" },
        { "timer",          5, "Setting a timer" },
        { "alarm",          5, "Setting an alarm" },
        { "notes",          5, "Checking notes" },
        { "file_read",      9, "Reading a file" },
        { "file_write",    10, "Writing a file" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strncmp(raw_name, map[i].raw, map[i].len) == 0 &&
            (raw_name[map[i].len] == '"' || raw_name[map[i].len] == '\0' ||
             raw_name[map[i].len] == ' '))
            return map[i].friendly;
    }
    return "Using a tool";
}

static void validate_model_for_mode(void)
{
    uint8_t mode = tab5_settings_get_voice_mode();
    char cur[64];
    tab5_settings_get_llm_model(cur, sizeof(cur));
    const char **models; int n;
    if (mode == VOICE_MODE_TINKERCLAW) { models = s_tinkerclaw_models; n = N_TINKERCLAW_MODELS; }
    else if (mode == VOICE_MODE_CLOUD) { models = s_cloud_models; n = N_CLOUD_MODELS; }
    else { models = s_local_models; n = N_LOCAL_MODELS; }
    bool found = false;
    for (int i = 0; i < n; i++) { if (strcmp(cur, models[i]) == 0) { found = true; break; } }
    if (!found) {
        ESP_LOGW(TAG, "NVS model '%s' invalid for mode %d — reset to '%s'", cur, mode, models[0]);
        tab5_settings_set_llm_model(models[0]);
        voice_send_config_update(mode, models[0]);
    }
}

static const char __attribute__((unused)) *friendly_model_name(const char *model_id)
{
    if (!model_id || !model_id[0]) return "Local AI";
    static const struct { const char *id; const char *friendly; } map[] = {
        { "qwen3:1.7b",                        "Local AI" },
        { "qwen3:4b",                          "Local AI" },
        { "anthropic/claude-3.5-haiku",         "Claude Haiku" },
        { "anthropic/claude-sonnet-4-20250514", "Claude Sonnet" },
        { "openai/gpt-4o-mini",                "GPT-4o mini" },
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        if (strcmp(model_id, map[i].id) == 0) return map[i].friendly;
    }
    if (strstr(model_id, "qwen")) return "Local AI";
    const char *slash = strrchr(model_id, '/');
    return slash ? slash + 1 : model_id;
}

/* ── Mode/status updates via header module ────────────────────── */

static void update_header_mode(void)
{
    if (!s_header) return;
    uint8_t mode = tab5_settings_get_voice_mode();
    if (mode >= VOICE_MODE_COUNT) mode = 0;
    chat_header_set_mode(s_header, s_mode_names[mode], s_mode_colors[mode]);
}

static void update_header_status(void)
{
    if (!s_header) return;
    voice_state_t st = voice_get_state();
    bool connected = voice_is_connected();
    const char *text = "Offline";
    switch (st) {
        case VOICE_STATE_IDLE:       text = connected ? "Ready" : "Offline"; break;
        case VOICE_STATE_CONNECTING: text = "Connecting..."; break;
        case VOICE_STATE_READY:      text = "Ready"; break;
        case VOICE_STATE_LISTENING:  text = "Listening..."; break;
        case VOICE_STATE_PROCESSING: text = "Processing..."; break;
        case VOICE_STATE_SPEAKING:   text = "Speaking..."; break;
    }
    if (ui_keyboard_is_visible()) text = "Typing...";
    chat_header_set_status(s_header, text, connected);
}

/* Forward declarations */
static void cb_suggestion_tap(lv_event_t *e);

/* ── Callbacks ────────────────────────────────────────────────── */

static void cb_touch_down(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    s_touch_start_x = p.x;
}

static void cb_close(lv_event_t *e)
{
    if (e && lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;
        if (s_touch_start_x < 0 || s_touch_start_x > SWIPE_EDGE_PX) return;
    }
    ui_chat_hide();
}

static void clear_guard_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_clear_guard = false;
    s_clear_timer = NULL;
    ESP_LOGI(TAG, "New-chat guard lifted");
}

static void cb_new_chat(lv_event_t *e)
{
    (void)e;
    if (s_clear_guard) return;

    voice_clear_history();
    s_clear_guard = true;
    if (s_clear_timer) { lv_timer_delete(s_clear_timer); s_clear_timer = NULL; }
    s_clear_timer = lv_timer_create(clear_guard_timer_cb, 500, NULL);
    lv_timer_set_repeat_count(s_clear_timer, 1);

    uint8_t mode = chat_view_get_mode();
    chat_store_clear(mode);
    s_img_load_gen++;
    s_streaming_active = false;
    chat_view_refresh();

    /* Show suggestions again */
    lv_obj_t *scroll = chat_view_get_scroll();
    if (scroll) chat_suggestions_create(scroll, mode, cb_suggestion_tap);
    ESP_LOGI(TAG, "New chat — cleared mode %d, guard active", mode);
}

static void cb_open_sessions(lv_event_t *e)
{
    (void)e;
    extern void ui_sessions_show(void);
    ui_sessions_show();
}

static void cb_mode_cycle(lv_event_t *e)
{
    (void)e;
    uint8_t mode = tab5_settings_get_voice_mode();
    mode = (mode + 1) % VOICE_MODE_COUNT;
    tab5_settings_set_voice_mode(mode);
    char model_buf[64];
    tab5_settings_get_llm_model(model_buf, sizeof(model_buf));
    voice_send_config_update(mode, model_buf);
    update_header_mode();

    /* Switch view to new mode */
    chat_view_set_mode(mode);
    s_streaming_active = false;
    validate_model_for_mode();

    /* Show suggestions if empty */
    if (chat_store_count(mode) == 0) {
        lv_obj_t *scroll = chat_view_get_scroll();
        if (scroll) chat_suggestions_create(scroll, mode, cb_suggestion_tap);
    } else {
        chat_suggestions_hide();
    }
    ESP_LOGI(TAG, "Mode cycled to %d", mode);
}

static void cb_textarea_click(lv_event_t *e)
{
    (void)e;
    if (s_input_bar && s_input_bar->textarea)
        ui_keyboard_show(s_input_bar->textarea);
}

static void cb_mic(lv_event_t *e)
{
    (void)e;
    if (s_clear_guard) { ESP_LOGI(TAG, "Mic blocked — guard active"); return; }
    voice_state_t st = voice_get_state();
    if (st == VOICE_STATE_READY)         { voice_start_listening(); }
    else if (st == VOICE_STATE_IDLE)     { ui_home_show_toast("Not connected -- tap orb on Home to connect"); }
    else if (st == VOICE_STATE_LISTENING) { voice_stop_listening(); }
    else { ESP_LOGI(TAG, "Chat mic: voice busy (state=%d)", st); }
}

static void cb_send(lv_event_t *e)
{
    (void)e;
    if (!s_input_bar) return;
    if (s_clear_guard) { ESP_LOGI(TAG, "Send blocked — guard active"); return; }
    ui_keyboard_hide();
    const char *txt = chat_input_bar_get_text(s_input_bar);
    if (!txt || !txt[0]) return;

    ui_chat_add_message(txt, true);
    esp_err_t ret = voice_send_text(txt);
    if (ret != ESP_OK) {
        voice_state_t st = voice_get_state();
        if (st == VOICE_STATE_LISTENING || st == VOICE_STATE_PROCESSING || st == VOICE_STATE_SPEAKING)
            ui_home_show_toast("Voice is active -- wait for it to finish");
        else
            ui_home_show_toast("Not connected to Dragon");
    }
    chat_input_bar_clear(s_input_bar);
}

static void cb_suggestion_tap(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(card, 0);
    if (!label) return;
    const char *raw = lv_label_get_text(label);
    if (!raw || !raw[0]) return;

    /* Strip surrounding quotes */
    char text[256];
    size_t len = strlen(raw);
    if (len >= 2 && raw[0] == '"' && raw[len-1] == '"') {
        size_t copy_len = len - 2;
        if (copy_len >= sizeof(text)) copy_len = sizeof(text) - 1;
        memcpy(text, raw + 1, copy_len);
        text[copy_len] = '\0';
    } else {
        safe_copy(text, sizeof(text), raw);
    }

    ui_chat_add_message(text, true);
    voice_send_text(text);
}

/* ── Keyboard layout callback ─────────────────────────────────── */

static void chat_keyboard_layout_cb(bool visible, int kb_height)
{
    lv_obj_t *scroll = chat_view_get_scroll();
    if (!s_input_bar || !scroll) return;

    if (visible) {
        /* Shrink the scroll area and move input bar above keyboard.
         * With flex layout, we set max_height on the scroll to force it shorter.
         * Then the input bar naturally sits below it. */
        int avail = 1160 - kb_height;   /* USABLE_H = 1160 */
        int hdr_h = s_header ? DPI_SCALE(48) : 0;
        int bar_h = DPI_SCALE(56);
        int scroll_max = avail - hdr_h - bar_h;
        if (scroll_max < 200) scroll_max = 200;
        lv_obj_set_height(scroll, scroll_max);
        chat_view_scroll_to_bottom();
    } else {
        /* Restore: let flex-grow reclaim the space */
        lv_obj_set_height(scroll, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(scroll, 1);
        chat_view_scroll_to_bottom();
    }
}

/* ── Poll timer — streaming + status ──────────────────────────── */

static void poll_voice_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    voice_state_t st = voice_get_state();
    update_header_status();
    update_header_mode();

    uint8_t mode = chat_view_get_mode();

    /* Stream LLM tokens into the view */
    if (st == VOICE_STATE_PROCESSING || st == VOICE_STATE_SPEAKING) {
        const char *llm = voice_get_llm_text();
        if (llm && llm[0]) {
            /* Check for tool tags */
            const char *tool = detect_tool_name(llm);

            /* Strip tool tags for display */
            char stripped[1024];
            strip_tool_tags(llm, stripped, sizeof(stripped));

            if (stripped[0]) {
                if (!s_streaming_active) {
                    /* First text — add an AI message to store and start streaming */
                    chat_msg_t msg = {0};
                    msg.type = MSG_TEXT;
                    msg.is_user = false;
                    safe_copy(msg.text, sizeof(msg.text), stripped);
                    tab5_rtc_time_t now;
                    if (tab5_rtc_get_time(&now) == ESP_OK) {
                        msg.timestamp = now.hour * 3600 + now.minute * 60 + now.second;
                    }
                    chat_store_add(mode, &msg);
                    chat_suggestions_hide();
                    chat_view_refresh();
                    s_streaming_active = true;
                }
                /* Update streaming text */
                chat_view_append_streaming(stripped);
            }

            /* Tool indicator: add ephemeral tool message */
            if (tool && !stripped[0]) {
                const char *label = friendly_tool_name(tool);
                char tool_text[128];
                snprintf(tool_text, sizeof(tool_text), LV_SYMBOL_WIFI "  %s...", label);
                /* Check if last message is already a tool indicator */
                chat_msg_t *last = chat_store_last(mode);
                if (!last || last->type != MSG_TOOL_STATUS) {
                    chat_msg_t tmsg = {0};
                    tmsg.type = MSG_TOOL_STATUS;
                    tmsg.is_user = false;
                    safe_copy(tmsg.text, sizeof(tmsg.text), tool_text);
                    chat_store_add(mode, &tmsg);
                    chat_view_refresh();
                    chat_view_scroll_to_bottom();
                }
            }
        } else if (!s_streaming_active) {
            /* PROCESSING but no LLM text yet — show typing indicator */
            chat_msg_t *last = chat_store_last(mode);
            if (!last || last->type != MSG_TOOL_STATUS) {
                /* Only add typing indicator if not already showing one */
                if (!last || (last->type != MSG_SYSTEM ||
                    strcmp(last->text, LV_SYMBOL_REFRESH "  Tinker is thinking...") != 0)) {
                    chat_msg_t tmsg = {0};
                    tmsg.type = MSG_SYSTEM;
                    tmsg.is_user = false;
                    safe_copy(tmsg.text, sizeof(tmsg.text),
                             LV_SYMBOL_REFRESH "  Tinker is thinking...");
                    chat_store_add(mode, &tmsg);
                    chat_view_refresh();
                    chat_view_scroll_to_bottom();
                }
            }
        }
    }

    /* Transition: PROCESSING/SPEAKING -> READY/IDLE = finalize */
    if (s_last_state == VOICE_STATE_PROCESSING || s_last_state == VOICE_STATE_SPEAKING) {
        if (st == VOICE_STATE_READY || st == VOICE_STATE_IDLE) {
            if (s_streaming_active) {
                /* Commit final text to store */
                const char *llm = voice_get_llm_text();
                if (llm && llm[0]) {
                    char stripped[1024];
                    strip_tool_tags(llm, stripped, sizeof(stripped));
                    if (stripped[0]) {
                        chat_msg_t *last = chat_store_last(mode);
                        if (last && last->type == MSG_TEXT && !last->is_user) {
                            safe_copy(last->text, sizeof(last->text), stripped);
                            last->height_px = 0;  /* force re-measure */
                        }
                    }
                }
                chat_view_finalize_streaming();
                s_streaming_active = false;
            }

            /* Remove tool/typing indicators */
            chat_msg_t *last = chat_store_last(mode);
            while (last && (last->type == MSG_TOOL_STATUS || last->type == MSG_SYSTEM)) {
                /* Remove the ephemeral message by clearing it and adjusting count.
                 * Since it's the last message, we can just clear it. The store
                 * doesn't have a remove_last, so we overwrite with empty.
                 * Actually, let's just leave them — they're informational.
                 * Better approach: on the NEXT add_message they'll scroll away. */
                break;
            }

            chat_view_refresh();
        }
    }

    s_last_state = st;
}

/* ── Image download pattern (copied from previous version) ───── */

typedef struct {
    char       url[256];
    lv_obj_t  *placeholder_parent;  /* not used in new arch — kept for compat */
    uint32_t   gen;
} img_load_ctx_t;

static void img_loaded_cb(void *arg)
{
    img_load_ctx_t *ctx = (img_load_ctx_t *)arg;
    if (!ctx) return;

    if (ctx->gen != s_img_load_gen || !s_overlay) {
        ESP_LOGD(TAG, "Image load stale, discarding");
        free(ctx);
        return;
    }

    /* Just refresh the view — the pool will show placeholder or updated content */
    chat_view_refresh();
    chat_view_scroll_to_bottom();

    ESP_LOGI(TAG, "Image downloaded: %s", ctx->url);
    free(ctx);
}

static void img_download_task(void *arg)
{
    img_load_ctx_t *ctx = (img_load_ctx_t *)arg;
    if (!ctx) { vTaskSuspend(NULL); return; }

    if (!s_img_dl_sem) {
        s_img_dl_sem = xSemaphoreCreateBinary();
        xSemaphoreGive(s_img_dl_sem);
    }

    xSemaphoreTake(s_img_dl_sem, portMAX_DELAY);

    lv_image_dsc_t dsc;
    media_cache_fetch(ctx->url, &dsc);

    xSemaphoreGive(s_img_dl_sem);

    lv_async_call(img_loaded_cb, ctx);
    vTaskSuspend(NULL);
}

/* ── Public API ───────────────────────────────────────────────── */

lv_obj_t *ui_chat_create(void)
{
    if (s_active) return s_overlay;

    /* Re-show existing overlay */
    if (s_overlay) {
        ESP_LOGI(TAG, "Restoring chat overlay");
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(s_overlay);
        s_active = true;
        s_last_state = voice_get_state();
        if (s_poll_timer) lv_timer_resume(s_poll_timer);
        else s_poll_timer = lv_timer_create(poll_voice_cb, 200, NULL);
        update_header_mode();
        update_header_status();
        ui_keyboard_set_layout_cb(chat_keyboard_layout_cb);
        return s_overlay;
    }

    ESP_LOGI(TAG, "Creating chat overlay");
    media_cache_init();
    if (!s_store_inited) { chat_store_init(); s_store_inited = true; }
    feed_wdt_yield();

    /* Fullscreen overlay on home screen */
    lv_obj_t *parent = ui_home_get_screen();
    if (!parent) parent = lv_screen_active();

    s_overlay = lv_obj_create(parent);
    if (!s_overlay) { ESP_LOGE(TAG, "OOM: overlay"); return NULL; }
    lv_obj_set_size(s_overlay, 720, 1280);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_overlay);

    /* Edge-swipe gesture */
    lv_obj_add_event_cb(s_overlay, cb_touch_down, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_overlay, cb_close, LV_EVENT_GESTURE, NULL);

    /* FLEX COLUMN layout: header / scroll (grow) / input bar */
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_overlay, 0, 0);

    /* Limit overlay to usable area (above nav bar) */
    lv_obj_set_size(s_overlay, 720, 1160);

    feed_wdt_yield();

    /* 1. Header */
    uint8_t mode = tab5_settings_get_voice_mode();
    if (mode >= VOICE_MODE_COUNT) mode = 0;
    s_header = chat_header_create(s_overlay, "Chat", s_mode_colors[mode], true);
    if (s_header) {
        chat_header_set_back_cb(s_header, cb_close, NULL);
        chat_header_set_action_cb(s_header, cb_new_chat, NULL);
        /* Make mode badge tappable for cycling */
        if (s_header->mode_badge) {
            lv_obj_add_flag(s_header->mode_badge, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(s_header->mode_badge, 10);
            lv_obj_add_event_cb(s_header->mode_badge, cb_mode_cycle, LV_EVENT_CLICKED, NULL);
        }
        /* Title tap opens the sessions browser (v5 spec shot-06 right pane). */
        if (s_header->title) {
            lv_obj_add_flag(s_header->title, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(s_header->title, 16);
            lv_obj_add_event_cb(s_header->title, cb_open_sessions,
                                LV_EVENT_CLICKED, NULL);
        }
    }

    feed_wdt_yield();

    /* 2. Message view (flex-grow fills middle) */
    chat_view_init(s_overlay);
    chat_view_set_mode(mode);

    feed_wdt_yield();

    /* 3. Input bar */
    s_input_bar = chat_input_bar_create(s_overlay, s_mode_colors[mode]);
    if (s_input_bar) {
        chat_input_bar_set_callbacks(s_input_bar, cb_send, cb_mic, cb_textarea_click);
    }

    feed_wdt_yield();

    /* Show suggestions if store is empty for this mode */
    if (chat_store_count(mode) == 0) {
        lv_obj_t *scroll = chat_view_get_scroll();
        if (scroll) chat_suggestions_create(scroll, mode, cb_suggestion_tap);
    }

    /* Done */
    s_active = true;
    s_last_state = voice_get_state();
    s_streaming_active = false;
    ui_keyboard_set_layout_cb(chat_keyboard_layout_cb);
    s_poll_timer = lv_timer_create(poll_voice_cb, 200, NULL);

    update_header_mode();
    update_header_status();
    validate_model_for_mode();

    ESP_LOGI(TAG, "Chat overlay ready");
    return s_overlay;
}

void ui_chat_add_message(const char *text, bool is_user)
{
    if (!text || !text[0]) return;

    char stripped[1024];
    strip_tool_tags(text, stripped, sizeof(stripped));
    if (!stripped[0]) return;

    uint8_t mode = chat_view_get_mode();

    chat_msg_t msg = {0};
    msg.type = MSG_TEXT;
    msg.is_user = is_user;
    safe_copy(msg.text, sizeof(msg.text), stripped);

    tab5_rtc_time_t now;
    if (tab5_rtc_get_time(&now) == ESP_OK) {
        msg.timestamp = now.hour * 3600 + now.minute * 60 + now.second;
    }

    chat_store_add(mode, &msg);
    chat_suggestions_hide();
    chat_view_refresh();
    chat_view_scroll_to_bottom();
}

void ui_chat_hide(void)
{
    if (!s_overlay) return;
    ui_keyboard_set_layout_cb(NULL);
    ui_keyboard_hide();
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    s_active = false;
    ESP_LOGI(TAG, "Chat hidden");
}

void ui_chat_destroy(void)
{
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
    if (s_clear_timer) { lv_timer_delete(s_clear_timer); s_clear_timer = NULL; }
    ui_keyboard_set_layout_cb(NULL);
    ui_keyboard_hide();

    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }

    if (s_header) { free(s_header); s_header = NULL; }
    if (s_input_bar) { free(s_input_bar); s_input_bar = NULL; }

    s_active = false;
    s_last_state = VOICE_STATE_IDLE;
    s_streaming_active = false;
    s_img_load_gen++;

    ESP_LOGI(TAG, "Chat destroyed");
}

bool ui_chat_is_active(void) { return s_active; }

/* ── Thread-safe push from voice task ─────────────────────────── */

static void async_push_cb(void *arg)
{
    chat_push_msg_t *msg = (chat_push_msg_t *)arg;
    if (!msg) return;

    bool is_user = (msg->role && strcmp(msg->role, "user") == 0);

    if (!s_overlay) {
        ESP_LOGW(TAG, "push_message: no overlay, dropping: %.40s...",
                 msg->text ? msg->text : "(null)");
        goto cleanup;
    }

    /* Dedup: if assistant and overlay is active, poll already handled it */
    if (!is_user && s_active) {
        ESP_LOGD(TAG, "push_message: skipping assistant (poll handled)");
        goto cleanup;
    }

    if (msg->text && msg->text[0]) {
        ui_chat_add_message(msg->text, is_user);
    }

cleanup:
    free(msg->role);
    free(msg->text);
    free(msg);
}

void ui_chat_push_message(const char *role, const char *text)
{
    if (!text || !text[0]) return;
    chat_push_msg_t *msg = malloc(sizeof(chat_push_msg_t));
    if (!msg) return;
    msg->role = strdup(role ? role : "assistant");
    msg->text = strdup(text);
    if (!msg->role || !msg->text) { free(msg->role); free(msg->text); free(msg); return; }
    lv_async_call(async_push_cb, msg);
}

/* ── Update last AI message (text_update from Dragon) ─────────── */

static void async_update_last_cb(void *arg)
{
    chat_update_msg_t *msg = (chat_update_msg_t *)arg;
    if (!msg) return;

    if (!s_overlay || !msg->text || !msg->text[0]) {
        free(msg->text); free(msg); return;
    }

    uint8_t mode = chat_view_get_mode();
    /* Find last AI message in store and update it */
    int count = chat_store_count(mode);
    for (int i = count - 1; i >= 0; i--) {
        chat_msg_t *m = chat_store_get_mut(mode, i);
        if (m && m->type == MSG_TEXT && !m->is_user) {
            safe_copy(m->text, sizeof(m->text), msg->text);
            m->height_px = 0;  /* force re-measure */
            break;
        }
    }

    chat_view_refresh();
    ESP_LOGI(TAG, "text_update: last AI message updated (%d chars)", (int)strlen(msg->text));

    free(msg->text);
    free(msg);
}

void ui_chat_update_last_message(const char *text)
{
    if (!text || !text[0]) return;
    chat_update_msg_t *msg = malloc(sizeof(chat_update_msg_t));
    if (!msg) return;
    msg->text = strdup(text);
    if (!msg->text) { free(msg); return; }
    lv_async_call(async_update_last_cb, msg);
}

/* ── Rich media push callbacks ────────────────────────────────── */

static void push_media_cb(void *arg)
{
    media_push_t *m = (media_push_t *)arg;
    if (!m) return;
    if (!s_overlay) { free(m); return; }

    uint8_t mode = chat_view_get_mode();
    chat_msg_t msg = {0};
    msg.type = MSG_IMAGE;
    msg.is_user = false;
    safe_copy(msg.text, sizeof(msg.text), m->alt);
    safe_copy(msg.media_url, sizeof(msg.media_url), m->url);
    tab5_rtc_time_t now;
    if (tab5_rtc_get_time(&now) == ESP_OK)
        msg.timestamp = now.hour * 3600 + now.minute * 60 + now.second;
    chat_store_add(mode, &msg);
    chat_suggestions_hide();
    chat_view_refresh();
    chat_view_scroll_to_bottom();

    /* Spawn background download */
    img_load_ctx_t *ctx = malloc(sizeof(img_load_ctx_t));
    if (ctx) {
        safe_copy(ctx->url, sizeof(ctx->url), m->url);
        ctx->placeholder_parent = NULL;
        ctx->gen = s_img_load_gen;
        xTaskCreatePinnedToCore(img_download_task, "img_dl", 4096, ctx, 2, NULL, 1);
    }

    free(m);
}

static void push_card_cb(void *arg)
{
    card_push_t *c = (card_push_t *)arg;
    if (!c) return;
    if (!s_overlay) { free(c); return; }

    uint8_t mode = chat_view_get_mode();
    chat_msg_t msg = {0};
    msg.type = MSG_CARD;
    msg.is_user = false;
    safe_copy(msg.text, sizeof(msg.text), c->title);
    safe_copy(msg.subtitle, sizeof(msg.subtitle), c->subtitle);
    if (c->image_url[0])
        safe_copy(msg.media_url, sizeof(msg.media_url), c->image_url);
    tab5_rtc_time_t now;
    if (tab5_rtc_get_time(&now) == ESP_OK)
        msg.timestamp = now.hour * 3600 + now.minute * 60 + now.second;
    chat_store_add(mode, &msg);
    chat_suggestions_hide();
    chat_view_refresh();
    chat_view_scroll_to_bottom();

    free(c);
}

static void push_audio_clip_cb(void *arg)
{
    audio_push_t *a = (audio_push_t *)arg;
    if (!a) return;
    if (!s_overlay) { free(a); return; }

    uint8_t mode = chat_view_get_mode();
    chat_msg_t msg = {0};
    msg.type = MSG_AUDIO_CLIP;
    msg.is_user = false;
    safe_copy(msg.text, sizeof(msg.text), a->label[0] ? a->label : "Audio clip");
    safe_copy(msg.media_url, sizeof(msg.media_url), a->url);
    tab5_rtc_time_t now;
    if (tab5_rtc_get_time(&now) == ESP_OK)
        msg.timestamp = now.hour * 3600 + now.minute * 60 + now.second;
    chat_store_add(mode, &msg);
    chat_suggestions_hide();
    chat_view_refresh();
    chat_view_scroll_to_bottom();

    free(a);
}

void ui_chat_push_media(const char *url, const char *media_type,
                        int width, int height, const char *alt)
{
    (void)media_type;
    media_push_t *m = malloc(sizeof(media_push_t));
    if (!m) return;
    safe_copy(m->url, sizeof(m->url), url);
    safe_copy(m->alt, sizeof(m->alt), alt);
    m->width = width;
    m->height = height;
    lv_async_call(push_media_cb, m);
}

void ui_chat_push_card(const char *title, const char *subtitle,
                       const char *image_url, const char *description)
{
    (void)description; /* not stored in chat_msg_t — subtitle covers it */
    card_push_t *c = malloc(sizeof(card_push_t));
    if (!c) return;
    safe_copy(c->title, sizeof(c->title), title);
    safe_copy(c->subtitle, sizeof(c->subtitle), subtitle);
    safe_copy(c->image_url, sizeof(c->image_url), image_url);
    safe_copy(c->description, sizeof(c->description), description);
    lv_async_call(push_card_cb, c);
}

void ui_chat_push_audio_clip(const char *url, float duration_s, const char *label)
{
    audio_push_t *a = malloc(sizeof(audio_push_t));
    if (!a) return;
    safe_copy(a->url, sizeof(a->url), url);
    a->duration_s = duration_s;
    safe_copy(a->label, sizeof(a->label), label);
    lv_async_call(push_audio_clip_cb, a);
}
