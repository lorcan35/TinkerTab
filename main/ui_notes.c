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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasestr */
#include <sys/stat.h>
#include <unistd.h> /* wave 13 H8: fsync() */

#include "audio.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mode_manager.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "settings.h"
#include "tab5_rtc.h"
#include "ui_core.h"
#include "ui_home.h"
#include "ui_keyboard.h"
#include "ui_voice.h"
#include "voice.h"
#include "voice_dictation.h"
#include "voice_dictation_lvgl.h" /* PR 3: pipeline subscriber for processing row */
#include "wifi.h"

static const char *TAG = "ui_notes";

/* ── Palette (Material Dark — matches Settings) ──────────── */
#define COL_BG         0x0A0A0F
#define COL_CARD       0x1A1A2E
#define COL_CARD2      0x2C2C2E
#define COL_AMBER      0xF59E0B
/* TT #328 Wave 1: COL_CYAN was an alias for COL_AMBER (0xF59E0B) — clearly
 * a copy-paste bug from the v3 palette pull-out.  No call-site references
 * it (verified via grep), so dropping it cleanly rather than fixing the
 * hex.  If a real cyan is needed later, alias it through ui_theme.h. */
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

/* Reason a note ended up in NOTE_STATE_FAILED — surfaced as a chip on
 * the list row instead of the old generic "FAIL".  Persist+restore as the
 * "fr" JSON key. */
typedef enum {
   NOTE_FAIL_NONE = 0,
   NOTE_FAIL_AUTH,     /* Dragon returned 401 (no bearer or wrong token) */
   NOTE_FAIL_NETWORK,  /* HTTP open / write / non-200 (not auth) */
   NOTE_FAIL_EMPTY,    /* Dragon returned 200 + empty STT text */
   NOTE_FAIL_NO_AUDIO, /* WAV missing on disk / too small / unreadable */
   NOTE_FAIL_TOO_LONG, /* hit MAX_NOTE_REC_SECS cap during recording */
} note_fail_t;

#define MAX_AUDIO_PATH 64
#define MAX_NOTE_REC_SECS 300 /* 5 min hard cap on SD recording */

/* PR 3 cleanup pass: layout constants hoisted to file scope so the
 * dynamic-relayout helper (notes_relayout_list) can use them outside
 * ui_notes_create.  SEARCH_H is the collapsed-by-default search ta
 * height; FILTER_H is the four-pill row; PROC_H is the processing
 * row that the pipeline subscriber paints; FAB_SZ is the amber dictate
 * button.  Per-screen ui_notes_create still re-#defines these locally
 * for documentation context — the file-scope decls win at compile time. */
#define SEARCH_H 48
#define FILTER_H 44
#define PROC_H 56
#define FAB_SZ 64

/* PR 3: classification of the note's content.  Informs the timeline filter
 * pills (All / Voice / Text / Pending) and feeds PR 4's action chips.
 * NOTE_TYPE_AUTO is the legacy compatibility value — when loaded from JSON
 * we leave it as AUTO and the render path falls back to `is_voice` so the
 * timeline behaves identically until a new dictation lands with a concrete
 * type from Dragon.  PR 4 introduces NOTE_TYPE_LIST + NOTE_TYPE_REMINDER. */
typedef enum {
   NOTE_TYPE_AUTO = 0,
   NOTE_TYPE_TEXT,
   NOTE_TYPE_VOICE,
   NOTE_TYPE_LIST,
   NOTE_TYPE_REMINDER,
} note_type_t;

/* PR 4: action-chip pending state.  Dragon's classifier proposes a
 * conversion (reminder or list) after dictation_summary; the chip
 * renders below the row body when kind != PENDING_NONE and confidence
 * is at or above PENDING_CONFIDENCE_FLOOR.  Tap ✓ confirms (schedules
 * notification / flips type); tap ✕ clears.  Persisted as JSON object
 * under key "pc". */
typedef enum {
   PENDING_NONE = 0,
   PENDING_REMINDER = 1,
   PENDING_LIST = 2,
} pending_kind_t;

#define PENDING_CONFIDENCE_FLOOR 75 /* 0-100; chip hidden below this */
#define PENDING_PAYLOAD_LEN 128

typedef struct {
   uint8_t kind;       /* pending_kind_t */
   uint8_t confidence; /* 0-100 */
   char payload[PENDING_PAYLOAD_LEN];
   /* Reminder payload format: ISO-8601 datetime, e.g. "2026-05-19T18:00".
    * List payload format: a single comma + delimiter signal like
    * "delim=comma" — Tab5 splits the transcript itself on confirm. */
} pending_chip_t;

/* ── Note storage ───────────────────────────────────────── */
typedef struct {
    char text[MAX_NOTE_LEN];
    char audio_path[MAX_AUDIO_PATH]; /* e.g. "/sdcard/rec/0042.wav" or "" */
    note_state_t state;
    note_fail_t fail_reason;
    note_type_t type;       /* PR 3 */
    pending_chip_t pending; /* PR 3 (reserved for PR 4) */
    bool is_voice;
    uint8_t hour;
    uint8_t minute;
    uint8_t day;
    uint8_t month;
    uint8_t year;     /* year offset from 2000 */
    bool used;
    bool needs_sync;  /* S6: true if not yet synced to Dragon */
} note_entry_t;

/* Wave 20 (closes #330): s_notes was a BSS-static array (~17.7 KB) that
 * eats internal SRAM at boot — same failure class as the recurring
 * vApplicationGetTimerTaskMemory assert hit in Waves 11 / 13 / 15.
 * PSRAM-lazy: allocate on first use via ensure_notes_buf().  Every public
 * entry point (notes_load + the few callers that bypass it) calls the
 * helper before touching the buffer, so the rest of the file's index
 * arithmetic stays unchanged. */
static note_entry_t *s_notes = NULL;
static int s_note_count = 0;
static int s_next_slot = 0;
static bool s_loaded = false; /* NVS loaded at least once */

static bool ensure_notes_buf(void) {
   if (s_notes) return true;
   s_notes = heap_caps_calloc(MAX_NOTES, sizeof(*s_notes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!s_notes) {
      ESP_LOGE(TAG, "PSRAM alloc for s_notes (%u bytes) failed", (unsigned)(MAX_NOTES * sizeof(*s_notes)));
      return false;
   }
   ESP_LOGI(TAG, "s_notes PSRAM-allocated: %u bytes", (unsigned)(MAX_NOTES * sizeof(*s_notes)));
   return true;
}

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
    /* Dragon W13 C2: /api/notes is bearer-gated.  Read dragon_tok from NVS
     * and add the header; without it every sync silently 401s. */
    char dtok[80];
    if (tab5_settings_get_dragon_api_token(dtok, sizeof(dtok)) == ESP_OK && dtok[0]) {
       char auth_hdr[96];
       snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", dtok);
       esp_http_client_set_header(client, "Authorization", auth_hdr);
    }
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
        if (n->state == NOTE_STATE_FAILED && n->fail_reason != NOTE_FAIL_NONE) {
           cJSON_AddNumberToObject(obj, "fr", (int)n->fail_reason);
        }
        if (n->type != NOTE_TYPE_AUTO) {
           cJSON_AddNumberToObject(obj, "ty", (int)n->type);
        }
        /* PR 4: persist pending_chip when populated. */
        if (n->pending.kind != PENDING_NONE) {
           cJSON *pc = cJSON_CreateObject();
           cJSON_AddNumberToObject(pc, "k", n->pending.kind);
           cJSON_AddNumberToObject(pc, "c", n->pending.confidence);
           cJSON_AddStringToObject(pc, "p", n->pending.payload);
           cJSON_AddItemToObject(obj, "pc", pc);
        }
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

    /* Atomic write: .tmp → rename. Prevents corruption on power loss.
     *
     * Wave 13 H8: fflush() only pushes libc's buffer into the kernel; the
     * ESP-IDF FATFS driver still holds dirty sectors in its own cache until
     * an f_sync() lands. A yank during that window left NOTES_TMP_PATH
     * readable but with stale content, which rename() then promoted to
     * NOTES_SD_PATH — a silent data corruption.  Use fsync(fd) on the raw
     * descriptor, which the ESP-IDF VFS routes to FATFS f_sync(). */
    if (tab5_sdcard_mounted()) {
        FILE *f = fopen(NOTES_TMP_PATH, "w");
        if (f) {
            int written = fputs(json, f);
            fflush(f);
            int fd = fileno(f);
            if (fd >= 0) {
                if (fsync(fd) != 0) {
                    ESP_LOGW(TAG, "fsync of notes.json.tmp failed (errno=%d)", errno);
                }
            }
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
    /* Wave 20: ensure the PSRAM buffer exists before any read/write below.
     * If the alloc fails we still flip s_loaded so we don't loop, but
     * every subsequent operation will short-circuit via the same guard. */
    if (!ensure_notes_buf()) {
       s_loaded = true;
       return;
    }
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

    memset(s_notes, 0, MAX_NOTES * sizeof(*s_notes)); /* Wave 20: pointer */
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
        cJSON *jfr = cJSON_GetObjectItem(item, "fr");
        n->fail_reason = (cJSON_IsNumber(jfr)) ? (note_fail_t)(int)jfr->valuedouble : NOTE_FAIL_NONE;
        cJSON *jty = cJSON_GetObjectItem(item, "ty");
        n->type = (cJSON_IsNumber(jty)) ? (note_type_t)(int)jty->valuedouble : NOTE_TYPE_AUTO;
        /* PR 4: load pending_chip; missing keys default to PENDING_NONE. */
        cJSON *jpc = cJSON_GetObjectItem(item, "pc");
        memset(&n->pending, 0, sizeof(n->pending));
        if (cJSON_IsObject(jpc)) {
           cJSON *jk = cJSON_GetObjectItem(jpc, "k");
           cJSON *jc = cJSON_GetObjectItem(jpc, "c");
           cJSON *jp = cJSON_GetObjectItem(jpc, "p");
           if (cJSON_IsNumber(jk)) n->pending.kind = (uint8_t)jk->valuedouble;
           if (cJSON_IsNumber(jc)) n->pending.confidence = (uint8_t)jc->valuedouble;
           if (cJSON_IsString(jp) && jp->valuestring) {
              strncpy(n->pending.payload, jp->valuestring, PENDING_PAYLOAD_LEN - 1);
              n->pending.payload[PENDING_PAYLOAD_LEN - 1] = '\0';
           }
        }
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

/* ── PR 3: filter pills + day-section state ─────────────── */
typedef enum {
   NOTE_FILTER_ALL = 0,
   NOTE_FILTER_VOICE,
   NOTE_FILTER_TEXT,
   NOTE_FILTER_PENDING, /* placeholder for PR 4 pending_chip filter */
} note_filter_t;
static note_filter_t s_filter = NOTE_FILTER_ALL;

typedef enum {
   DAY_SECTION_TODAY = 0,
   DAY_SECTION_YESTERDAY,
   DAY_SECTION_THIS_WEEK,
   DAY_SECTION_OLDER,
} day_section_t;

static lv_obj_t *s_filter_row = NULL;
static lv_obj_t *s_filter_pill[4] = {NULL};
static lv_obj_t *s_proc_row = NULL; /* processing row with mini-orb */
static lv_obj_t *s_proc_orb = NULL;
static lv_obj_t *s_proc_label = NULL;
static lv_obj_t *s_proc_close = NULL;
static lv_obj_t *s_fab = NULL; /* amber dictate FAB */

/* ── Screen state ──────────────────────────────────────── */
static lv_obj_t *s_screen      = NULL;
static lv_obj_t *s_list        = NULL;
/* #170: guard all LVGL accesses from the background transcription task
 * (and any other async-dispatched path) against a racing
 * ui_notes_destroy.  Pattern matches ui_wifi.c / ui_settings.c. */
static volatile bool s_destroying = false;
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
static lv_obj_t *s_topbar_clear = NULL;   /* "CLEAR FAILED (N)" button — visible when any FAILED notes exist */
static lv_timer_t *s_rec_timer = NULL;    /* 1s update timer */
static int s_rec_seconds = 0;
/* Wave 14 W14-L01: deleted `s_rec_paused` + its three callsites.
 * Pause/resume was never implemented; the variable was always false,
 * the guard in rec_timer_cb was a no-op, and the two assignments-to-
 * false at recording start/stop were dead stores. If/when we actually
 * add pause/resume, re-introduce with a matching toggle path. */

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
static void cb_note_retry(lv_event_t *e);
static void cb_clear_failed(lv_event_t *e);
static void cb_search_changed(lv_event_t *e);
static void refresh_list(void);
static void add_note_card(lv_obj_t *parent, const note_entry_t *note, int note_idx);
static void add_note_card_sectioned(lv_obj_t *parent, const note_entry_t *note, int note_idx, day_section_t sec);
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

/* Bar: full width, 40px tall, positioned below search bar above notes list.
 * Layout (post-cleanup, May 2026): topbar (48) → filter pills (44) →
 * processing row (56) → list.  Recording-bar reuses the slot the
 * processing row would occupy when no pipeline is active.
 * We insert the indicator right there and push list down. */
#define REC_BAR_Y (TOPBAR_H + 8 + 44 /* FILTER_H */ + 8)
#define REC_BAR_H 40

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

    /* Restore list position */
    if (s_list) {
#define LIST_Y_NORMAL (TOPBAR_H + 8 + 44 /* FILTER_H */ + 56 /* PROC_H */ + 16)
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

/* Forward decl — s_rec_note_slot is defined further down with the
 * recording-flow statics but the dictated-async helper below uses it
 * to skip duplicate creation when the local "+ NEW VOICE NOTE" flow
 * already owns a slot.  #537 adds s_pipeline_armed_slot + s_rec_file
 * to distinguish the home Dictate chip's pre-armed WAV path. */
static int s_rec_note_slot;
static bool s_pipeline_armed_slot;
static FILE *s_rec_file;
static int find_most_recent_used_slot(void);
static void pending_chip_apply_inline(int idx);

/* PR 3 follow-up: deferred WS-thread → LVGL-thread dictated-note add.
 * The dictation_summary handler in voice_ws_proto.c calls
 * ui_notes_add_dictated_async with a transcript; we copy the bytes
 * into PSRAM (s_notes touches PSRAM but refresh_list touches LVGL,
 * so this MUST run on the LVGL thread) and dispatch via
 * tab5_lv_async_call.  The callback runs ui_notes_add directly and
 * frees the heap buffer. */
static void notes_add_dictated_async_cb(void *arg) {
   char *text = (char *)arg;
   if (!text) return;
   /* #537: if the pipeline-arm path (home Dictate chip / chat mic) opened
    * a WAV file ahead of this dictation, finalise it now — write the
    * transcript into the reserved slot, close the WAV, attach audio_path.
    * This is the path that gives Dragon-routed voice notes a playable
    * SD recording. */
   if (s_pipeline_armed_slot && s_rec_note_slot >= 0 && s_rec_file != NULL) {
      ESP_LOGI(TAG, "Pipeline-armed slot %d → finalising with transcript", s_rec_note_slot);
      s_pipeline_armed_slot = false;
      /* The arm path hides the slot (used=false) so a placeholder row
       * doesn't render in-flight; flip it back before finalisation so
       * the new transcribed note actually appears on the timeline. */
      if (s_rec_note_slot < MAX_NOTES && !s_notes[s_rec_note_slot].used) {
         s_notes[s_rec_note_slot].used = true;
         if (s_note_count < MAX_NOTES) s_note_count++;
      }
      int finalized_slot = s_rec_note_slot;
      ui_notes_stop_recording(text[0] ? text : NULL);
      /* PR 4: attach any pending chip from the WS handoff buffer to
       * the slot we just finalised — this same-frame inline path
       * sidesteps LVGL's LIFO async ordering. */
      pending_chip_apply_inline(finalized_slot);
      free(text);
      return;
   }
   /* Skip if the local-recording flow already owns a slot (the
    * "+ NEW VOICE NOTE" path on Notes — ui_notes_start_recording
    * sets s_rec_note_slot >= 0 until ui_notes_stop_recording
    * finalises it).  Without this guard a single dictation would
    * land twice on the timeline. */
   if (s_rec_note_slot >= 0) {
      ESP_LOGI(TAG, "Skipping pipeline-add — local recording slot %d already active", s_rec_note_slot);
      free(text);
      return;
   }
   if (text[0]) {
      ui_notes_add(text, true);
      /* ui_notes_add stored at (s_next_slot - 1) and bumped s_next_slot;
       * find_most_recent_used_slot returns that same index. */
      int idx = find_most_recent_used_slot();
      if (idx >= 0 && idx < MAX_NOTES) {
         /* Tag the new note as a voice dictation explicitly so the
          * Notes filter pill 'Voice' picks it up without relying on
          * legacy is_voice (which is also set above). */
         s_notes[idx].type = NOTE_TYPE_VOICE;
         /* PR 4: attach any pending chip from the WS handoff. */
         pending_chip_apply_inline(idx);
         notes_save();
         refresh_list();
      }
   }
   free(text);
}

void ui_notes_add_dictated_async(const char *transcript) {
   if (!transcript || !transcript[0]) return;
   size_t n = strnlen(transcript, MAX_NOTE_LEN - 1);
   char *copy = (char *)malloc(n + 1);
   if (!copy) return;
   memcpy(copy, transcript, n);
   copy[n] = '\0';
   tab5_lv_async_call(notes_add_dictated_async_cb, copy);
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

/* #537: pipeline-armed recording flag.  Distinguishes a slot opened by
 * ui_notes_pipeline_arm_recording (home Dictate chip path) from one
 * opened by ui_notes_start_recording via the FAB.  The FAB owns its own
 * stop-via-tap teardown; the pipeline-armed slot is finalised when
 * dictation_summary arrives or discarded on cancel. */
static bool s_pipeline_armed_slot = false;

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

/* #537: arm a SD WAV recording for an incoming pipeline-path dictation.
 * The reserved slot is left with `used = false` until dictation_summary
 * arrives — the processing row at the top of Notes (and the home Dictate
 * chip M:SS hint) communicate state during the in-flight phase, so a
 * placeholder row in the list would be redundant + visually noisy with
 * an inert play button. */
bool ui_notes_pipeline_arm_recording(void) {
   if (s_rec_file != NULL || s_rec_note_slot >= 0) {
      ESP_LOGI(TAG, "Pipeline arm rejected — recording already in flight (slot %d)", s_rec_note_slot);
      return false;
   }
   const char *wav = ui_notes_start_recording();
   if (!wav) return false;
   s_pipeline_armed_slot = true;
   if (s_rec_note_slot >= 0 && s_rec_note_slot < MAX_NOTES) {
      /* Hide the row until summary lands.  notes_add_dictated_async_cb
       * flips used back to true via ui_notes_stop_recording(transcript). */
      s_notes[s_rec_note_slot].used = false;
      if (s_note_count > 0) s_note_count--;
      s_notes[s_rec_note_slot].text[0] = '\0';
      s_notes[s_rec_note_slot].type = NOTE_TYPE_VOICE;
   }
   refresh_list();
   return true;
}

/* #537: cancel a pipeline-armed recording.  Closes the WAV, removes the
 * file, releases the slot. */
void ui_notes_pipeline_cancel_recording(void) {
   if (!s_pipeline_armed_slot) return;
   ESP_LOGI(TAG, "Pipeline-armed slot %d → discarding", s_rec_note_slot);
   s_pipeline_armed_slot = false;

   if (s_rec_mutex) xSemaphoreTake(s_rec_mutex, portMAX_DELAY);
   if (s_rec_file) {
      fclose(s_rec_file);
      s_rec_file = NULL;
   }
   if (s_rec_mutex) xSemaphoreGive(s_rec_mutex);

   if (s_rec_path[0]) {
      remove(s_rec_path);
      s_rec_path[0] = '\0';
   }
   if (s_rec_note_slot >= 0 && s_rec_note_slot < MAX_NOTES) {
      s_notes[s_rec_note_slot].used = false;
      if (s_note_count > 0) s_note_count--;
   }
   s_rec_note_slot = -1;
   s_rec_samples = 0;
   notes_save();
   refresh_list();
}

/* PR 4: pending-chip async attach. */
typedef struct {
   uint8_t kind;
   uint8_t confidence;
   char payload[PENDING_PAYLOAD_LEN];
} pending_chip_msg_t;

static int find_most_recent_used_slot(void) {
   /* Walk back from s_next_slot through the ring, return the most-recent
    * used slot index, or -1 if none. */
   for (int i = 0; i < MAX_NOTES; i++) {
      int idx = (s_next_slot - 1 - i + MAX_NOTES) % MAX_NOTES;
      if (s_notes[idx].used) return idx;
   }
   return -1;
}

/* Pending-chip handoff buffer.  Set by the WS RX thread when a
 * dictation_summary frame carries a proposed_action; consumed by the
 * LVGL-thread notes_add_dictated_async_cb AFTER it lands the new note.
 *
 * LVGL's lv_async_call is implemented as a LIFO (`_lv_ll_ins_head`),
 * so queuing two callbacks (add then attach) makes the attach run
 * first — exactly off-by-one against the just-added slot.  Instead we
 * stash the chip data here and the same add cb attaches it inline
 * after the slot is finalised. */
static pending_chip_msg_t s_pending_chip_handoff = {0};
static bool s_pending_chip_pending = false;
static SemaphoreHandle_t s_pending_chip_mutex = NULL;

static void pending_chip_apply_inline(int idx) {
   if (idx < 0 || idx >= MAX_NOTES) return;
   if (!s_pending_chip_mutex) return;
   xSemaphoreTake(s_pending_chip_mutex, portMAX_DELAY);
   if (s_pending_chip_pending) {
      s_notes[idx].pending.kind = s_pending_chip_handoff.kind;
      s_notes[idx].pending.confidence = s_pending_chip_handoff.confidence;
      size_t plen = strnlen(s_pending_chip_handoff.payload, PENDING_PAYLOAD_LEN - 1);
      memcpy(s_notes[idx].pending.payload, s_pending_chip_handoff.payload, plen);
      s_notes[idx].pending.payload[plen] = '\0';
      ESP_LOGI(TAG, "Pending chip → slot %d (kind=%u conf=%u)", idx, s_notes[idx].pending.kind,
               s_notes[idx].pending.confidence);
      memset(&s_pending_chip_handoff, 0, sizeof(s_pending_chip_handoff));
      s_pending_chip_pending = false;
   }
   xSemaphoreGive(s_pending_chip_mutex);
}

void ui_notes_attach_pending_chip_async(uint8_t kind, uint8_t confidence, const char *payload) {
   if (kind == PENDING_NONE) return;
   if (confidence < PENDING_CONFIDENCE_FLOOR) return; /* below floor — don't surface */
   if (!s_pending_chip_mutex) {
      s_pending_chip_mutex = xSemaphoreCreateMutex();
      if (!s_pending_chip_mutex) return;
   }
   xSemaphoreTake(s_pending_chip_mutex, portMAX_DELAY);
   s_pending_chip_handoff.kind = kind;
   s_pending_chip_handoff.confidence = confidence;
   s_pending_chip_handoff.payload[0] = '\0';
   if (payload && payload[0]) {
      size_t plen = strnlen(payload, PENDING_PAYLOAD_LEN - 1);
      memcpy(s_pending_chip_handoff.payload, payload, plen);
      s_pending_chip_handoff.payload[plen] = '\0';
   }
   s_pending_chip_pending = true;
   xSemaphoreGive(s_pending_chip_mutex);
}

/* PR 4: scheduler-notification POST helper.  Fires when the user taps ✓
 * on a reminder chip.  Re-uses the same auth + URL pattern as
 * sync_note_to_dragon_task.  Fire-and-forget; pending_chip is cleared
 * locally regardless of HTTP outcome (the user's intent was confirmed). */
typedef struct {
   char when_iso[40];
   char label[120];
} scheduler_post_args_t;

static void scheduler_post_task(void *arg) {
   scheduler_post_args_t *a = (scheduler_post_args_t *)arg;
   char dhost[64];
   tab5_settings_get_dragon_host(dhost, sizeof(dhost));
   char url[160];
   snprintf(url, sizeof(url), "http://%s:%d/api/v1/scheduler/notifications", dhost, 3502);

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "when", a->when_iso);
   cJSON_AddStringToObject(body, "label", a->label);
   char *json = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!json) {
      free(a);
      vTaskSuspend(NULL);
      return;
   }
   esp_http_client_config_t cfg = {
       .url = url,
       .method = HTTP_METHOD_POST,
       .timeout_ms = 10000,
   };
   esp_http_client_handle_t client = esp_http_client_init(&cfg);
   esp_http_client_set_header(client, "Content-Type", "application/json");
   char dtok[80];
   if (tab5_settings_get_dragon_api_token(dtok, sizeof(dtok)) == ESP_OK && dtok[0]) {
      char auth_hdr[96];
      snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", dtok);
      esp_http_client_set_header(client, "Authorization", auth_hdr);
   }
   esp_http_client_set_post_field(client, json, strlen(json));
   esp_err_t err = esp_http_client_perform(client);
   int status = esp_http_client_get_status_code(client);
   if (err == ESP_OK && (status == 200 || status == 201)) {
      ESP_LOGI(TAG, "Reminder scheduled on Dragon (status=%d): %s @ %s", status, a->label, a->when_iso);
   } else {
      ESP_LOGW(TAG, "Reminder schedule failed: err=%s status=%d", esp_err_to_name(err), status);
   }
   esp_http_client_cleanup(client);
   free(json);
   free(a);
   vTaskSuspend(NULL);
}

static void scheduler_post(const char *when_iso, const char *label) {
   if (!when_iso || !when_iso[0]) return;
   if (!tab5_wifi_connected()) {
      ESP_LOGW(TAG, "Scheduler post skipped — WiFi not connected");
      return;
   }
   scheduler_post_args_t *args = calloc(1, sizeof(*args));
   if (!args) return;
   size_t wlen = strnlen(when_iso, sizeof(args->when_iso) - 1);
   memcpy(args->when_iso, when_iso, wlen);
   args->when_iso[wlen] = '\0';
   const char *lab = label ? label : "Reminder";
   size_t llen = strnlen(lab, sizeof(args->label) - 1);
   memcpy(args->label, lab, llen);
   args->label[llen] = '\0';
   xTaskCreatePinnedToCore(scheduler_post_task, "sched_post", 4096, args, 3, NULL, 0);
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
                s_notes[i].fail_reason = NOTE_FAIL_NO_AUDIO;
                voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NO_AUDIO, (uint32_t)(esp_timer_get_time() / 1000));
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

        /* PR 1: pipeline UPLOADING — REST POST about to begin. */
        voice_dictation_set_note_slot(slot);
        voice_dictation_set_state(DICT_UPLOADING, DICT_FAIL_NONE, (uint32_t)(esp_timer_get_time() / 1000));

        /* Read WAV file from SD */
        FILE *f = fopen(n->audio_path, "rb");
        if (!f) {
            ESP_LOGW(TAG, "Cannot open WAV: %s", n->audio_path);
            n->state = NOTE_STATE_FAILED;
            n->fail_reason = NOTE_FAIL_NO_AUDIO;
            voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NO_AUDIO, (uint32_t)(esp_timer_get_time() / 1000));
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
            n->fail_reason = NOTE_FAIL_NO_AUDIO;
            voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NO_AUDIO, (uint32_t)(esp_timer_get_time() / 1000));
            snprintf(n->text, MAX_NOTE_LEN, "(Empty recording)");
            notes_save();
            continue;
        }
        /* Fix WAV header if it was from a crashed recording (header says 0 data).
         * Wave 14 W14-M04: check every fread/fwrite return.  A short
         * read used to leave `hdr_data_size` with stack garbage, which
         * then got written to disk — silent corruption of the RIFF
         * size fields on a flaky SD card. */
        if (file_size > 44) {
            uint32_t hdr_data_size = 0;
            if (fseek(f, 40, SEEK_SET) != 0 ||
                fread(&hdr_data_size, 4, 1, f) != 1) {
                ESP_LOGW(TAG, "WAV header read failed for %s — skipping repair",
                         n->audio_path);
            } else if (hdr_data_size == 0 ||
                       hdr_data_size > (uint32_t)(file_size - 44)) {
                /* Fix the header in place */
                uint32_t actual_data = (uint32_t)(file_size - 44);
                uint32_t riff_size = actual_data + 36;
                bool ok =
                    (fseek(f, 4, SEEK_SET) == 0) &&
                    (fwrite(&riff_size, 4, 1, f) == 1) &&
                    (fseek(f, 40, SEEK_SET) == 0) &&
                    (fwrite(&actual_data, 4, 1, f) == 1);
                if (ok) {
                    fflush(f);
                    ESP_LOGI(TAG, "Fixed WAV header: %lu data bytes",
                             (unsigned long)actual_data);
                } else {
                    ESP_LOGW(TAG, "WAV header repair fwrite failed for %s",
                             n->audio_path);
                }
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
        /* Wave 14 W14-C03: esp_http_client_init can return NULL under
         * fragmented internal SRAM.  Prior code called set_header on NULL
         * and crashed the transcribe_q task, which silently dies until the
         * next reboot. Fail the note cleanly instead. */
        if (!client) {
            ESP_LOGE(TAG, "transcribe: esp_http_client_init NULL (heap pressure?)");
            fclose(f);
            n->state = NOTE_STATE_FAILED;
            n->fail_reason = NOTE_FAIL_NETWORK;
            voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, (uint32_t)(esp_timer_get_time() / 1000));
            notes_save();
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "audio/wav");
        /* Dragon W13 C2 middleware gates /api/v1/transcribe behind bearer
         * auth.  Without this header every upload 401s — was the root
         * cause of the 100% FAIL rate on the Notes screen. */
        char dtok[80];
        if (tab5_settings_get_dragon_api_token(dtok, sizeof(dtok)) == ESP_OK && dtok[0]) {
           char auth_hdr[96];
           snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", dtok);
           esp_http_client_set_header(client, "Authorization", auth_hdr);
        }

        fseek(f, 0, SEEK_SET);
        esp_err_t err = esp_http_client_open(client, file_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            fclose(f);
            n->state = NOTE_STATE_FAILED;
            n->fail_reason = NOTE_FAIL_NETWORK;
            voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, (uint32_t)(esp_timer_get_time() / 1000));
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

        /* PR 1: TRANSCRIBING — request fully sent, Dragon now running STT. */
        voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, (uint32_t)(esp_timer_get_time() / 1000));

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
                        n->fail_reason = NOTE_FAIL_NONE;
                        voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, (uint32_t)(esp_timer_get_time() / 1000));
                        ESP_LOGI(TAG, "Transcription done [%d]: %.60s", slot, text);
                    } else {
                        snprintf(n->text, MAX_NOTE_LEN, "(Empty transcription)");
                        n->state = NOTE_STATE_FAILED;
                        n->fail_reason = NOTE_FAIL_EMPTY;
                        voice_dictation_set_state(DICT_FAILED, DICT_FAIL_EMPTY,
                                                  (uint32_t)(esp_timer_get_time() / 1000));
                    }
                    cJSON_Delete(root);
                }
                free(resp);
            }
        } else {
            ESP_LOGW(TAG, "Transcribe HTTP %d (len=%d)", status, content_len);
            n->state = NOTE_STATE_FAILED;
            n->fail_reason = (status == 401 || status == 403) ? NOTE_FAIL_AUTH : NOTE_FAIL_NETWORK;
            voice_dictation_set_state(DICT_FAILED,
                                      (status == 401 || status == 403) ? DICT_FAIL_AUTH : DICT_FAIL_NETWORK,
                                      (uint32_t)(esp_timer_get_time() / 1000));
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        notes_save();
        /* #170: refresh_list touches LVGL objects; this task runs on
         * Core 1 outside the UI task, so direct calls race with the UI
         * thread (including concurrent ui_notes_destroy from a nav).
         * Route via lv_async_call so the refresh happens on the LVGL
         * timer tick and the s_destroying guard catches it cleanly. */
        tab5_lv_async_call((lv_async_cb_t)refresh_list, NULL);
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
    /* Tear down anything floating above the notes list before we hide it.
     * Without this, swiping out of the note editor left the keyboard (and
     * its preview row showing the note text) stuck on top of Home. */
    if (s_edit_overlay) { lv_obj_del(s_edit_overlay); s_edit_overlay = NULL; s_edit_ta = NULL; }
    ui_keyboard_hide();
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
    /* 5-min hard cap.  Mic chunks are 20 ms (50 frames/s) so 300 s = 15000.
     * Without this the SD recording would run until the user comes back
     * and taps stop — a 477 s zombie was the proximate cause for adding
     * this cap (audit 2026-05-14). */
    const int max_frames = MAX_NOTE_REC_SECS * 50;
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
        if (frames >= max_frames) {
           ESP_LOGW(TAG, "SD rec: hit %ds cap — auto-stopping", MAX_NOTE_REC_SECS);
           s_sd_rec_running = false;
           /* Marshal the stop onto the LVGL thread same as the manual
            * stop path in cb_new_voice so we don't race the recording
            * indicator or s_rec_file teardown. */
           tab5_lv_async_call((lv_async_cb_t)ui_notes_stop_recording, NULL);
           break;
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
        tab5_lv_async_call((lv_async_cb_t)ui_notes_stop_recording, NULL);
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
        /* Offline or busy — SD-only recording with standalone mic task.
         * Wave 14 W14-H07: stack bumped from 4 KB to 8 KB. The task
         * calls tab5_mic_read (I2S DMA path) then funnels through
         * FATFS/VFS (~2-4 KB of FATFS sector buffers + libc FILE state)
         * plus ESP_LOGI with formatted args. 4 KB trapped stack_chk_fail
         * on long offline recordings (>20 s). 8 KB matches the voice
         * mic task and gives comfortable headroom in PSRAM. */
        s_sd_rec_running = true;
        xTaskCreatePinnedToCore(
            sd_record_task, "sd_rec", 8192, NULL, 5, &s_sd_rec_task, 1);
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

    /* ── Paper-like editor.  Topbar runs a soft sentence-case caption
     *   on the left (kind · date · time) and two flat text actions on
     *   the right.  No card, no border, no all-caps — matches the row
     *   typography pass on the list. */
    lv_obj_t *tb = lv_obj_create(s_edit_overlay);
    lv_obj_set_size(tb, SW, TOPBAR_H + 16);
    lv_obj_set_pos(tb, 0, 0);
    lv_obj_set_style_bg_color(tb, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(0x1C1C28), 0); /* TH_HAIRLINE */
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    /* Left caption: 'Voice note · May 14 · 17:24' (sentence case, mid-dot) */
    char sys_buf[80];
    static const char *mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int mi = (n->month >= 1 && n->month <= 12) ? n->month - 1 : 0;
    /* Bullet glyph (U+2022, \xe2\x80\xa2) is the only sub-glyph in our
     * Montserrat builds; U+00B7 middle-dot is missing and renders as a
     * tofu rectangle.  Soft amber bullet keeps the separator quiet. */
    snprintf(sys_buf, sizeof(sys_buf), "%s note  \xe2\x80\xa2  %s %d  \xe2\x80\xa2  %02d:%02d",
             n->is_voice ? "Voice" : "Text", mn[mi], n->day, n->hour, n->minute);
    lv_obj_t *sys = lv_label_create(tb);
    lv_label_set_text(sys, sys_buf);
    lv_obj_set_style_text_font(sys, FONT_SMALL, 0);
    lv_obj_set_style_text_color(sys, lv_color_hex(0x8A8A92), 0);
    lv_obj_set_style_text_letter_space(sys, 0, 0);
    lv_obj_align(sys, LV_ALIGN_LEFT_MID, 24, 0);

    /* Right-side actions: Cancel + Save as flat text links. */
    lv_obj_t *cancel_btn = lv_button_create(tb);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_size(cancel_btn, 100, TOPBAR_H);
    lv_obj_align(cancel_btn, LV_ALIGN_RIGHT_MID, -130, 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(cancel_btn, cb_edit_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0x8A8A92), 0);
    lv_obj_set_style_text_font(cancel_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_letter_space(cancel_lbl, 0, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *save_btn = lv_button_create(tb);
    lv_obj_remove_style_all(save_btn);
    lv_obj_set_size(save_btn, 100, TOPBAR_H);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(save_btn, cb_edit_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_text_font(save_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_letter_space(save_lbl, 0, 0);
    lv_obj_center(save_lbl);

    /* ── Large textarea — flat, no card, paper-like. */
    int ta_y = TOPBAR_H + 40;
    int ta_h = USABLE_H - ta_y - 24;
    s_edit_ta = lv_textarea_create(s_edit_overlay);
    lv_obj_set_size(s_edit_ta, SW - 2 * 36, ta_h);
    lv_obj_set_pos(s_edit_ta, 36, ta_y);
    lv_textarea_set_text(s_edit_ta, n->text);
    lv_obj_set_style_bg_opa(s_edit_ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_edit_ta, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(s_edit_ta, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_line_space(s_edit_ta, 6, 0);
    lv_obj_set_style_border_width(s_edit_ta, 0, 0);
    lv_obj_set_style_radius(s_edit_ta, 0, 0);
    lv_obj_set_style_pad_all(s_edit_ta, 0, 0);
    lv_obj_add_flag(s_edit_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_edit_ta, cb_edit_ta_tap, LV_EVENT_CLICKED, NULL);
    ui_keyboard_show(s_edit_ta);
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

/* Retry a FAILED note: flip it back to RECORDED so the 15 s polling
 * task picks it up on its next sweep.  Audio still on SD, so this is
 * just a state reset.  No re-upload happens synchronously. */
static void cb_note_retry(lv_event_t *e) {
   int idx = (int)(intptr_t)lv_event_get_user_data(e);
   if (idx < 0 || idx >= MAX_NOTES || !s_notes[idx].used) return;
   if (s_notes[idx].state != NOTE_STATE_FAILED) return;
   if (!s_notes[idx].audio_path[0]) return;

   s_notes[idx].state = NOTE_STATE_RECORDED;
   s_notes[idx].fail_reason = NOTE_FAIL_NONE;
   notes_save();
   refresh_list();
   ESP_LOGI(TAG, "Retry queued for note [%d]: %s", idx, s_notes[idx].audio_path);
}

/* PR 4: action-chip tap handlers ─────────────────────────────────── */

/* Format an ISO-8601 datetime as "Tue 6 PM" (short, friendly).
 * Robust to malformed input — falls back to the raw string. */
static void format_reminder_when(const char *iso, char *out, size_t out_sz) {
   int Y = 0, M = 0, D = 0, h = 0, m = 0;
   if (sscanf(iso, "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &m) != 5) {
      size_t ilen = strnlen(iso, out_sz - 1);
      memcpy(out, iso, ilen);
      out[ilen] = '\0';
      return;
   }
   struct tm tm = {0};
   tm.tm_year = Y - 1900;
   tm.tm_mon = M - 1;
   tm.tm_mday = D;
   tm.tm_hour = h;
   tm.tm_min = m;
   mktime(&tm); /* fills tm_wday */
   static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
   const char *ampm = (h >= 12) ? "PM" : "AM";
   int h12 = h % 12;
   if (h12 == 0) h12 = 12;
   if (m == 0) {
      snprintf(out, out_sz, "%s %d %s", wd[tm.tm_wday % 7], h12, ampm);
   } else {
      snprintf(out, out_sz, "%s %d:%02d %s", wd[tm.tm_wday % 7], h12, m, ampm);
   }
}

/* Reformat a comma-separated transcript into newline-prefixed bullets. */
static void reformat_list_text(char *text) {
   char buf[MAX_NOTE_LEN];
   size_t out = 0;
   buf[out++] = '\xe2'; /* prepend a leading bullet */
   buf[out++] = '\x80';
   buf[out++] = '\xa2';
   buf[out++] = ' ';
   const char *p = text;
   while (*p && out < sizeof(buf) - 6) {
      while (*p == ' ' || *p == ',') p++;
      while (*p && *p != ',' && out < sizeof(buf) - 6) {
         buf[out++] = *p++;
      }
      if (*p == ',') {
         /* Insert "\n• " */
         buf[out++] = '\n';
         buf[out++] = '\xe2';
         buf[out++] = '\x80';
         buf[out++] = '\xa2';
         buf[out++] = ' ';
         p++;
      }
   }
   buf[out] = '\0';
   snprintf(text, MAX_NOTE_LEN, "%s", buf);
}

static void cb_chip_confirm(lv_event_t *e) {
   int idx = (int)(intptr_t)lv_event_get_user_data(e);
   if (idx < 0 || idx >= MAX_NOTES || !s_notes[idx].used) return;
   note_entry_t *n = &s_notes[idx];
   if (n->pending.kind == PENDING_REMINDER) {
      char preview_txt[120];
      size_t plen = strnlen(n->text, sizeof(preview_txt) - 1);
      memcpy(preview_txt, n->text, plen);
      preview_txt[plen] = '\0';
      scheduler_post(n->pending.payload, preview_txt);
      n->type = NOTE_TYPE_REMINDER;
   } else if (n->pending.kind == PENDING_LIST) {
      reformat_list_text(n->text);
      n->type = NOTE_TYPE_LIST;
   }
   memset(&n->pending, 0, sizeof(n->pending));
   notes_save();
   refresh_list();
   ESP_LOGI(TAG, "Pending chip confirmed for slot %d", idx);
}

static void cb_chip_dismiss(lv_event_t *e) {
   int idx = (int)(intptr_t)lv_event_get_user_data(e);
   if (idx < 0 || idx >= MAX_NOTES || !s_notes[idx].used) return;
   memset(&s_notes[idx].pending, 0, sizeof(s_notes[idx].pending));
   notes_save();
   refresh_list();
   ESP_LOGI(TAG, "Pending chip dismissed for slot %d", idx);
}

/* Bulk-delete all FAILED notes.  Calls the public ui_notes_clear_failed
 * which already knows how to drop entries safely. */
static void cb_clear_failed(lv_event_t *e) {
   (void)e;
   int n = ui_notes_clear_failed();
   if (n > 0) {
      ESP_LOGI(TAG, "Cleared %d failed notes", n);
   }
   refresh_list();
}

/* ── Note card widget ──────────────────────────────────── */
static void add_note_card_sectioned(lv_obj_t *parent, const note_entry_t *note, int note_idx, day_section_t sec) {
   note_entry_t n = *note;

   /* #170 follow-up: under sustained rapid-nav stress the LVGL pool can
    * transiently exhaust, making lv_*_create() return NULL.  Every
    * subsequent set_text / align / style-set on that NULL pointer
    * panics.  Short-circuit the card build if ANY of the essential
    * objects fail to allocate.  Worst case we drop one or two note
    * cards for this refresh — the next refresh will retry and usually
    * succeed once the pool has breathed. */

   /* Card — compact, flex column layout. Tap for full view. */
   lv_obj_t *card = lv_obj_create(parent);
   if (!card) return;
   lv_obj_set_width(card, lv_pct(100));
   lv_obj_set_height(card, LV_SIZE_CONTENT);
   /* FIX N1 capped cards at 160 px to keep the timeline scannable.  PR 4
    * lifts the cap to 230 px when an action chip is going to render below
    * the preview so the chip isn't clipped. */
   bool has_chip = (note->pending.kind != PENDING_NONE && note->pending.confidence >= PENDING_CONFIDENCE_FLOOR);
   lv_obj_set_style_max_height(card, has_chip ? 230 : 160, 0);
   /* v5: flat row, hairline rule underneath — no rounded bubble. */
   lv_obj_set_style_bg_color(card, lv_color_hex(0x08080E), 0); /* TH_BG */
   lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(card, 0, 0);
   lv_obj_set_style_border_width(card, 1, 0);
   lv_obj_set_style_border_color(card, lv_color_hex(0x1C1C28), 0); /* TH_HAIRLINE */
   lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
   lv_obj_set_style_pad_all(card, 12, 0);
   lv_obj_set_style_pad_row(card, 6, 0);
   lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN); /* Stack header + preview vertically */
   lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
   lv_obj_clear_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE); /* Bug fix: prevent gesture bubbling to s_screen cb_back */
   lv_obj_set_ext_click_area(card, 10);
   lv_obj_add_event_cb(card, cb_note_tap, LV_EVENT_CLICKED, (void *)(intptr_t)note_idx);

   /* Row 1: timestamp + badge + action buttons (all in one line) */
   lv_obj_t *header = lv_obj_create(card);
   if (!header) return; /* lv_obj_del(card) not needed — lv_obj_del(parent) will cascade */
   lv_obj_remove_style_all(header);
   lv_obj_set_size(header, lv_pct(100), 44); /* 44px matches touch target height */
   lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

   /* Section-aware timestamp: when the row is grouped under Today /
    * Yesterday the section header already says the day, so the row
    * just shows HH:MM and reads cleaner.  For This week / Earlier we
    * include the month + day so the user can tell rows apart at a
    * glance. */
   lv_obj_t *ts = lv_label_create(header);
   if (!ts) return;
   char ts_buf[32];
   static const char *mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
   int mi = (n.month >= 1 && n.month <= 12) ? n.month - 1 : 0;
   if (sec == DAY_SECTION_TODAY || sec == DAY_SECTION_YESTERDAY) {
      snprintf(ts_buf, sizeof(ts_buf), "%02d:%02d", n.hour, n.minute);
   } else {
      snprintf(ts_buf, sizeof(ts_buf), "%s %d  %02d:%02d", mn[mi], n.day, n.hour, n.minute);
   }
   lv_label_set_text(ts, ts_buf);
   lv_obj_set_style_text_color(ts, lv_color_hex(COL_LABEL2), 0);
   lv_obj_set_style_text_font(ts, FONT_CAPTION, 0);
   lv_obj_align(ts, LV_ALIGN_LEFT_MID, 0, 0);

   /* v5: kill the colored Material pill. Use a letter-spaced caption
    * in amber (or red on failure) inline next to the timestamp.  On
    * FAILED state, the badge now surfaces the specific failure reason
    * instead of a generic "FAIL" — so the user knows whether the
    * server is unreachable (NETWORK), the auth token is missing
    * (AUTH), the audio came back empty (EMPTY), the WAV is gone
    * (NO AUDIO), or the recording hit the 5-min cap (TOO LONG). */
   /* Type badge — sentence-cased, lower letter-spacing, dimmer hue for
    * passive note metadata; red only when the state is actively
    * surfacing a failure reason that the user can act on.  Was an
    * all-caps amber pill that read like a CTA. */
   lv_obj_t *badge = lv_label_create(header);
   if (!badge) return;
   const char *badge_text;
   uint32_t badge_color;
   switch (n.state) {
      case NOTE_STATE_RECORDED:
         badge_text = "Recording";
         badge_color = 0x8E8E98;
         break;
      case NOTE_STATE_TRANSCRIBING:
         badge_text = "Transcribing";
         badge_color = 0x8E8E98;
         break;
      case NOTE_STATE_TRANSCRIBED:
         badge_text = "Voice";
         badge_color = 0x8E8E98;
         break;
      case NOTE_STATE_FAILED:
         switch (n.fail_reason) {
            case NOTE_FAIL_AUTH:
               badge_text = "Auth fail";
               break;
            case NOTE_FAIL_EMPTY:
               badge_text = "Empty";
               break;
            case NOTE_FAIL_NO_AUDIO:
               badge_text = "No audio";
               break;
            case NOTE_FAIL_TOO_LONG:
               badge_text = "Too long";
               break;
            case NOTE_FAIL_NETWORK:
               badge_text = "Network";
               break;
            default:
               badge_text = "Failed";
               break;
         }
         badge_color = COL_RED;
         break;
      default:
         badge_text = n.is_voice ? "Voice" : "Text";
         badge_color = 0x8E8E98;
         break;
   }
   lv_label_set_text(badge, badge_text);
   lv_obj_set_style_text_color(badge, lv_color_hex(badge_color), 0);
   lv_obj_set_style_text_font(badge, FONT_CAPTION, 0);
   lv_obj_set_style_text_letter_space(badge, 0, 0);
   lv_obj_align_to(badge, ts, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

   /* Delete — outlined ghost button so the action stays accessible at
    * a 44 px touch target but doesn't compete with the note content
    * for visual weight.  Was a filled-dark pill with red icon; now a
    * transparent outline with a dimmer glyph that lights up on press. */
   lv_obj_t *del = lv_button_create(header);
   if (!del) return;
   lv_obj_set_size(del, 44, 44);
   lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
   lv_obj_set_style_bg_opa(del, LV_OPA_TRANSP, 0);
   lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_border_width(del, 0, 0);
   lv_obj_set_style_bg_color(del, lv_color_hex(COL_RED), LV_PART_MAIN | LV_STATE_PRESSED);
   lv_obj_set_style_bg_opa(del, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
   lv_obj_add_event_cb(del, cb_note_delete, LV_EVENT_CLICKED, (void *)(intptr_t)note_idx);
   lv_obj_t *del_lbl = lv_label_create(del);
   if (!del_lbl) return;
   lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
   lv_obj_set_style_text_color(del_lbl, lv_color_hex(0x6A6A72), 0);
   lv_obj_set_style_text_font(del_lbl, FONT_CAPTION, 0);
   lv_obj_center(del_lbl);

   /* Action button — outlined ghost (was filled solid).  Play stays
    * mint-tinted, retry stays amber-tinted via the icon color; the
    * background only fills on press for tactile feedback. */
   if (n.audio_path[0]) {
      bool is_retry = (n.state == NOTE_STATE_FAILED);
      lv_obj_t *act = lv_button_create(header);
      if (!act) return;
      lv_obj_set_size(act, 44, 44);
      lv_obj_align(act, LV_ALIGN_RIGHT_MID, -48, 0);
      lv_obj_set_style_bg_opa(act, LV_OPA_TRANSP, 0);
      lv_obj_set_style_radius(act, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(act, 1, 0);
      lv_obj_set_style_border_color(act, lv_color_hex(is_retry ? COL_AMBER : COL_MINT), 0);
      lv_obj_set_style_border_opa(act, LV_OPA_50, 0);
      lv_obj_set_style_bg_color(act, lv_color_hex(is_retry ? COL_AMBER : COL_MINT), LV_PART_MAIN | LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(act, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
      lv_obj_add_event_cb(act, is_retry ? cb_note_retry : cb_note_play, LV_EVENT_CLICKED, (void *)(intptr_t)note_idx);
      lv_obj_t *act_lbl = lv_label_create(act);
      if (!act_lbl) return;
      lv_label_set_text(act_lbl, is_retry ? LV_SYMBOL_REFRESH : LV_SYMBOL_PLAY);
      lv_obj_set_style_text_color(act_lbl, lv_color_hex(is_retry ? COL_AMBER : COL_MINT), 0);
      lv_obj_set_style_text_font(act_lbl, FONT_CAPTION, 0);
      lv_obj_center(act_lbl);
   }

   /* FIX N1+N4: Note preview — truncated to ~100 chars, smaller font */
   lv_obj_t *preview = lv_label_create(card);
   if (!preview) return; /* exact crash site from #170 follow-up coredump */
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

   /* PR 4: action chip — proposed reminder / list conversion from Dragon.
    * Rendered below the row body when kind != NONE and confidence >= floor.
    * Soft amber pill with a leading emoji-style indicator + label + ✓ + ✕.
    * Tap ✓ → schedules notification (reminder) or reformats text (list) +
    * clears chip.  Tap ✕ → clears chip, leaves note untouched. */
   if (n.pending.kind != PENDING_NONE && n.pending.confidence >= PENDING_CONFIDENCE_FLOOR) {
      lv_obj_t *chip = lv_obj_create(card);
      if (!chip) return;
      lv_obj_remove_style_all(chip);
      lv_obj_set_width(chip, lv_pct(100));
      lv_obj_set_height(chip, 56);
      lv_obj_set_style_bg_color(chip, lv_color_hex(0x1F1F2A), 0);
      lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(chip, 12, 0);
      lv_obj_set_style_border_width(chip, 1, 0);
      lv_obj_set_style_border_color(chip, lv_color_hex(COL_AMBER), 0);
      lv_obj_set_style_border_opa(chip, LV_OPA_40, 0);
      lv_obj_set_style_pad_left(chip, 14, 0);
      lv_obj_set_style_pad_right(chip, 6, 0);
      lv_obj_set_style_margin_top(chip, 10, 0);
      lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

      /* Label: "Set reminder Tue 6 PM" or "Make a list (4 items)". */
      char label_buf[120];
      if (n.pending.kind == PENDING_REMINDER) {
         char when_short[40];
         format_reminder_when(n.pending.payload, when_short, sizeof(when_short));
         snprintf(label_buf, sizeof(label_buf), LV_SYMBOL_BELL "  Set reminder %s", when_short);
      } else {
         /* Count commas as a rough item count for the chip hint. */
         int items = 1;
         for (const char *q = n.text; *q; q++) {
            if (*q == ',') items++;
         }
         snprintf(label_buf, sizeof(label_buf), LV_SYMBOL_LIST "  Make a list (%d items)", items);
      }
      lv_obj_t *lbl = lv_label_create(chip);
      if (!lbl) return;
      lv_label_set_text(lbl, label_buf);
      lv_obj_set_style_text_color(lbl, lv_color_hex(COL_AMBER), 0);
      lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
      lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

      /* ✕ dismiss button — rightmost, dim gray. */
      lv_obj_t *dismiss = lv_button_create(chip);
      if (!dismiss) return;
      lv_obj_set_size(dismiss, 44, 44);
      lv_obj_align(dismiss, LV_ALIGN_RIGHT_MID, 0, 0);
      lv_obj_set_style_bg_opa(dismiss, LV_OPA_TRANSP, 0);
      lv_obj_set_style_radius(dismiss, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(dismiss, 0, 0);
      lv_obj_set_style_bg_color(dismiss, lv_color_hex(0x6A6A72), LV_PART_MAIN | LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(dismiss, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
      lv_obj_add_event_cb(dismiss, cb_chip_dismiss, LV_EVENT_CLICKED, (void *)(intptr_t)note_idx);
      lv_obj_t *dismiss_lbl = lv_label_create(dismiss);
      if (!dismiss_lbl) return;
      lv_label_set_text(dismiss_lbl, LV_SYMBOL_CLOSE);
      lv_obj_set_style_text_color(dismiss_lbl, lv_color_hex(0x8A8A92), 0);
      lv_obj_set_style_text_font(dismiss_lbl, FONT_CAPTION, 0);
      lv_obj_center(dismiss_lbl);

      /* ✓ confirm button — to the left of dismiss, filled amber. */
      lv_obj_t *confirm = lv_button_create(chip);
      if (!confirm) return;
      lv_obj_set_size(confirm, 44, 44);
      lv_obj_align(confirm, LV_ALIGN_RIGHT_MID, -52, 0);
      lv_obj_set_style_bg_color(confirm, lv_color_hex(COL_AMBER), 0);
      lv_obj_set_style_bg_opa(confirm, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(confirm, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(confirm, 0, 0);
      lv_obj_add_event_cb(confirm, cb_chip_confirm, LV_EVENT_CLICKED, (void *)(intptr_t)note_idx);
      lv_obj_t *confirm_lbl = lv_label_create(confirm);
      if (!confirm_lbl) return;
      lv_label_set_text(confirm_lbl, LV_SYMBOL_OK);
      lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(COL_BG), 0);
      lv_obj_set_style_text_font(confirm_lbl, FONT_CAPTION, 0);
      lv_obj_center(confirm_lbl);
   }
}

/* ── PR 3: day-section grouping ─────────────────────────── */

static int days_since_2000_local(uint8_t y, uint8_t m, uint8_t d) {
   /* Cheap monotonic day index for comparison.  Uses tm_yday math via
    * mktime to handle month lengths correctly. */
   struct tm t = {0};
   t.tm_year = 100 + (int)y; /* tm_year is years since 1900 */
   t.tm_mon = (m > 0 ? m - 1 : 0);
   t.tm_mday = d;
   t.tm_hour = 12;
   time_t tt = mktime(&t);
   return (int)(tt / 86400);
}

static day_section_t classify_note_day(const note_entry_t *n) {
   time_t now = time(NULL);
   struct tm nt;
   localtime_r(&now, &nt);
   int today = days_since_2000_local((uint8_t)(nt.tm_year - 100), (uint8_t)(nt.tm_mon + 1), (uint8_t)nt.tm_mday);
   int nd = days_since_2000_local(n->year, n->month, n->day);
   int delta = today - nd;
   if (delta <= 0) return DAY_SECTION_TODAY;
   if (delta == 1) return DAY_SECTION_YESTERDAY;
   if (delta < 7) return DAY_SECTION_THIS_WEEK;
   return DAY_SECTION_OLDER;
}

static const char *day_section_label(day_section_t s) {
   switch (s) {
      case DAY_SECTION_TODAY:
         return "Today";
      case DAY_SECTION_YESTERDAY:
         return "Yesterday";
      case DAY_SECTION_THIS_WEEK:
         return "This week";
      case DAY_SECTION_OLDER:
         return "Earlier";
   }
   return "";
}

/* PR 3: filter predicate.  AUTO type falls back to is_voice for
 * pre-PR-4 notes so the timeline behaves identically. */
static bool note_matches_filter(const note_entry_t *n) {
   if (s_filter == NOTE_FILTER_ALL) return true;
   if (s_filter == NOTE_FILTER_VOICE) {
      if (n->type == NOTE_TYPE_VOICE || n->type == NOTE_TYPE_LIST || n->type == NOTE_TYPE_REMINDER) return true;
      if (n->type == NOTE_TYPE_AUTO && n->is_voice) return true;
      return false;
   }
   if (s_filter == NOTE_FILTER_TEXT) {
      if (n->type == NOTE_TYPE_TEXT) return true;
      if (n->type == NOTE_TYPE_AUTO && !n->is_voice) return true;
      return false;
   }
   if (s_filter == NOTE_FILTER_PENDING) {
      /* PR 4: a note is "pending" when Dragon's classifier proposed a
       * conversion (reminder / list) and the user hasn't confirmed or
       * dismissed yet AND the proposal cleared the confidence floor.
       * FAILED notes also count — they still need user attention. */
      if (n->pending.kind != PENDING_NONE && n->pending.confidence >= PENDING_CONFIDENCE_FLOOR) {
         return true;
      }
      return n->state == NOTE_STATE_FAILED;
   }
   return true;
}

static void add_day_section_header(lv_obj_t *parent, day_section_t s) {
   /* Softer, notebook-style heading.  Was an all-caps amber line that
    * read as terminal output; now it's title-cased dimmed-white with
    * tight letter-spacing so the bucket reads as a quiet caption above
    * its rows rather than competing with the note content for
    * attention. */
   lv_obj_t *lbl = lv_label_create(parent);
   lv_label_set_text(lbl, day_section_label(s));
   lv_obj_set_style_text_font(lbl, FONT_HEADING, 0);
   lv_obj_set_style_text_color(lbl, lv_color_hex(0xA0A0AA), 0);
   lv_obj_set_style_text_letter_space(lbl, 0, 0);
   lv_obj_set_style_pad_top(lbl, 14, 0);
   lv_obj_set_style_pad_bottom(lbl, 6, 0);
}

/* PR 3: paint the filter pills so the active one gets amber bg + dark
 * text; inactive ones get dark-pill bg + neutral text.  Called whenever
 * s_filter changes (on tap or on screen create). */
void ui_notes_paint_filter_pills(void) {
   for (int i = 0; i < 4; i++) {
      lv_obj_t *p = s_filter_pill[i];
      if (!p) continue;
      bool active = ((int)s_filter == i);
      lv_obj_set_style_bg_color(p, lv_color_hex(active ? 0xF59E0B : 0x141420), 0);
      lv_obj_set_style_border_color(p, lv_color_hex(active ? 0xF59E0B : 0x262637), 0);
      lv_obj_t *lbl = lv_obj_get_child(p, 0);
      if (lbl) {
         lv_obj_set_style_text_color(lbl, lv_color_hex(active ? 0x141420 : 0xE8E8EF), 0);
      }
   }
}

/* ── PR 3: processing row that subscribes to the dictation pipeline ── */

static lv_timer_t *s_proc_rec_ticker = NULL;

static void proc_rec_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_proc_label) return;
   dict_event_t e = voice_dictation_get();
   if (e.state != DICT_RECORDING) {
      if (s_proc_rec_ticker) {
         lv_timer_del(s_proc_rec_ticker);
         s_proc_rec_ticker = NULL;
      }
      return;
   }
   uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
   uint32_t dur_ms = (e.started_ms && now_ms >= e.started_ms) ? (now_ms - e.started_ms) : 0;
   uint32_t s = dur_ms / 1000;
   char buf[40];
   snprintf(buf, sizeof(buf), "RECORDING  %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
   lv_label_set_text(s_proc_label, buf);
}

/* Dynamic list y: when the processing row is hidden, the list slides
 * up by PROC_H+8 px so we don't waste ~64 px of vertical real estate
 * showing an empty placeholder.  Called whenever proc_paint_state
 * toggles the row's visibility. */
static void notes_relayout_list(bool proc_visible) {
   if (!s_list) return;
   int y_with = TOPBAR_H + 8 + FILTER_H + PROC_H + 8;
   int y_without = TOPBAR_H + 8 + FILTER_H + 4;
   int y = proc_visible ? y_with : y_without;
   lv_obj_set_pos(s_list, 0, y);
   lv_obj_set_size(s_list, SW, OVERLAY_H - y - 4);
}

static void proc_paint_state(const dict_event_t *e) {
   if (!s_proc_row || !s_proc_orb || !s_proc_label) return;
   if (!e || e->state == DICT_IDLE) {
      lv_obj_add_flag(s_proc_row, LV_OBJ_FLAG_HIDDEN);
      if (s_proc_rec_ticker) {
         lv_timer_del(s_proc_rec_ticker);
         s_proc_rec_ticker = NULL;
      }
      notes_relayout_list(false);
      return;
   }
   notes_relayout_list(true);
   lv_obj_clear_flag(s_proc_row, LV_OBJ_FLAG_HIDDEN);
   /* mini-orb hue tracks pipeline state, mirroring the heroic orb */
   uint32_t body_hex = 0xE74C3C;
   uint32_t edge_hex = 0xFF5C50;
   const char *txt = "";
   char buf[40];
   switch (e->state) {
      case DICT_RECORDING: {
         body_hex = 0xE74C3C;
         edge_hex = 0xFF5C50;
         uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
         uint32_t dur_ms = (e->started_ms && now_ms >= e->started_ms) ? (now_ms - e->started_ms) : 0;
         uint32_t s = dur_ms / 1000;
         snprintf(buf, sizeof(buf), "RECORDING  %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
         txt = buf;
         if (!s_proc_rec_ticker) s_proc_rec_ticker = lv_timer_create(proc_rec_tick_cb, 200, NULL);
         break;
      }
      case DICT_UPLOADING:
         body_hex = 0xF59E0B;
         edge_hex = 0xFCD34D;
         txt = "UPLOADING";
         break;
      case DICT_TRANSCRIBING:
         body_hex = 0xF59E0B;
         edge_hex = 0xFCD34D;
         txt = "TRANSCRIBING";
         break;
      case DICT_SAVED:
         body_hex = 0x22C55E;
         edge_hex = 0x4ADE80;
         txt = "SAVED";
         break;
      case DICT_FAILED:
         body_hex = 0xE74C3C;
         edge_hex = 0xFF5C50;
         txt = "FAILED  TAP TO RETRY";
         break;
      default:
         break;
   }
   if (e->state != DICT_RECORDING && s_proc_rec_ticker) {
      lv_timer_del(s_proc_rec_ticker);
      s_proc_rec_ticker = NULL;
   }
   lv_obj_set_style_bg_color(s_proc_orb, lv_color_hex(body_hex), 0);
   lv_obj_set_style_border_color(s_proc_orb, lv_color_hex(edge_hex), 0);
   lv_label_set_text(s_proc_label, txt);
}

static void proc_pipeline_cb(const dict_event_t *event, void *user_data) {
   (void)user_data;
   if (s_destroying) return;
   proc_paint_state(event);
}

static void cb_proc_close_tap(lv_event_t *e) {
   (void)e;
   dict_event_t cur = voice_dictation_get();
   if (cur.state == DICT_RECORDING) {
      voice_cancel();
      voice_dictation_set_state(DICT_FAILED, DICT_FAIL_CANCELLED, (uint32_t)(esp_timer_get_time() / 1000));
   } else if (cur.state != DICT_IDLE) {
      /* For non-RECORDING non-IDLE (UPLOADING/TRANSCRIBING/SAVED/FAILED),
       * just dismiss the row by snapping back to IDLE.  The dictation
       * itself can't really be cancelled mid-transcribe, but the row
       * shouldn't be sticky in the user's face. */
      voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, (uint32_t)(esp_timer_get_time() / 1000));
   }
}

/* Topbar magnifier tap → toggle search ta visibility.  When showing,
 * also pop the keyboard so the user can type immediately. */
void cb_topbar_search_tap(lv_event_t *e) {
   (void)e;
   if (!s_search_ta) return;
   if (lv_obj_has_flag(s_search_ta, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_clear_flag(s_search_ta, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(s_search_ta);
      ui_keyboard_show(s_search_ta);
   } else {
      ui_keyboard_hide();
      lv_obj_add_flag(s_search_ta, LV_OBJ_FLAG_HIDDEN);
      /* Clear filter when collapsing so the list returns to full view. */
      if (lv_textarea_get_text(s_search_ta)) {
         lv_textarea_set_text(s_search_ta, "");
         s_search_text[0] = '\0';
         refresh_list();
      }
   }
}

/* Topbar pencil tap → open the existing text-input area (kept around
 * for typed notes).  Mirrors the legacy cb_new_text path. */
void cb_topbar_text_tap(lv_event_t *e) {
   (void)e;
   /* Re-uses cb_new_text logic via a synthetic event.  Cleaner than
    * forward-declaring the impl. */
   extern void cb_new_text(lv_event_t * e);
   cb_new_text(NULL);
}

void cb_filter_pill_tap(lv_event_t *e) {
   lv_obj_t *p = lv_event_get_target(e);
   int idx = (int)(uintptr_t)lv_obj_get_user_data(p);
   if (idx < 0 || idx > 3) return;
   if ((int)s_filter == idx) return; /* no-op when tapping the active pill */
   s_filter = (note_filter_t)idx;
   ui_notes_paint_filter_pills();
   refresh_list();
}

/* ── Refresh list ───────────────────────────────────────── */
static void refresh_list(void)
{
    /* #170: hard bail if the screen was destroyed between the call being
     * scheduled (often via lv_async_call from the background transcription
     * task) and execution.  All s_* pointers below may be dangling. */
    if (s_destroying || !s_screen) return;
    /* Count failed entries up-front so we can update both the topbar
     * meta and the conditional CLEAR FAILED button in one pass. */
    int failed_count = 0;
    for (int i = 0; i < MAX_NOTES; i++) {
       if (s_notes[i].used && s_notes[i].state == NOTE_STATE_FAILED) failed_count++;
    }
    /* v5 topbar meta — always shows "N NOTES" in muted gray; the FAIL
     * count is surfaced by the conditional CLEAR FAILED button so we
     * don't duplicate the same number twice on the same row. */
    if (s_topbar_meta) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%d \xe2\x80\xa2 %s",
                 s_note_count, s_note_count == 1 ? "NOTE" : "NOTES");
        lv_label_set_text(s_topbar_meta, buf);
        lv_obj_set_style_text_color(s_topbar_meta, lv_color_hex(0x6A6A72), 0);
    }
    if (s_topbar_clear) {
       if (failed_count > 0) {
          /* Reflect the live count on the button label so the user
           * sees "CLEAR 11 FAILED" instead of a static caption. */
          lv_obj_t *lbl = lv_obj_get_child(s_topbar_clear, 0);
          if (lbl) {
             char b[24];
             /* Compact badge: warning + count.  Red bg + border on the
              * chip telegraphs "failed"; the leading exclamation mark
              * adds urgency without depending on a unicode dot glyph
              * that the caption font may not carry. */
             snprintf(b, sizeof(b), "! %d", failed_count);
             lv_label_set_text(lbl, b);
          }
          lv_obj_clear_flag(s_topbar_clear, LV_OBJ_FLAG_HIDDEN);
       } else {
          lv_obj_add_flag(s_topbar_clear, LV_OBJ_FLAG_HIDDEN);
       }
    }
    if (!s_list) return;
    lv_obj_clean(s_list);

    int shown = 0;
    /* PR 3: emit day-section headers as we walk the (already-reverse-
     * chronological) notes ring.  A header is emitted the first time
     * we see a row whose classify_note_day() bucket differs from the
     * previous header.  Tracks the last emitted bucket via cur_section
     * with -1 as the sentinel for "no header emitted yet". */
    int cur_section = -1;
    for (int i = 0; i < MAX_NOTES && shown < s_note_count; i++) {
        int idx = (s_next_slot - 1 - i + MAX_NOTES) % MAX_NOTES;
        if (!s_notes[idx].used) continue;
        /* M2: Search filter — skip notes that don't match search text */
        if (s_search_text[0]) {
            /* N1: Case-insensitive search */
            if (!strcasestr(s_notes[idx].text, s_search_text)) continue;
        }
        /* PR 3: filter pill predicate */
        if (!note_matches_filter(&s_notes[idx])) continue;
        /* PR 3: emit a day-section header when entering a new bucket. */
        day_section_t sec = classify_note_day(&s_notes[idx]);
        if ((int)sec != cur_section) {
           add_day_section_header(s_list, sec);
           cur_section = (int)sec;
        }
        add_note_card_sectioned(s_list, &s_notes[idx], idx, sec);
        shown++;
    }
    if (shown == 0) {
       const char *head = NULL;
       const char *body = NULL;
       if (s_search_text[0]) {
          head = "No matches";
          body = "Try a different word.";
       } else {
          switch (s_filter) {
             case NOTE_FILTER_VOICE:
                head = "No voice notes yet";
                body = "Tap the amber mic to dictate one.";
                break;
             case NOTE_FILTER_TEXT:
                head = "No typed notes yet";
                body = "Tap the pencil to write one.";
                break;
             case NOTE_FILTER_PENDING:
                head = "Nothing pending";
                body = "Reminders + lists will gather here.";
                break;
             case NOTE_FILTER_ALL:
             default:
                head = "No notes yet";
                body = "Tap the amber mic to dictate,\nor the pencil to type.";
                break;
          }
       }
       lv_obj_t *empty = lv_obj_create(s_list);
       lv_obj_remove_style_all(empty);
       lv_obj_set_size(empty, SW, 220);
       lv_obj_set_style_bg_opa(empty, LV_OPA_TRANSP, 0);
       lv_obj_clear_flag(empty, LV_OBJ_FLAG_SCROLLABLE);
       lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
       lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
       lv_obj_set_style_pad_top(empty, 80, 0);
       lv_obj_set_style_pad_row(empty, 10, 0);

       lv_obj_t *h = lv_label_create(empty);
       lv_label_set_text(h, head);
       lv_obj_set_style_text_color(h, lv_color_hex(0xC8C8D2), 0);
       lv_obj_set_style_text_font(h, FONT_HEADING, 0);
       lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

       lv_obj_t *b = lv_label_create(empty);
       lv_label_set_text(b, body);
       lv_obj_set_style_text_color(b, lv_color_hex(0x6A6A72), 0);
       lv_obj_set_style_text_font(b, FONT_BODY, 0);
       lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
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

    /* Count/size meta — kept allocated for backwards-compat with
     * refresh_list which still sets the text, but hidden by default.
     * The topbar real estate is now used by the magnifier + pencil
     * icons (created in ui_notes_create alongside the rest of the
     * cleanup pass).  refresh_list's write to a hidden label is a
     * no-op; if we want to surface the count later we can flip the
     * hidden flag without re-plumbing. */
    lv_obj_t *meta = lv_label_create(tb);
    lv_label_set_text(meta, "");
    lv_obj_add_flag(meta, LV_OBJ_FLAG_HIDDEN);
    s_topbar_meta = meta;

    /* Clear-failed badge — tiny red chip with just the count, sits
     * immediately to the right of the "Notes" title.  Was a chunky
     * "CLEAR 2 FAILED" pill hogging the topbar centre; the per-row
     * × buttons already let you delete individual failed notes, so
     * this bulk action only needs to be discoverable, not loud.
     * Hidden when failed_count == 0; refresh_list sets the label
     * to "● N" with the live count. */
    lv_obj_t *clear_btn = lv_button_create(tb);
    lv_obj_set_size(clear_btn, LV_SIZE_CONTENT, 28);
    lv_obj_align(clear_btn, LV_ALIGN_LEFT_MID, 130, 0);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_bg_opa(clear_btn, LV_OPA_20, 0);
    lv_obj_set_style_border_width(clear_btn, 1, 0);
    lv_obj_set_style_border_color(clear_btn, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_opa(clear_btn, LV_OPA_50, 0);
    lv_obj_set_style_radius(clear_btn, 14, 0);
    lv_obj_set_style_pad_hor(clear_btn, 10, 0);
    lv_obj_set_style_pad_ver(clear_btn, 0, 0);
    lv_obj_set_style_shadow_width(clear_btn, 0, 0);
    lv_obj_add_event_cb(clear_btn, cb_clear_failed, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, "0");
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(clear_lbl, FONT_CAPTION, 0);
    lv_obj_set_style_text_letter_space(clear_lbl, 0, 0);
    lv_obj_center(clear_lbl);
    lv_obj_add_flag(clear_btn, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_list */
    s_topbar_clear = clear_btn;

    return tb;
}

/* ── Screen create/destroy ─────────────────────────────── */
lv_obj_t *ui_notes_create(void)
{
    /* TT #328 Wave 10 — show persistent home button as escape hatch. */
    extern void ui_chrome_set_home_visible(bool visible);
    ui_chrome_set_home_visible(true);
    /* TT #328 Wave 11 follow-up — hide error banner. */
    extern void ui_home_set_error_banner_visible(bool);
    ui_home_set_error_banner_visible(false);
    /* TT #328 Wave 13 — sync /screen current. */
    extern void tab5_debug_set_nav_target(const char *);
    tab5_debug_set_nav_target("notes");

    /* #250 fix: notes is an overlay parented to home, not a separate
     * lv_screen.  When /navigate goes camera→notes, async_navigate calls
     * ui_camera_destroy() (lv_obj_delete on the active screen) before
     * us; if we don't reload home, disp->act_scr stays dangling and the
     * next render fires lv_obj_update_layout(NULL) at lv_obj_pos.c:304.
     * Same guard ui_chat / ui_memory / ui_agents / ui_focus / ui_sessions
     * already have. */
    {
        extern lv_obj_t *ui_home_get_screen(void);
        lv_obj_t *home = ui_home_get_screen();
        if (home && lv_screen_active() != home) {
            lv_screen_load(home);
        }
    }

    /* #170: we're (re-)entering a live notes screen — clear the guard
     * so the next refresh_list can run.  Must happen BEFORE the
     * already-exists early-return because hide→show goes through this
     * function too. */
    s_destroying = false;
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
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_GESTURE_BUBBLE);

    make_topbar(s_screen);

/* Cleanup pass (post-PR-3): the BTN_ROW_H+SEARCH_H stack (legacy
 * "+ NEW VOICE NOTE" / "+ NEW TEXT NOTE" buttons + always-on
 * search bar) is gone.  Voice entry → amber FAB bottom-right;
 * text entry → pencil icon in the topbar; search → magnifier
 * icon in the topbar that expands a slim collapsible ta below
 * the filter pills.  Net: list gained ~130 px of vertical
 * real estate and the screen has one clear primary action. */
#define SEARCH_H 48
   s_search_ta = lv_textarea_create(s_screen);
   lv_obj_set_size(s_search_ta, SW - 32, SEARCH_H);
   lv_obj_set_pos(s_search_ta, 16, TOPBAR_H + 4);
   lv_textarea_set_one_line(s_search_ta, true);
   lv_textarea_set_placeholder_text(s_search_ta, LV_SYMBOL_LOOP " search what you wrote...");
   lv_obj_set_style_bg_color(s_search_ta, lv_color_hex(COL_CARD), 0);
   lv_obj_set_style_text_color(s_search_ta, lv_color_hex(COL_WHITE), 0);
   lv_obj_set_style_text_font(s_search_ta, FONT_SECONDARY, 0);
   lv_obj_set_style_border_width(s_search_ta, 1, 0);
   lv_obj_set_style_border_color(s_search_ta, lv_color_hex(COL_BORDER), 0);
   lv_obj_set_style_radius(s_search_ta, 8, 0);
   lv_obj_set_style_pad_left(s_search_ta, 16, 0);
   lv_obj_set_style_text_color(s_search_ta, lv_color_hex(COL_LABEL3), LV_PART_TEXTAREA_PLACEHOLDER);
   lv_obj_add_flag(s_search_ta, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
   if (s_search_text[0]) lv_textarea_set_text(s_search_ta, s_search_text);
   lv_obj_add_event_cb(s_search_ta, cb_search_tap, LV_EVENT_CLICKED, NULL);
   lv_obj_add_event_cb(s_search_ta, cb_search_changed, LV_EVENT_VALUE_CHANGED, NULL);
   /* Collapsed by default — toggled by the topbar magnifier icon. */
   lv_obj_add_flag(s_search_ta, LV_OBJ_FLAG_HIDDEN);

   /* Topbar pencil: text-note entry.  The magnifier search icon was
    * removed because LVGL ships no proper magnifying-glass glyph —
    * LV_SYMBOL_LOOP renders as a refresh-arrow which read more like
    * "retry" than "search" and confused the affordance.  Search is
    * a secondary action; the filter pills already cover the primary
    * "find by type" navigation.  We keep cb_topbar_search_tap +
    * the collapsed search ta around so a future, glyph-aware
    * iteration can re-introduce search without re-plumbing. */
   {
      extern void cb_topbar_text_tap(lv_event_t * e);
      lv_obj_t *pen = lv_label_create(s_screen);
      lv_label_set_text(pen, LV_SYMBOL_EDIT);
      lv_obj_set_style_text_color(pen, lv_color_hex(COL_AMBER), 0);
      lv_obj_set_style_text_font(pen, FONT_HEADING, 0);
      lv_obj_align(pen, LV_ALIGN_TOP_RIGHT, -24, 10);
      lv_obj_add_flag(pen, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_ext_click_area(pen, 18);
      lv_obj_add_event_cb(pen, cb_topbar_text_tap, LV_EVENT_CLICKED, NULL);
   }

   /* PR 3: filter-pill row sits at the top of the content area (no
    * btn-row above it anymore — voice = FAB, text = topbar pencil).
    * 44 px tall; four equal-width pills with 8 px gaps; active pill
    * gets amber background, inactive pills are dark-pill style. */
   s_filter_row = lv_obj_create(s_screen);
   lv_obj_remove_style_all(s_filter_row);
   lv_obj_set_size(s_filter_row, SW, FILTER_H);
   lv_obj_set_pos(s_filter_row, 0, TOPBAR_H + 8);
   lv_obj_set_style_pad_hor(s_filter_row, 16, 0);
   lv_obj_set_style_pad_top(s_filter_row, 4, 0);
   lv_obj_set_flex_flow(s_filter_row, LV_FLEX_FLOW_ROW);
   lv_obj_set_style_pad_column(s_filter_row, 8, 0);
   lv_obj_clear_flag(s_filter_row, LV_OBJ_FLAG_SCROLLABLE);
   {
      static const char *PILL_LABELS[4] = {"All", "Voice", "Text", "Pending"};
      int pill_w = (SW - 32 - 24) / 4; /* 24 = 3*8 column gaps */
      for (int i = 0; i < 4; i++) {
         lv_obj_t *p = lv_obj_create(s_filter_row);
         lv_obj_remove_style_all(p);
         lv_obj_set_size(p, pill_w, FILTER_H - 12);
         lv_obj_set_style_radius(p, 18, 0);
         lv_obj_set_style_bg_color(p, lv_color_hex(0x141420), 0);
         lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
         lv_obj_set_style_border_width(p, 1, 0);
         lv_obj_set_style_border_color(p, lv_color_hex(0x262637), 0);
         lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
         lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
         lv_obj_t *lbl = lv_label_create(p);
         lv_label_set_text(lbl, PILL_LABELS[i]);
         lv_obj_set_style_text_color(lbl, lv_color_hex(0xE8E8EF), 0);
         lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
         lv_obj_center(lbl);
         lv_obj_set_user_data(p, (void *)(uintptr_t)i);
         extern void cb_filter_pill_tap(lv_event_t * e);
         lv_obj_add_event_cb(p, cb_filter_pill_tap, LV_EVENT_CLICKED, NULL);
         s_filter_pill[i] = p;
      }
      /* Initial active-pill paint reflecting s_filter (default = ALL). */
      extern void ui_notes_paint_filter_pills(void);
      ui_notes_paint_filter_pills();
   }

/* PR 3: processing row sits at the top of the notes list area.
 * Mini-orb (28 px circle, colored gradient matching heroic orb) +
 * state caption + close ×.  Hidden when pipeline == DICT_IDLE. */
#define PROC_H 56
   s_proc_row = lv_obj_create(s_screen);
   lv_obj_remove_style_all(s_proc_row);
   lv_obj_set_size(s_proc_row, SW - 32, PROC_H);
   lv_obj_set_pos(s_proc_row, 16, TOPBAR_H + 8 + FILTER_H + 4);
   lv_obj_set_style_bg_color(s_proc_row, lv_color_hex(0x141420), 0);
   lv_obj_set_style_bg_opa(s_proc_row, LV_OPA_COVER, 0);
   lv_obj_set_style_border_width(s_proc_row, 1, 0);
   lv_obj_set_style_border_color(s_proc_row, lv_color_hex(0x262637), 0);
   lv_obj_set_style_radius(s_proc_row, 18, 0);
   lv_obj_set_style_pad_hor(s_proc_row, 14, 0);
   lv_obj_clear_flag(s_proc_row, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(s_proc_row, LV_OBJ_FLAG_HIDDEN); /* shown only during pipeline */

   s_proc_orb = lv_obj_create(s_proc_row);
   lv_obj_remove_style_all(s_proc_orb);
   lv_obj_set_size(s_proc_orb, 28, 28);
   lv_obj_align(s_proc_orb, LV_ALIGN_LEFT_MID, 0, 0);
   lv_obj_set_style_radius(s_proc_orb, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_bg_color(s_proc_orb, lv_color_hex(0xE74C3C), 0);
   lv_obj_set_style_bg_opa(s_proc_orb, LV_OPA_COVER, 0);
   lv_obj_set_style_border_width(s_proc_orb, 2, 0);
   lv_obj_set_style_border_color(s_proc_orb, lv_color_hex(0xFF5C50), 0);

   s_proc_label = lv_label_create(s_proc_row);
   lv_label_set_text(s_proc_label, "");
   lv_obj_set_style_text_color(s_proc_label, lv_color_hex(0xE8E8EF), 0);
   lv_obj_set_style_text_font(s_proc_label, FONT_BODY, 0);
   lv_obj_set_style_text_letter_space(s_proc_label, 1, 0);
   lv_obj_align(s_proc_label, LV_ALIGN_LEFT_MID, 40, 0);

   s_proc_close = lv_label_create(s_proc_row);
   lv_label_set_text(s_proc_close, LV_SYMBOL_CLOSE);
   lv_obj_set_style_text_color(s_proc_close, lv_color_hex(0xE8E8EF), 0);
   lv_obj_set_style_text_font(s_proc_close, FONT_BODY, 0);
   lv_obj_align(s_proc_close, LV_ALIGN_RIGHT_MID, 0, 0);
   lv_obj_add_flag(s_proc_close, LV_OBJ_FLAG_CLICKABLE);
   lv_obj_set_ext_click_area(s_proc_close, 12);
   lv_obj_add_event_cb(s_proc_close, cb_proc_close_tap, LV_EVENT_CLICKED, NULL);

   /* Subscribe + rehydrate from current pipeline state.  The static
    * guard prevents double-subscribe across screen recreate cycles. */
   static int s_proc_sub = -1;
   if (s_proc_sub < 0) {
      s_proc_sub = voice_dictation_subscribe_lvgl(proc_pipeline_cb, NULL);
      ESP_LOGI(TAG, "Notes pipeline subscriber registered handle=%d", s_proc_sub);
   }
   {
      dict_event_t cur = voice_dictation_get();
      proc_paint_state(&cur);
   }

   /* Scrollable notes list — gains ~132px from reduced button row + search bar */
   s_list = lv_obj_create(s_screen);
   /* Initial size — proc row starts hidden so list takes the full
    * available vertical real estate.  notes_relayout_list() snaps
    * to the right offset whenever the pipeline state changes. */
   notes_relayout_list(false);
   lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
   lv_obj_set_style_border_width(s_list, 0, 0);
   lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
   lv_obj_set_style_pad_row(s_list, 12, 0);
   lv_obj_set_style_pad_hor(s_list, 16, 0);
   lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ON);

   /* PR 3: amber dictate FAB — bottom-right above the nav bar.  Tap
    * fires voice_start_dictation (the same path the home Dictate chip
    * uses) so a recording started here flows through the pipeline +
    * surfaces on the processing row above. */
   s_fab = lv_obj_create(s_screen);
   lv_obj_remove_style_all(s_fab);
   lv_obj_set_size(s_fab, FAB_SZ, FAB_SZ);
   /* Position the FAB above the global home button (which lv_layer_top
    * paints at the bottom-right corner with a similar circular outline
    * treatment).  Without the extra clearance the two circles read as
    * a single 2-button stack and the user mis-taps.  +20 px breathing
    * room + a smaller halo shadow gives the FAB its own personality
    * without competing for visual weight. */
   lv_obj_set_pos(s_fab, SW - FAB_SZ - 24, USABLE_H - FAB_SZ - 24 - 80);
   lv_obj_set_style_radius(s_fab, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_bg_color(s_fab, lv_color_hex(0xF59E0B), 0);
   lv_obj_set_style_bg_opa(s_fab, LV_OPA_COVER, 0);
   lv_obj_set_style_border_width(s_fab, 0, 0);
   lv_obj_set_style_shadow_width(s_fab, 12, 0);
   lv_obj_set_style_shadow_color(s_fab, lv_color_hex(0xF59E0B), 0);
   lv_obj_set_style_shadow_opa(s_fab, LV_OPA_30, 0);
   lv_obj_set_style_shadow_offset_y(s_fab, 4, 0);
   lv_obj_clear_flag(s_fab, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(s_fab, LV_OBJ_FLAG_CLICKABLE);
   {
      lv_obj_t *fic = lv_label_create(s_fab);
      lv_label_set_text(fic, LV_SYMBOL_AUDIO);
      lv_obj_set_style_text_color(fic, lv_color_hex(0x141420), 0);
      lv_obj_set_style_text_font(fic, FONT_HEADING, 0);
      lv_obj_center(fic);
   }
   extern void cb_notes_fab_tap(lv_event_t * e);
   lv_obj_add_event_cb(s_fab, cb_notes_fab_tap, LV_EVENT_CLICKED, NULL);

   refresh_list();
   ui_keyboard_set_layout_cb(notes_keyboard_layout_cb);

   /* Hide the global keyboard trigger button while on Notes — the
    * pencil icon in the topbar already exposes text entry, so the
    * lv_layer_top trigger button is redundant clutter stacking
    * between the FAB and the home button on the right edge. */
   {
      lv_obj_t *kbd = ui_keyboard_get_trigger_btn();
      if (kbd) lv_obj_add_flag(kbd, LV_OBJ_FLAG_HIDDEN);
   }

   ESP_LOGI(TAG, "Notes screen created, %d notes", s_note_count);
   return s_screen;
}

/* PR 3: amber FAB tap → fires voice_start_dictation.  Same path the home
 * Dictate chip uses; the pipeline subscriber will then drive the
 * processing row at the top of the list. */
void cb_notes_fab_tap(lv_event_t *e) {
   (void)e;
   dict_event_t cur = voice_dictation_get();
   if (cur.state == DICT_RECORDING) {
      /* Already recording — second tap cancels (matches home chip semantics). */
      voice_cancel();
      voice_dictation_set_state(DICT_FAILED, DICT_FAIL_CANCELLED, (uint32_t)(esp_timer_get_time() / 1000));
      return;
   }
   if (cur.state == DICT_FAILED || cur.state == DICT_SAVED) {
      voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, (uint32_t)(esp_timer_get_time() / 1000));
   }
   esp_err_t err = voice_start_dictation();
   if (err != ESP_OK) {
      ESP_LOGW(TAG, "Notes FAB tap: voice_start_dictation failed: %s", esp_err_to_name(err));
   }
}

void ui_notes_destroy(void)
{
    /* #170: latch the destroying flag FIRST so any async refresh_list
     * that's already scheduled on the LVGL async queue sees it and
     * bails before it touches s_list / s_screen / s_topbar_meta. */
    s_destroying = true;
    ui_keyboard_set_layout_cb(NULL);
    hide_recording_indicator();
    hide_input_area();
    /* Restore the global keyboard trigger we hid in ui_notes_create
     * — leaving it hidden would break text entry on screens that
     * rely on the lv_layer_top button (e.g., the Wi-Fi password ta). */
    {
       lv_obj_t *kbd = ui_keyboard_get_trigger_btn();
       if (kbd) lv_obj_clear_flag(kbd, LV_OBJ_FLAG_HIDDEN);
    }
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
    /* Tear down floating UI before hiding the list — same reason as cb_back.
     * Without this, a navigation away from the notes editor via the debug
     * server left the keyboard + preview row stuck on top of the new
     * screen. */
    if (s_edit_overlay) { lv_obj_del(s_edit_overlay); s_edit_overlay = NULL; s_edit_ta = NULL; }
    ui_keyboard_hide();
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

bool ui_notes_is_visible(void)
{
    return s_screen && !lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
}
