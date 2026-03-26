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

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_lcd_mipi_dsi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "ui_core";

/* ---- State ---- */
static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t          *s_display = NULL;
static lv_indev_t            *s_indev = NULL;
static SemaphoreHandle_t      s_lvgl_mutex = NULL;
static esp_timer_handle_t     s_tick_timer = NULL;
static TaskHandle_t           s_ui_task_handle = NULL;

/* Draw buffer size: 720 pixels wide * 100 lines * 2 bytes per pixel = 144000 bytes */
#define DRAW_BUF_LINES 100
#define DRAW_BUF_SIZE  (TAB5_DISPLAY_WIDTH * DRAW_BUF_LINES * sizeof(uint16_t))

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

    lv_display_flush_ready(disp);
}

/* ========================================================================= */
/*  Touch input callback                                                      */
/* ========================================================================= */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    tab5_touch_point_t points[TAB5_TOUCH_MAX_POINTS];
    uint8_t count = 0;

    bool touched = tab5_touch_read(points, &count);
    if (touched && count > 0) {
        data->point.x = points[0].x;
        data->point.y = points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
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

    while (1) {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            uint32_t time_till_next = lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
            /* Sleep for the time LVGL suggests, clamped to 5-50ms */
            if (time_till_next < 5) time_till_next = 5;
            if (time_till_next > 50) time_till_next = 50;
            vTaskDelay(pdMS_TO_TICKS(time_till_next));
        } else {
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
    s_lvgl_mutex = xSemaphoreCreateMutex();
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

    /* ---- Apply dark theme ---- */
    lv_theme_t *theme = lv_theme_default_init(
        s_display,
        lv_color_hex(0x3B82F6),  /* primary: blue accent */
        lv_color_hex(0x1E40AF),  /* secondary: darker blue */
        true,                    /* dark mode */
        &lv_font_montserrat_14
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

    /* ---- Start UI task ---- */
    BaseType_t xret = xTaskCreatePinnedToCore(
        ui_task,
        "ui_task",
        8192,       /* 8KB stack */
        NULL,
        5,          /* priority */
        &s_ui_task_handle,
        0           /* core 0 */
    );
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
    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        lv_timer_handler();
        xSemaphoreGive(s_lvgl_mutex);
    }
}

lv_display_t *tab5_ui_get_display(void)
{
    return s_display;
}

void tab5_ui_lock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    }
}

void tab5_ui_unlock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreGive(s_lvgl_mutex);
    }
}
