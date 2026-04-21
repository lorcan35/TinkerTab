/**
 * Chat Session Drawer — pull-down session list (chat v4·C §5.4).
 *
 *   [ gradient spine: Local | Hybrid | Cloud | Claw ]
 *   [ row: dot · MODE · MODEL · title · time ]    × up to 10
 *   [ + Start new conversation ]
 *
 * Fetch runs on a detached FreeRTOS task; results are applied on the
 * LVGL thread via lv_async_call — guarantees LVGL data structures
 * aren't touched from Core 1.
 */
#include "chat_session_drawer.h"
#include "task_worker.h"
#include "ui_theme.h"
#include "config.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "chat_drawer";

#define DRAWER_Y        96           /* sits below 96-h header */
#define DRAWER_W        720
#define DRAWER_H        800
#define SPINE_H         2
#define ROW_H           64
#define ROW_SIDE_PAD    40
#define DOT_SZ          12
#define MAX_ROWS        10

static const uint32_t s_mode_tint[4] = {
    TH_MODE_LOCAL, TH_MODE_HYBRID, TH_MODE_CLOUD, TH_MODE_CLAW,
};
static const char *s_mode_short[4] = { "LOCAL", "HYBRID", "CLOUD", "CLAW" };

typedef struct {
    lv_obj_t *row;
    lv_obj_t *dot;
    lv_obj_t *info;   /* MODE · MODEL kicker */
    lv_obj_t *title;
    lv_obj_t *time;
    lv_obj_t *left_bar; /* amber active marker */
    chat_session_t data;
    bool in_use;
} drawer_row_t;

struct chat_session_drawer {
    lv_obj_t *scrim;          /* black-50 backdrop behind the drawer */
    lv_obj_t *panel;          /* the sliding card */
    lv_obj_t *spine;          /* 720×2 4-segment gradient */
    lv_obj_t *loading;        /* "Loading…" label */
    drawer_row_t rows[MAX_ROWS];
    lv_obj_t *footer;
    bool      open;

    chat_drawer_pick_cb_t     pick_cb;    void *pick_ud;
    chat_drawer_new_cb_t      new_cb;     void *new_ud;
    chat_drawer_dismiss_cb_t  dismiss_cb; void *dismiss_ud;

    char active_session_id[CHAT_SESSION_ID_LEN];
    uint32_t fetch_gen;       /* incremented on each show; stale results dropped */
};

/* ── Forward decls ─────────────────────────────────────────────── */
typedef struct {
    chat_session_t sessions[MAX_ROWS];
    int            count;
    uint32_t       gen;
    chat_session_drawer_t *d;
} drawer_fetch_result_t;

static void render_loading(chat_session_drawer_t *d);
static void render_rows(chat_session_drawer_t *d,
                        const chat_session_t *list, int count);

/* ── Event trampolines ─────────────────────────────────────────── */

static void ev_row_click(lv_event_t *e)
{
    chat_session_drawer_t *d = (chat_session_drawer_t *)lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_target(e);
    if (!d) return;
    for (int i = 0; i < MAX_ROWS; i++) {
        if (d->rows[i].row == row && d->rows[i].in_use) {
            if (d->pick_cb) d->pick_cb(&d->rows[i].data, d->pick_ud);
            return;
        }
    }
}

static void ev_footer_click(lv_event_t *e)
{
    chat_session_drawer_t *d = (chat_session_drawer_t *)lv_event_get_user_data(e);
    if (d && d->new_cb) d->new_cb(d->new_ud);
}

static void ev_scrim_click(lv_event_t *e)
{
    chat_session_drawer_t *d = (chat_session_drawer_t *)lv_event_get_user_data(e);
    if (!d) return;
    /* Tap on scrim (outside panel) dismisses. */
    lv_obj_t *t = lv_event_get_target(e);
    if (t == d->scrim && d->dismiss_cb) d->dismiss_cb(d->dismiss_ud);
}

/* ── Row creation ──────────────────────────────────────────────── */

static void row_create(chat_session_drawer_t *d, drawer_row_t *r,
                       lv_obj_t *parent, int idx)
{
    r->row = lv_obj_create(parent);
    lv_obj_remove_style_all(r->row);
    lv_obj_set_size(r->row, DRAWER_W, ROW_H);
    lv_obj_set_pos(r->row, 0, idx * ROW_H);
    lv_obj_set_style_bg_color(r->row, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(r->row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r->row, 0, 0);
    lv_obj_set_style_border_side(r->row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r->row, 1, 0);
    lv_obj_set_style_border_color(r->row, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->row, ev_row_click, LV_EVENT_CLICKED, d);

    r->left_bar = lv_obj_create(r->row);
    lv_obj_remove_style_all(r->left_bar);
    lv_obj_set_size(r->left_bar, 3, ROW_H);
    lv_obj_set_pos(r->left_bar, 0, 0);
    lv_obj_set_style_bg_color(r->left_bar, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(r->left_bar, LV_OPA_COVER, 0);
    lv_obj_add_flag(r->left_bar, LV_OBJ_FLAG_HIDDEN);

    r->dot = lv_obj_create(r->row);
    lv_obj_remove_style_all(r->dot);
    lv_obj_set_size(r->dot, DOT_SZ, DOT_SZ);
    lv_obj_set_pos(r->dot, ROW_SIDE_PAD, (ROW_H - DOT_SZ) / 2);
    lv_obj_set_style_radius(r->dot, DOT_SZ / 2, 0);
    lv_obj_set_style_bg_color(r->dot, lv_color_hex(TH_MODE_LOCAL), 0);
    lv_obj_set_style_bg_opa(r->dot, LV_OPA_COVER, 0);

    int info_x = ROW_SIDE_PAD + DOT_SZ + 16;
    r->info = lv_label_create(r->row);
    lv_obj_set_style_text_font(r->info, FONT_CHAT_MONO, 0);
    lv_obj_set_style_text_color(r->info, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(r->info, 2, 0);
    lv_label_set_text(r->info, "");
    lv_obj_set_pos(r->info, info_x, 10);

    r->title = lv_label_create(r->row);
    lv_obj_set_style_text_font(r->title, FONT_BODY, 0);
    lv_obj_set_style_text_color(r->title, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_label_set_text(r->title, "");
    lv_obj_set_pos(r->title, info_x, 32);
    lv_obj_set_width(r->title, DRAWER_W - info_x - 120 - ROW_SIDE_PAD);
    lv_label_set_long_mode(r->title, LV_LABEL_LONG_DOT);

    r->time = lv_label_create(r->row);
    lv_obj_set_style_text_font(r->time, FONT_CHAT_MONO, 0);
    lv_obj_set_style_text_color(r->time, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_label_set_text(r->time, "");
    lv_obj_set_pos(r->time, DRAWER_W - ROW_SIDE_PAD - 80, 22);
    lv_obj_set_width(r->time, 80);
    lv_obj_set_style_text_align(r->time, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
}

/* ── Relative time helper ──────────────────────────────────────── */

static void format_relative(char *buf, size_t n, uint32_t updated_at)
{
    if (!updated_at) { snprintf(buf, n, "\xe2\x80\x93"); return; }
    time_t now = 0; time(&now);
    long delta = (long)now - (long)updated_at;
    if (delta < 0) delta = 0;
    if (delta < 60)                snprintf(buf, n, "NOW");
    else if (delta < 3600)         snprintf(buf, n, "%ldM", delta / 60);
    else if (delta < 86400)        snprintf(buf, n, "%ldH", delta / 3600);
    else                           snprintf(buf, n, "%ldD", delta / 86400);
}

/* ── Render loading / rows ─────────────────────────────────────── */

static void hide_all_rows(chat_session_drawer_t *d)
{
    for (int i = 0; i < MAX_ROWS; i++) {
        if (d->rows[i].row) lv_obj_add_flag(d->rows[i].row, LV_OBJ_FLAG_HIDDEN);
        d->rows[i].in_use = false;
    }
}

static void render_loading(chat_session_drawer_t *d)
{
    if (!d || !d->loading) return;
    hide_all_rows(d);
    lv_obj_clear_flag(d->loading, LV_OBJ_FLAG_HIDDEN);
}

static void render_rows(chat_session_drawer_t *d,
                        const chat_session_t *list, int count)
{
    if (!d) return;
    if (d->loading) lv_obj_add_flag(d->loading, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_ROWS; i++) {
        drawer_row_t *r = &d->rows[i];
        if (i >= count) {
            r->in_use = false;
            if (r->row) lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const chat_session_t *s = &list[i];
        r->data = *s;
        r->in_use = true;

        uint8_t mode = s->voice_mode <= 3 ? s->voice_mode : 0;
        lv_obj_set_style_bg_color(r->dot, lv_color_hex(s_mode_tint[mode]), 0);

        char info[96];
        const char *model = s->llm_model[0] ? s->llm_model : "default";
        /* strip provider prefix + :tag */
        const char *nick = model;
        const char *slash = strchr(nick, '/');
        if (slash) nick = slash + 1;
        char nickbuf[40];
        size_t nicklen = strlen(nick);
        if (nicklen >= sizeof(nickbuf)) nicklen = sizeof(nickbuf) - 1;
        memcpy(nickbuf, nick, nicklen); nickbuf[nicklen] = 0;
        char *colon = strchr(nickbuf, ':'); if (colon) *colon = 0;
        for (char *p = nickbuf; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);

        snprintf(info, sizeof(info), "%s \xc2\xb7 %s",
                 s_mode_short[mode], nickbuf[0] ? nickbuf : "\xe2\x80\x93");
        lv_label_set_text(r->info, info);

        lv_label_set_text(r->title, s->title[0] ? s->title : "Untitled");

        char t[24];
        format_relative(t, sizeof(t), s->updated_at);
        lv_label_set_text(r->time, t);

        bool active = d->active_session_id[0] &&
                      strncmp(d->active_session_id, s->session_id,
                              CHAT_SESSION_ID_LEN) == 0;
        if (r->left_bar) {
            if (active) lv_obj_clear_flag(r->left_bar, LV_OBJ_FLAG_HIDDEN);
            else        lv_obj_add_flag(r->left_bar, LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_clear_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Async "render after fetch" ────────────────────────────────── */

static void on_result_async(void *arg)
{
    drawer_fetch_result_t *res = (drawer_fetch_result_t *)arg;
    if (!res) return;
    if (res->d && res->gen == res->d->fetch_gen && res->d->open) {
        render_rows(res->d, res->sessions, res->count);
    } else {
        ESP_LOGD(TAG, "stale fetch gen=%u (cur=%u)",
                 (unsigned)res->gen,
                 res->d ? (unsigned)res->d->fetch_gen : 0u);
    }
    free(res);
}

/* ── HTTP fetch task (Core 1) ──────────────────────────────────── */

typedef struct {
    chat_session_drawer_t *d;
    uint32_t gen;
    char    url[192];
} drawer_fetch_ctx_t;

static void parse_sessions_json(const char *json_txt, size_t len,
                                chat_session_t *out, int *out_count)
{
    *out_count = 0;
    if (!json_txt || len == 0) return;
    cJSON *root = cJSON_ParseWithLength(json_txt, len);
    if (!root) return;
    /* Accept either {"sessions":[...]} or raw array. */
    cJSON *arr = cJSON_GetObjectItem(root, "sessions");
    if (!cJSON_IsArray(arr)) arr = root;
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return; }

    int n = cJSON_GetArraySize(arr);
    if (n > MAX_ROWS) n = MAX_ROWS;
    for (int i = 0; i < n; i++) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(obj)) continue;
        chat_session_t s = {0};
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "id"));
        if (!id) id = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "session_id"));
        if (id) {
            strncpy(s.session_id, id, sizeof(s.session_id) - 1);
        }
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "title"));
        if (title) strncpy(s.title, title, sizeof(s.title) - 1);
        const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "llm_model"));
        if (model) strncpy(s.llm_model, model, sizeof(s.llm_model) - 1);
        cJSON *vm = cJSON_GetObjectItem(obj, "voice_mode");
        if (cJSON_IsNumber(vm)) s.voice_mode = (uint8_t)vm->valueint;
        cJSON *ts = cJSON_GetObjectItem(obj, "updated_at");
        if (cJSON_IsNumber(ts)) s.updated_at = (uint32_t)ts->valuedouble;
        else {
            ts = cJSON_GetObjectItem(obj, "created_at");
            if (cJSON_IsNumber(ts)) s.updated_at = (uint32_t)ts->valuedouble;
        }
        s.valid = (s.session_id[0] != 0);
        if (s.valid) out[(*out_count)++] = s;
    }
    cJSON_Delete(root);
}

/* Wave 14 W14-H06: runs on the shared tab5_worker, not its own task.
 * Plain function body — early returns instead of vTaskSuspend(NULL). */
static void drawer_fetch_task(void *arg)
{
    drawer_fetch_ctx_t *ctx = (drawer_fetch_ctx_t *)arg;
    if (!ctx) return;

    drawer_fetch_result_t *res = calloc(1, sizeof(*res));
    if (!res) { free(ctx); return; }
    res->d = ctx->d;
    res->gen = ctx->gen;

    esp_http_client_config_t cfg = {
        .url = ctx->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 4000,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            int cl = (int)esp_http_client_fetch_headers(client);
            if (cl <= 0) cl = 16384;                         /* safety */
            if (cl > 32 * 1024) cl = 32 * 1024;              /* upper bound */
            char *buf = malloc(cl + 1);
            if (buf) {
                int total = 0;
                while (total < cl) {
                    int r = esp_http_client_read(client, buf + total, cl - total);
                    if (r <= 0) break;
                    total += r;
                }
                buf[total] = 0;
                if (total > 0) {
                    parse_sessions_json(buf, total, res->sessions, &res->count);
                }
                free(buf);
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    lv_async_call(on_result_async, res);
    free(ctx);
}

/* ── Public API ────────────────────────────────────────────────── */

chat_session_drawer_t *chat_session_drawer_create(lv_obj_t *parent)
{
    chat_session_drawer_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    /* Full-screen scrim: tap outside panel to dismiss. */
    d->scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(d->scrim);
    lv_obj_set_size(d->scrim, 720, 1280);
    lv_obj_set_pos(d->scrim, 0, 0);
    lv_obj_set_style_bg_color(d->scrim, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(d->scrim, LV_OPA_70, 0);
    lv_obj_clear_flag(d->scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(d->scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d->scrim, ev_scrim_click, LV_EVENT_CLICKED, d);
    lv_obj_add_flag(d->scrim, LV_OBJ_FLAG_HIDDEN);

    /* Gradient spine (4 equal segments). */
    d->spine = lv_obj_create(d->scrim);
    lv_obj_remove_style_all(d->spine);
    lv_obj_set_size(d->spine, DRAWER_W, SPINE_H);
    lv_obj_set_pos(d->spine, 0, DRAWER_Y);
    lv_obj_set_style_bg_opa(d->spine, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d->spine, lv_color_hex(TH_MODE_LOCAL), 0);
    lv_obj_clear_flag(d->spine, LV_OBJ_FLAG_SCROLLABLE);
    /* Paint four equal sub-strips as children (LVGL 9 doesn't do multi-stop
     * linear gradients directly, but four rectangles are cheap and exact). */
    const int seg_w = DRAWER_W / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *seg = lv_obj_create(d->spine);
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, seg_w, SPINE_H);
        lv_obj_set_pos(seg, i * seg_w, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(s_mode_tint[i]), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    }

    /* Panel — scrollable list container. */
    d->panel = lv_obj_create(d->scrim);
    lv_obj_remove_style_all(d->panel);
    lv_obj_set_size(d->panel, DRAWER_W, DRAWER_H);
    lv_obj_set_pos(d->panel, 0, DRAWER_Y + SPINE_H);
    lv_obj_set_style_bg_color(d->panel, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(d->panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(d->panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *list = lv_obj_create(d->panel);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, DRAWER_W, DRAWER_H - 96);   /* footer = 96 px tall */
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    d->loading = lv_label_create(list);
    lv_label_set_text(d->loading, "Loading...");
    lv_obj_set_style_text_font(d->loading, FONT_CHAT_MONO, 0);
    lv_obj_set_style_text_color(d->loading, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_align(d->loading, LV_ALIGN_TOP_MID, 0, 40);

    for (int i = 0; i < MAX_ROWS; i++) {
        row_create(d, &d->rows[i], list, i);
    }

    /* Footer — + Start new conversation */
    d->footer = lv_obj_create(d->panel);
    lv_obj_remove_style_all(d->footer);
    lv_obj_set_size(d->footer, DRAWER_W - 2 * ROW_SIDE_PAD, 80);
    lv_obj_set_pos(d->footer, ROW_SIDE_PAD, DRAWER_H - 90);
    lv_obj_set_style_bg_color(d->footer, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(d->footer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d->footer, 16, 0);
    lv_obj_set_style_border_width(d->footer, 1, 0);
    lv_obj_set_style_border_color(d->footer, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_border_opa(d->footer, LV_OPA_60, 0);
    lv_obj_clear_flag(d->footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(d->footer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d->footer, ev_footer_click, LV_EVENT_CLICKED, d);

    lv_obj_t *f_lbl = lv_label_create(d->footer);
    lv_label_set_text(f_lbl, "+  Start new conversation");
    lv_obj_set_style_text_font(f_lbl, FONT_HEADING, 0);
    lv_obj_set_style_text_color(f_lbl, lv_color_hex(TH_AMBER), 0);
    lv_obj_center(f_lbl);

    return d;
}

void chat_session_drawer_destroy(chat_session_drawer_t *d)
{
    if (!d) return;
    if (d->scrim) lv_obj_del(d->scrim);
    free(d);
}

void chat_session_drawer_show(chat_session_drawer_t *d)
{
    if (!d || d->open) return;
    d->open = true;
    d->fetch_gen++;
    lv_obj_clear_flag(d->scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(d->scrim);
    render_loading(d);

    drawer_fetch_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->d = d;
    ctx->gen = d->fetch_gen;
    char host[64] = {0};
    tab5_settings_get_dragon_host(host, sizeof(host));
    uint16_t port = tab5_settings_get_dragon_port();
    if (port == 0) port = 3502;
    if (!host[0]) snprintf(host, sizeof(host), "%s", TAB5_DRAGON_HOST);
    snprintf(ctx->url, sizeof(ctx->url),
             "http://%s:%u/api/v1/sessions?limit=10", host, port);
    /* W14-H06: shared worker; ctx is freed inside drawer_fetch_task. */
    esp_err_t enq = tab5_worker_enqueue(drawer_fetch_task, ctx, "drawer_fetch");
    if (enq != ESP_OK) {
        ESP_LOGE(TAG, "drawer_fetch enqueue failed: %s", esp_err_to_name(enq));
        free(ctx);
    }
}

void chat_session_drawer_hide(chat_session_drawer_t *d)
{
    if (!d || !d->open) return;
    d->open = false;
    d->fetch_gen++;  /* invalidate any in-flight fetch */
    if (d->scrim) lv_obj_add_flag(d->scrim, LV_OBJ_FLAG_HIDDEN);
}

bool chat_session_drawer_is_open(chat_session_drawer_t *d)
{
    return d && d->open;
}

void chat_session_drawer_on_pick(chat_session_drawer_t *d,
    chat_drawer_pick_cb_t cb, void *ud)
{ if (d) { d->pick_cb = cb; d->pick_ud = ud; } }

void chat_session_drawer_on_new(chat_session_drawer_t *d,
    chat_drawer_new_cb_t cb, void *ud)
{ if (d) { d->new_cb = cb; d->new_ud = ud; } }

void chat_session_drawer_on_dismiss(chat_session_drawer_t *d,
    chat_drawer_dismiss_cb_t cb, void *ud)
{ if (d) { d->dismiss_cb = cb; d->dismiss_ud = ud; } }

void chat_session_drawer_mark_active(chat_session_drawer_t *d, const char *session_id)
{
    if (!d) return;
    if (!session_id) { d->active_session_id[0] = 0; return; }
    strncpy(d->active_session_id, session_id, sizeof(d->active_session_id) - 1);
    d->active_session_id[sizeof(d->active_session_id) - 1] = 0;
    /* Re-paint left bars. */
    for (int i = 0; i < MAX_ROWS; i++) {
        drawer_row_t *r = &d->rows[i];
        if (!r->in_use || !r->left_bar) continue;
        bool active = strncmp(d->active_session_id, r->data.session_id,
                              CHAT_SESSION_ID_LEN) == 0;
        if (active) lv_obj_clear_flag(r->left_bar, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(r->left_bar, LV_OBJ_FLAG_HIDDEN);
    }
}
