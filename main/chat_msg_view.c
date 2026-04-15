/**
 * TinkerTab — Recycled Message View (Virtual Scroll)
 *
 * Fixed pool of BSP_CHAT_POOL_SIZE (12) slots, each with 3 LVGL objects
 * (container + content label + timestamp label = 36 total objects).
 * Messages are positioned manually by Y offset. As the user scrolls,
 * pool slots are recycled to display whichever messages are visible.
 *
 * This module does NOT handle rich media downloads — it shows placeholder
 * text for MSG_IMAGE types. Media rendering is handled by the orchestrator.
 */
#include "chat_msg_view.h"
#include "chat_msg_store.h"
#include "config.h"

#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Layout constants ──────────────────────────────────────────── */
#define BUBBLE_MAX_W    460
#define BUBBLE_PAD       14
#define BUBBLE_GAP       10
#define LABEL_MAX_W     (BUBBLE_MAX_W - 2 * BUBBLE_PAD)

#define DISPLAY_W        720
#define TOPBAR_H          60
#define INPUT_BAR_H       80
#define NAV_BAR_H        120
#define USABLE_H        (1280 - NAV_BAR_H)           /* 1160 */
#define MSG_AREA_H      (USABLE_H - TOPBAR_H - INPUT_BAR_H)  /* 1020 */

/* Visibility buffer — render messages this far above/below viewport */
#define VIS_BUFFER       720

/* ── Pool slot ─────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *container;    /* bubble background */
    lv_obj_t *content;      /* text label or image placeholder */
    lv_obj_t *timestamp;    /* time label */
    int       data_idx;     /* which store index this slot shows, -1 = unused */
} msg_slot_t;

/* ── Static state ──────────────────────────────────────────────── */
static msg_slot_t  s_pool[BSP_CHAT_POOL_SIZE];
static lv_obj_t   *s_scroll     = NULL;
static uint8_t     s_mode       = 0;
static bool        s_streaming  = false;
static char        s_stream_buf[2048];
static int         s_stream_len = 0;

/* ── Shared static styles (initialized once) ───────────────────── */
static lv_style_t  s_style_user_bubble;
static lv_style_t  s_style_ai_bubble;
static lv_style_t  s_style_tool_bubble;
static lv_style_t  s_style_system_bubble;
static lv_style_t  s_style_timestamp;
static lv_style_t  s_style_card_bubble;
static bool        s_styles_inited = false;

/* ── Style initialization ──────────────────────────────────────── */

static void init_styles(void)
{
    if (s_styles_inited) return;
    s_styles_inited = true;

    /* User bubble — amber bg, black text */
    lv_style_init(&s_style_user_bubble);
    lv_style_set_bg_color(&s_style_user_bubble, lv_color_hex(0xF5A623));
    lv_style_set_bg_opa(&s_style_user_bubble, LV_OPA_COVER);
    lv_style_set_radius(&s_style_user_bubble, 16);
    lv_style_set_pad_all(&s_style_user_bubble, BUBBLE_PAD);
    lv_style_set_pad_bottom(&s_style_user_bubble, 8);
    lv_style_set_border_width(&s_style_user_bubble, 0);
    lv_style_set_text_color(&s_style_user_bubble, lv_color_hex(0x000000));

    /* AI bubble — indigo bg, white text, subtle border */
    lv_style_init(&s_style_ai_bubble);
    lv_style_set_bg_color(&s_style_ai_bubble, lv_color_hex(0x2A2A3E));
    lv_style_set_bg_opa(&s_style_ai_bubble, LV_OPA_COVER);
    lv_style_set_radius(&s_style_ai_bubble, 16);
    lv_style_set_pad_all(&s_style_ai_bubble, BUBBLE_PAD);
    lv_style_set_pad_bottom(&s_style_ai_bubble, 8);
    lv_style_set_border_width(&s_style_ai_bubble, 1);
    lv_style_set_border_color(&s_style_ai_bubble, lv_color_hex(0x333333));
    lv_style_set_text_color(&s_style_ai_bubble, lv_color_hex(0xFFFFFF));

    /* Card bubble — indigo bg with left orange border */
    lv_style_init(&s_style_card_bubble);
    lv_style_set_bg_color(&s_style_card_bubble, lv_color_hex(0x2A2A3E));
    lv_style_set_bg_opa(&s_style_card_bubble, LV_OPA_COVER);
    lv_style_set_radius(&s_style_card_bubble, 12);
    lv_style_set_pad_all(&s_style_card_bubble, BUBBLE_PAD);
    lv_style_set_pad_left(&s_style_card_bubble, BUBBLE_PAD + 6);
    lv_style_set_border_width(&s_style_card_bubble, 1);
    lv_style_set_border_color(&s_style_card_bubble, lv_color_hex(0xff6b35));
    lv_style_set_border_side(&s_style_card_bubble, LV_BORDER_SIDE_LEFT);
    lv_style_set_text_color(&s_style_card_bubble, lv_color_hex(0xFFFFFF));

    /* Tool status bubble — dark bg, centered cyan text */
    lv_style_init(&s_style_tool_bubble);
    lv_style_set_bg_color(&s_style_tool_bubble, lv_color_hex(0x1A1A2E));
    lv_style_set_bg_opa(&s_style_tool_bubble, LV_OPA_COVER);
    lv_style_set_radius(&s_style_tool_bubble, 12);
    lv_style_set_pad_all(&s_style_tool_bubble, 10);
    lv_style_set_border_width(&s_style_tool_bubble, 0);
    lv_style_set_text_color(&s_style_tool_bubble, lv_color_hex(0x00E5FF));
    lv_style_set_text_align(&s_style_tool_bubble, LV_TEXT_ALIGN_CENTER);

    /* System bubble — dark bg, centered dim text */
    lv_style_init(&s_style_system_bubble);
    lv_style_set_bg_color(&s_style_system_bubble, lv_color_hex(0x1A1A2E));
    lv_style_set_bg_opa(&s_style_system_bubble, LV_OPA_COVER);
    lv_style_set_radius(&s_style_system_bubble, 12);
    lv_style_set_pad_all(&s_style_system_bubble, 10);
    lv_style_set_border_width(&s_style_system_bubble, 0);
    lv_style_set_text_color(&s_style_system_bubble, lv_color_hex(0x555555));
    lv_style_set_text_align(&s_style_system_bubble, LV_TEXT_ALIGN_CENTER);

    /* Timestamp — small font, right-aligned */
    lv_style_init(&s_style_timestamp);
    lv_style_set_text_font(&s_style_timestamp, FONT_CAPTION);
    lv_style_set_text_align(&s_style_timestamp, LV_TEXT_ALIGN_RIGHT);
}

/* ── Height estimation ─────────────────────────────────────────── */

static int estimate_height(const chat_msg_t *msg)
{
    if (!msg) return DPI_SCALE(60);

    /* Use cached height if available */
    if (msg->height_px > 0) return msg->height_px;

    switch (msg->type) {
        case MSG_IMAGE:
            return DPI_SCALE(200);
        case MSG_AUDIO_CLIP:
            return DPI_SCALE(56);
        default:
            break;
    }

    /* Text-based height estimate: base + lines */
    int text_len = (int)strlen(msg->text);
    return DPI_SCALE(60) + (text_len / 40) * DPI_SCALE(20);
}

/* ── Slot configuration ────────────────────────────────────────── */

static void configure_slot(msg_slot_t *slot, const chat_msg_t *msg, int y_pos)
{
    if (!slot || !msg) return;

    /* Determine X position: user = right, AI/others = left */
    int x_pos = msg->is_user ? (DISPLAY_W - BUBBLE_MAX_W - 20) : 20;

    /* Remove all existing styles from container, then apply the right one */
    lv_obj_remove_style_all(slot->container);

    /* Apply type-specific style */
    switch (msg->type) {
        case MSG_CARD:
            lv_obj_add_style(slot->container, &s_style_card_bubble, 0);
            break;
        case MSG_TOOL_STATUS:
            lv_obj_add_style(slot->container, &s_style_tool_bubble, 0);
            x_pos = (DISPLAY_W - BUBBLE_MAX_W) / 2;  /* centered */
            break;
        case MSG_SYSTEM:
            lv_obj_add_style(slot->container, &s_style_system_bubble, 0);
            x_pos = (DISPLAY_W - BUBBLE_MAX_W) / 2;  /* centered */
            break;
        default:
            if (msg->is_user) {
                lv_obj_add_style(slot->container, &s_style_user_bubble, 0);
            } else {
                lv_obj_add_style(slot->container, &s_style_ai_bubble, 0);
            }
            break;
    }

    /* Position and size */
    lv_obj_set_pos(slot->container, x_pos, y_pos);
    lv_obj_set_size(slot->container, BUBBLE_MAX_W, LV_SIZE_CONTENT);
    lv_obj_clear_flag(slot->container, LV_OBJ_FLAG_SCROLLABLE);

    /* Content label */
    switch (msg->type) {
        case MSG_IMAGE:
            lv_label_set_text(slot->content, "Loading...");
            lv_obj_set_style_text_font(slot->content, FONT_SMALL, 0);
            lv_obj_set_style_text_color(slot->content, lv_color_hex(0x888888), 0);
            lv_obj_set_style_text_align(slot->content, LV_TEXT_ALIGN_CENTER, 0);
            break;
        case MSG_CARD:
            /* Show subtitle in content label for cards */
            lv_label_set_text(slot->content, msg->subtitle[0] ? msg->subtitle : msg->text);
            lv_obj_set_style_text_font(slot->content, FONT_BODY, 0);
            lv_obj_set_style_text_color(slot->content, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_align(slot->content, LV_TEXT_ALIGN_LEFT, 0);
            break;
        case MSG_AUDIO_CLIP:
            /* Play icon + duration placeholder */
            {
                char audio_text[64];
                char clip_label[48];
                if (msg->text[0]) {
                    strncpy(clip_label, msg->text, sizeof(clip_label) - 1);
                    clip_label[sizeof(clip_label) - 1] = '\0';
                } else {
                    strcpy(clip_label, "Audio clip");
                }
                snprintf(audio_text, sizeof(audio_text),
                         LV_SYMBOL_PLAY "  %s", clip_label);
                lv_label_set_text(slot->content, audio_text);
            }
            lv_obj_set_style_text_font(slot->content, FONT_BODY, 0);
            lv_obj_set_style_text_color(slot->content, lv_color_hex(0x22C55E), 0);
            lv_obj_set_style_text_align(slot->content, LV_TEXT_ALIGN_LEFT, 0);
            break;
        case MSG_TOOL_STATUS:
            lv_label_set_text(slot->content, msg->text);
            lv_obj_set_style_text_font(slot->content, FONT_SMALL, 0);
            lv_obj_set_style_text_color(slot->content, lv_color_hex(0x00E5FF), 0);
            lv_obj_set_style_text_align(slot->content, LV_TEXT_ALIGN_CENTER, 0);
            break;
        case MSG_SYSTEM:
            lv_label_set_text(slot->content, msg->text);
            lv_obj_set_style_text_font(slot->content, FONT_SMALL, 0);
            lv_obj_set_style_text_color(slot->content, lv_color_hex(0x555555), 0);
            lv_obj_set_style_text_align(slot->content, LV_TEXT_ALIGN_CENTER, 0);
            break;
        default:
            /* MSG_TEXT */
            lv_label_set_text(slot->content, msg->text);
            lv_obj_set_style_text_font(slot->content, FONT_BODY, 0);
            lv_obj_set_style_text_color(slot->content,
                lv_color_hex(msg->is_user ? 0x000000 : 0xFFFFFF), 0);
            lv_obj_set_style_text_align(slot->content, LV_TEXT_ALIGN_LEFT, 0);
            break;
    }

    lv_label_set_long_mode(slot->content, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(slot->content, LABEL_MAX_W);

    /* Timestamp */
    if (msg->timestamp > 0) {
        time_t ts = (time_t)msg->timestamp;
        struct tm *tm_info = localtime(&ts);
        if (tm_info) {
            char time_buf[8];
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
                     tm_info->tm_hour, tm_info->tm_min);
            lv_label_set_text(slot->timestamp, time_buf);
        } else {
            lv_label_set_text(slot->timestamp, "");
        }
        lv_obj_set_style_text_color(slot->timestamp,
            lv_color_hex(msg->is_user ? 0x555555 : 0x888888), 0);
        lv_obj_clear_flag(slot->timestamp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(slot->timestamp, LABEL_MAX_W);
        /* Place timestamp below content with 4px gap */
        lv_obj_align_to(slot->timestamp, slot->content,
                        LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);
    } else {
        lv_obj_add_flag(slot->timestamp, LV_OBJ_FLAG_HIDDEN);
    }

    /* Show the slot */
    lv_obj_clear_flag(slot->container, LV_OBJ_FLAG_HIDDEN);
}

/* ── Hide all pool slots ───────────────────────────────────────── */

static void hide_all_slots(void)
{
    for (int i = 0; i < BSP_CHAT_POOL_SIZE; i++) {
        lv_obj_add_flag(s_pool[i].container, LV_OBJ_FLAG_HIDDEN);
        s_pool[i].data_idx = -1;
    }
}

/* ── Find a free pool slot ─────────────────────────────────────── */

static msg_slot_t *find_free_slot(void)
{
    for (int i = 0; i < BSP_CHAT_POOL_SIZE; i++) {
        if (s_pool[i].data_idx == -1) {
            return &s_pool[i];
        }
    }
    return NULL;
}

/* ── Scroll event handler — recycles slots on scroll ───────────── */

static void scroll_event_cb(lv_event_t *e)
{
    (void)e;
    chat_view_refresh();
}

/* ── Public API ────────────────────────────────────────────────── */

void chat_view_init(lv_obj_t *parent)
{
    if (!parent) return;

    init_styles();

    /* Create scrollable container */
    s_scroll = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, DISPLAY_W, MSG_AREA_H);
    lv_obj_set_pos(s_scroll, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_scroll, 0, 0);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_OFF);

    /* Register scroll event for recycling */
    lv_obj_add_event_cb(s_scroll, scroll_event_cb, LV_EVENT_SCROLL, NULL);

    /* Create pool slots — all start hidden */
    for (int i = 0; i < BSP_CHAT_POOL_SIZE; i++) {
        msg_slot_t *slot = &s_pool[i];

        /* Container (bubble background) */
        slot->container = lv_obj_create(s_scroll);
        lv_obj_remove_style_all(slot->container);
        lv_obj_set_size(slot->container, BUBBLE_MAX_W, LV_SIZE_CONTENT);
        lv_obj_clear_flag(slot->container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(slot->container, LV_OBJ_FLAG_HIDDEN);

        /* Content label */
        slot->content = lv_label_create(slot->container);
        lv_label_set_long_mode(slot->content, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(slot->content, LABEL_MAX_W);
        lv_obj_set_style_text_font(slot->content, FONT_BODY, 0);

        /* Timestamp label */
        slot->timestamp = lv_label_create(slot->container);
        lv_obj_add_style(slot->timestamp, &s_style_timestamp, 0);
        lv_obj_set_width(slot->timestamp, LABEL_MAX_W);
        lv_obj_add_flag(slot->timestamp, LV_OBJ_FLAG_HIDDEN);

        slot->data_idx = -1;
    }
}

void chat_view_set_mode(uint8_t mode)
{
    if (mode >= CHAT_MODE_COUNT) mode = 0;
    s_mode = mode;
    hide_all_slots();
    chat_view_refresh();
}

uint8_t chat_view_get_mode(void)
{
    return s_mode;
}

void chat_view_refresh(void)
{
    if (!s_scroll) return;

    int total = chat_store_count(s_mode);
    if (total == 0) {
        hide_all_slots();
        lv_obj_set_content_height(s_scroll, 0);
        return;
    }

    /* --- Pass 1: Calculate cumulative Y positions for all messages --- */
    /* Use a small static array sized to BSP_CHAT_MAX_MESSAGES */
    static int y_positions[BSP_CHAT_MAX_MESSAGES];
    int running_y = BUBBLE_GAP;   /* start with top gap */

    for (int i = 0; i < total; i++) {
        const chat_msg_t *msg = chat_store_get(s_mode, i);
        y_positions[i] = running_y;
        int h = estimate_height(msg);
        running_y += h + BUBBLE_GAP;
    }

    int total_height = running_y;

    /* Set virtual scroll content height */
    lv_obj_set_content_height(s_scroll, total_height);

    /* --- Pass 2: Determine visible range based on current scroll position --- */
    int scroll_y = lv_obj_get_scroll_y(s_scroll);
    int view_top = scroll_y - VIS_BUFFER;
    int view_bot = scroll_y + MSG_AREA_H + VIS_BUFFER;

    /* --- Pass 3: Mark all slots as free --- */
    /* But first, keep slots that are still in the visible range */
    for (int i = 0; i < BSP_CHAT_POOL_SIZE; i++) {
        if (s_pool[i].data_idx >= 0 && s_pool[i].data_idx < total) {
            int idx = s_pool[i].data_idx;
            int msg_y = y_positions[idx];
            int msg_h = estimate_height(chat_store_get(s_mode, idx));
            /* If slot is still visible, keep it */
            if (msg_y + msg_h >= view_top && msg_y <= view_bot) {
                continue;  /* keep this slot */
            }
        }
        /* Free this slot */
        lv_obj_add_flag(s_pool[i].container, LV_OBJ_FLAG_HIDDEN);
        s_pool[i].data_idx = -1;
    }

    /* --- Pass 4: Assign visible messages to pool slots --- */
    for (int i = 0; i < total; i++) {
        const chat_msg_t *msg = chat_store_get(s_mode, i);
        if (!msg) continue;

        int msg_y = y_positions[i];
        int msg_h = estimate_height(msg);

        /* Skip if outside visible range */
        if (msg_y + msg_h < view_top || msg_y > view_bot) continue;

        /* Check if already assigned to a slot */
        bool already = false;
        for (int s = 0; s < BSP_CHAT_POOL_SIZE; s++) {
            if (s_pool[s].data_idx == i) {
                already = true;
                break;
            }
        }
        if (already) continue;

        /* Find a free slot */
        msg_slot_t *slot = find_free_slot();
        if (!slot) break;  /* all slots used — shouldn't happen with 12 + buffer */

        slot->data_idx = i;
        configure_slot(slot, msg, msg_y);

        /* Cache measured height back to store */
        lv_obj_update_layout(slot->container);
        int measured = lv_obj_get_height(slot->container);
        if (measured > 0) {
            chat_msg_t *mut = chat_store_get_mut(s_mode, i);
            if (mut) mut->height_px = (int16_t)measured;
        }
    }
}

void chat_view_scroll_to_bottom(void)
{
    if (!s_scroll) return;
    lv_obj_scroll_to_y(s_scroll, lv_obj_get_scroll_y(s_scroll) +
                        lv_obj_get_content_height(s_scroll), LV_ANIM_ON);
    /* Use LVGL's built-in method for scrolling to the end */
    lv_obj_scroll_to_y(s_scroll, LV_COORD_MAX, LV_ANIM_ON);
}

void chat_view_append_streaming(const char *token)
{
    if (!token || !s_scroll) return;

    if (!s_streaming) {
        s_streaming = true;
        s_stream_len = 0;
        s_stream_buf[0] = '\0';
    }

    /* Append token to streaming buffer */
    int tok_len = (int)strlen(token);
    if (s_stream_len + tok_len < (int)sizeof(s_stream_buf) - 1) {
        memcpy(s_stream_buf + s_stream_len, token, tok_len);
        s_stream_len += tok_len;
        s_stream_buf[s_stream_len] = '\0';
    }

    /* Find the streaming slot — last slot with highest data_idx,
     * or create one if needed by using the last message index */
    int total = chat_store_count(s_mode);
    int last_idx = total > 0 ? total - 1 : -1;

    msg_slot_t *stream_slot = NULL;
    for (int i = 0; i < BSP_CHAT_POOL_SIZE; i++) {
        if (s_pool[i].data_idx == last_idx) {
            stream_slot = &s_pool[i];
            break;
        }
    }

    if (!stream_slot) {
        /* Try to get a free slot and assign to last message */
        stream_slot = find_free_slot();
        if (stream_slot && last_idx >= 0) {
            stream_slot->data_idx = last_idx;
            const chat_msg_t *msg = chat_store_get(s_mode, last_idx);
            if (msg) {
                /* Position at bottom */
                int y = 0;
                int running = BUBBLE_GAP;
                for (int m = 0; m < total; m++) {
                    if (m == last_idx) { y = running; break; }
                    running += estimate_height(chat_store_get(s_mode, m)) + BUBBLE_GAP;
                }
                configure_slot(stream_slot, msg, y);
            }
        }
    }

    /* Update the streaming slot's label text */
    if (stream_slot) {
        lv_label_set_text(stream_slot->content, s_stream_buf);
        lv_obj_clear_flag(stream_slot->container, LV_OBJ_FLAG_HIDDEN);
    }

    /* Auto-scroll to bottom */
    chat_view_scroll_to_bottom();
}

void chat_view_finalize_streaming(void)
{
    s_streaming = false;
    s_stream_buf[0] = '\0';
    s_stream_len = 0;
    /* The orchestrator commits the accumulated text to the store.
     * Refresh to recalculate heights and positions. */
    chat_view_refresh();
}

bool chat_view_is_streaming(void)
{
    return s_streaming;
}

lv_obj_t *chat_view_get_scroll(void)
{
    return s_scroll;
}
