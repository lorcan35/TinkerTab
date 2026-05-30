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

/** True if the notes screen exists and is not hidden. Used by home's
 *  any_overlay_visible() guard so gestures don't bubble. */
bool ui_notes_is_visible(void);

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

/** S6: Sync all pending (needs_sync) notes to Dragon. Call on reconnect. */
void ui_notes_sync_pending(void);

/** PR 3 follow-up: add a dictated note from the WS pipeline path (FAB
 *  or home Dictate chip) — Dragon's `dictation_summary` doesn't create
 *  a Tab5-local slot otherwise.  Marshals to the LVGL thread via
 *  tab5_lv_async_call so it's safe to call from the WS event task.
 *  No-op if a local-recording slot is already active (the
 *  "+ NEW VOICE NOTE" button path owns that slot via
 *  ui_notes_start_recording).  Caller owns the string; we copy. */
void ui_notes_add_dictated_async(const char *transcript);

/* ── W4 optimistic save (all marshal to the LVGL thread; reconcile by turn_id) ── */

/** W4: seed (or tag) an optimistic note row for a live dictation turn_id at stop
 *  time.  Idempotent per turn_id; tags an existing recording slot instead of
 *  duplicating.  Reconciled later by the two functions below (same turn_id). */
void ui_notes_seed_optimistic(const char *turn_id);

/** W4: reconcile Dragon's authoritative note_created by turn_id — adopt note_id
 *  in place (no dup row), or create the row if note_created raced the seed.
 *  turn_id may be "-"/NULL (absent) → treated as a fresh note (backward compat). */
void ui_notes_reconcile_note_created(const char *turn_id, const char *note_id, const char *title);

/** W4: apply dictation_summary as an in-place note update keyed by turn_id — sets
 *  the body text + flips enrich to DONE so the badge clears.  Falls back to a
 *  fresh add if no row matches turn_id, so a dictation is never lost. */
void ui_notes_apply_summary(const char *turn_id, const char *title, const char *summary);

/** W4: mark the just-finalized SD recording as offline-pending (badge "Pending")
 *  and stamp turn_id so a reconnect note_created reconciles it in place.  Called
 *  from voice.c's offline dictation stop branch. */
void ui_notes_mark_offline_pending(const char *turn_id);

/** #537: arm a SD WAV recording for an incoming pipeline-path dictation
 *  (Home Dictate chip / chat overlay mic).  Opens a new WAV file +
 *  reserves a note slot in NOTE_STATE_RECORDED state — the existing
 *  ui_notes_write_audio() hook in voice.c's mic capture task will
 *  populate the file frame-by-frame.  Returns true if the slot was
 *  armed, false if SD isn't mounted or another recording is already
 *  in flight (FAB or prior arm).  Called from the LVGL thread. */
bool ui_notes_pipeline_arm_recording(void);

/** #537: cancel a pipeline-armed recording.  Closes the WAV, deletes
 *  the half-written file from SD, and removes the dangling note slot
 *  from the timeline.  No-op if no pipeline-armed recording exists
 *  (FAB-armed slots are untouched).  Idempotent.  Called on user-
 *  initiated cancel + on dictation_postprocessing_cancelled WS frames. */
void ui_notes_pipeline_cancel_recording(void);

/** PR 4: attach a Dragon-classifier proposed_action to the most recent
 *  note (the one just created by ui_notes_add_dictated_async).  Async-
 *  safe: marshals to the LVGL thread.  Caller passes kind / confidence
 *  / payload as parsed from the dictation_summary frame; ui_notes
 *  stores them on the matching slot and triggers a refresh so the
 *  action chip renders.  kind: 1=reminder, 2=list.  Confidence is
 *  0-100; below PENDING_CONFIDENCE_FLOOR (75) the chip is suppressed.
 *  Caller owns the payload string; we copy. */
void ui_notes_attach_pending_chip_async(uint8_t kind, uint8_t confidence, const char *payload);
