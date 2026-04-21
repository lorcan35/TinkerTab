#pragma once

#include "esp_err.h"
#include "lvgl.h"

#define MEDIA_CACHE_SLOTS       5
#define MEDIA_CACHE_MAX_W       660
#define MEDIA_CACHE_MAX_H       440
#define MEDIA_CACHE_SLOT_BYTES  (MEDIA_CACHE_MAX_W * MEDIA_CACHE_MAX_H * 2)

esp_err_t media_cache_init(void);
esp_err_t media_cache_fetch(const char *relative_url, lv_image_dsc_t *out_dsc);
void media_cache_clear(void);

/** Wave 11 stability P1: introspect cache usage for the heap watchdog
 *  telemetry log. `used_slots` is how many of MEDIA_CACHE_SLOTS currently
 *  have a valid decoded image, `resident_kb` is their total byte cost
 *  (slot size × used count, PSRAM-resident).  Caller may pass NULL for
 *  fields it doesn't need. */
void media_cache_stats(int *used_slots, unsigned *resident_kb);
