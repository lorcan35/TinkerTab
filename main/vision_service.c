/**
 * @file vision_service.c
 * @brief Implementation — see vision_service.h.
 *
 * Vision V2-A.1 (TT #674).  Scaffolding-only — runs continuous YOLO
 * but doesn't yet evaluate rules or fire notifications.  Layers
 * (tracker → DSL → rules → notification) ship in V2-A.2 .. V2-A.4.
 */

#include "vision_service.h"

#include <string.h>

#include "camera.h"
#include "debug_obs.h"
#include "driver/jpeg_encode.h" /* jpeg_alloc_encoder_mem for DMA-aligned output */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "ui_camera.h"
#include "voice_video.h"
#include "voice_yolo.h"

static const char *TAG = "vision_svc";

#define POLL_INTERVAL_MS 500u /* config re-read cadence when disabled */
#define MIN_TICK_MS 333u      /* hard floor matches K144 USB-FFS budget */
#define VS_MAX_BOXES 8
#define VS_INPUT_W 320
#define VS_INPUT_H 320
#define VS_JPEG_CAP (24 * 1024)

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_lock = NULL;
static vision_service_state_t s_state = {0};
static uint64_t s_boot_ms = 0;
static uint16_t *s_small_buf = NULL;
static uint8_t *s_jpeg_buf = NULL;

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* RGB565 nearest-neighbour downsample.  Mirrors ui_camera's yolo
 * downsampler but local-only so we don't need to expose it. */
static void downsample_rgb565(const uint16_t *src, int sw, int sh, uint16_t *dst) {
   const int dw = VS_INPUT_W;
   const int dh = VS_INPUT_H;
   for (int y = 0; y < dh; y++) {
      int sy = (y * sh) / dh;
      const uint16_t *srow = src + (size_t)sy * sw;
      uint16_t *drow = dst + (size_t)y * dw;
      for (int x = 0; x < dw; x++) {
         int sx = (x * sw) / dw;
         drow[x] = srow[sx];
      }
   }
}

static esp_err_t ensure_buffers(void) {
   if (!s_small_buf) {
      s_small_buf = heap_caps_malloc((size_t)VS_INPUT_W * VS_INPUT_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!s_small_buf) return ESP_ERR_NO_MEM;
   }
   if (!s_jpeg_buf) {
      /* HW JPEG engine needs a DMA-aligned output buffer.  Plain
       * heap_caps_malloc on PSRAM doesn't guarantee the cache-line
       * alignment the encoder expects — `jpeg_encoder_process` then
       * fails with ESP_ERR_INVALID_ARG (=258) even for valid 320×320
       * inputs.  ui_camera's yolo path does the same. */
      jpeg_encode_memory_alloc_cfg_t mcfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
      size_t actual = 0;
      s_jpeg_buf = (uint8_t *)jpeg_alloc_encoder_mem(VS_JPEG_CAP, &mcfg, &actual);
      if (!s_jpeg_buf) return ESP_ERR_NO_MEM;
   }
   return ESP_OK;
}

static bool is_interesting_class(const char *klass) {
   /* V2-A.1 surfaces only the 3 classes the built-in rules will care
    * about in V2-A.2 (person / dog / cat).  Other COCO classes still
    * count toward detections_total but don't fire obs.  Cheaper than
    * a hash table and avoids surfacing noise. */
   return klass && (strcmp(klass, "person") == 0 || strcmp(klass, "dog") == 0 || strcmp(klass, "cat") == 0);
}

static void process_one_frame(void) {
   tab5_cam_frame_t frame;
   esp_err_t err = tab5_camera_capture(&frame);
   if (err != ESP_OK) {
      /* NOT_FINISHED races against ui_camera's preview tick.  Treat
       * as a yield — try again next tick. */
      s_state.frames_yielded++;
      return;
   }
   if (frame.format != TAB5_CAM_FMT_RGB565) {
      ESP_LOGW(TAG, "frame format %d not RGB565 — skip", (int)frame.format);
      char detail[24];
      snprintf(detail, sizeof(detail), "fmt=%d", (int)frame.format);
      tab5_debug_obs_event("vision.skip", detail);
      return;
   }
   downsample_rgb565((const uint16_t *)frame.data, frame.width, frame.height, s_small_buf);

   uint32_t jpeg_bytes = 0;
   esp_err_t enc_err = voice_video_encode_rgb565((const uint8_t *)s_small_buf, VS_INPUT_W, VS_INPUT_H, 80, s_jpeg_buf,
                                                 VS_JPEG_CAP, &jpeg_bytes);
   if (enc_err != ESP_OK || jpeg_bytes == 0) {
      ESP_LOGD(TAG, "JPEG encode failed (%d, bytes=%u)", (int)enc_err, (unsigned)jpeg_bytes);
      char detail[32];
      snprintf(detail, sizeof(detail), "enc=%d b=%u", (int)enc_err, (unsigned)jpeg_bytes);
      tab5_debug_obs_event("vision.skip", detail);
      return;
   }

   voice_yolo_box_t boxes[VS_MAX_BOXES];
   size_t n = 0;
   esp_err_t i_err = voice_yolo_infer(s_jpeg_buf, jpeg_bytes, boxes, VS_MAX_BOXES, &n, 1500);
   s_state.frames_processed++;
   if (i_err != ESP_OK) {
      ESP_LOGD(TAG, "infer failed: %d", (int)i_err);
      return;
   }
   s_state.detections_total += (uint32_t)n;
   for (size_t i = 0; i < n; i++) {
      if (!is_interesting_class(boxes[i].klass)) continue;
      strlcpy(s_state.last_class, boxes[i].klass, sizeof(s_state.last_class));
      s_state.last_detection_ms = now_ms();
      s_state.last_confidence = boxes[i].confidence;
      char detail[48];
      snprintf(detail, sizeof(detail), "%s %.2f", boxes[i].klass, boxes[i].confidence);
      tab5_debug_obs_event("vision.detect", detail);
   }
}

static void vision_service_task(void *arg) {
   (void)arg;
   ESP_LOGI(TAG, "task started");
   for (;;) {
      bool enabled = tab5_settings_get_vision_on();
      uint8_t rate = tab5_settings_get_vision_rate();
      if (rate < 1) rate = 1;
      if (rate > 3) rate = 3;

      xSemaphoreTake(s_lock, portMAX_DELAY);
      s_state.enabled = enabled;
      s_state.rate_hz = rate;
      s_state.uptime_ms = now_ms() - s_boot_ms;
      xSemaphoreGive(s_lock);

      if (!enabled) {
         vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
         continue;
      }

      /* Yield to foreground YOLO loop on camera screen — avoids
       * USB-FFS contention which Wi-Fi DMA can't tolerate. */
      if (ui_camera_yolo_active()) {
         s_state.frames_yielded++;
         vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
         continue;
      }

      if (!tab5_camera_initialized()) {
         vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
         continue;
      }
      /* Self-arm YOLO when the service is enabled.  voice_yolo_init is
       * idempotent — fast no-op once armed.  Returns INVALID_STATE if
       * K144 USB-FFS isn't connected yet; we just back off and retry. */
      if (!voice_yolo_is_ready()) {
         esp_err_t arm = voice_yolo_init();
         if (arm != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
         }
      }

      if (ensure_buffers() != ESP_OK) {
         ESP_LOGW(TAG, "buffer alloc failed");
         vTaskDelay(pdMS_TO_TICKS(2000));
         continue;
      }

      process_one_frame();

      uint32_t period_ms = 1000u / rate;
      if (period_ms < MIN_TICK_MS) period_ms = MIN_TICK_MS;
      vTaskDelay(pdMS_TO_TICKS(period_ms));
   }
}

esp_err_t vision_service_init(void) {
   if (s_task) return ESP_OK;
   s_lock = xSemaphoreCreateMutex();
   if (!s_lock) return ESP_ERR_NO_MEM;
   s_boot_ms = now_ms();
   /* voice_video_encode_rgb565 requires voice_video_init to have run
    * for the HW JPEG encoder + mutex.  voice.c calls this at boot but
    * we self-init here too — idempotent — to guard against init-order
    * regressions. */
   extern esp_err_t voice_video_init(void);
   voice_video_init();
   /* PSRAM-backed task stack — keeps internal SRAM free for the
    * Wi-Fi + LVGL hot paths. */
   StaticTask_t *task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
   StackType_t *stack_buf = heap_caps_malloc(8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!task_buf || !stack_buf) {
      ESP_LOGE(TAG, "task alloc failed");
      return ESP_ERR_NO_MEM;
   }
   s_task = xTaskCreateStatic(vision_service_task, "vision_svc", 8192, NULL, 5, stack_buf, task_buf);
   if (!s_task) {
      ESP_LOGE(TAG, "task create failed");
      return ESP_FAIL;
   }
   tab5_debug_obs_event("vision.init", "ok");
   return ESP_OK;
}

esp_err_t vision_service_set_enabled(bool enabled) {
   esp_err_t err = tab5_settings_set_vision_on(enabled);
   if (err == ESP_OK) tab5_debug_obs_event("vision.toggle", enabled ? "on" : "off");
   return err;
}

void vision_service_get_state(vision_service_state_t *out) {
   if (!out) return;
   xSemaphoreTake(s_lock, portMAX_DELAY);
   *out = s_state;
   out->uptime_ms = now_ms() - s_boot_ms;
   xSemaphoreGive(s_lock);
}
