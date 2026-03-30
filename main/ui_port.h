/**
 * ui_port.h — TinkerOS UI Platform Portability Layer
 *
 * This header is the ONLY platform-specific include that ui_*.c files need.
 * It provides identical macros on both ESP32-P4 firmware and desktop simulator.
 *
 * Rules:
 *   - ui_*.c files MUST include this header instead of esp_log.h, esp_heap_caps.h,
 *     esp_task_wdt.h, esp_timer.h, or freertos/*.h directly.
 *   - Hardware module headers (camera.h, wifi.h, etc.) are still included directly
 *     because the simulator stubs them via sim/stubs.c.
 *   - No platform #ifdef should appear in ui_*.c — only here.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>

/* ═══════════════════════════════════════════════════════════════════════
 * DESKTOP SIMULATOR (TINKEROS_SIMULATOR defined by sim/CMakeLists.txt)
 * ═══════════════════════════════════════════════════════════════════════ */
#ifdef TINKEROS_SIMULATOR

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* ── Logging ──────────────────────────────────────────────────────── */
#define UI_LOGI(tag, fmt, ...) printf("I [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define UI_LOGE(tag, fmt, ...) fprintf(stderr, "E [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define UI_LOGW(tag, fmt, ...) fprintf(stderr, "W [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define UI_LOGD(tag, fmt, ...) /* debug suppressed */

/* ── Memory — tracked, capped at 32MB to match real PSRAM ───────── */
void  *ui_port_malloc(size_t size, const char *file, int line);
void   ui_port_free(void *ptr);
void  *ui_port_realloc(void *ptr, size_t size, const char *file, int line);
size_t ui_port_heap_used(void);
void   ui_port_heap_print(void);  /* print current heap stats */

#define UI_MALLOC_PSRAM(size) ui_port_malloc((size), __FILE__, __LINE__)
#define UI_FREE(p)            ui_port_free(p)
#define UI_REALLOC(p, size)   ui_port_realloc((p), (size), __FILE__, __LINE__)

/* ── Watchdog — no-op ────────────────────────────────────────────── */
#define UI_WDT_RESET() ((void)0)

/* ── Delay — SDL_Delay on sim, usleep fallback ───────────────────── */
#include <SDL2/SDL.h>
#define UI_DELAY_MS(ms) SDL_Delay(ms)

/* ── Time ────────────────────────────────────────────────────────── */
static inline uint32_t ui_time_ms(void) { return SDL_GetTicks(); }
#define UI_TIME_MS() ui_time_ms()

/* ── Heap query — sim: derived from tracker, firmware: heap_caps ─── */
#define UI_HEAP_FREE_PSRAM() ((size_t)(32UL * 1024 * 1024) - ui_port_heap_used())

/* ── FreeRTOS types — sim port headers shadow the real ones ─────── */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* esp_err_to_name — provided by sim/port/esp_err.h */
#include "esp_err.h"

/* ═══════════════════════════════════════════════════════════════════════
 * ESP32-P4 FIRMWARE
 * ═══════════════════════════════════════════════════════════════════════ */
#else

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Logging ──────────────────────────────────────────────────────── */
#define UI_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define UI_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define UI_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define UI_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)

/* ── Memory — PSRAM-backed via heap_caps ────────────────────────── */
#define UI_MALLOC_PSRAM(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define UI_FREE(p)            heap_caps_free(p)
#define UI_REALLOC(p, size)   heap_caps_realloc((p), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define UI_HEAP_FREE_PSRAM()  heap_caps_get_free_size(MALLOC_CAP_SPIRAM)

/* ── Watchdog ────────────────────────────────────────────────────── */
#define UI_WDT_RESET() esp_task_wdt_reset()

/* ── Delay ───────────────────────────────────────────────────────── */
#define UI_DELAY_MS(ms) vTaskDelay(pdMS_TO_TICKS(ms))

/* ── Time ────────────────────────────────────────────────────────── */
#define UI_TIME_MS() ((uint32_t)(esp_timer_get_time() / 1000ULL))

#endif /* TINKEROS_SIMULATOR */
