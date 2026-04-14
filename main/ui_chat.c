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
#include "ui_keyboard.h"
#include "settings.h"
#include "rtc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "lvgl.h"
#include <string.h>

/* Declared in ui_home.c */
extern lv_obj_t *ui_home_get_screen(void);

static const char *TAG = "ui_chat";

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

/* Streaming assistant response tracking */
static lv_obj_t  *s_assist_bubble = NULL;
static lv_obj_t  *s_assist_label  = NULL;

/* Tool indicator */
static lv_obj_t  *s_tool_label   = NULL;

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

#define MAX_MESSAGES     50
#define BUBBLE_MAX_W    480
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

/* ── Mode / status helpers ─────────────────────────────────────── */

static void update_mode_badge_obj(lv_obj_t *badge)
{
    if (!badge) return;
    uint8_t mode = tab5_settings_get_voice_mode();
    const char *labels[] = {"Local", "Hybrid", "Cloud"};
    const uint32_t colors[] = {0x22C55E, 0xF5A623, 0x06B6D4};
    if (mode > 2) mode = 0;
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
    mode = (mode + 1) % 3;
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
#define N_LOCAL_MODELS  (sizeof(s_local_models) / sizeof(s_local_models[0]))
#define N_CLOUD_MODELS  (sizeof(s_cloud_models) / sizeof(s_cloud_models[0]))

static void update_model_label(void)
{
    if (!s_model_lbl) return;
    char model_buf[64];
    tab5_settings_get_llm_model(model_buf, sizeof(model_buf));
    uint8_t mode = tab5_settings_get_voice_mode();
    if (model_buf[0]) {
        /* Show short name: strip "anthropic/" or "openai/" prefix */
        const char *short_name = model_buf;
        const char *slash = strrchr(model_buf, '/');
        if (slash) short_name = slash + 1;
        lv_label_set_text_fmt(s_model_lbl, LV_SYMBOL_EDIT " %s", short_name);
    } else {
        lv_label_set_text(s_model_lbl,
            mode == 2 ? LV_SYMBOL_EDIT " claude-3.5-haiku" :
                        LV_SYMBOL_EDIT " qwen3:1.7b");
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
    if (mode == 2) {
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

static void cb_new_chat(lv_event_t *e)
{
    (void)e;
    voice_clear_history();
    /* Clear all messages from scroll */
    if (s_msg_scroll) lv_obj_clean(s_msg_scroll);
    s_msg_count = 0;
    s_next_y = 12;
    s_assist_bubble = NULL;
    s_assist_label = NULL;
    s_tool_label = NULL;
    s_typing_lbl = NULL;
    s_empty_hint = NULL;
    /* Re-add welcome */
    ui_chat_add_message("Fresh conversation started!", false);
    ESP_LOGI(TAG, "New chat — history cleared");
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

    /* Create a small bubble-style indicator */
    s_typing_lbl = lv_obj_create(s_msg_scroll);
    lv_obj_remove_style_all(s_typing_lbl);
    lv_obj_set_size(s_typing_lbl, 200, 36);
    lv_obj_set_pos(s_typing_lbl, 16, s_next_y);
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
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  Using %s...", tool_name ? tool_name : "tool");
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

static void cb_close(lv_event_t *e)
{
    /* Support swipe-right as back gesture */
    if (e && lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;
    }

    /* If in conversation, go back to Chat Home instead of closing */
    if (s_in_conversation) {
        show_chat_home();
        return;
    }

    /* Not in conversation — close entire overlay */
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
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

static void cb_textarea_click(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Textarea tapped — showing keyboard");
    if (s_textarea) ui_keyboard_show(s_textarea);
}

static void cb_mic(lv_event_t *e)
{
    (void)e;
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
    ui_keyboard_hide();
    const char *txt = lv_textarea_get_text(s_textarea);
    if (!txt || !txt[0]) return;

    /* If on Chat Home, enter conversation first */
    if (!s_in_conversation) {
        enter_conversation();
    }

    /* Show user bubble immediately */
    ui_chat_add_message(txt, true);

    /* Send to Dragon */
    esp_err_t ret = voice_send_text(txt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "voice_send_text failed: %s", esp_err_to_name(ret));
        ui_chat_add_message("(Not connected to Dragon)", false);
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

/* Quick action callbacks */
static void cb_quick_web(lv_event_t *e)
{
    (void)e;
    enter_conversation();
    ui_chat_add_message("Search the web for...", true);
    voice_send_text("Search the web for the latest news");
}

static void cb_quick_remember(lv_event_t *e)
{
    (void)e;
    enter_conversation();
    ui_chat_add_message("What do you remember about me?", true);
    voice_send_text("What do you remember about me?");
}

static void cb_quick_timer(lv_event_t *e)
{
    (void)e;
    enter_conversation();
    ui_chat_add_message("Set a timer for 5 minutes", true);
    voice_send_text("Set a timer for 5 minutes");
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

    if (s_conv_panel) {
        lv_obj_clear_flag(s_conv_panel, LV_OBJ_FLAG_HIDDEN);
    }

    /* Swap active textarea to conversation panel */
    s_textarea = s_conv_textarea;

    update_mode_badge();
    update_status_bar();
}

/* ── Build Chat Home panel ─────────────────────────────────────── */

static void build_home_panel(void)
{
    ESP_LOGI(TAG, "Building Chat Home panel");
    esp_task_wdt_reset();

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

    esp_task_wdt_reset();

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

    /* ── Quick Actions Bar (56px) ────────────────────────────── */
    int qa_y = y + 8;
    int qa_x = 20;
    int qa_w = 140;
    int qa_gap = 16;

    /* Web search */
    lv_obj_t *qa_web = lv_button_create(s_home_panel);
    lv_obj_set_size(qa_web, qa_w, 40);
    lv_obj_set_pos(qa_web, qa_x, qa_y);
    lv_obj_set_style_bg_color(qa_web, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_radius(qa_web, 20, 0);
    lv_obj_set_style_border_width(qa_web, 1, 0);
    lv_obj_set_style_border_color(qa_web, lv_color_hex(0x333350), 0);
    lv_obj_add_event_cb(qa_web, cb_quick_web, LV_EVENT_CLICKED, NULL);
    ui_fb_button(qa_web);
    lv_obj_t *qa_web_lbl = lv_label_create(qa_web);
    lv_label_set_text(qa_web_lbl, LV_SYMBOL_WIFI " Web");
    lv_obj_set_style_text_color(qa_web_lbl, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(qa_web_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(qa_web_lbl);

    /* Timer */
    lv_obj_t *qa_timer = lv_button_create(s_home_panel);
    lv_obj_set_size(qa_timer, qa_w, 40);
    lv_obj_set_pos(qa_timer, qa_x + qa_w + qa_gap, qa_y);
    lv_obj_set_style_bg_color(qa_timer, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_radius(qa_timer, 20, 0);
    lv_obj_set_style_border_width(qa_timer, 1, 0);
    lv_obj_set_style_border_color(qa_timer, lv_color_hex(0x333350), 0);
    lv_obj_add_event_cb(qa_timer, cb_quick_timer, LV_EVENT_CLICKED, NULL);
    ui_fb_button(qa_timer);
    lv_obj_t *qa_timer_lbl = lv_label_create(qa_timer);
    lv_label_set_text(qa_timer_lbl, LV_SYMBOL_BELL " Timer");
    lv_obj_set_style_text_color(qa_timer_lbl, lv_color_hex(CLR_AMBER), 0);
    lv_obj_set_style_text_font(qa_timer_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(qa_timer_lbl);

    /* Remember */
    lv_obj_t *qa_mem = lv_button_create(s_home_panel);
    lv_obj_set_size(qa_mem, qa_w + 20, 40);
    lv_obj_set_pos(qa_mem, qa_x + 2 * (qa_w + qa_gap), qa_y);
    lv_obj_set_style_bg_color(qa_mem, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_radius(qa_mem, 20, 0);
    lv_obj_set_style_border_width(qa_mem, 1, 0);
    lv_obj_set_style_border_color(qa_mem, lv_color_hex(0x333350), 0);
    lv_obj_add_event_cb(qa_mem, cb_quick_remember, LV_EVENT_CLICKED, NULL);
    ui_fb_button(qa_mem);
    lv_obj_t *qa_mem_lbl = lv_label_create(qa_mem);
    lv_label_set_text(qa_mem_lbl, LV_SYMBOL_EYE_OPEN " Memory");
    lv_obj_set_style_text_color(qa_mem_lbl, lv_color_hex(0xBF5AF2), 0);
    lv_obj_set_style_text_font(qa_mem_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(qa_mem_lbl);

    y += 56;

    esp_task_wdt_reset();

    /* ── Session Cards (scrollable area) ─────────────────────── */
    /* Available height: 1280 - topbar(60) - model(44) - quickactions(56) - memory(48) - input(80) = 992 */
    int cards_h = 1280 - TOPBAR_H - 44 - 56 - 48 - INPUT_BAR_H;  /* 992 */

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
    lv_label_set_text_fmt(card1_count, "%d messages", s_msg_count);
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
        lv_obj_set_size(sug, card_w, 48);
        lv_obj_set_pos(sug, card_x, card_y);
        lv_obj_set_style_bg_color(sug, lv_color_hex(CLR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(sug, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sug, 12, 0);
        lv_obj_set_style_border_width(sug, 1, 0);
        lv_obj_set_style_border_color(sug, lv_color_hex(0x222230), 0);
        lv_obj_set_style_pad_left(sug, 16, 0);
        lv_obj_clear_flag(sug, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sug_lbl = lv_label_create(sug);
        lv_label_set_text(sug_lbl, suggestions[i]);
        lv_obj_set_style_text_color(sug_lbl, lv_color_hex(sug_colors[i]), 0);
        lv_obj_set_style_text_font(sug_lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(sug_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        card_y += 48 + 8;
    }

    esp_task_wdt_reset();

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
    lv_obj_set_style_text_color(s_memory_lbl, lv_color_hex(0x666688), 0);
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
    esp_task_wdt_reset();

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

    esp_task_wdt_reset();

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

    /* Welcome message */
    ui_chat_add_message("Hi! I'm Tinker. Type or tap the mic to chat.", false);

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

        /* Always show Chat Home on re-open */
        if (s_in_conversation) {
            show_chat_home();
        }
        return s_overlay;
    }

    ESP_LOGI(TAG, "Creating chat overlay");
    esp_task_wdt_reset();

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

    /* Swipe-right to close / go back */
    lv_obj_add_event_cb(s_overlay, cb_close, LV_EVENT_GESTURE, NULL);

    esp_task_wdt_reset();

    /* ── Build both panels ─────────────────────────────────── */
    build_home_panel();

    esp_task_wdt_reset();

    /* Conversation panel is built lazily on first enter */
    /* s_conv_panel created in build_conversation_ui() */

    /* ── Done ───────────────────────────────────────────────── */
    s_active = true;
    s_in_conversation = false;
    s_last_state = voice_get_state();

    /* Start polling for streaming LLM responses + status updates */
    s_poll_timer = lv_timer_create(poll_voice_cb, 200, NULL);

    ESP_LOGI(TAG, "Chat overlay ready — showing Chat Home");
    return s_overlay;
}

void ui_chat_add_message(const char *text, bool is_user)
{
    if (!s_msg_scroll || !text || !text[0]) return;

    /* Enforce max message count — delete oldest */
    if (s_msg_count >= MAX_MESSAGES) {
        lv_obj_t *oldest = lv_obj_get_child(s_msg_scroll, 0);
        if (oldest) {
            lv_obj_del(oldest);
            s_msg_count--;
            /* Recalculate s_next_y from remaining children */
            s_next_y = 12;
            uint32_t cnt = lv_obj_get_child_count(s_msg_scroll);
            for (uint32_t i = 0; i < cnt; i++) {
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
    int x_pos = is_user ? (720 - BUBBLE_MAX_W - 16) : 16;   /* right or left aligned */
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
        lv_obj_set_style_text_color(ts, lv_color_hex(is_user ? 0x333333 : 0x666666), 0);
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
    s_status_lbl    = NULL;
    s_status_dot    = NULL;
    s_active        = false;
    s_in_conversation = false;
    s_conv_created  = false;
    s_msg_count     = 0;
    s_next_y        = 0;
    s_last_state    = VOICE_STATE_IDLE;

    ESP_LOGI(TAG, "Chat destroyed");
}

bool ui_chat_is_active(void) { return s_active; }
