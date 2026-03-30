#pragma once
/* Stub: esp_lcd_panel_ops.h for linux target */
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef void *esp_lcd_panel_handle_t;

static inline esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p)       { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p)        { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t p)         { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_disp_sleep(esp_lcd_panel_handle_t p, bool sleep) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t p, bool mx, bool my) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t p, bool swap) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t p, int x, int y) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t p, bool inv) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p,
    int xs, int ys, int xe, int ye, const void *buf) { return ESP_OK; }
