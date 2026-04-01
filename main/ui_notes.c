/*
 * ui_notes.c — TinkerTab Notes Screen
 *
 * Voice-first notes: tap the mic to record a voice note,
 * or use the keyboard to type. Notes shown as a scrollable
 * list of cards, most recent first.
 *
 * Storage: static ring buffer (survives screen navigations,
 * not reboots). SD card persistence is a future enhancement.
 */

#include "ui_notes.h"
#include "ui_home.h"
#include "ui_core.h"
#include "ui_voice.h"
#include "ui_chat.h"
#include "voice.h"
#include "rtc.h"
#include "settings.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_notes";

/* ── Palette ──────────────────────────────────────────────── */
#define COL_BG         0x0D0D0D
#define COL_CARD       0x1C1C1E
#define COL_CARD2      0x2C2C2E
#define COL_AMBER      0xFFB800
#define COL_CYAN       0x00B4D8
#define COL_MINT       0x30D158
#define COL_WHITE      0xFFFFFF
#define COL_LABEL      0xEBEBF5
#define COL_LABEL2     0x8E8E93
#define COL_LABEL3     0x48484A

/* ── Layout ─────────────────────────────────────────────── */
#define SW             720
#define SH             1280
#define TOPBAR_H       56
#define CARD_PAD       16
#define CARD_RAD       16
#define MAX_NOTES      30
#define MAX_NOTE_LEN   512

/* ── Note storage ───────────────────────────────────────── */
typedef struct {
    char text[MAX_NOTE_LEN];
    bool is_voice;      /* true = voice note, false = typed */
    uint8_t hour;
    uint8_t minute;
    uint8_t day;
    uint8_t month;
    bool used;           /* slot is occupied */
} note_entry_t;

static note_entry_t s_notes[MAX_NOTES];
static int s_note_count = 0;      /* used entries */
static int s_next_slot  = 0;      /* next write position */

/* ── Screen state ──────────────────────────────────────── */
static lv_obj_t *s_screen   = NULL;
static lv_obj_t *s_list    = NULL;
static lv_timer_t *s_timer  = NULL;

/* ── Forward decls ─────────────────────────────────────── */
static void cb_back(lv_event_t *e);
static void cb_new_voice(lv_event_t *e);
static void cb_new_text(lv_event_t *e);
static void cb_note_tap(lv_event_t *e);
static void refresh_list(void);
static void add_note_card(lv_obj_t *parent, const note_entry_t *note);
static lv_obj_t *make_topbar(lv_obj_t *parent);

/* ── Note storage API ──────────────────────────────────── */
int ui_notes_add(const char *text, bool is_voice)
{
    if (!text || !text[0]) return -1;

    tab5_rtc_time_t rtc = {0};
    tab5_rtc_get_time(&rtc);

    note_entry_t *n = &s_notes[s_next_slot];
    memset(n, 0, sizeof(*n));
    strncpy(n->text, text, MAX_NOTE_LEN - 1);
    n->text[MAX_NOTE_LEN - 1] = '\0';
    n->is_voice = is_voice;
    n->hour  = rtc.hour;
    n->minute = rtc.minute;
    n->day   = rtc.day;
    n->month = rtc.month;
    n->used  = true;

    /* Advance ring buffer */
    s_next_slot = (s_next_slot + 1) % MAX_NOTES;
    if (s_note_count < MAX_NOTES) s_note_count++;

    ESP_LOGI(TAG, "Note added [%s]: %.40s",
             is_voice ? "voice" : "text", text);

    return s_note_count - 1;
}

static const char *month_names[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

static void format_time(char *buf, size_t len, const note_entry_t *n)
{
    snprintf(buf, len, "%s %d, %02d:%02d",
             month_names[n->month - 1], n->day, n->hour, n->minute);
}

/* ── Refresh list ───────────────────────────────────────── */
static void refresh_list(void)
{
    if (!s_list) return;

    /* Clear existing cards */
    lv_obj_clean(s_list);

    if (s_note_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty,
            "No notes yet.\n\nTap the mic to record\nor type to add a note.");
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_LABEL2), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    /* Show notes newest-first */
    for (int i = 0; i < MAX_NOTES; i++) {
        int idx = (s_next_slot - 1 - i + MAX_NOTES) % MAX_NOTES;
        if (!s_notes[idx].used) continue;
        add_note_card(s_list, &s_notes[idx]);
    }
}

/* ── Note card widget ──────────────────────────────────── */
static void add_note_card(lv_obj_t *parent, const note_entry_t *note)
{
    /* Card container */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, CARD_RAD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Timestamp row */
    lv_obj_t *ts = lv_label_create(card);
    char ts_buf[32];
    format_time(ts_buf, sizeof(ts_buf), note);
    lv_label_set_text(ts, ts_buf);
    lv_obj_set_style_text_color(ts, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(ts, &lv_font_montserrat_14, 0);

    /* Type badge */
    lv_obj_t *badge = lv_label_create(card);
    lv_label_set_text(badge, note->is_voice ? "  voice  " : "  text  ");
    lv_obj_set_style_bg_color(badge, lv_color_hex(
        note->is_voice ? COL_CYAN : COL_AMBER), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_30, 0);
    lv_obj_set_style_text_color(badge, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_radius(badge, 6, 0);
    /* Position badge next to timestamp */
    lv_obj_align_to(badge, ts, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    /* Note preview — first line only */
    lv_obj_t *preview = lv_label_create(card);
    /* Truncate to ~60 chars for preview */
    char preview_text[MAX_NOTE_LEN + 4];
    snprintf(preview_text, sizeof(preview_text), "%.60s%s",
             note->text,
             strlen(note->text) > 60 ? "..." : "");
    lv_label_set_text(preview, preview_text);
    lv_obj_set_style_text_color(preview, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(preview, &lv_font_montserrat_18, 0);
    lv_obj_set_width(preview, lv_pct(100));

    /* Divider */
    lv_obj_t *div = lv_obj_create(card);
    lv_obj_set_width(div, lv_pct(100));
    lv_obj_set_height(div, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_radius(div, 0, 0);
}

/* ── Top bar ───────────────────────────────────────────── */
static lv_obj_t *make_topbar(lv_obj_t *parent)
{
    lv_obj_t *tb = lv_obj_create(parent);
    lv_obj_set_size(tb, SW, TOPBAR_H);
    lv_obj_set_pos(tb, 0, 0);
    lv_obj_set_style_bg_color(tb, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tb, 0, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t *btn = lv_button_create(tb);
    lv_obj_set_size(btn, 80, 36);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "< Home");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(btn_lbl);

    /* Title */
    lv_obj_t *title = lv_label_create(tb);
    lv_label_set_text(title, "Notes");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    return tb;
}

/* ── New note buttons ─────────────────────────────────── */
static void add_new_note_buttons(lv_obj_t *parent)
{
    /* Container */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 80);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Voice button */
    lv_obj_t *vbtn = lv_button_create(row);
    lv_obj_set_size(vbtn, 160, 56);
    lv_obj_align(vbtn, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_color(vbtn, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_bg_opa(vbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vbtn, 14, 0);
    lv_obj_set_style_border_width(vbtn, 0, 0);
    lv_obj_add_event_cb(vbtn, cb_new_voice, LV_EVENT_CLICKED, NULL);

    lv_obj_t *vicon = lv_label_create(vbtn);
    lv_label_set_text(vicon, "Mic  Voice");
    lv_obj_set_style_text_color(vicon, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(vicon, &lv_font_montserrat_18, 0);
    lv_obj_center(vicon);

    /* Text button */
    lv_obj_t *tbtn = lv_button_create(row);
    lv_obj_set_size(tbtn, 160, 56);
    lv_obj_align(tbtn, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(tbtn, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_bg_opa(tbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tbtn, 14, 0);
    lv_obj_set_style_border_width(tbtn, 0, 0);
    lv_obj_add_event_cb(tbtn, cb_new_text, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ticon = lv_label_create(tbtn);
    lv_label_set_text(ticon, "Aa  Text");
    lv_obj_set_style_text_color(ticon, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(ticon, &lv_font_montserrat_18, 0);
    lv_obj_center(ticon);
}

/* ── Callbacks ────────────────────────────────────────── */
static void cb_back(lv_event_t *e)
{
    (void)e;
    ui_notes_destroy();
    lv_screen_load(ui_home_get_screen());
}

static void cb_new_voice(lv_event_t *e)
{
    (void)e;
    /* Show a toast hint — voice activation is via the voice overlay */
    ESP_LOGI(TAG, "Voice note — tap mic button on home screen");
    /* Actually open voice overlay */
    if (voice_get_state() == VOICE_STATE_IDLE) {
        /* Trigger voice recording */
        voice_start_listening();
    }
}

static void cb_new_text(lv_event_t *e)
{
    (void)e;
    /* Open chat with keyboard for text input */
    lv_async_call((lv_async_cb_t)ui_chat_create, NULL);
}

/* ── Screen create/destroy ─────────────────────────────── */
lv_obj_t *ui_notes_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, SW, SH);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Top bar */
    make_topbar(s_screen);

    /* New note buttons */
    lv_obj_t *btn_container = lv_obj_create(s_screen);
    lv_obj_set_size(btn_container, SW, 80);
    lv_obj_set_pos(btn_container, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_container, 0, 0);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);
    add_new_note_buttons(btn_container);

    /* Divider */
    lv_obj_t *div = lv_obj_create(s_screen);
    lv_obj_set_size(div, SW, 1);
    lv_obj_set_pos(div, 0, TOPBAR_H + 80);
    lv_obj_set_style_bg_color(div, lv_color_hex(COL_CARD2), 0);

    /* Scrollable notes list */
    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, SW, SH - TOPBAR_H - 80 - 1);
    lv_obj_set_pos(s_list, 0, TOPBAR_H + 80 + 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 12, 0);
    lv_obj_set_style_pad_hor(s_list, 16, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ON);

    /* Populate notes */
    refresh_list();

    lv_screen_load(s_screen);
    ESP_LOGI(TAG, "Notes screen created, %d notes", s_note_count);
    return s_screen;
}

void ui_notes_destroy(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_list = NULL;
}
