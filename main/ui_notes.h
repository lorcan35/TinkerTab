/**
 * TinkerTab — Notes Screen
 *
 * Voice-first note-taking: tap to record a note, tap to play back.
 * Notes are stored in a static ring buffer in this module.
 * Long-term: SD card storage with WiFi/SDIO conflict resolved.
 *
 * 720x1280 portrait, LVGL v9, dark theme.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

/** Create and show the notes screen. */
lv_obj_t *ui_notes_create(void);

/** Destroy the notes screen. */
void ui_notes_destroy(void);

/** Add a new note. Returns note index or -1 on failure. */
int ui_notes_add(const char *text, bool is_voice);

/** Get last note's text preview (up to 80 chars). Returns false if no notes. */
bool ui_notes_get_last_preview(char *buf, size_t len);

/** Check if notes exist. */
int ui_notes_count(void);
