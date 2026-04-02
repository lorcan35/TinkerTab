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
static bool s_input_visible    = false;
static bool s_voice_recording  = false;
static bool s_pending_dictation = false;  /* waiting for READY to start dictation */
static volatile bool s_sd_rec_running = false;  /* standalone SD recording active */

/* ── Forward decls ─────────────────────────────────────── */
static void cb_back(lv_event_t *e);
static void cb_new_voice(lv_event_t *e);
static void cb_new_text(lv_event_t *e);
static void cb_input_send(lv_event_t *e);
static void cb_note_tap(lv_event_t *e);
static void cb_note_delete(lv_event_t *e);
static void cb_note_play(lv_event_t *e);
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
            bool needs_work = (s_notes[i].state == NOTE_STATE_RECORDED) ||
                              (s_notes[i].state == NOTE_STATE_FAILED && s_notes[i].audio_path[0]);
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

static void cb_new_voice(lv_event_t *e)
{
    (void)e;
    if (s_sd_rec_running || s_voice_recording) {
        /* Stop current recording */
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
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_timer_t *tmr = lv_timer_create((lv_timer_cb_t)lv_obj_delete, 2000, toast);
        lv_timer_set_repeat_count(tmr, 1);
        return;
    }

    s_voice_recording = true;

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
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
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
    lv_obj_set_style_text_font(yes_lbl, &lv_font_montserrat_24, 0);
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
    lv_obj_set_style_text_font(no_lbl, &lv_font_montserrat_24, 0);
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

    /* Skip WAV header (44 bytes) */
    fseek(f, 44, SEEK_SET);

    tab5_audio_speaker_enable(true);

    /* Read and play in 2048-sample chunks (16-bit mono @ 16kHz → 128ms each)
     * Tab5 I2S runs at 48kHz, so upsample 3:1 inline */
    int16_t buf_16k[2048];
    int16_t buf_48k[2048 * 3];
    size_t rd;
    while (s_wav_playing && (rd = fread(buf_16k, sizeof(int16_t), 2048, f)) > 0) {
        /* Upsample 16kHz → 48kHz (simple sample-hold) */
        for (size_t i = 0; i < rd; i++) {
            buf_48k[i * 3]     = buf_16k[i];
            buf_48k[i * 3 + 1] = buf_16k[i];
            buf_48k[i * 3 + 2] = buf_16k[i];
        }
        tab5_audio_play_raw(buf_48k, rd * 3);
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

    /* Play button — only for notes with audio */
    if (n.audio_path[0] && n.state != NOTE_STATE_FAILED) {
        lv_obj_t *play = lv_button_create(card);
        lv_obj_set_size(play, 80, 56);
        lv_obj_align(play, LV_ALIGN_TOP_RIGHT, -90, 0);
        lv_obj_set_style_bg_color(play, lv_color_hex(COL_CYAN), 0);
        lv_obj_set_style_bg_opa(play, LV_OPA_80, 0);
        lv_obj_set_style_radius(play, 16, 0);
        lv_obj_set_style_border_width(play, 0, 0);
        lv_obj_add_event_cb(play, cb_note_play, LV_EVENT_CLICKED,
                           (void *)(intptr_t)note_idx);

        lv_obj_t *play_lbl = lv_label_create(play);
        lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(play_lbl, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(play_lbl, &lv_font_montserrat_28, 0);
        lv_obj_center(play_lbl);
    }

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
