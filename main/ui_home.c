/*
 * ui_home.c — TinkerOS Home Screen (Redesigned)
 *
 * Voice-first layout for a privacy-first AI companion device.
 * 720x1280 portrait, LVGL v9.
 *
 * 4-page vertical tileview:
 *   Page 0 — Home: clock + tappable orb + last note + quick actions
 *   Page 1 — Notes: voice notes list
 *   Page 2 — Voice: full voice overlay (minimal hint — tap orb to activate)
 *   Page 3 — Settings: full settings screen
 *
 * Status bar: time + privacy badge + wifi + battery
 * Bottom nav: Home | Notes | Voice | Settings
 */

#include "ui_home.h"
#include "ui_notes.h"
#include "ui_chat.h"
#include "ui_settings.h"
#include "ui_voice.h"
#include "ui_keyboard.h"
#include "mode_manager.h"
#include "voice.h"
#include "ui_wifi.h"
#include "ui_camera.h"
#include "ui_files.h"
#include "battery.h"
#include "rtc.h"
#include "dragon_link.h"
#include "wifi.h"
#include "display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static const char *TAG = "ui_home";

/* ── Palette — soft dark ─────────────────────────────────────── */
#define COL_BG          0x000000
#define COL_CARD        0x1C1C1E
#define COL_CARD2       0x2C2C2E
#define COL_AMBER       0xFFB800
#define COL_AMBER_SOFT  0xFFD060
#define COL_CYAN        0x00B4D8
#define COL_MINT        0x30D158
#define COL_BLUE        0x0A84FF
#define COL_PURPLE      0xBF5AF2
#define COL_ORANGE      0xFF9F0A
#define COL_RED         0xFF453A
#define COL_WHITE       0xFFFFFF
#define COL_LABEL       0xEBEBF5
#define COL_LABEL2      0x8E8E93
#define COL_LABEL3      0x48484A
#define COL_SEP         0x38383A

/* ── Layout — BIG TOUCH TARGETS ─────────────────────────────────── */
#define SW              720
#define SH              1280
#define SBAR_H          56
#define NAV_H           120
#define NUM_PAGES       4

/* ── Forward decls ───────────────────────────────────────────── */
static void update_timer_cb(lv_timer_t *t);
static void tileview_scroll_cb(lv_event_t *e);
static void nav_click_cb(lv_event_t *e);
static void brain_pulse_cb(void *obj, int32_t val);
static void show_toast(const char *text);
static void orb_tap_cb(lv_event_t *e);
static void voice_hint_tap_cb(lv_event_t *e);
static void privacy_tap_cb(lv_event_t *e);
static void ask_tap_cb(lv_event_t *e);
static void cb_last_note_tap(lv_event_t *e);

/* ── State ───────────────────────────────────────────────────── */
static lv_obj_t  *scr        = NULL;
static lv_obj_t  *tileview   = NULL;
static lv_obj_t  *tiles[NUM_PAGES] = {NULL};

/* Status bar */
static lv_obj_t  *lbl_sbar_time   = NULL;
static lv_obj_t  *lbl_sbar_wifi  = NULL;
static lv_obj_t  *lbl_sbar_batt  = NULL;
static lv_obj_t  *lbl_privacy    = NULL;   /* "Local" badge */
static lv_obj_t  *lbl_sbar_dragon = NULL;  /* Dragon connection dot */

/* Home page (Page 0) */
static lv_obj_t  *lbl_clock     = NULL;
static lv_obj_t  *lbl_date      = NULL;
static lv_obj_t  *lbl_greeting  = NULL;
static lv_obj_t  *orb_ring      = NULL;
static lv_obj_t  *lbl_last_note = NULL;    /* last note preview card */
static lv_obj_t  *last_note_card = NULL;

/* Nav bar */
static lv_obj_t  *nav_icons[NUM_PAGES] = {NULL};
static int        cur_page = 0;

/* Page dots */
static lv_obj_t  *page_dots[NUM_PAGES] = {NULL};

static lv_anim_t  orb_anim;
static bool       orb_anim_on = false;
static lv_timer_t *tmr_update = NULL;

/* ── Helpers ─────────────────────────────────────────────────── */
static void update_nav_ui(int page);

/* ── Page 0: Home ──────────────────────────────────────────── */
static lv_obj_t *build_page_home(void)
{
    lv_obj_t *pg = tiles[0];

    /* ── Clock (top center) ─────────────────────────── */
    lbl_clock = lv_label_create(pg);
    lv_label_set_text(lbl_clock, "00:00");
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_letter_space(lbl_clock, 4, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_MID, 0, 60);

    /* ── Date ────────────────────────────────────────── */
    lbl_date = lv_label_create(pg);
    lv_label_set_text(lbl_date, "Wednesday, March 26");
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_36, 0);
    lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, 0, 148);

    /* ── Voice Orb (center) — TINKER ─────────────────── */
    /* Outer ring (breathing animation) */
    orb_ring = lv_obj_create(pg);
    lv_obj_set_size(orb_ring, 200, 200);
    lv_obj_align(orb_ring, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_style_bg_opa(orb_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(orb_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(orb_ring, 3, 0);
    lv_obj_set_style_border_color(orb_ring, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_border_opa(orb_ring, LV_OPA_40, 0);
    lv_obj_clear_flag(orb_ring, LV_OBJ_FLAG_SCROLLABLE);

    /* Inner orb — CLICKABLE (tappable to start voice) */
    lv_obj_t *orb = lv_obj_create(pg);
    lv_obj_set_size(orb, 160, 160);
    lv_obj_align(orb, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_style_bg_color(orb, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(orb, 3, 0);
    lv_obj_set_style_border_color(orb, lv_color_hex(COL_AMBER_SOFT), 0);
    lv_obj_clear_flag(orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(orb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(orb, 20);
    lv_obj_add_event_cb(orb, orb_tap_cb, LV_EVENT_CLICKED, NULL);

    /* Orb eye icon */
    lv_obj_t *orb_icon = lv_label_create(orb);
    lv_label_set_text(orb_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(orb_icon, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(orb_icon, &lv_font_montserrat_48, 0);
    lv_obj_center(orb_icon);

    /* Orb label below */
    lv_obj_t *orb_lbl = lv_label_create(pg);
    lv_label_set_text(orb_lbl, "Tap to ask");
    lv_obj_set_style_text_color(orb_lbl, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(orb_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(orb_lbl, LV_ALIGN_CENTER, 0, 40);

    /* Breathing animation on ring */
    lv_anim_init(&orb_anim);
    lv_anim_set_var(&orb_anim, orb_ring);
    lv_anim_set_values(&orb_anim, 10, 90);
    lv_anim_set_duration(&orb_anim, 3000);
    lv_anim_set_playback_duration(&orb_anim, 3000);
    lv_anim_set_repeat_count(&orb_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&orb_anim, brain_pulse_cb);
    lv_anim_start(&orb_anim);
    orb_anim_on = true;

    /* ── Last Note Card ────────────────────────────────────── */
    last_note_card = lv_obj_create(pg);
    lv_obj_set_size(last_note_card, SW - 48, 160);
    lv_obj_align(last_note_card, LV_ALIGN_CENTER, 0, 240);
    lv_obj_set_style_bg_color(last_note_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(last_note_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(last_note_card, 24, 0);
    lv_obj_set_style_border_width(last_note_card, 0, 0);
    lv_obj_set_style_pad_hor(last_note_card, 32, 0);
    lv_obj_clear_flag(last_note_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(last_note_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(last_note_card, 20);
    lv_obj_add_event_cb(last_note_card, cb_last_note_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t *note_icon = lv_label_create(last_note_card);
    lv_label_set_text(note_icon, LV_SYMBOL_LIST "  Last note");
    lv_obj_set_style_text_color(note_icon, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(note_icon, &lv_font_montserrat_28, 0);
    lv_obj_align(note_icon, LV_ALIGN_TOP_LEFT, 0, 12);

    lbl_last_note = lv_label_create(last_note_card);
    lv_label_set_text(lbl_last_note, "Tap to see notes");
    lv_obj_set_style_text_color(lbl_last_note, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(lbl_last_note, &lv_font_montserrat_28, 0);
    lv_obj_set_width(lbl_last_note, SW - 88);
    lv_label_set_long_mode(lbl_last_note, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_last_note, LV_ALIGN_BOTTOM_LEFT, 0, -12);

    /* ── Ask Tinker Button ──────────────────────── */
    lv_obj_t *ask_btn = lv_button_create(pg);
    lv_obj_set_size(ask_btn, SW - 48, 96);
    lv_obj_align(ask_btn, LV_ALIGN_BOTTOM_MID, 0, -(NAV_H + 52));
    lv_obj_set_style_bg_color(ask_btn, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_opa(ask_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ask_btn, 24, 0);
    lv_obj_set_style_border_width(ask_btn, 0, 0);
    lv_obj_add_event_cb(ask_btn, ask_tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ask_lbl = lv_label_create(ask_btn);
    lv_label_set_text(ask_lbl, LV_SYMBOL_AUDIO "  Ask Tinker");
    lv_obj_set_style_text_color(ask_lbl, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(ask_lbl, &lv_font_montserrat_36, 0);
    lv_obj_center(ask_lbl);

    return pg;
}

/* ── Page 1: Notes ────────────────────────────────────────── */
static lv_obj_t *build_page_notes(void)
{
    lv_obj_t *pg = tiles[1];
    /* Notes page is a placeholder that navigates to the full notes screen */
    lv_obj_t *hint = lv_label_create(pg);
    lv_label_set_text(hint, "Loading notes...");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_center(hint);
    return pg;
}

/* ── Page 2: Voice — tap anywhere to start ─────────────────── */
static void voice_page_tap_cb(lv_event_t *e)
{
    (void)e;
    orb_tap_cb(e);  /* same as tapping the orb */
}

static lv_obj_t *build_page_voice(void)
{
    lv_obj_t *pg = tiles[2];

    /* Big tappable area */
    lv_obj_add_flag(pg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pg, voice_page_tap_cb, LV_EVENT_CLICKED, NULL);

    /* Large mic icon */
    lv_obj_t *icon = lv_label_create(pg);
    lv_label_set_text(icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *lbl = lv_label_create(pg);
    lv_label_set_text(lbl, "Tap to talk to Tinker");
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 20);

    return pg;
}

/* ── Page 3: Settings ─────────────────────────────────────── */
static lv_obj_t *build_page_settings(void)
{
    lv_obj_t *pg = tiles[3];
    /* Settings page is a placeholder that navigates to the full settings screen */
    lv_obj_t *hint = lv_label_create(pg);
    lv_label_set_text(hint, "Loading settings...");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_center(hint);
    return pg;
}

/* ================================================================
 *  Public API
 * ================================================================ */

lv_obj_t *ui_home_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SW, SH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── TILEVIEW ────────────────────────────────────────────── */
    tileview = lv_tileview_create(scr);
    lv_obj_set_size(tileview, SW, SH);
    lv_obj_set_pos(tileview, 0, 0);
    lv_obj_set_style_bg_color(tileview, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);

    tiles[0] = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_BOTTOM);
    tiles[1] = lv_tileview_add_tile(tileview, 0, 1, LV_DIR_TOP | LV_DIR_BOTTOM);
    tiles[2] = lv_tileview_add_tile(tileview, 0, 2, LV_DIR_TOP | LV_DIR_BOTTOM);
    tiles[3] = lv_tileview_add_tile(tileview, 0, 3, LV_DIR_TOP);

    /* ── Build each page ─────────────────────────────────── */
    build_page_home();
    build_page_notes();
    build_page_voice();
    build_page_settings();

    /* ── STATUS BAR ─────────────────────────────────────────── */
    {
        lv_obj_t *sbar = lv_obj_create(scr);
        lv_obj_set_size(sbar, SW, SBAR_H);
        lv_obj_set_pos(sbar, 0, 0);
        lv_obj_set_style_bg_color(sbar, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(sbar, LV_OPA_90, 0);
        lv_obj_set_style_radius(sbar, 0, 0);
        lv_obj_set_style_border_width(sbar, 0, 0);
        lv_obj_clear_flag(sbar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* Time (left) */
        lbl_sbar_time = lv_label_create(sbar);
        lv_label_set_text(lbl_sbar_time, "00:00");
        lv_obj_set_style_text_color(lbl_sbar_time, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_sbar_time, &lv_font_montserrat_24, 0);
        lv_obj_align(lbl_sbar_time, LV_ALIGN_LEFT_MID, 16, 0);

        /* Privacy badge (center-left) */
        lbl_privacy = lv_label_create(sbar);
        lv_label_set_text(lbl_privacy, "  Local  ");
        lv_obj_set_style_bg_color(lbl_privacy, lv_color_hex(COL_MINT), 0);
        lv_obj_set_style_bg_opa(lbl_privacy, LV_OPA_30, 0);
        lv_obj_set_style_text_color(lbl_privacy, lv_color_hex(COL_MINT), 0);
        lv_obj_set_style_text_font(lbl_privacy, &lv_font_montserrat_20, 0);
        lv_obj_set_style_radius(lbl_privacy, 8, 0);
        lv_obj_align(lbl_privacy, LV_ALIGN_LEFT_MID, 80, 0);
        lv_obj_add_flag(lbl_privacy, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(lbl_privacy, 10);
        lv_obj_add_event_cb(lbl_privacy, privacy_tap_cb, LV_EVENT_CLICKED, NULL);

        /* Dragon status dot (green=connected, red=offline) */
        lbl_sbar_dragon = lv_obj_create(sbar);
        lv_obj_set_size(lbl_sbar_dragon, 12, 12);
        lv_obj_set_style_radius(lbl_sbar_dragon, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(lbl_sbar_dragon, lv_color_hex(COL_RED), 0);
        lv_obj_set_style_bg_opa(lbl_sbar_dragon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(lbl_sbar_dragon, 0, 0);
        lv_obj_align(lbl_sbar_dragon, LV_ALIGN_RIGHT_MID, -120, 0);
        lv_obj_clear_flag(lbl_sbar_dragon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* WiFi (right of center) */
        lbl_sbar_wifi = lv_label_create(sbar);
        lv_label_set_text(lbl_sbar_wifi, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(lbl_sbar_wifi, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_sbar_wifi, &lv_font_montserrat_24, 0);
        lv_obj_align(lbl_sbar_wifi, LV_ALIGN_RIGHT_MID, -90, 0);

        /* Battery (far right) */
        lbl_sbar_batt = lv_label_create(sbar);
        lv_label_set_text(lbl_sbar_batt, "100%");
        lv_obj_set_style_text_color(lbl_sbar_batt, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_sbar_batt, &lv_font_montserrat_24, 0);
        lv_obj_align(lbl_sbar_batt, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    /* ── BOTTOM NAV ─────────────────────────────────────────── */
    {
        lv_obj_t *nav = lv_obj_create(scr);
        lv_obj_set_size(nav, SW, NAV_H);
        lv_obj_set_pos(nav, 0, SH - NAV_H);
        lv_obj_set_style_bg_color(nav, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(nav, LV_OPA_90, 0);
        lv_obj_set_style_radius(nav, 0, 0);
        lv_obj_set_style_border_width(nav, 0, 0);
        lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

        /* Nav labels — Home | Notes | Voice | Settings */
        const char *labels[NUM_PAGES] = {
            "Home", "Notes", "Voice", "Settings"
        };
        int slot = SW / NUM_PAGES;
        for (int i = 0; i < NUM_PAGES; i++) {
            nav_icons[i] = lv_label_create(nav);
            lv_label_set_text(nav_icons[i], labels[i]);
            lv_obj_set_style_text_font(nav_icons[i], &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(nav_icons[i],
                lv_color_hex(i == 0 ? COL_AMBER : COL_LABEL2), 0);
            lv_obj_set_style_text_align(nav_icons[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(nav_icons[i], slot);
            lv_obj_align(nav_icons[i], LV_ALIGN_LEFT_MID, slot * i, 0);
            lv_obj_add_flag(nav_icons[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(nav_icons[i], 20);
            lv_obj_add_event_cb(nav_icons[i], nav_click_cb, LV_EVENT_CLICKED,
                               (void *)(intptr_t)i);
        }
    }

    /* ── PAGE DOTS — BIG ───────────────────────────────────── */
    {
        int dot_sz = 16;
        int dot_gap = 12;
        int total = NUM_PAGES * dot_sz + (NUM_PAGES - 1) * dot_gap;
        int x0 = (SW - total) / 2;
        int y = SH - NAV_H - 20;
        for (int i = 0; i < NUM_PAGES; i++) {
            page_dots[i] = lv_obj_create(scr);
            lv_obj_set_size(page_dots[i], dot_sz, dot_sz);
            lv_obj_set_pos(page_dots[i], x0 + i * (dot_sz + dot_gap), y);
            lv_obj_set_style_radius(page_dots[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(page_dots[i], 0, 0);
            lv_obj_set_style_bg_color(page_dots[i],
                lv_color_hex(i == 0 ? COL_AMBER : COL_LABEL3), 0);
            lv_obj_set_style_bg_opa(page_dots[i], LV_OPA_COVER, 0);
            lv_obj_clear_flag(page_dots[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    lv_obj_add_event_cb(tileview, tileview_scroll_cb, LV_EVENT_VALUE_CHANGED, NULL);

    ui_home_update_status();
    tmr_update = lv_timer_create(update_timer_cb, 1000, NULL);

    /* Hide persistent floating buttons on home page (orb + Ask Tinker are
     * the primary voice entry points here — mic/kbd triggers are redundant) */
    lv_obj_t *mic = ui_voice_get_mic_btn();
    if (mic) lv_obj_add_flag(mic, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *kbd = ui_keyboard_get_trigger_btn();
    if (kbd) lv_obj_add_flag(kbd, LV_OBJ_FLAG_HIDDEN);

    lv_screen_load(scr);
    ESP_LOGI(TAG, "TinkerOS home screen created");
    return scr;
}

/* ================================================================
 *  Callbacks
 * ================================================================ */

/* FreeRTOS task to connect to Dragon voice (mode switch) */
static void voice_connect_task(void *arg)
{
    tab5_mode_switch(MODE_VOICE);
    vTaskSuspend(NULL);
}

static void orb_tap_cb(lv_event_t *e)
{
    (void)e;
    voice_state_t st = voice_get_state();

    if (st == VOICE_STATE_READY) {
        esp_err_t err = voice_start_listening();
        if (err != ESP_OK) {
            show_toast("Voice not ready — try again");
            ESP_LOGW(TAG, "voice_start_listening failed: %s", esp_err_to_name(err));
        }
    } else if (st == VOICE_STATE_IDLE) {
        /* Not connected — connect to Dragon first */
        show_toast("Connecting to Tinker...");
        xTaskCreatePinnedToCore(
            voice_connect_task, "voice_conn", 8192, NULL, 5, NULL, 1);
    } else if (st == VOICE_STATE_CONNECTING) {
        show_toast("Connecting...");
    } else {
        show_toast("Voice is busy");
        ESP_LOGI(TAG, "Voice busy (state %d)", st);
    }
}

static void ask_tap_cb(lv_event_t *e)
{
    (void)e;
    orb_tap_cb(e);  /* Same action as orb tap */
}

static void cb_last_note_tap(lv_event_t *e)
{
    (void)e;
    /* Navigate to notes page */
    if (tileview && tiles[1]) {
        lv_tileview_set_tile(tileview, tiles[1], LV_ANIM_ON);
    }
}

static void privacy_tap_cb(lv_event_t *e)
{
    (void)e;
    show_toast("Local mode: all data stays on your network");
}

static void voice_hint_tap_cb(lv_event_t *e)
{
    (void)e;
    /* Navigate to home page and start voice */
    if (tileview && tiles[0]) {
        lv_tileview_set_tile(tileview, tiles[0], LV_ANIM_ON);
    }
    orb_tap_cb(e);
}

static void brain_pulse_cb(void *obj, int32_t val)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

/* ── Toast helper ─────────────────────────────────────────── */
static void toast_timer_cb(lv_timer_t *t)
{
    lv_obj_t *toast = (lv_obj_t *)lv_timer_get_user_data(t);
    if (toast) lv_obj_delete(toast);
}

static void show_toast(const char *text)
{
    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(toast, 16, 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_style_pad_hor(toast, 32, 0);
    lv_obj_set_style_pad_ver(toast, 16, 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(toast);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl);

    lv_timer_t *tmr = lv_timer_create(toast_timer_cb, 2000, toast);
    lv_timer_set_repeat_count(tmr, 1);
}

static void update_nav_ui(int page)
{
    for (int i = 0; i < NUM_PAGES; i++) {
        if (page_dots[i]) {
            lv_obj_set_style_bg_color(page_dots[i],
                lv_color_hex(i == page ? COL_AMBER : COL_LABEL3), 0);
        }
        if (nav_icons[i]) {
            lv_obj_set_style_text_color(nav_icons[i],
                lv_color_hex(i == page ? COL_AMBER : COL_LABEL2), 0);
        }
    }

    /* Gate JPEG rendering — only on Dragon/voice page (page 2) */
    tab5_display_set_jpeg_enabled(page == 2);

    /* Show/hide persistent floating buttons: hidden on Page 0 (home has
     * its own orb + Ask Tinker), visible on other pages */
    lv_obj_t *mic = ui_voice_get_mic_btn();
    lv_obj_t *kbd = ui_keyboard_get_trigger_btn();
    if (page == 0) {
        if (mic) lv_obj_add_flag(mic, LV_OBJ_FLAG_HIDDEN);
        if (kbd) lv_obj_add_flag(kbd, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (mic) lv_obj_clear_flag(mic, LV_OBJ_FLAG_HIDDEN);
        if (kbd) lv_obj_clear_flag(kbd, LV_OBJ_FLAG_HIDDEN);
    }

    /* When switching to Notes page, load the notes screen */
    if (page == 1 && tiles[1]) {
        lv_async_call((lv_async_cb_t)ui_notes_create, NULL);
    }
    /* When switching to Settings page, load the settings screen */
    if (page == 3 && tiles[3]) {
        lv_async_call((lv_async_cb_t)ui_settings_create, NULL);
    }
}

static void nav_click_cb(lv_event_t *e)
{
    int pg = (int)(intptr_t)lv_event_get_user_data(e);
    if (pg >= 0 && pg < NUM_PAGES && tiles[pg] && tileview) {
        lv_tileview_set_tile(tileview, tiles[pg], LV_ANIM_ON);
    }
}

static void tileview_scroll_cb(lv_event_t *e)
{
    lv_obj_t *active = lv_tileview_get_tile_active(lv_event_get_target(e));
    if (!active) return;
    int pg = lv_obj_get_y(active) / SH;
    if (pg < 0) pg = 0;
    if (pg >= NUM_PAGES) pg = NUM_PAGES - 1;
    if (pg != cur_page) {
        cur_page = pg;
        update_nav_ui(cur_page);
    }
}

static void update_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_home_update_status();
}

/* ================================================================
 *  Status update
 * ================================================================ */
void ui_home_update_status(void)
{
    if (!scr) return;

    /* Time + Date */
    tab5_rtc_time_t rtc = {0};
    if (tab5_rtc_get_time(&rtc) == ESP_OK) {
        char tb[8];
        snprintf(tb, sizeof(tb), "%02d:%02d", rtc.hour, rtc.minute);
        if (lbl_clock) lv_label_set_text(lbl_clock, tb);
        if (lbl_sbar_time) lv_label_set_text(lbl_sbar_time, tb);

        /* Dynamic date from RTC */
        if (lbl_date) {
            static const char *day_names[] = {
                "Sunday","Monday","Tuesday","Wednesday",
                "Thursday","Friday","Saturday"
            };
            static const char *month_names[] = {
                "January","February","March","April","May","June",
                "July","August","September","October","November","December"
            };
            if (rtc.weekday < 7 && rtc.month >= 1 && rtc.month <= 12) {
                lv_label_set_text_fmt(lbl_date, "%s, %s %d",
                    day_names[rtc.weekday], month_names[rtc.month - 1], rtc.day);
            }
        }
    }

    /* Battery */
    uint8_t bpct = tab5_battery_percent();
    char bb[8];
    snprintf(bb, sizeof(bb), "%u%%", bpct);
    if (lbl_sbar_batt) lv_label_set_text(lbl_sbar_batt, bb);

    /* WiFi */
    bool wifi_on = tab5_wifi_connected();
    if (lbl_sbar_wifi) {
        lv_obj_set_style_text_color(lbl_sbar_wifi,
            lv_color_hex(wifi_on ? COL_WHITE : COL_LABEL3), 0);
    }

    /* Dragon connection status dot */
    if (lbl_sbar_dragon) {
        dragon_state_t ds = tab5_dragon_get_state();
        bool dragon_ok = (ds == DRAGON_STATE_CONNECTED || ds == DRAGON_STATE_STREAMING);
        lv_obj_set_style_bg_color(lbl_sbar_dragon,
            lv_color_hex(dragon_ok ? COL_MINT : COL_RED), 0);
    }

    /* Low battery warnings */
    {
        static bool s_warned_10 = false;
        static bool s_warned_5 = false;
        if (bpct <= 10 && bpct > 5 && !s_warned_10) {
            s_warned_10 = true;
            show_toast("Battery low (10%)");
        }
        if (bpct <= 5 && bpct > 0 && !s_warned_5) {
            s_warned_5 = true;
            show_toast("Battery critical — plug in soon!");
            ESP_LOGW(TAG, "Battery critical (%u%%)", bpct);
        }
    }

    /* Last note card */
    if (lbl_last_note) {
        char note_preview[96];
        if (ui_notes_get_last_preview(note_preview, sizeof(note_preview))) {
            lv_label_set_text(lbl_last_note, note_preview);
        } else {
            lv_label_set_text(lbl_last_note, "No notes yet — tap Voice to record");
        }
    }
}

/* ================================================================
 *  Destroy
 * ================================================================ */
void ui_home_destroy(void)
{
    if (orb_anim_on) {
        lv_anim_delete(orb_ring, brain_pulse_cb);
        orb_anim_on = false;
    }
    if (tmr_update) {
        lv_timer_delete(tmr_update);
        tmr_update = NULL;
    }
    if (scr) {
        lv_obj_delete(scr);
        scr = NULL;
        tileview = NULL;
        orb_ring = NULL;
        lbl_clock = lbl_date = lbl_greeting = NULL;
        lbl_sbar_time = lbl_sbar_wifi = lbl_sbar_batt = NULL;
        lbl_sbar_dragon = NULL;
        lbl_privacy = NULL;
        lbl_last_note = NULL;
        last_note_card = NULL;
        memset(nav_icons, 0, sizeof(nav_icons));
        memset(page_dots, 0, sizeof(page_dots));
        memset(tiles, 0, sizeof(tiles));
        cur_page = 0;
        ESP_LOGI(TAG, "Home destroyed");
    }
}

lv_obj_t *ui_home_get_screen(void)
{
    return scr;
}

void ui_home_go_home(void)
{
    if (tileview && tiles[0]) {
        lv_tileview_set_tile(tileview, tiles[0], LV_ANIM_OFF);
        cur_page = 0;
        update_nav_ui(0);
    }
}
