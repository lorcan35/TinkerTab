/**
 * ui_feedback.h — Unified touch feedback for TinkerOS
 *
 * Provides one-line functions to add pressed/released visual feedback
 * to any LVGL object. Uses 100ms transitions for smooth feel.
 *
 * Usage:
 *   ui_fb_button(btn);           // Darkens bg on press
 *   ui_fb_card(card);            // Lightens border + bg shift on press
 *   ui_fb_icon(label);           // Dims opacity on press
 *   ui_fb_nav(label);            // Brightens text on press
 *   ui_fb_custom(obj, 0x2A2A3E); // Custom pressed bg color
 */
#pragma once

#include "lvgl.h"

/**
 * Button feedback: darken background 20% on press, 100ms transition.
 * Works on lv_button and any obj with bg_color set.
 */
void ui_fb_button(lv_obj_t *obj);

/**
 * Button feedback with explicit pressed color.
 */
void ui_fb_button_colored(lv_obj_t *obj, uint32_t pressed_hex);

/**
 * Card feedback: lighten border + subtle bg shift on press.
 * For tappable card containers.
 */
void ui_fb_card(lv_obj_t *obj);

/**
 * Icon/label feedback: reduce opacity to 60% on press.
 * For clickable text labels and icon buttons.
 */
void ui_fb_icon(lv_obj_t *obj);

/**
 * Nav item feedback: brighten text color on press.
 * For bottom nav bar labels.
 */
void ui_fb_nav(lv_obj_t *obj);

/**
 * Custom pressed color: set any bg color on press with transition.
 */
void ui_fb_custom(lv_obj_t *obj, uint32_t pressed_bg_hex);
