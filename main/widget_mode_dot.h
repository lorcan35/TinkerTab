/**
 * widget_mode_dot — shared mode-color dot widget.
 *
 * TT #328 Wave 6: pre-Wave-6 the same circular-bg-colored dot was open-
 * coded in three places — ui_home.c (mode pill), chat_header.c (chip),
 * chat_session_drawer.c (row prefix).  Each used different size + radius
 * + opacity values and the mode→color lookup drifted (Wave 1's COL_CYAN
 * alias bug was a member of this same class).  Routing all three through
 * this single primitive prevents future drift.
 *
 *   lv_obj_t *dot = widget_mode_dot_create(parent, 10, vmode);
 *   …
 *   widget_mode_dot_set_mode(dot, new_vmode);
 *
 * Color comes from th_mode_colors[VOICE_MODE_COUNT] (ui_theme.{c,h}).
 * Out-of-range vmode falls back to a neutral grey.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

/** Create a circular mode-tinted dot of `size` px on `parent`.
 *  Returns the LVGL object so callers can position / further style it. */
lv_obj_t *widget_mode_dot_create(lv_obj_t *parent, int size, uint8_t vmode);

/** Repaint an existing dot for a different voice_mode (fast: only touches
 *  the bg_color style — no resize, no realloc). */
void widget_mode_dot_set_mode(lv_obj_t *dot, uint8_t vmode);
