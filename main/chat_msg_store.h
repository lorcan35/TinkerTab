#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "bsp_config.h"  /* BSP_CHAT_MAX_MESSAGES */

/* Voice modes — matches config.h VOICE_MODE_* */
#define CHAT_MODE_COUNT  4

typedef enum {
    MSG_TEXT,           /* Plain text (user or AI) */
    MSG_IMAGE,          /* Rendered code/table/photo (JPEG URL from Dragon) */
    MSG_CARD,           /* Link preview / search result */
    MSG_AUDIO_CLIP,     /* Pronunciation / music preview */
    MSG_TOOL_STATUS,    /* "Searching the web..." (ephemeral) */
    MSG_SYSTEM,         /* "Clearing..." / errors (centered) */
} msg_type_t;

typedef struct {
    msg_type_t  type;
    bool        is_user;          /* true = right-aligned user bubble */
    char        text[512];        /* message content or alt text for media */
    char        media_url[256];   /* relative URL for MSG_IMAGE/CARD/AUDIO */
    char        subtitle[128];    /* for MSG_CARD only */
    uint32_t    timestamp;        /* epoch seconds (from RTC) */
    int16_t     height_px;        /* measured bubble height, 0 = unmeasured */
    bool        active;           /* slot is in use */
} chat_msg_t;

/** Initialize the store (zeroes all buffers). Call once at boot. */
void chat_store_init(void);

/** Add a message to the specified mode's ring buffer. Returns index or -1 on error. */
int chat_store_add(uint8_t mode, const chat_msg_t *msg);

/** Get message count for a mode. */
int chat_store_count(uint8_t mode);

/** Get message by index (0 = oldest). Returns NULL if out of range. */
const chat_msg_t *chat_store_get(uint8_t mode, int index);

/** Get mutable pointer (for caching height_px after render). */
chat_msg_t *chat_store_get_mut(uint8_t mode, int index);

/** Clear all messages for a mode (New Chat). */
void chat_store_clear(uint8_t mode);

/** Clear all modes (factory reset). */
void chat_store_clear_all(void);

/** Get the last message for a mode (for streaming append). NULL if empty. */
chat_msg_t *chat_store_last(uint8_t mode);
