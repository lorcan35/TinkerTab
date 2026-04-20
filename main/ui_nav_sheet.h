#pragma once
#include "lvgl.h"

/* v4·D Sovereign Halo nav sheet.
 * Opens when the 4-dot "more chip" on the home say-pill is tapped.
 * Six tiles: Chat / Notes / Settings / Camera / Files / Memory.
 *
 * The sheet lives on lv_layer_top() so it renders over any surface.
 * Uses hide/show semantics -- created once on first invoke, then
 * toggled visibility on subsequent opens, to avoid internal-SRAM
 * fragmentation (see heap_watchdog.c). */
void ui_nav_sheet_show(void);
void ui_nav_sheet_hide(void);
bool ui_nav_sheet_is_visible(void);
