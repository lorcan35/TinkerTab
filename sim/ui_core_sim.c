/*
 * ui_core_sim.c — Desktop simulator backend for TinkerOS UI
 *
 * Implements the tab5_ui_* API using SDL2 + LVGL SDL driver.
 * This file replaces ui_core.c in the simulator build.
 *
 * On desktop:
 *   - lv_sdl_window_create(720, 1280) creates the SDL2 window
 *   - lv_sdl_mouse_create() maps mouse clicks to LVGL touch events
 *   - lv_timer_handler() runs in the main loop (no FreeRTOS needed)
 *   - No mutex needed (single-threaded)
 */

#include "ui_core.h"
#include <stdio.h>
#include <stdbool.h>

/* LVGL — SDL2 driver headers */
#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"

static lv_display_t *s_display = NULL;
static bool          s_initialized = false;

/* Called from main.c — sets up LVGL + SDL2 window */
void tab5_ui_sim_init(void)
{
    if (s_initialized) return;

    lv_init();

    /* Create 720×1280 SDL2 window */
    s_display = lv_sdl_window_create(720, 1280);
    if (!s_display) {
        fprintf(stderr, "[ui_core_sim] Failed to create SDL2 display\n");
        return;
    }
    lv_display_set_default(s_display);

    /* Mouse → touch input */
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, s_display);

    /* Apply TinkerOS dark theme */
    lv_theme_t *theme = lv_theme_default_init(
        s_display,
        lv_color_hex(0xFFB800),  /* primary: amber */
        lv_color_hex(0x00B4D8),  /* secondary: cyan */
        true,                    /* dark mode */
        &lv_font_montserrat_18
    );
    lv_display_set_theme(s_display, theme);

    s_initialized = true;
    printf("[ui_core_sim] LVGL %d.%d.%d + SDL2 window 720x1280 ready\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
}

/* ── tab5_ui_* API stubs (called from ui_*.c) ─────────────────────── */

/* On desktop these are no-ops — LVGL is not multi-threaded */
void tab5_ui_lock(void)   { }
void tab5_ui_unlock(void) { }

bool tab5_ui_try_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}

lv_display_t *tab5_ui_get_display(void) { return s_display; }

void tab5_ui_tick(void)
{
    lv_timer_handler();
}
