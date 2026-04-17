/*
 * ui_home.c — TinkerOS Home Screen — v5 "Zero Interface" (FLAT density)
 *
 * Voice-first layout for a 720x1280 portrait device that sits on a desk.
 * Single page, no tileview, no bottom nav. Hero orb, hero text, poem line.
 *
 * FLAT density = native LVGL primitives only. 2-stop linear gradient on the
 * orb (no halo/rings/pulse — those need pre-baked images and are gated behind
 * the MINIMAL/HARDCORE density toggles in Settings, implemented later).
 *
 * Interactions:
 *   Tap orb            → voice listen overlay
 *   Long-press orb     → cycle voice mode (Local → Hybrid → Cloud → TinkerClaw)
 *   Swipe up from bot  → open Chat (focus state proxy for now)
 *   Swipe from left    → open Notes
 *   Swipe from right   → open Settings
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
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static const char *TAG = "ui_home";

/* ── Layout constants (real device pixels, DPI-scaled where it matters) ── */
#define SW          720
#define SH          1280
#define SIDE_PAD    52
#define ORB_SIZE    500
#define ORB_CY      700   /* orb vertical center */

/* ── Root + text objects ─────────────────────────────────────── */
static lv_obj_t *s_screen       = NULL;  /* full-screen lv_obj child of active_screen */
static lv_obj_t *s_hero_part    = NULL;  /* "Afternoon" / "Good morning" etc */
static lv_obj_t *s_hero_clock   = NULL;  /* "14 : 32" mono */
static lv_obj_t *s_hero_date    = NULL;  /* "WEDNESDAY · APRIL 16" */
static lv_obj_t *s_sys_label    = NULL;  /* top-left: "DRAGON · 14:32" */
static lv_obj_t *s_sys_dot      = NULL;  /* top-left green pulse */
static lv_obj_t *s_mode_diamond = NULL;  /* top-right tinted diamond */
static lv_obj_t *s_mode_label   = NULL;  /* top-right "CLAW" / "LOCAL" etc */
static lv_obj_t *s_orb          = NULL;  /* the orb */
static lv_obj_t *s_poem_label   = NULL;  /* "Earlier — you asked…" */
static lv_obj_t *s_hint_label   = NULL;  /* bottom "FOCUS ⌄" */
static lv_obj_t *s_toast        = NULL;  /* toast overlay (lazy) */

/* Periodic refresh timer */
static lv_timer_t *s_refresh_timer = NULL;

/* Mode diamond colors (indexed by VOICE_MODE_*) */
static const uint32_t s_mode_tint[4] = {
    TH_MODE_LOCAL,   /* Local */
    TH_MODE_HYBRID,  /* Hybrid */
    TH_MODE_CLOUD,   /* Cloud */
    TH_MODE_CLAW,    /* TinkerClaw */
};
static const char *s_mode_short[4] = { "LOCAL", "HYBRID", "CLOUD", "CLAW" };

static uint8_t s_badge_mode = 0;

/* ── Forward decls ───────────────────────────────────────────── */
static void refresh_timer_cb(lv_timer_t *t);
static void orb_click_cb(lv_event_t *e);
static void orb_long_press_cb(lv_event_t *e);
static void screen_gesture_cb(lv_event_t *e);
static void poem_click_cb(lv_event_t *e);
static void sys_click_cb(lv_event_t *e);
static bool any_overlay_visible(void);
static void show_toast_internal(const char *text);
static void orb_paint_for_mode(uint8_t mode);

/* ───────────────────────────── helpers ───────────────────────── */

/* Return the greeting word appropriate for the current hour. */
static const char *greeting_for_hour(int hour)
{
    if (hour < 5)  return "Late night";
    if (hour < 12) return "Morning";
    if (hour < 17) return "Afternoon";
    if (hour < 21) return "Evening";
    return "Tonight";
}

/* Format "WEDNESDAY · APRIL 16" into buf. */
static void format_date(char *buf, size_t n, const struct tm *tm)
{
    static const char *wd[7] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    static const char *mo[12] = {"JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
                                  "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"};
    int wday = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
    int mon  = (tm->tm_mon  >= 0 && tm->tm_mon  < 12) ? tm->tm_mon : 0;
    /* U+2022 BULLET (•) — this glyph IS in the built-in Montserrat range
     * (ASCII + 0xB0 + 0x2022), unlike U+00B7 MIDDLE DOT which isn't. */
    snprintf(buf, n, "%s \xe2\x80\xa2 %s %d", wd[wday], mo[mon], tm->tm_mday);
}

/* ────────────────────────── orb painting ─────────────────────── */

static void orb_paint_for_mode(uint8_t mode)
{
    if (!s_orb) return;
    if (mode >= 4) mode = 0;

    /* FLAT: vertical 2-stop linear gradient per mode. Light top → dark bottom. */
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
    /* Shadow disabled for initial v5 ship — LVGL 9 shadow rendering at
       500 px on ESP32-P4 can blow the draw buffer. Re-enable with a small
       width once the MINIMAL density is wired via pre-baked images. */
}

static void update_mode_ui(uint8_t mode)
{
    if (mode >= 4) mode = 0;
    s_badge_mode = mode;
    if (s_mode_diamond) {
        lv_obj_set_style_bg_color(s_mode_diamond, lv_color_hex(s_mode_tint[mode]), 0);
    }
    if (s_mode_label) lv_label_set_text(s_mode_label, s_mode_short[mode]);
    orb_paint_for_mode(mode);
}

/* ────────────────────────── screen build ────────────────────── */

lv_obj_t *ui_home_create(void)
{
    /* Create a fresh top-level screen (lv_obj_create(NULL) = screen).
       Overlays (settings/chat/notes) attach as children of this screen. */
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, SW, SH);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_screen);

    /* Gesture handler at the screen level for swipe-up / edge-swipes */
    lv_obj_add_event_cb(s_screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* ── Edge ticks — v5 visible affordances for left/right swipes ── */
    /* Left edge (flush with x=0) — swipe-right from here opens Notes. */
    lv_obj_t *edge_l = lv_obj_create(s_screen);
    lv_obj_remove_style_all(edge_l);
    lv_obj_set_size(edge_l, 2, 40);
    lv_obj_set_pos(edge_l, 0, SH / 2 - 20);
    lv_obj_set_style_bg_color(edge_l, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(edge_l, 80, 0);   /* ~31 % — hairline hint */
    lv_obj_set_style_radius(edge_l, 2, 0);

    /* Right edge (flush with x=SW-2) — swipe-left opens Settings. */
    lv_obj_t *edge_r = lv_obj_create(s_screen);
    lv_obj_remove_style_all(edge_r);
    lv_obj_set_size(edge_r, 2, 40);
    lv_obj_set_pos(edge_r, SW - 2, SH / 2 - 20);
    lv_obj_set_style_bg_color(edge_r, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(edge_r, 80, 0);
    lv_obj_set_style_radius(edge_r, 2, 0);

    /* ── Top-left system label (green pulse + "DRAGON · 14:32") ── */
    s_sys_dot = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_sys_dot);
    lv_obj_set_size(s_sys_dot, 10, 10);
    lv_obj_set_pos(s_sys_dot, SIDE_PAD, 56);
    lv_obj_set_style_radius(s_sys_dot, 5, 0);
    lv_obj_set_style_bg_color(s_sys_dot, lv_color_hex(TH_STATUS_GREEN), 0);
    lv_obj_set_style_bg_opa(s_sys_dot, LV_OPA_COVER, 0);

    s_sys_label = lv_label_create(s_screen);
    lv_label_set_text(s_sys_label, "DRAGON \xe2\x80\xa2 --");
    lv_obj_set_pos(s_sys_label, SIDE_PAD + 20, 50);
    lv_obj_set_style_text_font(s_sys_label, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_sys_label, lv_color_hex(0x8A8A93), 0);
    lv_obj_set_style_text_letter_space(s_sys_label, 3, 0);
    /* Tap the sys label → Memory search overlay */
    lv_obj_add_flag(s_sys_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sys_label, sys_click_cb, LV_EVENT_CLICKED, NULL);

    /* ── Top-right mode diamond + label ── */
    s_mode_diamond = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_mode_diamond);
    lv_obj_set_size(s_mode_diamond, 14, 14);
    /* 45° rotation requires LV_USE_TRANSFORM; we fake a diamond with a small
       square + border — acceptable for v5. Rotated transform will come later. */
    lv_obj_set_style_radius(s_mode_diamond, 2, 0);
    lv_obj_set_style_bg_color(s_mode_diamond, lv_color_hex(TH_MODE_CLAW), 0);
    lv_obj_set_style_bg_opa(s_mode_diamond, LV_OPA_COVER, 0);

    s_mode_label = lv_label_create(s_screen);
    lv_label_set_text(s_mode_label, s_mode_short[0]);
    lv_obj_set_style_text_font(s_mode_label, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0x8A8A93), 0);
    lv_obj_set_style_text_letter_space(s_mode_label, 3, 0);
    /* Right-align: reserve 120px region, right edge = SW - SIDE_PAD */
    lv_obj_set_width(s_mode_label, 120);
    lv_obj_set_style_text_align(s_mode_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_mode_label, SW - SIDE_PAD - 120, 50);
    lv_obj_set_pos(s_mode_diamond, SW - SIDE_PAD - 120 - 22, 54);

    /* ── Hero: greeting word, clock numerics, date ── */
    s_hero_part = lv_label_create(s_screen);
    lv_label_set_text(s_hero_part, "Afternoon");
    lv_obj_set_style_text_font(s_hero_part, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_hero_part, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_letter_space(s_hero_part, -1, 0);
    lv_obj_set_width(s_hero_part, SW);
    lv_obj_set_style_text_align(s_hero_part, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hero_part, 0, 150);

    s_hero_clock = lv_label_create(s_screen);
    lv_label_set_text(s_hero_clock, "— : —");
    lv_obj_set_style_text_font(s_hero_clock, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(s_hero_clock, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_style_text_letter_space(s_hero_clock, 12, 0);
    lv_obj_set_width(s_hero_clock, SW);
    lv_obj_set_style_text_align(s_hero_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hero_clock, 0, 230);

    s_hero_date = lv_label_create(s_screen);
    lv_label_set_text(s_hero_date, "—");
    lv_obj_set_style_text_font(s_hero_date, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_hero_date, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(s_hero_date, 4, 0);
    lv_obj_set_width(s_hero_date, SW);
    lv_obj_set_style_text_align(s_hero_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hero_date, 0, 272);

    /* ── The orb (FLAT: solid circle + 2-stop vertical gradient) ── */
    s_orb = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_orb);
    lv_obj_set_size(s_orb, ORB_SIZE, ORB_SIZE);
    lv_obj_set_pos(s_orb, (SW - ORB_SIZE) / 2, ORB_CY - ORB_SIZE / 2);
    lv_obj_set_style_radius(s_orb, LV_RADIUS_CIRCLE, 0);
    orb_paint_for_mode(s_badge_mode);
    lv_obj_add_flag(s_orb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_orb, orb_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_orb, orb_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* ── Poem line (lower right) ── */
    s_poem_label = lv_label_create(s_screen);
    lv_label_set_long_mode(s_poem_label, LV_LABEL_LONG_WRAP);
    /* ASCII only — Montserrat font subset on device has no em-dash. */
    lv_label_set_text(s_poem_label, "Earlier -- heartbeat drafted a reply.\nTap to see what your agents did.");
    lv_obj_set_style_text_font(s_poem_label, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(s_poem_label, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_style_text_align(s_poem_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_line_space(s_poem_label, 4, 0);
    lv_obj_set_width(s_poem_label, SW - SIDE_PAD * 2 - 60);
    lv_obj_set_pos(s_poem_label, SIDE_PAD + 60, 1030);
    lv_obj_add_flag(s_poem_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_poem_label, poem_click_cb, LV_EVENT_CLICKED, NULL);

    /* ── Swipe-up hint ── */
    s_hint_label = lv_label_create(s_screen);
    /* ASCII only — no Unicode chevron in Montserrat subset */
    lv_label_set_text(s_hint_label, "FOCUS");
    lv_obj_set_style_text_font(s_hint_label, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x3a3a40), 0);
    lv_obj_set_style_text_letter_space(s_hint_label, 6, 0);
    lv_obj_set_width(s_hint_label, SW);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hint_label, 0, 1210);

    /* Initial status fill */
    ui_home_update_status();

    /* Refresh timer (5 s cadence — minute precision is enough for the clock,
       and each refresh only invalidates labels whose text actually changed). */
    if (s_refresh_timer == NULL) {
        s_refresh_timer = lv_timer_create(refresh_timer_cb, 5000, NULL);
    }

    ESP_LOGI(TAG, "v5 FLAT home created (orb %dpx, mode %d)", ORB_SIZE, s_badge_mode);
    return s_screen;
}

/* ───────────────────────── periodic status ─────────────────── */

void ui_home_update_status(void)
{
    /* Fetch wall clock time */
    time_t now = 0;
    time(&now);
    struct tm tm_local;
    localtime_r(&now, &tm_local);

    /* Hero greeting word (re-evaluated each tick so it shifts with hour) */
    if (s_hero_part) {
        const char *g = greeting_for_hour(tm_local.tm_hour);
        /* Only set text when it actually changes — LVGL skips the redraw then */
        const char *cur = lv_label_get_text(s_hero_part);
        if (!cur || strcmp(cur, g) != 0) lv_label_set_text(s_hero_part, g);
    }

    /* Clock + date — only invalidate when text actually changes. Keeps LVGL
       from repainting the 500 px orb underneath them every second. */
    if (s_hero_clock) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d : %02d", tm_local.tm_hour, tm_local.tm_min);
        const char *cur = lv_label_get_text(s_hero_clock);
        if (!cur || strcmp(cur, buf) != 0) lv_label_set_text(s_hero_clock, buf);
    }
    if (s_hero_date) {
        char buf[40];
        format_date(buf, sizeof(buf), &tm_local);
        const char *cur = lv_label_get_text(s_hero_date);
        if (!cur || strcmp(cur, buf) != 0) lv_label_set_text(s_hero_date, buf);
    }

    /* ── Edge-state detection (v5 spec section 10 "When something's off") ──
     * Priority order: NO WI-FI > DRAGON DOWN > MUTED > QUIET HOURS > normal.
     * Same composition, different voice: sys label + poem + status-dot
     * colour all reflect the dominant state. */
    bool wifi_ok  = tab5_wifi_connected();
    bool dragon   = voice_is_connected();
    bool mic_off  = tab5_settings_get_mic_mute() != 0;
    bool quiet    = tab5_settings_quiet_active(tm_local.tm_hour);

    enum { ST_NORMAL, ST_NO_WIFI, ST_DRAGON_DOWN, ST_MUTED, ST_QUIET } state;
    if (!wifi_ok)       state = ST_NO_WIFI;
    else if (!dragon)   state = ST_DRAGON_DOWN;
    else if (mic_off)   state = ST_MUTED;
    else if (quiet)     state = ST_QUIET;
    else                state = ST_NORMAL;

    if (s_poem_label) {
        char preview[180];
        bool have = ui_notes_get_last_preview(preview, sizeof(preview));
        char buf[260];
        switch (state) {
            case ST_NO_WIFI:
                snprintf(buf, sizeof(buf),
                    "I can't reach the network.\n%s didn't answer three times.\nTap to scan.",
                    "Wi-Fi");
                break;
            case ST_DRAGON_DOWN:
                snprintf(buf, sizeof(buf),
                    "My brain is offline.\nI'll record voice notes locally\nuntil Dragon comes back.");
                break;
            case ST_MUTED:
                snprintf(buf, sizeof(buf),
                    "Listening off.\nI won't hear a thing\nuntil you tap me.");
                break;
            case ST_QUIET:
                snprintf(buf, sizeof(buf),
                    "Good night, Emile.\n%02d:%02d -- I won't speak until 07:00.\nEmergencies can still reach me.",
                    tm_local.tm_hour, tm_local.tm_min);
                break;
            default:
                if (have && preview[0]) {
                    snprintf(buf, sizeof(buf),
                        "Last note -- %s\nTap to see what your agents did.",
                        preview);
                } else {
                    snprintf(buf, sizeof(buf),
                        "Standing by.\nTap me on the orb to ask something.");
                }
                break;
        }
        const char *cur = lv_label_get_text(s_poem_label);
        if (!cur || strcmp(cur, buf) != 0) lv_label_set_text(s_poem_label, buf);
    }

    /* Top-left system status — v5 spec.  Format + colour matches the
     * active edge state.  Normal: "DRAGON • 14:32".  Active voice:
     * "DRAGON • LISTENING".  Edge states: per spec shot-13. */
    if (s_sys_label) {
        char buf[64];
        voice_state_t vs = voice_get_state();
        const char *state_hint = NULL;
        if (dragon) {
            switch (vs) {
                case VOICE_STATE_LISTENING:  state_hint = "LISTENING"; break;
                case VOICE_STATE_PROCESSING: state_hint = "THINKING";  break;
                case VOICE_STATE_SPEAKING:   state_hint = "SPEAKING";  break;
                default: break;
            }
        }
        uint32_t label_col = 0x8A8A93;  /* default muted */
        switch (state) {
            case ST_NO_WIFI:
                snprintf(buf, sizeof(buf), "OFFLINE \xe2\x80\xa2 NO WI-FI");
                label_col = TH_STATUS_RED;
                break;
            case ST_DRAGON_DOWN:
                snprintf(buf, sizeof(buf), "DRAGON \xe2\x80\xa2 UNREACHABLE");
                label_col = TH_STATUS_RED;
                break;
            case ST_MUTED:
                snprintf(buf, sizeof(buf), "MIC \xe2\x80\xa2 OFF");
                label_col = 0x7A7A82;
                break;
            case ST_QUIET:
                snprintf(buf, sizeof(buf), "QUIET \xe2\x80\xa2 UNTIL 07:00");
                label_col = 0x7A7A82;
                break;
            default:
                if (state_hint) {
                    snprintf(buf, sizeof(buf), "DRAGON \xe2\x80\xa2 %s", state_hint);
                } else {
                    snprintf(buf, sizeof(buf), "DRAGON \xe2\x80\xa2 %02d:%02d",
                             tm_local.tm_hour, tm_local.tm_min);
                }
                break;
        }
        const char *cur = lv_label_get_text(s_sys_label);
        if (!cur || strcmp(cur, buf) != 0) lv_label_set_text(s_sys_label, buf);
        lv_obj_set_style_text_color(s_sys_label, lv_color_hex(label_col), 0);
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

    /* Top-right mode chip — re-read from NVS (cheap) */
    uint8_t current_mode = tab5_settings_get_voice_mode();
    if (current_mode != s_badge_mode) {
        update_mode_ui(current_mode);
    }

    /* Mode label + diamond are pre-positioned in ui_home_create() at
       fixed right-aligned coords — nothing to reposition here. */
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_home_update_status();
}

/* ───────────────────────── interactions ────────────────────── */

static void orb_click_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;  /* ignore stray tap-through from a closing overlay */
    ESP_LOGI(TAG, "orb tapped -> open voice");
    if (!voice_is_connected()) {
        /* Kick off async connect if we're offline */
        char dhost[64];
        tab5_settings_get_dragon_host(dhost, sizeof(dhost));
        if (dhost[0]) {
            voice_connect_async(dhost, TAB5_VOICE_PORT, false);
        }
    }
    ui_voice_show();
    voice_start_listening();
}

static void orb_long_press_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    uint8_t next = (s_badge_mode + 1) % VOICE_MODE_COUNT;
    ESP_LOGI(TAG, "orb long-pressed -> cycling mode %d -> %d", s_badge_mode, next);
    tab5_settings_set_voice_mode(next);
    char model[64];
    tab5_settings_get_llm_model(model, sizeof(model));
    voice_send_config_update(next, model);
    update_mode_ui(next);

    char buf[48];
    snprintf(buf, sizeof(buf), "Mode: %s", s_mode_short[next]);
    show_toast_internal(buf);
}

static void poem_click_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    ESP_LOGI(TAG, "poem tapped -> Agents");
    ui_agents_show();
}

static void sys_click_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    ESP_LOGI(TAG, "sys label tapped -> Memory");
    ui_memory_show();
}

/* Return true if an overlay is currently obscuring the home screen.
   Home gestures must no-op while an overlay is up so a single swipe doesn't
   open two screens (e.g. already-in-Chat + swipe-up stacks a second Chat). */
static bool any_overlay_visible(void)
{
    extern bool ui_notes_is_visible(void);
    extern bool ui_settings_is_visible(void);
    extern bool ui_sessions_is_visible(void);
    if (ui_chat_is_active())       return true;
    if (ui_agents_is_visible())    return true;
    if (ui_memory_is_visible())    return true;
    if (ui_focus_is_visible())     return true;
    if (ui_notes_is_visible())     return true;
    if (ui_settings_is_visible())  return true;
    if (ui_sessions_is_visible())  return true;
    return false;
}

/* Async callbacks — deferred so the originating gesture has fully dispatched
   before we create the overlay.  Prevents a swipe-right on home from opening
   Notes, then immediately bubbling to Notes's own swipe-right=back handler
   and tearing it down mid-layout (the visible crash the user reported). */
static void async_open_focus(void *arg)   { (void)arg; ui_focus_show(); }
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
        case LV_DIR_TOP:
            ESP_LOGI(TAG, "swipe up -> Focus (async)");
            lv_async_call(async_open_focus, NULL);
            break;
        case LV_DIR_RIGHT:
            ESP_LOGI(TAG, "swipe right -> Notes (async)");
            lv_async_call(async_open_notes, NULL);
            break;
        case LV_DIR_LEFT:
            ESP_LOGI(TAG, "swipe left -> Settings (async)");
            lv_async_call(async_open_settings, NULL);
            break;
        default:
            break;
    }
}

/* ───────────────────────── toast ─────────────────────────────── */

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
    /* Remove any existing toast first */
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

/* ───────────────────────── public API ────────────────────────── */

void ui_home_destroy(void)
{
    if (s_refresh_timer) { lv_timer_delete(s_refresh_timer); s_refresh_timer = NULL; }
    if (s_screen) { lv_obj_del(s_screen); s_screen = NULL; }
    s_hero_part = s_hero_clock = s_hero_date = NULL;
    s_sys_label = s_sys_dot = NULL;
    s_mode_diamond = s_mode_label = NULL;
    s_orb = s_poem_label = s_hint_label = NULL;
    s_toast = NULL;
}

lv_obj_t *ui_home_get_screen(void) { return s_screen; }

void ui_home_go_home(void)
{
    /* v5 home is single-page. "Go home" = hide any open overlays. */
    if (ui_chat_is_active()) ui_chat_hide();
    /* ui_settings / ui_notes have their own back button flows; nothing to do */
}

lv_obj_t *ui_home_get_tileview(void)
{
    /* v5 has no tileview. Returning NULL is fine — debug_server/navigate
       handles this gracefully (see ui_home_go_home for the equivalent). */
    return NULL;
}

lv_obj_t *ui_home_get_tile(int page)
{
    /* v5 single-page: page 0 == the home screen; everything else opens as
       an overlay via ui_home_nav_* helpers. */
    return (page == 0) ? s_screen : NULL;
}

void ui_home_nav_settings(void)
{
    ui_settings_create();
}

void ui_home_refresh_mode_badge(void)
{
    uint8_t m = tab5_settings_get_voice_mode();
    update_mode_ui(m);
}

void ui_home_show_toast(const char *text)
{
    show_toast_internal(text);
}

/* Pull just the voice-state line out of update_status and make it callable
   from anywhere. lv_async_call marshals back onto the LVGL thread so
   voice.c callbacks firing on the WS recv task don't touch LVGL from the
   wrong core. */
static void sys_label_refresh_async_cb(void *arg)
{
    (void)arg;
    if (!s_sys_label) return;
    char buf[64];
    bool dragon = voice_is_connected();
    int bat = (int)tab5_battery_percent();
    voice_state_t vs = voice_get_state();
    const char *state_hint = NULL;
    if (dragon) {
        switch (vs) {
            case VOICE_STATE_LISTENING:  state_hint = "LISTENING"; break;
            case VOICE_STATE_PROCESSING: state_hint = "THINKING";  break;
            case VOICE_STATE_SPEAKING:   state_hint = "SPEAKING";  break;
            default: break;
        }
    }
    bool muted = tab5_settings_get_mic_mute();
    bool quiet = false;
    {
        time_t now = 0; time(&now);
        struct tm tm_local; localtime_r(&now, &tm_local);
        quiet = tab5_settings_quiet_active(tm_local.tm_hour);
    }
    if (quiet)            snprintf(buf, sizeof(buf), "QUIET - %d%%", bat);
    else if (muted)       snprintf(buf, sizeof(buf), "MUTED - %d%%", bat);
    else if (state_hint)  snprintf(buf, sizeof(buf), "DRAGON %d%% - %s", bat, state_hint);
    else if (dragon)      snprintf(buf, sizeof(buf), "DRAGON %d%%", bat);
    else                  snprintf(buf, sizeof(buf), "OFFLINE %d%%", bat);
    const char *cur = lv_label_get_text(s_sys_label);
    if (!cur || strcmp(cur, buf) != 0) lv_label_set_text(s_sys_label, buf);
}

void ui_home_refresh_sys_label(void)
{
    /* Marshal to LVGL thread — voice state callbacks fire on the WS recv
       task (Core 1), but LVGL objects must only be touched from the LVGL
       task. lv_async_call is the standard ESP-IDF + LVGL pattern. */
    lv_async_call(sys_label_refresh_async_cb, NULL);
}
