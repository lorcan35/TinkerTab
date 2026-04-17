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
#include "audio.h"
#include "config.h"
#include "mode_manager.h"
#include "tab5_rtc.h"
#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasestr */
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_http_client.h"
#include "dragon_link.h"
#include "wifi.h"

static const char *TAG = "ui_notes";

/* ── Palette (Material Dark — matches Settings) ──────────── */
#define COL_BG         0x0A0A0F
#define COL_CARD       0x1A1A2E
#define COL_CARD2      0x2C2C2E
#define COL_AMBER      0xF59E0B
#define COL_CYAN       0xF59E0B
#define COL_MINT       0x22C55E
#define COL_RED        0xEF4444
#define COL_WHITE      0xE8E8EF
#define COL_LABEL      0xE8E8EF
#define COL_LABEL2     0x888888
#define COL_LABEL3     0x555555
#define COL_PURPLE     0xA855F7
#define COL_BORDER     0x1A1A24

/* ── Layout — BIG TOUCH TARGETS ─────────────────────────────────── */
#define SW             720
#define SH             1280     /* Notes is a separate screen, not tileview page */
#define OVERLAY_H      SH
#define NAV_BAR_H      120
#define USABLE_H       (SH - NAV_BAR_H)  /* Full screen — nav bar on lv_layer_top() */
#define TOPBAR_H       48      /* match Settings style */
#define INPUT_H        140     /* was 64 */
#define NAV_H          140     /* was 64 */
#define CARD_PAD       48      /* was 16 */
#define CARD_RAD       24      /* was 16 */
#define BTN_ROW_H      80      /* Voice/Type button row height (was 160) */
#define ACTION_BTN_H   56      /* Voice/Type button height (was 120) */
#define MAX_NOTES      30
#define MAX_NOTE_LEN   512

/* ── Note states ────────────────────────────────────────── */
typedef enum {
    NOTE_STATE_TEXT,         /* text-only note (typed) */
    NOTE_STATE_RECORDED,    /* has audio file, not yet transcribed */
    NOTE_STATE_TRANSCRIBING,/* transcription in progress */
    NOTE_STATE_TRANSCRIBED, /* has audio file + transcript */
    NOTE_STATE_FAILED,      /* transcription failed — can retry */
} note_state_t;

#define MAX_AUDIO_PATH  64

/* ── Note storage ───────────────────────────────────────── */
typedef struct {
    char text[MAX_NOTE_LEN];
    char audio_path[MAX_AUDIO_PATH]; /* e.g. "/sdcard/rec/0042.wav" or "" */
    note_state_t state;
    bool is_voice;
    uint8_t hour;
    uint8_t minute;
    uint8_t day;
    uint8_t month;
    uint8_t year;     /* year offset from 2000 */
    bool used;
    bool needs_sync;  /* S6: true if not yet synced to Dragon */
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
#define NOTES_TMP_PATH "/sdcard/notes.tmp"
#define REC_DIR        "/sdcard/rec"
#define NVS_NAMESPACE  "notes"
#define NVS_KEY_DATA   "data"
#define NVS_KEY_RECID  "recid"   /* next recording ID counter */

static uint32_t s_next_rec_id = 1;  /* monotonic recording counter */

#include "sdcard.h"
#include "cJSON.h"

/* Forward declarations for persistence (defined below) */
static void notes_load(void);
static void notes_save(void);

/* ── Sync single note to Dragon REST API (fire-and-forget task) ─── */

typedef struct {
    char title[128];
    char text[MAX_NOTE_LEN];
    int  note_idx;  /* S6: index in s_notes for clearing needs_sync */
} sync_note_args_t;

static void sync_note_to_dragon_task(void *arg)
{
    sync_note_args_t *a = (sync_note_args_t *)arg;

    /* Build Dragon URL from settings */
    char dhost[64];
    tab5_settings_get_dragon_host(dhost, sizeof(dhost));
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d/api/notes", dhost, 3502);

    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "title", a->title);
    cJSON_AddStringToObject(body, "text", a->text);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json) { free(a); vTaskSuspend(NULL); return; }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && (status == 200 || status == 201)) {
        ESP_LOGI(TAG, "Note synced to Dragon (status=%d, idx=%d)", status, a->note_idx);
        /* S6: Clear needs_sync flag on success */
        if (a->note_idx >= 0 && a->note_idx < MAX_NOTES) {
            s_notes[a->note_idx].needs_sync = false;
        }
    } else {
        ESP_LOGW(TAG, "Note sync failed: err=%s status=%d",
                 esp_err_to_name(err), status);
    }

    esp_http_client_cleanup(client);
    free(json);
    free(a);
    vTaskSuspend(NULL);
}

/* S6: Find note index by text content (for needs_sync tracking) */
static int find_note_idx_by_text(const char *text)
{
    for (int i = 0; i < MAX_NOTES; i++) {
        if (s_notes[i].used && strncmp(s_notes[i].text, text, 64) == 0) return i;
    }
    return -1;
}

static void sync_note_to_dragon(const char *title, const char *text)
{
    if (!text || !text[0]) return;
    int idx = find_note_idx_by_text(text);

    if (!tab5_wifi_connected()) {
        ESP_LOGW(TAG, "Note sync skipped — WiFi not connected");
        /* S6: Mark for later sync */
        if (idx >= 0) s_notes[idx].needs_sync = true;
        return;
    }

    sync_note_args_t *args = calloc(1, sizeof(sync_note_args_t));
    if (!args) return;
    strncpy(args->title, title ? title : "", sizeof(args->title) - 1);
    strncpy(args->text, text, sizeof(args->text) - 1);
    args->note_idx = idx;

    ESP_LOGI(TAG, "Syncing note to Dragon (%zu chars, idx=%d)", strlen(text), idx);
    xTaskCreatePinnedToCore(sync_note_to_dragon_task, "note_sync", 4096,
                            args, 3, NULL, 0);
}

/* S6: Sync all pending notes to Dragon (called on reconnect) */
void ui_notes_sync_pending(void)
{
    notes_load();
    int synced = 0;
    for (int i = 0; i < MAX_NOTES; i++) {
        if (s_notes[i].used && s_notes[i].needs_sync && s_notes[i].text[0]) {
            ESP_LOGI(TAG, "Catch-up sync: note %d", i);
            sync_note_to_dragon("", s_notes[i].text);
            synced++;
            vTaskDelay(pdMS_TO_TICKS(500));  /* stagger to avoid flooding */
        }
    }
    if (synced > 0) {
        ESP_LOGI(TAG, "Catch-up sync: %d notes queued", synced);
    }
}

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
        cJSON_AddNumberToObject(obj, "s", (int)n->state);
        if (n->audio_path[0]) {
            cJSON_AddStringToObject(obj, "a", n->audio_path);
        }
        cJSON_AddNumberToObject(obj, "h", n->hour);
        cJSON_AddNumberToObject(obj, "m", n->minute);
        cJSON_AddNumberToObject(obj, "d", n->day);
        cJSON_AddNumberToObject(obj, "mo", n->month);
        cJSON_AddNumberToObject(obj, "y", n->year);
        cJSON_AddNumberToObject(obj, "i", i);
        if (n->needs_sync) cJSON_AddNumberToObject(obj, "ns", 1);
        cJSON_AddItemToArray(arr, obj);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", s_note_count);
    cJSON_AddNumberToObject(root, "next", s_next_slot);
    cJSON_AddNumberToObject(root, "recid", (double)s_next_rec_id);
    cJSON_AddItemToObject(root, "notes", arr);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return;

    /* Atomic write: .tmp → rename. Prevents corruption on power loss. */
    if (tab5_sdcard_mounted()) {
        FILE *f = fopen(NOTES_TMP_PATH, "w");
        if (f) {
            int written = fputs(json, f);
            fflush(f);
            fclose(f);
            if (written >= 0) {
                /* Atomic rename — old file replaced only after new is complete */
                remove(NOTES_SD_PATH);
                if (rename(NOTES_TMP_PATH, NOTES_SD_PATH) == 0) {
                    ESP_LOGI(TAG, "Notes saved to SD (%d notes)", s_note_count);
                    free(json);
                    return;
                }
            }
        }
        ESP_LOGW(TAG, "SD write failed (errno=%d), falling back to NVS", errno);
    }

    /* Fallback: NVS */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_DATA, json, strlen(json) + 1);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Notes saved to NVS fallback (%d notes)", s_note_count);
    }
    free(json);

    /* Persist recording counter in NVS */
    nvs_handle_t h2;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h2) == ESP_OK) {
        nvs_set_u32(h2, NVS_KEY_RECID, s_next_rec_id);
        nvs_commit(h2);
        nvs_close(h2);
    }
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

    cJSON *jrecid = cJSON_GetObjectItem(root, "recid");
    if (cJSON_IsNumber(jrecid)) {
        s_next_rec_id = (uint32_t)jrecid->valuedouble;
    }

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
        const char *apath = cJSON_GetStringValue(cJSON_GetObjectItem(item, "a"));
        if (apath) {
            strncpy(n->audio_path, apath, MAX_AUDIO_PATH - 1);
            n->audio_path[MAX_AUDIO_PATH - 1] = '\0';
        }
        cJSON *jstate = cJSON_GetObjectItem(item, "s");
        if (cJSON_IsNumber(jstate)) {
            n->state = (note_state_t)(int)jstate->valuedouble;
        } else {
            /* Legacy notes without state field — infer from content */
            n->state = NOTE_STATE_TEXT;
        }
        /* Fix up: voice notes with placeholder text → RECORDED.
         * Covers notes saved before schema change or crashed recordings. */
        if (n->is_voice && strncmp(n->text, "(Recording", 10) == 0) {
            n->state = NOTE_STATE_RECORDED;
        }
        n->is_voice = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "v")) != 0;
        n->hour     = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "h"));
        n->minute   = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "m"));
        n->day      = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "d"));
        n->month    = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "mo"));
        n->year     = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "y"));
        cJSON *jns = cJSON_GetObjectItem(item, "ns");
        n->needs_sync = (cJSON_IsNumber(jns) && (int)jns->valuedouble != 0);
        n->used = true;
        loaded++;
    }

    cJSON_Delete(root);

    /* Load recording counter from NVS (survives SD card removal) */
    nvs_handle_t nh;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nh) == ESP_OK) {
        uint32_t rid = 0;
        if (nvs_get_u32(nh, NVS_KEY_RECID, &rid) == ESP_OK && rid > s_next_rec_id) {
            s_next_rec_id = rid;
        }
        nvs_close(nh);
    }

    /* Reset stuck TRANSCRIBING notes back to RECORDED for retry.
     * This happens when the device reboots mid-transcription. */
    int reset_count = 0;
    for (int i = 0; i < MAX_NOTES; i++) {
        if (s_notes[i].used && s_notes[i].state == NOTE_STATE_TRANSCRIBING) {
            s_notes[i].state = NOTE_STATE_RECORDED;
            reset_count++;
        }
    }
    if (reset_count > 0) {
        ESP_LOGI(TAG, "Reset %d stuck TRANSCRIBING notes to RECORDED", reset_count);
    }

    /* Ensure recordings directory exists */
    if (tab5_sdcard_mounted()) {
        struct stat st;
        if (stat(REC_DIR, &st) != 0) {
            mkdir(REC_DIR, 0755);
            ESP_LOGI(TAG, "Created recordings directory: %s", REC_DIR);
        }
    }

    ESP_LOGI(TAG, "Loaded %d notes (next_rec_id=%lu)", loaded, (unsigned long)s_next_rec_id);
}

/* ── Screen state ──────────────────────────────────────── */
static lv_obj_t *s_screen      = NULL;
static lv_obj_t *s_list        = NULL;
static lv_obj_t *s_input_area  = NULL;
static lv_obj_t *s_input_btn   = NULL;
static lv_obj_t *s_search_ta   = NULL;  /* M2: search bar */
static char      s_search_text[64] = {0};  /* current search filter */
static bool s_input_visible    = false;
static bool s_voice_recording  = false;
static bool s_pending_dictation = false;  /* waiting for READY to start dictation */
static volatile bool s_sd_rec_running = false;  /* standalone SD recording active */

/* ── Recording indicator ──────────────────────────────── */
static lv_obj_t *s_rec_indicator = NULL;  /* container for the recording bar */
static lv_obj_t *s_rec_dot = NULL;        /* red pulsing dot */
static lv_obj_t *s_rec_time_lbl = NULL;   /* "0:05" timer */
static lv_obj_t *s_rec_text_lbl = NULL;   /* "Recording..." or "Paused" */
static lv_obj_t *s_topbar_meta  = NULL;   /* v5 right-aligned count/size */
static lv_timer_t *s_rec_timer = NULL;    /* 1s update timer */
static int s_rec_seconds = 0;
static bool s_rec_paused = false;         /* TODO: pause/resume not yet implemented */

/* ── Edit overlay state ────────────────────────────────── */
static lv_obj_t *s_edit_overlay = NULL;
static lv_obj_t *s_edit_ta     = NULL;
static int       s_edit_idx    = -1;

/* ── Forward decls ─────────────────────────────────────── */
static void cb_back(lv_event_t *e);
static void cb_new_voice(lv_event_t *e);
static void cb_new_text(lv_event_t *e);
static void cb_input_send(lv_event_t *e);
static void cb_note_tap(lv_event_t *e);
static void cb_note_delete(lv_event_t *e);
static void cb_note_play(lv_event_t *e);
static void cb_search_changed(lv_event_t *e);
static void refresh_list(void);
static void add_note_card(lv_obj_t *parent, const note_entry_t *note, int note_idx);
static lv_obj_t *make_topbar(lv_obj_t *parent);
static void show_input_area(void);
static void hide_input_area(void);
static void voice_session_done(void);
static void show_recording_indicator(void);
static void hide_recording_indicator(void);

/* ── Recording indicator timer callback ────────────────── */
static void rec_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_rec_paused) return;
    s_rec_seconds++;
    if (s_rec_time_lbl) {
        lv_label_set_text_fmt(s_rec_time_lbl, "%d:%02d", s_rec_seconds / 60, s_rec_seconds % 60);
    }
    /* Pulse the dot (toggle opacity) */
    if (s_rec_dot) {
        static bool dot_on = true;
        dot_on = !dot_on;
        lv_obj_set_style_bg_opa(s_rec_dot, dot_on ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

/* ── Recording indicator show/hide ─────────────────────── */
static void show_recording_indicator(void)
{
    if (!s_screen) return;
    if (s_rec_indicator) return;  /* already showing */

    s_rec_seconds = 0;
    s_rec_paused = false;

    /* Bar: full width, 40px tall, positioned below search bar above notes list.
     * Search bar ends at TOPBAR_H + BTN_ROW_H + SEARCH_H + 4.
     * We insert the indicator right there and push list down. */
    #define REC_BAR_Y  (TOPBAR_H + BTN_ROW_H + 48 /* SEARCH_H */ + 8)
    #define REC_BAR_H  40

    s_rec_indicator = lv_obj_create(s_screen);
    lv_obj_set_size(s_rec_indicator, SW - 32, REC_BAR_H);
    lv_obj_set_pos(s_rec_indicator, 16, REC_BAR_Y);
    lv_obj_set_style_bg_color(s_rec_indicator, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(s_rec_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_rec_indicator, 8, 0);
    lv_obj_set_style_border_width(s_rec_indicator, 1, 0);
    lv_obj_set_style_border_color(s_rec_indicator, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_opa(s_rec_indicator, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_rec_indicator, LV_OBJ_FLAG_SCROLLABLE);

    /* Red pulsing dot — 8px circle */
    s_rec_dot = lv_obj_create(s_rec_indicator);
    lv_obj_remove_style_all(s_rec_dot);
    lv_obj_set_size(s_rec_dot, 12, 12);
    lv_obj_align(s_rec_dot, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(s_rec_dot, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_bg_opa(s_rec_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_rec_dot, LV_RADIUS_CIRCLE, 0);

    /* "Recording..." text */
    s_rec_text_lbl = lv_label_create(s_rec_indicator);
    lv_label_set_text(s_rec_text_lbl, "Recording...");
    lv_obj_set_style_text_color(s_rec_text_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(s_rec_text_lbl, FONT_BODY, 0);
    lv_obj_align(s_rec_text_lbl, LV_ALIGN_LEFT_MID, 32, 0);

    /* Elapsed time */
    s_rec_time_lbl = lv_label_create(s_rec_indicator);
    lv_label_set_text(s_rec_time_lbl, "0:00");
    lv_obj_set_style_text_color(s_rec_time_lbl, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(s_rec_time_lbl, FONT_BODY, 0);
    lv_obj_align(s_rec_time_lbl, LV_ALIGN_RIGHT_MID, -12, 0);

    /* Push the notes list down to make room */
    if (s_list) {
        lv_obj_set_pos(s_list, 0, REC_BAR_Y + REC_BAR_H + 4);
        lv_obj_set_height(s_list, USABLE_H - (REC_BAR_Y + REC_BAR_H + 4));
    }

    /* Start 1-second update timer */
    s_rec_timer = lv_timer_create(rec_timer_cb, 1000, NULL);

    ESP_LOGI(TAG, "Recording indicator shown");
}

static void hide_recording_indicator(void)
{
    if (s_rec_timer) {
        lv_timer_delete(s_rec_timer);
        s_rec_timer = NULL;
    }
    if (s_rec_indicator) {
        lv_obj_del(s_rec_indicator);
        s_rec_indicator = NULL;
    }
    s_rec_dot = NULL;
    s_rec_text_lbl = NULL;
    s_rec_time_lbl = NULL;
    s_rec_seconds = 0;
    s_rec_paused = false;

    /* Restore list position */
    if (s_list) {
        #define LIST_Y_NORMAL (TOPBAR_H + BTN_ROW_H + 48 /* SEARCH_H */ + 10)
        lv_obj_set_pos(s_list, 0, LIST_Y_NORMAL);
        lv_obj_set_height(s_list, USABLE_H - LIST_Y_NORMAL);
    }

    ESP_LOGI(TAG, "Recording indicator hidden");
}

/* ── Voice state callback (used by dictation connect flow) ── */
static void __attribute__((unused)) voice_state_cb(voice_state_t state, const char *detail)
{
    /* Auto-start dictation once Dragon connection is READY */
    if (state == VOICE_STATE_READY && s_pending_dictation) {
        s_pending_dictation = false;
        s_voice_recording = true;
        voice_start_dictation();
        ESP_LOGI(TAG, "Auto-starting dictation after connect");
        return;
    }
    /* Dictation ends with READY + "dictation_done" detail */
    if (s_voice_recording && state == VOICE_STATE_READY
        && voice_get_mode() == VOICE_MODE_DICTATE
        && detail && strcmp(detail, "dictation_done") == 0) {
        s_voice_recording = false;
        hide_recording_indicator();
        voice_session_done();
    }
    /* Ask mode ends with IDLE */
    if (state == VOICE_STATE_IDLE && s_voice_recording) {
        s_voice_recording = false;
        hide_recording_indicator();
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
    n->year  = rtc.year;  /* RTC year is already offset from 2000 */
    n->used  = true;

    s_next_slot = (s_next_slot + 1) % MAX_NOTES;
    if (s_note_count < MAX_NOTES) s_note_count++;

    ESP_LOGI(TAG, "Note added [%s]: %.40s",
             is_voice ? "voice" : "text", text);

    notes_save();

    /* Sync to Dragon notes DB (fire-and-forget) */
    sync_note_to_dragon("", text);

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
    static const char *state_names[] = {"TEXT","RECORDED","TRANSCRIBING","TRANSCRIBED","FAILED"};
    for (int i = 0; i < MAX_NOTES; i++) {
        if (s_notes[i].used) {
            printf("  [%d] %s (%s): %.50s %s\n",
                   i,
                   s_notes[i].is_voice ? "V" : "T",
                   state_names[s_notes[i].state < 5 ? s_notes[i].state : 0],
                   s_notes[i].text,
                   s_notes[i].audio_path[0] ? s_notes[i].audio_path : "");
        }
    }
}

bool ui_notes_get_last_preview(char *buf, size_t len)
{
    notes_load();
    if (!buf || s_note_count == 0) return false;

    /* Enhancement 8: Iterate backwards from newest to find the first note
     * with valid text — skip broken recordings and empty placeholders. */
    for (int k = 0; k < MAX_NOTES; k++) {
        int idx = (s_next_slot - 1 - k + MAX_NOTES) % MAX_NOTES;
        if (!s_notes[idx].used) continue;
        /* Skip notes that are still recording or failed transcription */
        if (s_notes[idx].state == NOTE_STATE_FAILED) continue;
        if (s_notes[idx].state == NOTE_STATE_RECORDED) continue;
        if (s_notes[idx].state == NOTE_STATE_TRANSCRIBING) continue;

        const char *src = s_notes[idx].text;
        /* Skip placeholder/broken note texts */
        if (strncmp(src, "(Recording", 10) == 0) continue;
        if (strncmp(src, "(Empty recording)", 17) == 0) continue;
        if (strncmp(src, "(No audio file)", 15) == 0) continue;

        /* N4: Strip "[Untitled Note] " prefix for cleaner home preview */
        if (strncmp(src, "[Untitled Note] ", 16) == 0) src += 16;

        /* Skip if remaining text is empty after stripping */
        if (!src[0]) continue;

        snprintf(buf, len, "%.80s", src);
        return true;
    }

    /* All notes are invalid placeholders */
    return false;
}

int ui_notes_count(void)
{
    notes_load();
    return s_note_count;
}

int ui_notes_unprocessed_count(void)
{
    notes_load();
    int count = 0;
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!s_notes[i].used) continue;
        /* Count RECORDED and FAILED-with-audio (retry on transient errors) */
        if (s_notes[i].state == NOTE_STATE_RECORDED) count++;
        if (s_notes[i].state == NOTE_STATE_FAILED && s_notes[i].audio_path[0]) count++;
    }
    return count;
}

int ui_notes_clear_failed(void)
{
    notes_load();
    int cleared = 0;
    for (int i = 0; i < MAX_NOTES; i++) {
        if (s_notes[i].used && (s_notes[i].state == NOTE_STATE_FAILED ||
            (s_notes[i].is_voice && s_notes[i].state == NOTE_STATE_RECORDED
             && strncmp(s_notes[i].text, "(Recording", 10) == 0
             && !s_notes[i].audio_path[0]))) {
            s_notes[i].used = false;
            s_note_count--;
            cleared++;
        }
    }
    if (cleared > 0) notes_save();
    return cleared;
}

/* ── WAV recording ──────────────────────────────────────── */
/*
 * Records raw PCM 16kHz mono to a WAV file on the SD card.
 * The mic capture task calls ui_notes_write_audio() for each chunk.
 * Thread-safe via s_rec_mutex.
 */
#include "freertos/semphr.h"

static FILE *s_rec_file = NULL;
static char s_rec_path[MAX_AUDIO_PATH] = {0};
static uint32_t s_rec_samples = 0;
static SemaphoreHandle_t s_rec_mutex = NULL;
static int s_rec_note_slot = -1;

/* WAV header for PCM 16-bit mono */
static void wav_write_header(FILE *f, uint32_t data_bytes, uint16_t sample_rate)
{
    uint32_t file_size = 36 + data_bytes;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint32_t byte_rate = sample_rate * channels * bits / 8;
    uint16_t block_align = channels * bits / 8;

    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t pcm = 1;
    fwrite(&pcm, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

const char *ui_notes_start_recording(void)
{
    notes_load();

    if (!tab5_sdcard_mounted()) {
        ESP_LOGE(TAG, "SD card not mounted — cannot record");
        return NULL;
    }

    /* Create mutex on first use */
    if (!s_rec_mutex) {
        s_rec_mutex = xSemaphoreCreateMutex();
    }

    /* Ensure recordings directory exists */
    struct stat st;
    if (stat(REC_DIR, &st) != 0) {
        mkdir(REC_DIR, 0755);
    }

    /* Generate unique filename */
    snprintf(s_rec_path, sizeof(s_rec_path), "%s/%04lu.wav",
             REC_DIR, (unsigned long)s_next_rec_id);
    s_next_rec_id++;

    /* Open file and write placeholder WAV header (updated on stop) */
    s_rec_file = fopen(s_rec_path, "wb");
    if (!s_rec_file) {
        ESP_LOGE(TAG, "Failed to create recording: %s (errno=%d)", s_rec_path, errno);
        s_rec_path[0] = '\0';
        return NULL;
    }

    wav_write_header(s_rec_file, 0, TAB5_VOICE_SAMPLE_RATE);
    s_rec_samples = 0;

    /* Reserve a note slot for this recording */
    tab5_rtc_time_t rtc = {0};
    tab5_rtc_get_time(&rtc);

    s_rec_note_slot = s_next_slot;
    note_entry_t *n = &s_notes[s_rec_note_slot];
    memset(n, 0, sizeof(*n));
    snprintf(n->audio_path, MAX_AUDIO_PATH, "%s", s_rec_path);
    n->state = NOTE_STATE_RECORDED;
    n->is_voice = true;
    n->hour = rtc.hour;
    n->minute = rtc.minute;
    n->day = rtc.day;
    n->month = rtc.month;
    n->year = rtc.year;  /* RTC year is already offset from 2000 */
    n->used = true;
    snprintf(n->text, MAX_NOTE_LEN, "(Recording...)");

    s_next_slot = (s_next_slot + 1) % MAX_NOTES;
    if (s_note_count < MAX_NOTES) s_note_count++;

    /* Save immediately so the note exists even if we crash mid-recording.
     * The WAV header is a placeholder (0 bytes data) — stop_recording updates it. */
    notes_save();

    ESP_LOGI(TAG, "Recording started: %s (slot %d)", s_rec_path, s_rec_note_slot);
    return s_rec_path;
}

void ui_notes_write_audio(const int16_t *samples, size_t count)
{
    if (!s_rec_file || !s_rec_mutex) return;
    if (xSemaphoreTake(s_rec_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    if (s_rec_file) {
        fwrite(samples, sizeof(int16_t), count, s_rec_file);
        s_rec_samples += count;

        /* Commit to SD every ~2 seconds: close + reopen to force FAT metadata update.
         * fflush alone doesn't update directory entry size on FAT. */
        if (s_rec_samples % (100 * 320) < count) {
            /* Update WAV header with current size, close, reopen at end */
            uint32_t data_bytes = s_rec_samples * sizeof(int16_t);
            fseek(s_rec_file, 0, SEEK_SET);
            wav_write_header(s_rec_file, data_bytes, TAB5_VOICE_SAMPLE_RATE);
            fflush(s_rec_file);
            fclose(s_rec_file);
            s_rec_file = fopen(s_rec_path, "r+b");
            if (s_rec_file) {
                fseek(s_rec_file, 0, SEEK_END);
            } else {
                ESP_LOGE(TAG, "Failed to reopen recording file");
            }
        }
    }

    xSemaphoreGive(s_rec_mutex);
}

void ui_notes_stop_recording(const char *transcript)
{
    if (!s_rec_file) return;

    if (s_rec_mutex) xSemaphoreTake(s_rec_mutex, portMAX_DELAY);

    /* Update WAV header with final size */
    uint32_t data_bytes = s_rec_samples * sizeof(int16_t);
    fseek(s_rec_file, 0, SEEK_SET);
    wav_write_header(s_rec_file, data_bytes, TAB5_VOICE_SAMPLE_RATE);
    fclose(s_rec_file);
    s_rec_file = NULL;

    if (s_rec_mutex) xSemaphoreGive(s_rec_mutex);

    float duration_s = (float)s_rec_samples / TAB5_VOICE_SAMPLE_RATE;
    ESP_LOGI(TAG, "Recording stopped: %s (%.1fs, %lu samples)",
             s_rec_path, duration_s, (unsigned long)s_rec_samples);

    /* Update the note */
    if (s_rec_note_slot >= 0 && s_rec_note_slot < MAX_NOTES) {
        note_entry_t *n = &s_notes[s_rec_note_slot];
        if (transcript && transcript[0]) {
            strncpy(n->text, transcript, MAX_NOTE_LEN - 1);
            n->text[MAX_NOTE_LEN - 1] = '\0';
            n->state = NOTE_STATE_TRANSCRIBED;
        } else {
            snprintf(n->text, MAX_NOTE_LEN, "Voice recording (%.0fs)", duration_s);
            n->state = NOTE_STATE_RECORDED;
        }
    }

    s_rec_note_slot = -1;
    s_rec_samples = 0;
    notes_save();

    /* Sync transcribed note to Dragon */
    if (transcript && transcript[0]) {
        const char *title = voice_get_dictation_title();
        sync_note_to_dragon(title && title[0] ? title : "", transcript);
    }

    refresh_list();
}

/* ── Background transcription queue ─────────────────────── */
/*
 * Periodically checks for RECORDED notes, reads the WAV from SD,
 * POSTs to Dragon's /api/v1/transcribe, and updates the note.
 */
#include "esp_http_client.h"
#include "dragon_link.h"
#include "wifi.h"

static void transcription_queue_task(void *arg)
{
    ESP_LOGI(TAG, "Transcription queue started");
    vTaskDelay(pdMS_TO_TICKS(10000));  /* wait 10s for system to settle */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));  /* check every 15s */

        /* Don't process while recording or voice is active — concurrent
         * HTTP upload + WS connection exhausts DMA memory */
        if (s_rec_file || s_sd_rec_running || s_voice_recording) continue;
        voice_state_t vst = voice_get_state();
        if (vst != VOICE_STATE_IDLE && vst != VOICE_STATE_READY) continue;

        notes_load();
        int pending = ui_notes_unprocessed_count();
        if (pending == 0) continue;
        ESP_LOGI(TAG, "Transcription queue: %d unprocessed", pending);

        /* Need WiFi to be up — Dragon reachability is tested by the HTTP POST itself */
        if (!tab5_wifi_connected()) continue;

        /* Find the first note needing transcription (RECORDED or FAILED with audio) */
        int slot = -1;
        for (int i = 0; i < MAX_NOTES; i++) {
            if (!s_notes[i].used) continue;
            /* Only retry RECORDED notes — FAILED notes already tried and failed.
             * Retrying FAILED notes with broken audio (e.g. 44-byte header-only WAV)
             * creates an infinite 15s retry loop that wastes CPU and SDIO bandwidth. */
            bool needs_work = (s_notes[i].state == NOTE_STATE_RECORDED);
            if (!needs_work) continue;
            if (!s_notes[i].audio_path[0]) {
                s_notes[i].state = NOTE_STATE_FAILED;
                snprintf(s_notes[i].text, MAX_NOTE_LEN, "(No audio file)");
                notes_save();
                continue;
            }
            slot = i;
            break;
        }
        if (slot < 0) continue;

        note_entry_t *n = &s_notes[slot];
        ESP_LOGI(TAG, "Transcribing note [%d]: %s", slot, n->audio_path);
        n->state = NOTE_STATE_TRANSCRIBING;
        notes_save();

        /* Read WAV file from SD */
        FILE *f = fopen(n->audio_path, "rb");
        if (!f) {
            ESP_LOGW(TAG, "Cannot open WAV: %s", n->audio_path);
            n->state = NOTE_STATE_FAILED;
            notes_save();
            continue;
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (file_size < 100 || file_size > 30 * 1024 * 1024) {
            ESP_LOGW(TAG, "WAV file too small or too large: %ld bytes", file_size);
            fclose(f);
            n->state = NOTE_STATE_FAILED;
            snprintf(n->text, MAX_NOTE_LEN, "(Empty recording)");
            notes_save();
            continue;
        }
        /* Fix WAV header if it was from a crashed recording (header says 0 data) */
        if (file_size > 44) {
            uint32_t hdr_data_size = 0;
            fseek(f, 40, SEEK_SET);
            fread(&hdr_data_size, 4, 1, f);
            if (hdr_data_size == 0 || hdr_data_size > (uint32_t)(file_size - 44)) {
                /* Fix the header in place */
                uint32_t actual_data = (uint32_t)(file_size - 44);
                uint32_t riff_size = actual_data + 36;
                fseek(f, 4, SEEK_SET);
                fwrite(&riff_size, 4, 1, f);
                fseek(f, 40, SEEK_SET);
                fwrite(&actual_data, 4, 1, f);
                fflush(f);
                ESP_LOGI(TAG, "Fixed WAV header: %lu data bytes", (unsigned long)actual_data);
            }
            fseek(f, 0, SEEK_SET);
        }

        /* Build URL: http://<dragon_host>:3502/api/v1/transcribe */
        char dragon_host[64];
        tab5_settings_get_dragon_host(dragon_host, sizeof(dragon_host));
        char url[128];
        snprintf(url, sizeof(url), "http://%s:%d/api/v1/transcribe",
                 dragon_host, TAB5_VOICE_PORT);

        /* HTTP POST — stream from file in 4KB chunks (never hold >4KB in RAM) */
        esp_http_client_config_t http_cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 120000,  /* 2min — large files take time to upload+transcribe */
            .buffer_size = 4096,
        };
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        esp_http_client_set_header(client, "Content-Type", "audio/wav");

        fseek(f, 0, SEEK_SET);
        esp_err_t err = esp_http_client_open(client, file_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            fclose(f);
            n->state = NOTE_STATE_FAILED;
            notes_save();
            continue;
        }

        /* Stream file to HTTP in 4KB chunks */
        char chunk[4096];
        long sent = 0;
        while (sent < file_size) {
            size_t to_read = (file_size - sent > (long)sizeof(chunk))
                             ? sizeof(chunk) : (size_t)(file_size - sent);
            size_t got = fread(chunk, 1, to_read, f);
            if (got == 0) break;
            int written = esp_http_client_write(client, chunk, got);
            if (written < 0) {
                ESP_LOGE(TAG, "HTTP write failed at %ld/%ld", sent, file_size);
                break;
            }
            sent += got;
        }
        fclose(f);
        ESP_LOGI(TAG, "Uploaded %ld/%ld bytes", sent, file_size);

        int content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 200 && content_len > 0 && content_len < 8192) {
            char *resp = malloc(content_len + 1);
            if (resp) {
                esp_http_client_read(client, resp, content_len);
                resp[content_len] = '\0';

                /* Parse JSON response: {"text":"...", "duration_s":..., "stt_ms":...} */
                cJSON *root = cJSON_Parse(resp);
                if (root) {
                    const char *text = cJSON_GetStringValue(
                        cJSON_GetObjectItem(root, "text"));
                    if (text && text[0]) {
                        strncpy(n->text, text, MAX_NOTE_LEN - 1);
                        n->text[MAX_NOTE_LEN - 1] = '\0';
                        n->state = NOTE_STATE_TRANSCRIBED;
                        ESP_LOGI(TAG, "Transcription done [%d]: %.60s", slot, text);
                    } else {
                        snprintf(n->text, MAX_NOTE_LEN, "(Empty transcription)");
                        n->state = NOTE_STATE_FAILED;
                    }
                    cJSON_Delete(root);
                }
                free(resp);
            }
        } else {
            ESP_LOGW(TAG, "Transcribe HTTP %d (len=%d)", status, content_len);
            n->state = NOTE_STATE_FAILED;
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        notes_save();
        refresh_list();
    }
}

void ui_notes_start_transcription_queue(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        transcription_queue_task, "transcribe_q", 16384,  /* needs room for HTTP client */
        NULL, 3, NULL, 1);
    if (ret == pdPASS) {
        ESP_LOGI(TAG, "Transcription queue task created");
    } else {
        ESP_LOGE(TAG, "Failed to create transcription queue task");
    }
}

/* ── Voice session complete — save transcript ─────────────── */
static void voice_session_done(void)
{
    const char *text = NULL;
    if (voice_get_mode() == VOICE_MODE_DICTATE) {
        text = voice_get_dictation_text();
    } else {
        text = voice_get_stt_text();
    }

    /* Stop SD recording and attach transcript (if we have one) */
    ui_notes_stop_recording((text && text[0]) ? text : NULL);
    ESP_LOGI(TAG, "Voice session done: %s",
             (text && text[0]) ? "transcribed" : "recorded (needs transcription)");
}

/* ── Keyboard layout callback — move input area / edit TA above keyboard ── */
static void notes_keyboard_layout_cb(bool visible, int kb_height)
{
    if (visible) {
        int above_kb = USABLE_H - kb_height;

        /* Move text input area + save button above keyboard */
        if (s_input_area) {
            lv_obj_set_pos(s_input_area, 8, above_kb - INPUT_H - 8);
        }
        if (s_input_btn) {
            lv_obj_set_pos(s_input_btn, SW - 168, above_kb - INPUT_H - 8);
        }

        /* Shrink edit overlay textarea so bottom doesn't go under keyboard */
        if (s_edit_ta && s_edit_overlay) {
            int ta_y = lv_obj_get_y(s_edit_ta);
            int new_h = above_kb - ta_y - 12;
            if (new_h > 100) lv_obj_set_height(s_edit_ta, new_h);
        }
    } else {
        /* Restore original positions */
        if (s_input_area) {
            lv_obj_set_pos(s_input_area, 8, USABLE_H - INPUT_H - 8);
        }
        if (s_input_btn) {
            lv_obj_set_pos(s_input_btn, SW - 168, USABLE_H - INPUT_H - 8);
        }

        /* Restore edit textarea height */
        if (s_edit_ta && s_edit_overlay) {
            int ta_y = lv_obj_get_y(s_edit_ta);
            int ta_h = USABLE_H - ta_y - 24;
            lv_obj_set_height(s_edit_ta, ta_h);
        }
    }
}

/* ── Input area (inline text note creation) ───────────────── */
static void show_input_area(void)
{
    if (s_input_visible) return;
    s_input_visible = true;

    /* Position above the keyboard from the start since keyboard will open */
    int input_y = USABLE_H - UI_KB_HEIGHT - INPUT_H - 8;

    /* Textarea — big and readable */
    s_input_area = lv_textarea_create(s_screen);
    lv_obj_set_size(s_input_area, SW - 180 - 16, INPUT_H);
    lv_obj_set_pos(s_input_area, 8, input_y);
    lv_textarea_set_placeholder_text(s_input_area, "write it down...");
    lv_textarea_set_one_line(s_input_area, true);
    lv_obj_set_style_bg_color(s_input_area, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_text_color(s_input_area, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(s_input_area, FONT_HEADING, 0);
    lv_obj_set_style_radius(s_input_area, 16, 0);
    lv_obj_set_style_border_width(s_input_area, 0, 0);
    lv_obj_set_style_pad_hor(s_input_area, 20, 0);
    lv_obj_add_flag(s_input_area, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_input_area, cb_input_send, LV_EVENT_READY, NULL);

    /* Save button — huge touch target */
    s_input_btn = lv_button_create(s_screen);
    lv_obj_set_size(s_input_btn, 160, INPUT_H);
    lv_obj_set_pos(s_input_btn, SW - 168, input_y);
    lv_obj_set_style_bg_color(s_input_btn, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_opa(s_input_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_input_btn, 16, 0);
    lv_obj_set_style_border_width(s_input_btn, 0, 0);
    lv_obj_add_event_cb(s_input_btn, cb_input_send, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(s_input_btn);
    lv_label_set_text(btn_lbl, "Save");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(btn_lbl, FONT_HEADING, 0);
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
    /* L5: Support both click and swipe-right gesture */
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;  /* only swipe-right triggers back */
    }
    hide_input_area();
    /* Hide overlay — don't destroy (preserve note list for quick re-open) */
    if (s_screen) {
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* FreeRTOS task to switch to VOICE mode (connects to Dragon) */
static void dictation_connect_task(void *arg)
{
    tab5_mode_switch(MODE_VOICE);
    vTaskSuspend(NULL);  /* P4 TLSP crash workaround (#20) */
}

/* LVGL timer: poll for READY state after Dragon connect, then start dictation */
static void __attribute__((unused)) pending_dictation_poll_cb(lv_timer_t *t)
{
    int *ticks = (int *)lv_timer_get_user_data(t);
    (*ticks)++;

    if (voice_get_state() == VOICE_STATE_READY && s_pending_dictation) {
        s_pending_dictation = false;
        lv_timer_delete(t);
        free(ticks);
        s_voice_recording = true;
        voice_start_dictation();
        ESP_LOGI(TAG, "Dictation auto-started after Dragon connect");
        return;
    }
    /* Timeout after 15s */
    if (*ticks > 150) {
        s_pending_dictation = false;
        lv_timer_delete(t);
        free(ticks);
        ESP_LOGW(TAG, "Dictation connect timeout");
    }
}

/* Standalone SD-only recording task — reads mic, writes WAV, no Dragon needed */
static TaskHandle_t  s_sd_rec_task = NULL;

static void sd_record_task(void *arg)
{
    ESP_LOGI(TAG, "SD recording task started (core %d)", xPortGetCoreID());

    /* Allocate buffers in PSRAM */
    const int tdm_samples = 960 * 4;  /* 20ms @ 48kHz, 4 TDM channels */
    const int mono_samples = 320;      /* 20ms @ 16kHz */
    int16_t *tdm_buf = heap_caps_malloc(tdm_samples * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono_buf = heap_caps_malloc(mono_samples * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tdm_buf || !mono_buf) {
        ESP_LOGE(TAG, "SD rec: buffer alloc failed");
        heap_caps_free(tdm_buf);
        heap_caps_free(mono_buf);
        s_sd_rec_running = false;
        s_sd_rec_task = NULL;
        vTaskSuspend(NULL);
        return;
    }

    int frames = 0;
    while (s_sd_rec_running) {
        esp_err_t err = tab5_mic_read(tdm_buf, tdm_samples, 100);
        if (err != ESP_OK) {
            if (frames == 0) {
                ESP_LOGE(TAG, "SD rec: mic_read failed: %s", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* Downsample 48kHz TDM slot 0 → 16kHz mono */
        int out_idx = 0;
        for (int i = 0; i + 2 < 960 && out_idx < mono_samples; i += 3) {
            int32_t sum = tdm_buf[i * 4] + tdm_buf[(i+1) * 4] + tdm_buf[(i+2) * 4];
            mono_buf[out_idx++] = (int16_t)(sum / 3);
        }

        ui_notes_write_audio(mono_buf, out_idx);
        frames++;
        if (frames == 1) {
            ESP_LOGI(TAG, "SD rec: first audio chunk written (%d samples)", out_idx);
        }
        if (frames % 250 == 0) {  /* every 5 seconds */
            ESP_LOGI(TAG, "SD rec: %d frames (%.1fs)", frames, frames * 0.02f);
        }
    }

    heap_caps_free(tdm_buf);
    heap_caps_free(mono_buf);
    ESP_LOGI(TAG, "SD recording task exiting");
    s_sd_rec_task = NULL;
    vTaskSuspend(NULL);
}

/* Safe toast deletion — timer user_data is the toast lv_obj_t* */
static void toast_delete_cb(lv_timer_t *t)
{
    lv_obj_t *obj = lv_timer_get_user_data(t);
    if (obj && lv_obj_is_valid(obj)) {
        lv_obj_delete(obj);
    }
}

static void cb_new_voice(lv_event_t *e)
{
    (void)e;
    if (s_sd_rec_running || s_voice_recording) {
        /* Stop current recording */
        hide_recording_indicator();
        s_sd_rec_running = false;  /* signal task to exit */
        if (voice_get_state() == VOICE_STATE_LISTENING) {
            voice_stop_listening();
        }
        s_voice_recording = false;
        /* Give mic task time to exit before finalizing WAV.
         * Use lv_async_call to run on next LVGL cycle. */
        lv_async_call((lv_async_cb_t)ui_notes_stop_recording, NULL);
        ESP_LOGI(TAG, "Recording stopping...");
        return;
    }

    voice_state_t st = voice_get_state();

    /* Always start SD recording first */
    const char *wav = ui_notes_start_recording();
    if (!wav) {
        ESP_LOGE(TAG, "Failed to start recording — SD card not mounted?");
        /* Show toast on the layer_top so it's visible from any screen */
        lv_obj_t *toast = lv_obj_create(lv_layer_top());
        lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(toast, lv_color_hex(0xFF453A), 0);
        lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
        lv_obj_set_style_radius(toast, 16, 0);
        lv_obj_set_style_pad_all(toast, 20, 0);
        lv_obj_set_style_border_width(toast, 0, 0);
        lv_obj_t *lbl = lv_label_create(toast);
        lv_label_set_text(lbl, "SD card not ready");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xE8E8EF), 0);
        lv_obj_set_style_text_font(lbl, FONT_HEADING, 0);
        lv_timer_t *tmr = lv_timer_create(toast_delete_cb, 2000, toast);
        lv_timer_set_repeat_count(tmr, 1);
        return;
    }

    s_voice_recording = true;
    show_recording_indicator();

    if (st == VOICE_STATE_READY) {
        /* Dragon online — dual-write: SD + live dictation via voice module */
        esp_err_t err = voice_start_dictation();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Dictation start failed, continuing SD-only");
        } else {
            ESP_LOGI(TAG, "Dictation started (SD + Dragon): %s", wav);
        }
    } else {
        /* Offline or busy — SD-only recording with standalone mic task */
        s_sd_rec_running = true;
        xTaskCreatePinnedToCore(
            sd_record_task, "sd_rec", 4096, NULL, 5, &s_sd_rec_task, 1);
        ESP_LOGI(TAG, "Recording to SD only: %s", wav);

        /* Try to connect Dragon in background for later transcription */
        if (st == VOICE_STATE_IDLE) {
            xTaskCreatePinnedToCore(
                dictation_connect_task, "dict_conn", 8192, NULL, 3, NULL, 1);
        }
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

/* M2: Search callback — live filter on keystroke */
static void cb_search_changed(lv_event_t *e)
{
    (void)e;
    if (!s_search_ta) return;
    const char *txt = lv_textarea_get_text(s_search_ta);
    strncpy(s_search_text, txt ? txt : "", sizeof(s_search_text) - 1);
    s_search_text[sizeof(s_search_text) - 1] = '\0';
    refresh_list();
}

/* Bug fix: proper click callback for search bar keyboard popup */
static void cb_search_tap(lv_event_t *e)
{
    (void)e;
    if (s_search_ta) ui_keyboard_show(s_search_ta);
}

/* Bug fix: proper click callback for edit overlay keyboard popup */
static void cb_edit_ta_tap(lv_event_t *e)
{
    (void)e;
    if (s_edit_ta) ui_keyboard_show(s_edit_ta);
}

/* M1: Edit overlay — save callback (uses static s_edit_* variables) */
static void cb_edit_save(lv_event_t *e)
{
    (void)e;
    if (!s_edit_ta || s_edit_idx < 0 || s_edit_idx >= MAX_NOTES
        || !s_notes[s_edit_idx].used) {
        goto close;
    }
    {
        const char *txt = lv_textarea_get_text(s_edit_ta);
        if (txt && txt[0]) {
            strncpy(s_notes[s_edit_idx].text, txt, MAX_NOTE_LEN - 1);
            s_notes[s_edit_idx].text[MAX_NOTE_LEN - 1] = '\0';
            notes_save();
            sync_note_to_dragon("", txt);
            ESP_LOGI(TAG, "Note %d edited: %.40s", s_edit_idx, txt);
        }
    }
close:
    ui_keyboard_hide();
    if (s_edit_overlay) { lv_obj_del(s_edit_overlay); s_edit_overlay = NULL; }
    s_edit_ta = NULL;
    s_edit_idx = -1;
    refresh_list();
}

static void cb_edit_close(lv_event_t *e)
{
    (void)e;
    ui_keyboard_hide();
    if (s_edit_overlay) { lv_obj_del(s_edit_overlay); s_edit_overlay = NULL; }
    s_edit_ta = NULL;
    s_edit_idx = -1;
}

static void cb_note_tap(lv_event_t *e)
{
    int note_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (note_idx < 0 || note_idx >= MAX_NOTES || !s_notes[note_idx].used) return;
    if (!s_screen) return;

    /* Close any existing edit overlay first */
    if (s_edit_overlay) { lv_obj_del(s_edit_overlay); s_edit_overlay = NULL; }

    note_entry_t *n = &s_notes[note_idx];
    s_edit_idx = note_idx;

    /* Fullscreen overlay — child of s_screen so it covers the notes list */
    s_edit_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_edit_overlay);
    lv_obj_set_size(s_edit_overlay, SW, OVERLAY_H);
    lv_obj_set_pos(s_edit_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_edit_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_edit_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_edit_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_edit_overlay);

    /* ── Topbar: Cancel (left) | "Edit Note" (center) | Save (right) ── */
    lv_obj_t *tb = lv_obj_create(s_edit_overlay);
    lv_obj_set_size(tb, SW, TOPBAR_H + 16);
    lv_obj_set_pos(tb, 0, 0);
    lv_obj_set_style_bg_color(tb, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    /* Cancel button — left */
    lv_obj_t *cancel_btn = lv_button_create(tb);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_size(cancel_btn, 120, TOPBAR_H);
    lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, cb_edit_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(cancel_lbl, FONT_BODY, 0);
    lv_obj_center(cancel_lbl);

    /* Title — center */
    lv_obj_t *title = lv_label_create(tb);
    lv_label_set_text(title, "Edit Note");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(title, FONT_HEADING, 0);
    lv_obj_center(title);

    /* Save button — right */
    lv_obj_t *save_btn = lv_button_create(tb);
    lv_obj_remove_style_all(save_btn);
    lv_obj_set_size(save_btn, 100, TOPBAR_H);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(COL_MINT), 0);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_add_event_cb(save_btn, cb_edit_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0x08080E), 0);
    lv_obj_set_style_text_font(save_lbl, FONT_BODY, 0);
    lv_obj_center(save_lbl);

    /* ── Timestamp hint ── */
    char ts_buf[64];
    static const char *mn[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    int mi = (n->month >= 1 && n->month <= 12) ? n->month - 1 : 0;
    snprintf(ts_buf, sizeof(ts_buf), "%s %d, %02d:%02d — %s note",
             mn[mi], n->day, n->hour, n->minute,
             n->is_voice ? "Voice" : "Text");
    lv_obj_t *ts = lv_label_create(s_edit_overlay);
    lv_label_set_text(ts, ts_buf);
    lv_obj_set_style_text_color(ts, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(ts, FONT_CAPTION, 0);
    lv_obj_set_pos(ts, 24, TOPBAR_H + 24);

    /* ── Large textarea — full width, most of the screen ── */
    int ta_y = TOPBAR_H + 56;
    int ta_h = USABLE_H - ta_y - 24;
    s_edit_ta = lv_textarea_create(s_edit_overlay);
    lv_obj_set_size(s_edit_ta, SW - 32, ta_h);
    lv_obj_set_pos(s_edit_ta, 16, ta_y);
    lv_textarea_set_text(s_edit_ta, n->text);
    lv_obj_set_style_bg_color(s_edit_ta, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_text_color(s_edit_ta, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(s_edit_ta, FONT_BODY, 0);
    lv_obj_set_style_border_width(s_edit_ta, 1, 0);
    lv_obj_set_style_border_color(s_edit_ta, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_radius(s_edit_ta, 8, 0);
    lv_obj_set_style_pad_all(s_edit_ta, 16, 0);
    lv_obj_add_flag(s_edit_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_edit_ta, cb_edit_ta_tap, LV_EVENT_CLICKED, NULL);
    ui_keyboard_show(s_edit_ta);  /* Bug fix: show keyboard immediately when edit overlay opens */
}

/* Delete confirmation callbacks */
static void cb_confirm_delete_yes(lv_event_t *e)
{
    lv_obj_t *dialog = (lv_obj_t *)lv_event_get_user_data(e);
    int note_idx = (int)(intptr_t)lv_obj_get_user_data(dialog);
    lv_obj_del(dialog);
    ui_notes_delete(note_idx);
}

static void cb_confirm_delete_no(lv_event_t *e)
{
    lv_obj_t *dialog = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_del(dialog);
}

static void cb_note_delete(lv_event_t *e)
{
    int note_idx = (int)(intptr_t)lv_event_get_user_data(e);

    /* Show confirmation dialog */
    lv_obj_t *dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(dialog, 480, 200);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dialog, 24, 0);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_opa(dialog, LV_OPA_40, 0);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(dialog, (void *)(intptr_t)note_idx);

    lv_obj_t *lbl = lv_label_create(dialog);
    lv_label_set_text(lbl, "Delete this note?");
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl, FONT_TITLE, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 20);

    /* Yes button */
    lv_obj_t *yes = lv_button_create(dialog);
    lv_obj_set_size(yes, 160, 60);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_LEFT, 20, -16);
    lv_obj_set_style_bg_color(yes, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_radius(yes, 16, 0);
    lv_obj_add_event_cb(yes, cb_confirm_delete_yes, LV_EVENT_CLICKED, dialog);
    lv_obj_t *yes_lbl = lv_label_create(yes);
    lv_label_set_text(yes_lbl, "Delete");
    lv_obj_set_style_text_color(yes_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(yes_lbl, FONT_HEADING, 0);
    lv_obj_center(yes_lbl);

    /* No button */
    lv_obj_t *no = lv_button_create(dialog);
    lv_obj_set_size(no, 160, 60);
    lv_obj_align(no, LV_ALIGN_BOTTOM_RIGHT, -20, -16);
    lv_obj_set_style_bg_color(no, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_radius(no, 16, 0);
    lv_obj_add_event_cb(no, cb_confirm_delete_no, LV_EVENT_CLICKED, dialog);
    lv_obj_t *no_lbl = lv_label_create(no);
    lv_label_set_text(no_lbl, "Cancel");
    lv_obj_set_style_text_color(no_lbl, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(no_lbl, FONT_HEADING, 0);
    lv_obj_center(no_lbl);
}

/* ── WAV playback ──────────────────────────────────────── */
static volatile bool s_wav_playing = false;

static void wav_play_task(void *arg)
{
    char *path = (char *)arg;
    ESP_LOGI(TAG, "Playing WAV: %s", path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        goto done;
    }

    /* Read WAV header to get actual sample rate */
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) < 44) {
        ESP_LOGE(TAG, "WAV header too short");
        fclose(f);
        goto done;
    }
    uint32_t wav_rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    uint16_t wav_channels = hdr[22] | (hdr[23] << 8);
    uint16_t wav_bits = hdr[34] | (hdr[35] << 8);
    ESP_LOGI(TAG, "WAV: %luHz %uch %ubit", (unsigned long)wav_rate, wav_channels, wav_bits);

    if (wav_bits != 16 || wav_channels != 1) {
        ESP_LOGE(TAG, "Only 16-bit mono WAV supported");
        fclose(f);
        goto done;
    }

    /* H4: Proper resampling to 48kHz using linear interpolation.
     * Previous integer division (48000/22050=2) caused pitch shift for
     * non-integer ratios. Now uses fixed-point fractional stepping. */
    const uint32_t target_rate = 48000;
    ESP_LOGI(TAG, "Resampling: %luHz → %luHz", (unsigned long)wav_rate, (unsigned long)target_rate);

    tab5_audio_speaker_enable(true);

    int16_t buf_in[512];
    int16_t buf_out[512 * 4];  /* max expansion: 48000/8000 = 6x, but cap at 4x read size */
    size_t rd;
    while (s_wav_playing && (rd = fread(buf_in, sizeof(int16_t), 512, f)) > 0) {
        /* Calculate exact output samples for this chunk */
        size_t out_samples = (size_t)((uint64_t)rd * target_rate / wav_rate);
        if (out_samples > sizeof(buf_out) / sizeof(buf_out[0])) {
            out_samples = sizeof(buf_out) / sizeof(buf_out[0]);
        }
        /* Linear interpolation resampling */
        for (size_t o = 0; o < out_samples; o++) {
            /* Fixed-point source position: where in buf_in does this output sample come from? */
            uint64_t src_pos_fixed = (uint64_t)o * wav_rate;  /* numerator */
            size_t src_idx = (size_t)(src_pos_fixed / target_rate);
            uint32_t frac = (uint32_t)((src_pos_fixed % target_rate) * 256 / target_rate);

            int16_t s0 = (src_idx < rd) ? buf_in[src_idx] : 0;
            int16_t s1 = (src_idx + 1 < rd) ? buf_in[src_idx + 1] : s0;
            buf_out[o] = (int16_t)(s0 + ((int32_t)(s1 - s0) * (int32_t)frac / 256));
        }
        tab5_audio_play_raw(buf_out, out_samples);
    }

    tab5_audio_speaker_enable(false);
    fclose(f);

done:
    free(path);
    s_wav_playing = false;
    ESP_LOGI(TAG, "WAV playback done");
    vTaskSuspend(NULL);  /* P4 TLSP workaround (#20) */
}

static void cb_note_play(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MAX_NOTES || !s_notes[idx].used) return;
    if (!s_notes[idx].audio_path[0]) return;

    if (s_wav_playing) {
        s_wav_playing = false;  /* Stop current playback */
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    char *path = strdup(s_notes[idx].audio_path);
    if (!path) return;

    s_wav_playing = true;
    xTaskCreate(wav_play_task, "wav_play", 8192, path, 5, NULL);
}

/* ── Note card widget ──────────────────────────────────── */
static void add_note_card(lv_obj_t *parent, const note_entry_t *note, int note_idx)
{
    note_entry_t n = *note;

    /* Card — compact, flex column layout. Tap for full view. */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, 160, 0);  /* FIX N1: cap card height */
    /* v5: flat row, hairline rule underneath — no rounded bubble. */
    lv_obj_set_style_bg_color(card, lv_color_hex(0x08080E), 0); /* TH_BG */
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1C1C28), 0); /* TH_HAIRLINE */
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);  /* Stack header + preview vertically */
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);  /* Bug fix: prevent gesture bubbling to s_screen cb_back */
    lv_obj_set_ext_click_area(card, 10);
    lv_obj_add_event_cb(card, cb_note_tap, LV_EVENT_CLICKED,
                       (void *)(intptr_t)note_idx);

    /* Row 1: timestamp + badge + action buttons (all in one line) */
    lv_obj_t *header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 44);  /* 44px matches touch target height */
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* Timestamp — 14pt for compact display */
    lv_obj_t *ts = lv_label_create(header);
    char ts_buf[32];
    static const char *mn[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    int mi = (n.month >= 1 && n.month <= 12) ? n.month - 1 : 0;
    snprintf(ts_buf, sizeof(ts_buf), "%s %d, %02d:%02d",
             mn[mi], n.day, n.hour, n.minute);
    lv_label_set_text(ts, ts_buf);
    lv_obj_set_style_text_color(ts, lv_color_hex(COL_LABEL2), 0);
    lv_obj_set_style_text_font(ts, FONT_CAPTION, 0);
    lv_obj_align(ts, LV_ALIGN_LEFT_MID, 0, 0);

    /* v5: kill the colored Material pill. Use a letter-spaced caption
     * in amber (or red on failure) inline next to the timestamp. */
    lv_obj_t *badge = lv_label_create(header);
    const char *badge_text;
    uint32_t badge_color;
    switch (n.state) {
    case NOTE_STATE_RECORDED:     badge_text = "\xe2\x80\xa2 REC";    badge_color = COL_AMBER; break;
    case NOTE_STATE_TRANSCRIBING: badge_text = "\xe2\x80\xa2 . . .";  badge_color = COL_AMBER; break;
    case NOTE_STATE_TRANSCRIBED:  badge_text = "\xe2\x80\xa2 VOICE";  badge_color = COL_AMBER; break;
    case NOTE_STATE_FAILED:       badge_text = "\xe2\x80\xa2 FAIL";   badge_color = COL_RED;   break;
    default:                      badge_text = n.is_voice ? "\xe2\x80\xa2 VOICE" : "\xe2\x80\xa2 TEXT";
                                  badge_color = COL_AMBER; break;
    }
    lv_label_set_text(badge, badge_text);
    lv_obj_set_style_text_color(badge, lv_color_hex(badge_color), 0);
    lv_obj_set_style_text_font(badge, FONT_CAPTION, 0);
    lv_obj_set_style_text_letter_space(badge, 3, 0);
    lv_obj_align_to(badge, ts, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    /* Delete button — 44x44 touch target, dark bg, red X */
    lv_obj_t *del = lv_button_create(header);
    lv_obj_set_size(del, 44, 44);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(del, 8, 0);
    lv_obj_set_style_border_width(del, 0, 0);
    lv_obj_add_event_cb(del, cb_note_delete, LV_EVENT_CLICKED,
                       (void *)(intptr_t)note_idx);
    lv_obj_t *del_lbl = lv_label_create(del);
    lv_label_set_text(del_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(del_lbl, FONT_CAPTION, 0);
    lv_obj_center(del_lbl);

    /* Play button — 44x44 touch target, green, next to delete, only for notes with audio */
    if (n.audio_path[0] && n.state != NOTE_STATE_FAILED) {
        lv_obj_t *play = lv_button_create(header);
        lv_obj_set_size(play, 44, 44);
        lv_obj_align(play, LV_ALIGN_RIGHT_MID, -48, 0);
        lv_obj_set_style_bg_color(play, lv_color_hex(COL_MINT), 0);
        lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(play, 8, 0);
        lv_obj_set_style_border_width(play, 0, 0);
        lv_obj_add_event_cb(play, cb_note_play, LV_EVENT_CLICKED,
                           (void *)(intptr_t)note_idx);
        lv_obj_t *play_lbl = lv_label_create(play);
        lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(play_lbl, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(play_lbl, FONT_CAPTION, 0);
        lv_obj_center(play_lbl);
    }

    /* FIX N1+N4: Note preview — truncated to ~100 chars, smaller font */
    lv_obj_t *preview = lv_label_create(card);
    /* Truncate long text for card preview — full text in edit overlay */
    char preview_text[120];
    const char *src = n.text;
    /* Skip "[Untitled Note] " prefix (N7) */
    if (strncmp(src, "[Untitled Note] ", 16) == 0) src += 16;
    if (strlen(src) > 100) {
        snprintf(preview_text, sizeof(preview_text), "%.100s...", src);
    } else {
        strncpy(preview_text, src, sizeof(preview_text) - 1);
        preview_text[sizeof(preview_text) - 1] = '\0';
    }
    lv_label_set_text(preview, preview_text);
    lv_obj_set_style_text_color(preview, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(preview, FONT_BODY, 0);
    lv_obj_set_width(preview, lv_pct(100));
}

/* ── Refresh list ───────────────────────────────────────── */
static void refresh_list(void)
{
    /* v5 topbar meta — update count regardless of list presence. */
    if (s_topbar_meta) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%d \xe2\x80\xa2 %s",
                 s_note_count, s_note_count == 1 ? "NOTE" : "NOTES");
        lv_label_set_text(s_topbar_meta, buf);
    }
    if (!s_list) return;
    lv_obj_clean(s_list);

    if (s_note_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "No notes yet.\n\nTap Voice to record\na note, or Text to type one.");
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_LABEL2), 0);
        lv_obj_set_style_text_font(empty, FONT_HEADING, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    int shown = 0;
    for (int i = 0; i < MAX_NOTES && shown < s_note_count; i++) {
        int idx = (s_next_slot - 1 - i + MAX_NOTES) % MAX_NOTES;
        if (!s_notes[idx].used) continue;
        /* M2: Search filter — skip notes that don't match search text */
        if (s_search_text[0]) {
            /* N1: Case-insensitive search */
            if (!strcasestr(s_notes[idx].text, s_search_text)) continue;
        }
        add_note_card(s_list, &s_notes[idx], idx);
        shown++;
    }
    if (shown == 0 && s_search_text[0]) {
        lv_obj_t *nf = lv_label_create(s_list);
        lv_label_set_text(nf, "No matching notes");
        lv_obj_set_style_text_color(nf, lv_color_hex(COL_LABEL2), 0);
        lv_obj_set_style_text_font(nf, FONT_TITLE, 0);
        lv_obj_set_style_text_align(nf, LV_TEXT_ALIGN_CENTER, 0);
    }
}

/* ── Top bar (v5: typography-forward, amber title, 'HOME' caption back) ─ */
static lv_obj_t *make_topbar(lv_obj_t *parent)
{
    lv_obj_t *tb = lv_obj_create(parent);
    lv_obj_set_size(tb, SW, TOPBAR_H);
    lv_obj_set_pos(tb, 0, 0);
    lv_obj_set_style_bg_color(tb, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(0x1A1A24), 0); /* TH_HAIRLINE */
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    /* v5 spec: big amber 'Notes' title LEFT; small mono count meta RIGHT.
     * Swipe-right replaces the old HOME caption for back navigation. */
    lv_obj_t *title = lv_label_create(tb);
    lv_label_set_text(title, "Notes");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF59E0B), 0); /* TH_AMBER */
    lv_obj_set_style_text_font(title, FONT_TITLE, 0);              /* 28 px */
    lv_obj_set_style_text_letter_space(title, -1, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    /* Count/size meta — refreshed elsewhere when the note list changes. */
    lv_obj_t *meta = lv_label_create(tb);
    lv_label_set_text(meta, "\xe2\x80\xa2");  /* placeholder until first refresh */
    lv_obj_set_style_text_color(meta, lv_color_hex(0x6A6A72), 0);
    lv_obj_set_style_text_font(meta, FONT_CAPTION, 0);
    lv_obj_set_style_text_letter_space(meta, 3, 0);
    lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -24, 0);
    s_topbar_meta = meta;

    return tb;
}

/* ── Screen create/destroy ─────────────────────────────── */
lv_obj_t *ui_notes_create(void)
{
    if (s_screen) {
        /* Already exists — just unhide */
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(s_screen);
        refresh_list();
        ui_keyboard_set_layout_cb(notes_keyboard_layout_cb);
        ESP_LOGI(TAG, "Notes screen resumed");
        return s_screen;
    }

    /* Fullscreen overlay on home screen (NOT a separate lv_screen) */
    extern lv_obj_t *ui_home_get_screen(void);
    s_screen = lv_obj_create(ui_home_get_screen());
    if (!s_screen) {
        ESP_LOGE(TAG, "OOM: failed to create notes screen");
        return NULL;
    }
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, SW, OVERLAY_H);
    lv_obj_set_pos(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_screen);

    /* L5: Swipe-right to go back */
    lv_obj_add_event_cb(s_screen, cb_back, LV_EVENT_GESTURE, NULL);

    make_topbar(s_screen);

    /* ── Voice + Text buttons (compact row) ─────────────── */
    lv_obj_t *btn_row = lv_obj_create(s_screen);
    lv_obj_set_size(btn_row, SW, BTN_ROW_H);
    lv_obj_set_pos(btn_row, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Voice button — compact 56px tall */
    lv_obj_t *vbtn = lv_button_create(btn_row);
    lv_obj_set_size(vbtn, SW/2 - 24, ACTION_BTN_H);
    lv_obj_align(vbtn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(vbtn, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_bg_opa(vbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vbtn, 12, 0);
    lv_obj_set_style_border_width(vbtn, 0, 0);
    lv_obj_add_event_cb(vbtn, cb_new_voice, LV_EVENT_CLICKED, NULL);

    lv_obj_t *vicon = lv_label_create(vbtn);
    lv_label_set_text(vicon, LV_SYMBOL_AUDIO "  Voice");
    lv_obj_set_style_text_color(vicon, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_font(vicon, FONT_HEADING, 0);
    lv_obj_set_style_text_align(vicon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(vicon);

    /* Text button — compact 56px tall */
    lv_obj_t *tbtn = lv_button_create(btn_row);
    lv_obj_set_size(tbtn, SW/2 - 24, ACTION_BTN_H);
    lv_obj_align(tbtn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(tbtn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(tbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tbtn, 12, 0);
    lv_obj_set_style_border_width(tbtn, 1, 0);
    lv_obj_set_style_border_color(tbtn, lv_color_hex(COL_BORDER), 0);
    lv_obj_add_event_cb(tbtn, cb_new_text, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ticon = lv_label_create(tbtn);
    lv_label_set_text(ticon, LV_SYMBOL_EDIT "  Type");
    lv_obj_set_style_text_color(ticon, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(ticon, FONT_HEADING, 0);
    lv_obj_set_style_text_align(ticon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ticon);

    /* M2: Search bar — 48px tall for comfortable touch */
    #define SEARCH_H 48
    s_search_ta = lv_textarea_create(s_screen);
    lv_obj_set_size(s_search_ta, SW - 32, SEARCH_H);
    lv_obj_set_pos(s_search_ta, 16, TOPBAR_H + BTN_ROW_H + 4);
    lv_textarea_set_one_line(s_search_ta, true);
    lv_textarea_set_placeholder_text(s_search_ta, LV_SYMBOL_LOOP " search what you wrote...");
    lv_obj_set_style_bg_color(s_search_ta, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_text_color(s_search_ta, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(s_search_ta, FONT_SECONDARY, 0);
    lv_obj_set_style_border_width(s_search_ta, 1, 0);
    lv_obj_set_style_border_color(s_search_ta, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_radius(s_search_ta, 8, 0);
    lv_obj_set_style_pad_left(s_search_ta, 16, 0);
    lv_obj_set_style_text_color(s_search_ta, lv_color_hex(COL_LABEL3),
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_flag(s_search_ta, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (s_search_text[0]) lv_textarea_set_text(s_search_ta, s_search_text);
    lv_obj_add_event_cb(s_search_ta, cb_search_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_search_ta, cb_search_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Divider */
    lv_obj_t *div = lv_obj_create(s_screen);
    lv_obj_set_size(div, SW, 2);
    lv_obj_set_pos(div, 0, TOPBAR_H + BTN_ROW_H + SEARCH_H + 8);
    lv_obj_set_style_bg_color(div, lv_color_hex(COL_CARD), 0);

    /* Scrollable notes list — gains ~132px from reduced button row + search bar */
    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, SW, OVERLAY_H - TOPBAR_H - BTN_ROW_H - SEARCH_H - 10);
    lv_obj_set_pos(s_list, 0, TOPBAR_H + BTN_ROW_H + SEARCH_H + 10);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 12, 0);
    lv_obj_set_style_pad_hor(s_list, 16, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ON);

    refresh_list();
    ui_keyboard_set_layout_cb(notes_keyboard_layout_cb);

    ESP_LOGI(TAG, "Notes screen created, %d notes", s_note_count);
    return s_screen;
}

void ui_notes_destroy(void)
{
    ui_keyboard_set_layout_cb(NULL);
    hide_recording_indicator();
    hide_input_area();
    /* Edit overlay is a child of s_screen, so lv_obj_del(s_screen) destroys it.
     * Just clear the pointers. */
    s_edit_overlay = NULL;
    s_edit_ta = NULL;
    s_edit_idx = -1;
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

void ui_notes_hide(void)
{
    ui_keyboard_set_layout_cb(NULL);
    hide_input_area();
    /* Clear recording state — prevents transcription queue blockage if user
     * navigates away mid-recording. Without this, s_voice_recording stays
     * true forever since ui_notes_destroy() is never called. */
    s_voice_recording = false;
    if (s_screen) {
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    }
}

lv_obj_t *ui_notes_get_screen(void) { return s_screen; }
