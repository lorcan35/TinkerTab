/**
 * chat_msg_view — recycled object pool + virtual scroll (chat v4·C).
 *
 * Creates its own scroll container inside `parent`. The pool holds
 * BSP_CHAT_POOL_SIZE (12) slots; messages outside the visible window
 * are detached from their pool slots and re-assigned on scroll.
 *
 * Bubble styles + breakouts match the v4·C spec §3–§4.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct chat_msg_view chat_msg_view_t;

chat_msg_view_t *chat_msg_view_create(lv_obj_t *parent, int x, int y, int w, int h);
void chat_msg_view_destroy(chat_msg_view_t *v);

/** Resize the scroll viewport (e.g. when the keyboard slides up). */
void chat_msg_view_set_size(chat_msg_view_t *v, int w, int h);

/** Paint the break-out amber accents in the active session's mode color. */
void chat_msg_view_set_mode_color(chat_msg_view_t *v, uint32_t hex);

/** Rebuild the visible window from the store. */
void chat_msg_view_refresh(chat_msg_view_t *v);
void chat_msg_view_scroll_to_bottom(chat_msg_view_t *v);

/** Begin a streaming AI message: pin the last slot, buffer tokens. */
void chat_msg_view_begin_streaming(chat_msg_view_t *v);
void chat_msg_view_append_stream(chat_msg_view_t *v, const char *text);
void chat_msg_view_end_streaming(chat_msg_view_t *v);
bool chat_msg_view_is_streaming(chat_msg_view_t *v);

/** Expose the scroll container (keyboard layout resize). */
lv_obj_t *chat_msg_view_get_scroll(chat_msg_view_t *v);
