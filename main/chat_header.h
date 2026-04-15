#pragma once

#include "lvgl.h"
#include <stdint.h>

typedef struct {
    lv_obj_t *container;     /* horizontal flex row */
    lv_obj_t *back_btn;      /* back arrow (left) */
    lv_obj_t *title;         /* screen title (flex-grow) */
    lv_obj_t *status_dot;    /* connection indicator dot */
    lv_obj_t *status_label;  /* "Ready" / "Processing..." */
    lv_obj_t *action_btn;    /* optional right button (+ for new chat) */
    lv_obj_t *mode_badge;    /* mode indicator (right) */
} chat_header_t;

/**
 * Create a reusable header widget. Returns heap-allocated struct (caller frees on destroy).
 * @param parent      LVGL parent object
 * @param title       Screen title ("Chat", "Notes", etc.)
 * @param accent_color Mode accent color (hex)
 * @param show_action true to show + button
 */
chat_header_t *chat_header_create(lv_obj_t *parent, const char *title,
                                   uint32_t accent_color, bool show_action);

void chat_header_set_status(chat_header_t *hdr, const char *text, bool connected);
void chat_header_set_mode(chat_header_t *hdr, const char *mode_name, uint32_t color);
void chat_header_set_back_cb(chat_header_t *hdr, lv_event_cb_t cb, void *user_data);
void chat_header_set_action_cb(chat_header_t *hdr, lv_event_cb_t cb, void *user_data);
