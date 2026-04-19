/**
 * Chat Suggestions — empty-state prompt cards (chat v4·C spec §5.4).
 *
 * 4 cards stacked in a column. Mode-specific prompts. Card color is
 * TH_CARD with 1 px border; pressed states handled by LVGL feedback.
 */
#include "chat_suggestions.h"
#include "ui_theme.h"
#include "config.h"
#include "settings.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "suggestions";

#define SUGG_CARD_H   72
#define SUGG_CARD_W   620
#define SUGG_GAP      12

/* Ellipsis / em-dash / NBSP-hyphen are NOT in the Montserrat font subset
 * shipped on-device — they render as tofu boxes. Stick to ASCII here. */
static const char *s_prompts[4][4] = {
    /* Local */
    { "What's the date?",
      "Add a note about...",
      "Remind me to...",
      "Summarize my last note" },
    /* Hybrid */
    { "Search the web for...",
      "Explain like I'm 5...",
      "What's the weather?",
      "Brief me on..." },
    /* Cloud */
    { "Write a Python script...",
      "Compare X and Y",
      "Draft a reply to...",
      "Plan my day around..." },
    /* Claw */
    { "Search my inbox for...",
      "Book a car at...",
      "Update my calendar...",
      "Pull the Tab5 docs" },
};

static const uint32_t s_mode_tint[4] = {
    TH_MODE_LOCAL, TH_MODE_HYBRID, TH_MODE_CLOUD, TH_MODE_CLAW,
};

/* Local + Hybrid + Agent (TinkerClaw) leads are static -- they describe
 * the TOPOLOGY not the model.  The Cloud lead is filled in live by
 * chat_suggestions_set_mode() so it reads the actual llm_model from NVS
 * ("Powered by gemini-3-flash-preview") instead of the old hardcoded
 * Claude/GPT-4o string. */
static char        s_cloud_lead_buf[80] = "Powered by cloud LLM.";
static const char *s_mode_lead[4] = {
    "Fast local AI -- private by default.",
    "Local model + cloud audio for clarity.",
    s_cloud_lead_buf,
    "Your agent: memory, tools, web.",
};

struct chat_suggestions {
    lv_obj_t *root;
    lv_obj_t *lead;
    lv_obj_t *cards[4];
    lv_obj_t *labels[4];
    uint8_t   mode;
    chat_sugg_pick_cb_t pick_cb;
    void     *pick_ud;
};

/* ── Event trampoline ──────────────────────────────────────────── */
static void ev_card_click(lv_event_t *e)
{
    chat_suggestions_t *s = (chat_suggestions_t *)lv_event_get_user_data(e);
    lv_obj_t *card = lv_event_get_target(e);
    if (!s || !s->pick_cb || !card) return;
    lv_obj_t *lbl = lv_obj_get_child(card, 0);
    if (!lbl) return;
    const char *txt = lv_label_get_text(lbl);
    if (txt && *txt) s->pick_cb(txt, s->pick_ud);
}

/* ── Create ────────────────────────────────────────────────────── */

chat_suggestions_t *chat_suggestions_create(lv_obj_t *parent)
{
    chat_suggestions_t *s = calloc(1, sizeof(*s));
    if (!s) { ESP_LOGE(TAG, "OOM"); return NULL; }

    s->root = lv_obj_create(parent);
    lv_obj_remove_style_all(s->root);
    lv_obj_set_size(s->root, SUGG_CARD_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s->root, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s->root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s->root, SUGG_GAP, 0);
    lv_obj_clear_flag(s->root, LV_OBJ_FLAG_SCROLLABLE);
    /* Sit below the 96-h chat header + 2-px accent + breathing room. */
    lv_obj_align(s->root, LV_ALIGN_TOP_MID, 0, 128);

    s->lead = lv_label_create(s->root);
    lv_label_set_text(s->lead, s_mode_lead[0]);
    lv_obj_set_style_text_font(s->lead, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s->lead, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_style_text_align(s->lead, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(s->lead, 8, 0);
    lv_obj_set_width(s->lead, SUGG_CARD_W - 40);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *card = lv_obj_create(s->root);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, SUGG_CARD_W, SUGG_CARD_H);
        lv_obj_set_style_bg_color(card, lv_color_hex(TH_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x1E1E2A), 0);
        lv_obj_set_style_pad_hor(card, 20, 0);
        lv_obj_set_style_pad_ver(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, ev_card_click, LV_EVENT_CLICKED, s);

        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, s_prompts[0][i]);
        lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_PRIMARY), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        s->cards[i]  = card;
        s->labels[i] = lbl;
    }

    return s;
}

void chat_suggestions_destroy(chat_suggestions_t *s)
{
    if (!s) return;
    if (s->root) lv_obj_del(s->root);
    free(s);
}

void chat_suggestions_set_mode(chat_suggestions_t *s, uint8_t m)
{
    if (!s) return;
    if (m > 3) m = 0;
    s->mode = m;
    /* Rebuild the Cloud lead from the live llm_model so it doesn't lie
     * when the user has picked, say, gemini or gpt-4o-mini.  Other modes
     * keep their static topology text. */
    if (m == 2) {
        char lm[64] = {0};
        tab5_settings_get_llm_model(lm, sizeof(lm));
        if (lm[0]) {
            const char *slash = strchr(lm, '/');
            const char *tail  = slash ? slash + 1 : lm;
            snprintf(s_cloud_lead_buf, sizeof(s_cloud_lead_buf),
                     "Powered by %.40s.", tail);
        } else {
            snprintf(s_cloud_lead_buf, sizeof(s_cloud_lead_buf),
                     "Powered by cloud LLM.");
        }
    }
    if (s->lead) lv_label_set_text(s->lead, s_mode_lead[m]);
    for (int i = 0; i < 4; i++) {
        if (s->labels[i]) lv_label_set_text(s->labels[i], s_prompts[m][i]);
        if (s->cards[i])
            lv_obj_set_style_border_color(s->cards[i],
                                          lv_color_hex(m == 0 ? 0x1E1E2A : s_mode_tint[m]), 0);
        if (s->cards[i] && m != 0) {
            lv_obj_set_style_border_opa(s->cards[i], LV_OPA_30, 0);
        } else if (s->cards[i]) {
            lv_obj_set_style_border_opa(s->cards[i], LV_OPA_COVER, 0);
        }
    }
}

void chat_suggestions_show(chat_suggestions_t *s)
{ if (s && s->root) lv_obj_clear_flag(s->root, LV_OBJ_FLAG_HIDDEN); }

void chat_suggestions_hide(chat_suggestions_t *s)
{ if (s && s->root) lv_obj_add_flag(s->root, LV_OBJ_FLAG_HIDDEN); }

bool chat_suggestions_visible(chat_suggestions_t *s)
{ return s && s->root && !lv_obj_has_flag(s->root, LV_OBJ_FLAG_HIDDEN); }

void chat_suggestions_on_pick(chat_suggestions_t *s, chat_sugg_pick_cb_t cb, void *ud)
{ if (s) { s->pick_cb = cb; s->pick_ud = ud; } }
