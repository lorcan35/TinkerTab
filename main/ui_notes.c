/*
 * ui_notes.c — TinkerTab Notes Screen
 *
 * Full CRUD notes: create (voice or text), read (list), delete (swipe/button).
 * Voice notes: tap Voice → Dragon STT → note saved when session ends.
 * Text notes: tap Text → keyboard opens inline → type + Enter to save.
 *
 * Storage: ring buffer in RAM, persisted to /sdcard/notes.json (SD card).
 * Falls back to NVS blob if SD unavailable. Saved on every add/delete.
 */

#include "ui_notes.h"
#include "ui_home.h"
#include "ui_core.h"
#include "ui_voice.h"
#include "ui_keyboard.h"
#include "voice.h"
#include "rtc.h"
#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

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

/* ── Layout — BIG TOUCH TARGETS ─────────────────────────────────── */
#define SW             720
#define SH             1280
#define TOPBAR_H       120     /* was 56 */
#define INPUT_H        140     /* was 64 */
#define NAV_H          140     /* was 64 */
#define CARD_PAD       48      /* was 16 */
#define CARD_RAD       24      /* was 16 */
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
static bool s_loaded    = false;   /* NVS loaded at least once */

/* ── Persistence — SD card JSON file ────────────────────── */
/*
 * Notes are stored as a JSON array in /sdcard/notes.json.
 * Each note: {"t":"text","v":1,"ts":"2026-04-01T19:05"}
 * Human-readable, easily accessible by plugging the SD card into a PC.
 * Falls back to NVS blob if SD card is not mounted.
 */
#define NOTES_SD_PATH  "/sdcard/notes.js"
#define NVS_NAMESPACE  "notes"
#define NVS_KEY_DATA   "data"

#include "sdcard.h"
#include "cJSON.h"

static void notes_save(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;

    for (int i = 0; i < MAX_NOTES; i++) {
        if (!s_notes[i].used) continue;
        const note_entry_t *n = &s_notes[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "t", n->text);
        cJSON_AddNumberToObject(obj, "v", n->is_voice ? 1 : 0);
        cJSON_AddNumberToObject(obj, "h", n->hour);
        cJSON_AddNumberToObject(obj, "m", n->minute);
        cJSON_AddNumberToObject(obj, "d", n->day);
        cJSON_AddNumberToObject(obj, "mo", n->month);
        cJSON_AddNumberToObject(obj, "y", n->year);
        cJSON_AddNumberToObject(obj, "i", i);  /* slot index */
        cJSON_AddItemToArray(arr, obj);
    }

    /* Wrap in envelope with ring buffer state */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", s_note_count);
    cJSON_AddNumberToObject(root, "next", s_next_slot);
    cJSON_AddItemToObject(root, "notes", arr);

    char *json = cJSON_Print(root);  /* pretty-printed for readability on PC */
    cJSON_Delete(root);
    if (!json) return;

    /* Try SD card first */
    if (tab5_sdcard_mounted()) {
        FILE *f = fopen(NOTES_SD_PATH, "w");
        if (f) {
            int written = fputs(json, f);
            fclose(f);
            if (written >= 0) {
                ESP_LOGI(TAG, "Notes saved to SD (%d notes)", s_note_count);
                free(json);
                return;
            }
        }
        ESP_LOGW(TAG, "SD write failed (errno=%d), falling back to NVS", errno);
    }

    /* Fallback: NVS blob (compact binary for size) */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_DATA, json, strlen(json) + 1);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Notes saved to NVS fallback (%d notes)", s_note_count);
    }
    free(json);
}

static void notes_load(void)
{
    if (s_loaded) return;
    s_loaded = true;

    char *json = NULL;

    /* Try SD card first */
    if (tab5_sdcard_mounted()) {
        FILE *f = fopen(NOTES_SD_PATH, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 64 * 1024) {
                json = malloc(sz + 1);
                if (json) {
                    fread(json, 1, sz, f);
                    json[sz] = '\0';
                }
            }
            fclose(f);
            if (json) {
                ESP_LOGI(TAG, "Reading notes from SD (%ld bytes)", sz);
            }
        }
    }

    /* Fallback: NVS */
    if (!json) {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            size_t blob_size = 0;
            if (nvs_get_blob(h, NVS_KEY_DATA, NULL, &blob_size) == ESP_OK && blob_size > 2) {
                json = malloc(blob_size);
                if (json) {
                    nvs_get_blob(h, NVS_KEY_DATA, json, &blob_size);
                    ESP_LOGI(TAG, "Reading notes from NVS fallback (%u bytes)",
                             (unsigned)blob_size);
                }
            }
            nvs_close(h);
        }
    }

    if (!json) {
        ESP_LOGI(TAG, "No saved notes found");
        return;
    }

    /* Parse JSON */
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        ESP_LOGW(TAG, "Notes JSON parse failed");
        return;
    }

    memset(s_notes, 0, sizeof(s_notes));
    s_note_count = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "count"));
    s_next_slot  = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "next"));

    cJSON *arr = cJSON_GetObjectItem(root, "notes");
    int loaded = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        int slot = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "i"));
        if (slot < 0 || slot >= MAX_NOTES) continue;

        note_entry_t *n = &s_notes[slot];
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "t"));
        if (text) {
            strncpy(n->text, text, MAX_NOTE_LEN - 1);
            n->text[MAX_NOTE_LEN - 1] = '\0';
        }
        n->is_voice = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "v")) != 0;
        n->hour     = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "h"));
        n->minute   = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "m"));
        n->day      = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "d"));
        n->month    = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "mo"));
        n->year     = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "y"));
        n->used = true;
        loaded++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d notes", loaded);
}

/* ── Screen state ──────────────────────────────────────── */
static lv_obj_t *s_screen      = NULL;
static lv_obj_t *s_list        = NULL;
static lv_obj_t *s_input_area  = NULL;
static lv_obj_t *s_input_btn   = NULL;
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
        voice_session_done();
    }
}

/* ── Note storage API ──────────────────────────────────── */
int ui_notes_add(const char *text, bool is_voice)
{
    if (!text || !text[0]) return -1;
    notes_load();  /* ensure loaded */

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

    notes_save();
    refresh_list();
    return 0;
}

void ui_notes_delete(int idx)
{
    if (idx < 0 || idx >= MAX_NOTES || !s_notes[idx].used) return;
    s_notes[idx].used = false;
    s_note_count--;
    notes_save();
    refresh_list();
    ESP_LOGI(TAG, "Note deleted");
}

void ui_notes_list(void)
{
    notes_load();
    printf("%d note(s) stored\n", s_note_count);
    for (int i = 0; i < MAX_NOTES; i++) {
        if (s_notes[i].used) {
            printf("  [%d] %s: %.60s\n",
                   i,
                   s_notes[i].is_voice ? "V" : "T",
                   s_notes[i].text);
        }
    }
}

bool ui_notes_get_last_preview(char *buf, size_t len)
{
    notes_load();
    if (!buf || s_note_count == 0) return false;
    int last_idx = (s_next_slot - 1 + MAX_NOTES) % MAX_NOTES;
    if (!s_notes[last_idx].used) return false;
    snprintf(buf, len, "%.80s", s_notes[last_idx].text);
    return true;
}

int ui_notes_count(void)
{
    notes_load();
    return s_note_count;
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

    /* Textarea — big and readable */
    s_input_area = lv_textarea_create(s_screen);
    lv_obj_set_size(s_input_area, SW - 180 - 16, INPUT_H);
    lv_obj_set_pos(s_input_area, 8, SH - INPUT_H - 8);
    lv_textarea_set_placeholder_text(s_input_area, "Type a note...");
    lv_textarea_set_one_line(s_input_area, true);
    lv_obj_set_style_bg_color(s_input_area, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_text_color(s_input_area, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(s_input_area, &lv_font_montserrat_36, 0);
    lv_obj_set_style_radius(s_input_area, 16, 0);
    lv_obj_set_style_border_width(s_input_area, 0, 0);
    lv_obj_set_style_pad_hor(s_input_area, 20, 0);
    lv_obj_add_event_cb(s_input_area, cb_input_send, LV_EVENT_READY, NULL);

    /* Save button — huge touch target */
    s_input_btn = lv_button_create(s_screen);
    lv_obj_set_size(s_input_btn, 160, INPUT_H);
    lv_obj_set_pos(s_input_btn, SW - 168, SH - INPUT_H - 8);
    lv_obj_set_style_bg_color(s_input_btn, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_opa(s_input_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_input_btn, 16, 0);
    lv_obj_set_style_border_width(s_input_btn, 0, 0);
    lv_obj_add_event_cb(s_input_btn, cb_input_send, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(s_input_btn);
    lv_label_set_text(btn_lbl, "Save");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_36, 0);
    lv_obj_center(btn_lbl);

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
    ui_home_go_home();
    lv_screen_load(ui_home_get_screen());
}

static void cb_new_voice(lv_event_t *e)
{
    (void)e;
    if (voice_get_state() == VOICE_STATE_IDLE) {
        s_voice_recording = true;
        voice_start_listening();
        ESP_LOGI(TAG, "Voice recording — note saves when session ends");
    } else {
        ESP_LOGI(TAG, "Voice busy (state %d)", voice_get_state());
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
    int note_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (note_idx < 0 || note_idx >= MAX_NOTES || !s_notes[note_idx].used) return;

    const note_entry_t *n = &s_notes[note_idx];
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, lv_pct(85), lv_pct(70));
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(overlay, 28, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 40, 0);
    lv_obj_add_event_cb(overlay, (lv_event_cb_t)lv_obj_del, LV_EVENT_CLICKED, NULL);

    /* Timestamp */
    char ts_buf[64];
    snprintf(ts_buf, sizeof(ts_buf), "%s %d, %02d:%02d — %s note",
             (const char *[]){"Jan","Feb","Mar","Apr","May","Jun",
              "Jul","Aug","Sep","Oct","Nov","Dec"}[n->month - 1],
             n->day, n->hour, n->minute,
             n->is_voice ? "Voice" : "Text");
    lv_obj_t *ts = lv_label_create(overlay);
    lv_label_set_text(ts, ts_buf);
    lv_obj_set_style_text_color(ts, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(ts, &lv_font_montserrat_28, 0);

    /* Full note text */
    lv_obj_t *lbl = lv_label_create(overlay);
    lv_label_set_text(lbl, n->text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_36, 0);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_style_pad_top(lbl, 20, 0);
}

static void cb_note_delete(lv_event_t *e)
{
    int note_idx = (int)(intptr_t)lv_event_get_user_data(e);
    ui_notes_delete(note_idx);
}

/* ── Note card widget ──────────────────────────────────── */
static void add_note_card(lv_obj_t *parent, const note_entry_t *note, int note_idx)
{
    note_entry_t n = *note;

    /* Card — big padding, full width */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 160, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, CARD_RAD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(card, 20);
    lv_obj_add_event_cb(card, cb_note_tap, LV_EVENT_CLICKED,
                       (void *)(intptr_t)note_idx);

    /* Row 1: timestamp + badge */
    lv_obj_t *ts = lv_label_create(card);
    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%s %d, %02d:%02d",
             (const char *[]){"Jan","Feb","Mar","Apr","May","Jun",
              "Jul","Aug","Sep","Oct","Nov","Dec"}[n.month - 1],
             n.day, n.hour, n.minute);
    lv_label_set_text(ts, ts_buf);
    lv_obj_set_style_text_color(ts, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(ts, &lv_font_montserrat_28, 0);

    /* Type badge — bigger */
    lv_obj_t *badge = lv_label_create(card);
    lv_label_set_text(badge, n.is_voice ? "  voice  " : "  text  ");
    lv_obj_set_style_bg_color(badge, lv_color_hex(n.is_voice ? COL_CYAN : COL_AMBER), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_40, 0);
    lv_obj_set_style_text_color(badge, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_24, 0);
    lv_obj_set_style_radius(badge, 12, 0);
    lv_obj_align_to(badge, ts, LV_ALIGN_OUT_RIGHT_MID, 16, 0);

    /* Delete button — big touch target */
    lv_obj_t *del = lv_button_create(card);
    lv_obj_set_size(del, 80, 56);
    lv_obj_align(del, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_80, 0);
    lv_obj_set_style_radius(del, 16, 0);
    lv_obj_set_style_border_width(del, 0, 0);
    lv_obj_add_event_cb(del, cb_note_delete, LV_EVENT_CLICKED,
                       (void *)(intptr_t)note_idx);

    lv_obj_t *del_lbl = lv_label_create(del);
    lv_label_set_text(del_lbl, "X");
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(del_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(del_lbl);

    /* Note text — BIG readable font */
    lv_obj_t *preview = lv_label_create(card);
    lv_label_set_text(preview, n.text);
    lv_obj_set_style_text_color(preview, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(preview, &lv_font_montserrat_36, 0);
    lv_obj_set_width(preview, lv_pct(100));
    lv_obj_set_style_pad_top(preview, 20, 0);
}

/* ── Refresh list ───────────────────────────────────────── */
static void refresh_list(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);

    if (s_note_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "No notes yet.\n\nTap Voice to record\na note, or Text to type one.");
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_LABEL2), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

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

    /* Back button — big touch target */
    lv_obj_t *btn = lv_button_create(tb);
    lv_obj_set_size(btn, 120, 64);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "< Home");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_36, 0);
    lv_obj_center(btn_lbl);

    /* Title — big and centered */
    lv_obj_t *title = lv_label_create(tb);
    lv_label_set_text(title, "Notes");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_center(title);

    return tb;
}

/* ── Screen create/destroy ─────────────────────────────── */
lv_obj_t *ui_notes_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, SW, SH);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    make_topbar(s_screen);

    /* ── Big Voice + Text buttons ──────────────────────── */
    lv_obj_t *btn_row = lv_obj_create(s_screen);
    lv_obj_set_size(btn_row, SW, 160);
    lv_obj_set_pos(btn_row, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Voice button — HUGE */
    lv_obj_t *vbtn = lv_button_create(btn_row);
    lv_obj_set_size(vbtn, SW/2 - 24, 120);
    lv_obj_align(vbtn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(vbtn, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_bg_opa(vbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vbtn, 20, 0);
    lv_obj_set_style_border_width(vbtn, 0, 0);
    lv_obj_add_event_cb(vbtn, cb_new_voice, LV_EVENT_CLICKED, NULL);

    lv_obj_t *vicon = lv_label_create(vbtn);
    lv_label_set_text(vicon, "Mic\nVoice Note");
    lv_obj_set_style_text_color(vicon, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(vicon, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_align(vicon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(vicon);

    /* Text button — HUGE */
    lv_obj_t *tbtn = lv_button_create(btn_row);
    lv_obj_set_size(tbtn, SW/2 - 24, 120);
    lv_obj_align(tbtn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(tbtn, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_bg_opa(tbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tbtn, 20, 0);
    lv_obj_set_style_border_width(tbtn, 0, 0);
    lv_obj_add_event_cb(tbtn, cb_new_text, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ticon = lv_label_create(tbtn);
    lv_label_set_text(ticon, "Aa\nType Note");
    lv_obj_set_style_text_color(ticon, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(ticon, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_align(ticon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ticon);

    /* Divider */
    lv_obj_t *div = lv_obj_create(s_screen);
    lv_obj_set_size(div, SW, 2);
    lv_obj_set_pos(div, 0, TOPBAR_H + 160);
    lv_obj_set_style_bg_color(div, lv_color_hex(COL_CARD2), 0);

    /* Scrollable notes list */
    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, SW, SH - TOPBAR_H - 160 - 2);
    lv_obj_set_pos(s_list, 0, TOPBAR_H + 160 + 2);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 20, 0);
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
