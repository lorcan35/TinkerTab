/*
 * ui_home.c — TinkerOS Home Screen — v4·C "Ambient Canvas"
 *
 * 720×1280 voice-first portrait layout for the ESP32-P4 Tab5.
 * See .superpowers/brainstorm/ui-overhaul-v4/c-ambient-canvas.html for the
 * source mockup and docs/WIDGETS.md §3.1 for the tone rendering contract.
 *
 * LVGL primitives only:
 *   - Orb: 156 px circle, 2-stop vertical gradient (existing paint_for_*).
 *   - Halo: 2 concentric dim amber circles (no shadow — LVGL 9 shadows can
 *     blow the 144 KB partial draw buffer at scale on ESP32-P4).
 *   - Rings: 3 border-only circles around the orb.
 *   - Now-slot card: widget_live renders directly into kicker/lede/stats.
 *   - Strip: 108 px say-pill + 56 px rail of 4 chips. One composed unit,
 *     replaces v5's stacked input+nav.
 *
 * Interactions (preserved from v5):
 *   Tap orb            → voice overlay + start listening
 *   Long-press orb     → cycle voice mode (Local → Hybrid → Cloud → Claw)
 *   Tap now-card       → Agents overlay (thread context)
 *   Tap sys label      → Memory overlay
 *   Long-press mode    → (future) open segmented picker sheet
 *   Swipe up           → Focus
 *   Swipe from left    → Notes
 *   Swipe from right   → Settings
 */

#include "ui_home.h"
#include "ui_theme.h"
#include "ui_agents.h"
#include "ui_memory.h"
#include "ui_focus.h"
#include "ui_chat.h"
#include "ui_notes.h"
#include "ui_settings.h"
#include "ui_voice.h"
#include "ui_keyboard.h"
#include "ui_core.h"
#include "voice.h"
#include "settings.h"
#include "config.h"
#include "tab5_rtc.h"
#include "battery.h"
#include "dragon_link.h"
#include "wifi.h"
#include "widget.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static const char *TAG = "ui_home";

/* ── Layout constants ─────────────────────────────────────────── */
#define SW                720
#define SH                1280
#define SIDE_PAD          40

/* Orb stage */
#define ORB_CX            (SW / 2)
#define ORB_CY            320
#define ORB_SIZE          156
#define HALO_OUTER        440
#define HALO_INNER        300
#define RING_OUTER        260
#define RING_MID          210
#define RING_INNER        175

/* Now-slot card */
#define CARD_X            SIDE_PAD
#define CARD_Y            780
#define CARD_W            (SW - 2 * SIDE_PAD)
#define CARD_H            240
#define CARD_PAD          28

/* Bottom strip */
#define STRIP_BOT_PAD     40
#define RAIL_H            56
#define SAY_H             108
#define SAY_GAP           14

/* ── State ────────────────────────────────────────────────────── */
static lv_obj_t *s_screen          = NULL;

/* Status strip */
static lv_obj_t *s_sys_dot         = NULL;
static lv_obj_t *s_sys_label       = NULL;  /* left: "ONLINE" / "OFFLINE" / etc */
static lv_obj_t *s_time_label      = NULL;  /* right: "Thursday · 9:42" */

/* Orb stage */
static lv_obj_t *s_halo_outer      = NULL;
static lv_obj_t *s_halo_inner      = NULL;
static lv_obj_t *s_ring_outer      = NULL;
static lv_obj_t *s_ring_mid        = NULL;
static lv_obj_t *s_ring_inner      = NULL;
static lv_obj_t *s_orb             = NULL;

/* State word + greet line */
static lv_obj_t *s_state_word      = NULL;  /* "listening" / "offline" */
static lv_obj_t *s_greet_line      = NULL;  /* "morning, emile" */

/* Mode chip */
static lv_obj_t *s_mode_chip       = NULL;
static lv_obj_t *s_mode_dot        = NULL;
static lv_obj_t *s_mode_name       = NULL;  /* "Hybrid" */
static lv_obj_t *s_mode_sub        = NULL;  /* "LOCAL + CLOUD" */

/* Now-slot card (widget live target + empty-state) */
static lv_obj_t *s_now_card        = NULL;
static lv_obj_t *s_now_accent      = NULL;  /* 140×3 amber bar top-left */
static lv_obj_t *s_now_kicker      = NULL;
static lv_obj_t *s_now_lede        = NULL;
static lv_obj_t *s_stat_k[3]       = {NULL, NULL, NULL};
static lv_obj_t *s_stat_v[3]       = {NULL, NULL, NULL};

/* Bottom strip */
static lv_obj_t *s_say_pill        = NULL;
static lv_obj_t *s_say_mic         = NULL;
static lv_obj_t *s_say_label_main  = NULL;
static lv_obj_t *s_say_label_sub   = NULL;
static lv_obj_t *s_rail            = NULL;
static lv_obj_t *s_rail_btn[4]     = {NULL, NULL, NULL, NULL};

/* Toast + timer */
static lv_obj_t   *s_toast         = NULL;
static lv_timer_t *s_refresh_timer = NULL;

/* Mode colors + labels */
static const uint32_t s_mode_tint[4] = {
    TH_MODE_LOCAL, TH_MODE_HYBRID, TH_MODE_CLOUD, TH_MODE_CLAW,
};
static const char *s_mode_short[4]  = { "Local",   "Hybrid",  "Cloud",  "Claw"   };
static const char *s_mode_tagline[4] = {
    "ON-DEVICE",
    "LOCAL + CLOUD",
    "CLOUD ONLY",
    "TINKERCLAW",
};

static uint8_t s_badge_mode = 0;

/* ── Forward decls ───────────────────────────────────────────── */
static void refresh_timer_cb(lv_timer_t *t);
static void orb_click_cb(lv_event_t *e);
static void orb_long_press_cb(lv_event_t *e);
static void screen_gesture_cb(lv_event_t *e);
static void now_card_click_cb(lv_event_t *e);
static void sys_click_cb(lv_event_t *e);
static void mode_chip_long_press_cb(lv_event_t *e);
static void rail_home_cb(lv_event_t *e);
static void rail_threads_cb(lv_event_t *e);
static void rail_notes_cb(lv_event_t *e);
static void rail_settings_cb(lv_event_t *e);
static bool any_overlay_visible(void);
static void show_toast_internal(const char *text);
static void orb_paint_for_mode(uint8_t mode);
static void orb_paint_for_tone(widget_tone_t tone);

/* ── Helpers ─────────────────────────────────────────────────── */

static const char *tone_name(widget_tone_t t)
{
    switch (t) {
        case WIDGET_TONE_CALM:        return "CALM";
        case WIDGET_TONE_ACTIVE:      return "ACTIVE";
        case WIDGET_TONE_APPROACHING: return "APPROACH";
        case WIDGET_TONE_URGENT:      return "URGENT";
        case WIDGET_TONE_DONE:        return "DONE";
        case WIDGET_TONE_INFO:        return "INFO";
        case WIDGET_TONE_SUCCESS:     return "OK";
        case WIDGET_TONE_ALERT:       return "ALERT";
        default:                      return "--";
    }
}

static const char *greeting_for_hour(int hour)
{
    if (hour < 5)  return "late night, emile";
    if (hour < 12) return "morning, emile";
    if (hour < 17) return "afternoon, emile";
    if (hour < 21) return "evening, emile";
    return "tonight, emile";
}

/* "Thursday · 9:42" — bullet is U+2022, which IS in the Montserrat subset. */
static void format_right_time(char *buf, size_t n, const struct tm *tm)
{
    static const char *wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday" };
    int w = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
    snprintf(buf, n, "%s \xe2\x80\xa2 %d:%02d",
             wd[w], tm->tm_hour, tm->tm_min);
}

/* ── Orb painting (preserved from v5) ─────────────────────────── */

static void orb_paint_for_mode(uint8_t mode)
{
    if (!s_orb) return;
    if (mode >= 4) mode = 0;
    uint32_t top, bot;
    switch (mode) {
        case VOICE_MODE_LOCAL:      top = 0x7DE69F; bot = 0x166C3A; break;
        case VOICE_MODE_HYBRID:     top = 0xFFC75A; bot = 0xB9650A; break;
        case VOICE_MODE_CLOUD:      top = 0x9AC0F9; bot = 0x1D3A78; break;
        case VOICE_MODE_TINKERCLAW: top = 0xFF7E95; bot = 0x7A1428; break;
        default:                    top = 0xFFC75A; bot = 0xB9650A; break;
    }
    lv_obj_set_style_bg_color(s_orb, lv_color_hex(top), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_orb, lv_color_hex(bot), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_orb, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_orb, LV_OPA_COVER, LV_PART_MAIN);
}

/* Widget-tone orb override — used when a live widget claims the slot.
 * calm=emerald, active=amber, approaching=amber-hot, urgent=rose, done=settled. */
static void orb_paint_for_tone(widget_tone_t tone)
{
    if (!s_orb) return;
    uint32_t top, bot;
    switch (tone) {
        case WIDGET_TONE_CALM:        top = 0x7DE69F; bot = 0x166C3A; break;
        case WIDGET_TONE_ACTIVE:      top = 0xFFC75A; bot = 0xB9650A; break;
        case WIDGET_TONE_APPROACHING: top = 0xFFB637; bot = 0xD97706; break;
        case WIDGET_TONE_URGENT:      top = 0xFF7E95; bot = 0xF43F5E; break;
        case WIDGET_TONE_DONE:        top = 0xBBFFCC; bot = 0x0E5E2A; break;
        default:                      top = 0xFFC75A; bot = 0xB9650A; break;
    }
    lv_obj_set_style_bg_color(s_orb, lv_color_hex(top), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_orb, lv_color_hex(bot), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_orb, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_orb, LV_OPA_COVER, LV_PART_MAIN);
}

static void update_mode_ui(uint8_t mode)
{
    if (mode >= 4) mode = 0;
    s_badge_mode = mode;
    if (s_mode_dot) {
        lv_obj_set_style_bg_color(s_mode_dot, lv_color_hex(s_mode_tint[mode]), 0);
    }
    if (s_mode_name) lv_label_set_text(s_mode_name, s_mode_short[mode]);
    if (s_mode_sub)  lv_label_set_text(s_mode_sub,  s_mode_tagline[mode]);
    orb_paint_for_mode(mode);
}

/* ── Centered placement helper ──────────────────────────────────
 * Parent is assumed to be s_screen (no flex), so absolute x = center - w/2. */
static void pos_centered(lv_obj_t *obj, int y, int w, int h)
{
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, (SW - w) / 2, y);
}

/* ── Screen build ───────────────────────────────────────────── */

lv_obj_t *ui_home_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, SW, SH);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_screen);
    lv_obj_add_event_cb(s_screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* ── Edge ticks (keep swipe affordance) ── */
    lv_obj_t *edge_l = lv_obj_create(s_screen);
    lv_obj_remove_style_all(edge_l);
    lv_obj_set_size(edge_l, 2, 40);
    lv_obj_set_pos(edge_l, 0, SH / 2 - 20);
    lv_obj_set_style_bg_color(edge_l, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(edge_l, 60, 0);
    lv_obj_set_style_radius(edge_l, 2, 0);

    lv_obj_t *edge_r = lv_obj_create(s_screen);
    lv_obj_remove_style_all(edge_r);
    lv_obj_set_size(edge_r, 2, 40);
    lv_obj_set_pos(edge_r, SW - 2, SH / 2 - 20);
    lv_obj_set_style_bg_color(edge_r, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(edge_r, 60, 0);
    lv_obj_set_style_radius(edge_r, 2, 0);

    /* ── Status strip (y=0..56) ────────────────────────────── */
    s_sys_dot = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_sys_dot);
    lv_obj_set_size(s_sys_dot, 8, 8);
    lv_obj_set_pos(s_sys_dot, SIDE_PAD, 32);
    lv_obj_set_style_radius(s_sys_dot, 4, 0);
    lv_obj_set_style_bg_color(s_sys_dot, lv_color_hex(TH_STATUS_GREEN), 0);
    lv_obj_set_style_bg_opa(s_sys_dot, LV_OPA_COVER, 0);

    s_sys_label = lv_label_create(s_screen);
    lv_label_set_text(s_sys_label, "ONLINE");
    lv_obj_set_pos(s_sys_label, SIDE_PAD + 18, 26);
    lv_obj_set_style_text_font(s_sys_label, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_sys_label, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(s_sys_label, 3, 0);
    lv_obj_add_flag(s_sys_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sys_label, sys_click_cb, LV_EVENT_CLICKED, NULL);

    s_time_label = lv_label_create(s_screen);
    lv_label_set_text(s_time_label, "");
    lv_obj_set_width(s_time_label, 320);
    lv_obj_set_pos(s_time_label, SW - SIDE_PAD - 320, 24);
    lv_obj_set_style_text_font(s_time_label, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_letter_space(s_time_label, 1, 0);

    /* ── Orb stage (halos + rings + orb) ───────────────────── */
    /* Halos: dim amber circles, stacked largest-first so opacity adds. */
    s_halo_outer = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_halo_outer);
    pos_centered(s_halo_outer, ORB_CY - HALO_OUTER / 2, HALO_OUTER, HALO_OUTER);
    lv_obj_set_style_radius(s_halo_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_halo_outer, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(s_halo_outer, 18, 0);   /* ~7% */
    lv_obj_clear_flag(s_halo_outer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_halo_inner = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_halo_inner);
    pos_centered(s_halo_inner, ORB_CY - HALO_INNER / 2, HALO_INNER, HALO_INNER);
    lv_obj_set_style_radius(s_halo_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_halo_inner, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(s_halo_inner, 38, 0);   /* ~15% */
    lv_obj_clear_flag(s_halo_inner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Rings: border-only circles for a "composed" orb. */
    s_ring_outer = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_ring_outer);
    pos_centered(s_ring_outer, ORB_CY - RING_OUTER / 2, RING_OUTER, RING_OUTER);
    lv_obj_set_style_radius(s_ring_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ring_outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring_outer, 1, 0);
    lv_obj_set_style_border_color(s_ring_outer, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_border_opa(s_ring_outer, 60, 0);
    lv_obj_clear_flag(s_ring_outer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_ring_mid = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_ring_mid);
    pos_centered(s_ring_mid, ORB_CY - RING_MID / 2, RING_MID, RING_MID);
    lv_obj_set_style_radius(s_ring_mid, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ring_mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring_mid, 1, 0);
    lv_obj_set_style_border_color(s_ring_mid, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_border_opa(s_ring_mid, 90, 0);
    lv_obj_clear_flag(s_ring_mid, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_ring_inner = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_ring_inner);
    pos_centered(s_ring_inner, ORB_CY - RING_INNER / 2, RING_INNER, RING_INNER);
    lv_obj_set_style_radius(s_ring_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ring_inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring_inner, 1, 0);
    lv_obj_set_style_border_color(s_ring_inner, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_border_opa(s_ring_inner, 140, 0);
    lv_obj_clear_flag(s_ring_inner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* The orb itself — 156 px, 2-stop vertical gradient, no shadow. */
    s_orb = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_orb);
    pos_centered(s_orb, ORB_CY - ORB_SIZE / 2, ORB_SIZE, ORB_SIZE);
    lv_obj_set_style_radius(s_orb, LV_RADIUS_CIRCLE, 0);
    orb_paint_for_mode(s_badge_mode);
    lv_obj_add_flag(s_orb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_orb, orb_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_orb, orb_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* ── State word + greet line (below orb) ───────────────── */
    s_state_word = lv_label_create(s_screen);
    lv_label_set_text(s_state_word, "ready");
    lv_obj_set_style_text_font(s_state_word, FONT_CLOCK, 0);  /* Montserrat 48 */
    lv_obj_set_style_text_color(s_state_word, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(s_state_word, -1, 0);
    lv_obj_set_width(s_state_word, SW);
    lv_obj_set_style_text_align(s_state_word, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_state_word, 0, 555);

    s_greet_line = lv_label_create(s_screen);
    lv_label_set_text(s_greet_line, "morning, emile");
    lv_obj_set_style_text_font(s_greet_line, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_greet_line, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(s_greet_line, 5, 0);
    lv_obj_set_width(s_greet_line, SW);
    lv_obj_set_style_text_align(s_greet_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_greet_line, 0, 630);

    /* ── Mode chip (pill, centered at y=680) ─────────────────── */
    s_mode_chip = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_mode_chip);
    pos_centered(s_mode_chip, 680, 360, 52);
    lv_obj_set_style_bg_color(s_mode_chip, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(s_mode_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mode_chip, 26, 0);
    lv_obj_set_style_border_width(s_mode_chip, 1, 0);
    lv_obj_set_style_border_color(s_mode_chip, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(s_mode_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mode_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mode_chip, mode_chip_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    s_mode_dot = lv_obj_create(s_mode_chip);
    lv_obj_remove_style_all(s_mode_dot);
    lv_obj_set_size(s_mode_dot, 10, 10);
    lv_obj_set_pos(s_mode_dot, 24, 21);
    lv_obj_set_style_radius(s_mode_dot, 5, 0);
    lv_obj_set_style_bg_color(s_mode_dot, lv_color_hex(s_mode_tint[s_badge_mode]), 0);
    lv_obj_set_style_bg_opa(s_mode_dot, LV_OPA_COVER, 0);

    s_mode_name = lv_label_create(s_mode_chip);
    lv_label_set_text(s_mode_name, s_mode_short[s_badge_mode]);
    lv_obj_set_pos(s_mode_name, 44, 15);
    lv_obj_set_style_text_font(s_mode_name, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s_mode_name, lv_color_hex(TH_TEXT_PRIMARY), 0);

    s_mode_sub = lv_label_create(s_mode_chip);
    lv_label_set_text(s_mode_sub, s_mode_tagline[s_badge_mode]);
    lv_obj_set_pos(s_mode_sub, 140, 19);
    lv_obj_set_style_text_font(s_mode_sub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_mode_sub, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(s_mode_sub, 2, 0);

    /* ── Now-slot card ─────────────────────────────────────── */
    s_now_card = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_now_card);
    lv_obj_set_pos(s_now_card, CARD_X, CARD_Y);
    lv_obj_set_size(s_now_card, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(s_now_card, lv_color_hex(TH_CARD), 0);
    lv_obj_set_style_bg_opa(s_now_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_now_card, 24, 0);
    lv_obj_set_style_border_width(s_now_card, 1, 0);
    lv_obj_set_style_border_color(s_now_card, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(s_now_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_now_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_now_card, now_card_click_cb, LV_EVENT_CLICKED, NULL);

    /* Accent bar top-left (140×3 amber) */
    s_now_accent = lv_obj_create(s_now_card);
    lv_obj_remove_style_all(s_now_accent);
    lv_obj_set_size(s_now_accent, 140, 3);
    lv_obj_set_pos(s_now_accent, 0, 0);
    lv_obj_set_style_bg_color(s_now_accent, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(s_now_accent, LV_OPA_COVER, 0);

    /* Kicker */
    s_now_kicker = lv_label_create(s_now_card);
    lv_label_set_text(s_now_kicker, "NOW \xe2\x80\xa2 STANDING BY");
    lv_obj_set_pos(s_now_kicker, CARD_PAD, CARD_PAD);
    lv_obj_set_style_text_font(s_now_kicker, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_now_kicker, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(s_now_kicker, 4, 0);

    /* Lede */
    s_now_lede = lv_label_create(s_now_card);
    lv_label_set_long_mode(s_now_lede, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_now_lede, "Tap the orb to ask something.");
    lv_obj_set_pos(s_now_lede, CARD_PAD, CARD_PAD + 26);
    lv_obj_set_width(s_now_lede, CARD_W - CARD_PAD * 2);
    lv_obj_set_style_text_font(s_now_lede, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s_now_lede, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_line_space(s_now_lede, 4, 0);

    /* Stats row — 3 cells, hairline divider above */
    lv_obj_t *stats_div = lv_obj_create(s_now_card);
    lv_obj_remove_style_all(stats_div);
    lv_obj_set_size(stats_div, CARD_W - CARD_PAD * 2, 1);
    lv_obj_set_pos(stats_div, CARD_PAD, CARD_H - 72);
    lv_obj_set_style_bg_color(stats_div, lv_color_hex(0x1E1E2A), 0);
    lv_obj_set_style_bg_opa(stats_div, LV_OPA_COVER, 0);

    const int cell_w = (CARD_W - CARD_PAD * 2) / 3;
    for (int i = 0; i < 3; i++) {
        s_stat_k[i] = lv_label_create(s_now_card);
        lv_obj_set_pos(s_stat_k[i], CARD_PAD + cell_w * i, CARD_H - 62);
        lv_obj_set_style_text_font(s_stat_k[i], FONT_SMALL, 0);
        lv_obj_set_style_text_color(s_stat_k[i], lv_color_hex(TH_TEXT_DIM), 0);
        lv_obj_set_style_text_letter_space(s_stat_k[i], 3, 0);
        lv_label_set_text(s_stat_k[i], "");

        s_stat_v[i] = lv_label_create(s_now_card);
        lv_obj_set_pos(s_stat_v[i], CARD_PAD + cell_w * i, CARD_H - 40);
        lv_obj_set_style_text_font(s_stat_v[i], FONT_HEADING, 0);
        lv_obj_set_style_text_color(s_stat_v[i], lv_color_hex(TH_TEXT_PRIMARY), 0);
        lv_label_set_text(s_stat_v[i], "");
    }

    /* ── Say pill (108 px) ─────────────────────────────────── */
    const int strip_y = SH - STRIP_BOT_PAD - RAIL_H - SAY_GAP - SAY_H;
    s_say_pill = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_say_pill);
    lv_obj_set_pos(s_say_pill, SIDE_PAD, strip_y);
    lv_obj_set_size(s_say_pill, CARD_W, SAY_H);
    lv_obj_set_style_bg_color(s_say_pill, lv_color_hex(TH_CARD), 0);
    lv_obj_set_style_bg_opa(s_say_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_say_pill, 54, 0);
    lv_obj_set_style_border_width(s_say_pill, 1, 0);
    lv_obj_set_style_border_color(s_say_pill, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(s_say_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_say_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_say_pill, orb_click_cb, LV_EVENT_CLICKED, NULL);

    /* 84 px amber mic disc — inner element, left-aligned inside pill */
    s_say_mic = lv_obj_create(s_say_pill);
    lv_obj_remove_style_all(s_say_mic);
    lv_obj_set_size(s_say_mic, 84, 84);
    lv_obj_set_pos(s_say_mic, 12, 12);
    lv_obj_set_style_radius(s_say_mic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_say_mic, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_grad_color(s_say_mic, lv_color_hex(TH_AMBER_DARK), 0);
    lv_obj_set_style_bg_grad_dir(s_say_mic, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_say_mic, LV_OPA_COVER, 0);

    /* Label text inside mic — use text chevron placeholder. We don't ship a
     * mic glyph in the Montserrat subset, so a compact ASCII mark keeps the
     * pill legible until an icon font is added. */
    lv_obj_t *mic_mark = lv_label_create(s_say_mic);
    lv_label_set_text(mic_mark, "\xe2\x80\xa2");  /* U+2022 as placeholder dot */
    lv_obj_set_style_text_font(mic_mark, FONT_CLOCK, 0);
    lv_obj_set_style_text_color(mic_mark, lv_color_hex(TH_BG), 0);
    lv_obj_align(mic_mark, LV_ALIGN_CENTER, 0, -6);

    s_say_label_main = lv_label_create(s_say_pill);
    lv_label_set_text(s_say_label_main, "Hold to speak");
    lv_obj_set_pos(s_say_label_main, 116, 26);
    lv_obj_set_style_text_font(s_say_label_main, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s_say_label_main, lv_color_hex(TH_TEXT_PRIMARY), 0);

    s_say_label_sub = lv_label_create(s_say_pill);
    lv_label_set_text(s_say_label_sub, "OR SAY \"DRAGON\"");
    lv_obj_set_pos(s_say_label_sub, 116, 60);
    lv_obj_set_style_text_font(s_say_label_sub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_say_label_sub, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(s_say_label_sub, 4, 0);

    /* ── Rail (4 chips) ─────────────────────────────────────── */
    const int rail_y = SH - STRIP_BOT_PAD - RAIL_H;
    s_rail = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_rail);
    lv_obj_set_pos(s_rail, SIDE_PAD, rail_y);
    lv_obj_set_size(s_rail, CARD_W, RAIL_H);
    lv_obj_clear_flag(s_rail, LV_OBJ_FLAG_SCROLLABLE);

    const int rail_gap = 10;
    const int chip_w = (CARD_W - rail_gap * 3) / 4;
    const char *rail_names[4] = { "home", "chat", "notes", "settings" };
    lv_event_cb_t rail_cbs[4] = { rail_home_cb, rail_threads_cb, rail_notes_cb, rail_settings_cb };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(s_rail);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, chip_w, RAIL_H);
        lv_obj_set_pos(b, i * (chip_w + rail_gap), 0);
        bool is_home = (i == 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(is_home ? TH_CARD_ELEVATED : TH_CARD), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(is_home ? 0x24243a : 0x1E1E2A), 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, rail_cbs[i], LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, rail_names[i]);
        lv_obj_set_style_text_font(lbl, FONT_SMALL, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(is_home ? TH_TEXT_PRIMARY : TH_TEXT_SECONDARY), 0);
        lv_obj_center(lbl);

        s_rail_btn[i] = b;
    }

    /* Initial fill */
    ui_home_update_status();

    if (s_refresh_timer == NULL) {
        /* 2 s safety-net poll — the real state push is from voice.c via
         * voice_set_state() + ws_receive_task cleanup, which fires within
         * one LVGL tick on disconnect. This timer just catches anything
         * the push missed (e.g. a state change during LVGL-lock contention
         * where the fallback ui_home_refresh_sys_label() also got delayed). */
        s_refresh_timer = lv_timer_create(refresh_timer_cb, 2000, NULL);
    }

    ESP_LOGI(TAG, "v4 Ambient Canvas home created (orb %dpx, mode %d)",
             ORB_SIZE, s_badge_mode);
    return s_screen;
}

/* ── Periodic status refresh ─────────────────────────────────── */

void ui_home_update_status(void)
{
    time_t now = 0; time(&now);
    struct tm tm_local;
    localtime_r(&now, &tm_local);

    /* Greet line (shifts with hour) */
    if (s_greet_line) {
        const char *g = greeting_for_hour(tm_local.tm_hour);
        const char *cur = lv_label_get_text(s_greet_line);
        if (!cur || strcmp(cur, g) != 0) lv_label_set_text(s_greet_line, g);
    }

    /* Right-side time label */
    if (s_time_label) {
        char buf[48];
        format_right_time(buf, sizeof(buf), &tm_local);
        const char *cur = lv_label_get_text(s_time_label);
        if (!cur || strcmp(cur, buf) != 0) lv_label_set_text(s_time_label, buf);
    }

    /* Edge-state detection — same priority as v5. */
    bool wifi_ok = tab5_wifi_connected();
    bool dragon  = voice_is_connected();
    bool mic_off = tab5_settings_get_mic_mute() != 0;
    bool quiet   = tab5_settings_quiet_active(tm_local.tm_hour);

    enum { ST_NORMAL, ST_NO_WIFI, ST_DRAGON_DOWN, ST_MUTED, ST_QUIET } state;
    if (!wifi_ok)     state = ST_NO_WIFI;
    else if (!dragon) state = ST_DRAGON_DOWN;
    else if (mic_off) state = ST_MUTED;
    else if (quiet)   state = ST_QUIET;
    else              state = ST_NORMAL;

    /* Voice sub-state (overrides "ready" when something is happening) */
    voice_state_t vs = voice_get_state();
    const char *state_hint = NULL;
    if (dragon) {
        switch (vs) {
            case VOICE_STATE_LISTENING:  state_hint = "listening"; break;
            case VOICE_STATE_PROCESSING: state_hint = "thinking";  break;
            case VOICE_STATE_SPEAKING:   state_hint = "speaking";  break;
            default: break;
        }
    }

    /* State word below the orb */
    if (s_state_word) {
        const char *word;
        uint32_t col = TH_AMBER;
        switch (state) {
            case ST_NO_WIFI:      word = "no wi-fi";  col = TH_STATUS_RED; break;
            case ST_DRAGON_DOWN:  word = "offline";   col = TH_STATUS_RED; break;
            case ST_MUTED:        word = "muted";     col = 0x7A7A82;      break;
            case ST_QUIET:        word = "quiet";     col = 0x7A7A82;      break;
            default:              word = state_hint ? state_hint : "ready"; break;
        }
        const char *cur = lv_label_get_text(s_state_word);
        if (!cur || strcmp(cur, word) != 0) lv_label_set_text(s_state_word, word);
        lv_obj_set_style_text_color(s_state_word, lv_color_hex(col), 0);
    }

    /* Sys label (left of status strip) */
    if (s_sys_label) {
        const char *sys;
        uint32_t col = TH_TEXT_SECONDARY;
        switch (state) {
            case ST_NO_WIFI:     sys = "OFFLINE";   col = TH_STATUS_RED; break;
            case ST_DRAGON_DOWN: sys = "NO DRAGON"; col = TH_STATUS_RED; break;
            case ST_MUTED:       sys = "MUTED";     col = 0x7A7A82;      break;
            case ST_QUIET:       sys = "QUIET";     col = 0x7A7A82;      break;
            default:             sys = "ONLINE";    col = TH_TEXT_SECONDARY; break;
        }
        const char *cur = lv_label_get_text(s_sys_label);
        if (!cur || strcmp(cur, sys) != 0) lv_label_set_text(s_sys_label, sys);
        lv_obj_set_style_text_color(s_sys_label, lv_color_hex(col), 0);
    }
    if (s_sys_dot) {
        uint32_t col;
        switch (state) {
            case ST_NO_WIFI:
            case ST_DRAGON_DOWN: col = TH_STATUS_RED;   break;
            case ST_MUTED:
            case ST_QUIET:       col = 0x5C5C68;        break;
            default:             col = TH_STATUS_GREEN; break;
        }
        lv_obj_set_style_bg_color(s_sys_dot, lv_color_hex(col), 0);
    }

    /* Now-slot — widget live wins; otherwise empty-state. */
    widget_t *live_w = widget_store_live_active();
    if (live_w) {
        char kicker[80];
        /* NOW · <skill_id in caps> */
        snprintf(kicker, sizeof(kicker), "NOW \xe2\x80\xa2 %.*s",
                 (int)(sizeof(kicker) - 8),
                 live_w->skill_id[0] ? live_w->skill_id : "WIDGET");
        for (int i = 0; kicker[i]; i++) {
            if (kicker[i] >= 'a' && kicker[i] <= 'z') kicker[i] -= 32;
        }
        if (s_now_kicker) {
            const char *cur = lv_label_get_text(s_now_kicker);
            if (!cur || strcmp(cur, kicker) != 0) lv_label_set_text(s_now_kicker, kicker);
        }
        if (s_now_lede) {
            /* title on top, body below (matches widget spec §3.1) */
            char buf[260];
            snprintf(buf, sizeof(buf), "%.63s\n%.180s",
                     live_w->title[0] ? live_w->title : "",
                     live_w->body[0]  ? live_w->body  : "");
            const char *cur = lv_label_get_text(s_now_lede);
            if (!cur || strcmp(cur, buf) != 0) lv_label_set_text(s_now_lede, buf);
        }
        /* Stats: progress if available, otherwise tone-coded state. */
        if (s_stat_k[0] && s_stat_v[0]) {
            int pct = (int)(live_w->progress * 100.0f + 0.5f);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            lv_label_set_text(s_stat_k[0], "PROGRESS");
            char pbuf[16]; snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
            lv_label_set_text(s_stat_v[0], pbuf);
            lv_obj_set_style_text_color(s_stat_v[0], lv_color_hex(TH_AMBER), 0);
        }
        if (s_stat_k[1] && s_stat_v[1]) {
            lv_label_set_text(s_stat_k[1], "TONE");
            lv_label_set_text(s_stat_v[1], tone_name(live_w->tone));
            lv_obj_set_style_text_color(s_stat_v[1], lv_color_hex(TH_TEXT_PRIMARY), 0);
        }
        if (s_stat_k[2] && s_stat_v[2]) {
            lv_label_set_text(s_stat_k[2], "PRIORITY");
            char buf[8]; snprintf(buf, sizeof(buf), "%d", live_w->priority);
            lv_label_set_text(s_stat_v[2], buf);
            lv_obj_set_style_text_color(s_stat_v[2], lv_color_hex(TH_TEXT_PRIMARY), 0);
        }
        orb_paint_for_tone(live_w->tone);
    } else {
        /* Empty-state kicker + lede */
        const char *kicker = "NOW \xe2\x80\xa2 STANDING BY";
        const char *lede;
        switch (state) {
            case ST_NO_WIFI:
                lede = "Can't reach the network.\nTap the sys label to scan.";
                break;
            case ST_DRAGON_DOWN:
                lede = "Dragon is unreachable.\nVoice notes save locally.";
                break;
            case ST_MUTED:
                lede = "Mic off.\nTap the orb when you want me.";
                break;
            case ST_QUIET:
                lede = "Quiet hours.\nI won't speak until 07:00.";
                break;
            default:
                lede = "Tap the orb to ask something.";
                break;
        }
        if (s_now_kicker) {
            const char *cur = lv_label_get_text(s_now_kicker);
            if (!cur || strcmp(cur, kicker) != 0) lv_label_set_text(s_now_kicker, kicker);
        }
        if (s_now_lede) {
            const char *cur = lv_label_get_text(s_now_lede);
            if (!cur || strcmp(cur, lede) != 0) lv_label_set_text(s_now_lede, lede);
        }
        /* Default stats: voice mode, battery, wifi — always something here. */
        if (s_stat_k[0] && s_stat_v[0]) {
            lv_label_set_text(s_stat_k[0], "VOICE");
            lv_label_set_text(s_stat_v[0], s_mode_short[s_badge_mode]);
            lv_obj_set_style_text_color(s_stat_v[0], lv_color_hex(TH_AMBER), 0);
        }
        if (s_stat_k[1] && s_stat_v[1]) {
            lv_label_set_text(s_stat_k[1], "BATTERY");
            char b[8];
            snprintf(b, sizeof(b), "%d%%", (int)tab5_battery_percent());
            lv_label_set_text(s_stat_v[1], b);
            lv_obj_set_style_text_color(s_stat_v[1], lv_color_hex(TH_TEXT_PRIMARY), 0);
        }
        if (s_stat_k[2] && s_stat_v[2]) {
            lv_label_set_text(s_stat_k[2], "NET");
            lv_label_set_text(s_stat_v[2], wifi_ok ? "OK" : "OFF");
            lv_obj_set_style_text_color(s_stat_v[2],
                lv_color_hex(wifi_ok ? TH_TEXT_PRIMARY : TH_STATUS_RED), 0);
        }
        orb_paint_for_mode(s_badge_mode);
    }

    /* Mode chip (re-read from NVS) */
    uint8_t current_mode = tab5_settings_get_voice_mode();
    if (current_mode != s_badge_mode) update_mode_ui(current_mode);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_home_update_status();
}

/* ── Interactions ────────────────────────────────────────────── */

static void orb_click_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;

    /* Tap debounce — prevents SDIO TX copy_buff exhaustion from repeat-taps.
     * Same 500 ms window as v5. */
    static uint32_t last_tap_ms = 0;
    uint32_t now = lv_tick_get();
    if (now - last_tap_ms < 500) {
        ESP_LOGI(TAG, "orb/pill tap debounced (dt=%lums)",
                 (unsigned long)(now - last_tap_ms));
        return;
    }
    last_tap_ms = now;

    ESP_LOGI(TAG, "orb/pill tap -> open voice");
    if (!voice_is_connected()) {
        char dhost[64];
        tab5_settings_get_dragon_host(dhost, sizeof(dhost));
        if (dhost[0]) voice_connect_async(dhost, TAB5_VOICE_PORT, false);
    }
    ui_voice_show();
    voice_start_listening();
}

static void orb_long_press_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    uint8_t next = (s_badge_mode + 1) % VOICE_MODE_COUNT;
    ESP_LOGI(TAG, "orb long-pressed -> cycle mode %d -> %d", s_badge_mode, next);
    tab5_settings_set_voice_mode(next);
    char model[64];
    tab5_settings_get_llm_model(model, sizeof(model));
    voice_send_config_update(next, model);
    update_mode_ui(next);

    char buf[48];
    snprintf(buf, sizeof(buf), "Mode: %s", s_mode_short[next]);
    show_toast_internal(buf);
}

static void mode_chip_long_press_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    /* Same behavior as long-pressing the orb — cycles modes.
     * TODO: swap for a segmented picker sheet when lv_menu is wired. */
    uint8_t next = (s_badge_mode + 1) % VOICE_MODE_COUNT;
    tab5_settings_set_voice_mode(next);
    char model[64];
    tab5_settings_get_llm_model(model, sizeof(model));
    voice_send_config_update(next, model);
    update_mode_ui(next);
    char buf[48];
    snprintf(buf, sizeof(buf), "Mode: %s", s_mode_short[next]);
    show_toast_internal(buf);
}

static void now_card_click_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    ESP_LOGI(TAG, "now-card tapped -> Agents");
    ui_agents_show();
}

static void sys_click_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    ESP_LOGI(TAG, "sys label tapped -> Memory");
    ui_memory_show();
}

/* Rail handlers */
static void rail_home_cb(lv_event_t *e)     { (void)e; /* already home */ }
static void async_open_chat(void *arg)
{
    (void)arg;
    extern lv_obj_t *ui_chat_create(void);
    ui_chat_create();
}
static void rail_threads_cb(lv_event_t *e)  { (void)e; if (!any_overlay_visible()) lv_async_call(async_open_chat, NULL); }
static void async_open_notes_rail(void *arg)
{
    (void)arg;
    extern lv_obj_t *ui_notes_create(void);
    ui_notes_create();
}
static void rail_notes_cb(lv_event_t *e)    { (void)e; if (!any_overlay_visible()) lv_async_call(async_open_notes_rail, NULL); }
static void rail_settings_cb(lv_event_t *e) { (void)e; if (!any_overlay_visible()) ui_settings_create(); }

static bool any_overlay_visible(void)
{
    extern bool ui_notes_is_visible(void);
    extern bool ui_settings_is_visible(void);
    extern bool ui_sessions_is_visible(void);
    if (ui_chat_is_active())      return true;
    if (ui_agents_is_visible())   return true;
    if (ui_memory_is_visible())   return true;
    if (ui_focus_is_visible())    return true;
    if (ui_notes_is_visible())    return true;
    if (ui_settings_is_visible()) return true;
    if (ui_sessions_is_visible()) return true;
    return false;
}

/* Async open callbacks — deferred so the originating gesture fully dispatches
   before we create the overlay. Prevents swipe-right on home from opening
   Notes, then bubbling to Notes' own swipe-right=back and tearing it down. */
static void async_open_focus(void *arg)    { (void)arg; ui_focus_show(); }
static void async_open_notes(void *arg)
{
    (void)arg;
    extern lv_obj_t *ui_notes_create(void);
    ui_notes_create();
}
static void async_open_settings(void *arg)
{
    (void)arg;
    extern lv_obj_t *ui_settings_create(void);
    ui_settings_create();
}

static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    switch (dir) {
        case LV_DIR_TOP:    lv_async_call(async_open_focus,    NULL); break;
        case LV_DIR_RIGHT:  lv_async_call(async_open_notes,    NULL); break;
        case LV_DIR_LEFT:   lv_async_call(async_open_settings, NULL); break;
        default: break;
    }
}

/* ── Toast ───────────────────────────────────────────────────── */

typedef struct { lv_timer_t *t; lv_obj_t *obj; } toast_ctx_t;

static void toast_remove_cb(lv_timer_t *t)
{
    toast_ctx_t *ctx = (toast_ctx_t *)lv_timer_get_user_data(t);
    if (ctx) {
        if (ctx->obj) lv_obj_del(ctx->obj);
        if (s_toast == ctx->obj) s_toast = NULL;
        free(ctx);
    }
    lv_timer_delete(t);
}

static void show_toast_internal(const char *text)
{
    if (!text) return;
    if (s_toast) { lv_obj_del(s_toast); s_toast = NULL; }

    lv_obj_t *t = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(t, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(t, 240, 0);
    lv_obj_set_style_radius(t, 18, 0);
    lv_obj_set_style_pad_hor(t, 22, 0);
    lv_obj_set_style_pad_ver(t, 14, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(TH_CARD_BORDER), 0);

    lv_obj_t *lbl = lv_label_create(t);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);

    lv_obj_align(t, LV_ALIGN_CENTER, 0, 200);
    s_toast = t;

    toast_ctx_t *ctx = (toast_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx) {
        ctx->obj = t;
        ctx->t = lv_timer_create(toast_remove_cb, 2200, ctx);
        lv_timer_set_repeat_count(ctx->t, 1);
    }
}

/* ── Public API ──────────────────────────────────────────────── */

void ui_home_destroy(void)
{
    if (s_refresh_timer) { lv_timer_delete(s_refresh_timer); s_refresh_timer = NULL; }
    if (s_screen) { lv_obj_del(s_screen); s_screen = NULL; }
    s_sys_dot = s_sys_label = s_time_label = NULL;
    s_halo_outer = s_halo_inner = NULL;
    s_ring_outer = s_ring_mid = s_ring_inner = NULL;
    s_orb = NULL;
    s_state_word = s_greet_line = NULL;
    s_mode_chip = s_mode_dot = s_mode_name = s_mode_sub = NULL;
    s_now_card = s_now_accent = s_now_kicker = s_now_lede = NULL;
    for (int i = 0; i < 3; i++) s_stat_k[i] = s_stat_v[i] = NULL;
    s_say_pill = s_say_mic = s_say_label_main = s_say_label_sub = NULL;
    s_rail = NULL;
    for (int i = 0; i < 4; i++) s_rail_btn[i] = NULL;
    s_toast = NULL;
}

lv_obj_t *ui_home_get_screen(void) { return s_screen; }

void ui_home_go_home(void)
{
    if (ui_chat_is_active()) ui_chat_hide();
}

lv_obj_t *ui_home_get_tileview(void) { return NULL; }

lv_obj_t *ui_home_get_tile(int page)
{
    return (page == 0) ? s_screen : NULL;
}

void ui_home_nav_settings(void) { ui_settings_create(); }

void ui_home_refresh_mode_badge(void)
{
    uint8_t m = tab5_settings_get_voice_mode();
    update_mode_ui(m);
}

void ui_home_show_toast(const char *text) { show_toast_internal(text); }

/* Voice-state-driven state-word refresh — marshalled onto the LVGL thread. */
static void sys_label_refresh_async_cb(void *arg)
{
    (void)arg;
    ui_home_update_status();
}

void ui_home_refresh_sys_label(void)
{
    lv_async_call(sys_label_refresh_async_cb, NULL);
}
