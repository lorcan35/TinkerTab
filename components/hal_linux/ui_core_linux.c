/*
 * ui_core_linux.c — LVGL + SDL2 display/touch for ESP-IDF Linux target.
 *
 * Implements the same tab5_ui_* API as ui_core.c, but uses LVGL's built-in
 * SDL2 driver instead of the DPI framebuffer.
 *
 * Display: 720x1280 SDL2 window via lv_sdl_window_create()
 * Touch:   SDL2 mouse events via lv_sdl_mouse_create()
 * Tick:    FreeRTOS timer task calls lv_tick_inc(5) every 5ms
 * Mutex:   FreeRTOS mutex for thread-safe LVGL access
 */

#include "ui_core.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lvgl.h"
#include "drivers/sdl/lv_sdl_window.h"
#include "drivers/sdl/lv_sdl_mouse.h"

#include <SDL2/SDL.h>

static const char *TAG = "ui_core_linux";

static lv_display_t     *s_display = NULL;
static SemaphoreHandle_t s_mutex   = NULL;
static TaskHandle_t      s_task    = NULL;

/* ── LVGL + SDL2 event loop task ────────────────────────────────────── */
static void ui_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started (Linux/SDL2)");

    while (1) {
        /* Process SDL events — required for SDL2 window to stay responsive */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                ESP_LOGI(TAG, "SDL quit event — calling exit(0)");
                exit(0);
            }
            /* ESC / Q keys quit */
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    exit(0);
                }
            }
        }

        /* Drive LVGL */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        uint32_t delay_ms = lv_timer_handler();
        xSemaphoreGive(s_mutex);

        if (delay_ms > 16) delay_ms = 16;   /* cap at ~60 FPS */
        vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));
    }
}

/* ── Tick task — feeds lv_tick_inc every 5ms ────────────────────────── */
static void tick_task(void *arg)
{
    while (1) {
        lv_tick_inc(5);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Public API — matches ui_core.h                                       */
/* ══════════════════════════════════════════════════════════════════════ */

esp_err_t tab5_ui_init(esp_lcd_panel_handle_t panel)
{
    /* panel is NULL on linux target — we use SDL2 instead */
    (void)panel;

    ESP_LOGI(TAG, "Initializing LVGL (Linux/SDL2) — 720x1280 window");

    /* Initialize SDL2 */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        ESP_LOGE(TAG, "SDL_Init failed: %s", SDL_GetError());
        return ESP_FAIL;
    }

    /* Initialize LVGL */
    lv_init();

    /* Create SDL2 display window — 720x1280 matches Tab5 */
    s_display = lv_sdl_window_create(720, 1280);
    if (!s_display) {
        ESP_LOGE(TAG, "lv_sdl_window_create failed");
        return ESP_FAIL;
    }
    lv_display_set_default(s_display);

    /* SDL2 mouse → LVGL touch input */
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, s_display);

    /* Apply TinkerOS dark theme */
    lv_theme_t *theme = lv_theme_default_init(s_display,
        lv_palette_main(LV_PALETTE_AMBER),   /* primary: amber */
        lv_palette_main(LV_PALETTE_CYAN),    /* secondary: cyan */
        true,                                 /* dark mode */
        LV_FONT_DEFAULT);
    lv_display_set_theme(s_display, theme);

    /* Mutex for thread-safe LVGL access */
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_FAIL;
    }

    /* Tick task — 5ms period */
    xTaskCreate(tick_task, "lv_tick", 2048, NULL, configMAX_PRIORITIES - 2, NULL);

    /* Main LVGL event loop task */
    xTaskCreate(ui_task, "lv_ui", 8192, NULL, configMAX_PRIORITIES - 1, &s_task);

    ESP_LOGI(TAG, "LVGL initialized — SDL2 window 720x1280 ready");
    return ESP_OK;
}

void tab5_ui_tick(void)
{
    /* Handled by tick_task */
}

lv_display_t *tab5_ui_get_display(void)
{
    return s_display;
}

void tab5_ui_lock(void)
{
    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

bool tab5_ui_try_lock(uint32_t timeout_ms)
{
    if (!s_mutex) return true;
    return xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void tab5_ui_unlock(void)
{
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}

uint32_t ui_core_get_fps(void) { return 0; }
