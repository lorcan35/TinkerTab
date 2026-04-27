/*
 * ui_sessions.c — v5 Conversations browser (chat sessions list)
 *
 * Pattern cloned from ui_agents.c: fullscreen overlay on the home screen,
 * hand-curated demo data rendered as flat row list (no card surfaces,
 * hairline dividers).
 */

#include "ui_sessions.h"
#include "ui_theme.h"
#include "ui_home.h"
#include "ui_core.h"
#include "config.h"
#include "settings.h"
#include "task_worker.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui_sessions";

#define SW        720
#define SH        1280
#define SIDE_PAD  52

static lv_obj_t *s_overlay  = NULL;
static lv_obj_t *s_back_btn = NULL;
static lv_obj_t *s_count_lbl = NULL;
static lv_obj_t *s_rows_root = NULL;   /* container for fetched rows */
static lv_obj_t *s_status    = NULL;   /* "Loading..." / "Dragon unreachable" */
static bool      s_visible  = false;
static volatile bool s_fetch_inflight = false;

/* U1 (#206): runtime row, populated from Dragon /api/v1/sessions.
 * The static-string fields in the legacy `session_row_t` are now owned
 * char[]s so the parser can write into them. */
#define MAX_ROWS 12
typedef struct {
    char        time_top[12];   /* "10:15" or "YEST" */
    char        time_bot[12];   /* optional second line "19:04" */
    char        subject[80];    /* title (truncated) */
    char        preview[128];   /* model name when no preview is sent */
    int         msg_count;      /* -1 if unknown — Dragon doesn't ship it */
    char        mode_tag[10];   /* "CLAW" / "CLOUD" / "LOCAL" / "HYBRID" */
    uint32_t    mode_color;     /* TH_MODE_* */
} session_row_t;

typedef struct {
    session_row_t items[MAX_ROWS];
    int           count;
    bool          ok;
} sessions_fetch_t;
/* PSRAM-backed — putting a 3 KB struct in BSS tipped the internal-SRAM
 * budget past the FreeRTOS timer-task stack alloc and we boot-looped
 * with `vApplicationGetTimerTaskMemory: pxStackBufferTemp != NULL`.
 * Lazy-allocate on first use via heap_caps_malloc(MALLOC_CAP_SPIRAM). */
static sessions_fetch_t *s_fetch = NULL;

static bool ensure_fetch_buf(void)
{
    if (s_fetch) return true;
    s_fetch = heap_caps_calloc(1, sizeof(*s_fetch),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fetch) {
        ESP_LOGW(TAG, "PSRAM alloc for sessions buffer failed");
        return false;
    }
    return true;
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ui_sessions_hide();
}

static void overlay_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) {
        ui_sessions_hide();
    }
}

/* U1 (#206): voice_mode → tag/color mapping (matches chat header tints). */
static void mode_to_tag(uint8_t vm, char *out, size_t n, uint32_t *color_out)
{
    const char *tag;
    uint32_t    col;
    switch (vm) {
        case 0: tag = "LOCAL";  col = TH_MODE_LOCAL;  break;
        case 1: tag = "HYBRID"; col = TH_MODE_HYBRID; break;
        case 2: tag = "CLOUD";  col = TH_MODE_CLOUD;  break;
        case 3: tag = "CLAW";   col = TH_MODE_CLAW;   break;
        default: tag = "?";     col = 0x5C5C68;       break;
    }
    snprintf(out, n, "%s", tag);
    if (color_out) *color_out = col;
}

/* U1 (#206): unix-epoch updated_at → time_top + optional time_bot.
 * Today  → "HH:MM"
 * Yest.  → "YEST" + "HH:MM"
 * Older  → "MMM DD" + "HH:MM" */
static void format_when(uint32_t updated_at,
                        char *t_top, size_t n_top,
                        char *t_bot, size_t n_bot)
{
    t_top[0] = 0; t_bot[0] = 0;
    if (updated_at == 0) {
        snprintf(t_top, n_top, "--:--");
        return;
    }
    time_t now = time(NULL);
    time_t ts  = (time_t)updated_at;
    struct tm now_tm, ts_tm;
    localtime_r(&now, &now_tm);
    localtime_r(&ts,  &ts_tm);

    char hhmm[8];
    strftime(hhmm, sizeof(hhmm), "%H:%M", &ts_tm);

    bool same_day = (now_tm.tm_year == ts_tm.tm_year &&
                     now_tm.tm_yday == ts_tm.tm_yday);
    bool yest = (now - ts < 48 * 3600 && !same_day);

    if (same_day) {
        snprintf(t_top, n_top, "%s", hhmm);
    } else if (yest) {
        snprintf(t_top, n_top, "YEST");
        snprintf(t_bot, n_bot, "%s", hhmm);
    } else {
        char md[12];
        strftime(md, sizeof(md), "%b %d", &ts_tm);
        for (char *p = md; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        }
        snprintf(t_top, n_top, "%s", md);
        snprintf(t_bot, n_bot, "%s", hhmm);
    }
}

static int build_session_row(lv_obj_t *parent, int y, const session_row_t *r)
{
    const int row_w = SW - 2 * SIDE_PAD;

    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, row_w, LV_SIZE_CONTENT);
    lv_obj_set_pos(c, SIDE_PAD, y);
    lv_obj_set_style_pad_top(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 18, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x1C1C28), 0);  /* TH_HAIRLINE */
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    /* Left column: time (stack top+bot if present) */
    lv_obj_t *tt = lv_label_create(c);
    lv_label_set_text(tt, r->time_top);
    lv_obj_set_style_text_font(tt, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(tt, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(tt, 2, 0);
    lv_obj_set_pos(tt, 0, 0);

    if (r->time_bot[0]) {
        lv_obj_t *tb = lv_label_create(c);
        lv_label_set_text(tb, r->time_bot);
        lv_obj_set_style_text_font(tb, FONT_CAPTION, 0);
        lv_obj_set_style_text_color(tb, lv_color_hex(0x5C5C68), 0);
        lv_obj_set_style_text_letter_space(tb, 2, 0);
        lv_obj_set_pos(tb, 0, 22);
    }

    /* Subject (amber, FONT_HEADING) */
    lv_obj_t *sub = lv_label_create(c);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_label_set_text(sub, r->subject);
    lv_obj_set_width(sub, row_w - 140 - 110);
    lv_obj_set_style_text_font(sub, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_pos(sub, 120, 0);

    /* Preview (dim, 2-line wrap) */
    lv_obj_t *pv = lv_label_create(c);
    lv_label_set_long_mode(pv, LV_LABEL_LONG_DOT);
    lv_label_set_text(pv, r->preview);
    lv_obj_set_width(pv, row_w - 140 - 110);
    lv_obj_set_style_text_font(pv, FONT_SMALL, 0);
    lv_obj_set_style_text_color(pv, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_line_space(pv, 3, 0);
    lv_obj_set_pos(pv, 120, 28);

    /* Right meta: msg count + mode tag.  msg_count == -1 means
     * "Dragon's /api/v1/sessions doesn't ship message counts" — we
     * just hide the count line in that case so the row reads cleanly. */
    if (r->msg_count >= 0) {
        char msgbuf[16];
        snprintf(msgbuf, sizeof(msgbuf), "%d MSG", r->msg_count);
        lv_obj_t *mc = lv_label_create(c);
        lv_label_set_text(mc, msgbuf);
        lv_obj_set_style_text_font(mc, FONT_CAPTION, 0);
        lv_obj_set_style_text_color(mc, lv_color_hex(0x5C5C68), 0);
        lv_obj_set_style_text_letter_space(mc, 2, 0);
        lv_obj_align(mc, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    lv_obj_t *mt = lv_label_create(c);
    lv_label_set_text(mt, r->mode_tag);
    lv_obj_set_style_text_font(mt, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(mt, lv_color_hex(r->mode_color), 0);
    lv_obj_set_style_text_letter_space(mt, 3, 0);
    lv_obj_align(mt, LV_ALIGN_TOP_RIGHT, 0, r->msg_count >= 0 ? 22 : 0);

    /* Return the content height so caller can advance y cleanly. */
    return 72;
}

/* U1 (#206): hop to the LVGL thread to (re)build the row list from
 * s_fetch->  Called by the worker via lv_async_call. */
static void render_rows_cb(void *arg)
{
    (void)arg;
    if (!s_overlay || !s_rows_root || !s_fetch) return;

    /* Wipe previous rows. */
    lv_obj_clean(s_rows_root);

    if (s_status) {
        if (!s_fetch->ok) {
            lv_label_set_text(s_status,
                "Dragon unreachable. Check Settings \xe2\x80\xa2 Network.");
            lv_obj_set_style_text_color(s_status, lv_color_hex(0xFF6464), 0);
            lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        } else if (s_fetch->count == 0) {
            lv_label_set_text(s_status, "No conversations yet.");
            lv_obj_set_style_text_color(s_status,
                lv_color_hex(TH_TEXT_DIM), 0);
            lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_count_lbl) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%d \xe2\x80\xa2 RECENT", s_fetch->count);
        lv_label_set_text(s_count_lbl, buf);
    }

    int y = 0;
    for (int i = 0; i < s_fetch->count; i++) {
        int h = build_session_row(s_rows_root, y, &s_fetch->items[i]);
        y += h + 14;
    }
    /* Force the container to size to its content so the scroll works. */
    lv_obj_set_height(s_rows_root, y > 0 ? y : LV_SIZE_CONTENT);
}

/* U1 (#206): runs on tab5_worker — single-threaded HTTP GET against
 * /api/v1/sessions, parse JSON, populate s_fetch, hop back to LVGL. */
static void fetch_sessions_job(void *arg)
{
    (void)arg;
    if (!ensure_fetch_buf()) {
        s_fetch_inflight = false;
        return;
    }
    char host[64] = {0};
    tab5_settings_get_dragon_host(host, sizeof(host));
    if (!host[0]) snprintf(host, sizeof(host), "192.168.1.91");
    uint16_t port = tab5_settings_get_dragon_port();
    if (port == 0) port = 3502;

    char url[160];
    snprintf(url, sizeof(url),
             "http://%s:%u/api/v1/sessions?limit=%d", host, port, MAX_ROWS);

    s_fetch->count = 0;
    s_fetch->ok    = false;

    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 4000, .buffer_size = 4096,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) goto done;

    /* Dragon's /api/v1 routes need Authorization: Bearer <token>
     * (same one voice.c uses on the WS upgrade). */
    if (TAB5_DRAGON_TOKEN && TAB5_DRAGON_TOKEN[0] &&
        strcmp(TAB5_DRAGON_TOKEN, "CHANGEME_SET_IN_SDKCONFIG_LOCAL") != 0) {
        char auth[96];
        snprintf(auth, sizeof(auth), "Bearer %s", TAB5_DRAGON_TOKEN);
        esp_http_client_set_header(cli, "Authorization", auth);
    }

    esp_err_t err = esp_http_client_open(cli, 0);
    int status = 0, content_len = 0, total = 0;
    char *body = NULL;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http_client_open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    content_len = esp_http_client_fetch_headers(cli);
    if (content_len <= 0)        content_len = 16384;
    if (content_len > 32 * 1024) content_len = 32 * 1024;
    /* PSRAM-backed: prevents internal-SRAM fragmentation on every
     * fetch (#182 follow-up — the REST body could be 6-32 KB and was
     * carving the tight internal heap in plain malloc, contributing
     * to the slow drift the SRAM-exhaustion watchdog catches). */
    body = heap_caps_malloc((size_t)content_len + 1,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) goto cleanup;
    while (total < content_len) {
        int r = esp_http_client_read(cli, body + total, content_len - total);
        if (r <= 0) break;
        total += r;
    }
    body[total > 0 ? total : 0] = 0;
    status = esp_http_client_get_status_code(cli);
    ESP_LOGI(TAG, "GET %s -> %d (%d bytes)", url, status, total);
    if (status != 200 || total <= 0) goto cleanup;

    cJSON *root = cJSON_ParseWithLength(body, total);
    if (!root) goto cleanup;
    /* Dragon ships {"items":[...]} on /api/v1/sessions; older mocks
     * shipped {"sessions":[...]} or a raw array.  Accept all three. */
    cJSON *arr = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(arr)) arr = cJSON_GetObjectItem(root, "sessions");
    if (!cJSON_IsArray(arr)) arr = root;
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); goto cleanup; }

    int n = cJSON_GetArraySize(arr);
    if (n > MAX_ROWS) n = MAX_ROWS;
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o)) continue;
        session_row_t *r = &s_fetch->items[s_fetch->count];
        memset(r, 0, sizeof(*r));

        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(o, "title"));
        snprintf(r->subject, sizeof(r->subject), "%s",
                 title && title[0] ? title : "(untitled)");

        const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(o, "llm_model"));
        if (model && model[0]) {
            snprintf(r->preview, sizeof(r->preview), "%s", model);
        }

        cJSON *vm = cJSON_GetObjectItem(o, "voice_mode");
        uint8_t v = cJSON_IsNumber(vm) ? (uint8_t)vm->valueint : 0;
        mode_to_tag(v, r->mode_tag, sizeof(r->mode_tag), &r->mode_color);

        /* Dragon ships message_count on /api/v1/sessions; -1 means
         * "not present" so build_session_row hides the count line. */
        cJSON *mc = cJSON_GetObjectItem(o, "message_count");
        r->msg_count = cJSON_IsNumber(mc) ? mc->valueint : -1;

        /* Recency timestamp — Dragon uses "last_active_at" (touched on
         * every turn).  Fall back to updated_at then created_at for
         * older payloads / stripped responses. */
        uint32_t ts = 0;
        cJSON *u = cJSON_GetObjectItem(o, "last_active_at");
        if (!cJSON_IsNumber(u)) u = cJSON_GetObjectItem(o, "updated_at");
        if (!cJSON_IsNumber(u)) u = cJSON_GetObjectItem(o, "created_at");
        if (cJSON_IsNumber(u)) ts = (uint32_t)u->valuedouble;
        format_when(ts, r->time_top, sizeof(r->time_top),
                       r->time_bot, sizeof(r->time_bot));

        s_fetch->count++;
    }
    s_fetch->ok = true;
    cJSON_Delete(root);

cleanup:
    if (body) heap_caps_free(body);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
done:
    s_fetch_inflight = false;
    lv_async_call(render_rows_cb, NULL);
}

static void kick_sessions_fetch(void)
{
    if (s_fetch_inflight) return;
    s_fetch_inflight = true;
    if (tab5_worker_enqueue(fetch_sessions_job, NULL, "sessions") != ESP_OK) {
        ESP_LOGW(TAG, "worker queue full — dropping sessions fetch");
        s_fetch_inflight = false;
    }
}

void ui_sessions_show(void)
{
    /* Match the go-home fix: if a separate lv_screen is active, load
     * home first so this overlay actually renders. */
    lv_obj_t *home = ui_home_get_screen();
    if (home && lv_screen_active() != home) {
        lv_screen_load(home);
    }

    if (s_overlay) {
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_visible = true;
        /* Refresh on every re-show so the list stays current. */
        kick_sessions_fetch();
        return;
    }

    lv_obj_t *parent = home;
    if (!parent) parent = lv_screen_active();
    if (!parent) return;

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, SW, SH);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_overlay, LV_DIR_VER);
    lv_obj_add_event_cb(s_overlay, overlay_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Back hit — top-left (taps). Swipe-right also closes. */
    s_back_btn = lv_button_create(s_overlay);
    lv_obj_set_size(s_back_btn, 120, 60);
    lv_obj_set_pos(s_back_btn, 24, 30);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    lv_obj_set_style_border_width(s_back_btn, 0, 0);
    lv_obj_add_event_cb(s_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(s_back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  HOME");
    lv_obj_set_style_text_font(bl, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(bl, 3, 0);
    lv_obj_center(bl);

    /* Header */
    lv_obj_t *head = lv_label_create(s_overlay);
    lv_label_set_text(head, "Conversations");
    lv_obj_set_style_text_font(head, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_pos(head, SIDE_PAD, 110);

    s_count_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_count_lbl, "LOADING \xe2\x80\xa2 SESSIONS");
    lv_obj_set_style_text_font(s_count_lbl, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_count_lbl, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(s_count_lbl, 3, 0);
    lv_obj_set_pos(s_count_lbl, SIDE_PAD, 190);

    /* Status line — empty/error message; hidden once we have rows. */
    s_status = lv_label_create(s_overlay);
    lv_label_set_text(s_status, "Loading conversations...");
    lv_obj_set_style_text_font(s_status, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_pos(s_status, SIDE_PAD, 250);

    /* Row container — render_rows_cb populates it.  Rows render at
     * y=0+ within this container; the container itself sits at y=290. */
    s_rows_root = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_rows_root);
    lv_obj_set_size(s_rows_root, SW, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_rows_root, 0, 290);
    lv_obj_clear_flag(s_rows_root, LV_OBJ_FLAG_SCROLLABLE);

    s_visible = true;
    kick_sessions_fetch();
    ESP_LOGI(TAG, "sessions overlay shown (fetching from Dragon)");
}

void ui_sessions_hide(void)
{
    if (!s_visible) return;
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ESP_LOGI(TAG, "sessions overlay hidden");
}

bool ui_sessions_is_visible(void)
{
    return s_visible;
}
