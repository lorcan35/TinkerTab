/**
 * Mode-specific suggestion cards for empty chat state.
 * Shows 4 tappable prompt suggestions based on current voice mode.
 * Uses flex column layout, DPI_SCALE sizing.
 */

#include "chat_suggestions.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "suggestions";

static lv_obj_t *s_container = NULL;

/* Suggestion texts per mode */
static const char *s_suggestions[4][4] = {
    /* Local */
    { "\"Tell me a joke\"",
      "\"What time is it?\"",
      "\"Set a 5 minute timer\"",
      "\"How's the weather?\"" },
    /* Hybrid */
    { "\"Tell me a joke\"",
      "\"What time is it?\"",
      "\"Set a 5 minute timer\"",
      "\"Translate hello to French\"" },
    /* Cloud */
    { "\"Explain quantum computing\"",
      "\"Write a Python function\"",
      "\"Compare AirPods vs Sony XM5\"",
      "\"Summarize this article...\"" },
    /* TinkerClaw */
    { "\"What do you know about me?\"",
      "\"Search the web for...\"",
      "\"Remember that I like sushi\"",
      "\"What can you do?\"" },
};

/* Mode accent colors */
static const uint32_t s_mode_colors[4] = {
    0x22C55E,  /* Local: green */
    0xEAB308,  /* Hybrid: yellow */
    0x3B82F6,  /* Cloud: blue */
    0xE8457A,  /* TinkerClaw: rose */
};

/* Mode hints */
static const char *s_mode_hints[4] = {
    "Ask me anything!\nFast local AI, private.",
    "Ask me anything!\nLocal AI, cloud audio.",
    "Ask me anything!\nPowered by Claude / GPT-4o.",
    "I'm your AI agent.\nMemory, web search, tools.",
};

void chat_suggestions_create(lv_obj_t *parent, uint8_t mode, lv_event_cb_t on_tap)
{
    if (mode >= 4) mode = 0;

    /* Destroy old container if mode changed */
    if (s_container) {
        lv_obj_del(s_container);
        s_container = NULL;
    }

    s_container = lv_obj_create(parent);
    lv_obj_remove_style_all(s_container);
    lv_obj_set_size(s_container, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_align(s_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(s_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_container, DPI_SCALE(8), 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Hint text */
    lv_obj_t *hint = lv_label_create(s_container);
    lv_label_set_text(hint, s_mode_hints[mode]);
    lv_obj_set_style_text_font(hint, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(hint, DPI_SCALE(12), 0);

    /* Suggestion cards */
    uint32_t color = s_mode_colors[mode];
    for (int i = 0; i < 4; i++) {
        lv_obj_t *card = lv_obj_create(s_container);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, lv_pct(100), DPI_SCALE(48));
        lv_obj_set_style_bg_color(card, lv_color_hex(0x111122), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, DPI_SCALE(10), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x222233), 0);
        lv_obj_set_style_pad_hor(card, DPI_SCALE(14), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, s_suggestions[mode][i]);
        lv_obj_set_style_text_font(lbl, FONT_SECONDARY, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        if (on_tap) {
            lv_obj_add_event_cb(card, on_tap, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
        }
    }
}

void chat_suggestions_show(void)
{
    if (s_container) lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
}

void chat_suggestions_hide(void)
{
    if (s_container) lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
}

bool chat_suggestions_visible(void)
{
    return s_container && !lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN);
}
