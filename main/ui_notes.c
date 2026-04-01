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
#include "config.h"
#include "mode_manager.h"
#include "rtc.h"
#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

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
        n->state = cJSON_IsNumber(jstate) ? (note_state_t)(int)jstate->valuedouble
                                          : NOTE_STATE_TEXT;
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

    /* Load recording counter from NVS (survives SD card removal) */
    nvs_handle_t nh;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nh) == ESP_OK) {
        uint32_t rid = 0;
        if (nvs_get_u32(nh, NVS_KEY_RECID, &rid) == ESP_OK && rid > s_next_rec_id) {
            s_next_rec_id = rid;
        }
        nvs_close(nh);
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
static bool s_input_visible    = false;
static bool s_voice_recording  = false;
static bool s_pending_dictation = false;  /* waiting for READY to start dictation */

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
        voice_session_done();
    }
    /* Ask mode ends with IDLE */
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

int ui_notes_unprocessed_count(void)
{
    notes_load();
    int count = 0;
    for (int i = 0; i < MAX_NOTES; i++) {
        if (s_notes[i].used && s_notes[i].state == NOTE_STATE_RECORDED) count++;
    }
    return count;
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
    n->year = (rtc.year > 2000) ? rtc.year - 2000 : 0;
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
    refresh_list();
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

/* FreeRTOS task to switch to VOICE mode (connects to Dragon) */
static void dictation_connect_task(void *arg)
{
    tab5_mode_switch(MODE_VOICE);
    vTaskSuspend(NULL);  /* P4 TLSP crash workaround (#20) */
}

/* LVGL timer: poll for READY state after Dragon connect, then start dictation */
static void pending_dictation_poll_cb(lv_timer_t *t)
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

static void cb_new_voice(lv_event_t *e)
{
    (void)e;
    voice_state_t st = voice_get_state();

    /* Always start SD recording first — this never needs Dragon */
    const char *wav = ui_notes_start_recording();
    if (!wav) {
        ESP_LOGE(TAG, "Failed to start recording (SD card issue?)");
        return;
    }

    s_voice_recording = true;

    if (st == VOICE_STATE_READY) {
        /* Dragon online — dual-write: SD + live dictation */
        voice_start_dictation();
        ESP_LOGI(TAG, "Dictation started (SD + Dragon): %s", wav);
    } else if (st == VOICE_STATE_IDLE) {
        /* Dragon offline — SD-only recording, connect in background for later transcription */
        s_pending_dictation = true;
        int *ticks = calloc(1, sizeof(int));
        lv_timer_create(pending_dictation_poll_cb, 100, ticks);
        ESP_LOGI(TAG, "Recording to SD (connecting to Dragon...): %s", wav);
        xTaskCreatePinnedToCore(
            dictation_connect_task, "dict_conn", 8192, NULL, 5, NULL, 1);
    } else {
        /* Voice busy but still record to SD */
        ESP_LOGI(TAG, "Recording to SD only (voice busy state %d): %s", st, wav);
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

    /* State badge — shows recording/transcription state */
    lv_obj_t *badge = lv_label_create(card);
    const char *badge_text;
    uint32_t badge_color;
    switch (n.state) {
    case NOTE_STATE_RECORDED:     badge_text = "  recorded  "; badge_color = 0xFF9F0A; break;
    case NOTE_STATE_TRANSCRIBING: badge_text = "  processing  "; badge_color = COL_CYAN; break;
    case NOTE_STATE_TRANSCRIBED:  badge_text = "  voice  "; badge_color = COL_CYAN; break;
    case NOTE_STATE_FAILED:       badge_text = "  failed  "; badge_color = COL_RED; break;
    default:                      badge_text = n.is_voice ? "  voice  " : "  text  ";
                                  badge_color = n.is_voice ? COL_CYAN : COL_AMBER; break;
    }
    lv_label_set_text(badge, badge_text);
    lv_obj_set_style_bg_color(badge, lv_color_hex(badge_color), 0);
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
