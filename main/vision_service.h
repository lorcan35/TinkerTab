/**
 * @file vision_service.h
 * @brief Background always-on vision service — Vision V2-A.1 (TT #674).
 *
 * Runs YOLO11n at a user-configurable rate when enabled.  Captures
 * frames from the shared SC202CS sensor, downscales to 320×320,
 * encodes via the HW JPEG engine, and dispatches to K144 over the
 * existing voice_yolo + USB-FFS path.
 *
 * This first slice ships the scaffold + continuous detection only.
 * The full plan (vision_service + object tracker + DSL evaluator +
 * built-in rules + notification surface) lands across V2-A.2 .. V2-A.4.
 *
 * Architecture:
 *   - Single FreeRTOS task with PSRAM-backed stack
 *   - Wakes at NVS `vision_rate` Hz when NVS `vision_on` is true
 *   - Yields when ui_camera has DETECT toggled on (avoid K144
 *     USB-FFS contention with the foreground YOLO loop)
 *   - Emits obs events ("vision.detect") for high-confidence
 *     person / dog / cat detections
 *   - Exposes service state for `GET /vision/state`
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Initialize the background vision task.  Call once at boot, after
 *  tab5_settings_init and voice_yolo_init (camera HW must already be
 *  alive).  Task remains idle until vision_on is set true via NVS or
 *  the runtime setter. */
esp_err_t vision_service_init(void);

/** Runtime enable/disable.  Persists to NVS `vision_on` so the state
 *  survives reboot.  Safe to call from any task. */
esp_err_t vision_service_set_enabled(bool enabled);

/** Snapshot of the service state — populated for `GET /vision/state`. */
typedef struct {
   bool enabled;
   uint8_t rate_hz;
   uint32_t frames_processed;
   uint32_t frames_yielded;    /* skipped because ui_camera was busy */
   uint32_t detections_total;  /* boxes returned, summed across frames */
   uint64_t uptime_ms;         /* ms since service init */
   char last_class[24];        /* class name of most recent detection */
   uint64_t last_detection_ms; /* uptime_ms at last detection */
   float last_confidence;
   /* V2-A.2 tracker metrics. */
   uint8_t active_tracks;        /* tracks currently in flight (any seen_count) */
   uint8_t confirmed_tracks;     /* subset that crossed CONFIRM_HITS */
   uint64_t last_welcome_ms;     /* uptime_ms at last Welcome glance fire */
   uint32_t welcome_fires_total; /* lifetime Welcome glance count */
} vision_service_state_t;

void vision_service_get_state(vision_service_state_t *out);

/** Force-fire the Welcome glance rule for harness + manual testing.
 *  Bypasses all gates (absence threshold, cooldown, person-class
 *  requirement).  Used by `POST /vision/test_welcome`.  Returns
 *  ESP_OK after the toast + chime + obs event are dispatched. */
esp_err_t vision_service_fire_welcome_test(void);
