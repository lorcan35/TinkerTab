/**
 * widget_mode_dot — implementation.  See header for rationale.
 */
#include "widget_mode_dot.h"

#include "config.h"   /* VOICE_MODE_COUNT */
#include "ui_theme.h" /* th_mode_colors */

/* Out-of-range vmode fallback colour — same neutral grey ui_sessions.c
 * uses at the bottom of mode_to_tag's else branch. */
#define MODE_DOT_FALLBACK 0x5C5C68

static uint32_t resolve_color(uint8_t vmode) {
   if (vmode < VOICE_MODE_COUNT) return th_mode_colors[vmode];
   return MODE_DOT_FALLBACK;
}

lv_obj_t *widget_mode_dot_create(lv_obj_t *parent, int size, uint8_t vmode) {
   if (!parent || size <= 0) return NULL;
   lv_obj_t *dot = lv_obj_create(parent);
   if (!dot) return NULL;
   lv_obj_remove_style_all(dot);
   lv_obj_set_size(dot, size, size);
   lv_obj_set_style_radius(dot, size / 2, 0);
   lv_obj_set_style_bg_color(dot, lv_color_hex(resolve_color(vmode)), 0);
   lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
   return dot;
}

void widget_mode_dot_set_mode(lv_obj_t *dot, uint8_t vmode) {
   if (!dot) return;
   lv_obj_set_style_bg_color(dot, lv_color_hex(resolve_color(vmode)), 0);
}
