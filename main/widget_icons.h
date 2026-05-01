/**
 * @file widget_icons.h
 * @brief 16 built-in widget icons rendered via LVGL primitives.
 *
 * Icons are 48×48 px, drawn with stroked-only line/arc/rect primitives
 * matching the visual language in
 * `.superpowers/brainstorm/widget-platform/02-icon-library.html`.
 * No glyph fonts, no bitmaps — all paths are encoded as compact
 * structured data + materialised into a small tree of `lv_arc`,
 * `lv_line`, and `lv_obj` (for rectangles) widgets parented under a
 * caller-supplied container.
 *
 * Each icon costs roughly 1-5 child widgets in the LVGL pool when
 * rendered.  The container is owned by the caller, who is expected
 * to `lv_obj_clean()` it before re-rendering with a different icon
 * (or call `widget_icons_clear()` for symmetry).
 *
 * Coords throughout are in the source 24×24 SVG viewBox; the renderer
 * scales them ×2 so they land 1:1 on a 48 px target.
 *
 * Color comes from the caller — typically derived from the widget's
 * tone via `widget_icons_color_for_tone()`.
 *
 * v1 set (from issue #69):  clock, briefcase, laundry, coffee, book,
 *   car, pot, person, droplet, check, alert, sun, moon, cloud,
 *   calendar, star.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "widget.h" /* widget_tone_t */

#ifdef __cplusplus
extern "C" {
#endif

/** Standard render size for the v1 set.  All icons are authored at
 *  48×48; rendering at other sizes works but is not pixel-tuned. */
#define WIDGET_ICONS_SIZE 48

/** Default stroke width when no caller override.  Matches the
 *  brainstorm reference (1.8 → rounds to 2 px on the bitmap grid). */
#define WIDGET_ICONS_STROKE 2

typedef enum {
   WIDGET_ICON_NONE = 0,
   WIDGET_ICON_CLOCK,
   WIDGET_ICON_BRIEFCASE,
   WIDGET_ICON_LAUNDRY,
   WIDGET_ICON_COFFEE,
   WIDGET_ICON_BOOK,
   WIDGET_ICON_CAR,
   WIDGET_ICON_POT,
   WIDGET_ICON_PERSON,
   WIDGET_ICON_DROPLET,
   WIDGET_ICON_CHECK,
   WIDGET_ICON_ALERT,
   WIDGET_ICON_SUN,
   WIDGET_ICON_MOON,
   WIDGET_ICON_CLOUD,
   WIDGET_ICON_CALENDAR,
   WIDGET_ICON_STAR,
   WIDGET_ICON__COUNT,
} widget_icon_id_t;

/**
 * @brief Look up an icon id by its short name.
 *
 * Names match the strings the Dragon side emits in `widget.icon`
 * (kebab-cased, lowercase).  Unknown / NULL / empty input maps to
 * @ref WIDGET_ICON_NONE so the caller can early-out without a
 * separate empty-string branch.
 */
widget_icon_id_t widget_icons_lookup(const char *name);

/**
 * @brief Render an icon onto the caller's container.
 *
 * Creates 1-5 child widgets parented under @p container.  All
 * children inherit the container's position; the icon's intrinsic
 * coords fall in [0..WIDGET_ICONS_SIZE).  The container itself
 * is NOT cleared before render — caller must `lv_obj_clean()`
 * before re-rendering a different icon.
 *
 * @p stroke_w defaults to @ref WIDGET_ICONS_STROKE when given as 0.
 *
 * Safe to pass @ref WIDGET_ICON_NONE (no-op) or an out-of-range
 * id (logged + treated as no-op).
 */
void widget_icons_render(lv_obj_t *container, widget_icon_id_t id, lv_color_t color, int stroke_w);

/**
 * @brief Convenience: render by short name (lookup + render in one).
 *
 * Equivalent to:
 *   `widget_icons_render(c, widget_icons_lookup(name), col, stroke);`
 */
void widget_icons_render_named(lv_obj_t *container, const char *name, lv_color_t color, int stroke_w);

/**
 * @brief Map a widget tone to the icon stroke color.
 *
 * Calm → emerald, active → amber, urgent → rose, etc.  Keeps the
 * brainstorm's tone-driven palette consistent with the live-slot
 * orb tinting elsewhere in ui_home.
 */
lv_color_t widget_icons_color_for_tone(widget_tone_t tone);

#ifdef __cplusplus
}
#endif
