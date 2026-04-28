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
/* Widget-platform phase 2 (#70): card action slot.  Per docs/WIDGETS.md
 * §6 the protocol limits action.label to ≤8 visible chars; we allow a
 * bit more bytes to cover UTF-8 + a NUL.  card_id matches WIDGET_ID_LEN
 * (32) in widget.h. */
#define CHAT_CARD_ID_LEN      32
#define CHAT_ACTION_LABEL_LEN 16
#define CHAT_ACTION_EVENT_LEN 32

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
    bool       receipt_retried;          /* v4·D Gauntlet G2: context-trim or 429 retry */
    /* Widget-platform phase 2 (#70): MSG_CARD optional tappable action.
     * card_id[0]=='\0' means no card_id known; action_label[0]=='\0'
     * means no action button rendered.  When both label and event are
     * non-empty the renderer draws an amber pill at the bottom-right of
     * the breakout that fires voice_send_widget_action(card_id, event)
     * on tap. */
    char       card_id[CHAT_CARD_ID_LEN];
    char       action_label[CHAT_ACTION_LABEL_LEN];
    char       action_event[CHAT_ACTION_EVENT_LEN];
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

/** Audit B2 (TinkerBox #137 / TinkerTab #202): how many oldest-message
 *  evictions have happened since boot.  Pre-fix the ring overwrote
 *  silently; this counter lets the debug server / dashboard surface
 *  whether a session ever scrolled past BSP_CHAT_MAX_MESSAGES. */
uint32_t chat_store_evictions_total(void);

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

/** Gauntlet G2 — extended form with retry flag so the bubble can
 *  stamp a "RETRIED" chip when Dragon fell into a 429 or context-trim
 *  retry mid-turn. */
int  chat_store_attach_receipt_ex(uint32_t mils,
                                  uint16_t prompt_tok,
                                  uint16_t completion_tok,
                                  const char *model_short,
                                  bool retried);

/** Remove the last message (used to drop ephemeral system placeholders). */
bool chat_store_pop_last(void);

/** Wipe all messages (New Chat). Session descriptor is preserved. */
void chat_store_clear(void);
