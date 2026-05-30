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

/* Fire-and-forget POST of a note (title+text) to Dragon's /api/notes.  Marks the
 * matching slot needs_sync if Wi-Fi is down (retried by ui_notes_sync_pending on
 * reconnect).  Called from the dictation save paths in ui_notes.c. */
void sync_note_to_dragon(const char *title, const char *text);

/* ui_notes_sync_pending() is declared in ui_notes.h (public catch-up entry). */

#endif /* DICTATION_NOTES_H */
