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
#include "settings.h"
#include "config.h"
#include "ui_wifi.h"
#include "ui_camera.h"
#include "ui_files.h"
#include "battery.h"
#include "rtc.h"
#include "dragon_link.h"
#include "ui_core.h"
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
#define COL_CARD        0x1A1A2E
#define COL_CARD2       0x2C2C2E
#define COL_AMBER       0xF5A623
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
/* voice_hint_tap_cb removed — was dead code, never wired to any UI element */
static void privacy_tap_cb(lv_event_t *e);
static void ask_tap_cb(lv_event_t *e);
static void cb_last_note_tap(lv_event_t *e);
static void cb_camera_launch(lv_event_t *e);
static void cb_files_launch(lv_event_t *e);
static void dismiss_all_overlays(void);
static void async_notes_create(void *arg);

/* ── State ───────────────────────────────────────────────────── */
static uint8_t    s_badge_mode = 0;   /* local cycling state for mode badge */
static lv_obj_t  *scr        = NULL;
static lv_obj_t  *tileview   = NULL;
static lv_obj_t  *tiles[NUM_PAGES] = {NULL};

/* Status bar */
static lv_obj_t  *lbl_sbar_time   = NULL;
static lv_obj_t  *lbl_sbar_wifi  = NULL;
static lv_obj_t  *lbl_sbar_batt  = NULL;
static lv_obj_t  *lbl_privacy    = NULL;   /* "Local" badge */
static lv_obj_t  *lbl_sbar_dragon = NULL;  /* Dragon connection dot */

/* Disconnect banner */
static lv_obj_t  *s_disconnect_banner = NULL;

/* Home page (Page 0) */
static lv_obj_t  *lbl_clock     = NULL;
static lv_obj_t  *lbl_date      = NULL;
/* lbl_greeting removed — was declared but never created or assigned */
static lv_obj_t  *orb_ring      = NULL;
static lv_obj_t  *orb_inner     = NULL;    /* inner filled orb circle */
static lv_obj_t  *lbl_ask       = NULL;    /* "Tap to ask" / "Connecting..." below orb */
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
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xA0A0A8), 0);
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_24, 0);
    lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, 0, 140);

    /* ── Voice Orb (center) — TINKER ─────────────────── */
    /* Outer ring (breathing animation) */
    orb_ring = lv_obj_create(pg);
    lv_obj_set_size(orb_ring, 200, 200);
    lv_obj_align(orb_ring, LV_ALIGN_CENTER, 0, -212);
    lv_obj_set_style_bg_opa(orb_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(orb_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(orb_ring, 3, 0);
    lv_obj_set_style_border_color(orb_ring, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_border_opa(orb_ring, LV_OPA_40, 0);
    lv_obj_clear_flag(orb_ring, LV_OBJ_FLAG_SCROLLABLE);

    /* Inner orb — CLICKABLE (tappable to start voice) */
    orb_inner = lv_obj_create(pg);
    lv_obj_set_size(orb_inner, 160, 160);
    lv_obj_align(orb_inner, LV_ALIGN_CENTER, 0, -212);
    lv_obj_set_style_bg_color(orb_inner, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_opa(orb_inner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(orb_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(orb_inner, 3, 0);
    lv_obj_set_style_border_color(orb_inner, lv_color_hex(COL_AMBER_SOFT), 0);
    lv_obj_clear_flag(orb_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(orb_inner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(orb_inner, 20);
    lv_obj_add_event_cb(orb_inner, orb_tap_cb, LV_EVENT_CLICKED, NULL);

    /* L1: Orb icon — audio waveform (closest to mic in LVGL built-in set) */
    lv_obj_t *orb_icon = lv_label_create(orb_inner);
    lv_label_set_text(orb_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(orb_icon, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(orb_icon, &lv_font_montserrat_48, 0);
    lv_obj_center(orb_icon);

    /* Orb label — "Tap to ask" / "Connecting..." with long-press hint */
    lbl_ask = lv_label_create(pg);
    lv_label_set_text(lbl_ask, "Tap to ask");
    lv_obj_set_style_text_color(lbl_ask, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_ask, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_ask, LV_ALIGN_CENTER, 0, -92);

    /* H1: Long-press hint — brighter, more descriptive */
    lv_obj_t *hold_hint = lv_label_create(pg);
    lv_label_set_text(hold_hint, LV_SYMBOL_EDIT "  Long-press to dictate");
    lv_obj_set_style_text_color(hold_hint, lv_color_hex(0x777777), 0);
    lv_obj_set_style_text_font(hold_hint, &lv_font_montserrat_20, 0);
    lv_obj_align(hold_hint, LV_ALIGN_CENTER, 0, -58);

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

    /* ── Bottom layout (from bottom up):
     *   Nav bar:        120px  (separate from tileview)
     *   Page dots:       36px  (separate from tileview)
     *   Breathing room: ~310px
     *   Camera+Files:    64px  (proper touch targets)
     *   16px gap
     *   Ask Tinker:      64px
     *   16px gap
     *   Note card:       90px
     * ─────────────────────────────────────────── */

    /* Camera + Files — FULL WIDTH buttons, 64px height */
    #define BTN_GAP 16
    int qbtn_w = (SW - 48 - BTN_GAP) / 2;

    lv_obj_t *cam_btn = lv_button_create(pg);
    lv_obj_set_size(cam_btn, qbtn_w, 64);
    lv_obj_align(cam_btn, LV_ALIGN_BOTTOM_LEFT, 24, -310);
    lv_obj_set_style_bg_color(cam_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(cam_btn, 12, 0);
    lv_obj_set_style_border_width(cam_btn, 1, 0);
    lv_obj_set_style_border_color(cam_btn, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(cam_btn, cb_camera_launch, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cam_lbl = lv_label_create(cam_btn);
    lv_label_set_text(cam_lbl, LV_SYMBOL_IMAGE "  Camera");
    lv_obj_set_style_text_color(cam_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(cam_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(cam_lbl);

    lv_obj_t *files_btn = lv_button_create(pg);
    lv_obj_set_size(files_btn, qbtn_w, 64);
    lv_obj_align(files_btn, LV_ALIGN_BOTTOM_RIGHT, -24, -310);
    lv_obj_set_style_bg_color(files_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(files_btn, 12, 0);
    lv_obj_set_style_border_width(files_btn, 1, 0);
    lv_obj_set_style_border_color(files_btn, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(files_btn, cb_files_launch, LV_EVENT_CLICKED, NULL);
    lv_obj_t *files_lbl = lv_label_create(files_btn);
    lv_label_set_text(files_lbl, LV_SYMBOL_DIRECTORY "  Files");
    lv_obj_set_style_text_color(files_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(files_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(files_lbl);

    /* Ask Tinker — above Camera/Files */
    lv_obj_t *ask_btn = lv_button_create(pg);
    lv_obj_set_size(ask_btn, SW - 48, 64);
    lv_obj_align(ask_btn, LV_ALIGN_BOTTOM_MID, 0, -(310 + 64 + 16));
    lv_obj_set_style_bg_color(ask_btn, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_opa(ask_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ask_btn, 12, 0);
    lv_obj_set_style_border_width(ask_btn, 0, 0);
    lv_obj_add_event_cb(ask_btn, ask_tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ask_lbl = lv_label_create(ask_btn);
    lv_label_set_text(ask_lbl, LV_SYMBOL_AUDIO "  Ask Tinker");
    lv_obj_set_style_text_color(ask_lbl, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(ask_lbl, &lv_font_montserrat_36, 0);
    lv_obj_center(ask_lbl);

    /* Last Note Card — above Ask Tinker, compact */
    last_note_card = lv_obj_create(pg);
    lv_obj_set_size(last_note_card, SW - 48, 90);
    lv_obj_align(last_note_card, LV_ALIGN_BOTTOM_MID, 0, -(310 + 64 + 16 + 64 + 16));
    lv_obj_set_style_bg_color(last_note_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(last_note_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(last_note_card, 12, 0);
    lv_obj_set_style_border_width(last_note_card, 1, 0);
    lv_obj_set_style_border_color(last_note_card, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_hor(last_note_card, 24, 0);
    lv_obj_clear_flag(last_note_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(last_note_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(last_note_card, 10);
    lv_obj_add_event_cb(last_note_card, cb_last_note_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t *note_icon = lv_label_create(last_note_card);
    lv_label_set_text(note_icon, LV_SYMBOL_LIST "  Last note");
    lv_obj_set_style_text_color(note_icon, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(note_icon, &lv_font_montserrat_18, 0);
    lv_obj_align(note_icon, LV_ALIGN_TOP_LEFT, 0, 6);

    lbl_last_note = lv_label_create(last_note_card);
    lv_label_set_text(lbl_last_note, "Tap to see notes");
    lv_obj_set_style_text_color(lbl_last_note, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(lbl_last_note, &lv_font_montserrat_24, 0);
    lv_obj_set_width(lbl_last_note, SW - 96);
    lv_label_set_long_mode(lbl_last_note, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_last_note, LV_ALIGN_BOTTOM_LEFT, 0, -6);

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

/* ── Page 2: Chat — opens text chat with Tinker ─────────────── */
static void chat_page_tap_cb(lv_event_t *e)
{
    (void)e;
    /* Launch Chat overlay (modal on lv_layer_top) */
    extern lv_obj_t *ui_chat_create(void);
    ui_chat_create();
}

static lv_obj_t *build_page_chat(void)
{
    lv_obj_t *pg = tiles[2];

    /* Tap anywhere to open Chat */
    lv_obj_add_flag(pg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pg, chat_page_tap_cb, LV_EVENT_CLICKED, NULL);

    /* Chat visual — big tappable card in center */
    lv_obj_t *card = lv_obj_create(pg);
    lv_obj_set_size(card, SW - 80, 280);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 32, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x333344), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_NEW_LINE);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 32);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, "Chat with Tinker");
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Type questions instead of speaking");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -28);

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
    /* Leave room at bottom for nav bar (120px) + page dots (36px) */
    lv_obj_set_size(tileview, SW, SH - NAV_H - 36);
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
    build_page_chat();
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

    /* ── DISCONNECT BANNER (hidden by default) ────────────── */
    s_disconnect_banner = lv_label_create(scr);
    lv_label_set_text(s_disconnect_banner, "  " LV_SYMBOL_WARNING " Dragon disconnected — reconnecting...");
    lv_obj_set_pos(s_disconnect_banner, 0, 56);
    lv_obj_set_size(s_disconnect_banner, 720, 32);
    lv_obj_set_style_bg_color(s_disconnect_banner, lv_color_hex(0xEF4444), 0);
    lv_obj_set_style_bg_opa(s_disconnect_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_disconnect_banner, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_disconnect_banner, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(s_disconnect_banner, 12, 0);
    lv_obj_add_flag(s_disconnect_banner, LV_OBJ_FLAG_HIDDEN);

    /* ── BOTTOM NAV ─────────────────────────────────────────── */
    {
        lv_obj_t *nav = lv_obj_create(scr);
        lv_obj_set_size(nav, SW, NAV_H);
        lv_obj_set_pos(nav, 0, SH - NAV_H);
        lv_obj_set_style_bg_color(nav, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(nav, 0, 0);
        lv_obj_set_style_border_width(nav, 0, 0);
        lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

        /* Nav labels — Home | Notes | Chat | Settings */
        const char *labels[NUM_PAGES] = {
            "Home", "Notes", "Chat", "Settings"
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
            lv_obj_set_ext_click_area(nav_icons[i], 8);
            lv_obj_add_event_cb(nav_icons[i], nav_click_cb, LV_EVENT_CLICKED,
                               (void *)(intptr_t)i);
        }
    }

    /* ── PAGE DOTS — BIG ───────────────────────────────────── */
    {
        int dot_sz = 20;
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

    /* Seed local badge mode from NVS so it starts in sync */
    s_badge_mode = tab5_settings_get_voice_mode();

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

static void orb_tap_cb(lv_event_t *e)
{
    (void)e;
    voice_state_t st = voice_get_state();

    if (st == VOICE_STATE_READY) {
        esp_err_t err = voice_start_listening();
        if (err == ESP_OK) {
            /* Bug2: Show voice overlay — orb_tap bypasses mic button's
             * s_pending_ask flag, so the state callback won't auto-show. */
            ui_voice_show();
        } else {
            show_toast("Voice not ready — try again");
            ESP_LOGW(TAG, "voice_start_listening failed: %s", esp_err_to_name(err));
        }
    } else if (st == VOICE_STATE_IDLE) {
        /* Not connected — force immediate reconnect (Bug4: resets backoff)
         * Enhancement 5: Show "Reconnecting..." toast and auto-listen on success */
        show_toast("Reconnecting...");
        voice_force_reconnect();
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

/* S1: Camera quick-launch from Home */
static void cb_camera_launch(lv_event_t *e)
{
    (void)e;
    extern lv_obj_t *ui_camera_create(void);
    ui_camera_create();
}

/* S2: Files quick-launch from Home */
static void cb_files_launch(lv_event_t *e)
{
    (void)e;
    extern lv_obj_t *ui_files_create(void);
    ui_files_create();
}

static void cb_last_note_tap(lv_event_t *e)
{
    (void)e;
    /* Open Notes overlay (same as nav bar Notes button) */
    dismiss_all_overlays();
    lv_async_call(async_notes_create, NULL);
}

static void update_mode_badge(uint8_t mode)
{
    if (!lbl_privacy) return;
    /* Mode labels, colors, and orb ring accent */
    static const char *labels[] = {"  Local  ", " Hybrid ", "  Cloud  ", "TinkerClaw"};
    static const uint32_t colors[] = {0x22C55E, 0xF59E0B, 0x3B82F6, 0xE11D48};
    if (mode >= VOICE_MODE_COUNT) mode = 0;

    lv_label_set_text(lbl_privacy, labels[mode]);
    lv_obj_set_style_bg_color(lbl_privacy, lv_color_hex(colors[mode]), 0);
    lv_obj_set_style_text_color(lbl_privacy, lv_color_hex(colors[mode]), 0);
    lv_obj_set_style_bg_opa(lbl_privacy, LV_OPA_30, 0);

    /* Update home orb ring color to match mode (US-PR14) */
    if (orb_ring) {
        static const uint32_t ring_colors[] = {0x22C55E, 0xEAB308, 0x3B82F6, 0xE11D48};
        uint32_t orb_color = ring_colors[mode];
        lv_obj_set_style_border_color(orb_ring, lv_color_hex(orb_color), 0);
    }
}

static void privacy_tap_cb(lv_event_t *e)
{
    (void)e;
    /* Cycle: Local(0) → Hybrid(1) → Cloud(2) → TinkerClaw(3) → Local(0) */
    uint8_t prev = s_badge_mode;
    s_badge_mode = (s_badge_mode + 1) % VOICE_MODE_COUNT;

    tab5_settings_set_voice_mode(s_badge_mode);

    /* Send config_update to Dragon */
    if (voice_is_connected()) {
        char model[64] = {0};
        tab5_settings_get_llm_model(model, sizeof(model));
        voice_send_config_update((int)s_badge_mode,
            (s_badge_mode >= 2) ? model : NULL);
    }

    /* Update badge */
    update_mode_badge(s_badge_mode);

    /* Toast */
    const char *names[] = {
        "Local — all on-device",
        "Hybrid — cloud STT+TTS",
        "Cloud — all cloud",
        "TinkerClaw — Agent mode",
    };
    show_toast(names[s_badge_mode]);

    ESP_LOGI(TAG, "Mode cycled: %d → %d", prev, s_badge_mode);
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

    lv_timer_t *tmr = lv_timer_create(toast_timer_cb, 3000, toast);
    lv_timer_set_repeat_count(tmr, 1);
}

/* Wrappers for lv_async_call — avoid cast between incompatible function types */
static void async_notes_create(void *arg) { (void)arg; ui_notes_create(); }
static void async_settings_create(void *arg) { (void)arg; ui_settings_create(); }

static void update_nav_ui(int page)
{
    for (int i = 0; i < NUM_PAGES; i++) {
        if (page_dots[i]) {
            lv_obj_set_style_bg_color(page_dots[i],
                lv_color_hex(i == page ? 0xF5A623 : 0x333333), 0);
        }
        if (nav_icons[i]) {
            lv_obj_set_style_text_color(nav_icons[i],
                lv_color_hex(i == page ? COL_AMBER : COL_LABEL2), 0);
        }
    }

    /* Gate JPEG rendering — only when in streaming mode (not Chat page) */
    tab5_display_set_jpeg_enabled(false);

    /* Show/hide persistent floating buttons:
     * - Page 0 (Home): hidden (home has orb + Ask Tinker)
     * - Page 3 (Settings): mic hidden (H5: overlaps scrollable content)
     * - Pages 1,2: both visible */
    lv_obj_t *mic = ui_voice_get_mic_btn();
    lv_obj_t *kbd = ui_keyboard_get_trigger_btn();
    if (page == 0 || page == 3) {
        if (mic) lv_obj_add_flag(mic, LV_OBJ_FLAG_HIDDEN);
        if (page == 0 && kbd) lv_obj_add_flag(kbd, LV_OBJ_FLAG_HIDDEN);
        if (page == 3 && kbd) lv_obj_clear_flag(kbd, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (mic) lv_obj_clear_flag(mic, LV_OBJ_FLAG_HIDDEN);
        if (kbd) lv_obj_clear_flag(kbd, LV_OBJ_FLAG_HIDDEN);
    }

    /* When switching to Notes page, load the notes screen */
    if (page == 1 && tiles[1]) {
        lv_async_call(async_notes_create, NULL);
    }
    /* Settings handled in nav_click_cb directly */
}

static void _async_settings(void *arg)
{
    (void)arg;
    extern lv_obj_t *ui_settings_create(void);
    ui_settings_create();
}

/* U09: Delayed settings open — one-shot timer gives LVGL a render cycle
 * to flush nav button pressed-state feedback before the heavy Settings
 * creation (~55 objects) monopolizes the render loop for ~500ms.
 * lv_async_call runs in the same lv_timer_handler() pass as style flush,
 * so the pressed state never reaches the display. A 30ms timer guarantees
 * at least one full render cycle (DPI refresh at ~16ms) completes first.
 * NOTE: lv_refr_now() cannot be used — causes internal heap exhaustion
 * on ESP32-P4 DPI display (see LEARNINGS.md). */
static void _delayed_settings_cb(lv_timer_t *t)
{
    (void)t;
    _async_settings(NULL);
}

void ui_home_nav_settings(void)
{
    lv_timer_t *t = lv_timer_create(_delayed_settings_cb, 30, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void _async_chat(void *arg)
{
    (void)arg;
    extern lv_obj_t *ui_chat_create(void);
    ui_chat_create();
}

/* Dismiss ALL overlays before opening any screen.
 *
 * C07 audit (April 2026): No use-after-free risk here.
 * ui_settings_hide() and ui_chat_hide() only set LV_OBJ_FLAG_HIDDEN (no deletion).
 * ui_settings_destroy() and ui_chat_destroy() use synchronous lv_obj_delete()/lv_obj_del(),
 * NOT lv_obj_del_async(). All deletion is immediate — no deferred-delete race. */
static void dismiss_all_overlays(void)
{
    ui_keyboard_hide();
    extern void ui_settings_hide(void);
    extern void ui_notes_hide(void);
    extern void ui_chat_destroy(void);
    ui_settings_hide();
    ui_notes_hide();
    extern void ui_chat_hide(void);
    ui_chat_hide();
}

/* U09: Delayed overlay open callbacks — same pattern as Settings.
 * 30ms delay lets LVGL flush nav button pressed-state to display. */
static void _delayed_notes_cb(lv_timer_t *t) { (void)t; async_notes_create(NULL); }
static void _delayed_chat_cb(lv_timer_t *t)  { (void)t; _async_chat(NULL); }

static void nav_click_cb(lv_event_t *e)
{
    int pg = (int)(intptr_t)lv_event_get_user_data(e);
    if (pg < 0 || pg >= NUM_PAGES) return;
    dismiss_all_overlays();
    if (pg == 1) {
        /* U09: Delay overlay open so nav pressed-state renders first */
        lv_timer_t *t = lv_timer_create(_delayed_notes_cb, 30, NULL);
        lv_timer_set_repeat_count(t, 1);
        return;
    }
    if (pg == 2) {
        /* U09: Delay overlay open so nav pressed-state renders first */
        lv_timer_t *t = lv_timer_create(_delayed_chat_cb, 30, NULL);
        lv_timer_set_repeat_count(t, 1);
        return;
    }
    if (pg == 3) {
        ui_home_nav_settings();
        return;
    }
    if (tiles[pg] && tileview) {
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
        /* Hide keyboard when swiping between pages — prevents
         * persistent keyboard from Notes blocking other screens */
        ui_keyboard_hide();
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

    /* S4: Connection dot reflects voice WS state (the real indicator) */
    {
        bool voice_ok = voice_is_connected();
        if (lbl_sbar_dragon) {
            lv_obj_set_style_bg_color(lbl_sbar_dragon,
                lv_color_hex(voice_ok ? COL_MINT : COL_RED), 0);
        }
        if (s_disconnect_banner) {
            if (!voice_ok) {
                lv_obj_clear_flag(s_disconnect_banner, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_disconnect_banner, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    /* US-PR25: Dim orb when voice is not ready (boot connect / disconnected).
     * Prevents user from tapping a normal-looking orb and getting "Connecting..." toast.
     * Uses lv_obj opa (not bg_opa/border_opa) so it composites over the breathing animation. */
    {
        voice_state_t vst = voice_get_state();
        bool voice_ready = (vst == VOICE_STATE_READY || vst == VOICE_STATE_LISTENING
                            || vst == VOICE_STATE_PROCESSING || vst == VOICE_STATE_SPEAKING);
        lv_opa_t dim = voice_ready ? LV_OPA_COVER : LV_OPA_40;
        if (orb_inner) lv_obj_set_style_opa(orb_inner, dim, 0);
        if (orb_ring)  lv_obj_set_style_opa(orb_ring, dim, 0);

        /* US-PR25: Update orb label to show connection progress.
         * "Connecting..." while IDLE/CONNECTING, "Tap to ask" once READY+. */
        if (lbl_ask) {
            if (vst == VOICE_STATE_IDLE || vst == VOICE_STATE_CONNECTING) {
                lv_label_set_text(lbl_ask, "Connecting...");
                lv_obj_set_style_text_color(lbl_ask, lv_color_hex(0x666666), 0);
            } else {
                lv_label_set_text(lbl_ask, "Tap to ask");
                lv_obj_set_style_text_color(lbl_ask, lv_color_hex(0x888888), 0);
            }
        }
    }

    /* Mode badge — keep in sync with NVS */
    /* Use local s_badge_mode — NVS gets overwritten by Dragon ACK */
    update_mode_badge(s_badge_mode);

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
            lv_label_set_text(lbl_last_note, "No notes yet - tap Voice to record");
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
        orb_inner = NULL;
        lbl_clock = lbl_date = lbl_ask = NULL;
        lbl_sbar_time = lbl_sbar_wifi = lbl_sbar_batt = NULL;
        lbl_sbar_dragon = NULL;
        s_disconnect_banner = NULL;
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

lv_obj_t *ui_home_get_tileview(void)
{
    return tileview;
}

lv_obj_t *ui_home_get_tile(int page)
{
    if (page < 0 || page >= NUM_PAGES) return NULL;
    return tiles[page];
}

void ui_home_go_home(void)
{
    if (tileview && tiles[0]) {
        lv_tileview_set_tile(tileview, tiles[0], LV_ANIM_OFF);
        cur_page = 0;
        update_nav_ui(0);
    }
}

void ui_home_refresh_mode_badge(void)
{
    s_badge_mode = tab5_settings_get_voice_mode();
    update_mode_badge(s_badge_mode);
}

void ui_home_show_toast(const char *text)
{
    show_toast(text);
}
