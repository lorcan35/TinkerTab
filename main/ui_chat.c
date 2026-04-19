/**
 * TinkerTab — Chat v4·C Ambient Orchestrator.
 *
 * Thin coordinator (< 400 LOC) wiring the chat subwidgets:
 *   chat_header         — 96-h header with back, title, chevron, chip, +
 *   chat_msg_view       — pool-recycled virtual scroll (v4·C bubbles + break-outs)
 *   chat_input_bar      — 108-h say-pill with 84 amber orb-ball + keyboard
 *   chat_suggestions    — empty-state cards
 *   chat_session_drawer — pull-down session picker (REST fetch to Dragon)
 *   chat_msg_store      — session-scoped ring buffer
 *
 * Spec: docs/superpowers/specs/2026-04-19-chat-v4c-design.md §7.
 */
#include "ui_chat.h"

#include "chat_msg_store.h"
#include "chat_msg_view.h"
#include "chat_header.h"
#include "chat_input_bar.h"
#include "chat_suggestions.h"
#include "chat_session_drawer.h"

#include "ui_theme.h"
#include "ui_keyboard.h"
#include "config.h"
#include "settings.h"
#include "voice.h"
#include "tab5_rtc.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

extern lv_obj_t *ui_home_get_screen(void);
extern void      ui_home_show_toast(const char *text);

static const char *TAG = "ui_chat";

/* Pixel spec (raw 720×1280). */
#define CHAT_W            720
#define CHAT_H           1280
#define CHAT_HDR_H         96
#define CHAT_ACCENT_H       2
#define CHAT_PILL_H       108
#define CHAT_PILL_BOT_PAD  40
#define NAV_H              72   /* home nav bar */

/* Message view spans y=100 to y=(chat_h - PILL_BOT_PAD - PILL_H - 20). */
#define CHAT_VIEW_Y       (CHAT_HDR_H + CHAT_ACCENT_H + 20)
#define CHAT_VIEW_H       (CHAT_H - CHAT_VIEW_Y - (CHAT_PILL_BOT_PAD + CHAT_PILL_H + 24) - NAV_H)

static const uint32_t s_mode_tint[4] = {
    TH_MODE_LOCAL, TH_MODE_HYBRID, TH_MODE_CLOUD, TH_MODE_CLAW,
};

/* ── State ─────────────────────────────────────────────────────── */
static lv_obj_t             *s_overlay  = NULL;
static chat_header_t        *s_hdr      = NULL;
static chat_msg_view_t      *s_view     = NULL;
static chat_input_bar_t     *s_input    = NULL;
static chat_suggestions_t   *s_sugg     = NULL;
static chat_session_drawer_t *s_drawer  = NULL;
static lv_timer_t           *s_poll     = NULL;
static bool                  s_active   = false;
static bool                  s_streaming = false;
static voice_state_t         s_last_state = VOICE_STATE_IDLE;

/* Async push payloads. */
typedef struct { char *role; char *text; } push_msg_t;
typedef struct { char url[256]; char alt[128]; int w, h; } push_media_t;
typedef struct { char title[128]; char subtitle[256]; char img[256]; char desc[256]; } push_card_t;
typedef struct { char url[256]; float dur; char label[128]; } push_audio_t;
typedef struct { char *text; } push_update_t;

/* ── Helpers ───────────────────────────────────────────────────── */

static void safe_copy(char *dst, size_t n, const char *src)
{
    if (!dst || n == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

static uint32_t now_ts(void)
{
    time_t t = 0; time(&t);
    if (t > 0) return (uint32_t)t;
    tab5_rtc_time_t r;
    if (tab5_rtc_get_time(&r) == ESP_OK) {
        return (uint32_t)(r.hour * 3600 + r.minute * 60 + r.second);
    }
    return 0;
}

static void suggestions_sync_visibility(void)
{
    if (!s_sugg) return;
    if (chat_store_count() == 0) chat_suggestions_show(s_sugg);
    else                         chat_suggestions_hide(s_sugg);
}

static void ensure_session_loaded(void)
{
    const chat_session_t *cur = chat_store_active_session();
    if (cur) return;
    /* Seed a fresh session from current NVS config — Dragon session_start
     * will overwrite this as soon as the real session_id arrives. */
    chat_session_t s = {0};
    char sid[CHAT_SESSION_ID_LEN] = {0};
    tab5_settings_get_session_id(sid, sizeof(sid));
    if (sid[0]) safe_copy(s.session_id, sizeof(s.session_id), sid);
    s.voice_mode = tab5_settings_get_voice_mode();
    char llm[CHAT_LLM_MODEL_LEN] = {0};
    tab5_settings_get_llm_model(llm, sizeof(llm));
    safe_copy(s.llm_model, sizeof(s.llm_model), llm);
    s.updated_at = now_ts();
    chat_store_set_session(&s);
}

static void paint_header_and_view_for_mode(uint8_t mode)
{
    if (mode > 3) mode = 0;
    chat_header_set_mode(s_hdr, mode, NULL);
    {
        char llm[CHAT_LLM_MODEL_LEN] = {0};
        const chat_session_t *cur = chat_store_active_session();
        if (cur && cur->llm_model[0]) {
            safe_copy(llm, sizeof(llm), cur->llm_model);
        } else {
            tab5_settings_get_llm_model(llm, sizeof(llm));
        }
        chat_header_set_mode(s_hdr, mode, llm);
    }
    chat_msg_view_set_mode_color(s_view, s_mode_tint[mode]);
}

/* ── Callbacks ─────────────────────────────────────────────────── */

static void on_back(void *ud)  { (void)ud; ui_chat_hide(); }

static void on_chev(void *ud)
{
    (void)ud;
    if (!s_drawer) return;
    if (chat_session_drawer_is_open(s_drawer)) {
        chat_session_drawer_hide(s_drawer);
        chat_header_set_title(s_hdr, "Chat");
        chat_header_set_chevron_open(s_hdr, false);
    } else {
        const chat_session_t *cur = chat_store_active_session();
        if (cur) chat_session_drawer_mark_active(s_drawer, cur->session_id);
        chat_session_drawer_show(s_drawer);
        chat_header_set_title(s_hdr, "Conversations");
        chat_header_set_chevron_open(s_hdr, true);
    }
}

static void on_plus(void *ud)
{
    (void)ud;
    /* New chat: clear history on Dragon, wipe store, refresh view. */
    voice_clear_history();
    chat_store_clear();
    if (s_view) chat_msg_view_refresh(s_view);
    suggestions_sync_visibility();
    ui_home_show_toast("New conversation");
}

static void on_mode_lp(void *ud)
{
    (void)ud;
    uint8_t m = tab5_settings_get_voice_mode();
    m = (m + 1) % VOICE_MODE_COUNT;
    tab5_settings_set_voice_mode(m);
    char llm[CHAT_LLM_MODEL_LEN] = {0};
    tab5_settings_get_llm_model(llm, sizeof(llm));
    voice_send_config_update((int)m, llm);

    chat_store_update_session_mode(m, llm);
    paint_header_and_view_for_mode(m);
    if (s_sugg) chat_suggestions_set_mode(s_sugg, m);

    static const char *names[4] = { "Local", "Hybrid", "Cloud", "Claw" };
    char toast[64];
    const char *nick = llm;
    const char *slash = strchr(nick, '/');
    if (slash) nick = slash + 1;
    snprintf(toast, sizeof(toast), "Mode: %s \xc2\xb7 %s",
             names[m], nick[0] ? nick : "default");
    ui_home_show_toast(toast);
}

static void on_ball_tap(void *ud)
{
    (void)ud;
    voice_state_t st = voice_get_state();
    if (st == VOICE_STATE_LISTENING) {
        voice_stop_listening();
    } else if (st == VOICE_STATE_READY || st == VOICE_STATE_IDLE) {
        voice_start_listening();
    } else {
        ESP_LOGI(TAG, "ball tap ignored (state=%d)", (int)st);
    }
}

static void on_keyboard(void *ud)
{
    (void)ud;
    if (s_input) ui_keyboard_show(chat_input_bar_get_textarea(s_input));
}

static void on_pill_tap(void *ud)
{
    (void)ud;
    if (s_input) ui_keyboard_show(chat_input_bar_get_textarea(s_input));
}

static void on_text_submit(const char *text, void *ud)
{
    (void)ud;
    if (!text || !*text) return;
    ui_keyboard_hide();

    chat_msg_t m = {0};
    m.type = MSG_TEXT;
    m.is_user = true;
    m.timestamp = now_ts();
    m.height_px = -1;
    safe_copy(m.text, sizeof(m.text), text);
    chat_store_add(&m);
    suggestions_sync_visibility();
    if (s_view) {
        chat_msg_view_refresh(s_view);
        chat_msg_view_scroll_to_bottom(s_view);
    }

    esp_err_t err = voice_send_text(text);
    if (err != ESP_OK) ui_home_show_toast("Not connected to Dragon");
    if (s_input) chat_input_bar_clear(s_input);
}

static void on_sugg_pick(const char *prompt, void *ud)
{
    (void)ud;
    on_text_submit(prompt, NULL);
}

static void on_pick_session(const chat_session_t *s, void *ud)
{
    (void)ud;
    if (!s) return;
    voice_send_config_update((int)s->voice_mode, s->llm_model);
    tab5_settings_set_voice_mode(s->voice_mode);
    tab5_settings_set_llm_model(s->llm_model);
    chat_store_set_session(s);
    paint_header_and_view_for_mode(s->voice_mode);
    if (s_sugg) chat_suggestions_set_mode(s_sugg, s->voice_mode);
    suggestions_sync_visibility();
    if (s_view) chat_msg_view_refresh(s_view);
    chat_session_drawer_hide(s_drawer);
    chat_header_set_title(s_hdr, "Chat");
    chat_header_set_chevron_open(s_hdr, false);
    ui_home_show_toast("Loaded session");
}

static void on_drawer_new(void *ud)
{
    (void)ud;
    uint8_t m = tab5_settings_get_voice_mode();
    char llm[CHAT_LLM_MODEL_LEN] = {0};
    tab5_settings_get_llm_model(llm, sizeof(llm));
    voice_send_config_update((int)m, llm);
    voice_clear_history();
    chat_session_t s = {0};
    s.voice_mode = m;
    safe_copy(s.llm_model, sizeof(s.llm_model), llm);
    s.updated_at = now_ts();
    chat_store_set_session(&s);
    paint_header_and_view_for_mode(m);
    suggestions_sync_visibility();
    if (s_view) chat_msg_view_refresh(s_view);
    chat_session_drawer_hide(s_drawer);
    chat_header_set_title(s_hdr, "Chat");
    chat_header_set_chevron_open(s_hdr, false);
    ui_home_show_toast("New conversation");
}

static void on_drawer_dismiss(void *ud)
{
    (void)ud;
    chat_session_drawer_hide(s_drawer);
    chat_header_set_title(s_hdr, "Chat");
    chat_header_set_chevron_open(s_hdr, false);
}

/* ── Keyboard layout callback ─────────────────────────────────── */

static void keyboard_layout_cb(bool visible, int kb_h)
{
    if (!s_view) return;
    if (visible) {
        int avail = CHAT_H - kb_h - CHAT_VIEW_Y - CHAT_PILL_H - 40;
        if (avail < 240) avail = 240;
        chat_msg_view_set_size(s_view, CHAT_W, avail);
        chat_msg_view_scroll_to_bottom(s_view);
    } else {
        chat_msg_view_set_size(s_view, CHAT_W, CHAT_VIEW_H);
    }
}

/* ── Voice state polling — streams LLM tokens into the store ──── */

static void poll_voice(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    voice_state_t st = voice_get_state();

    /* Map state → orb-ball visual + ghost. */
    int ball_state = 0;
    switch (st) {
        case VOICE_STATE_LISTENING: ball_state = 1; break;
        case VOICE_STATE_PROCESSING: ball_state = 2; break;
        case VOICE_STATE_SPEAKING:   ball_state = 3; break;
        default:                     ball_state = 0; break;
    }
    if (s_input) chat_input_bar_set_voice_state(s_input, ball_state);

    /* Stream AI tokens into the view. */
    if (st == VOICE_STATE_PROCESSING || st == VOICE_STATE_SPEAKING) {
        const char *llm = voice_get_llm_text();
        if (llm && llm[0]) {
            if (!s_streaming) {
                chat_msg_t m = {0};
                m.type = MSG_TEXT;
                m.is_user = false;
                m.timestamp = now_ts();
                m.height_px = -1;
                safe_copy(m.text, sizeof(m.text), llm);
                chat_store_add(&m);
                suggestions_sync_visibility();
                if (s_view) chat_msg_view_begin_streaming(s_view);
                s_streaming = true;
            } else {
                /* Replace text in place. */
                chat_store_update_last_text(llm);
                if (s_view) {
                    chat_msg_view_refresh(s_view);
                    chat_msg_view_scroll_to_bottom(s_view);
                }
            }
        }
    }

    if ((s_last_state == VOICE_STATE_PROCESSING ||
         s_last_state == VOICE_STATE_SPEAKING) &&
        (st == VOICE_STATE_READY || st == VOICE_STATE_IDLE)) {
        if (s_streaming && s_view) {
            chat_msg_view_end_streaming(s_view);
        }
        s_streaming = false;
    }

    s_last_state = st;
}

/* ── Public API ───────────────────────────────────────────────── */

lv_obj_t *ui_chat_create(void)
{
    if (s_overlay) {
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_active = true;
        if (!s_poll) s_poll = lv_timer_create(poll_voice, 150, NULL);
        else         lv_timer_resume(s_poll);
        ui_keyboard_set_layout_cb(keyboard_layout_cb);
        return s_overlay;
    }

    chat_store_init();
    ensure_session_loaded();

    lv_obj_t *parent = ui_home_get_screen();
    if (!parent) parent = lv_screen_active();

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, CHAT_W, CHAT_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_overlay);

    s_hdr = chat_header_create(s_overlay, "Chat");
    chat_header_on_back(s_hdr, on_back, NULL);
    chat_header_on_chevron(s_hdr, on_chev, NULL);
    chat_header_on_plus(s_hdr, on_plus, NULL);
    chat_header_on_mode_long_press(s_hdr, on_mode_lp, NULL);

    s_view = chat_msg_view_create(s_overlay, 0, CHAT_VIEW_Y, CHAT_W, CHAT_VIEW_H);
    s_input = chat_input_bar_create(s_overlay, CHAT_H - NAV_H);
    chat_input_bar_on_ball_tap(s_input, on_ball_tap, NULL);
    chat_input_bar_on_keyboard(s_input, on_keyboard, NULL);
    chat_input_bar_on_pill_tap(s_input, on_pill_tap, NULL);
    chat_input_bar_on_text_submit(s_input, on_text_submit, NULL);

    s_sugg = chat_suggestions_create(s_overlay);
    chat_suggestions_on_pick(s_sugg, on_sugg_pick, NULL);

    s_drawer = chat_session_drawer_create(s_overlay);
    chat_session_drawer_on_pick(s_drawer, on_pick_session, NULL);
    chat_session_drawer_on_new(s_drawer, on_drawer_new, NULL);
    chat_session_drawer_on_dismiss(s_drawer, on_drawer_dismiss, NULL);

    uint8_t mode = tab5_settings_get_voice_mode();
    if (mode >= VOICE_MODE_COUNT) mode = 0;
    paint_header_and_view_for_mode(mode);
    chat_suggestions_set_mode(s_sugg, mode);
    suggestions_sync_visibility();
    chat_msg_view_refresh(s_view);

    s_active = true;
    s_last_state = voice_get_state();
    s_poll = lv_timer_create(poll_voice, 150, NULL);
    ui_keyboard_set_layout_cb(keyboard_layout_cb);

    /* Force full invalidate + synchronous refresh — PARTIAL render only
     * paints the first strip per tick; lv_refr_now cycles through every
     * strip so all 1280 rows hit the framebuffer before we return. */
    lv_obj_invalidate(s_overlay);
    lv_refr_now(lv_display_get_default());

    ESP_LOGI(TAG, "Chat v4·C created (mode=%u)", mode);
    return s_overlay;
}

void ui_chat_show(void)
{
    if (!s_overlay) { ui_chat_create(); return; }
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_active = true;
    if (s_poll) lv_timer_resume(s_poll);
    ui_keyboard_set_layout_cb(keyboard_layout_cb);
    lv_obj_invalidate(s_overlay);
    lv_refr_now(lv_display_get_default());
}

void ui_chat_hide(void)
{
    if (!s_overlay) return;
    ui_keyboard_set_layout_cb(NULL);
    ui_keyboard_hide();
    if (s_poll) lv_timer_pause(s_poll);
    if (s_drawer) chat_session_drawer_hide(s_drawer);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_active = false;
}

void ui_chat_destroy(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    ui_keyboard_set_layout_cb(NULL);
    ui_keyboard_hide();
    if (s_drawer) { chat_session_drawer_destroy(s_drawer); s_drawer = NULL; }
    if (s_sugg)   { chat_suggestions_destroy(s_sugg);       s_sugg = NULL; }
    if (s_input)  { chat_input_bar_destroy(s_input);        s_input = NULL; }
    if (s_view)   { chat_msg_view_destroy(s_view);          s_view = NULL; }
    if (s_hdr)    { chat_header_destroy(s_hdr);             s_hdr = NULL; }
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_active = false;
    s_streaming = false;
    s_last_state = VOICE_STATE_IDLE;
}

bool ui_chat_is_active(void) { return s_active; }

/* Optional helper — legacy API kept for voice.c compatibility. */
void ui_chat_add_message(const char *text, bool is_user)
{
    if (!text || !*text) return;
    chat_msg_t m = {0};
    m.type = MSG_TEXT;
    m.is_user = is_user;
    m.timestamp = now_ts();
    m.height_px = -1;
    safe_copy(m.text, sizeof(m.text), text);
    chat_store_add(&m);
    suggestions_sync_visibility();
    if (s_view) {
        chat_msg_view_refresh(s_view);
        chat_msg_view_scroll_to_bottom(s_view);
    }
}

/* ── Thread-safe async push from voice task ──────────────────── */

static void async_push_msg_cb(void *arg)
{
    push_msg_t *p = (push_msg_t *)arg;
    if (!p) return;
    bool is_user = p->role && strcmp(p->role, "user") == 0;
    /* For assistant text, the poll loop already appended streaming text.
     * Only append a fresh bubble when the assistant message arrives
     * without poll having caught it (e.g. short responses where we hit
     * llm_done before the poll tick fires). */
    if (is_user) {
        ui_chat_add_message(p->text, true);
    } else {
        if (!s_streaming) ui_chat_add_message(p->text, false);
        else {
            /* Finalise streaming with the complete text. */
            chat_store_update_last_text(p->text ? p->text : "");
            if (s_view) chat_msg_view_refresh(s_view);
        }
    }
    free(p->role); free(p->text); free(p);
}

void ui_chat_push_message(const char *role, const char *text)
{
    if (!text || !*text) return;
    push_msg_t *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->role = strdup(role ? role : "assistant");
    p->text = strdup(text);
    if (!p->role || !p->text) { free(p->role); free(p->text); free(p); return; }
    lv_async_call(async_push_msg_cb, p);
}

static void async_push_media_cb(void *arg)
{
    push_media_t *m = (push_media_t *)arg;
    if (!m) { return; }
    chat_msg_t msg = {0};
    msg.type = MSG_IMAGE;
    msg.is_user = false;
    msg.timestamp = now_ts();
    msg.height_px = -1;
    safe_copy(msg.text, sizeof(msg.text), m->alt);
    safe_copy(msg.media_url, sizeof(msg.media_url), m->url);
    chat_store_add(&msg);
    suggestions_sync_visibility();
    if (s_view) {
        chat_msg_view_refresh(s_view);
        chat_msg_view_scroll_to_bottom(s_view);
    }
    free(m);
}

void ui_chat_push_media(const char *url, const char *media_type,
                        int width, int height, const char *alt)
{
    (void)media_type;
    push_media_t *m = calloc(1, sizeof(*m));
    if (!m) return;
    safe_copy(m->url, sizeof(m->url), url);
    safe_copy(m->alt, sizeof(m->alt), alt);
    m->w = width; m->h = height;
    lv_async_call(async_push_media_cb, m);
}

static void async_push_card_cb(void *arg)
{
    push_card_t *c = (push_card_t *)arg;
    if (!c) return;
    chat_msg_t msg = {0};
    msg.type = MSG_CARD;
    msg.is_user = false;
    msg.timestamp = now_ts();
    msg.height_px = -1;
    safe_copy(msg.text, sizeof(msg.text), c->title);
    safe_copy(msg.subtitle, sizeof(msg.subtitle), c->subtitle);
    if (c->img[0]) safe_copy(msg.media_url, sizeof(msg.media_url), c->img);
    chat_store_add(&msg);
    suggestions_sync_visibility();
    if (s_view) {
        chat_msg_view_refresh(s_view);
        chat_msg_view_scroll_to_bottom(s_view);
    }
    free(c);
}

void ui_chat_push_card(const char *title, const char *subtitle,
                       const char *image_url, const char *description)
{
    push_card_t *c = calloc(1, sizeof(*c));
    if (!c) return;
    safe_copy(c->title,    sizeof(c->title),    title);
    safe_copy(c->subtitle, sizeof(c->subtitle), subtitle);
    safe_copy(c->img,      sizeof(c->img),      image_url);
    safe_copy(c->desc,     sizeof(c->desc),     description);
    lv_async_call(async_push_card_cb, c);
}

static void async_push_audio_cb(void *arg)
{
    push_audio_t *a = (push_audio_t *)arg;
    if (!a) return;
    chat_msg_t msg = {0};
    msg.type = MSG_AUDIO_CLIP;
    msg.is_user = false;
    msg.timestamp = now_ts();
    msg.height_px = -1;
    safe_copy(msg.text, sizeof(msg.text), a->label[0] ? a->label : "audio clip");
    safe_copy(msg.media_url, sizeof(msg.media_url), a->url);
    chat_store_add(&msg);
    suggestions_sync_visibility();
    if (s_view) {
        chat_msg_view_refresh(s_view);
        chat_msg_view_scroll_to_bottom(s_view);
    }
    free(a);
}

void ui_chat_push_audio_clip(const char *url, float duration_s, const char *label)
{
    push_audio_t *a = calloc(1, sizeof(*a));
    if (!a) return;
    safe_copy(a->url,   sizeof(a->url),   url);
    safe_copy(a->label, sizeof(a->label), label);
    a->dur = duration_s;
    lv_async_call(async_push_audio_cb, a);
}

static void async_update_last_cb(void *arg)
{
    push_update_t *u = (push_update_t *)arg;
    if (!u) return;
    if (u->text && *u->text) {
        chat_store_update_last_text(u->text);
        if (s_view) chat_msg_view_refresh(s_view);
    }
    free(u->text); free(u);
}

void ui_chat_update_last_message(const char *text)
{
    if (!text || !*text) return;
    push_update_t *u = calloc(1, sizeof(*u));
    if (!u) return;
    u->text = strdup(text);
    if (!u->text) { free(u); return; }
    lv_async_call(async_update_last_cb, u);
}
