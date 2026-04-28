/*
 * ui_onboarding.c — First-boot welcome carousel (audit G / P0 UX).
 *
 * Three full-screen cards on lv_layer_top, with a left-swipe-style
 * Next button. Finish marks NVS so the overlay never shows again.
 */

#include "ui_onboarding.h"
#include "ui_home.h"
#include "ui_theme.h"
#include "settings.h"
#include "voice.h"
#include "wifi.h"
#include "config.h"

#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ui_onboarding";

#define CARD_W 720
#define CARD_H 1280

static lv_obj_t *s_overlay   = NULL;
static lv_obj_t *s_card      = NULL;
static lv_obj_t *s_title     = NULL;
static lv_obj_t *s_body      = NULL;
static lv_obj_t *s_accent    = NULL;
static lv_obj_t *s_kicker    = NULL;
static lv_obj_t *s_dot[3]    = {NULL, NULL, NULL};
static lv_obj_t *s_primary_btn   = NULL;
static lv_obj_t *s_primary_lbl   = NULL;
static lv_obj_t *s_secondary_btn = NULL;
static lv_obj_t *s_secondary_lbl = NULL;

static int s_step = 0;

typedef struct {
    const char *kicker;
    const char *title;
    const char *body;
    const char *primary;
    const char *secondary;
} onboard_card_t;

static const onboard_card_t s_cards[] = {
    {
        .kicker    = "\xe2\x80\xa2 WELCOME",
        .title     = "Hey, I'm Tinker.",
        .body      = "Hold the amber orb to talk - any screen, any time.\n"
                     "I'll listen, think, and either speak back or paint\n"
                     "something useful on the home screen.\n\n"
                     "No wake word needed. No typing unless you want to.",
        .primary   = "Next",
        .secondary = "Skip",
    },
    {
        .kicker    = "\xe2\x80\xa2 HOW I THINK",
        .title     = "Four ways to reply.",
        .body      = "Local: runs on the Dragon box next to you.\n"
                     "            Private, free, a little slower.\n\n"
                     "Hybrid: cloud ears + local brain.\n\n"
                     "Cloud: GPT/Claude/Gemini - fast, smart, billed.\n\n"
                     "Agent: the TinkerClaw gateway with tools + memory.\n"
                     "            Long-press the mode chip to switch.",
        .primary   = "Next",
        .secondary = "Skip",
    },
    {
        .kicker    = "\xe2\x80\xa2 YOUR DATA",
        .title     = "Memory is yours.",
        .body      = "Everything you say sits on the Dragon box on your\n"
                     "network, not in someone's data center. Ask me to\n"
                     "forget anything at any time.\n\n"
                     "Agent mode bypasses your local memory - I run from\n"
                     "the gateway's context. You'll see a warning first.",
        .primary   = "Get started",
        .secondary = NULL,
    },
};

#define S_CARD_COUNT ((int)(sizeof(s_cards) / sizeof(s_cards[0])))

static void render_step(int step);
static void primary_cb(lv_event_t *e);
static void secondary_cb(lv_event_t *e);

static void destroy_overlay(void)
{
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    s_card = s_title = s_body = s_accent = s_kicker = NULL;
    s_primary_btn = s_primary_lbl = NULL;
    s_secondary_btn = s_secondary_lbl = NULL;
    for (int i = 0; i < 3; i++) s_dot[i] = NULL;
}

static void build_overlay(void)
{
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, CARD_W, CARD_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x08080E), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Card — full-screen minus margins for a softer look. */
    s_card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 640, 1140);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x13131F), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 32, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    s_accent = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_accent);
    lv_obj_set_size(s_accent, 160, 4);
    lv_obj_set_pos(s_accent, 48, 60);
    lv_obj_set_style_bg_color(s_accent, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(s_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_accent, 2, 0);

    s_kicker = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_kicker, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_kicker, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(s_kicker, 4, 0);
    lv_obj_set_pos(s_kicker, 48, 80);

    s_title = lv_label_create(s_card);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_title, 540);
    lv_obj_set_style_text_font(s_title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_pos(s_title, 48, 116);

    s_body = lv_label_create(s_card);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body, 540);
    lv_obj_set_style_text_font(s_body, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_body, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_line_space(s_body, 6, 0);
    lv_obj_set_pos(s_body, 48, 220);

    /* Progress dots near bottom of card. */
    for (int i = 0; i < 3; i++) {
        s_dot[i] = lv_obj_create(s_card);
        lv_obj_remove_style_all(s_dot[i]);
        lv_obj_set_size(s_dot[i], 10, 10);
        lv_obj_set_pos(s_dot[i], 280 + i * 30, 820);
        lv_obj_set_style_radius(s_dot[i], 5, 0);
        lv_obj_set_style_bg_opa(s_dot[i], LV_OPA_COVER, 0);
    }

    /* Primary button. */
    s_primary_btn = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_primary_btn);
    lv_obj_set_size(s_primary_btn, 540, 72);
    lv_obj_set_pos(s_primary_btn, 48, 900);
    lv_obj_set_style_bg_color(s_primary_btn, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(s_primary_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_primary_btn, 36, 0);
    lv_obj_set_style_border_width(s_primary_btn, 0, 0);
    lv_obj_clear_flag(s_primary_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_primary_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_primary_btn, primary_cb, LV_EVENT_CLICKED, NULL);

    s_primary_lbl = lv_label_create(s_primary_btn);
    lv_obj_set_style_text_font(s_primary_lbl, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s_primary_lbl, lv_color_hex(0x08080E), 0);
    lv_obj_center(s_primary_lbl);

    /* Secondary button (Skip). */
    s_secondary_btn = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_secondary_btn);
    lv_obj_set_size(s_secondary_btn, 540, 56);
    lv_obj_set_pos(s_secondary_btn, 48, 988);
    lv_obj_set_style_bg_opa(s_secondary_btn, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_secondary_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_secondary_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_secondary_btn, secondary_cb, LV_EVENT_CLICKED, NULL);

    s_secondary_lbl = lv_label_create(s_secondary_btn);
    lv_obj_set_style_text_font(s_secondary_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_secondary_lbl, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_center(s_secondary_lbl);
}

static void render_step(int step)
{
    if (step < 0 || step >= S_CARD_COUNT) return;
    const onboard_card_t *c = &s_cards[step];
    lv_label_set_text(s_kicker, c->kicker);
    lv_label_set_text(s_title,  c->title);
    lv_label_set_text(s_body,   c->body);
    lv_label_set_text(s_primary_lbl, c->primary);

    /* Dots: amber for current, dim for others. */
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_dot[i],
            lv_color_hex(i == step ? TH_AMBER : 0x2A2A3A), 0);
    }

    if (c->secondary) {
        lv_obj_clear_flag(s_secondary_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_secondary_lbl, c->secondary);
    } else {
        lv_obj_add_flag(s_secondary_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void primary_cb(lv_event_t *e)
{
    (void)e;
    if (s_step < S_CARD_COUNT - 1) {
        s_step++;
        render_step(s_step);
    } else {
        ui_onboarding_finish();
    }
}

static void secondary_cb(lv_event_t *e)
{
    (void)e;
    ui_onboarding_finish();
}

/* ── Public API ──────────────────────────────────────────────────── */

void ui_onboarding_show_if_needed(void)
{
    if (tab5_settings_is_onboarded()) {
        ESP_LOGI(TAG, "onboarding: already done, skipping");
        return;
    }
    ui_onboarding_force_show();
}

void ui_onboarding_force_show(void)
{
    if (s_overlay) {
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        return;
    }
    build_overlay();
    s_step = 0;
    render_step(s_step);
    ESP_LOGI(TAG, "onboarding shown (step 0/%d)", S_CARD_COUNT);
}

/* U22 (#206): post-finish round-trip verification.  The static
 * carousel doesn't actually exercise the device's connectivity, so a
 * user can finish onboarding while WiFi is mis-configured or Dragon is
 * unreachable and never know — they'll just hit "Hold to speak" and
 * wonder why nothing happens.
 *
 * 3 s after finish, check WiFi + voice-WS state.  Both up: silent
 * "Connected to Dragon ✓" confirmation.  Either down: visible
 * "Setup unfinished — check Settings → Network" toast.  Either way
 * onboarding stays marked done (no relooping), but the user gets a
 * clear signal about whether their setup actually works. */
static void verify_round_trip_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    bool wifi   = tab5_wifi_connected();
    bool dragon = voice_is_connected();
    if (wifi && dragon) {
        ESP_LOGI(TAG, "onboard verify: wifi=1 dragon=1 — round-trip OK");
        ui_home_show_toast("Connected to Dragon");
    } else {
        ESP_LOGW(TAG, "onboard verify: wifi=%d dragon=%d — round-trip incomplete",
                 wifi, dragon);
        ui_home_show_toast("Setup unfinished — check Settings");
    }
}

void ui_onboarding_finish(void)
{
    tab5_settings_set_onboarded(true);
    destroy_overlay();
    ESP_LOGI(TAG, "onboarding finished, NVS onboard=1");
    /* U22 (#206): single-shot timer so the home screen is visible
     * before the verify toast lands.  3 s gives wifi/voice a moment
     * to settle if they were mid-connect during the carousel. */
    lv_timer_t *vt = lv_timer_create(verify_round_trip_cb, 3000, NULL);
    if (vt) lv_timer_set_repeat_count(vt, 1);
}

bool ui_onboarding_visible(void)
{
    return s_overlay != NULL
           && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}
