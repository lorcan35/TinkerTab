/**
 * chat_msg_store — Session-scoped message ring buffer.
 *
 * Bounded at BSP_CHAT_MAX_MESSAGES entries for the ACTIVE session.
 * Switching sessions wipes the store. No LVGL dependency — pure C.
 *
 * Storage: PSRAM-allocated at init so internal SRAM isn't eaten by
 * the ~90 KB buffer. Thread safety: access from the LVGL thread or
 * via lv_async_call (see ui_chat.c push helpers).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bsp_config.h"  /* BSP_CHAT_MAX_MESSAGES */

#define CHAT_SESSION_ID_LEN   40
#define CHAT_LLM_MODEL_LEN    64
#define CHAT_TITLE_LEN        80
#define CHAT_TEXT_LEN         512
#define CHAT_MEDIA_URL_LEN    256
#define CHAT_SUBTITLE_LEN     128

typedef enum {
    MSG_TEXT = 0,        /* Plain text (user or AI) */
    MSG_IMAGE,           /* Rendered code/table/photo (JPEG URL from Dragon) */
    MSG_CARD,            /* Link preview / search result */
    MSG_AUDIO_CLIP,      /* Pronunciation / music preview */
    MSG_SYSTEM,          /* "Session switched", errors (centered, dim) */
} msg_type_t;

/* Active session descriptor — carries mode + model fingerprint. */
typedef struct {
    char     session_id[CHAT_SESSION_ID_LEN];
    uint8_t  voice_mode;                     /* 0..3 */
    char     llm_model[CHAT_LLM_MODEL_LEN];
    char     title[CHAT_TITLE_LEN];          /* Dragon-auto-titled */
    uint32_t updated_at;
    bool     valid;
} chat_session_t;

typedef struct {
    msg_type_t type;
    bool       is_user;
    char       text[CHAT_TEXT_LEN];
    char       media_url[CHAT_MEDIA_URL_LEN];
    char       subtitle[CHAT_SUBTITLE_LEN];
    uint32_t   timestamp;
    int16_t    height_px;   /* cached after first render; -1 = not measured */
    bool       active;      /* slot in use */
    /* v4·D Phase 3d per-bubble receipt. receipt_mils==0 means no receipt
     * attached (local turn, or receipt hasn't arrived yet).  Populated
     * by chat_store_attach_receipt_to_last_ai() after the Dragon WS
     * receipt frame lands. */
    uint32_t   receipt_mils;
    uint16_t   receipt_ptok;
    uint16_t   receipt_ctok;
    char       receipt_model_short[16];  /* "haiku-3.5", "sonnet-3.5", etc */
} chat_msg_t;

/* ── API ─────────────────────────────────────────────────────── */

/** Initialize the store. Idempotent; safe to call on every chat open. */
void chat_store_init(void);

/** Swap in a new active session (wipes the message buffer). */
bool chat_store_set_session(const chat_session_t *s);

/** Returns the active session descriptor, or NULL if no session is active. */
const chat_session_t *chat_store_active_session(void);

/** Mutate fields on the active session without wiping messages
 *  (e.g. when a mid-session mode switch acks from Dragon). */
void chat_store_update_session_mode(uint8_t voice_mode, const char *llm_model);

/** Append a message; returns the logical index (0..count-1) or -1 on error.
 *  If the ring is full the oldest entry is overwritten. */
int chat_store_add(const chat_msg_t *msg);

/** Current message count (0..BSP_CHAT_MAX_MESSAGES). */
int chat_store_count(void);

/** Get message by logical index (0 = oldest, count-1 = newest). */
const chat_msg_t *chat_store_get(int index);

/** Mutable pointer for cache writes (height_px etc). */
chat_msg_t *chat_store_get_mut(int index);

/** Pointer to newest message, or NULL if empty. */
chat_msg_t *chat_store_last(void);

/** Cache measured bubble height so virtual scroll doesn't re-measure. */
bool chat_store_set_height(int index, int16_t h);

/** Replace text on the last message (streaming + text_update). */
bool chat_store_update_last_text(const char *text);

/** Attach receipt data to the most recent assistant message (scans back
 *  from newest to find first is_user==false bubble). Returns the index
 *  updated, or -1 if no assistant message exists in the ring.
 *  model_short may be a shortened display name ("haiku-3.5") or full id. */
int  chat_store_attach_receipt_to_last_ai(uint32_t mils,
                                          uint16_t prompt_tok,
                                          uint16_t completion_tok,
                                          const char *model_short);

/** Remove the last message (used to drop ephemeral system placeholders). */
bool chat_store_pop_last(void);

/** Wipe all messages (New Chat). Session descriptor is preserved. */
void chat_store_clear(void);
