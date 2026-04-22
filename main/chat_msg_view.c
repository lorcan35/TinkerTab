/**
 * TinkerTab — Chat Message View (v4·C Ambient).
 *
 * Fixed pool of BSP_CHAT_POOL_SIZE slots. Each slot has a bubble
 * container, body label, and a timestamp label. Break-outs (code blocks,
 * images, cards) get a second full-bleed container lazily created when
 * the slot binds to a non-text message.
 *
 * Visual spec §3–§4:
 *   - Bubble radius 22, user amber bg, AI card bg + 1 px border
 *   - Tails: user bottom-right radius 6, AI bottom-left radius 6
 *   - Timestamps: FONT_CHAT_MONO, uppercase, dim
 *   - Break-out: full-bleed (-SIDE_PAD margin), 1 px top/bottom border,
 *     3 px amber bar top-left, mode-inherited color
 */
#include "chat_msg_view.h"
#include "chat_msg_store.h"
#include "ui_theme.h"
#include "config.h"
#include "bsp_config.h"
#include "media_cache.h"
#include "esp_log.h"

#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG __attribute__((unused)) = "chat_view";

/* ── Layout ────────────────────────────────────────────────────── */
#define SIDE_PAD        40
#define BUBBLE_MAX_W    520
#define BUBBLE_PAD_H    22
#define BUBBLE_PAD_V    16
#define BUBBLE_RADIUS   22
#define BUBBLE_TAIL_R    6
/* closes #109: bump from 20 to 36 so the timestamp label (placed at
 * `y + bh + 4` with font FONT_CHAT_MONO ~18 px tall + descenders)
 * doesn't bleed over the next bubble's top edge.  estimate_height
 * is a heuristic only — the real bubble uses LV_SIZE_CONTENT and can
 * exceed the estimate, which means the previous layout with GAP=20
 * routinely collided under wrapping text. */
#define BUBBLE_GAP      36
#define BUBBLE_TS_H     22   /* reserved height for the timestamp line */
#define LABEL_MAX_W    (BUBBLE_MAX_W - 2 * BUBBLE_PAD_H)

#define BREAK_H         168   /* default break-out body height for text/code */
#define BREAK_IMG_H     260
#define BREAK_IMG_R      10

#define VIS_BUFFER      480

/* ── Pool slot ─────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *bubble;       /* text/bubble container */
    lv_obj_t *body;         /* primary label */
    lv_obj_t *ts;           /* timestamp label */
    lv_obj_t *breakout;     /* optional full-bleed band (created on demand) */
    lv_obj_t *brk_kicker;   /* kicker label inside breakout */
    lv_obj_t *brk_body;     /* body label (mono for code, regular for image alt) */
    lv_obj_t *brk_accent;   /* 3 px amber bar top-left of breakout */
    lv_obj_t *brk_image;    /* MSG_IMAGE decoded JPEG, lazy-created (Tab5 audit D6) */
    lv_image_dsc_t brk_dsc; /* decoded pixel buffer owned by media_cache */
    char      brk_url[256]; /* the URL currently bound to brk_image */
    bool      brk_fetch_inflight;
    int       data_idx;     /* store logical index bound to this slot, -1 = empty */
} msg_slot_t;

struct chat_msg_view {
    lv_obj_t *scroll;
    msg_slot_t pool[BSP_CHAT_POOL_SIZE];
    uint32_t   mode_color;
    bool       streaming;
    int        stream_idx;
    char       stream_buf[2048];
    int        stream_len;
};

/* ── Helpers ───────────────────────────────────────────────────── */

static int estimate_height(const chat_msg_t *msg)
{
    if (!msg) return 60;
    if (msg->height_px > 0) return msg->height_px;
    switch (msg->type) {
        case MSG_IMAGE:      return BREAK_IMG_H + 48;
        case MSG_CARD:       return BREAK_H + 24;
        case MSG_AUDIO_CLIP: return 56 + 24;
        case MSG_SYSTEM:     return 40;
        default: break;
    }
    int text_len = (int)strlen(msg->text);
    /* 60 base + 22 per logical line (~36 chars). */
    return 60 + (text_len / 36) * 22 + 24;
}

static void fmt_timestamp(char *buf, size_t n, uint32_t ts, bool is_user)
{
    int h = (int)((ts / 3600) % 24);
    int m = (int)((ts / 60) % 60);
    snprintf(buf, n, "%02d:%02d \xc2\xb7 %s", h, m, is_user ? "YOU" : "TINKER");
    for (char *p = buf; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    }
}

static bool msg_is_breakout(const chat_msg_t *m)
{
    return m && (m->type == MSG_IMAGE || m->type == MSG_CARD || m->type == MSG_AUDIO_CLIP);
}

/* ── MSG_IMAGE async JPEG decode (Tab5 audit D6) ──────────────────
 * CLAUDE.md's "Dragon renders code → Tab5 decodes JPEG inline" claim
 * had only ever been wired for the home widget_media slot; the chat
 * MSG_IMAGE path rendered a LV_SYMBOL_IMAGE+alt placeholder. Mirror
 * the ui_home.c pattern: spawn a fetch task on Core 1, hop back via
 * lv_async_call to bind the decoded dsc onto the slot's lv_image. */

typedef struct {
    msg_slot_t *slot;      /* slot that kicked the fetch */
    int         expected_idx;  /* slot->data_idx at kick time */
    char        url[256];
    lv_image_dsc_t dsc;    /* filled by fetch task */
    bool        ok;
} chat_media_ctx_t;

static void chat_media_bind_cb(void *arg)
{
    chat_media_ctx_t *c = (chat_media_ctx_t *)arg;
    if (!c) return;
    msg_slot_t *slot = c->slot;
    /* Verify the slot is still bound to the same message it was when
     * we kicked off the fetch.  If the user scrolled / the pool got
     * recycled, discard silently — the next refresh will re-kick for
     * the now-current URL. */
    if (slot && slot->data_idx == c->expected_idx &&
        strncmp(slot->brk_url, c->url, sizeof(slot->brk_url)) == 0 &&
        c->ok && c->dsc.data && c->dsc.header.w > 0) {
        slot->brk_dsc = c->dsc;
        if (slot->brk_image) {
            int w = c->dsc.header.w, h = c->dsc.header.h;
            if (w > 720 - 2 * SIDE_PAD) w = 720 - 2 * SIDE_PAD;
            if (h > BREAK_IMG_H - 8)    h = BREAK_IMG_H - 8;
            lv_obj_set_size(slot->brk_image, w, h);
            lv_obj_set_pos(slot->brk_image, (720 - w) / 2, 40);
            lv_image_set_src(slot->brk_image, &slot->brk_dsc);
            lv_obj_clear_flag(slot->brk_image, LV_OBJ_FLAG_HIDDEN);
            if (slot->brk_body) lv_obj_add_flag(slot->brk_body, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (slot) slot->brk_fetch_inflight = false;
    free(c);
}

static void chat_media_fetch_task(void *pv)
{
    chat_media_ctx_t *c = (chat_media_ctx_t *)pv;
    if (!c) { vTaskSuspend(NULL)  /* wave 13 C4: P4 TLSP crash on delete — suspend instead */; return; }
    esp_err_t err = media_cache_fetch(c->url, &c->dsc);
    c->ok = (err == ESP_OK && c->dsc.data && c->dsc.header.w > 0);
    lv_async_call(chat_media_bind_cb, c);
    vTaskSuspend(NULL)  /* wave 13 C4: P4 TLSP crash on delete — suspend instead */;
}

static void kick_chat_image_fetch(msg_slot_t *slot, const char *url)
{
    if (!slot || !url || !url[0]) return;
    /* Same URL already bound or in-flight — no-op. */
    if (strncmp(slot->brk_url, url, sizeof(slot->brk_url)) == 0 &&
        (slot->brk_dsc.data || slot->brk_fetch_inflight)) {
        return;
    }
    snprintf(slot->brk_url, sizeof(slot->brk_url), "%s", url);
    memset(&slot->brk_dsc, 0, sizeof(slot->brk_dsc));

    chat_media_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return;
    c->slot = slot;
    c->expected_idx = slot->data_idx;
    snprintf(c->url, sizeof(c->url), "%s", url);
    slot->brk_fetch_inflight = true;
    BaseType_t ok = xTaskCreatePinnedToCore(
        chat_media_fetch_task, "chat_media", 8192, c, 3, NULL, 1);
    if (ok != pdPASS) {
        slot->brk_fetch_inflight = false;
        free(c);
    }
}

/* ── Slot reset ────────────────────────────────────────────────── */

static void slot_reset(msg_slot_t *slot)
{
    if (!slot) return;
    slot->data_idx = -1;
    if (slot->bubble)    lv_obj_add_flag(slot->bubble, LV_OBJ_FLAG_HIDDEN);
    if (slot->breakout)  lv_obj_add_flag(slot->breakout, LV_OBJ_FLAG_HIDDEN);
}

static void slot_ensure_breakout(msg_slot_t *slot, lv_obj_t *parent, uint32_t accent)
{
    if (!slot) return;
    if (slot->breakout) return;

    slot->breakout = lv_obj_create(parent);
    lv_obj_remove_style_all(slot->breakout);
    lv_obj_set_size(slot->breakout, 720, BREAK_H);
    lv_obj_set_style_bg_color(slot->breakout, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(slot->breakout, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(slot->breakout, 1, 0);
    lv_obj_set_style_border_color(slot->breakout, lv_color_hex(0x1E1E2A), 0);
    lv_obj_set_style_border_side(slot->breakout,
        LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(slot->breakout, SIDE_PAD, 0);
    lv_obj_set_style_pad_ver(slot->breakout, 20, 0);
    lv_obj_clear_flag(slot->breakout, LV_OBJ_FLAG_SCROLLABLE);

    slot->brk_accent = lv_obj_create(slot->breakout);
    lv_obj_remove_style_all(slot->brk_accent);
    lv_obj_set_size(slot->brk_accent, 140, 3);
    lv_obj_set_pos(slot->brk_accent, SIDE_PAD, 0);
    lv_obj_set_style_bg_color(slot->brk_accent, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(slot->brk_accent, LV_OPA_COVER, 0);

    slot->brk_kicker = lv_label_create(slot->breakout);
    lv_obj_set_style_text_font(slot->brk_kicker, FONT_CHAT_MONO, 0);
    lv_obj_set_style_text_color(slot->brk_kicker, lv_color_hex(accent), 0);
    lv_obj_set_style_text_letter_space(slot->brk_kicker, 2, 0);
    lv_label_set_text(slot->brk_kicker, "");

    slot->brk_body = lv_label_create(slot->breakout);
    lv_label_set_long_mode(slot->brk_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(slot->brk_body, 720 - 2 * SIDE_PAD);
    lv_obj_set_style_text_color(slot->brk_body, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(slot->brk_body, FONT_CHAT_MONO, 0);
    lv_obj_set_style_text_line_space(slot->brk_body, 4, 0);
    lv_label_set_text(slot->brk_body, "");
}

/* ── Bind slot to a message ────────────────────────────────────── */

static void slot_bind(chat_msg_view_t *v, msg_slot_t *slot,
                      const chat_msg_t *msg, int y)
{
    if (!slot || !msg) return;
    slot->data_idx = -1;       /* set later after successful bind */

    bool use_breakout = msg_is_breakout(msg);
    if (use_breakout) {
        /* Break-out band — full screen width, absolute x=0. */
        slot_ensure_breakout(slot, v->scroll, v->mode_color);
        lv_obj_clear_flag(slot->breakout, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(slot->breakout, 0, y);
        lv_obj_add_flag(slot->bubble, LV_OBJ_FLAG_HIDDEN);
        if (slot->brk_accent)
            lv_obj_set_style_bg_color(slot->brk_accent, lv_color_hex(v->mode_color), 0);
        if (slot->brk_kicker)
            lv_obj_set_style_text_color(slot->brk_kicker, lv_color_hex(v->mode_color), 0);

        char kicker[64];
        switch (msg->type) {
            case MSG_IMAGE:
                snprintf(kicker, sizeof(kicker), "IMAGE \xc2\xb7 %.40s",
                         msg->subtitle[0] ? msg->subtitle : "INLINE");
                break;
            case MSG_CARD:
                snprintf(kicker, sizeof(kicker), "CARD \xc2\xb7 %.40s",
                         msg->subtitle[0] ? msg->subtitle : "PREVIEW");
                break;
            case MSG_AUDIO_CLIP:
                snprintf(kicker, sizeof(kicker), "AUDIO \xc2\xb7 CLIP");
                break;
            default:
                kicker[0] = 0;
                break;
        }
        for (char *p = kicker; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        }
        lv_label_set_text(slot->brk_kicker, kicker);
        lv_obj_set_pos(slot->brk_kicker, SIDE_PAD, 12);

        /* Body — for images we render a placeholder pane with alt text
         * WHILE the JPEG is being fetched/decoded, and once the dsc is
         * bound we hide the placeholder and show the decoded bitmap.
         * (Tab5 audit D6 fix — previously only the placeholder branch
         *  ever rendered, so the "Dragon renders → Tab5 decodes inline"
         *  claim was invisible.) */
        if (msg->type == MSG_IMAGE) {
            lv_obj_set_style_text_font(slot->brk_body, FONT_BODY, 0);
            lv_obj_set_style_text_color(slot->brk_body, lv_color_hex(TH_TEXT_DIM), 0);
            char body[256];
            snprintf(body, sizeof(body),
                     LV_SYMBOL_IMAGE "  %.220s",
                     msg->text[0] ? msg->text : "inline image");
            lv_label_set_text(slot->brk_body, body);
            lv_obj_set_pos(slot->brk_body, SIDE_PAD, 40);
            lv_obj_set_size(slot->breakout, 720, BREAK_IMG_H);
            lv_obj_set_style_radius(slot->breakout, 0, 0);

            /* Lazy-create the image widget once per slot. */
            if (!slot->brk_image) {
                slot->brk_image = lv_image_create(slot->breakout);
                lv_obj_add_flag(slot->brk_image, LV_OBJ_FLAG_HIDDEN);
            }
            /* If a decoded dsc is already bound AND the URL is unchanged,
             * just show it immediately without re-fetching. */
            if (msg->media_url[0] &&
                strncmp(slot->brk_url, msg->media_url, sizeof(slot->brk_url)) == 0 &&
                slot->brk_dsc.data && slot->brk_dsc.header.w > 0) {
                int w = slot->brk_dsc.header.w, h = slot->brk_dsc.header.h;
                if (w > 720 - 2 * SIDE_PAD) w = 720 - 2 * SIDE_PAD;
                if (h > BREAK_IMG_H - 8)    h = BREAK_IMG_H - 8;
                lv_obj_set_size(slot->brk_image, w, h);
                lv_obj_set_pos(slot->brk_image, (720 - w) / 2, 40);
                lv_image_set_src(slot->brk_image, &slot->brk_dsc);
                lv_obj_clear_flag(slot->brk_image, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(slot->brk_body, LV_OBJ_FLAG_HIDDEN);
            } else {
                /* Not yet decoded — hide image, leave body as the loading
                 * placeholder ("🖼  alt text").  The refresh loop below
                 * kicks the fetch after slot->data_idx is committed, so
                 * the async bind callback can verify idx-match. */
                lv_obj_add_flag(slot->brk_image, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(slot->brk_body, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (msg->type == MSG_CARD) {
            lv_obj_set_style_text_font(slot->brk_body, FONT_BODY, 0);
            lv_obj_set_style_text_color(slot->brk_body, lv_color_hex(TH_TEXT_PRIMARY), 0);
            char body[400];
            if (msg->subtitle[0] && msg->text[0]) {
                snprintf(body, sizeof(body), "%.120s\n%.200s", msg->text, msg->subtitle);
            } else if (msg->text[0]) {
                snprintf(body, sizeof(body), "%.300s", msg->text);
            } else {
                body[0] = 0;
            }
            lv_label_set_text(slot->brk_body, body);
            lv_obj_set_pos(slot->brk_body, SIDE_PAD, 40);
            lv_obj_set_size(slot->breakout, 720, BREAK_H);
        } else { /* MSG_AUDIO_CLIP */
            lv_obj_set_style_text_font(slot->brk_body, FONT_BODY, 0);
            lv_obj_set_style_text_color(slot->brk_body, lv_color_hex(TH_AMBER), 0);
            char body[128];
            snprintf(body, sizeof(body), LV_SYMBOL_PLAY "  %.100s",
                     msg->text[0] ? msg->text : "audio clip");
            lv_label_set_text(slot->brk_body, body);
            lv_obj_set_pos(slot->brk_body, SIDE_PAD, 40);
            lv_obj_set_size(slot->breakout, 720, 96);
        }

        /* Measure + cache. */
        lv_obj_update_layout(slot->breakout);
        int measured = lv_obj_get_height(slot->breakout);
        if (measured > 0) chat_store_set_height(slot->data_idx, (int16_t)measured);
        slot->data_idx = -2;   /* marker fixed up by caller */
        return;
    }

    /* Regular bubble path (MSG_TEXT + MSG_SYSTEM). */
    lv_obj_clear_flag(slot->bubble, LV_OBJ_FLAG_HIDDEN);
    if (slot->breakout) lv_obj_add_flag(slot->breakout, LV_OBJ_FLAG_HIDDEN);

    if (msg->type == MSG_SYSTEM) {
        /* Centered dim line, no bubble frame. */
        lv_obj_set_style_bg_opa(slot->bubble, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(slot->bubble, 0, 0);
        lv_obj_set_style_radius(slot->bubble, 0, 0);
        lv_obj_set_style_pad_all(slot->bubble, 0, 0);
        lv_obj_set_size(slot->bubble, BUBBLE_MAX_W, LV_SIZE_CONTENT);
        int x = (720 - BUBBLE_MAX_W) / 2;
        lv_obj_set_pos(slot->bubble, x, y);
        lv_obj_set_width(slot->body, BUBBLE_MAX_W);
        lv_obj_set_style_text_font(slot->body, FONT_CHAT_MONO, 0);
        lv_obj_set_style_text_color(slot->body, lv_color_hex(TH_TEXT_DIM), 0);
        lv_obj_set_style_text_align(slot->body, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(slot->body, msg->text);
        lv_obj_set_pos(slot->body, 0, 0);
        if (slot->ts) lv_obj_add_flag(slot->ts, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* MSG_TEXT bubble */
    lv_obj_set_style_bg_opa(slot->bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(slot->bubble, BUBBLE_PAD_H, 0);
    lv_obj_set_style_pad_ver(slot->bubble, BUBBLE_PAD_V, 0);

    if (msg->is_user) {
        lv_obj_set_style_bg_color(slot->bubble, lv_color_hex(TH_AMBER), 0);
        lv_obj_set_style_border_width(slot->bubble, 0, 0);
        /* Rounded-rect with a 6-px bottom-right tail. LVGL doesn't ship
         * per-corner radius, so we settle for the 22 radius — the tail
         * is approximated by a small 12×12 amber square tucked under
         * the bubble's bottom-right edge. */
        lv_obj_set_style_radius(slot->bubble, BUBBLE_RADIUS, 0);
        lv_obj_set_style_text_color(slot->body, lv_color_hex(TH_BG), 0);
    } else {
        lv_obj_set_style_bg_color(slot->bubble, lv_color_hex(TH_CARD), 0);
        /* Wave 15 W15-C09: was `border_width=1 + radius=22`.  LVGL's
         * draw_border_complex → lv_draw_mask_radius → get_next_line
         * path crashed drawing a 1 px border on a 22 px-rounded widget
         * (coredump captured this during second-turn chat render).
         * Same class of rasterizer bug as W15-C07 (voice overlay fade).
         * Dark card (0x111119) on near-black (0x08080E) is visibly
         * distinct without a border, so dropping it eliminates the
         * crash path while keeping the bubble readable. */
        lv_obj_set_style_border_width(slot->bubble, 0, 0);
        lv_obj_set_style_radius(slot->bubble, BUBBLE_RADIUS, 0);
        lv_obj_set_style_text_color(slot->body, lv_color_hex(TH_TEXT_PRIMARY), 0);
    }
    (void)BUBBLE_TAIL_R;   /* kept in header for future per-corner support */

    lv_obj_set_size(slot->bubble, BUBBLE_MAX_W, LV_SIZE_CONTENT);
    int x = msg->is_user ? (720 - BUBBLE_MAX_W - SIDE_PAD) : SIDE_PAD;
    lv_obj_set_pos(slot->bubble, x, y);

    /* Body */
    lv_obj_set_width(slot->body, LABEL_MAX_W);
    lv_obj_set_style_text_font(slot->body, FONT_BODY, 0);
    lv_obj_set_style_text_align(slot->body, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(slot->body, 4, 0);
    lv_label_set_text(slot->body, msg->text);

    /* Timestamp (+ Phase 3d receipt stamp for AI bubbles with cost).
     * Format: "9:42 · HAIKU-3.5 · $0.003"  (AI only, when receipt > 0). */
    if (slot->ts) {
        lv_obj_clear_flag(slot->ts, LV_OBJ_FLAG_HIDDEN);
        char ts[96];
        fmt_timestamp(ts, sizeof(ts),
                      msg->timestamp ? msg->timestamp : 0,
                      msg->is_user);
        if (!msg->is_user && msg->receipt_model_short[0]) {
            /* Phase 3d + 4a: stamp + optional Gauntlet G2 retry marker.
             *   cloud turn  : " · MODEL · $X.XXX"
             *   local turn  : " · MODEL · FREE"
             *   +retried    : prepend " · RETRIED" to either */
            size_t cur = strlen(ts);
            const char *retry_tag = msg->receipt_retried ? " \xc2\xb7 RETRIED" : "";
            if (msg->receipt_mils > 0) {
                int dollars     = (int)(msg->receipt_mils / 100000);
                int thousandths = (int)((msg->receipt_mils / 100) % 1000);
                snprintf(ts + cur, sizeof(ts) - cur,
                         " \xc2\xb7 %s \xc2\xb7 $%d.%03d%s",
                         msg->receipt_model_short,
                         dollars, thousandths, retry_tag);
            } else {
                snprintf(ts + cur, sizeof(ts) - cur,
                         " \xc2\xb7 %s \xc2\xb7 FREE%s",
                         msg->receipt_model_short, retry_tag);
            }
        }
        lv_label_set_text(slot->ts, ts);
        lv_obj_set_style_text_font(slot->ts, FONT_CHAT_MONO, 0);
        lv_obj_set_style_text_color(slot->ts, lv_color_hex(TH_TEXT_DIM), 0);
        /* Place below bubble, right-aligned for user, left for AI. */
        lv_obj_update_layout(slot->bubble);
        int bh = lv_obj_get_height(slot->bubble);
        int tx = msg->is_user
                   ? (720 - SIDE_PAD - 220)
                   : SIDE_PAD + 4;
        lv_obj_set_pos(slot->ts, tx, y + bh + 4);
        lv_obj_set_width(slot->ts, 220);
        lv_obj_set_style_text_align(slot->ts,
            msg->is_user ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    }
}

/* ── Hide all slots ────────────────────────────────────────────── */

static void hide_all_slots(chat_msg_view_t *v)
{
    for (int i = 0; i < BSP_CHAT_POOL_SIZE; i++) slot_reset(&v->pool[i]);
}

/* ── Find / scroll ─────────────────────────────────────────────── */

static void ev_scroll(lv_event_t *e)
{
    chat_msg_view_t *v = lv_event_get_user_data(e);
    chat_msg_view_refresh(v);
}

/* ── Public API ────────────────────────────────────────────────── */

chat_msg_view_t *chat_msg_view_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    chat_msg_view_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->mode_color = TH_AMBER;
    v->stream_idx = -1;

    v->scroll = lv_obj_create(parent);
    lv_obj_remove_style_all(v->scroll);
    lv_obj_set_size(v->scroll, w, h);
    lv_obj_set_pos(v->scroll, x, y);
    lv_obj_set_style_bg_opa(v->scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(v->scroll, 0, 0);
    lv_obj_add_flag(v->scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(v->scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(v->scroll, ev_scroll, LV_EVENT_SCROLL, v);

    for (int i = 0; i < BSP_CHAT_POOL_SIZE; i++) {
        msg_slot_t *slot = &v->pool[i];
        slot->bubble = lv_obj_create(v->scroll);
        lv_obj_remove_style_all(slot->bubble);
        lv_obj_set_size(slot->bubble, BUBBLE_MAX_W, LV_SIZE_CONTENT);
        lv_obj_clear_flag(slot->bubble, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(slot->bubble, LV_OBJ_FLAG_HIDDEN);

        slot->body = lv_label_create(slot->bubble);
        lv_label_set_long_mode(slot->body, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(slot->body, LABEL_MAX_W);
        lv_obj_set_style_text_font(slot->body, FONT_BODY, 0);

        slot->ts = lv_label_create(v->scroll);
        lv_obj_set_style_text_font(slot->ts, FONT_CHAT_MONO, 0);
        lv_obj_set_style_text_color(slot->ts, lv_color_hex(TH_TEXT_DIM), 0);
        lv_obj_add_flag(slot->ts, LV_OBJ_FLAG_HIDDEN);

        slot->data_idx = -1;
        slot->breakout = NULL;
    }

    return v;
}

void chat_msg_view_destroy(chat_msg_view_t *v)
{
    if (!v) return;
    if (v->scroll) lv_obj_del(v->scroll);
    free(v);
}

void chat_msg_view_set_size(chat_msg_view_t *v, int w, int h)
{
    if (v && v->scroll) lv_obj_set_size(v->scroll, w, h);
}

void chat_msg_view_set_mode_color(chat_msg_view_t *v, uint32_t hex)
{
    if (!v) return;
    v->mode_color = hex;
    chat_msg_view_refresh(v);
}

void chat_msg_view_refresh(chat_msg_view_t *v)
{
    if (!v || !v->scroll) return;

    int total = chat_store_count();
    if (total == 0) {
        hide_all_slots(v);
        lv_obj_set_content_height(v->scroll, 0);
        return;
    }

    /* Pass 1: cumulative Y. */
    static int y_positions[BSP_CHAT_MAX_MESSAGES];
    int running = BUBBLE_GAP;
    for (int i = 0; i < total; i++) {
        const chat_msg_t *m = chat_store_get(i);
        y_positions[i] = running;
        /* closes #109: reserve the timestamp row height inside each
         * bubble's slot so the next bubble doesn't start before the
         * current bubble's ts has room to render. */
        running += estimate_height(m) + BUBBLE_TS_H + BUBBLE_GAP;
    }
    lv_obj_set_content_height(v->scroll, running);

    int scroll_y = lv_obj_get_scroll_y(v->scroll);
    int view_top = scroll_y - VIS_BUFFER;
    int view_bot = scroll_y + lv_obj_get_height(v->scroll) + VIS_BUFFER;

    /* Pass 2: free slots outside window. */
    for (int i = 0; i < BSP_CHAT_POOL_SIZE; i++) {
        msg_slot_t *slot = &v->pool[i];
        if (slot->data_idx < 0 || slot->data_idx >= total) {
            if (v->streaming && slot->data_idx == v->stream_idx) continue;
            slot_reset(slot);
            continue;
        }
        int y = y_positions[slot->data_idx];
        int h = estimate_height(chat_store_get(slot->data_idx));
        if (y + h < view_top || y > view_bot) {
            if (v->streaming && slot->data_idx == v->stream_idx) continue;
            slot_reset(slot);
        }
    }

    /* Pass 3: bind visible messages to free slots. */
    for (int i = 0; i < total; i++) {
        int y = y_positions[i];
        int h = estimate_height(chat_store_get(i));
        if (y + h < view_top || y > view_bot) continue;

        /* Already bound? */
        bool bound = false;
        for (int s = 0; s < BSP_CHAT_POOL_SIZE; s++) {
            if (v->pool[s].data_idx == i) { bound = true; break; }
        }
        if (bound) continue;

        /* Find free slot */
        msg_slot_t *slot = NULL;
        for (int s = 0; s < BSP_CHAT_POOL_SIZE; s++) {
            if (v->pool[s].data_idx == -1) { slot = &v->pool[s]; break; }
        }
        if (!slot) break;

        slot_bind(v, slot, chat_store_get(i), y);
        slot->data_idx = i;

        /* After data_idx is committed, kick JPEG fetch for MSG_IMAGE
         * bubbles whose dsc isn't already bound to the current URL.
         * Running here (not inside slot_bind) guarantees expected_idx
         * matches what the refresh loop last bound. */
        {
            const chat_msg_t *m = chat_store_get(i);
            if (m && m->type == MSG_IMAGE && m->media_url[0]) {
                bool already = (strncmp(slot->brk_url, m->media_url,
                                        sizeof(slot->brk_url)) == 0 &&
                                slot->brk_dsc.data &&
                                slot->brk_dsc.header.w > 0);
                if (!already) {
                    kick_chat_image_fetch(slot, m->media_url);
                }
            }
        }

        /* Cache measured height for next refresh. */
        lv_obj_t *measure_obj = msg_is_breakout(chat_store_get(i)) ? slot->breakout : slot->bubble;
        if (measure_obj) {
            lv_obj_update_layout(measure_obj);
            int measured = lv_obj_get_height(measure_obj);
            if (measured > 0) chat_store_set_height(i, (int16_t)(measured + 28));
        }
    }
}

void chat_msg_view_scroll_to_bottom(chat_msg_view_t *v)
{
    if (v && v->scroll) lv_obj_scroll_to_y(v->scroll, LV_COORD_MAX, LV_ANIM_ON);
}

void chat_msg_view_begin_streaming(chat_msg_view_t *v)
{
    if (!v) return;
    v->streaming = true;
    v->stream_len = 0;
    v->stream_buf[0] = 0;
    v->stream_idx = chat_store_count() - 1;
}

void chat_msg_view_append_stream(chat_msg_view_t *v, const char *text)
{
    if (!v || !text) return;
    if (!v->streaming) chat_msg_view_begin_streaming(v);

    size_t tlen = strlen(text);
    if ((size_t)v->stream_len + tlen >= sizeof(v->stream_buf) - 1) {
        tlen = sizeof(v->stream_buf) - 1 - (size_t)v->stream_len;
    }
    if (tlen == 0) return;
    memcpy(v->stream_buf + v->stream_len, text, tlen);
    v->stream_len += (int)tlen;
    v->stream_buf[v->stream_len] = 0;

    /* Push into the store so the view refresh picks it up. */
    chat_store_update_last_text(v->stream_buf);
    chat_msg_view_refresh(v);
    chat_msg_view_scroll_to_bottom(v);
}

void chat_msg_view_end_streaming(chat_msg_view_t *v)
{
    if (!v) return;
    v->streaming = false;
    v->stream_idx = -1;
    v->stream_len = 0;
    v->stream_buf[0] = 0;
    chat_msg_view_refresh(v);
}

bool chat_msg_view_is_streaming(chat_msg_view_t *v)
{
    return v && v->streaming;
}

lv_obj_t *chat_msg_view_get_scroll(chat_msg_view_t *v)
{
    return v ? v->scroll : NULL;
}
