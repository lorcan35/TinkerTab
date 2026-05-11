/*
 * voice_messages_sync.c — see voice_messages_sync.h.
 *
 * Wave 3-C-b (cross-stack cohesion audit 2026-05-11).
 */
#include "voice_messages_sync.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h" /* TAB5_VOICE_PORT */
#include "debug_obs.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "settings.h"
#include "task_worker.h"

static const char *TAG = "msg_sync";

/* W3-C-d: SD-backed offline queue.  Each line is a JSON envelope:
 *   {"sid":"<session_id>","body":<the full POST body JSON>}
 * Body is stored as-is so drain doesn't re-cJSON-build.  Cap is in
 * lines (not bytes) — 100 entries at ~1 KB each = ~100 KB headroom.
 * The drop-oldest policy + truncate-on-drain bound the file size. */
/* FAT 8.3 short filename — Tab5 sdkconfig has CONFIG_FATFS_LFN_NONE=y
 * so the basename is capped at 8 chars + 3-char extension. */
#define MSG_SYNC_QUEUE_PATH "/sdcard/msgsync.txt"
#define MSG_SYNC_QUEUE_CAP 100
/* Largest single queue line we'll accept on read.  Matches the SOLO
 * accumulator hard cap (64 KB) + envelope overhead. */
#define MSG_SYNC_QUEUE_LINE_MAX (72 * 1024)

/* Job state — fully self-contained.  Worker reads, no NVS touch. */
typedef struct {
   char session_id[64]; /* for offline-queue write on failure */
   char url[160];       /* http://{host}:3502/api/v1/sessions/{session_id}/messages */
   char auth[160];      /* "Bearer {token}" or "" when no token */
   char *body;          /* PSRAM, free()d by worker */
   size_t body_len;
   /* For obs / log: keep role + a short content prefix.  Avoids
    * leaking secrets if a malformed caller passes a token as content. */
   char role[16];
   char preview[32];
} msg_sync_job_t;

static void free_job(msg_sync_job_t *job) {
   if (!job) return;
   if (job->body) heap_caps_free(job->body);
   heap_caps_free(job);
}

/* Append one envelope line to the offline queue.  Drop-oldest if the
 * file is at MSG_SYNC_QUEUE_CAP entries.  Returns ESP_OK on success.
 *
 * Called only from the worker thread (post_job failure path + drain_job
 * re-queue path) so no mutex needed against itself. */
static esp_err_t queue_append(const char *session_id, const char *body, size_t body_len) {
   /* Count current entries — if at cap, evict the oldest line before
    * appending.  Simple read-tail-only path: read all, drop first
    * line, write the rest + the new line. */
   FILE *f = fopen(MSG_SYNC_QUEUE_PATH, "r");
   size_t lines = 0;
   if (f) {
      int c;
      while ((c = fgetc(f)) != EOF) {
         if (c == '\n') lines++;
      }
      fclose(f);
   }

   if (lines >= MSG_SYNC_QUEUE_CAP) {
      /* Roll the oldest off.  Read all, skip first line, rewrite. */
      char *all = heap_caps_malloc(MSG_SYNC_QUEUE_LINE_MAX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!all) {
         ESP_LOGW(TAG, "queue rotate alloc failed — appending past cap");
      } else {
         f = fopen(MSG_SYNC_QUEUE_PATH, "r");
         if (f) {
            /* Skip oldest line. */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
               /* discard */
            }
            size_t off = 0;
            while ((c = fgetc(f)) != EOF && off < MSG_SYNC_QUEUE_LINE_MAX * 2 - 1) {
               all[off++] = (char)c;
            }
            all[off] = '\0';
            fclose(f);
            FILE *out = fopen(MSG_SYNC_QUEUE_PATH, "w");
            if (out) {
               fwrite(all, 1, off, out);
               fclose(out);
               ESP_LOGI(TAG, "queue rotated: dropped oldest, kept %u bytes", (unsigned)off);
            }
         }
         heap_caps_free(all);
      }
   }

   f = fopen(MSG_SYNC_QUEUE_PATH, "a");
   if (!f) {
      ESP_LOGW(TAG, "queue append: fopen failed — message lost");
      tab5_debug_obs_event("msg_sync.queue_err", "fopen");
      return ESP_FAIL;
   }
   /* Build the envelope with cJSON so the embedded body string is
    * properly escaped.  body itself is JSON; we treat it as an opaque
    * UTF-8 string in the envelope. */
   cJSON *env = cJSON_CreateObject();
   if (!env) {
      fclose(f);
      return ESP_ERR_NO_MEM;
   }
   cJSON_AddStringToObject(env, "sid", session_id);
   cJSON_AddRawToObject(env, "body", body); /* body is already valid JSON */
   char *line = cJSON_PrintUnformatted(env);
   cJSON_Delete(env);
   if (!line) {
      fclose(f);
      return ESP_ERR_NO_MEM;
   }
   fputs(line, f);
   fputc('\n', f);
   fclose(f);
   ESP_LOGI(TAG, "queued offline (%u bytes body)", (unsigned)body_len);
   tab5_debug_obs_event("msg_sync.queued", "ok");
   cJSON_free(line);
   return ESP_OK;
}

/* Internal POST helper used by both the normal path and the drain
 * re-post path.  Returns ESP_OK iff HTTP 2xx.  Does NOT log; caller
 * owns logging + obs events so the two paths can label themselves. */
static esp_err_t do_http_post(const char *url, const char *auth, const char *body, size_t body_len) {
   esp_http_client_config_t cfg = {
       .url = url,
       .method = HTTP_METHOD_POST,
       .timeout_ms = 5000,
       .buffer_size = 2048,
       .buffer_size_tx = 1024,
       .crt_bundle_attach = NULL,
   };
   esp_http_client_handle_t client = esp_http_client_init(&cfg);
   if (!client) return ESP_FAIL;
   esp_http_client_set_header(client, "Content-Type", "application/json");
   if (auth && auth[0]) {
      esp_http_client_set_header(client, "Authorization", auth);
   }
   esp_err_t err = esp_http_client_open(client, (int)body_len);
   if (err != ESP_OK) {
      esp_http_client_cleanup(client);
      return ESP_FAIL;
   }
   int written = esp_http_client_write(client, body, (int)body_len);
   if (written < 0 || (size_t)written != body_len) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return ESP_FAIL;
   }
   esp_http_client_fetch_headers(client);
   int status = esp_http_client_get_status_code(client);
   char drain[256];
   while (esp_http_client_read(client, drain, sizeof drain) > 0) {
      /* discard */
   }
   esp_http_client_close(client);
   esp_http_client_cleanup(client);
   return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

static void msg_sync_post_job(void *arg) {
   msg_sync_job_t *job = (msg_sync_job_t *)arg;
   if (!job) return;

   esp_err_t r = do_http_post(job->url, job->auth, job->body, job->body_len);
   if (r == ESP_OK) {
      ESP_LOGI(TAG, "POST ok role=%s preview=%s", job->role, job->preview);
      char detail[48];
      snprintf(detail, sizeof detail, "role=%s status=ok", job->role);
      tab5_debug_obs_event("msg_sync.ok", detail);
   } else {
      ESP_LOGW(TAG, "POST failed role=%s preview=%s — queuing offline", job->role, job->preview);
      tab5_debug_obs_event("msg_sync.fail", job->role);
      /* W3-C-d: keep the message — append to offline queue so it
       * survives until WS reconnect triggers a drain. */
      queue_append(job->session_id, job->body, job->body_len);
   }
   free_job(job);
}

esp_err_t voice_messages_sync_post(const char *role, const char *content, const char *input_mode) {
   if (!role || !role[0] || !content || !content[0]) {
      return ESP_ERR_INVALID_ARG;
   }
   if (!input_mode || !input_mode[0]) input_mode = "text";

   /* NVS snapshot in caller's thread.  Worker will not touch NVS. */
   char session_id[64] = {0};
   char dragon_host[64] = {0};
   char dragon_tok[96] = {0};
   tab5_settings_get_session_id(session_id, sizeof session_id);
   tab5_settings_get_dragon_host(dragon_host, sizeof dragon_host);
   tab5_settings_get_dragon_api_token(dragon_tok, sizeof dragon_tok);

   if (!session_id[0] || !dragon_host[0]) {
      ESP_LOGD(TAG, "skip: session_id=%s dragon_host=%s", session_id[0] ? "set" : "empty",
               dragon_host[0] ? "set" : "empty");
      return ESP_ERR_INVALID_STATE;
   }

   /* Build JSON body via cJSON so escaping is correct for arbitrary
    * UTF-8 content.  Worker takes ownership of the resulting buffer. */
   cJSON *root = cJSON_CreateObject();
   if (!root) return ESP_ERR_NO_MEM;
   cJSON_AddStringToObject(root, "role", role);
   cJSON_AddStringToObject(root, "content", content);
   cJSON_AddStringToObject(root, "input_mode", input_mode);
   char *body = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!body) return ESP_ERR_NO_MEM;
   size_t body_len = strlen(body);

   /* Re-pack body into PSRAM so cJSON's internal-SRAM allocation can
    * be freed immediately. */
   char *psram_body = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!psram_body) {
      cJSON_free(body);
      return ESP_ERR_NO_MEM;
   }
   memcpy(psram_body, body, body_len + 1);
   cJSON_free(body);

   msg_sync_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!job) {
      heap_caps_free(psram_body);
      return ESP_ERR_NO_MEM;
   }
   /* snprintf forces null-termination and silences -Wstringop-truncation. */
   snprintf(job->session_id, sizeof job->session_id, "%s", session_id);
   snprintf(job->url, sizeof job->url, "http://%s:%d/api/v1/sessions/%s/messages", dragon_host, TAB5_VOICE_PORT,
            session_id);
   if (dragon_tok[0]) {
      snprintf(job->auth, sizeof job->auth, "Bearer %s", dragon_tok);
   }
   job->body = psram_body;
   job->body_len = body_len;
   snprintf(job->role, sizeof job->role, "%s", role);
   /* Short preview for the obs/log — first ~30 chars, ASCII-only safe. */
   size_t prev_n = strlen(content);
   if (prev_n > sizeof(job->preview) - 1) prev_n = sizeof(job->preview) - 1;
   memcpy(job->preview, content, prev_n);
   job->preview[prev_n] = '\0';

   esp_err_t r = tab5_worker_enqueue(msg_sync_post_job, job, "msg_sync");
   if (r != ESP_OK) {
      ESP_LOGW(TAG, "worker queue full — dropping (role=%s)", role);
      tab5_debug_obs_event("msg_sync.drop", "queue_full");
      free_job(job);
   }
   return r;
}

/* ── W3-C-d: offline queue drain ─────────────────────────────────── */

/* Process one queue line (post + on failure re-queue).  Returns ESP_OK
 * iff the entry was successfully POSTed; ESP_FAIL iff it was either
 * unparseable (dropped) or re-queued for the next drain attempt. */
static esp_err_t drain_one_line(const char *line, const char *dragon_host, const char *dragon_tok) {
   cJSON *env = cJSON_Parse(line);
   if (!env) {
      ESP_LOGW(TAG, "drain: skip unparseable line");
      return ESP_FAIL;
   }
   const cJSON *sid_j = cJSON_GetObjectItem(env, "sid");
   const cJSON *body_j = cJSON_GetObjectItem(env, "body");
   if (!cJSON_IsString(sid_j) || sid_j->valuestring[0] == '\0' || body_j == NULL) {
      cJSON_Delete(env);
      ESP_LOGW(TAG, "drain: skip malformed line");
      return ESP_FAIL;
   }
   /* body was written via cJSON_AddRawToObject so it's already a JSON
    * object node; serialise it back to a string for the POST body. */
   char *body_str = cJSON_PrintUnformatted(body_j);
   if (!body_str) {
      cJSON_Delete(env);
      return ESP_FAIL;
   }
   char url[160];
   snprintf(url, sizeof url, "http://%s:%d/api/v1/sessions/%s/messages", dragon_host, TAB5_VOICE_PORT,
            sid_j->valuestring);
   char auth[160] = {0};
   if (dragon_tok && dragon_tok[0]) {
      snprintf(auth, sizeof auth, "Bearer %s", dragon_tok);
   }
   esp_err_t r = do_http_post(url, auth, body_str, strlen(body_str));
   if (r != ESP_OK) {
      /* Re-queue.  body_str is the inner body JSON (without envelope);
       * queue_append wraps it in a fresh envelope. */
      queue_append(sid_j->valuestring, body_str, strlen(body_str));
   }
   cJSON_free(body_str);
   cJSON_Delete(env);
   return r;
}

static void msg_sync_drain_job(void *arg) {
   (void)arg;

   /* Snapshot Dragon host/token once for the whole loop. */
   char dragon_host[64] = {0};
   char dragon_tok[96] = {0};
   tab5_settings_get_dragon_host(dragon_host, sizeof dragon_host);
   tab5_settings_get_dragon_api_token(dragon_tok, sizeof dragon_tok);
   if (!dragon_host[0]) {
      ESP_LOGD(TAG, "drain skip: dragon_host empty");
      return;
   }

   /* Rename-to-snapshot pattern: any concurrent queue_append from
    * the (same) worker thread can't actually race us — the worker
    * is single-threaded — but on reboot mid-drain we want the
    * .processing file to still hold the un-replayed entries.  Move
    * the active queue out of the way so new POST failures during
    * the drain loop hit a fresh /sdcard/msg_queue.jsonl. */
   /* FAT 8.3: drain snapshot uses a fresh 8.3 name (msgsync.bak). */
   const char *processing = "/sdcard/msgsync.bak";
   if (rename(MSG_SYNC_QUEUE_PATH, processing) != 0) {
      /* Either no queue file exists yet, or rename failed.  In
       * either case nothing to drain. */
      ESP_LOGD(TAG, "drain: no snapshot (nothing to do)");
      return;
   }

   FILE *f = fopen(processing, "r");
   if (!f) {
      ESP_LOGW(TAG, "drain: failed to open snapshot");
      remove(processing);
      return;
   }
   char *buf = heap_caps_malloc(MSG_SYNC_QUEUE_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!buf) {
      ESP_LOGE(TAG, "drain: PSRAM line buffer alloc failed");
      fclose(f);
      /* Put the snapshot back so the next drain attempt can pick it up. */
      rename(processing, MSG_SYNC_QUEUE_PATH);
      return;
   }

   tab5_debug_obs_event("msg_sync.drain", "start");

   int ok = 0;
   int fail = 0;
   while (fgets(buf, MSG_SYNC_QUEUE_LINE_MAX, f) != NULL) {
      size_t n = strlen(buf);
      if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
      if (buf[0] == '\0') continue;
      if (drain_one_line(buf, dragon_host, dragon_tok) == ESP_OK) {
         ok++;
      } else {
         fail++;
      }
   }
   fclose(f);
   heap_caps_free(buf);
   /* Snapshot fully processed — re-queued lines (failure path) live
    * in /sdcard/msg_queue.jsonl now, NOT the snapshot.  Remove it. */
   remove(processing);

   char detail[48];
   snprintf(detail, sizeof detail, "ok=%d fail=%d", ok, fail);
   ESP_LOGI(TAG, "drain complete: %s", detail);
   tab5_debug_obs_event("msg_sync.drain", detail);
}

esp_err_t voice_messages_sync_drain(void) {
   esp_err_t r = tab5_worker_enqueue(msg_sync_drain_job, NULL, "msg_drain");
   if (r != ESP_OK) {
      ESP_LOGW(TAG, "drain: worker queue full");
   }
   return r;
}
