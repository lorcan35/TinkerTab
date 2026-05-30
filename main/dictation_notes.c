/* dictation_notes.c — dictation engine extracted from ui_notes.c (W5).
 *
 * Owns the background, non-UI side of dictation notes:
 *   - Dragon REST sync (POST /api/notes)               [first increment]
 *   - SD WAV recording engine (start/stop/write, mutex) [this increment]
 *   - background transcription queue (POST /api/v1/transcribe)
 *   - standalone SD-only mic record task
 *
 * Shares the note store with ui_notes.c via ui_notes_internal.h.  The PUBLIC
 * record/transcribe entry points keep their prototypes in ui_notes.h (so
 * external callers are untouched) — only their definitions live here. */

#include "dictation_notes.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* mkdir / stat for the recordings dir */

#include "audio.h" /* tab5_mic_read (bsp/tab5/audio.h) */
#include "cJSON.h"
#include "config.h" /* TAB5_VOICE_SAMPLE_RATE / TAB5_VOICE_PORT */
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "settings.h"
#include "tab5_rtc.h"
#include "ui_core.h"           /* tab5_lv_async_call */
#include "ui_notes.h"          /* public record/transcribe decls + ui_notes_sync_pending */
#include "ui_notes_internal.h" /* shared store + helpers + shared statics */
#include "voice.h"             /* voice_get_state / voice_get_dictation_title */
#include "voice_dictation.h"   /* dictation FSM */
#include "wifi.h"

static const char *TAG = "dictation_notes";

typedef struct {
   char title[128];
   char text[MAX_NOTE_LEN];
   int note_idx; /* S6: index in s_notes for clearing needs_sync */
} sync_note_args_t;

static void sync_note_to_dragon_task(void *arg) {
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
      ESP_LOGW(TAG, "Note sync failed: err=%s status=%d", esp_err_to_name(err), status);
   }

   esp_http_client_cleanup(client);
   free(json);
   free(a);
   vTaskSuspend(NULL);
}

void sync_note_to_dragon(const char *title, const char *text) {
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
   xTaskCreatePinnedToCore(sync_note_to_dragon_task, "note_sync", 4096, args, 3, NULL, 0);
}

/* S6: Sync all pending notes to Dragon (called on reconnect) */
void ui_notes_sync_pending(void) {
   notes_load();
   int synced = 0;
   for (int i = 0; i < MAX_NOTES; i++) {
      if (s_notes[i].used && s_notes[i].needs_sync && s_notes[i].text[0]) {
         ESP_LOGI(TAG, "Catch-up sync: note %d", i);
         sync_note_to_dragon("", s_notes[i].text);
         synced++;
         vTaskDelay(pdMS_TO_TICKS(500)); /* stagger to avoid flooding */
      }
   }
   if (synced > 0) {
      ESP_LOGI(TAG, "Catch-up sync: %d notes queued", synced);
   }
}

/* ── WAV recording engine ──────────────────────────────────────────────────
 * Records raw PCM 16 kHz mono to a WAV file on the SD card.  The mic capture
 * task (voice.c) and the standalone sd_record_task below call
 * ui_notes_write_audio() for each chunk.  Thread-safe via s_rec_mutex.
 *
 * These statics stay file-static (engine-private); the UI reaches them only
 * through the dictation_* accessors declared in dictation_notes.h. */

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

/* ── UI → engine read accessors (see dictation_notes.h) ── */
bool dictation_recording_active(void) { return s_rec_file != NULL; }
int dictation_recording_slot(void) { return s_rec_note_slot; }
bool dictation_pipeline_armed(void) { return s_pipeline_armed_slot; }
void dictation_pipeline_clear_armed(void) { s_pipeline_armed_slot = false; }

/* WAV header for PCM 16-bit mono */
static void wav_write_header(FILE *f, uint32_t data_bytes, uint16_t sample_rate) {
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

const char *ui_notes_start_recording(void) {
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
   snprintf(s_rec_path, sizeof(s_rec_path), "%s/%04lu.wav", REC_DIR, (unsigned long)s_next_rec_id);
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
   n->year = rtc.year; /* RTC year is already offset from 2000 */
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

void ui_notes_write_audio(const int16_t *samples, size_t count) {
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

void ui_notes_stop_recording(const char *transcript) {
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
   ESP_LOGI(TAG, "Recording stopped: %s (%.1fs, %lu samples)", s_rec_path, duration_s, (unsigned long)s_rec_samples);

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

/* ── Background transcription queue ─────────────────────────────────────────
 * Periodically checks for RECORDED notes, reads the WAV from SD, POSTs to
 * Dragon's /api/v1/transcribe, and updates the note. */
static void transcription_queue_task(void *arg) {
   ESP_LOGI(TAG, "Transcription queue started");
   vTaskDelay(pdMS_TO_TICKS(10000)); /* wait 10s for system to settle */

   while (1) {
      vTaskDelay(pdMS_TO_TICKS(15000)); /* check every 15s */

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
            voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NO_AUDIO, voice_dictation_now_ms());
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

      /* W1: atomically claim the dictation FSM for this offline upload —
       * mints a turn_id, sets origin=OFFLINE + note_slot, transitions to
       * UPLOADING, but ONLY if no turn (e.g. a live WS dictation that began
       * in the gap since the voice_get_state() check above) is in flight.
       * If refused, leave the note RECORDED and retry next tick — this is
       * the atomic compare-and-begin that closes the WS↔offline clobber
       * (S1-3), replacing the old manual set_note_slot + set_state. */
      char off_turn[DICT_TURN_ID_LEN];
      voice_turn_id_gen(off_turn);
      if (!voice_dictation_try_begin_offline(off_turn, slot, voice_dictation_now_ms())) {
         ESP_LOGI(TAG, "Transcription queue: dictation FSM busy — deferring note [%d]", slot);
         continue; /* note stays RECORDED; retry next 15 s tick */
      }
      n->state = NOTE_STATE_TRANSCRIBING;
      if (n->enrich == ENRICH_PENDING) n->enrich = ENRICH_TRANSCRIBING; /* W4: badge advances */
      notes_save();

      /* Read WAV file from SD */
      FILE *f = fopen(n->audio_path, "rb");
      if (!f) {
         ESP_LOGW(TAG, "Cannot open WAV: %s", n->audio_path);
         n->state = NOTE_STATE_FAILED;
         n->fail_reason = NOTE_FAIL_NO_AUDIO;
         voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NO_AUDIO, voice_dictation_now_ms());
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
         voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NO_AUDIO, voice_dictation_now_ms());
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
         if (fseek(f, 40, SEEK_SET) != 0 || fread(&hdr_data_size, 4, 1, f) != 1) {
            ESP_LOGW(TAG, "WAV header read failed for %s — skipping repair", n->audio_path);
         } else if (hdr_data_size == 0 || hdr_data_size > (uint32_t)(file_size - 44)) {
            /* Fix the header in place */
            uint32_t actual_data = (uint32_t)(file_size - 44);
            uint32_t riff_size = actual_data + 36;
            bool ok = (fseek(f, 4, SEEK_SET) == 0) && (fwrite(&riff_size, 4, 1, f) == 1) &&
                      (fseek(f, 40, SEEK_SET) == 0) && (fwrite(&actual_data, 4, 1, f) == 1);
            if (ok) {
               fflush(f);
               ESP_LOGI(TAG, "Fixed WAV header: %lu data bytes", (unsigned long)actual_data);
            } else {
               ESP_LOGW(TAG, "WAV header repair fwrite failed for %s", n->audio_path);
            }
         }
         fseek(f, 0, SEEK_SET);
      }

      /* Build URL: http://<dragon_host>:3502/api/v1/transcribe */
      char dragon_host[64];
      tab5_settings_get_dragon_host(dragon_host, sizeof(dragon_host));
      char url[128];
      snprintf(url, sizeof(url), "http://%s:%d/api/v1/transcribe", dragon_host, TAB5_VOICE_PORT);

      /* HTTP POST — stream from file in 4KB chunks (never hold >4KB in RAM) */
      esp_http_client_config_t http_cfg = {
          .url = url,
          .method = HTTP_METHOD_POST,
          .timeout_ms = 120000, /* 2min — large files take time to upload+transcribe */
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
         voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, voice_dictation_now_ms());
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
         voice_dictation_set_state(DICT_FAILED, DICT_FAIL_NETWORK, voice_dictation_now_ms());
         notes_save();
         continue;
      }

      /* Stream file to HTTP in 4KB chunks */
      char chunk[4096];
      long sent = 0;
      while (sent < file_size) {
         size_t to_read = (file_size - sent > (long)sizeof(chunk)) ? sizeof(chunk) : (size_t)(file_size - sent);
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
      voice_dictation_set_state(DICT_TRANSCRIBING, DICT_FAIL_NONE, voice_dictation_now_ms());

      if (status == 200 && content_len > 0 && content_len < 8192) {
         char *resp = malloc(content_len + 1);
         if (resp) {
            esp_http_client_read(client, resp, content_len);
            resp[content_len] = '\0';

            /* Parse JSON response: {"text":"...", "duration_s":..., "stt_ms":...} */
            cJSON *root = cJSON_Parse(resp);
            if (root) {
               const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
               if (text && text[0]) {
                  strncpy(n->text, text, MAX_NOTE_LEN - 1);
                  n->text[MAX_NOTE_LEN - 1] = '\0';
                  n->state = NOTE_STATE_TRANSCRIBED;
                  n->fail_reason = NOTE_FAIL_NONE;
                  if (n->enrich != ENRICH_NONE) n->enrich = ENRICH_DONE; /* W4: offline auto-finish — badge clears */
                  voice_dictation_set_state(DICT_SAVED, DICT_FAIL_NONE, voice_dictation_now_ms());
                  ESP_LOGI(TAG, "Transcription done [%d]: %.60s", slot, text);
               } else {
                  snprintf(n->text, MAX_NOTE_LEN, "(Empty transcription)");
                  n->state = NOTE_STATE_FAILED;
                  n->fail_reason = NOTE_FAIL_EMPTY;
                  voice_dictation_set_state(DICT_FAILED, DICT_FAIL_EMPTY, voice_dictation_now_ms());
               }
               cJSON_Delete(root);
            }
            free(resp);
         }
      } else {
         ESP_LOGW(TAG, "Transcribe HTTP %d (len=%d)", status, content_len);
         n->state = NOTE_STATE_FAILED;
         n->fail_reason = (status == 401 || status == 403) ? NOTE_FAIL_AUTH : NOTE_FAIL_NETWORK;
         voice_dictation_set_state(DICT_FAILED, (status == 401 || status == 403) ? DICT_FAIL_AUTH : DICT_FAIL_NETWORK,
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

void ui_notes_start_transcription_queue(void) {
   BaseType_t ret =
       xTaskCreatePinnedToCore(transcription_queue_task, "transcribe_q", 16384, /* needs room for HTTP client */
                               NULL, 3, NULL, 1);
   if (ret == pdPASS) {
      ESP_LOGI(TAG, "Transcription queue task created");
   } else {
      ESP_LOGE(TAG, "Failed to create transcription queue task");
   }
}

/* Standalone SD-only recording task — reads mic, writes WAV, no Dragon needed */
static TaskHandle_t s_sd_rec_task = NULL;

static void sd_record_task(void *arg) {
   ESP_LOGI(TAG, "SD recording task started (core %d)", xPortGetCoreID());

   /* Allocate buffers in PSRAM */
   const int tdm_samples = 960 * 4; /* 20ms @ 48kHz, 4 TDM channels */
   const int mono_samples = 320;    /* 20ms @ 16kHz */
   int16_t *tdm_buf = heap_caps_malloc(tdm_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   int16_t *mono_buf = heap_caps_malloc(mono_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
   /* 4-hr hard cap (TT #572).  Mic chunks are 20 ms (50 frames/s) so
    * 14400 s = 720000 frames.  Original 5-min cap was a zombie-task
    * guard (477 s zombie in audit 2026-05-14); bumped to 4 hr so
    * meetings / podcasts / lectures fit while still preventing
    * unbounded recording when the user forgets to stop. */
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
         int32_t sum = tdm_buf[i * 4] + tdm_buf[(i + 1) * 4] + tdm_buf[(i + 2) * 4];
         mono_buf[out_idx++] = (int16_t)(sum / 3);
      }

      ui_notes_write_audio(mono_buf, out_idx);
      frames++;
      if (frames == 1) {
         ESP_LOGI(TAG, "SD rec: first audio chunk written (%d samples)", out_idx);
      }
      if (frames % 250 == 0) { /* every 5 seconds */
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

void dictation_sd_record_start(void) {
   /* Wave 14 W14-H07: stack bumped from 4 KB to 8 KB. The task calls
    * tab5_mic_read (I2S DMA path) then funnels through FATFS/VFS (~2-4 KB of
    * FATFS sector buffers + libc FILE state) plus ESP_LOGI with formatted
    * args. 4 KB trapped stack_chk_fail on long offline recordings (>20 s).
    * 8 KB matches the voice mic task and gives comfortable headroom in
    * PSRAM. */
   s_sd_rec_running = true;
   xTaskCreatePinnedToCore(sd_record_task, "sd_rec", 8192, NULL, 5, &s_sd_rec_task, 1);
}
