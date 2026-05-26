/*
 * debug_server_camera.c — display + camera capture debug HTTP family.
 *
 * Wave 23b follow-up (#332): eleventh per-family extract.  Owns the
 * three handlers + the shared hardware JPEG encoder they consume:
 *
 *   GET /screenshot       — UI framebuffer JPEG snapshot
 *   GET /screenshot.jpg   — alias of /screenshot (same handler)
 *   GET /camera           — live SC202CS frame as JPEG
 *
 * Pre-extract this all lived inline in debug_server.c, sharing the
 * same s_jpeg_enc + s_jpeg_mux pair.  Moving the JPEG infra with
 * the only callers keeps the encoder ownership coherent and frees
 * debug_server.c of ~285 lines.
 *
 * Same convention as the prior 10 per-family extracts:
 *   check_auth(req)            → tab5_debug_check_auth(req)
 *   send_json_resp(req, root)  → tab5_debug_send_json_resp(req, root)
 */
#include "debug_server_camera.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "camera.h" /* tab5_camera_initialized + capture */
#include "config.h" /* TAB5_DISPLAY_WIDTH / TAB5_DISPLAY_HEIGHT */
#include "debug_server_internal.h"
#include "display.h" /* tab5_display_get_panel */
#include "driver/jpeg_encode.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h" /* TT #721 — low-memory back-pressure */
#include "esp_http_server.h"
#include "esp_lcd_mipi_dsi.h" /* esp_lcd_dpi_panel_get_frame_buffer */
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/atomic.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "task_worker.h"    /* tab5_worker_enqueue */
#include "ui_core.h"        /* tab5_ui_try_lock / tab5_ui_unlock */
#include "vision_service.h" /* V2-A.1 — /vision/state */

static const char *TAG = "debug_camera";

#define check_auth(req) tab5_debug_check_auth(req)
#define send_json_resp(req, root) tab5_debug_send_json_resp(req, root)

/* Display-framebuffer geometry mirrors debug_server.c's FB_* defines —
 * kept colocated with the only consumer that snapshots the FB. */
#define FB_W TAB5_DISPLAY_WIDTH
#define FB_H TAB5_DISPLAY_HEIGHT
#define FB_BPP 2 /* RGB565 */

/* Lazily-initialised hardware JPEG encoder. One engine is reused across
 * requests, serialised by s_jpeg_mux because jpeg_encoder_process is not
 * safe to share across concurrent callers. */
static jpeg_encoder_handle_t s_jpeg_enc = NULL;
static SemaphoreHandle_t s_jpeg_mux = NULL;

static esp_err_t ensure_jpeg_encoder(void) {
   if (s_jpeg_enc) return ESP_OK;
   if (!s_jpeg_mux) {
      s_jpeg_mux = xSemaphoreCreateMutex();
      if (!s_jpeg_mux) return ESP_ERR_NO_MEM;
   }
   jpeg_encode_engine_cfg_t cfg = {
       .intr_priority = 0,
       .timeout_ms = 5000,
   };
   esp_err_t ret = jpeg_new_encoder_engine(&cfg, &s_jpeg_enc);
   if (ret != ESP_OK) {
      ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(ret));
      s_jpeg_enc = NULL;
   }
   return ret;
}

esp_err_t debug_server_camera_init_jpeg(void) { return ensure_jpeg_encoder(); }

/* #74: atomic busy-guard + async dispatch.
 *
 * Two layers of protection so /screenshot can't wedge the debug
 * server while it encodes + sends:
 *
 *   1. CAS busy flag: a second concurrent /screenshot request returns
 *      429 immediately instead of queueing.  Cheap, race-free.
 *
 *   2. Async handler: the entire encode + send happens on a spawned
 *      task via httpd_req_async_handler_begin, so the dispatch worker
 *      is freed within microseconds.  Other requests (/info /touch
 *      /heap /navigate) keep flowing even while a screenshot is mid-
 *      send over a slow WiFi link.  Dedicated task is fine because
 *      the busy-guard ensures at most one screenshot task ever exists.
 *
 *   3. send_wait_timeout dropped 90→15 s in tab5_debug_server_start.
 *      So a stuck client gets dropped fast, not in 90 s.
 */
static volatile uint32_t s_screenshot_busy = 0;
static esp_err_t screenshot_handler_inner(httpd_req_t *req);

static void screenshot_async_task(void *arg) {
   httpd_req_t *async_req = (httpd_req_t *)arg;
   if (async_req) {
      screenshot_handler_inner(async_req);
      httpd_req_async_handler_complete(async_req);
   }
   /* Release the busy flag here, AFTER complete, so a follow-up
    * client can't squeeze in while the previous send is still
    * draining the kernel buffer. */
   Atomic_Decrement_u32(&s_screenshot_busy);
   /* #254: returns instead of vTaskSuspend(NULL).  This runs on the
    * shared tab5_worker now (see screenshot_handler), so the worker
    * task picks up the next job. */
}

static esp_err_t screenshot_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   /* TT #721: low-memory back-pressure.  The HW JPEG encode is a heavy
    * DMA-capable-SRAM consumer; running it when internal/DMA SRAM is
    * critically low starves the SDIO WiFi driver (network-death) and can
    * NULL-crash the encoder under exhaustion.  Refuse with 503 so the caller
    * (e.g. the e2e harness) retries instead of driving the device toward a
    * crash loop.  Debug-only path — never hit in normal use. */
   if (heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) < (24 * 1024)) {
      httpd_resp_set_status(req, "503 Service Unavailable");
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"low_memory\",\"hint\":\"internal SRAM low — retry shortly\"}");
      return ESP_OK;
   }
   if (Atomic_CompareAndSwap_u32(&s_screenshot_busy, 1, 0) != ATOMIC_COMPARE_AND_SWAP_SUCCESS) {
      httpd_resp_set_status(req, "429 Too Many Requests");
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req,
                         "{\"error\":\"screenshot_in_flight\","
                         "\"hint\":\"retry after current encode completes (~200-500ms)\"}");
      return ESP_OK;
   }
   /* Detach the request from the dispatch worker.  The kernel-side
    * socket is "checked out" to us; the worker returns to picking up
    * the next request immediately. */
   httpd_req_t *async_req = NULL;
   esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
   if (err != ESP_OK) {
      Atomic_Decrement_u32(&s_screenshot_busy);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async handler begin failed");
      return ESP_FAIL;
   }
   /* #254: hand the JPEG encode + send to the shared tab5_worker.
    * Was a per-call xTaskCreate + vTaskSuspend(NULL) — even with
    * #247's WithCaps(SPIRAM) it accumulated ~870 B of internal-SRAM
    * task-list bookkeeping per call (+1 task per /screenshot).  The
    * busy-CAS above guarantees only one screenshot job is ever
    * queued, so we don't need a dedicated screenshot task.  Inline
    * fallback if the worker queue is full. */
   if (tab5_worker_enqueue(screenshot_async_task, async_req, "screenshot_async") != ESP_OK) {
      ESP_LOGW(TAG, "screenshot worker enqueue failed; running inline");
      screenshot_handler_inner(async_req);
      httpd_req_async_handler_complete(async_req);
      Atomic_Decrement_u32(&s_screenshot_busy);
   }
   return ESP_OK;
}

static esp_err_t screenshot_handler_inner(httpd_req_t *req) {
   esp_lcd_panel_handle_t panel = tab5_display_get_panel();
   if (!panel) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Display not initialized");
      return ESP_FAIL;
   }

   void *fb = NULL;
   esp_err_t ret = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb);
   if (ret != ESP_OK || !fb) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot get framebuffer");
      return ESP_FAIL;
   }

   if (ensure_jpeg_encoder() != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encoder init failed");
      return ESP_FAIL;
   }

   size_t fb_size = FB_W * FB_H * FB_BPP;

   /* DMA-aligned input buffer for the JPEG engine. */
   jpeg_encode_memory_alloc_cfg_t in_alloc = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
   size_t in_capacity = 0;
   uint8_t *in_buf = jpeg_alloc_encoder_mem(fb_size, &in_alloc, &in_capacity);
   if (!in_buf || in_capacity < fb_size) {
      if (in_buf) free(in_buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG input alloc failed");
      return ESP_FAIL;
   }

   /* Output buffer — 256 KB is plenty for a 720x1280 UI screenshot at q80
    * (measured ~60-120 KB for dark-UI content). */
   const size_t out_cap = 256 * 1024;
   jpeg_encode_memory_alloc_cfg_t out_alloc = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
   size_t out_capacity = 0;
   uint8_t *out_buf = jpeg_alloc_encoder_mem(out_cap, &out_alloc, &out_capacity);
   if (!out_buf) {
      free(in_buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG output alloc failed");
      return ESP_FAIL;
   }

   /* HW03: Lock LVGL for the copy, unlock before encode (encode is long
    * but operates on our copy, so LVGL is free to keep rendering). */
   if (!tab5_ui_try_lock(2000)) {
      ESP_LOGW(TAG, "Screenshot: LVGL lock timeout (2s) — returning 503");
      free(in_buf);
      free(out_buf);
      httpd_resp_set_status(req, "503 Service Unavailable");
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"display busy — LVGL lock timeout\"}");
      return ESP_OK;
   }
   esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
   memcpy(in_buf, fb, fb_size);
   tab5_ui_unlock();

   /* Encode. Serialize concurrent /screenshot calls — the engine is not
    * safe to share across simultaneous jpeg_encoder_process invocations. */
   jpeg_encode_cfg_t enc_cfg = {
       .height = FB_H,
       .width = FB_W,
       .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
       .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
       .image_quality = 80,
   };
   uint32_t out_size = 0;
   xSemaphoreTake(s_jpeg_mux, portMAX_DELAY);
   ret = jpeg_encoder_process(s_jpeg_enc, &enc_cfg, in_buf, fb_size, out_buf, out_capacity, &out_size);
   xSemaphoreGive(s_jpeg_mux);

   free(in_buf);

   if (ret != ESP_OK || out_size == 0) {
      ESP_LOGE(TAG, "jpeg_encoder_process: %s out=%u", esp_err_to_name(ret), (unsigned)out_size);
      free(out_buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encode failed");
      return ESP_FAIL;
   }

   httpd_resp_set_type(req, "image/jpeg");
   httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

   /* Typical payload 60-150 KB — fits comfortably in a single send with
    * the 32 KB LWIP window. No chunking needed. */
   ret = httpd_resp_send(req, (const char *)out_buf, out_size);
   free(out_buf);
   return ret;
}

static esp_err_t camera_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   if (!tab5_camera_initialized()) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"camera not initialized\"}");
      return ESP_OK;
   }

   tab5_cam_frame_t frame;
   esp_err_t err = tab5_camera_capture(&frame);
   if (err != ESP_OK) {
      httpd_resp_set_type(req, "application/json");
      char buf[128];
      snprintf(buf, sizeof(buf), "{\"error\":\"capture failed: %s\"}", esp_err_to_name(err));
      httpd_resp_sendstr(req, buf);
      return ESP_OK;
   }

   /* #148: was ~1.8 MB RGB565 BMP — now JPEG-encoded via the same
    * hardware engine as /screenshot (~40-80 KB typical).  Only the
    * RGB565 format path supports encoding; other formats fall back
    * to a clear error so callers can diagnose. */
   if (frame.format != TAB5_CAM_FMT_RGB565) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"unsupported camera format (expected RGB565)\"}");
      return ESP_OK;
   }
   if (ensure_jpeg_encoder() != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encoder init failed");
      return ESP_FAIL;
   }

   size_t fb_size = (size_t)frame.width * frame.height * 2;

   jpeg_encode_memory_alloc_cfg_t in_alloc = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
   size_t in_capacity = 0;
   uint8_t *in_buf = jpeg_alloc_encoder_mem(fb_size, &in_alloc, &in_capacity);
   if (!in_buf || in_capacity < fb_size) {
      if (in_buf) free(in_buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG input alloc failed");
      return ESP_FAIL;
   }
   memcpy(in_buf, frame.data, fb_size);

   const size_t out_cap = 256 * 1024;
   jpeg_encode_memory_alloc_cfg_t out_alloc = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
   size_t out_capacity = 0;
   uint8_t *out_buf = jpeg_alloc_encoder_mem(out_cap, &out_alloc, &out_capacity);
   if (!out_buf) {
      free(in_buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG output alloc failed");
      return ESP_FAIL;
   }

   jpeg_encode_cfg_t enc_cfg = {
       .height = frame.height,
       .width = frame.width,
       .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
       .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
       .image_quality = 80,
   };
   uint32_t out_size = 0;
   xSemaphoreTake(s_jpeg_mux, portMAX_DELAY);
   err = jpeg_encoder_process(s_jpeg_enc, &enc_cfg, in_buf, fb_size, out_buf, out_capacity, &out_size);
   xSemaphoreGive(s_jpeg_mux);
   free(in_buf);

   if (err != ESP_OK || out_size == 0) {
      ESP_LOGE(TAG, "camera jpeg_encoder_process: %s out=%u", esp_err_to_name(err), (unsigned)out_size);
      free(out_buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JPEG encode failed");
      return ESP_FAIL;
   }

   httpd_resp_set_type(req, "image/jpeg");
   httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   err = httpd_resp_send(req, (const char *)out_buf, out_size);
   free(out_buf);
   return err;
}

/* Vision V2-A.1 (TT #674) — GET /vision/state */
static esp_err_t vision_state_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;
   vision_service_state_t st;
   vision_service_get_state(&st);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "enabled", st.enabled);
   cJSON_AddNumberToObject(root, "rate_hz", st.rate_hz);
   cJSON_AddNumberToObject(root, "frames_processed", st.frames_processed);
   cJSON_AddNumberToObject(root, "frames_yielded", st.frames_yielded);
   cJSON_AddNumberToObject(root, "detections_total", st.detections_total);
   cJSON_AddNumberToObject(root, "uptime_ms", (double)st.uptime_ms);
   cJSON_AddStringToObject(root, "last_class", st.last_class);
   cJSON_AddNumberToObject(root, "last_detection_ms", (double)st.last_detection_ms);
   cJSON_AddNumberToObject(root, "last_confidence", st.last_confidence);
   /* V2-A.2 tracker. */
   cJSON_AddNumberToObject(root, "active_tracks", st.active_tracks);
   cJSON_AddNumberToObject(root, "confirmed_tracks", st.confirmed_tracks);
   cJSON_AddNumberToObject(root, "last_welcome_ms", (double)st.last_welcome_ms);
   cJSON_AddNumberToObject(root, "welcome_fires_total", st.welcome_fires_total);
   /* Surface upstream prereqs so the harness can correlate "frames=0
    * after 6 s enabled" → either K144 USB isn't up, or yolo isn't
    * armed, or the camera failed init. */
   extern bool tab5_camera_initialized(void);
   extern bool voice_yolo_is_ready(void);
   cJSON_AddBoolToObject(root, "camera_ready", tab5_camera_initialized());
   cJSON_AddBoolToObject(root, "yolo_ready", voice_yolo_is_ready());
   return tab5_debug_send_json_resp(req, root);
}

/* V2-A.4 (TT #682) — POST /vision/test_welcome: force-fire Welcome
 * glance bypassing the absence + cooldown gates.  Surface for the
 * e2e harness + manual testing. */
static esp_err_t vision_test_welcome_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;
   vision_service_fire_welcome_test();
   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "fired", true);
   return tab5_debug_send_json_resp(req, root);
}

/* Diagnostic — fire UI_CUE_MODE_SWITCH directly from the HTTP path
 * so we can A/B whether the cue path works at all from non-touch
 * contexts.  Used to debug the Welcome-cue-silent regression. */
static esp_err_t debug_cue_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;
   extern void ui_audio_cue_play(int id);
   ui_audio_cue_play(0); /* UI_CUE_MODE_SWITCH */
   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "fired", true);
   return tab5_debug_send_json_resp(req, root);
}

/* Diagnostic — fire the raw `tab5_audio_test_tone` path that the
 * Volume slider release in Settings uses.  This bypasses the worker
 * queue + cue PCM cache entirely and writes a synthesized triangle
 * wave directly to esp_codec_dev_write.  If THIS doesn't play, the
 * speaker itself is the problem. */
static esp_err_t debug_tone_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;
   extern esp_err_t tab5_audio_test_tone(uint32_t freq_hz, uint32_t duration_ms);
   esp_err_t r = tab5_audio_test_tone(440, 400);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "fired", true);
   cJSON_AddNumberToObject(root, "err", r);
   return tab5_debug_send_json_resp(req, root);
}

void debug_server_camera_register(httpd_handle_t server) {
   if (!server) return;

   static const httpd_uri_t uri_screenshot = {.uri = "/screenshot", .method = HTTP_GET, .handler = screenshot_handler};
   /* #148: /screenshot.bmp alias removed — the handler always returns
    * JPEG (hardware JPEG encoder), so the .bmp URL was lying to callers.
    * Use /screenshot or /screenshot.jpg. */
   static const httpd_uri_t uri_screenshot_jpg = {
       .uri = "/screenshot.jpg", .method = HTTP_GET, .handler = screenshot_handler};
   static const httpd_uri_t uri_camera = {.uri = "/camera", .method = HTTP_GET, .handler = camera_handler};
   static const httpd_uri_t uri_vision_state = {
       .uri = "/vision/state", .method = HTTP_GET, .handler = vision_state_handler};
   /* V2-A.4: harness / manual test path — force-fire Welcome bypassing
    * absence + cooldown gates so we can verify the chime + toast end
    * to end without walking away for >2 min every time. */
   static const httpd_uri_t uri_vision_test_welcome = {
       .uri = "/vision/test_welcome", .method = HTTP_POST, .handler = vision_test_welcome_handler};
   static const httpd_uri_t uri_debug_cue = {
       .uri = "/audio/test_cue", .method = HTTP_POST, .handler = debug_cue_handler};
   static const httpd_uri_t uri_debug_tone = {
       .uri = "/audio/test_tone", .method = HTTP_POST, .handler = debug_tone_handler};

   httpd_register_uri_handler(server, &uri_screenshot);
   httpd_register_uri_handler(server, &uri_screenshot_jpg);
   httpd_register_uri_handler(server, &uri_camera);
   httpd_register_uri_handler(server, &uri_vision_state);
   httpd_register_uri_handler(server, &uri_vision_test_welcome);
   httpd_register_uri_handler(server, &uri_debug_cue);
   httpd_register_uri_handler(server, &uri_debug_tone);
}
