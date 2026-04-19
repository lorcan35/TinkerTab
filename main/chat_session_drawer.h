/**
 * chat_session_drawer — pull-down drawer with past sessions.
 *
 * Fetches GET /api/v1/sessions?limit=10 from Dragon, paints rows with
 * per-session mode colors.  Tap a row → on_pick(session). Tap footer
 * "+ Start new conversation" → on_new().
 *
 * The fetch runs on a separate FreeRTOS task so the LVGL timeline is
 * never blocked by HTTP latency; rows materialise via lv_async_call.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include "chat_msg_store.h"   /* chat_session_t */

typedef struct chat_session_drawer chat_session_drawer_t;

typedef void (*chat_drawer_pick_cb_t)(const chat_session_t *s, void *user_data);
typedef void (*chat_drawer_new_cb_t)(void *user_data);
typedef void (*chat_drawer_dismiss_cb_t)(void *user_data);

chat_session_drawer_t *chat_session_drawer_create(lv_obj_t *parent);
void chat_session_drawer_destroy(chat_session_drawer_t *d);

/** Show and trigger a fresh fetch. */
void chat_session_drawer_show(chat_session_drawer_t *d);
void chat_session_drawer_hide(chat_session_drawer_t *d);
bool chat_session_drawer_is_open(chat_session_drawer_t *d);

void chat_session_drawer_on_pick(chat_session_drawer_t *d,
    chat_drawer_pick_cb_t cb, void *ud);
void chat_session_drawer_on_new(chat_session_drawer_t *d,
    chat_drawer_new_cb_t cb, void *ud);
void chat_session_drawer_on_dismiss(chat_session_drawer_t *d,
    chat_drawer_dismiss_cb_t cb, void *ud);

/** Mark the row matching this session_id as "active" (amber left border). */
void chat_session_drawer_mark_active(chat_session_drawer_t *d, const char *session_id);
