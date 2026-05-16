/**
 * TinkerTab — LVGL UI Core Implementation
 *
 * Connects LVGL v9 to the Tab5 DPI framebuffer and touch input.
 *
 * Display flush: copies LVGL rendered pixels into the DPI framebuffer
 * at the correct stride offset, then calls esp_cache_msync() so the
 * DMA controller sees the updated data.
 *
 * Touch: reads from tab5_touch_read() and feeds coordinates to LVGL.
 *
 * Threading: a dedicated FreeRTOS task on core 0 runs lv_timer_handler()
 * every 5ms, protected by a mutex for thread-safe access.
 */

#include "ui_core.h"
#include "config.h"
#include "touch.h"
#include "debug_server.h"
#include "imu.h"
#include "settings.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "ui_core";

/* ---- State ---- */
/* C03 Mutex ordering audit (April 2026):
 *
 * System mutexes and their contexts:
 *   1. s_lvgl_mutex (this file) — recursive, protects all LVGL API calls.
 *      Held by: ui_task (Core 0), tab5_ui_lock/try_lock callers.
 *   2. s_ws_mutex (voice.c) — protects WebSocket send/recv.
 *      Held by: voice WS task (Core 1), voice_send_* callers.
 *      Always bounded: 2000ms or 100ms timeout, never portMAX_DELAY.
 *   3. s_state_mutex (voice.c) — protects voice state transitions.
 *      Uses portMAX_DELAY but never acquires s_lvgl_mutex while held.
 *   4. s_play_mutex (voice.c) — protects TTS playback buffer.
 *      Uses portMAX_DELAY but never acquires s_lvgl_mutex while held.
 *
 * ABBA deadlock analysis:
 *   - Debug server (httpd, Core 1) uses tab5_ui_try_lock(2000) with
 *     bounded timeout for screenshots only. Never holds s_ws_mutex.
 *   - No code path holds both s_lvgl_mutex and s_ws_mutex simultaneously.
 *
 * Rule: voice/network/worker code must use lv_async_call() to hop work
 *       to the LVGL thread.  IMPORTANT (#256): lv_async_call is NOT
 *       thread-safe in LVGL 9.x — it calls lv_malloc + lv_timer_create
 *       on the unprotected TLSF heap.  Callers from a non-LVGL thread
 *       MUST take tab5_ui_lock around the lv_async_call.  Without it,
 *       a worker enqueueing while ui_task is allocating draw tasks will
 *       eventually corrupt a TLSF free-list pointer and TWDT in
 *       search_suitable_block. */
static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t          *s_display = NULL;
static lv_indev_t            *s_indev = NULL;
static SemaphoreHandle_t      s_lvgl_mutex = NULL;
static esp_timer_handle_t     s_tick_timer = NULL;
static TaskHandle_t           s_ui_task_handle = NULL;

/* Draw buffer size: 720 pixels wide * 100 lines * 2 bytes per pixel = 144000 bytes */
#define DRAW_BUF_LINES 100
#define DRAW_BUF_SIZE  (TAB5_DISPLAY_WIDTH * DRAW_BUF_LINES * sizeof(uint16_t))

/* ---- FPS counter ---- */
static uint32_t s_frame_count = 0;
static uint32_t s_fps = 0;
static int64_t  s_fps_last_us = 0;

/* ---- Forward declarations ---- */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
static void tick_timer_cb(void *arg);
static void ui_task(void *arg);

/* ========================================================================= */
/*  Display flush callback                                                    */
/* ========================================================================= */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    void *fb = NULL;
    esp_err_t ret = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb);
    if (ret != ESP_OK || fb == NULL) {
        ESP_LOGE(TAG, "Failed to get framebuffer: %s", esp_err_to_name(ret));
        lv_display_flush_ready(disp);
        return;
    }

    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t x2 = area->x2;
    int32_t y2 = area->y2;
    int32_t w = x2 - x1 + 1;
    int32_t stride = TAB5_DISPLAY_WIDTH * sizeof(uint16_t);  /* bytes per row in FB */

    uint8_t *fb8 = (uint8_t *)fb;
    uint8_t *src = px_map;

    for (int32_t y = y1; y <= y2; y++) {
        uint8_t *dst = fb8 + (y * stride) + (x1 * sizeof(uint16_t));
        memcpy(dst, src, w * sizeof(uint16_t));
        src += w * sizeof(uint16_t);
    }

    /* Flush CPU cache to main memory so DMA sees the updated pixels */
    size_t fb_size = TAB5_DISPLAY_WIDTH * TAB5_DISPLAY_HEIGHT * sizeof(uint16_t);
    esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* U21: Count flush calls for FPS measurement */
    s_frame_count++;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_fps_last_us >= 1000000) {  /* Every 1 second */
        s_fps = s_frame_count;
        s_frame_count = 0;
        s_fps_last_us = now_us;
    }

    lv_display_flush_ready(disp);
}

/* ========================================================================= */
/*  Touch input callback                                                      */
/* ========================================================================= */
static uint32_t s_touch_debug_counter = 0;

/* Audit U2 (#206): touch coords need to flip when display is rotated 180°.
 * /touch debug injection is in display-space (the user thinks of the
 * panel as rotated) so it does NOT flip. */
static void touch_apply_rotation(int32_t *x, int32_t *y)
{
    if (!s_display) return;
    lv_display_rotation_t rot = lv_display_get_rotation(s_display);
    if (rot == LV_DISPLAY_ROTATION_180) {
        *x = TAB5_DISPLAY_WIDTH  - 1 - *x;
        *y = TAB5_DISPLAY_HEIGHT - 1 - *y;
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    /* TT #483 (surfaced during W7-F.2 live test pass): when the debug
     * inject is actively in flight, give it priority over any real-touch
     * read.  Pre-#483 we polled real touch first per Wave 2, but
     * capacitive noise from the ST7123 TDDI occasionally registers
     * phantom micro-touches at low strength that masked HTTP injects
     * from the e2e harness — net result was a `/touch` POST returning
     * ok=true while the LVGL switch never received the event.
     *
     * Wave 2's original concern was "real finger during a swipe shouldn't
     * be cancelled by an HTTP inject".  That trade-off lands the other
     * way for the harness use case (the 300 ms tap window has no real
     * finger 99 %+ of the time; phantom-strength noise is what's
     * winning, not deliberate human input).  Keep the seqlock-protected
     * publish (Wave 2's *other* fix) — that's the real correctness
     * invariant, not the priority order.
     *
     * If a future scenario needs human-finger-wins-during-inject again
     * we can gate this by a strength threshold or a temporary
     * `inject_priority` flag on the override reader. */
    int32_t inj_x, inj_y;
    bool inj_pressed;
    if (tab5_debug_touch_override(&inj_x, &inj_y, &inj_pressed)) {
       data->point.x = inj_x;
       data->point.y = inj_y;
       data->state = inj_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
       return;
    }

    /* No inject in flight — read real touch hardware. */
    tab5_touch_point_t points[TAB5_TOUCH_MAX_POINTS];
    uint8_t count = 0;
    bool touched = tab5_touch_read(points, &count);
    if (touched && count > 0) {
        int32_t x = points[0].x;
        int32_t y = points[0].y;
        touch_apply_rotation(&x, &y);
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        if (s_touch_debug_counter < 20) {
            ESP_LOGI(TAG, "TOUCH: x=%d y=%d s=%d", points[0].x, points[0].y, points[0].strength);
            s_touch_debug_counter++;
        }
        return;
    }

    data->state = LV_INDEV_STATE_RELEASED;
}

/* ========================================================================= */
/*  Auto-rotate (audit U2 / #206)                                            */
/* ========================================================================= */
static lv_timer_t *s_autorot_timer = NULL;
static lv_display_rotation_t s_last_applied_rot = LV_DISPLAY_ROTATION_0;

static lv_display_rotation_t orient_to_rotation(tab5_orientation_t o)
{
    /* Tab5's UI is portrait-first; we only flip 180° for upside-down.
     * Landscape orientations are reported by the IMU but we keep the
     * UI in portrait — flipping to landscape here would break every
     * absolute-positioned overlay (Settings, Voice, Mode sheet, ...). */
    return (o == TAB5_ORIENT_PORTRAIT_INV)
           ? LV_DISPLAY_ROTATION_180
           : LV_DISPLAY_ROTATION_0;
}

static void autorot_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_display) return;
    if (!tab5_settings_get_auto_rotate()) return;
    tab5_orientation_t o = tab5_imu_get_orientation();
    lv_display_rotation_t want = orient_to_rotation(o);
    if (want == s_last_applied_rot) return;
    ESP_LOGI(TAG, "Auto-rotate: orientation=%d → applying rotation=%d",
             (int)o, (int)want);
    lv_display_set_rotation(s_display, want);
    s_last_applied_rot = want;
    lv_obj_invalidate(lv_screen_active());
}

void ui_core_apply_auto_rotation(bool enabled)
{
    if (enabled) {
        if (!s_autorot_timer) {
            /* 1 Hz poll — IMU read is cheap (~1 ms) and orientation
             * doesn't change faster than that in normal use. */
            s_autorot_timer = lv_timer_create(autorot_timer_cb, 1000, NULL);
        }
        /* Apply current orientation immediately so the user sees the
         * effect of toggling the switch. */
        autorot_timer_cb(NULL);
    } else if (s_display) {
        /* Switch off: snap back to portrait so the UI is always
         * usable when re-entering Settings. */
        lv_display_set_rotation(s_display, LV_DISPLAY_ROTATION_0);
        s_last_applied_rot = LV_DISPLAY_ROTATION_0;
        lv_obj_invalidate(lv_screen_active());
    }
}

void ui_core_init_auto_rotation_from_nvs(void)
{
    /* Called once after the display is alive so the persisted toggle
     * state takes effect at boot. */
    bool on = tab5_settings_get_auto_rotate() != 0;
    ui_core_apply_auto_rotation(on);
}

/* ========================================================================= */
/*  LVGL tick timer (2ms via esp_timer)                                       */
/* ========================================================================= */
static void tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

/* ========================================================================= */
/*  UI task — runs lv_timer_handler in a loop                                 */
/* ========================================================================= */
static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "UI task started on core %d", xPortGetCoreID());

    /* Register this task with the Task Watchdog Timer.
     * If the LVGL loop gets stuck (deadlock, infinite loop), the WDT
     * will fire a panic with a backtrace — captured in the core dump. */
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add UI task to WDT: %s", esp_err_to_name(wdt_ret));
    } else {
        ESP_LOGI(TAG, "UI task registered with Task Watchdog");
    }

    static int64_t last_heartbeat = 0;

    while (1) {
       /* TT #549: timeout bumped 50 → 100 ms.  With the bigger
        * render budget (CPU 400 MHz + shadow cache + parallel draw)
        * occasional heavy frames can run longer than 50 ms without
        * indicating a real stall — silencing those spurious
        * "mutex timeout" warnings + letting the lock hold longer. */
       if (xSemaphoreTakeRecursive(s_lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          uint32_t time_till_next = lv_timer_handler();
          xSemaphoreGiveRecursive(s_lvgl_mutex);

          /* Feed the watchdog — proves this task is alive and not stuck */
          esp_task_wdt_reset();

          /* Heartbeat log every 5 seconds to detect if task is alive */
          int64_t now = esp_timer_get_time();
          if (now - last_heartbeat > 5000000) {
             ESP_LOGI(TAG, "UI task alive, next=%lu ms, lvgl_fps=%lu", (unsigned long)time_till_next,
                      (unsigned long)s_fps);
             last_heartbeat = now;
          }

          /* Sleep for the time LVGL suggests, clamped to 5-50ms */
          if (time_till_next < 5) time_till_next = 5;
          if (time_till_next > 50) time_till_next = 50;
          vTaskDelay(pdMS_TO_TICKS(time_till_next));
       } else {
          /* Still feed WDT even on mutex timeout — the task isn't stuck,
           * it just couldn't acquire the lock this iteration */
          esp_task_wdt_reset();
          ESP_LOGW(TAG, "UI task: mutex timeout");
          vTaskDelay(pdMS_TO_TICKS(5));
       }
    }
}

/* ========================================================================= */
/*  Public API                                                                */
/* ========================================================================= */

esp_err_t tab5_ui_init(esp_lcd_panel_handle_t panel)
{
    if (panel == NULL) {
        ESP_LOGE(TAG, "Panel handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    s_panel = panel;

    /* Create mutex */
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_lvgl_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize LVGL library */
    lv_init();
    ESP_LOGI(TAG, "LVGL %d.%d.%d initialized", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    /* ---- Create display ---- */
    s_display = lv_display_create(TAB5_DISPLAY_WIDTH, TAB5_DISPLAY_HEIGHT);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_FAIL;
    }

    /* Allocate two draw buffers in PSRAM for double-buffered partial rendering */
    void *buf1 = heap_caps_malloc(DRAW_BUF_SIZE, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(DRAW_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers (%d bytes each)", (int)DRAW_BUF_SIZE);
        if (buf1) heap_caps_free(buf1);
        if (buf2) heap_caps_free(buf2);
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(s_display, buf1, buf2, DRAW_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush_cb);

    ESP_LOGI(TAG, "Display driver: %dx%d, draw bufs %d bytes x2 in PSRAM",
             TAB5_DISPLAY_WIDTH, TAB5_DISPLAY_HEIGHT, (int)DRAW_BUF_SIZE);

    /* ---- Apply TinkerOS dark theme ---- */
    lv_theme_t *theme = lv_theme_default_init(
        s_display,
        lv_color_hex(0xFFB800),  /* primary: amber */
        lv_color_hex(0x00B4D8),  /* secondary: cyan (dragon mode) */
        true,                    /* dark mode */
        &lv_font_montserrat_18
    );
    lv_display_set_theme(s_display, theme);

    /* ---- Create touch input device ---- */
    s_indev = lv_indev_create();
    if (s_indev == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL input device");
        return ESP_FAIL;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);

    ESP_LOGI(TAG, "Touch input device registered");

    /* ---- Start LVGL tick timer (2ms) ---- */
    const esp_timer_create_args_t tick_args = {
        .callback = tick_timer_cb,
        .name = "lvgl_tick",
    };
    esp_err_t ret = esp_timer_create(&tick_args, &s_tick_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LVGL tick timer: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_timer_start_periodic(s_tick_timer, 2000);  /* 2ms = 2000us */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL tick timer: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ---- Start UI task ----
     *
     * Wave 15 W15-C06: the pre-existing comment on this task claimed
     * "(PSRAM)" but the call was plain `xTaskCreatePinnedToCore` which
     * allocates the stack from internal SRAM.  Fulfilling the
     * original intent: use `WithCaps(MALLOC_CAP_SPIRAM)` so the 32 KB
     * stack lives in PSRAM.  That reclaims 32 KB of internal SRAM for
     * LVGL's sub-allocations (labels, sliders, switches — these hit
     * internal on every create).  Settings, the heaviest screen
     * (~55 LVGL objects), was hitting `lv_label_create → NULL` when
     * internal dropped to the single-digit-KB range, then panicking
     * on the next `lv_obj_set_pos`.
     *
     * PSRAM stack latency trade-off: the LVGL render loop pushes
     * pixel buffers through flush callbacks and doesn't allocate
     * deep stack frames; PSRAM reads take a few ns more per access
     * but the overall render rate is unaffected in practice.
     *
     * Falls back to internal-SRAM if PSRAM alloc somehow fails so the
     * device still boots on stripped PSRAM configs. */
    BaseType_t xret = xTaskCreatePinnedToCoreWithCaps(
        ui_task,
        "ui_task",
        32768,
        NULL,
        5,
        &s_ui_task_handle,
        0,
        MALLOC_CAP_SPIRAM);
    if (xret != pdPASS) {
        ESP_LOGW(TAG, "ui_task: PSRAM stack alloc failed — falling back to internal SRAM");
        xret = xTaskCreatePinnedToCore(
            ui_task,
            "ui_task",
            32768,
            NULL,
            5,
            &s_ui_task_handle,
            0);
    }
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UI core initialized — LVGL running on core 0");
    return ESP_OK;
}

void tab5_ui_tick(void)
{
    if (s_lvgl_mutex == NULL) return;
    if (xSemaphoreTakeRecursive(s_lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        lv_timer_handler();
        xSemaphoreGiveRecursive(s_lvgl_mutex);
    }
}

lv_display_t *tab5_ui_get_display(void)
{
    return s_display;
}

void tab5_ui_lock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    }
}

bool tab5_ui_try_lock(uint32_t timeout_ms)
{
    if (!s_lvgl_mutex) return false;
    return xSemaphoreTakeRecursive(s_lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void tab5_ui_unlock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreGiveRecursive(s_lvgl_mutex);
    }
}

/* #258: see header — wraps lv_async_call with the recursive LVGL mutex
 * to close the TLSF race documented in #256/#257. */
lv_result_t tab5_lv_async_call(lv_async_cb_t cb, void *user_data)
{
    tab5_ui_lock();
    lv_result_t r = lv_async_call(cb, user_data);
    tab5_ui_unlock();
    return r;
}

uint32_t ui_core_get_fps(void)
{
    return s_fps;
}

/* TT #328 Wave 5 — per-site tap-debounce ledger.
 *
 * Keyed by interned const char* (string literal); equality is pointer-
 * equality, NOT strcmp.  Callers must pass the SAME string literal at
 * every call site (e.g., always "chat:back").  This makes the lookup
 * O(N_SITES) with no hashing or strdup needed.  N_SITES is small (8
 * tracked clickables fits the firmware's interactive-element budget);
 * overflow LRU-evicts the oldest entry.
 *
 * Why not a hash table or LVGL event-aware solution?  The audit P0
 * called out that pre-Wave-5 only the orb + /navigate had any debounce;
 * the broad fix needed to be one-line opt-in at every CLICKED handler
 * entry.  That argued for the simplest possible primitive.
 */
#define UI_TAP_GATE_SITES 8
typedef struct {
   const char *site; /* interned literal pointer */
   uint32_t last_ms;
} tap_gate_entry_t;
static tap_gate_entry_t s_tap_gate[UI_TAP_GATE_SITES] = {0};
static uint32_t s_tap_gate_seq = 0; /* monotonically increasing tie-breaker for LRU */

bool ui_tap_gate(const char *site, int ms) {
   if (!site || ms <= 0) return true;
   uint32_t now = lv_tick_get();

   /* Search for matching site (pointer equality on interned literals). */
   int free_slot = -1;
   int lru_slot = 0;
   uint32_t lru_age = 0;
   for (int i = 0; i < UI_TAP_GATE_SITES; i++) {
      tap_gate_entry_t *e = &s_tap_gate[i];
      if (e->site == site) {
         if (now - e->last_ms < (uint32_t)ms) {
            ESP_LOGI(TAG, "tap_gate '%s' debounced (dt=%lums)", site, (unsigned long)(now - e->last_ms));
            return false;
         }
         e->last_ms = now;
         return true;
      }
      if (e->site == NULL && free_slot < 0) free_slot = i;
      uint32_t age = now - e->last_ms;
      if (age > lru_age) {
         lru_age = age;
         lru_slot = i;
      }
   }

   /* No match — install new entry, evicting LRU if full. */
   int slot = (free_slot >= 0) ? free_slot : lru_slot;
   s_tap_gate[slot].site = site;
   s_tap_gate[slot].last_ms = now;
   s_tap_gate_seq++;
   return true;
}
