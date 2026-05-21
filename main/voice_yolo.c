/**
 * @file voice_yolo.c
 * @brief Implementation — see voice_yolo.h.
 *
 * Architecture:
 *
 *    voice_yolo_infer ─► base64 encode ─► JSON wrap ─► voice_xport_send
 *                                          (USB bulk OUT via ffs.control)
 *    voice_xport_recv ◄── drain ◄── line-buffer ◄── parse delta JSON
 *                                                    ──► out_boxes[]
 *
 * All buffers are PSRAM-backed (encoded base64 + JSON wrapper are ~40 KB
 * for a typical 320×320 quality-80 frame).
 *
 * TT #621 W6.
 */

#include "voice_yolo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "voice_usb_ffs.h" /* TT #621 W6 — video channel APIs */

static const char *TAG = "voice_yolo";

#define SETUP_TIMEOUT_MS 5000
#define YOLO_LINE_MAX 4096
#define WORK_ID_MAX 32

/* Persistent workspace sizes — yolo inputs are 320×320 JPEGs, which
 * compress to ~30 KB at quality 80 and base64 to ~40 KB.  Drain buffer
 * sized for 8-10 detection lines + ack noise. */
#define INFER_RESP_CAP (8 * 1024)

#define BUF_CAP_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static SemaphoreHandle_t s_lock = NULL;
static char s_work_id[WORK_ID_MAX] = "";
static bool s_ready = false;

/* Pre-allocated PSRAM workspaces.  Reused across every inference call
 * so the hot path doesn't churn the PSRAM allocator + contend with
 * Wi-Fi DMA, which was flapping the connection on yolo bursts. */
static char *s_resp_buf = NULL;  /* INFER_RESP_CAP, PSRAM */
static char *s_setup_buf = NULL; /* YOLO_LINE_MAX, PSRAM — setup ack drain */

static void ensure_lock(void) {
   if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static esp_err_t ensure_workspaces(void) {
   if (!s_resp_buf) {
      s_resp_buf = heap_caps_malloc(INFER_RESP_CAP, BUF_CAP_PSRAM);
      if (!s_resp_buf) return ESP_ERR_NO_MEM;
   }
   if (!s_setup_buf) {
      s_setup_buf = heap_caps_malloc(YOLO_LINE_MAX, BUF_CAP_PSRAM);
      if (!s_setup_buf) return ESP_ERR_NO_MEM;
   }
   return ESP_OK;
}

/* Random 8-char hex id for request_id deduplication on K144 side. */
static void mk_request_id(char out[24]) {
   uint32_t r = esp_random();
   snprintf(out, 24, "tt_%08lx", (unsigned long)r);
}

/* Drain incoming bytes into @p buf until the absolute @p total_timeout_ms
 * budget elapses OR the buffer fills.  Unlike a quiet-window drain this
 * is robust against ext_pcm/asr acks streaming in continuously on the
 * shared xport — we just read up to the deadline and let the caller
 * filter by request_id. */
static size_t drain_into(char *buf, size_t cap, uint32_t total_timeout_ms) {
   size_t got = 0;
   int64_t deadline_us = esp_timer_get_time() + (int64_t)total_timeout_ms * 1000;
   for (;;) {
      int64_t now_us = esp_timer_get_time();
      if (now_us >= deadline_us) break;
      uint32_t remaining_ms = (uint32_t)((deadline_us - now_us) / 1000);
      uint32_t poll_ms = remaining_ms < 50 ? remaining_ms : 50;
      int n = voice_usb_ffs_video_recv(buf + got, cap - got - 1, poll_ms);
      if (n > 0) {
         got += (size_t)n;
         if (got >= cap - 1) break;
      }
   }
   buf[got] = '\0';
   return got;
}

/* Send a JSON request via the dedicated video bulk pair.  Caller must
 * hold voice_usb_ffs_video_lock + s_lock.  Appends the newline framing
 * the StackFlow JSON parser expects. */
static esp_err_t send_json_raw(const char *json, size_t len) {
   int sent = voice_usb_ffs_video_send(json, len);
   if (sent < 0 || (size_t)sent != len) {
      ESP_LOGW(TAG, "send_json: wrote %d/%u", sent, (unsigned)len);
      return ESP_FAIL;
   }
   const char nl = '\n';
   (void)voice_usb_ffs_video_send(&nl, 1);
   return ESP_OK;
}

/* Send one StackFlow JSON request, drain response, find the line whose
 * request_id matches ours (ignoring interleaved ext_pcm/asr acks), and
 * return its error code.  Caller must hold voice_xport_lock. */
static int send_action(const char *action, const char *work_id_in, const char *object, cJSON *data, char *out_wid,
                       size_t out_wid_cap) {
   char rid[24];
   mk_request_id(rid);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "request_id", rid);
   cJSON_AddStringToObject(root, "work_id", work_id_in);
   cJSON_AddStringToObject(root, "action", action);
   if (object) cJSON_AddStringToObject(root, "object", object);
   if (data) cJSON_AddItemToObject(root, "data", data);

   char *payload = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);

   voice_usb_ffs_video_flush(); /* drop any stale bytes */
   esp_err_t err = send_json_raw(payload, strlen(payload));
   free(payload);
   if (err != ESP_OK) return -1000;

   char *resp = s_setup_buf;
   if (!resp) return -1001;
   size_t got = drain_into(resp, YOLO_LINE_MAX, SETUP_TIMEOUT_MS);
   int code = -1002;
   if (got > 0) {
      char *line = resp, *next;
      while (line && *line) {
         next = strchr(line, '\n');
         if (next) *next++ = '\0';
         cJSON *obj = cJSON_Parse(line);
         if (obj) {
            cJSON *recv_rid = cJSON_GetObjectItem(obj, "request_id");
            if (cJSON_IsString(recv_rid) && recv_rid->valuestring && strcmp(recv_rid->valuestring, rid) == 0) {
               cJSON *err_obj = cJSON_GetObjectItem(obj, "error");
               code =
                   err_obj && cJSON_GetObjectItem(err_obj, "code") ? cJSON_GetObjectItem(err_obj, "code")->valueint : 0;
               cJSON *wid = cJSON_GetObjectItem(obj, "work_id");
               if (out_wid && cJSON_IsString(wid) && wid->valuestring) {
                  strlcpy(out_wid, wid->valuestring, out_wid_cap);
               }
               cJSON_Delete(obj);
               break; /* matched — stop scanning */
            }
            cJSON_Delete(obj);
         }
         line = next;
      }
   }
   /* s_setup_buf is persistent — no free here */
   return code;
}

/* On a fresh K144 the yolo task pool is empty.  Earlier dev-box probes
 * and pre-flash test sessions leak slots, so we may hit code=-21 "task
 * full".  Recovery: issue a yolo unit reset and retry. */
static int try_setup(char *out_wid, size_t out_wid_cap) {
   cJSON *data = cJSON_CreateObject();
   cJSON_AddStringToObject(data, "model", "yolo11n");
   cJSON_AddStringToObject(data, "response_format", "yolo.box.stream");
   cJSON_AddStringToObject(data, "input", "yolo.jpeg.base64");
   cJSON_AddBoolToObject(data, "enoutput", true);
   return send_action("setup", "yolo", "yolo.setup", data, out_wid, out_wid_cap);
}

esp_err_t voice_yolo_init(void) {
   ensure_lock();
   xSemaphoreTake(s_lock, portMAX_DELAY);
   if (s_ready) {
      xSemaphoreGive(s_lock);
      return ESP_OK;
   }
   if (ensure_workspaces() != ESP_OK) {
      ESP_LOGE(TAG, "workspace alloc failed");
      xSemaphoreGive(s_lock);
      return ESP_ERR_NO_MEM;
   }
   if (!voice_usb_ffs_video_is_connected()) {
      ESP_LOGW(TAG, "ffs.video not connected");
      xSemaphoreGive(s_lock);
      return ESP_ERR_INVALID_STATE;
   }
   if (voice_usb_ffs_video_lock(2000) != ESP_OK) {
      ESP_LOGW(TAG, "video lock timeout");
      xSemaphoreGive(s_lock);
      return ESP_ERR_TIMEOUT;
   }

   /* K144's yolo unit needs the NPU model loaded on first setup.  Cold
    * start can take several hundred ms — and a fresh K144 may have
    * leftover work_ids from prior dev-box probes that hit code=-21.
    * Try up to 4 times: each failure issues a yolo unit reset and waits
    * progressively longer before retrying. */
   char wid[WORK_ID_MAX] = "";
   int code = -1;
   const uint32_t backoff_ms[] = {400, 800, 1500, 2500};
   for (int attempt = 0; attempt < 4; attempt++) {
      wid[0] = '\0';
      code = try_setup(wid, sizeof(wid));
      if (code == 0 && wid[0] && strncmp(wid, "yolo.", 5) == 0) break;

      ESP_LOGW(TAG, "setup attempt %d/4: code=%d wid='%s' — reset + wait %lu ms", attempt + 1, code, wid,
               (unsigned long)backoff_ms[attempt]);
      (void)send_action("reset", "yolo", NULL, NULL, NULL, 0);
      vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
   }

   voice_usb_ffs_video_unlock();

   if (code != 0 || wid[0] == '\0' || strncmp(wid, "yolo.", 5) != 0) {
      ESP_LOGE(TAG, "setup failed after 4 attempts: code=%d wid='%s'", code, wid);
      xSemaphoreGive(s_lock);
      return ESP_ERR_INVALID_RESPONSE;
   }
   strlcpy(s_work_id, wid, sizeof(s_work_id));
   s_ready = true;
   ESP_LOGI(TAG, "ready, work_id=%s", s_work_id);
   xSemaphoreGive(s_lock);
   return ESP_OK;
}

bool voice_yolo_is_ready(void) { return s_ready; }

/* TT #629 Wave C.2 (R12b): K144 sys.reset reboots the StackFlow daemon
 * which invalidates every cached work_id on Tab5.  Re-running
 * yolo.inference with a stale work_id returns "invalid handle" + can
 * confuse the daemon's slot tracking.  Called from voice_onboard.c on
 * the m5.reset:recovered edge so the next yolo_infer reissues
 * yolo.setup against a fresh work_id. */
void voice_yolo_invalidate(void) {
   if (s_ready) {
      ESP_LOGI(TAG, "voice_yolo_invalidate: clearing work_id=%s after K144 reset", s_work_id);
   }
   s_work_id[0] = '\0';
   s_ready = false;
}

/* Parse a single yolo.box.stream JSON line, append box if present.
 * Skips lines whose request_id doesn't match @p want_rid (filters out
 * ext_pcm/asr ack noise on the shared xport).  Returns 1 if a
 * finish=true marker was seen on a matching line. */
static int parse_stream_line(const char *line, const char *want_rid, voice_yolo_box_t *boxes, size_t max_boxes,
                             size_t *count) {
   cJSON *obj = cJSON_Parse(line);
   if (!obj) return 0;
   int finished = 0;
   cJSON *rid = cJSON_GetObjectItem(obj, "request_id");
   if (!cJSON_IsString(rid) || !rid->valuestring || strcmp(rid->valuestring, want_rid) != 0) {
      cJSON_Delete(obj);
      return 0;
   }
   cJSON *data = cJSON_GetObjectItem(obj, "data");
   if (cJSON_IsObject(data)) {
      cJSON *fin = cJSON_GetObjectItem(data, "finish");
      if (cJSON_IsBool(fin) && cJSON_IsTrue(fin)) finished = 1;

      cJSON *delta = cJSON_GetObjectItem(data, "delta");
      if (cJSON_IsObject(delta) && *count < max_boxes) {
         cJSON *bbox = cJSON_GetObjectItem(delta, "bbox");
         cJSON *kls = cJSON_GetObjectItem(delta, "class");
         cJSON *conf = cJSON_GetObjectItem(delta, "confidence");
         if (cJSON_IsArray(bbox) && cJSON_GetArraySize(bbox) >= 4) {
            voice_yolo_box_t *b = &boxes[*count];
            b->x = (float)atof(cJSON_GetArrayItem(bbox, 0)->valuestring ?: "0");
            b->y = (float)atof(cJSON_GetArrayItem(bbox, 1)->valuestring ?: "0");
            b->w = (float)atof(cJSON_GetArrayItem(bbox, 2)->valuestring ?: "0");
            b->h = (float)atof(cJSON_GetArrayItem(bbox, 3)->valuestring ?: "0");
            b->confidence = cJSON_IsString(conf) && conf->valuestring ? (float)atof(conf->valuestring) : 0.0f;
            strlcpy(b->klass, cJSON_IsString(kls) && kls->valuestring ? kls->valuestring : "?", sizeof(b->klass));
            (*count)++;
         }
      }
   }
   cJSON_Delete(obj);
   return finished;
}

esp_err_t voice_yolo_infer(const void *jpeg, size_t jpeg_len, voice_yolo_box_t *out_boxes, size_t max_boxes,
                           size_t *out_count, uint32_t timeout_ms) {
   if (!s_ready || !jpeg || jpeg_len == 0 || !out_boxes || !out_count) return ESP_ERR_INVALID_ARG;

   *out_count = 0;

   if (ensure_workspaces() != ESP_OK) return ESP_ERR_NO_MEM;

   char rid[24];
   mk_request_id(rid);

   int64_t t0_us = esp_timer_get_time();
   if (voice_usb_ffs_video_lock(2000) != ESP_OK) {
      return ESP_ERR_TIMEOUT;
   }
   voice_usb_ffs_video_flush();

   /* Zero-copy compose: borrow the USB TX buffer and write the JSON
    * envelope + base64-encoded JPEG directly into it.  No intermediate
    * b64/json PSRAM allocations, no extra memcpy through PSRAM. */
   size_t tx_cap = 0;
   uint8_t *tx = (uint8_t *)voice_usb_ffs_video_tx_borrow(&tx_cap);
   if (!tx || tx_cap == 0) {
      voice_usb_ffs_video_unlock();
      return ESP_ERR_INVALID_STATE;
   }

   /* prefix = JSON up to and including the opening data-quote.
    * suffix = trailing close-quote + brace + newline. */
   int prefix_len = snprintf((char *)tx, tx_cap,
                             "{\"request_id\":\"%s\",\"work_id\":\"%s\","
                             "\"action\":\"inference\",\"object\":\"yolo.jpeg.base64\","
                             "\"data\":\"",
                             rid, s_work_id);
   if (prefix_len < 0 || (size_t)prefix_len >= tx_cap) {
      voice_usb_ffs_video_unlock();
      return ESP_ERR_INVALID_ARG;
   }

   /* Worst-case base64 size = 4/3 * jpeg_len rounded up to 4 + NUL. */
   const size_t suffix_len = 3; /* "\"}\n" */
   size_t b64_cap = tx_cap - (size_t)prefix_len - suffix_len - 1;
   size_t b64_len = 0;
   int rc = mbedtls_base64_encode(tx + prefix_len, b64_cap, &b64_len, jpeg, jpeg_len);
   if (rc != 0) {
      ESP_LOGW(TAG, "base64 encode: -0x%x (b64_cap=%u jpeg=%u)", -rc, (unsigned)b64_cap, (unsigned)jpeg_len);
      voice_usb_ffs_video_unlock();
      return ESP_FAIL;
   }

   /* Append closing quote + brace + newline that StackFlow expects. */
   tx[prefix_len + b64_len + 0] = '"';
   tx[prefix_len + b64_len + 1] = '}';
   tx[prefix_len + b64_len + 2] = '\n';
   size_t total = (size_t)prefix_len + b64_len + suffix_len;

   int sent = voice_usb_ffs_video_tx_commit(total);
   if (sent < 0 || (size_t)sent != total) {
      ESP_LOGW(TAG, "tx_commit: wrote %d/%u", sent, (unsigned)total);
      voice_usb_ffs_video_unlock();
      return ESP_FAIL;
   }

   /* Drain streamed yolo.box.stream lines into the persistent
    * s_resp_buf.  Bigger buffer because a busy frame can stream
    * several box lines plus noise. */
   size_t got = drain_into(s_resp_buf, INFER_RESP_CAP, timeout_ms);
   voice_usb_ffs_video_unlock();

   if (got == 0) {
      ESP_LOGW(TAG, "infer: no response");
      return ESP_ERR_TIMEOUT;
   }

   int finished = 0;
   char *line = s_resp_buf, *next;
   while (line && *line && *out_count < max_boxes) {
      next = strchr(line, '\n');
      if (next) *next++ = '\0';
      if (*line) finished |= parse_stream_line(line, rid, out_boxes, max_boxes, out_count);
      line = next;
   }

   int elapsed_ms = (int)((esp_timer_get_time() - t0_us) / 1000);
   ESP_LOGI(TAG, "infer: %u boxes in %d ms (finish=%d)", (unsigned)*out_count, elapsed_ms, finished);
   return ESP_OK;
}
