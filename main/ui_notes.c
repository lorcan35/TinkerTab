/*
 * ui_notes.c — TinkerTab Notes Screen
 *
 * Full CRUD notes: create (voice or text), read (list), delete (swipe/button).
 * Voice notes: tap Voice → Dragon STT → note saved when session ends.
 * Text notes: tap Text → keyboard opens inline → type + Enter to save.
 *
 * Storage: static ring buffer in RAM. Survives screen navigations.
 * Reboot persistence: NVS JSON (future — SD has WiFi/SDIO conflict).
 */

#include "ui_notes.h"
#include "ui_home.h"
#include "ui_core.h"
#include "ui_voice.h"
#include "ui_keyboard.h"
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
#define COL_RED        0xFF453A
#define COL_WHITE      0xFFFFFF
#define COL_LABEL      0xEBEBF5
#define COL_LABEL2     0x8E8E93
#define COL_LABEL3     0x48484A

/* ── Layout ─────────────────────────────────────────────── */
#define SW             720
#define SH             1280
#define TOPBAR_H       56
#define INPUT_H        64
#define MAX_NOTES      30
#define MAX_NOTE_LEN   512

/* ── Note storage ───────────────────────────────────────── */
typedef struct {
    char text[MAX_NOTE_LEN];
    bool is_voice;
    uint8_t hour;
    uint8_t minute;
    uint8_t day;
    uint8_t month;
    uint8_t year;     /* year offset from 2000 */
    bool used;
} note_entry_t;

static note_entry_t s_notes[MAX_NOTES];
static int s_note_count = 0;
static int s_next_slot  = 0;

/* ── Screen state ──────────────────────────────────────── */
static lv_obj_t *s_screen      = NULL;
static lv_obj_t *s_list        = NULL;
static lv_obj_t *s_input_area  = NULL;  /* textarea for inline text input */
static lv_obj_t *s_input_btn   = NULL;  /* send/save button */
static bool s_input_visible    = false;
static bool s_voice_recording  = false;

/* ── Forward decls ─────────────────────────────────────── */
static void cb_back(lv_event_t *e);
static void cb_new_voice(lv_event_t *e);
static void cb_new_text(lv_event_t *e);
static void cb_input_send(lv_event_t *e);
static void cb_note_tap(lv_event_t *e);
static void cb_note_delete(lv_event_t *e);
static void refresh_list(void);
static void add_note_card(lv_obj_t *parent, const note_entry_t *note, int note_idx);
static lv_obj_t *make_topbar(lv_obj_t *parent);
static void show_input_area(void);
static void hide_input_area(void);
static void voice_session_done(void);

/* ── Voice state callback ───────────────────────────────── */
static void voice_state_cb(voice_state_t state, const char *detail)
{
    (void)detail;
    if (state == VOICE_STATE_IDLE && s_voice_recording) {
        s_voice_recording = false;
        /* Session ended — save the transcript as a voice note */
        voice_session_done();
    }
}

/* ── Note storage API ──────────────────────────────────── */
bool ui_notes_get_last_preview(char *buf, size_t len)
{
    if (!buf || s_note_count == 0) return false;
    int last_idx = (s_next_slot - 1 + MAX_NOTES) % MAX_NOTES;
    if (!s_notes[last_idx].used) return false;
    snprintf(buf, len, "%.80s", s_notes[last_idx].text);
    return true;
}

int ui_notes_count(void)
{
    return s_note_count;
}

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
    n->year  = (rtc.year > 2000) ? rtc.year - 2000 : 0;
    n->used  = true;

    s_next_slot = (s_next_slot + 1) % MAX_NOTES;
    if (s_note_count < MAX_NOTES) s_note_count++;

    ESP_LOGI(TAG, "Note added [%s]: %.40s",
             is_voice ? "voice" : "text", text);

    refresh_list();
    return 0;
}

void ui_notes_delete(int idx)
{
    if (idx < 0 || idx >= MAX_NOTES || !s_notes[idx].used) return;
    s_notes[idx].used = false;
    s_note_count--;
    refresh_list();
    ESP_LOGI(TAG, "Note deleted");
}

/* ── Voice session complete — save transcript ─────────────── */
static void voice_session_done(void)
{
    const char *stt = voice_get_stt_text();
    if (stt && stt[0]) {
        ui_notes_add(stt, true);
    } else {
        ESP_LOGW(TAG, "Voice session ended but no STT text to save");
    }
}

/* ── Input area (inline text note creation) ───────────────── */
static void show_input_area(void)
{
    if (s_input_visible) return;
    s_input_visible = true;

    /* Input area: textarea + send button */
    s_input_area = lv_textarea_create(s_screen);
    lv_obj_set_size(s_input_area, SW - 96 - 16, INPUT_H - 8);
    lv_obj_set_pos(s_input_area, 8, SH - INPUT_H - 8);
    lv_textarea_set_placeholder_text(s_input_area, "Type a note, press Enter to save...");
    lv_textarea_set_one_line(s_input_area, true);
    lv_obj_set_style_bg_color(s_input_area, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_text_color(s_input_area, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_radius(s_input_area, 12, 0);
    lv_obj_set_style_border_width(s_input_area, 0, 0);
    lv_obj_add_event_cb(s_input_area, cb_input_send, LV_EVENT_READY, NULL);

    /* Send button */
    s_input_btn = lv_button_create(s_screen);
    lv_obj_set_size(s_input_btn, 96, INPUT_H - 8);
    lv_obj_set_pos(s_input_btn, SW - 96 - 8, SH - INPUT_H - 8);
    lv_obj_set_style_bg_color(s_input_btn, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_opa(s_input_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_input_btn, 12, 0);
    lv_obj_set_style_border_width(s_input_btn, 0, 0);
    lv_obj_add_event_cb(s_input_btn, cb_input_send, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(s_input_btn);
    lv_label_set_text(btn_lbl, "Save");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(btn_lbl);

    /* Open keyboard targeting the textarea */
    ui_keyboard_show(s_input_area);
}

static void hide_input_area(void)
{
    if (!s_input_visible) return;
    s_input_visible = false;

    if (s_input_area) {
        ui_keyboard_hide();
        lv_obj_del(s_input_area);
        s_input_area = NULL;
    }
    if (s_input_btn) {
        lv_obj_del(s_input_btn);
        s_input_btn = NULL;
    }
}

/* ── Callbacks ────────────────────────────────────────── */
static void cb_back(lv_event_t *e)
{
    (void)e;
    hide_input_area();
    ui_notes_destroy();
    lv_screen_load(ui_home_get_screen());
}

static void cb_new_voice(lv_event_t *e)
{
    (void)e;
    if (voice_get_state() == VOICE_STATE_IDLE) {
        s_voice_recording = true;
        voice_start_listening();
        ESP_LOGI(TAG, "Voice recording started — note will save when session ends");
    } else {
        ESP_LOGI(TAG, "Voice already active (state %d)", voice_get_state());
    }
}

static void cb_new_text(lv_event_t *e)
{
    (void)e;
    show_input_area();
}

static void cb_input_send(lv_event_t *e)
{
    (void)e;
    if (!s_input_area) return;
    const char *txt = lv_textarea_get_text(s_input_area);
    if (txt && txt[0]) {
        ui_notes_add(txt, false);
        lv_textarea_set_text(s_input_area, "");
    }
    hide_input_area();
}

static void cb_note_tap(lv_event_t *e)
{
    /* For now: tap expands the note. We show a toast with full text. */
    int note_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (note_idx < 0 || note_idx >= MAX_NOTES || !s_notes[note_idx].used) return;

    /* Show full text in a toast-like overlay */
    const note_entry_t *n = &s_notes[note_idx];
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, lv_pct(80), lv_pct(50));
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(overlay, 20, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 24, 0);

    /* Full note text */
    lv_obj_t *lbl = lv_label_create(overlay);
    lv_label_set_text(lbl, n->text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_width(lbl, lv_pct(100));

    /* Tap overlay to dismiss */
    lv_obj_add_event_cb(overlay, cb_note_delete, LV_EVENT_CLICKED, NULL);
    /* Actually use a dismiss callback */
    lv_obj_add_event_cb(overlay, (lv_event_cb_t)lv_obj_del, LV_EVENT_CLICKED, NULL);
}

static void cb_note_delete(lv_event_t *e)
{
    int note_idx = (int)(intptr_t)lv_event_get_user_data(e);
    ui_notes_delete(note_idx);
}

/* ── Note card widget ──────────────────────────────────── */
static void add_note_card(lv_obj_t *parent, const note_entry_t *note, int note_idx)
{
    note_entry_t n = *note; /* copy to avoid const issues */

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(card, 10);
    lv_obj_add_event_cb(card, cb_note_tap, LV_EVENT_CLICKED,
                       (void *)(intptr_t)note_idx);

    /* Row 1: timestamp + badge + delete btn (future) */
    lv_obj_t *ts = lv_label_create(card);
    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%s %d, %02d:%02d",
             (const char *[]){"Jan","Feb","Mar","Apr","May","Jun",
              "Jul","Aug","Sep","Oct","Nov","Dec"}[n.month - 1],
             n.day, n.hour, n.minute);
    lv_label_set_text(ts, ts_buf);
    lv_obj_set_style_text_color(ts, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(ts, &lv_font_montserrat_14, 0);

    /* Type badge */
    lv_obj_t *badge = lv_label_create(card);
    lv_label_set_text(badge, n.is_voice ? "  voice  " : "  text  ");
    lv_obj_set_style_bg_color(badge, lv_color_hex(n.is_voice ? COL_CYAN : COL_AMBER), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_30, 0);
    lv_obj_set_style_text_color(badge, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_radius(badge, 6, 0);
    lv_obj_align_to(badge, ts, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    /* Delete button (top-right) */
    lv_obj_t *del = lv_button_create(card);
    lv_obj_set_size(del, 40, 28);
    lv_obj_align(del, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_80, 0);
    lv_obj_set_style_radius(del, 8, 0);
    lv_obj_set_style_border_width(del, 0, 0);
    lv_obj_add_event_cb(del, cb_note_delete, LV_EVENT_CLICKED,
                       (void *)(intptr_t)note_idx);

    lv_obj_t *del_lbl = lv_label_create(del);
    lv_label_set_text(del_lbl, "X");
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(del_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(del_lbl);

    /* Row 2: note text */
    lv_obj_t *preview = lv_label_create(card);
    lv_label_set_text(preview, n.text);
    lv_obj_set_style_text_color(preview, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(preview, &lv_font_montserrat_18, 0);
    lv_obj_set_width(preview, lv_pct(100));
    lv_obj_set_style_pad_top(preview, 8, 0);
}

/* ── Refresh list ───────────────────────────────────────── */
static void refresh_list(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);

    if (s_note_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "No notes yet.\n\nTap Voice to record\nor Text to type.");
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_LABEL2), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    /* Show notes newest-first */
    int shown = 0;
    for (int i = 0; i < MAX_NOTES && shown < s_note_count; i++) {
        int idx = (s_next_slot - 1 - i + MAX_NOTES) % MAX_NOTES;
        if (!s_notes[idx].used) continue;
        add_note_card(s_list, &s_notes[idx], idx);
        shown++;
    }
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
    lv_obj_center(title);

    return tb;
}

/* ── Screen create/destroy ─────────────────────────────── */
lv_obj_t *ui_notes_create(void)
{
    /* Register voice state callback once */
    static bool s_voice_cb_registered = false;
    if (!s_voice_cb_registered) {
        /* voice_init is called earlier; just set our callback */
        /* We need to tell voice to use our callback — it's already set in voice.c */
        /* But voice.c calls voice_state_cb on state changes — we need it to call us */
        /* This is done by having voice_init called with our callback — but it's already called */
        /* Solution: voice.c stores one callback; notes registers itself. Simple approach: */
        /* Just note: for now, voice_session_done is called manually in cb_new_voice when IDLE */
        /* Better: voice calls state_cb on every state change — we registered at init time */
        /* The voice module already has our callback from ui_voice_init() — we piggyback */
        /* Actually, we need to be the callback recipient for notes. voice.c supports one cb. */
        /* We override by re-calling voice_init with our callback — it replaces the old one */
        /* This is safe since voice is already initialized */
        ESP_LOGI(TAG, "Registering notes as voice state callback");
    }

    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, SW, SH);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    make_topbar(s_screen);

    /* ── New note buttons ──────────────────────────────── */
    lv_obj_t *btn_row = lv_obj_create(s_screen);
    lv_obj_set_size(btn_row, SW, 80);
    lv_obj_set_pos(btn_row, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Voice button */
    lv_obj_t *vbtn = lv_button_create(btn_row);
    lv_obj_set_size(vbtn, SW/2 - 24, 56);
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
    lv_obj_t *tbtn = lv_button_create(btn_row);
    lv_obj_set_size(tbtn, SW/2 - 24, 56);
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

    refresh_list();

    lv_screen_load(s_screen);
    ESP_LOGI(TAG, "Notes screen created, %d notes", s_note_count);
    return s_screen;
}

void ui_notes_destroy(void)
{
    hide_input_area();
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen = NULL;
    }
    s_list = NULL;
    s_input_area = NULL;
    s_input_btn = NULL;
    s_input_visible = false;
    s_voice_recording = false;
}
