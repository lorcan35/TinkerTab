/**
 * TinkerTab — Chat Screen (iMessage/WhatsApp style)
 *
 * Fullscreen overlay parented to the home screen (NOT lv_layer_top).
 * Manual Y positioning — NO flex layout. Object count kept under 60.
 * Show/hide via LV_OBJ_FLAG_HIDDEN + lv_obj_move_foreground().
 */
#include "ui_chat.h"
#include "voice.h"
#include "ui_keyboard.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "lvgl.h"
#include <string.h>

/* Declared in ui_home.c */
extern lv_obj_t *ui_home_get_screen(void);

static const char *TAG = "ui_chat";

/* ── State ─────────────────────────────────────────────────────── */
static lv_obj_t  *s_overlay      = NULL;
static lv_obj_t  *s_msg_scroll   = NULL;
static lv_obj_t  *s_textarea     = NULL;
static lv_obj_t  *s_mode_badge   = NULL;   /* "Local" / "Cloud" label */
static lv_timer_t *s_poll_timer  = NULL;

/* Streaming assistant response tracking */
static lv_obj_t  *s_assist_bubble = NULL;
static lv_obj_t  *s_assist_label  = NULL;

/* Tool indicator */
static lv_obj_t  *s_tool_label   = NULL;

static bool         s_active     = false;
static int          s_msg_count  = 0;
static int          s_next_y     = 0;      /* Running Y offset in scroll area */
static voice_state_t s_last_state = VOICE_STATE_IDLE;

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

/* ── Helpers ───────────────────────────────────────────────────── */

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

static void update_mode_badge(void)
{
    if (!s_mode_badge) return;
    uint8_t mode = tab5_settings_get_voice_mode();
    if (mode == 0)
        lv_label_set_text(s_mode_badge, "Local");
    else
        lv_label_set_text(s_mode_badge, "Cloud");
}

/* ── Callbacks ─────────────────────────────────────────────────── */

static void cb_close(lv_event_t *e)
{
    /* Support swipe-right as back gesture */
    if (e && lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;
    }

    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
    ui_keyboard_hide();

    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    }

    s_assist_bubble = NULL;
    s_assist_label  = NULL;
    s_tool_label    = NULL;
    s_active        = false;
    ESP_LOGI(TAG, "Chat hidden (messages preserved)");
}

static void cb_textarea_click(lv_event_t *e)
{
    (void)e;
    if (s_textarea) ui_keyboard_show(s_textarea);
}

static void cb_mic(lv_event_t *e)
{
    (void)e;
    voice_state_t st = voice_get_state();
    if (st == VOICE_STATE_READY) {
        ESP_LOGI(TAG, "Chat mic: starting voice input");
        voice_start_listening();
    } else if (st == VOICE_STATE_IDLE) {
        ESP_LOGW(TAG, "Chat mic: not connected to Dragon");
        ui_chat_add_message("(Not connected — tap orb on Home to connect)", false);
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

/* ── Tool indicator management ─────────────────────────────────── */

static void show_tool_indicator(const char *tool_name)
{
    if (!s_msg_scroll) return;

    /* Build text like "Searching the web..." */
    char buf[128];
    if (tool_name && strncmp(tool_name, "web_search", 10) == 0) {
        snprintf(buf, sizeof(buf), "Searching the web...");
    } else if (tool_name && strncmp(tool_name, "a tool", 6) == 0) {
        snprintf(buf, sizeof(buf), "Using a tool...");
    } else if (tool_name) {
        /* Extract tool name up to quote */
        char name[48];
        int i = 0;
        while (tool_name[i] && tool_name[i] != '"' && i < 47) {
            name[i] = tool_name[i]; i++;
        }
        name[i] = '\0';
        snprintf(buf, sizeof(buf), "Using %s...", name);
    } else {
        snprintf(buf, sizeof(buf), "Thinking...");
    }

    if (s_tool_label) {
        lv_label_set_text(s_tool_label, buf);
        return;
    }

    /* Create a small italic-style label directly in scroll area */
    s_tool_label = lv_label_create(s_msg_scroll);
    lv_label_set_text(s_tool_label, buf);
    lv_obj_set_pos(s_tool_label, 16, s_next_y);
    lv_obj_set_style_text_color(s_tool_label, lv_color_hex(CLR_TOOL_DIM), 0);
    lv_obj_set_style_text_font(s_tool_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_opa(s_tool_label, LV_OPA_70, 0);

    /* Don't advance s_next_y — tool label will be removed when response arrives */
}

static void hide_tool_indicator(void)
{
    if (s_tool_label) {
        lv_obj_del(s_tool_label);
        s_tool_label = NULL;
    }
}

/* ── Voice polling timer ───────────────────────────────────────── */

static void poll_voice_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    voice_state_t st = voice_get_state();

    /* Stream LLM tokens into assistant bubble */
    if (st == VOICE_STATE_PROCESSING || st == VOICE_STATE_SPEAKING) {
        const char *llm = voice_get_llm_text();
        if (llm && llm[0]) {
            /* Check for tool tags */
            const char *tool = detect_tool_name(llm);
            if (tool) {
                show_tool_indicator(tool);
            }

            /* Strip tool tags for display */
            char stripped[1024];
            strip_tool_tags(llm, stripped, sizeof(stripped));

            if (stripped[0]) {
                hide_tool_indicator();

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
        }
    }

    /* Transition: PROCESSING/SPEAKING → READY/IDLE = finalize */
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
            s_assist_bubble = NULL;
            s_assist_label  = NULL;
        }
    }

    s_last_state = st;
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
        s_poll_timer = lv_timer_create(poll_voice_cb, 200, NULL);
        update_mode_badge();
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

    /* Swipe-right to close */
    lv_obj_add_event_cb(s_overlay, cb_close, LV_EVENT_GESTURE, NULL);

    /* ── Top bar (60px) ─────────────────────────────────────── */
    /* Back arrow button — left */
    lv_obj_t *back_btn = lv_button_create(s_overlay);
    lv_obj_set_size(back_btn, 80, 44);
    lv_obj_set_pos(back_btn, 12, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, cb_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(back_lbl);

    /* "Chat" title — center */
    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, "Chat");
    lv_obj_set_pos(title, 310, 16);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);

    /* Mode badge — right */
    s_mode_badge = lv_label_create(s_overlay);
    lv_obj_set_pos(s_mode_badge, 610, 20);
    lv_obj_set_style_text_color(s_mode_badge, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_text_font(s_mode_badge, &lv_font_montserrat_16, 0);
    update_mode_badge();

    /* Topbar separator line */
    lv_obj_t *sep = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 720, 1);
    lv_obj_set_pos(sep, 0, TOPBAR_H - 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    /* ── Message scroll area ────────────────────────────────── */
    s_msg_scroll = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_msg_scroll);
    lv_obj_set_size(s_msg_scroll, 720, MSG_AREA_H);
    lv_obj_set_pos(s_msg_scroll, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(s_msg_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_msg_scroll, 0, 0);
    lv_obj_add_flag(s_msg_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_msg_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_msg_scroll, LV_DIR_VER);

    s_next_y = 12;  /* Initial top padding */

    /* ── Input bar (80px) ───────────────────────────────────── */
    /* Bar background */
    lv_obj_t *bar = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, INPUT_BAR_H);
    lv_obj_set_pos(bar, 0, 1280 - INPUT_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Mic button — 48px circle, cyan */
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
    s_textarea = lv_textarea_create(bar);
    lv_obj_set_size(s_textarea, 440, 48);
    lv_obj_set_pos(s_textarea, 72, 16);
    lv_textarea_set_placeholder_text(s_textarea, "Type a message...");
    lv_textarea_set_one_line(s_textarea, true);
    lv_obj_set_style_bg_color(s_textarea, lv_color_hex(CLR_INPUT_BG), 0);
    lv_obj_set_style_text_color(s_textarea, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_textarea, &lv_font_montserrat_18, 0);
    lv_obj_set_style_border_color(s_textarea, lv_color_hex(CLR_INPUT_BORDER), 0);
    lv_obj_set_style_border_width(s_textarea, 1, 0);
    lv_obj_set_style_radius(s_textarea, 24, 0);
    lv_obj_set_style_pad_left(s_textarea, 20, 0);
    /* Placeholder dim gray */
    lv_obj_set_style_text_color(s_textarea, lv_color_hex(0x666666), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(s_textarea, cb_textarea_click, LV_EVENT_CLICKED, NULL);

    /* Send button — amber */
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

    /* ── Done ───────────────────────────────────────────────── */
    s_active = true;
    s_last_state = voice_get_state();

    /* Welcome message */
    ui_chat_add_message("Hi! I'm Tinker. Type or tap the mic to chat.", false);

    /* Start polling for streaming LLM responses */
    s_poll_timer = lv_timer_create(poll_voice_cb, 200, NULL);

    ESP_LOGI(TAG, "Chat overlay ready");
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

    /* Force layout so we can measure height */
    lv_obj_update_layout(bubble);
    int bh = lv_obj_get_height(bubble);

    s_next_y += bh + BUBBLE_GAP;
    s_msg_count++;

    /* Auto-scroll to bottom */
    lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_ON);
}

void ui_chat_destroy(void)
{
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = NULL; }
    ui_keyboard_hide();

    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }

    s_msg_scroll    = NULL;
    s_textarea      = NULL;
    s_mode_badge    = NULL;
    s_assist_bubble = NULL;
    s_assist_label  = NULL;
    s_tool_label    = NULL;
    s_active        = false;
    s_msg_count     = 0;
    s_next_y        = 0;
    s_last_state    = VOICE_STATE_IDLE;

    ESP_LOGI(TAG, "Chat destroyed");
}

bool ui_chat_is_active(void) { return s_active; }
