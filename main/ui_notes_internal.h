/* ui_notes_internal.h — shared note-store surface between ui_notes.c (UI:
 * list/edit/search/chips) and dictation_notes.c (engine: Dragon REST sync,
 * transcription queue, SD WAV I/O).  W5 extraction.
 *
 * NOT a public API — only these two translation units include it.  The note
 * store (s_notes + counters) is DEFINED in ui_notes.c and declared extern here
 * so the engine can reach it without a second copy.  Types live here so both
 * sides agree on the struct layout. */
#ifndef UI_NOTES_INTERNAL_H
#define UI_NOTES_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "voice_dictation.h" /* DICT_TURN_ID_LEN / DICT_NOTE_ID_LEN */

#define MAX_NOTES 30
#define MAX_NOTE_LEN 32768
#define MAX_AUDIO_PATH 64
#define MAX_NOTE_REC_SECS 14400 /* 4 hr cap (matches the WS-streaming cap) */
#define PENDING_CONFIDENCE_FLOOR 75
#define PENDING_PAYLOAD_LEN 128

/* W5: recordings directory — shared because notes_load() (UI, ensures the dir
 * exists on boot) and ui_notes_start_recording() (engine, writes the WAV here)
 * both need it. */
#define REC_DIR "/sdcard/rec"

/* ── Note lifecycle / classification enums ── */
typedef enum {
   NOTE_STATE_TEXT,         /* text-only note (typed) */
   NOTE_STATE_RECORDED,     /* has audio file, not yet transcribed */
   NOTE_STATE_TRANSCRIBING, /* transcription in progress */
   NOTE_STATE_TRANSCRIBED,  /* has audio file + transcript */
   NOTE_STATE_FAILED,       /* transcription failed — can retry */
} note_state_t;

/* W4: per-note enrichment lifecycle, independent of the orb/FSM. */
typedef enum {
   ENRICH_NONE = 0,
   ENRICH_TRANSCRIBING,
   ENRICH_SUMMARIZING,
   ENRICH_PENDING,
   ENRICH_DONE,
} enrich_state_t;

typedef enum {
   NOTE_FAIL_NONE = 0,
   NOTE_FAIL_AUTH,
   NOTE_FAIL_NETWORK,
   NOTE_FAIL_EMPTY,
   NOTE_FAIL_NO_AUDIO,
   NOTE_FAIL_TOO_LONG,
} note_fail_t;

typedef enum {
   NOTE_TYPE_AUTO = 0,
   NOTE_TYPE_TEXT,
   NOTE_TYPE_VOICE,
   NOTE_TYPE_LIST,
   NOTE_TYPE_REMINDER,
} note_type_t;

typedef enum {
   PENDING_NONE = 0,
   PENDING_REMINDER = 1,
   PENDING_LIST = 2,
} pending_kind_t;

typedef struct {
   uint8_t kind; /* pending_kind_t */
   uint8_t confidence;
   char payload[PENDING_PAYLOAD_LEN];
} pending_chip_t;

/* ── Note storage record ── */
typedef struct {
   char text[MAX_NOTE_LEN];
   char audio_path[MAX_AUDIO_PATH];
   note_state_t state;
   note_fail_t fail_reason;
   note_type_t type;
   pending_chip_t pending;
   bool is_voice;
   uint8_t hour;
   uint8_t minute;
   uint8_t day;
   uint8_t month;
   uint8_t year;
   bool used;
   bool needs_sync;
   /* W4 optimistic-save + reconcile-by-turn_id */
   enrich_state_t enrich;
   char turn_id[DICT_TURN_ID_LEN];
   char note_id[DICT_NOTE_ID_LEN];
} note_entry_t;

/* ── Shared store — DEFINED in ui_notes.c ── */
extern note_entry_t *s_notes;
extern int s_note_count;
extern int s_next_slot;
extern bool s_loaded;

/* ── W5 recording engine ⟷ UI shared state (all DEFINED in ui_notes.c) ──
 * These stay UI-owned (the UI orchestrates record start/stop via cb_new_voice
 * + voice_state_cb) but the engine (dictation_notes.c) reads/writes them:
 *   - s_voice_recording   : UI writes (voice_state_cb / cb_new_voice / destroy);
 *                           engine reads in transcription_queue_task to skip
 *                           transcribing while a live voice turn is in flight.
 *   - s_sd_rec_running    : both sides write — UI starts/stops the standalone SD
 *                           recording in cb_new_voice; the engine's sd_record_task
 *                           clears it on exit / cap-hit and reads it as the loop
 *                           condition.  Defined UI-side because the UI owns the
 *                           start path; the engine flips it via the shared symbol.
 *   - s_next_rec_id       : monotonic recording counter — persisted by notes_save
 *                           / notes_load (UI), bumped by ui_notes_start_recording
 *                           (engine) on each new WAV. */
extern bool s_voice_recording;
extern volatile bool s_sd_rec_running;
extern uint32_t s_next_rec_id;

/* ── Shared helpers — DEFINED in ui_notes.c (de-static'd for the engine) ──
 * Original names kept (no call-site churn); they become module-global symbols
 * shared by the two notes translation units. */
bool ensure_notes_buf(void);
void notes_save(void);
void notes_load(void);
void refresh_list(void);
int find_note_idx_by_text(const char *text);
int find_note_idx_by_turn_id(const char *turn_id);
int find_most_recent_used_slot(void);
void pending_chip_apply_inline(int idx);

#endif /* UI_NOTES_INTERNAL_H */
