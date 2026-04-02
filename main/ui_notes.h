/**
 * TinkerTab — Notes Screen
 *
 * Voice-first note-taking: tap to record a note, tap to play back.
 * Notes persist across reboots — saved as JSON to SD card (/sdcard/notes.json).
 * Falls back to NVS blob if SD is not mounted. Saved on every add/delete.
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

/** Delete a note by ring-buffer slot index. No-op if invalid index. */
void ui_notes_delete(int idx);

/** List all notes to stdout (for serial debug). */
void ui_notes_list(void);

/** Start recording a voice note to SD card. Returns the WAV file path, or NULL on failure.
 *  The returned path is valid until the next call to ui_notes_start_recording(). */
const char *ui_notes_start_recording(void);

/** Stop recording and finalize the WAV file. Creates a note in state RECORDED.
 *  If transcript is non-NULL, attaches it and sets state to TRANSCRIBED. */
void ui_notes_stop_recording(const char *transcript);

/** Write raw PCM samples to the active recording. Called from mic capture task.
 *  Thread-safe (uses internal mutex). */
void ui_notes_write_audio(const int16_t *samples, size_t count);

/** Get number of unprocessed (RECORDED) notes that need transcription. */
int ui_notes_unprocessed_count(void);

/** Start background transcription task (call once after WiFi + Dragon are up). */
void ui_notes_start_transcription_queue(void);

/** Delete all FAILED notes. Returns number deleted. */
int ui_notes_clear_failed(void);
