/* dictation_notes.h — dictation engine extracted from ui_notes.c (W5).
 *
 * Owns the background, non-UI side of dictation notes: Dragon REST sync today;
 * the transcription queue + SD WAV I/O follow in later W5 increments.  Shares
 * the note store with the UI side via ui_notes_internal.h.
 *
 * The UI (ui_notes.c) calls into here; this module calls back into the shared
 * store helpers (notes_load / find_note_idx_by_text / s_notes / …). */
#ifndef DICTATION_NOTES_H
#define DICTATION_NOTES_H

#include <stdbool.h>

/* Fire-and-forget POST of a note (title+text) to Dragon's /api/notes.  Marks the
 * matching slot needs_sync if Wi-Fi is down (retried by ui_notes_sync_pending on
 * reconnect).  Called from the dictation save paths in ui_notes.c. */
void sync_note_to_dragon(const char *title, const char *text);

/* ui_notes_sync_pending() is declared in ui_notes.h (public catch-up entry). */

/* ── W5: SD-recording engine + transcription queue (moved from ui_notes.c) ──
 * The PUBLIC entry points (ui_notes_start_recording / _stop_recording /
 * _write_audio / _pipeline_arm_recording / _pipeline_cancel_recording /
 * _start_transcription_queue) keep their declarations in ui_notes.h so external
 * callers (voice.c, ui_home.c, ui_voice.c, main.c, voice_ws_proto.c) are
 * unchanged; their DEFINITIONS live in dictation_notes.c.  This module includes
 * ui_notes.h so it sees those prototypes. */

/* Start the standalone SD-only recording task (mic → WAV, no Dragon).  Sets
 * s_sd_rec_running = true and spawns sd_record_task.  Called from the UI's
 * cb_new_voice offline branch. */
void dictation_sd_record_start(void);

/* ── UI → engine read accessors (engine owns these statics; UI reads them
 * from the W4 optimistic-save callbacks + the dictated-async finalise path) ── */

/* True while an SD WAV file handle is open (s_rec_file != NULL). */
bool dictation_recording_active(void);

/* The note slot reserved for the in-flight recording, or -1 if none
 * (s_rec_note_slot). */
int dictation_recording_slot(void);

/* True if the open recording was armed by the pipeline path (home Dictate chip /
 * chat mic) rather than the local FAB (s_pipeline_armed_slot). */
bool dictation_pipeline_armed(void);

/* Clear the pipeline-armed flag (s_pipeline_armed_slot = false).  Called by the
 * UI's dictated-async finalise path right before it flips the reserved row used
 * and calls ui_notes_stop_recording. */
void dictation_pipeline_clear_armed(void);

#endif /* DICTATION_NOTES_H */
