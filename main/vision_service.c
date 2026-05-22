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
#include "ui_audio_cues.h" /* V2-A.4 — UI_CUE_INCOMING_HIGH chime on Welcome */
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

/* V2-A.2 tracker — see header for design notes. */
#define VS_TRACK_SLOTS 8
#define VS_TRACK_IOU_THRESH 0.25f               /* IoU floor for association */
#define VS_TRACK_CONFIRM_HITS 3                 /* seen_count to fire ENTER */
#define VS_TRACK_FORGET_MISSES 8                /* ~4 s @ 2 Hz to fire LEAVE */
#define VS_WELCOME_COOLDOWN_MS (5 * 60 * 1000u) /* Nest Hub Max pattern — 5 min */
#define VS_ABSENCE_THRESHOLD_MS (120 * 1000u)   /* min absence before Welcome refires */

typedef struct {
   bool in_use;
   uint32_t id; /* monotonic; unique across lifetime */
   char klass[24];
   float x, y, w, h; /* 320×320 input coords */
   float confidence;
   uint8_t seen_count;
   uint8_t not_seen_count;
   uint64_t first_seen_ms;
   uint64_t last_seen_ms;
   bool confirmed; /* set once seen_count reaches CONFIRM_HITS */
} vs_track_t;

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_lock = NULL;
static vision_service_state_t s_state = {0};
static uint64_t s_boot_ms = 0;
static uint16_t *s_small_buf = NULL;
static uint8_t *s_jpeg_buf = NULL;
static vs_track_t s_tracks[VS_TRACK_SLOTS];
static uint32_t s_next_track_id = 1;

/* Rule cooldown ring — last_fired_ms per built-in rule. */
static uint64_t s_last_welcome_ms = 0;
static uint64_t s_last_person_leave_ms = 0; /* set when a person track fires LEAVE */
static bool s_user_present = false;         /* tracked across confirmed → leave */

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* RGB565 center-crop + downsample to 320×320.  The SC202CS streams a
 * landscape 1280×720 frame.  Downsampling that directly to 320×320
 * squashes the X axis 4× vs Y at 2.25× — yolo11n trained on square
 * aspect rejects the resulting distorted person silhouettes (0
 * detections in live test).  Take the center square (720×720 cropped
 * from 1280×720) then downsample preserving aspect. */
static void downsample_rgb565(const uint16_t *src, int sw, int sh, uint16_t *dst) {
   const int dw = VS_INPUT_W;
   const int dh = VS_INPUT_H;
   /* Take the largest centered square that fits inside (sw × sh). */
   int crop = sw < sh ? sw : sh;
   int x_off = (sw - crop) / 2;
   int y_off = (sh - crop) / 2;
   for (int y = 0; y < dh; y++) {
      int sy = y_off + (y * crop) / dh;
      const uint16_t *srow = src + (size_t)sy * sw;
      uint16_t *drow = dst + (size_t)y * dw;
      for (int x = 0; x < dw; x++) {
         int sx = x_off + (x * crop) / dw;
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
   /* V2-A.3: person-only.  The Pet timeline rule was dropped — yolo11n
    * false-positives dog/cat on humans badly enough that the feature
    * fired ghost-pet events in a single-user-at-desk scenario.  Real
    * pet logging needs a different model (or face/animal-detection
    * sub-program — V7 territory). */
   return klass && strcmp(klass, "person") == 0;
}

/* ── V2-A.2 IoU tracker ──────────────────────────────────────────
 *
 * Single-frame greedy IoU association.  Cheap, deterministic, good
 * enough at 2 Hz.  Track confirmation gates on `seen_count >= 3` so
 * a single noisy frame can't fire a Welcome event. */

static float iou(const vs_track_t *t, const voice_yolo_box_t *b) {
   float ax1 = t->x, ay1 = t->y, ax2 = t->x + t->w, ay2 = t->y + t->h;
   float bx1 = b->x, by1 = b->y, bx2 = b->x + b->w, by2 = b->y + b->h;
   float ix1 = ax1 > bx1 ? ax1 : bx1;
   float iy1 = ay1 > by1 ? ay1 : by1;
   float ix2 = ax2 < bx2 ? ax2 : bx2;
   float iy2 = ay2 < by2 ? ay2 : by2;
   float iw = ix2 > ix1 ? (ix2 - ix1) : 0.0f;
   float ih = iy2 > iy1 ? (iy2 - iy1) : 0.0f;
   float in = iw * ih;
   float ua = (ax2 - ax1) * (ay2 - ay1) + (bx2 - bx1) * (by2 - by1) - in;
   return ua > 0.0f ? in / ua : 0.0f;
}

/* Restore display brightness from NVS user pref.  Called on confirmed
 * person ENTER when away_dim was applied on a prior LEAVE. */
static void restore_brightness(void) {
   if (!tab5_settings_get_away_dim()) return;
   uint8_t pct = tab5_settings_get_brightness();
   extern esp_err_t tab5_display_set_brightness(int percent);
   tab5_display_set_brightness((int)pct);
   tab5_debug_obs_event("vision.presence", "wake");
}

/* Dim display to the away pref.  Called on confirmed person LEAVE.
 * Idempotent — re-firing is harmless. */
static void dim_brightness_for_away(void) {
   if (!tab5_settings_get_away_dim()) return;
   uint8_t pct = tab5_settings_get_away_dim_pct();
   extern esp_err_t tab5_display_set_brightness(int percent);
   tab5_display_set_brightness((int)pct);
   tab5_debug_obs_event("vision.presence", "dim");
}

/* Rules fire on the ENTER edge (seen_count crossing CONFIRM_HITS). */
static void fire_rules_on_enter(const vs_track_t *t) {
   uint64_t now = now_ms();
   char detail[48];
   snprintf(detail, sizeof(detail), "%s id=%u conf=%.2f", t->klass, (unsigned)t->id, t->confidence);
   tab5_debug_obs_event("vision.enter", detail);

   /* Welcome glance — person enters after a REAL absence.
    *
    * V2-A.3 rule: gate on `s_last_person_leave_ms` so we don't fire
    * Welcome when the user was already at their desk at boot.  First
    * boot has no prior LEAVE recorded — don't fire.  After a real
    * LEAVE → away_ms = now - s_last_person_leave_ms; only fire if
    * that's at least VS_ABSENCE_THRESHOLD_MS (120 s). */
   if (strcmp(t->klass, "person") == 0) {
      s_user_present = true;
      restore_brightness(); /* presence-aware screen */

      bool had_real_absence = (s_last_person_leave_ms != 0);
      uint64_t away_ms = had_real_absence ? (now - s_last_person_leave_ms) : 0;
      bool cooldown_ok = (now - s_last_welcome_ms >= VS_WELCOME_COOLDOWN_MS);
      if (had_real_absence && away_ms >= VS_ABSENCE_THRESHOLD_MS && cooldown_ok) {
         s_last_welcome_ms = now;
         s_state.last_welcome_ms = now;
         s_state.welcome_fires_total++;
         extern void ui_home_show_toast(const char *msg);
         ui_home_show_toast("Welcome back");
         /* V2-A.4: pair the toast with the high-priority chime so the
          * return is acknowledged audibly even when the user isn't
          * looking at the home screen at the moment of detection. */
         ui_audio_cue_play(UI_CUE_INCOMING_HIGH);
         char wdetail[64];
         snprintf(wdetail, sizeof(wdetail), "away_ms=%llu", (unsigned long long)away_ms);
         tab5_debug_obs_event("vision.welcome", wdetail);
      }
   }
}

/* LEAVE edge — record the leave timestamp for absence gating + dim
 * the display if presence-aware brightness is enabled. */
static void fire_rules_on_leave(const vs_track_t *t) {
   if (strcmp(t->klass, "person") != 0) return;
   s_user_present = false;
   s_last_person_leave_ms = now_ms();
   dim_brightness_for_away();
}

static void tracker_update(const voice_yolo_box_t *boxes, size_t n) {
   uint64_t now = now_ms();
   bool used[VS_MAX_BOXES] = {false};

   /* Pass 1: associate each existing track with its best-IoU
    * unmatched detection of the same class. */
   for (int s = 0; s < VS_TRACK_SLOTS; s++) {
      if (!s_tracks[s].in_use) continue;
      int best = -1;
      float bi = VS_TRACK_IOU_THRESH;
      for (size_t i = 0; i < n; i++) {
         if (used[i]) continue;
         if (strcmp(boxes[i].klass, s_tracks[s].klass) != 0) continue;
         float v = iou(&s_tracks[s], &boxes[i]);
         if (v > bi) {
            bi = v;
            best = (int)i;
         }
      }
      if (best >= 0) {
         used[best] = true;
         s_tracks[s].x = boxes[best].x;
         s_tracks[s].y = boxes[best].y;
         s_tracks[s].w = boxes[best].w;
         s_tracks[s].h = boxes[best].h;
         s_tracks[s].confidence = boxes[best].confidence;
         s_tracks[s].not_seen_count = 0;
         s_tracks[s].last_seen_ms = now;
         if (s_tracks[s].seen_count < 255) s_tracks[s].seen_count++;
         /* ENTER edge: fire once when crossing the confirm threshold. */
         if (!s_tracks[s].confirmed && s_tracks[s].seen_count >= VS_TRACK_CONFIRM_HITS) {
            s_tracks[s].confirmed = true;
            fire_rules_on_enter(&s_tracks[s]);
         }
      } else {
         /* Not seen this frame. */
         if (s_tracks[s].not_seen_count < 255) s_tracks[s].not_seen_count++;
         if (s_tracks[s].not_seen_count >= VS_TRACK_FORGET_MISSES) {
            if (s_tracks[s].confirmed) {
               char detail[64];
               snprintf(detail, sizeof(detail), "%s id=%u age_ms=%llu", s_tracks[s].klass, (unsigned)s_tracks[s].id,
                        (unsigned long long)(now - s_tracks[s].first_seen_ms));
               tab5_debug_obs_event("vision.leave", detail);
               fire_rules_on_leave(&s_tracks[s]);
            }
            s_tracks[s].in_use = false;
         }
      }
   }

   /* Pass 2: spawn tracks for unmatched detections.  Slot scan is
    * O(N) — cheap at 8 slots. */
   for (size_t i = 0; i < n; i++) {
      if (used[i]) continue;
      for (int s = 0; s < VS_TRACK_SLOTS; s++) {
         if (s_tracks[s].in_use) continue;
         s_tracks[s].in_use = true;
         s_tracks[s].id = s_next_track_id++;
         strlcpy(s_tracks[s].klass, boxes[i].klass, sizeof(s_tracks[s].klass));
         s_tracks[s].x = boxes[i].x;
         s_tracks[s].y = boxes[i].y;
         s_tracks[s].w = boxes[i].w;
         s_tracks[s].h = boxes[i].h;
         s_tracks[s].confidence = boxes[i].confidence;
         s_tracks[s].seen_count = 1;
         s_tracks[s].not_seen_count = 0;
         s_tracks[s].first_seen_ms = now;
         s_tracks[s].last_seen_ms = now;
         s_tracks[s].confirmed = false;
         break;
      }
   }
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
   /* V2-A.3: person-only filter.  Pet timeline was dropped after live
    * testing showed yolo11n's animal false-positives on humans were
    * the only thing the rule ever fired on.  All non-person boxes
    * are silently dropped here so the tracker stays clean. */
   voice_yolo_box_t filtered[VS_MAX_BOXES];
   size_t fn = 0;
   for (size_t i = 0; i < n && fn < VS_MAX_BOXES; i++) {
      if (!is_interesting_class(boxes[i].klass)) continue;
      filtered[fn++] = boxes[i];
      strlcpy(s_state.last_class, boxes[i].klass, sizeof(s_state.last_class));
      s_state.last_detection_ms = now_ms();
      s_state.last_confidence = boxes[i].confidence;
   }
   tracker_update(filtered, fn);
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
      /* V2-A.2: refresh tracker counters for /vision/state. */
      uint8_t act = 0, conf = 0;
      for (int s = 0; s < VS_TRACK_SLOTS; s++) {
         if (s_tracks[s].in_use) {
            act++;
            if (s_tracks[s].confirmed) conf++;
         }
      }
      s_state.active_tracks = act;
      s_state.confirmed_tracks = conf;
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
