/**
 * TinkerTab — Chat Screen (iMessage/WhatsApp style)
 *
 * TWO-PANEL architecture within a single fullscreen overlay:
 *   1. Chat Home (s_home_panel) — session cards, model picker, quick actions
 *   2. Conversation (s_conv_panel) — existing chat bubbles + streaming
 *
 * Only one panel visible at a time via LV_OBJ_FLAG_HIDDEN.
 * Manual Y positioning — NO flex layout. Object count kept under 55 per panel.
 * Show/hide via LV_OBJ_FLAG_HIDDEN + lv_obj_move_foreground().
 */
#include "ui_chat.h"
#include "ui_feedback.h"
#include "voice.h"
#include "config.h"
#include "ui_keyboard.h"
#include "settings.h"
#include "rtc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

/* Declared in ui_home.c */
extern lv_obj_t *ui_home_get_screen(void);

static const char *TAG = "ui_chat";

/* Yield to LWIP TCP/IP task during heavy UI creation.
 * Same pattern as ui_settings.c — prevents HTTP timeout. */
static inline void feed_wdt_yield(void) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ── State ─────────────────────────────────────────────────────── */
static lv_obj_t  *s_overlay      = NULL;

/* Two-panel architecture */
static lv_obj_t  *s_home_panel   = NULL;   /* Chat Home (session list) */
static lv_obj_t  *s_conv_panel   = NULL;   /* Conversation view (existing bubbles) */
static bool       s_in_conversation = false; /* Which panel is active */

/* Home panel widgets */
static lv_obj_t  *s_model_lbl    = NULL;   /* Model name label */
static lv_obj_t  *s_home_status_dot = NULL;
static lv_obj_t  *s_home_status_lbl = NULL;
static lv_obj_t  *s_home_mode_badge = NULL;
static lv_obj_t  *s_memory_lbl   = NULL;   /* Memory preview label */
static lv_obj_t  *s_current_card_preview = NULL; /* Last message preview on card */
static lv_obj_t  *s_home_textarea = NULL;   /* Textarea on Chat Home */

/* Conversation panel widgets */
static lv_obj_t  *s_msg_scroll   = NULL;
static lv_obj_t  *s_textarea     = NULL;    /* Currently active textarea (swapped on panel switch) */
static lv_obj_t  *s_conv_textarea = NULL;   /* Textarea on Conversation panel */
static lv_obj_t  *s_mode_badge   = NULL;   /* "Local" / "Cloud" label in conv */
static lv_timer_t *s_poll_timer  = NULL;

/* Input bars — need references for keyboard layout adjustment */
static lv_obj_t  *s_home_input_bar = NULL;
static lv_obj_t  *s_conv_input_bar = NULL;

/* Streaming assistant response tracking */
static lv_obj_t  *s_assist_bubble = NULL;
static lv_obj_t  *s_assist_label  = NULL;

/* Tool indicator */
static lv_obj_t  *s_tool_label   = NULL;

/* Archived messages notification (US-PR22) */
static lv_obj_t  *s_archived_lbl = NULL;
static bool       s_archived_shown = false;

/* Typing indicator */
static lv_obj_t  *s_typing_lbl   = NULL;

/* Empty state hint */
static lv_obj_t  *s_empty_hint   = NULL;

/* Status bar (in conv panel) */
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_status_dot = NULL;

static bool         s_active     = false;
static int          s_msg_count  = 0;
static int          s_next_y     = 0;      /* Running Y offset in scroll area */
static voice_state_t s_last_state = VOICE_STATE_IDLE;
static bool         s_conv_created = false; /* Has conversation UI been built? */
static int32_t      s_touch_start_x = -1;  /* X at touch-down for edge-swipe gating */
static bool         s_clear_guard = false;  /* Guard flag: blocks input after New Chat (US-PR03) */
static lv_timer_t  *s_clear_timer = NULL;   /* 500ms guard timer for clear_history ACK */
static bool         s_history_fetched = false; /* Guard: only fetch history once per session */

#define MAX_MESSAGES     30
#define SWIPE_EDGE_PX    60  /* back-gesture only from left 60 px */
#define BUBBLE_MAX_W    500
#define BUBBLE_PAD       16
#define BUBBLE_GAP        8
#define LABEL_MAX_W     (BUBBLE_MAX_W - 2 * BUBBLE_PAD)  /* 468 */

#define TOPBAR_H         60
#define INPUT_BAR_H      80
#define MSG_AREA_H      (1280 - TOPBAR_H - INPUT_BAR_H)  /* 1140 */

#define CLR_BG           0x0A0A0F   /* Material Dark near-black */
#define CLR_TOPBAR       0x0A0A0F   /* matches overlay bg */
#define CLR_BORDER       0x1A1A2E   /* subtle indigo border */
#define CLR_USER_BUB     0xF5A623   /* golden/amber */
#define CLR_TINKER_BUB   0x2A2A3E   /* lighter indigo — visible on OLED */
#define CLR_CYAN         0x00E5FF
#define CLR_AMBER        0xF5A623
#define CLR_INPUT_BG     0x1A1A2E   /* text input bg */
#define CLR_INPUT_BORDER 0x333333   /* text input border */
#define CLR_TOOL_DIM     0x00E5FF   /* dim cyan for tool indicator */
#define CLR_CARD_BG      0x141420   /* session card bg */
#define CLR_CARD_BORDER  0x2A2A3E   /* session card border */

/* ── Helpers ───────────────────────────────────────────────────── */

/**
 * Get current time as "HH:MM" string. Returns false if RTC unavailable.
 */
static bool get_time_str(char *buf, size_t buf_sz)
{
    tab5_rtc_time_t now;
    if (tab5_rtc_get_time(&now) == ESP_OK) {
        snprintf(buf, buf_sz, "%02d:%02d", now.hour, now.minute);
        return true;
    }
    return false;
}

/**
 * Strip <tool>...</tool> and everything from <tool> to end-of-string.
 * Writes result into `out` (max `out_sz` bytes including NUL).
 */
static void strip_tool_tags(const char *src, char *out, size_t out_sz)
{
    if (!src || !out || out_sz == 0) return;
    size_t j = 0;
    const char *p = src;
    while (*p && j < out_sz - 1) {
        if (*p == '<') {
            /* Check for <tool */
            if (strncmp(p, "<tool", 5) == 0) {
                /* Skip everything from <tool to end — per spec */
                break;
            }
        }
        out[j++] = *p++;
    }
    out[j] = '\0';
    /* Trim trailing whitespace */
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\n' || out[j - 1] == '\r')) {
        out[--j] = '\0';
    }
}

/**
 * Detect if text contains a tool invocation hint.
 * Returns pointer to tool name if found (e.g. "web_search"), else NULL.
 */
static const char *detect_tool_name(const char *text)
{
    if (!text) return NULL;
    const char *t = strstr(text, "<tool");
    if (!t) return NULL;
    /* Try to find the tool name in a name="..." attribute */
    const char *n = strstr(t, "name=\"");
    if (n) {
        n += 6;
        /* Return pointer — caller should only read up to next quote */
        return n;
    }
    return "a tool";
}

/**
 * Map raw tool names to user-friendly labels (US-PR24).
 * The raw_name pointer may not be null-terminated at the tool name boundary
 * (it points into a larger string after name="), so we use strncmp.
 */
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
             raw_name[map[i].len] == ' ')) {
            return map[i].friendly;
        }
    }

    return "Using a tool";
}

/* ── Mode / status helpers ─────────────────────────────────────── */

static void update_mode_badge_obj(lv_obj_t *badge)
{
    if (!badge) return;
    uint8_t mode = tab5_settings_get_voice_mode();
    const char *labels[] = {"Local", "Hybrid", "Cloud", "TinkerClaw"};
    const uint32_t colors[] = {0x22C55E, 0xF5A623, 0x06B6D4, 0xE11D48};
    if (mode >= VOICE_MODE_COUNT) mode = 0;
    lv_label_set_text(badge, labels[mode]);
    lv_obj_set_style_text_color(badge, lv_color_hex(colors[mode]), 0);
}

static void update_mode_badge(void)
{
    update_mode_badge_obj(s_mode_badge);
    update_mode_badge_obj(s_home_mode_badge);
}

static void update_status_bar(void)
{
    voice_state_t st = voice_get_state();
    bool connected = voice_is_connected();

    const char *state_text = "Offline";
    uint32_t state_color = 0x666666;
    uint32_t dot_color = connected ? 0x22C55E : 0xFF453A;
    switch (st) {
        case VOICE_STATE_IDLE:       state_text = connected ? "Ready" : "Offline"; state_color = connected ? 0x666666 : 0xFF453A; break;
        case VOICE_STATE_CONNECTING: state_text = "Connecting..."; state_color = 0xF5A623; break;
        case VOICE_STATE_READY:      state_text = "Ready"; state_color = 0x22C55E; break;
        case VOICE_STATE_LISTENING:  state_text = "Listening..."; state_color = 0x06B6D4; break;
        case VOICE_STATE_PROCESSING: state_text = "Processing..."; state_color = 0xF5A623; break;
        case VOICE_STATE_SPEAKING:   state_text = "Speaking..."; state_color = 0xBF5AF2; break;
    }

    /* Update conv panel status */
    if (s_status_lbl && s_status_dot) {
        lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(dot_color), 0);
        lv_label_set_text(s_status_lbl, state_text);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(state_color), 0);
    }

    /* Update home panel status */
    if (s_home_status_lbl && s_home_status_dot) {
        lv_obj_set_style_bg_color(s_home_status_dot, lv_color_hex(dot_color), 0);
        lv_label_set_text(s_home_status_lbl, state_text);
        lv_obj_set_style_text_color(s_home_status_lbl, lv_color_hex(state_color), 0);
    }
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
    update_mode_badge();
    ESP_LOGI(TAG, "Mode cycled to %d", mode);
}

/* Model lists per mode — cycle through on tap */
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

/**
 * Validate that the NVS-stored llm_mdl belongs to the current voice mode's
 * model list.  If not (e.g. mode changed since the model was saved), reset
 * to the first model in the current mode's list and persist + notify Dragon.
 * (US-PR05)
 */
static void validate_model_for_mode(void)
{
    uint8_t mode = tab5_settings_get_voice_mode();
    char cur_model[64];
    tab5_settings_get_llm_model(cur_model, sizeof(cur_model));

    const char **models;
    int n;
    if (mode == VOICE_MODE_TINKERCLAW) {
        models = s_tinkerclaw_models;
        n = N_TINKERCLAW_MODELS;
    } else if (mode == VOICE_MODE_CLOUD) {
        models = s_cloud_models;
        n = N_CLOUD_MODELS;
    } else {
        models = s_local_models;
        n = N_LOCAL_MODELS;
    }

    /* Check if current model is in the mode's list */
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(cur_model, models[i]) == 0) { found = true; break; }
    }

    if (!found) {
        ESP_LOGW(TAG, "NVS model '%s' invalid for mode %d — resetting to '%s'",
                 cur_model, mode, models[0]);
        tab5_settings_set_llm_model(models[0]);
        voice_send_config_update(mode, models[0]);
    }
}

/**
 * Map technical model identifiers to user-friendly display names.
 * Returns a static string — do not free.
 */
static const char *friendly_model_name(const char *model_id)
{
    if (!model_id || !model_id[0]) return "Local AI";

    /* Exact matches first */
    static const struct { const char *id; const char *friendly; } map[] = {
        { "qwen3:1.7b",                        "Local AI" },
        { "qwen3:4b",                          "Local AI" },
        { "anthropic/claude-3.5-haiku",         "Claude Haiku" },
        { "anthropic/claude-sonnet-4-20250514", "Claude Sonnet" },
        { "openai/gpt-4o-mini",                "GPT-4o mini" },
    };
    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        if (strcmp(model_id, map[i].id) == 0) return map[i].friendly;
    }

    /* Substring matches */
    if (strstr(model_id, "MiniMax") || strstr(model_id, "minimax"))
        return "MiniMax Agent";
    if (strstr(model_id, "qwen"))
        return "Local AI";

    /* Fallback: strip provider prefix, show as-is */
    const char *slash = strrchr(model_id, '/');
    return slash ? slash + 1 : model_id;
}

static void update_model_label(void)
{
    if (!s_model_lbl) return;

    /* Ensure NVS model matches current voice mode (US-PR05) */
    validate_model_for_mode();

    char model_buf[64];
    tab5_settings_get_llm_model(model_buf, sizeof(model_buf));
    uint8_t mode = tab5_settings_get_voice_mode();

    /* Get friendly display name */
    const char *display_name;
    if (!model_buf[0]) {
        display_name = (mode >= VOICE_MODE_CLOUD) ? "Claude Haiku" : "Local AI";
    } else {
        display_name = friendly_model_name(model_buf);
    }

    /* TinkerClaw mode: prefix with "Agent:" to signal it's an agent, not just LLM */
    if (mode == VOICE_MODE_TINKERCLAW) {
        lv_label_set_text_fmt(s_model_lbl, LV_SYMBOL_SETTINGS " Agent: %s", display_name);
        lv_obj_set_style_text_color(s_model_lbl, lv_color_hex(0xE11D48), 0);
    } else {
        lv_label_set_text_fmt(s_model_lbl, LV_SYMBOL_EDIT " %s", display_name);
        lv_obj_set_style_text_color(s_model_lbl, lv_color_hex(0xAAAAAA), 0);
    }
}

static void cb_model_cycle(lv_event_t *e)
{
    (void)e;
    uint8_t mode = tab5_settings_get_voice_mode();
    char cur_model[64];
    tab5_settings_get_llm_model(cur_model, sizeof(cur_model));

    const char **models;
    int n;
    if (mode == VOICE_MODE_TINKERCLAW) {
        models = s_tinkerclaw_models;
        n = N_TINKERCLAW_MODELS;
    } else if (mode == VOICE_MODE_CLOUD) {
        models = s_cloud_models;
        n = N_CLOUD_MODELS;
    } else {
        models = s_local_models;
        n = N_LOCAL_MODELS;
    }

    /* Find current index, advance to next */
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(cur_model, models[i]) == 0) { idx = i; break; }
    }
    idx = (idx + 1) % n;

    tab5_settings_set_llm_model(models[idx]);
    voice_send_config_update(mode, models[idx]);
    update_model_label();
    ESP_LOGI(TAG, "Model cycled to: %s (mode=%d)", models[idx], mode);
}

/* Timer callback: lift the clear_history guard after 500ms (US-PR03) */
static void clear_guard_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_clear_guard = false;
    s_clear_timer = NULL;
    ESP_LOGI(TAG, "New-chat guard lifted — input re-enabled");
}

static void cb_new_chat(lv_event_t *e)
{
    (void)e;

    /* Prevent rapid double-taps while guard is active */
    if (s_clear_guard) return;

    voice_clear_history();

    /* Engage 500ms guard — blocks send/mic until Dragon processes the clear (US-PR03) */
    s_clear_guard = true;
    if (s_clear_timer) { lv_timer_delete(s_clear_timer); s_clear_timer = NULL; }
    s_clear_timer = lv_timer_create(clear_guard_timer_cb, 500, NULL);
    lv_timer_set_repeat_count(s_clear_timer, 1);

    /* Clear all messages from scroll */
    if (s_msg_scroll) lv_obj_clean(s_msg_scroll);
    s_msg_count = 0;
    s_next_y = 12;
    s_assist_bubble = NULL;
    s_assist_label = NULL;
    s_tool_label = NULL;
    s_typing_lbl = NULL;
    s_empty_hint = NULL;
    s_archived_lbl = NULL;
    s_archived_shown = false;
    s_history_fetched = false;  /* Allow re-fetch on next enter */
    /* Re-add welcome */
    ui_chat_add_message("Clearing...", false);
    ESP_LOGI(TAG, "New chat — history cleared, 500ms guard active");
}

/* ── Forward declarations ──────────────────────────────────────── */
static void hide_typing_indicator(void);
static void hide_tool_indicator(void);
static void build_conversation_ui(void);
static void enter_conversation(void);
static void show_chat_home(void);

/* ── Typing indicator management ──────────────────────────────── */

static void show_typing_indicator(void)
{
    if (s_typing_lbl || !s_msg_scroll) return;

    /* Create a small bubble-style indicator — left-aligned with generous padding */
    s_typing_lbl = lv_obj_create(s_msg_scroll);
    lv_obj_remove_style_all(s_typing_lbl);
    lv_obj_set_size(s_typing_lbl, 240, 40);
    lv_obj_set_pos(s_typing_lbl, 20, s_next_y);
    lv_obj_set_style_bg_color(s_typing_lbl, lv_color_hex(CLR_TINKER_BUB), 0);
    lv_obj_set_style_bg_opa(s_typing_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_typing_lbl, 18, 0);
    lv_obj_set_style_pad_left(s_typing_lbl, 16, 0);
    lv_obj_clear_flag(s_typing_lbl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(s_typing_lbl);
    lv_label_set_text(lbl, LV_SYMBOL_REFRESH "  Tinker is thinking...");
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_OFF);
}

static void show_tool_indicator(const char *tool_name)
{
    hide_typing_indicator();
    if (s_tool_label || !s_msg_scroll) return;

    s_tool_label = lv_obj_create(s_msg_scroll);
    lv_obj_remove_style_all(s_tool_label);
    lv_obj_set_size(s_tool_label, 300, 36);
    lv_obj_set_pos(s_tool_label, 16, s_next_y);
    lv_obj_set_style_bg_color(s_tool_label, lv_color_hex(0x0A2030), 0);
    lv_obj_set_style_bg_opa(s_tool_label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_tool_label, 18, 0);
    lv_obj_set_style_border_width(s_tool_label, 1, 0);
    lv_obj_set_style_border_color(s_tool_label, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_pad_left(s_tool_label, 16, 0);
    lv_obj_clear_flag(s_tool_label, LV_OBJ_FLAG_SCROLLABLE);

    char buf[128];
    const char *label = friendly_tool_name(tool_name);
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  %s...", label);
    lv_obj_t *lbl = lv_label_create(s_tool_label);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_OFF);
}

static void hide_tool_indicator(void)
{
    if (s_tool_label) {
        lv_obj_del(s_tool_label);
        s_tool_label = NULL;
    }
}

static void hide_typing_indicator(void)
{
    if (s_typing_lbl) {
        lv_obj_del(s_typing_lbl);
        s_typing_lbl = NULL;
    }
}

/* ── Callbacks ─────────────────────────────────────────────────── */

/** Record touch-down X so the gesture callback can gate on left-edge start. */
static void cb_touch_down(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    s_touch_start_x = p.x;
}

static void cb_close(lv_event_t *e)
{
    /* Support swipe-right as back gesture — but only from left edge */
    if (e && lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;

        /* Only accept swipes that started within the left-edge strip */
        if (s_touch_start_x < 0 || s_touch_start_x > SWIPE_EDGE_PX) return;
    }

    /* If in conversation, go back to Chat Home instead of closing */
    if (s_in_conversation) {
        show_chat_home();
        return;
    }

    /* Not in conversation — close entire overlay */
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
    ui_keyboard_set_layout_cb(NULL);
    ui_keyboard_hide();

    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    }

    s_assist_bubble = NULL;
    s_assist_label  = NULL;
    s_tool_label    = NULL;
    s_typing_lbl    = NULL;
    s_active        = false;
    ESP_LOGI(TAG, "Chat hidden (messages preserved)");
}

/* ── Keyboard layout callback — move input bar above keyboard ── */
static void chat_keyboard_layout_cb(bool visible, int kb_height)
{
    /* Determine which input bar is active */
    lv_obj_t *bar = s_in_conversation ? s_conv_input_bar : s_home_input_bar;
    if (!bar) return;

    if (visible) {
        /* Move input bar above the keyboard.
         * Normal position: 1280 - INPUT_BAR_H = 1200
         * Keyboard top:    1280 - kb_height    =  860
         * New position:    860 - INPUT_BAR_H   =  780 */
        int new_y = 1280 - kb_height - INPUT_BAR_H;
        lv_obj_set_pos(bar, 0, new_y);

        /* Also shrink the message scroll area in conversation mode */
        if (s_in_conversation && s_msg_scroll) {
            int new_h = new_y - TOPBAR_H;
            lv_obj_set_height(s_msg_scroll, new_h);
            /* Scroll to bottom so latest messages stay visible */
            lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_OFF);
        }
    } else {
        /* Restore to original position */
        lv_obj_set_pos(bar, 0, 1280 - INPUT_BAR_H);

        if (s_in_conversation && s_msg_scroll) {
            lv_obj_set_height(s_msg_scroll, MSG_AREA_H);
            lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_OFF);
        }
    }
}

static void cb_textarea_click(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Textarea tapped — showing keyboard");
    if (s_textarea) ui_keyboard_show(s_textarea);
}

static void cb_mic(lv_event_t *e)
{
    (void)e;
    /* Block input during New Chat guard window (US-PR03) */
    if (s_clear_guard) {
        ESP_LOGI(TAG, "Mic blocked — clear_history guard active");
        return;
    }
    /* If on Chat Home, enter conversation first */
    if (!s_in_conversation) {
        enter_conversation();
    }
    voice_state_t st = voice_get_state();
    if (st == VOICE_STATE_READY) {
        ESP_LOGI(TAG, "Chat mic: starting voice input");
        voice_start_listening();
    } else if (st == VOICE_STATE_IDLE) {
        ESP_LOGW(TAG, "Chat mic: not connected to Dragon");
        ui_chat_add_message("(Not connected -- tap orb on Home to connect)", false);
    } else if (st == VOICE_STATE_LISTENING) {
        voice_stop_listening();
    } else {
        ESP_LOGI(TAG, "Chat mic: voice busy (state=%d)", st);
    }
}

static void cb_send(lv_event_t *e)
{
    (void)e;
    if (!s_textarea) return;
    /* Block input during New Chat guard window (US-PR03) */
    if (s_clear_guard) {
        ESP_LOGI(TAG, "Send blocked — clear_history guard active");
        return;
    }
    ui_keyboard_hide();
    const char *txt = lv_textarea_get_text(s_textarea);
    if (!txt || !txt[0]) return;

    /* If on Chat Home, enter conversation first */
    if (!s_in_conversation) {
        enter_conversation();
    }

    /* Show user bubble immediately */
    ui_chat_add_message(txt, true);

    /* Send to Dragon — voice_send_text rejects if voice pipeline is active (U13) */
    esp_err_t ret = voice_send_text(txt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "voice_send_text failed: %s", esp_err_to_name(ret));
        voice_state_t send_st = voice_get_state();
        if (send_st == VOICE_STATE_LISTENING || send_st == VOICE_STATE_PROCESSING ||
            send_st == VOICE_STATE_SPEAKING) {
            ui_chat_add_message("(Voice is active -- wait for it to finish)", false);
        } else {
            ui_chat_add_message("(Not connected to Dragon)", false);
        }
    }

    lv_textarea_set_text(s_textarea, "");
}

/* Callback: tap on "Current Session" card → enter conversation */
static void cb_current_session(lv_event_t *e)
{
    (void)e;
    enter_conversation();
}

/* Callback: tap on "New Chat" card → clear + enter conversation */
static void cb_new_chat_card(lv_event_t *e)
{
    (void)e;
    cb_new_chat(NULL);
    enter_conversation();
}

/* Suggestion card callback — sends the card's label text as a chat message */
static void cb_suggestion(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_target(e);
    /* The label is child 0 of the suggestion card (set in build_home_panel) */
    lv_obj_t *label = lv_obj_get_child(card, 0);
    if (!label) return;
    const char *raw = lv_label_get_text(label);
    if (!raw || !raw[0]) return;

    /* Strip surrounding quotes from display text (e.g. "\"What's the weather...\"") */
    char text[256];
    size_t len = strlen(raw);
    if (len >= 2 && raw[0] == '"' && raw[len - 1] == '"') {
        size_t copy_len = len - 2;
        if (copy_len >= sizeof(text)) copy_len = sizeof(text) - 1;
        memcpy(text, raw + 1, copy_len);
        text[copy_len] = '\0';
    } else {
        strncpy(text, raw, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
    }

    enter_conversation();
    ui_chat_add_message(text, true);
    voice_send_text(text);
}

/* Quick action callbacks — pre-fill textarea instead of auto-sending (US-PR10) */
static void cb_quick_web(lv_event_t *e)
{
    (void)e;
    enter_conversation();
    lv_textarea_set_text(s_conv_textarea, "Search the web for ");
    lv_textarea_set_cursor_pos(s_conv_textarea, LV_TEXTAREA_CURSOR_LAST);
    ui_keyboard_show(s_conv_textarea);
}

static void cb_quick_remember(lv_event_t *e)
{
    (void)e;
    enter_conversation();
    lv_textarea_set_text(s_conv_textarea, "Remember that ");
    lv_textarea_set_cursor_pos(s_conv_textarea, LV_TEXTAREA_CURSOR_LAST);
    ui_keyboard_show(s_conv_textarea);
}

static void cb_quick_timer(lv_event_t *e)
{
    (void)e;
    enter_conversation();
    lv_textarea_set_text(s_conv_textarea, "Set a timer for ");
    lv_textarea_set_cursor_pos(s_conv_textarea, LV_TEXTAREA_CURSOR_LAST);
    ui_keyboard_show(s_conv_textarea);
}

/* ── Voice polling timer ───────────────────────────────────────── */

static void poll_voice_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    voice_state_t st = voice_get_state();

    /* Update status bar on every poll — shows live state */
    update_status_bar();
    /* Update mode badge in case it changed externally */
    update_mode_badge();

    /* Update model label on home panel */
    if (s_model_lbl && !s_in_conversation) {
        update_model_label();
    }

    /* Update current session card preview */
    if (s_current_card_preview && !s_in_conversation) {
        const char *llm = voice_get_llm_text();
        if (llm && llm[0]) {
            char preview[80];
            strip_tool_tags(llm, preview, sizeof(preview));
            if (preview[0]) {
                /* Truncate to ~60 chars for card */
                if (strlen(preview) > 60) {
                    preview[57] = '.';
                    preview[58] = '.';
                    preview[59] = '.';
                    preview[60] = '\0';
                }
                lv_label_set_text(s_current_card_preview, preview);
            }
        }
    }

    /* Only process streaming if in conversation view */
    if (!s_in_conversation || !s_msg_scroll) {
        s_last_state = st;
        return;
    }

    /* Stream LLM tokens into assistant bubble */
    if (st == VOICE_STATE_PROCESSING || st == VOICE_STATE_SPEAKING) {
        const char *llm = voice_get_llm_text();
        if (llm && llm[0]) {
            /* Check for tool tags */
            const char *tool = detect_tool_name(llm);
            if (tool && !s_tool_label) {
                show_tool_indicator(tool);
            }

            /* Strip tool tags for display */
            char stripped[1024];
            strip_tool_tags(llm, stripped, sizeof(stripped));

            if (stripped[0]) {
                /* Remove typing indicator once real text arrives (keep tool indicator) */
                hide_typing_indicator();

                if (!s_assist_bubble) {
                    /* Create new assistant bubble */
                    ui_chat_add_message(stripped, false);
                    /* Grab the last bubble we just created */
                    int cnt = lv_obj_get_child_count(s_msg_scroll);
                    if (cnt > 0) {
                        s_assist_bubble = lv_obj_get_child(s_msg_scroll, cnt - 1);
                        s_assist_label  = lv_obj_get_child(s_assist_bubble, 0);
                    }
                } else if (s_assist_label) {
                    lv_label_set_text(s_assist_label, stripped);
                    /* Recalc bubble height and reposition scroll */
                    lv_obj_update_layout(s_msg_scroll);
                    int bh = lv_obj_get_height(s_assist_bubble);
                    /* Update s_next_y: bubble Y + new height + gap */
                    int by = lv_obj_get_y(s_assist_bubble);
                    s_next_y = by + bh + BUBBLE_GAP;
                    lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_OFF);
                }
            }
        } else if (!s_assist_bubble && !s_tool_label) {
            /* PROCESSING but no LLM text yet and no tool indicator — show typing */
            show_typing_indicator();
        }
    }

    /* Transition: PROCESSING/SPEAKING -> READY/IDLE = finalize */
    if (s_last_state == VOICE_STATE_PROCESSING || s_last_state == VOICE_STATE_SPEAKING) {
        if (st == VOICE_STATE_READY || st == VOICE_STATE_IDLE) {
            const char *llm = voice_get_llm_text();
            if (llm && llm[0] && s_assist_label) {
                char stripped[1024];
                strip_tool_tags(llm, stripped, sizeof(stripped));
                if (stripped[0]) {
                    lv_label_set_text(s_assist_label, stripped);
                    lv_obj_update_layout(s_msg_scroll);
                    int bh = lv_obj_get_height(s_assist_bubble);
                    int by = lv_obj_get_y(s_assist_bubble);
                    s_next_y = by + bh + BUBBLE_GAP;
                }
            }
            hide_tool_indicator();
            hide_typing_indicator();
            s_assist_bubble = NULL;
            s_assist_label  = NULL;
        }
    }

    s_last_state = st;
}

/* ── Panel transitions ─────────────────────────────────────────── */

static void show_chat_home(void)
{
    ESP_LOGI(TAG, "Showing Chat Home");
    ui_keyboard_hide();
    s_in_conversation = false;

    if (s_conv_panel) {
        lv_obj_add_flag(s_conv_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_home_panel) {
        lv_obj_clear_flag(s_home_panel, LV_OBJ_FLAG_HIDDEN);
    }

    /* Swap active textarea to home panel */
    s_textarea = s_home_textarea;

    /* Refresh card preview */
    if (s_current_card_preview) {
        const char *llm = voice_get_llm_text();
        if (llm && llm[0]) {
            char preview[80];
            strip_tool_tags(llm, preview, sizeof(preview));
            if (preview[0]) {
                if (strlen(preview) > 60) {
                    preview[57] = '.';
                    preview[58] = '.';
                    preview[59] = '.';
                    preview[60] = '\0';
                }
                lv_label_set_text(s_current_card_preview, preview);
            }
        }
    }
}

/* ── History fetch from Dragon REST API ────────────────────────── */

/** Max messages to fetch from Dragon history */
#define HISTORY_FETCH_LIMIT  10
/** Max HTTP response body size for history (10 messages ~ 10KB) */
#define HISTORY_BODY_MAX     16384

/** Holds one history message for async delivery to LVGL thread */
typedef struct {
    char *role;
    char *text;
} history_msg_t;

/** Batch of history messages delivered via lv_async_call */
typedef struct {
    history_msg_t *msgs;
    int count;
} history_batch_t;

static void history_async_cb(void *arg)
{
    history_batch_t *batch = (history_batch_t *)arg;
    if (!batch) return;

    /* Ensure conversation UI exists */
    if (!s_conv_created || !s_msg_scroll) {
        ESP_LOGW(TAG, "history_async: conv not ready, dropping %d msgs", batch->count);
        goto cleanup;
    }

    /* Only add if conversation is still empty (no race with voice messages) */
    if (s_msg_count > 1) {
        ESP_LOGI(TAG, "history_async: conv already has %d msgs, skipping history", s_msg_count);
        goto cleanup;
    }

    ESP_LOGI(TAG, "history_async: adding %d history messages", batch->count);
    for (int i = 0; i < batch->count; i++) {
        bool is_user = (batch->msgs[i].role &&
                        strcmp(batch->msgs[i].role, "user") == 0);
        if (batch->msgs[i].text && batch->msgs[i].text[0]) {
            ui_chat_add_message(batch->msgs[i].text, is_user);
        }
    }

    /* Scroll to bottom after loading history */
    if (s_msg_scroll) {
        lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_OFF);
    }

    /* Hide the empty state hint if present */
    if (s_empty_hint && s_msg_count >= 2) {
        lv_obj_add_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    }

cleanup:
    for (int i = 0; i < batch->count; i++) {
        free(batch->msgs[i].role);
        free(batch->msgs[i].text);
    }
    free(batch->msgs);
    free(batch);
}

static void history_fetch_task(void *arg)
{
    (void)arg;

    char dhost[64] = {0};
    tab5_settings_get_dragon_host(dhost, sizeof(dhost));
    if (!dhost[0]) {
        ESP_LOGW(TAG, "history_fetch: no Dragon host configured");
        vTaskDelete(NULL);
        return;
    }

    char session_id[64] = {0};
    tab5_settings_get_session_id(session_id, sizeof(session_id));
    if (!session_id[0]) {
        ESP_LOGW(TAG, "history_fetch: no session_id in NVS");
        vTaskDelete(NULL);
        return;
    }

    /* Build URL: http://{dragon}:3502/api/v1/sessions/{id}/messages?limit=10 */
    char url[384];
    snprintf(url, sizeof(url),
             "http://%s:%d/api/v1/sessions/%s/messages?limit=%d",
             dhost, TAB5_OTA_PORT, session_id, HISTORY_FETCH_LIMIT);

    ESP_LOGI(TAG, "history_fetch: GET %s", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "history_fetch: http_client_init failed");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "history_fetch: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200 || content_len <= 0 || content_len > HISTORY_BODY_MAX) {
        ESP_LOGW(TAG, "history_fetch: bad response status=%d len=%d", status, content_len);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    char *body = malloc(content_len + 1);
    if (!body) {
        ESP_LOGE(TAG, "history_fetch: OOM for %d bytes", content_len);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int read_total = 0;
    while (read_total < content_len) {
        int r = esp_http_client_read(client, body + read_total, content_len - read_total);
        if (r <= 0) break;
        read_total += r;
    }
    body[read_total] = '\0';
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "history_fetch: got %d bytes", read_total);

    /* Parse JSON — expect {"items":[{"role":"user","content":"..."},...],...} */
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "history_fetch: JSON parse failed");
        vTaskDelete(NULL);
        return;
    }

    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        ESP_LOGW(TAG, "history_fetch: no 'items' array in response");
        cJSON_Delete(root);
        vTaskDelete(NULL);
        return;
    }

    int count = cJSON_GetArraySize(items);
    if (count <= 0) {
        ESP_LOGI(TAG, "history_fetch: no messages in session");
        cJSON_Delete(root);
        vTaskDelete(NULL);
        return;
    }

    /* Build batch for async delivery */
    history_batch_t *batch = malloc(sizeof(history_batch_t));
    if (!batch) { cJSON_Delete(root); vTaskDelete(NULL); return; }
    batch->msgs = calloc(count, sizeof(history_msg_t));
    if (!batch->msgs) { free(batch); cJSON_Delete(root); vTaskDelete(NULL); return; }
    batch->count = 0;

    for (int i = 0; i < count; i++) {
        cJSON *msg = cJSON_GetArrayItem(items, i);
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");

        /* Skip system/tool messages — only show user and assistant */
        if (!cJSON_IsString(role) || !cJSON_IsString(content)) continue;
        if (strcmp(role->valuestring, "user") != 0 &&
            strcmp(role->valuestring, "assistant") != 0) continue;
        if (!content->valuestring[0]) continue;

        batch->msgs[batch->count].role = strdup(role->valuestring);
        batch->msgs[batch->count].text = strdup(content->valuestring);
        batch->count++;
    }

    cJSON_Delete(root);

    if (batch->count > 0) {
        ESP_LOGI(TAG, "history_fetch: delivering %d messages to UI", batch->count);
        lv_async_call(history_async_cb, batch);
    } else {
        free(batch->msgs);
        free(batch);
    }

    vTaskDelete(NULL);
}

/** Kick off async history fetch if conversation is empty */
static void maybe_fetch_history(void)
{
    if (s_history_fetched) return;
    if (s_msg_count > 1) return;  /* Already have messages (beyond welcome) */
    if (!voice_is_connected()) return;  /* Dragon unreachable */

    s_history_fetched = true;
    ESP_LOGI(TAG, "Fetching conversation history from Dragon...");
    xTaskCreate(history_fetch_task, "chat_hist", 6144, NULL, 5, NULL);
}

static void enter_conversation(void)
{
    ESP_LOGI(TAG, "Entering conversation");
    s_in_conversation = true;

    if (s_home_panel) {
        lv_obj_add_flag(s_home_panel, LV_OBJ_FLAG_HIDDEN);
    }

    /* Build conversation UI if it hasn't been created yet */
    if (!s_conv_created) {
        build_conversation_ui();
    }

    /* Preserve pending text from home textarea (US-PR16) */
    if (s_home_textarea && s_conv_textarea) {
        const char *pending = lv_textarea_get_text(s_home_textarea);
        if (pending && pending[0]) {
            lv_textarea_set_text(s_conv_textarea, pending);
            lv_textarea_set_cursor_pos(s_conv_textarea, LV_TEXTAREA_CURSOR_LAST);
            lv_textarea_set_text(s_home_textarea, "");
            ESP_LOGI(TAG, "Transferred pending text to conversation: '%s'", pending);
        }
    }

    if (s_conv_panel) {
        lv_obj_clear_flag(s_conv_panel, LV_OBJ_FLAG_HIDDEN);
    }

    /* Swap active textarea to conversation panel */
    s_textarea = s_conv_textarea;

    update_mode_badge();
    update_status_bar();

    /* Load history from Dragon if conversation is empty (e.g. after reboot) */
    maybe_fetch_history();
}

/* ── Build Chat Home panel ─────────────────────────────────────── */

static void build_home_panel(void)
{
    ESP_LOGI(TAG, "Building Chat Home panel");
    feed_wdt_yield();

    s_home_panel = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_home_panel);
    lv_obj_set_size(s_home_panel, 720, 1280);
    lv_obj_set_pos(s_home_panel, 0, 0);
    lv_obj_set_style_bg_color(s_home_panel, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(s_home_panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_home_panel, LV_OBJ_FLAG_SCROLLABLE);

    int y = 0;

    /* ── Top bar (60px) ──────────────────────────────────────── */
    /* Back arrow button */
    lv_obj_t *back_btn = lv_button_create(s_home_panel);
    lv_obj_set_size(back_btn, 80, 44);
    lv_obj_set_pos(back_btn, 12, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, cb_close, LV_EVENT_CLICKED, NULL);
    ui_fb_button(back_btn);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(back_lbl);

    /* "Conversations" title */
    lv_obj_t *title = lv_label_create(s_home_panel);
    lv_label_set_text(title, "Conversations");
    lv_obj_set_pos(title, 110, 16);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    /* Mode badge — tappable */
    s_home_mode_badge = lv_label_create(s_home_panel);
    lv_obj_set_pos(s_home_mode_badge, 460, 12);
    lv_obj_set_style_text_font(s_home_mode_badge, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_opa(s_home_mode_badge, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_home_mode_badge, 12, 0);
    lv_obj_set_style_pad_hor(s_home_mode_badge, 12, 0);
    lv_obj_set_style_pad_ver(s_home_mode_badge, 4, 0);
    lv_obj_add_flag(s_home_mode_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_home_mode_badge, 10);
    lv_obj_add_event_cb(s_home_mode_badge, cb_mode_cycle, LV_EVENT_CLICKED, NULL);

    /* "+ New" button */
    lv_obj_t *new_btn = lv_button_create(s_home_panel);
    lv_obj_set_size(new_btn, 100, 36);
    lv_obj_set_pos(new_btn, 600, 12);
    lv_obj_set_style_bg_color(new_btn, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_bg_opa(new_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(new_btn, 18, 0);
    lv_obj_set_style_border_width(new_btn, 0, 0);
    lv_obj_add_event_cb(new_btn, cb_new_chat_card, LV_EVENT_CLICKED, NULL);
    ui_fb_button(new_btn);

    lv_obj_t *new_lbl = lv_label_create(new_btn);
    lv_label_set_text(new_lbl, LV_SYMBOL_PLUS " New");
    lv_obj_set_style_text_color(new_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(new_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(new_lbl);

    /* Topbar separator */
    lv_obj_t *sep1 = lv_obj_create(s_home_panel);
    lv_obj_remove_style_all(sep1);
    lv_obj_set_size(sep1, 720, 1);
    lv_obj_set_pos(sep1, 0, TOPBAR_H - 1);
    lv_obj_set_style_bg_color(sep1, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_COVER, 0);

    y = TOPBAR_H;

    feed_wdt_yield();

    /* ── Model Bar (44px) ────────────────────────────────────── */
    s_model_lbl = lv_label_create(s_home_panel);
    lv_label_set_text(s_model_lbl, "");  /* Set by update_model_label() below */
    lv_obj_set_pos(s_model_lbl, 20, y + 10);
    lv_obj_set_style_text_color(s_model_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(s_model_lbl, &lv_font_montserrat_16, 0);
    /* Make model label tappable — cycles models within current mode */
    lv_obj_add_flag(s_model_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_model_lbl, 10);
    lv_obj_add_event_cb(s_model_lbl, cb_model_cycle, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_opa(s_model_lbl, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_model_lbl, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    update_model_label();

    /* Status dot + label on right */
    s_home_status_dot = lv_obj_create(s_home_panel);
    lv_obj_remove_style_all(s_home_status_dot);
    lv_obj_set_size(s_home_status_dot, 8, 8);
    lv_obj_set_pos(s_home_status_dot, 560, y + 16);
    lv_obj_set_style_bg_color(s_home_status_dot, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_bg_opa(s_home_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_home_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_home_status_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_home_status_lbl = lv_label_create(s_home_panel);
    lv_label_set_text(s_home_status_lbl, "Ready");
    lv_obj_set_pos(s_home_status_lbl, 574, y + 12);
    lv_obj_set_style_text_color(s_home_status_lbl, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(s_home_status_lbl, &lv_font_montserrat_14, 0);

    /* Model bar separator */
    lv_obj_t *sep2 = lv_obj_create(s_home_panel);
    lv_obj_remove_style_all(sep2);
    lv_obj_set_size(sep2, 680, 1);
    lv_obj_set_pos(sep2, 20, y + 43);
    lv_obj_set_style_bg_color(sep2, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);

    y += 44;

    /* ── Quick Actions Bar (64px) ────────────────────────────── */
    int qa_y = y + 8;
    int qa_x = 20;
    int qa_w = 200;
    int qa_gap = 16;

    /* Web search */
    lv_obj_t *qa_web = lv_button_create(s_home_panel);
    lv_obj_set_size(qa_web, qa_w, 48);
    lv_obj_set_pos(qa_web, qa_x, qa_y);
    lv_obj_set_style_bg_color(qa_web, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_radius(qa_web, 24, 0);
    lv_obj_set_style_border_width(qa_web, 1, 0);
    lv_obj_set_style_border_color(qa_web, lv_color_hex(0x333350), 0);
    lv_obj_set_style_pad_hor(qa_web, 12, 0);
    lv_obj_add_event_cb(qa_web, cb_quick_web, LV_EVENT_CLICKED, NULL);
    ui_fb_button(qa_web);
    lv_obj_t *qa_web_lbl = lv_label_create(qa_web);
    lv_label_set_text(qa_web_lbl, LV_SYMBOL_WIFI " Web");
    lv_obj_set_style_text_color(qa_web_lbl, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(qa_web_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(qa_web_lbl);

    /* Timer */
    lv_obj_t *qa_timer = lv_button_create(s_home_panel);
    lv_obj_set_size(qa_timer, qa_w, 48);
    lv_obj_set_pos(qa_timer, qa_x + qa_w + qa_gap, qa_y);
    lv_obj_set_style_bg_color(qa_timer, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_radius(qa_timer, 24, 0);
    lv_obj_set_style_border_width(qa_timer, 1, 0);
    lv_obj_set_style_border_color(qa_timer, lv_color_hex(0x333350), 0);
    lv_obj_set_style_pad_hor(qa_timer, 12, 0);
    lv_obj_add_event_cb(qa_timer, cb_quick_timer, LV_EVENT_CLICKED, NULL);
    ui_fb_button(qa_timer);
    lv_obj_t *qa_timer_lbl = lv_label_create(qa_timer);
    lv_label_set_text(qa_timer_lbl, LV_SYMBOL_BELL " Timer");
    lv_obj_set_style_text_color(qa_timer_lbl, lv_color_hex(CLR_AMBER), 0);
    lv_obj_set_style_text_font(qa_timer_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(qa_timer_lbl);

    /* Remember */
    lv_obj_t *qa_mem = lv_button_create(s_home_panel);
    lv_obj_set_size(qa_mem, 720 - 2 * qa_x - 2 * (qa_w + qa_gap), 48);
    lv_obj_set_pos(qa_mem, qa_x + 2 * (qa_w + qa_gap), qa_y);
    lv_obj_set_style_bg_color(qa_mem, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_radius(qa_mem, 24, 0);
    lv_obj_set_style_border_width(qa_mem, 1, 0);
    lv_obj_set_style_border_color(qa_mem, lv_color_hex(0x333350), 0);
    lv_obj_set_style_pad_hor(qa_mem, 12, 0);
    lv_obj_add_event_cb(qa_mem, cb_quick_remember, LV_EVENT_CLICKED, NULL);
    ui_fb_button(qa_mem);
    lv_obj_t *qa_mem_lbl = lv_label_create(qa_mem);
    lv_label_set_text(qa_mem_lbl, LV_SYMBOL_EYE_OPEN " Memory");
    lv_obj_set_style_text_color(qa_mem_lbl, lv_color_hex(0xBF5AF2), 0);
    lv_obj_set_style_text_font(qa_mem_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(qa_mem_lbl);

    y += 64;

    feed_wdt_yield();

    /* ── Session Cards (scrollable area) ─────────────────────── */
    /* Available height: 1280 - topbar(60) - model(44) - quickactions(64) - memory(48) - input(80) = 984 */
    int cards_h = 1280 - TOPBAR_H - 44 - 64 - 48 - INPUT_BAR_H;  /* 984 */

    lv_obj_t *card_scroll = lv_obj_create(s_home_panel);
    lv_obj_remove_style_all(card_scroll);
    lv_obj_set_size(card_scroll, 720, cards_h);
    lv_obj_set_pos(card_scroll, 0, y);
    lv_obj_set_style_bg_opa(card_scroll, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(card_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(card_scroll, LV_DIR_VER);

    int card_y = 12;
    int card_w = 680;
    int card_x = 20;

    /* ── Card 1: Current Session ──────────────────────────────── */
    lv_obj_t *card1 = lv_obj_create(card_scroll);
    lv_obj_remove_style_all(card1);
    lv_obj_set_size(card1, card_w, 120);
    lv_obj_set_pos(card1, card_x, card_y);
    lv_obj_set_style_bg_color(card1, lv_color_hex(CLR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card1, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card1, 16, 0);
    lv_obj_set_style_border_width(card1, 1, 0);
    lv_obj_set_style_border_color(card1, lv_color_hex(CLR_CARD_BORDER), 0);
    lv_obj_set_style_pad_all(card1, 16, 0);
    lv_obj_clear_flag(card1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card1, cb_current_session, LV_EVENT_CLICKED, NULL);
    ui_fb_card(card1);

    /* Card 1: header row */
    lv_obj_t *card1_title = lv_label_create(card1);
    lv_label_set_text(card1_title, "Current Conversation");
    lv_obj_set_pos(card1_title, 0, 0);
    lv_obj_set_style_text_color(card1_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(card1_title, &lv_font_montserrat_18, 0);

    /* Card 1: time + arrow */
    char time_buf[8] = "";
    get_time_str(time_buf, sizeof(time_buf));
    lv_obj_t *card1_meta = lv_label_create(card1);
    lv_label_set_text_fmt(card1_meta, "%s  " LV_SYMBOL_RIGHT, time_buf[0] ? time_buf : "Now");
    lv_obj_set_pos(card1_meta, 520, 0);
    lv_obj_set_style_text_color(card1_meta, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(card1_meta, &lv_font_montserrat_14, 0);

    /* Card 1: message preview */
    s_current_card_preview = lv_label_create(card1);
    {
        const char *llm = voice_get_llm_text();
        if (llm && llm[0]) {
            char preview[80];
            strip_tool_tags(llm, preview, sizeof(preview));
            if (strlen(preview) > 60) {
                preview[57] = '.';
                preview[58] = '.';
                preview[59] = '.';
                preview[60] = '\0';
            }
            lv_label_set_text(s_current_card_preview, preview[0] ? preview : "Tap to continue chatting...");
        } else {
            lv_label_set_text(s_current_card_preview, "Tap to continue chatting...");
        }
    }
    lv_obj_set_pos(s_current_card_preview, 0, 36);
    lv_obj_set_width(s_current_card_preview, card_w - 40);
    lv_label_set_long_mode(s_current_card_preview, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_current_card_preview, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_current_card_preview, &lv_font_montserrat_16, 0);

    /* Card 1: message count indicator */
    lv_obj_t *card1_count = lv_label_create(card1);
    if (s_msg_count > 0) {
        lv_label_set_text_fmt(card1_count, "%d message%s", s_msg_count, s_msg_count == 1 ? "" : "s");
    } else {
        lv_label_set_text(card1_count, "Tap to chat");
    }
    lv_obj_set_pos(card1_count, 0, 66);
    lv_obj_set_style_text_color(card1_count, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(card1_count, &lv_font_montserrat_14, 0);

    card_y += 120 + 12;

    /* ── Card 2: New Chat ─────────────────────────────────────── */
    lv_obj_t *card2 = lv_obj_create(card_scroll);
    lv_obj_remove_style_all(card2);
    lv_obj_set_size(card2, card_w, 90);
    lv_obj_set_pos(card2, card_x, card_y);
    lv_obj_set_style_bg_color(card2, lv_color_hex(CLR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card2, 16, 0);
    lv_obj_set_style_border_width(card2, 1, 0);
    lv_obj_set_style_border_color(card2, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_border_opa(card2, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(card2, 16, 0);
    lv_obj_clear_flag(card2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card2, cb_new_chat_card, LV_EVENT_CLICKED, NULL);
    ui_fb_card(card2);

    lv_obj_t *card2_icon = lv_label_create(card2);
    lv_label_set_text(card2_icon, LV_SYMBOL_PLUS);
    lv_obj_set_pos(card2_icon, 0, 8);
    lv_obj_set_style_text_color(card2_icon, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(card2_icon, &lv_font_montserrat_24, 0);

    lv_obj_t *card2_title = lv_label_create(card2);
    lv_label_set_text(card2_title, "Start New Chat");
    lv_obj_set_pos(card2_title, 40, 4);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_20, 0);

    lv_obj_t *card2_desc = lv_label_create(card2);
    lv_label_set_text(card2_desc, "Fresh conversation with Tinker");
    lv_obj_set_pos(card2_desc, 40, 32);
    lv_obj_set_style_text_color(card2_desc, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(card2_desc, &lv_font_montserrat_16, 0);

    card_y += 90 + 12;

    /* ── Suggestion cards ─────────────────────────────────────── */
    /* "What can Tinker do?" hint area */
    lv_obj_t *hint_title = lv_label_create(card_scroll);
    lv_label_set_text(hint_title, "Try asking...");
    lv_obj_set_pos(hint_title, card_x, card_y + 8);
    lv_obj_set_style_text_color(hint_title, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(hint_title, &lv_font_montserrat_16, 0);

    card_y += 36;

    const char *suggestions[] = {
        "\"What's the weather like today?\"",
        "\"Search for the latest tech news\"",
        "\"Set a 10 minute timer\"",
        "\"Remember that I like sushi\"",
    };
    const uint32_t sug_colors[] = {0x06B6D4, 0x22C55E, 0xF5A623, 0xBF5AF2};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *sug = lv_obj_create(card_scroll);
        lv_obj_remove_style_all(sug);
        lv_obj_set_size(sug, card_w, 56);
        lv_obj_set_pos(sug, card_x, card_y);
        lv_obj_set_style_bg_color(sug, lv_color_hex(CLR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(sug, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sug, 12, 0);
        lv_obj_set_style_border_width(sug, 1, 0);
        lv_obj_set_style_border_color(sug, lv_color_hex(0x222230), 0);
        lv_obj_set_style_pad_left(sug, 16, 0);
        lv_obj_set_style_pad_ver(sug, 12, 0);
        lv_obj_clear_flag(sug, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sug, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sug, cb_suggestion, LV_EVENT_CLICKED, NULL);
        ui_fb_card(sug);

        lv_obj_t *sug_lbl = lv_label_create(sug);
        lv_label_set_text(sug_lbl, suggestions[i]);
        lv_obj_set_style_text_color(sug_lbl, lv_color_hex(sug_colors[i]), 0);
        lv_obj_set_style_text_font(sug_lbl, &lv_font_montserrat_18, 0);
        lv_obj_align(sug_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        card_y += 56 + 12;
    }

    feed_wdt_yield();

    /* ── Memory Preview Bar (48px) ───────────────────────────── */
    int mem_y = 1280 - INPUT_BAR_H - 48;
    lv_obj_t *mem_bar = lv_obj_create(s_home_panel);
    lv_obj_remove_style_all(mem_bar);
    lv_obj_set_size(mem_bar, 720, 48);
    lv_obj_set_pos(mem_bar, 0, mem_y);
    lv_obj_set_style_bg_color(mem_bar, lv_color_hex(0x0D0D18), 0);
    lv_obj_set_style_bg_opa(mem_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(mem_bar, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(mem_bar, 1, 0);
    lv_obj_set_style_border_side(mem_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_clear_flag(mem_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_memory_lbl = lv_label_create(mem_bar);
    lv_label_set_text(s_memory_lbl, LV_SYMBOL_EYE_OPEN "  Tinker remembers your preferences");
    lv_obj_set_pos(s_memory_lbl, 20, 0);
    lv_obj_set_style_text_color(s_memory_lbl, lv_color_hex(0x777777), 0);
    lv_obj_set_style_text_font(s_memory_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_memory_lbl, LV_ALIGN_LEFT_MID, 16, 0);

    /* ── Input Bar (80px) — shared with conversation ──────────── */
    int input_y = 1280 - INPUT_BAR_H;

    lv_obj_t *bar = lv_obj_create(s_home_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, INPUT_BAR_H);
    lv_obj_set_pos(bar, 0, input_y);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_home_input_bar = bar;

    /* Mic button */
    lv_obj_t *mic_btn = lv_button_create(bar);
    lv_obj_set_size(mic_btn, 48, 48);
    lv_obj_set_pos(mic_btn, 12, 16);
    lv_obj_set_style_bg_color(mic_btn, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_radius(mic_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mic_btn, 0, 0);
    lv_obj_add_event_cb(mic_btn, cb_mic, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mic_icon = lv_label_create(mic_btn);
    lv_label_set_text(mic_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(mic_icon, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(mic_icon, &lv_font_montserrat_24, 0);
    lv_obj_center(mic_icon);

    /* Text input */
    s_home_textarea = lv_textarea_create(bar);
    lv_obj_set_size(s_home_textarea, 440, 48);
    lv_obj_set_pos(s_home_textarea, 72, 16);
    lv_textarea_set_placeholder_text(s_home_textarea, "Type a message...");
    lv_textarea_set_one_line(s_home_textarea, true);
    lv_obj_set_style_bg_color(s_home_textarea, lv_color_hex(CLR_INPUT_BG), 0);
    lv_obj_set_style_text_color(s_home_textarea, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_home_textarea, &lv_font_montserrat_18, 0);
    lv_obj_set_style_border_color(s_home_textarea, lv_color_hex(CLR_INPUT_BORDER), 0);
    lv_obj_set_style_border_width(s_home_textarea, 1, 0);
    lv_obj_set_style_radius(s_home_textarea, 24, 0);
    lv_obj_set_style_pad_left(s_home_textarea, 20, 0);
    lv_obj_set_style_pad_right(s_home_textarea, 12, 0);
    lv_obj_set_style_pad_top(s_home_textarea, 8, 0);
    lv_obj_set_style_pad_bottom(s_home_textarea, 8, 0);
    lv_obj_set_style_text_color(s_home_textarea, lv_color_hex(0x666666), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_flag(s_home_textarea, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_home_textarea, cb_textarea_click, LV_EVENT_CLICKED, NULL);

    /* Active textarea starts as home textarea */
    s_textarea = s_home_textarea;

    /* Send button */
    lv_obj_t *send_btn = lv_button_create(bar);
    lv_obj_set_size(send_btn, 100, 48);
    lv_obj_set_pos(send_btn, 524, 16);
    lv_obj_set_style_bg_color(send_btn, lv_color_hex(CLR_AMBER), 0);
    lv_obj_set_style_radius(send_btn, 24, 0);
    lv_obj_set_style_border_width(send_btn, 0, 0);
    lv_obj_add_event_cb(send_btn, cb_send, LV_EVENT_CLICKED, NULL);

    lv_obj_t *send_lbl = lv_label_create(send_btn);
    lv_label_set_text(send_lbl, "Send");
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(send_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(send_lbl);

    update_mode_badge_obj(s_home_mode_badge);
    update_status_bar();

    ESP_LOGI(TAG, "Chat Home panel built");
}

/* ── Build Conversation panel (existing chat UI) ───────────────── */

static void build_conversation_ui(void)
{
    if (s_conv_created) return;

    ESP_LOGI(TAG, "Building conversation panel");
    feed_wdt_yield();

    s_conv_panel = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_conv_panel);
    lv_obj_set_size(s_conv_panel, 720, 1280);
    lv_obj_set_pos(s_conv_panel, 0, 0);
    lv_obj_set_style_bg_color(s_conv_panel, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(s_conv_panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_conv_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Start hidden — enter_conversation() will show it */
    lv_obj_add_flag(s_conv_panel, LV_OBJ_FLAG_HIDDEN);

    /* ── Top bar (60px) ──────────────────────────────────────── */
    /* Back arrow button — goes to Chat Home */
    lv_obj_t *back_btn = lv_button_create(s_conv_panel);
    lv_obj_set_size(back_btn, 80, 44);
    lv_obj_set_pos(back_btn, 12, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, cb_close, LV_EVENT_CLICKED, NULL);
    ui_fb_button(back_btn);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(back_lbl);

    /* "Chat" title */
    lv_obj_t *title = lv_label_create(s_conv_panel);
    lv_label_set_text(title, "Chat");
    lv_obj_set_pos(title, 110, 8);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    /* Connection dot + status text */
    s_status_dot = lv_obj_create(s_conv_panel);
    lv_obj_remove_style_all(s_status_dot);
    lv_obj_set_size(s_status_dot, 8, 8);
    lv_obj_set_pos(s_status_dot, 112, 36);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_status_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_status_lbl = lv_label_create(s_conv_panel);
    lv_label_set_text(s_status_lbl, "Ready");
    lv_obj_set_pos(s_status_lbl, 126, 32);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);

    /* New chat button */
    lv_obj_t *new_btn = lv_button_create(s_conv_panel);
    lv_obj_set_size(new_btn, 36, 36);
    lv_obj_set_pos(new_btn, 400, 12);
    lv_obj_set_style_bg_color(new_btn, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_bg_opa(new_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(new_btn, 8, 0);
    lv_obj_set_style_border_width(new_btn, 0, 0);
    lv_obj_add_event_cb(new_btn, cb_new_chat, LV_EVENT_CLICKED, NULL);
    ui_fb_button(new_btn);
    lv_obj_t *new_lbl = lv_label_create(new_btn);
    lv_label_set_text(new_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(new_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(new_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(new_lbl);

    /* Mode badge */
    s_mode_badge = lv_label_create(s_conv_panel);
    lv_obj_set_pos(s_mode_badge, 540, 12);
    lv_obj_set_style_text_font(s_mode_badge, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_opa(s_mode_badge, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_mode_badge, 12, 0);
    lv_obj_set_style_pad_hor(s_mode_badge, 12, 0);
    lv_obj_set_style_pad_ver(s_mode_badge, 4, 0);
    lv_obj_add_flag(s_mode_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_mode_badge, 10);
    lv_obj_add_event_cb(s_mode_badge, cb_mode_cycle, LV_EVENT_CLICKED, NULL);

    /* Topbar separator line */
    lv_obj_t *sep = lv_obj_create(s_conv_panel);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 720, 1);
    lv_obj_set_pos(sep, 0, TOPBAR_H - 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    feed_wdt_yield();

    /* ── Message scroll area ────────────────────────────────── */
    s_msg_scroll = lv_obj_create(s_conv_panel);
    lv_obj_remove_style_all(s_msg_scroll);
    lv_obj_set_size(s_msg_scroll, 720, MSG_AREA_H);
    lv_obj_set_pos(s_msg_scroll, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(s_msg_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_msg_scroll, 0, 0);
    lv_obj_add_flag(s_msg_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_msg_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_msg_scroll, LV_DIR_VER);

    s_next_y = 12;

    /* ── Input bar (80px) ───────────────────────────────────── */
    lv_obj_t *bar = lv_obj_create(s_conv_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, INPUT_BAR_H);
    lv_obj_set_pos(bar, 0, 1280 - INPUT_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    s_conv_input_bar = bar;
    lv_obj_set_style_border_color(bar, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Mic button */
    lv_obj_t *mic_btn = lv_button_create(bar);
    lv_obj_set_size(mic_btn, 48, 48);
    lv_obj_set_pos(mic_btn, 12, 16);
    lv_obj_set_style_bg_color(mic_btn, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_radius(mic_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mic_btn, 0, 0);
    lv_obj_add_event_cb(mic_btn, cb_mic, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mic_icon = lv_label_create(mic_btn);
    lv_label_set_text(mic_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(mic_icon, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(mic_icon, &lv_font_montserrat_24, 0);
    lv_obj_center(mic_icon);

    /* Conversation textarea */
    s_conv_textarea = lv_textarea_create(bar);
    lv_obj_set_size(s_conv_textarea, 440, 48);
    lv_obj_set_pos(s_conv_textarea, 72, 16);
    lv_textarea_set_placeholder_text(s_conv_textarea, "Type a message...");
    lv_textarea_set_one_line(s_conv_textarea, true);
    lv_obj_set_style_bg_color(s_conv_textarea, lv_color_hex(CLR_INPUT_BG), 0);
    lv_obj_set_style_text_color(s_conv_textarea, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_conv_textarea, &lv_font_montserrat_18, 0);
    lv_obj_set_style_border_color(s_conv_textarea, lv_color_hex(CLR_INPUT_BORDER), 0);
    lv_obj_set_style_border_width(s_conv_textarea, 1, 0);
    lv_obj_set_style_radius(s_conv_textarea, 24, 0);
    lv_obj_set_style_pad_left(s_conv_textarea, 20, 0);
    lv_obj_set_style_pad_right(s_conv_textarea, 12, 0);
    lv_obj_set_style_pad_top(s_conv_textarea, 8, 0);
    lv_obj_set_style_pad_bottom(s_conv_textarea, 8, 0);
    lv_obj_set_style_text_color(s_conv_textarea, lv_color_hex(0x666666), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_flag(s_conv_textarea, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_conv_textarea, cb_textarea_click, LV_EVENT_CLICKED, NULL);

    /* Send button */
    lv_obj_t *send_btn = lv_button_create(bar);
    lv_obj_set_size(send_btn, 100, 48);
    lv_obj_set_pos(send_btn, 524, 16);
    lv_obj_set_style_bg_color(send_btn, lv_color_hex(CLR_AMBER), 0);
    lv_obj_set_style_radius(send_btn, 24, 0);
    lv_obj_set_style_border_width(send_btn, 0, 0);
    lv_obj_add_event_cb(send_btn, cb_send, LV_EVENT_CLICKED, NULL);

    lv_obj_t *send_lbl = lv_label_create(send_btn);
    lv_label_set_text(send_lbl, "Send");
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(send_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(send_lbl);

    /* Welcome message — smaller and dimmer so real messages stand out */
    {
        lv_obj_t *wb = lv_obj_create(s_msg_scroll);
        lv_obj_remove_style_all(wb);
        lv_obj_set_pos(wb, 20, s_next_y);
        lv_obj_set_size(wb, BUBBLE_MAX_W, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(wb, lv_color_hex(CLR_TINKER_BUB), 0);
        lv_obj_set_style_bg_opa(wb, LV_OPA_60, 0);
        lv_obj_set_style_radius(wb, 16, 0);
        lv_obj_set_style_pad_all(wb, 12, 0);
        lv_obj_set_style_border_width(wb, 1, 0);
        lv_obj_set_style_border_color(wb, lv_color_hex(0x333333), 0);
        lv_obj_clear_flag(wb, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *wl = lv_label_create(wb);
        lv_label_set_text(wl, "Hi! I'm Tinker. Type or tap the mic to chat.");
        lv_label_set_long_mode(wl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(wl, LABEL_MAX_W);
        lv_obj_set_style_text_font(wl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(wl, lv_color_hex(0x666666), 0);

        lv_obj_update_layout(wb);
        s_next_y += lv_obj_get_height(wb) + BUBBLE_GAP;
        s_msg_count++;
    }

    /* Empty state hint */
    s_empty_hint = lv_label_create(s_msg_scroll);
    lv_label_set_text(s_empty_hint, "Ask me anything!\nI can search the web,\ndo math, check weather,\nand remember things.");
    lv_obj_set_style_text_color(s_empty_hint, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(s_empty_hint, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_empty_hint, 120, 300);

    s_conv_created = true;

    ESP_LOGI(TAG, "Conversation panel built");
}

/* ── Public API ────────────────────────────────────────────────── */

lv_obj_t *ui_chat_create(void)
{
    if (s_active) return s_overlay;

    /* Re-show existing overlay (preserves messages) */
    if (s_overlay) {
        ESP_LOGI(TAG, "Restoring chat (%d messages)", s_msg_count);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(s_overlay);
        s_active = true;
        s_last_state = voice_get_state();
        /* Resume existing poll timer (don't create new — causes timer leak) */
        if (s_poll_timer) lv_timer_resume(s_poll_timer);
        else s_poll_timer = lv_timer_create(poll_voice_cb, 200, NULL);
        update_mode_badge();
        update_status_bar();
        ui_keyboard_set_layout_cb(chat_keyboard_layout_cb);

        /* Always show Chat Home on re-open */
        if (s_in_conversation) {
            show_chat_home();
        }
        return s_overlay;
    }

    ESP_LOGI(TAG, "Creating chat overlay");
    feed_wdt_yield();

    /* ── Fullscreen overlay on home screen ──────────────────── */
    lv_obj_t *parent = ui_home_get_screen();
    if (!parent) parent = lv_screen_active();

    s_overlay = lv_obj_create(parent);
    lv_obj_set_size(s_overlay, 720, 1280);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_overlay);

    /* Track touch-down position + swipe-right to close / go back (left-edge only) */
    lv_obj_add_event_cb(s_overlay, cb_touch_down, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_overlay, cb_close, LV_EVENT_GESTURE, NULL);

    feed_wdt_yield();

    /* ── Build both panels ─────────────────────────────────── */
    build_home_panel();

    feed_wdt_yield();

    /* Conversation panel is built lazily on first enter */
    /* s_conv_panel created in build_conversation_ui() */

    /* ── Done ───────────────────────────────────────────────── */
    s_active = true;
    s_in_conversation = false;
    s_last_state = voice_get_state();
    ui_keyboard_set_layout_cb(chat_keyboard_layout_cb);

    /* Start polling for streaming LLM responses + status updates */
    s_poll_timer = lv_timer_create(poll_voice_cb, 200, NULL);

    ESP_LOGI(TAG, "Chat overlay ready — showing Chat Home");
    return s_overlay;
}

void ui_chat_add_message(const char *text, bool is_user)
{
    if (!s_msg_scroll || !text || !text[0]) return;

    /* Enforce max message count -- delete oldest */
    if (s_msg_count >= MAX_MESSAGES) {
        /* Show "older messages archived" notification once (US-PR22) */
        if (!s_archived_shown && s_msg_scroll) {
            s_archived_lbl = lv_label_create(s_msg_scroll);
            lv_label_set_text(s_archived_lbl, "Older messages archived");
            lv_obj_set_style_text_color(s_archived_lbl, lv_color_hex(0x555555), 0);
            lv_obj_set_style_text_font(s_archived_lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_align(s_archived_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(s_archived_lbl, 720 - 40);
            lv_obj_set_pos(s_archived_lbl, 20, 0);
            s_archived_shown = true;
        }

        /* Delete oldest message bubble (skip archived label at index 0) */
        uint32_t start_idx = s_archived_shown ? 1 : 0;
        lv_obj_t *oldest = lv_obj_get_child(s_msg_scroll, start_idx);
        if (oldest) {
            lv_obj_del(oldest);
            s_msg_count--;
            /* Recalculate s_next_y from remaining children */
            int base_y = s_archived_shown ? 24 : 12;
            s_next_y = base_y;
            uint32_t cnt = lv_obj_get_child_count(s_msg_scroll);
            for (uint32_t i = (s_archived_shown ? 1 : 0); i < cnt; i++) {
                lv_obj_t *ch = lv_obj_get_child(s_msg_scroll, i);
                lv_obj_set_pos(ch, lv_obj_get_x(ch), s_next_y);
                lv_obj_update_layout(ch);
                s_next_y += lv_obj_get_height(ch) + BUBBLE_GAP;
            }
        }
    }

    /* Strip <tool> tags from display text */
    char stripped[1024];
    strip_tool_tags(text, stripped, sizeof(stripped));
    if (!stripped[0]) return;

    /* Create bubble container */
    lv_obj_t *bubble = lv_obj_create(s_msg_scroll);
    lv_obj_remove_style_all(bubble);
    int x_pos = is_user ? (720 - BUBBLE_MAX_W - 20) : 20;   /* right or left aligned */
    lv_obj_set_pos(bubble, x_pos, s_next_y);
    lv_obj_set_size(bubble, BUBBLE_MAX_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(is_user ? CLR_USER_BUB : CLR_TINKER_BUB), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_all(bubble, BUBBLE_PAD, 0);
    lv_obj_set_style_pad_bottom(bubble, 8, 0);  /* tighter bottom for timestamp */
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    /* Tinker bubbles get a visible border for OLED contrast */
    if (!is_user) {
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(0x333333), 0);
    }

    /* Label inside bubble */
    lv_obj_t *lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, stripped);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LABEL_MAX_W);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(is_user ? 0x000000 : 0xFFFFFF), 0);

    /* Timestamp label — right-aligned inside bubble, below message text */
    char time_buf[8];
    if (get_time_str(time_buf, sizeof(time_buf))) {
        lv_obj_t *ts = lv_label_create(bubble);
        lv_label_set_text(ts, time_buf);
        lv_obj_set_style_text_font(ts, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ts, lv_color_hex(0x888888), 0);
        lv_obj_set_width(ts, LABEL_MAX_W);
        lv_obj_set_style_text_align(ts, LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* Force layout so we can measure height */
    lv_obj_update_layout(bubble);
    int bh = lv_obj_get_height(bubble);

    s_next_y += bh + BUBBLE_GAP;
    s_msg_count++;

    /* Hide empty state hint once a real conversation starts */
    if (s_empty_hint && s_msg_count >= 2) {
        lv_obj_add_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    }

    /* Auto-scroll to bottom */
    lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_ON);
}

void ui_chat_hide(void)
{
    if (!s_overlay) return;
    ui_keyboard_set_layout_cb(NULL);
    ui_keyboard_hide();
    /* Delete poll timer — pausing alone is insufficient because the timer
     * can fire between pause call and LVGL processing it. Timer recreated on show. */
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    s_active = false;
    ESP_LOGI(TAG, "Chat hidden (nav)");
}

void ui_chat_destroy(void)
{
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
    ui_keyboard_set_layout_cb(NULL);
    ui_keyboard_hide();

    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }

    /* Reset all state */
    s_home_panel    = NULL;
    s_conv_panel    = NULL;
    s_msg_scroll    = NULL;
    s_textarea      = NULL;
    s_home_textarea = NULL;
    s_conv_textarea = NULL;
    s_home_input_bar = NULL;
    s_conv_input_bar = NULL;
    s_mode_badge    = NULL;
    s_home_mode_badge = NULL;
    s_model_lbl     = NULL;
    s_memory_lbl    = NULL;
    s_home_status_dot = NULL;
    s_home_status_lbl = NULL;
    s_current_card_preview = NULL;
    s_assist_bubble = NULL;
    s_assist_label  = NULL;
    s_tool_label    = NULL;
    s_typing_lbl    = NULL;
    s_empty_hint    = NULL;
    s_archived_lbl  = NULL;
    s_archived_shown = false;
    s_status_lbl    = NULL;
    s_status_dot    = NULL;
    s_active        = false;
    s_in_conversation = false;
    s_conv_created  = false;
    s_msg_count     = 0;
    s_next_y        = 0;
    s_last_state    = VOICE_STATE_IDLE;
    s_history_fetched = false;

    ESP_LOGI(TAG, "Chat destroyed");
}

/* ── Thread-safe push from voice task ─────────────────────────── */

typedef struct {
    char *role;   /* "user" or "assistant" — heap-allocated copy */
    char *text;   /* message text — heap-allocated copy */
} chat_push_msg_t;

static void async_push_cb(void *arg)
{
    chat_push_msg_t *msg = (chat_push_msg_t *)arg;
    if (!msg) return;

    bool is_user = (msg->role && strcmp(msg->role, "user") == 0);

    /* Ensure conversation UI exists so ui_chat_add_message has s_msg_scroll.
     * If the overlay hasn't been created yet, build it now (lazy init). */
    if (!s_overlay) {
        /* Overlay not created — can't show messages yet. Queue is lost.
         * This is acceptable: the user hasn't opened Chat yet, so they
         * won't see a missing message. The voice overlay shows results
         * immediately, so the user still gets feedback. */
        ESP_LOGW(TAG, "push_message: overlay not created yet, dropping: %.40s...",
                 msg->text ? msg->text : "(null)");
        free(msg->role);
        free(msg->text);
        free(msg);
        return;
    }

    /* Dedup assistant messages: if the conversation view was active during
     * this LLM response, poll_voice_cb already created and streamed the
     * assistant bubble. Adding another would duplicate it.
     * User messages from voice STT are always safe — no existing mechanism
     * adds them to chat when speaking via the voice overlay. */
    if (!is_user && s_in_conversation && s_conv_created) {
        /* Conversation was visible — poll already handled this response */
        ESP_LOGD(TAG, "push_message: skipping assistant (conv visible, poll handled)");
        free(msg->role);
        free(msg->text);
        free(msg);
        return;
    }

    /* Build conversation panel if needed (lazy — same as enter_conversation) */
    if (!s_conv_created) {
        build_conversation_ui();
    }

    if (msg->text && msg->text[0]) {
        ui_chat_add_message(msg->text, is_user);
    }

    free(msg->role);
    free(msg->text);
    free(msg);
}

void ui_chat_push_message(const char *role, const char *text)
{
    if (!text || !text[0]) return;

    /* Allocate copies on the heap — caller's buffers (s_stt_text, s_llm_text)
     * are static and get overwritten on the next voice interaction. */
    chat_push_msg_t *msg = malloc(sizeof(chat_push_msg_t));
    if (!msg) {
        ESP_LOGE(TAG, "push_message: OOM for msg struct");
        return;
    }
    msg->role = strdup(role ? role : "assistant");
    msg->text = strdup(text);
    if (!msg->role || !msg->text) {
        ESP_LOGE(TAG, "push_message: OOM for strdup");
        free(msg->role);
        free(msg->text);
        free(msg);
        return;
    }

    /* Schedule on LVGL thread (Core 0) — lv_async_call is thread-safe */
    lv_async_call(async_push_cb, msg);
}

bool ui_chat_is_active(void) { return s_active; }
