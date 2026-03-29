#pragma once
#include "lvgl.h"

/**
 * Global on-screen keyboard overlay for TinkerOS.
 *
 * Slides up from the bottom of any screen. Activated by a floating
 * trigger button that sits at the bottom of the screen.
 *
 * Usage:
 *   ui_keyboard_init(parent_screen);     // Call once after LVGL init
 *   ui_keyboard_show(target_textarea);    // Show keyboard, type into target
 *   ui_keyboard_hide();                   // Hide keyboard
 *   ui_keyboard_toggle(target_textarea);  // Toggle visibility
 *   ui_keyboard_is_visible();             // Check state
 */

// Initialize the global keyboard overlay and floating trigger button
// Call once after LVGL is initialized. Parent should be the top-level screen layer.
void ui_keyboard_init(lv_obj_t *parent);

// Show the keyboard, targeting a specific textarea for input
// If target is NULL, keyboard opens but doesn't type into anything
void ui_keyboard_show(lv_obj_t *target_textarea);

// Hide the keyboard with animation
void ui_keyboard_hide(void);

// Toggle keyboard visibility
void ui_keyboard_toggle(lv_obj_t *target_textarea);

// Check if keyboard is currently visible
bool ui_keyboard_is_visible(void);

// Set the input target textarea (switch focus without hiding)
void ui_keyboard_set_target(lv_obj_t *target_textarea);

// Get the floating trigger button (for positioning from other screens)
lv_obj_t *ui_keyboard_get_trigger_btn(void);
