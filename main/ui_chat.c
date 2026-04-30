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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "chat_header.h"
#include "chat_input_bar.h"
#include "chat_msg_store.h"
#include "chat_msg_view.h"
#include "chat_session_drawer.h"
#include "chat_suggestions.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "settings.h"
#include "tab5_rtc.h"
#include "task_worker.h"
#include "ui_audio.h"
#include "ui_core.h" /* tab5_lv_async_call (#258) */
#include "ui_keyboard.h"
#include "ui_theme.h"
#include "voice.h"
#include "voice_onboard.h"

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

static const uint32_t s_mode_tint[VOICE_MODE_COUNT] = {
    TH_MODE_LOCAL, TH_MODE_HYBRID, TH_MODE_CLOUD, TH_MODE_CLAW, TH_MODE_ONBOARD,
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
typedef struct {
    char title[128]; char subtitle[256]; char img[256]; char desc[256];
    /* Phase 2 (#70) */
    char card_id[CHAT_CARD_ID_LEN];
    char action_label[CHAT_ACTION_LABEL_LEN];
    char action_event[CHAT_ACTION_EVENT_LEN];
} push_card_t;
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
    /* Prefer the system clock (set by NTP sync at boot).  If NTP hasn't
     * synced yet, fall back to composing a proper unix epoch from the
     * RX8130CE RTC chip (year/month/day + hour/min/sec).
     *
     * closes #108: the old fallback returned `hour*3600 + min*60 + sec`
     * — seconds-within-today — which fmt_timestamp then ran through
     * (ts/3600) % 24 and got random hours back.  That's why messages
     * sent at 14:00 rendered as "09:17" etc. */
    time_t t = 0;
    time(&t);
    /* Any year >= 2024 means NTP (or a previously-synced system clock)
     * has given us a real epoch.  Below that we're on ESP-IDF's default
     * 2020 / 2000 boot time and should fall through to the RTC. */
    if (t >= 1704067200L /* 2024-01-01 */) return (uint32_t)t;

    tab5_rtc_time_t r;
    if (tab5_rtc_get_time(&r) == ESP_OK) {
        struct tm tm_r = {
            .tm_year = (int)r.year + 100,   /* RTC stores year as offset-from-2000; tm wants offset-from-1900 */
            .tm_mon  = (int)r.month - 1,    /* tm months are 0-based */
            .tm_mday = (int)r.day,
            .tm_hour = (int)r.hour,
            .tm_min  = (int)r.minute,
            .tm_sec  = (int)r.second,
        };
        time_t epoch = mktime(&tm_r);
        if (epoch > 0) return (uint32_t)epoch;
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
   if (mode >= VOICE_MODE_COUNT) mode = 0;
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
   /* TT #328 Wave 1 — paint daily-spend badge so it's populated
    * immediately on chat open / mode change, not just after the next
    * receipt fires. */
   if (s_hdr) {
      chat_header_set_spend(s_hdr, tab5_budget_get_today_mils(), tab5_budget_get_cap_mils());
   }
}

/* ── Callbacks ─────────────────────────────────────────────────── */

static void on_back(void *ud)  { (void)ud; ui_chat_hide(); }

/* TT #328 Wave 10 — swipe-right-back gesture on chat overlay.
 * Wave 10 follow-up: also reset /screen's nav target so the harness's
 * `current` field reflects that we returned to home. */
static void on_chat_gesture(lv_event_t *e) {
   (void)e;
   lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
   if (dir == LV_DIR_RIGHT) {
      ui_chat_hide();
      extern void tab5_debug_set_nav_target(const char *);
      tab5_debug_set_nav_target("home");
   }
}

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

    static const char *names[VOICE_MODE_COUNT] = {"Local", "Hybrid", "Cloud", "Claw", "Onboard"};
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
    /* Wave 4a: chain-mode tap-toggle is independent of voice state.
     * The chain spends most of its life in PROCESSING (5-sec setup) or
     * LISTENING (drain loop), and the user must be able to stop in both.
     * voice_stop_listening dispatches to voice_m5_chain_stop when the
     * chain is active. */
    if (voice_onboard_chain_active()) {
       voice_stop_listening();
       return;
    }
    if (st == VOICE_STATE_LISTENING) {
        voice_stop_listening();
    } else if (st == VOICE_STATE_READY || st == VOICE_STATE_IDLE) {
        voice_start_listening();
    } else {
        ESP_LOGI(TAG, "ball tap ignored (state=%d)", (int)st);
    }
}

/* TT #328 Wave 3 — chat-overlay dictation entry.  Pre-Wave-3 the only
 * way to dictate was via the Notes screen Record button; chat had no
 * affordance.  Long-press the orb-ball in the chat input bar = same
 * gesture as the home-screen mic, same outcome (open dictation, mic
 * streams unbounded, auto-stops on 5 s silence, post-process emits
 * title + summary).  The dictation result lands in the existing
 * stt-text path in voice.c so the user sees the transcript on the
 * voice overlay; if they're in Notes context they get a Note, else
 * they get the read-only transcript display.
 *
 * Future (separate audit item): wire the dictation-via-chat outcome
 * into chat_msg_store as a user message + run it through the LLM.
 * That changes Dragon-side behaviour (dictation mode = STT-only) and
 * is bigger scope than the chat-entry gap closes here. */
static void on_ball_long_press(void *ud) {
   (void)ud;
   voice_state_t st = voice_get_state();
   if (voice_onboard_chain_active()) {
      ESP_LOGI(TAG, "ball long-press ignored: K144 chain active");
      return;
   }
   if (st != VOICE_STATE_READY && st != VOICE_STATE_IDLE) {
      ESP_LOGI(TAG, "ball long-press ignored (state=%d)", (int)st);
      return;
   }
   ESP_LOGI(TAG, "Chat orb long-press → starting dictation");
   esp_err_t ret = voice_start_dictation();
   if (ret != ESP_OK) {
      ESP_LOGW(TAG, "voice_start_dictation failed: %s", esp_err_to_name(ret));
      ui_home_show_toast("Cannot dictate — Dragon offline");
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

/* U11 (#206): camera affordance in the chat composer.  Arms the
 * one-shot "share next capture" flag so ui_camera's capture handler
 * knows to upload the resulting frame to Dragon (which then echoes a
 * signed `media` event back over the WS — the existing chat media
 * renderer picks that up and draws an inline image bubble). */
static void on_camera_tap(void *ud)
{
    (void)ud;
    extern lv_obj_t *ui_camera_create(void);
    extern void      ui_camera_arm_chat_share(void);
    ui_camera_arm_chat_share();
    /* Hide chat first so the camera viewfinder owns the screen. */
    ui_chat_hide();
    ui_camera_create();
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
    if (s_view) {
        chat_msg_view_refresh(s_view);
        chat_msg_view_scroll_to_bottom(s_view);   /* closes #107 */
    }
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

/* closes #190: gap between raised pill and keyboard top edge. */
#define PILL_KB_GAP      16

static void keyboard_layout_cb(bool visible, int kb_h)
{
    if (!s_view) return;
    if (visible) {
        /* #190: the pill is created at y = CHAT_H - NAV_H - PILL_BOT_PAD -
         * PILL_H = 1060.  The keyboard panel slides to y = CHAT_H - kb_h
         * = 900 — which completely covers the pill and its textarea, so
         * anything the user types is invisible.  Raise the pill so its
         * bottom sits PILL_KB_GAP above the keyboard top, and shrink the
         * message view to end just above the raised pill. */
        int pill_top  = (CHAT_H - kb_h) - PILL_KB_GAP - CHAT_PILL_H;
        int view_top  = CHAT_VIEW_Y;
        int avail     = pill_top - view_top - 20;   /* 20 px gap above pill */
        if (avail < 240) avail = 240;
        chat_msg_view_set_size(s_view, CHAT_W, avail);
        if (s_input) chat_input_bar_set_pill_y(s_input, pill_top);
        chat_msg_view_scroll_to_bottom(s_view);
    } else {
        chat_msg_view_set_size(s_view, CHAT_W, CHAT_VIEW_H);
        if (s_input) chat_input_bar_restore_pill_y(s_input);
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
            /* closes #129: before adding a fresh bubble, walk back
             * past any system messages (tool activity) to see if the
             * most recent assistant bubble already exists \u2014 re-use it.
             * This covers the state-oscillation case (SPEAKING \u2192 brief
             * READY \u2192 SPEAKING for multi-chunk TTS) that cleared
             * s_streaming and otherwise created a second AI bubble
             * carrying a suffix of the same response. */
            int count_ = chat_store_count();
            int reuse_idx = -1;
            for (int k = count_ - 1; k >= 0 && k >= count_ - 6; k--) {
                const chat_msg_t *m = chat_store_get(k);
                if (!m) continue;
                if (m->type == MSG_TEXT && !m->is_user) { reuse_idx = k; break; }
                if (m->type == MSG_SYSTEM) continue;
                break;
            }

            if (!s_streaming && reuse_idx < 0) {
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
            } else if (reuse_idx >= 0) {
                chat_msg_t *m = chat_store_get_mut(reuse_idx);
                if (m) {
                    safe_copy(m->text, sizeof(m->text), llm);
                    m->height_px = -1;
                }
                if (s_view) {
                    chat_msg_view_refresh(s_view);
                    chat_msg_view_scroll_to_bottom(s_view);
                }
                s_streaming = true;
            } else {
                /* s_streaming is true but reuse_idx < 0 \u2014 shouldn't
                 * happen in practice (streaming implies our bubble is
                 * somewhere in recent history) but handle anyway. */
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
    /* #229 fix: when the user navigates "camera -> chat", the
     * navigate handler tears down the camera screen (lv_obj_delete on
     * the active screen) before calling us.  If we don't ensure home
     * is loaded as the active screen, our overlay (parented to home)
     * hangs in a tree whose root isn't on any display.  LVGL's render
     * timer then walks a stale screen list and calls
     * lv_obj_update_layout(NULL), exploding inside lv_obj_pos.c:304.
     * Mirrors the guard ui_memory / ui_focus / ui_agents / ui_sessions
     * already have. */
    {
        lv_obj_t *home = ui_home_get_screen();
        if (home && lv_screen_active() != home) {
            lv_screen_load(home);
        }
    }

    if (s_overlay) {
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_active = true;
        if (!s_poll) s_poll = lv_timer_create(poll_voice, 150, NULL);
        else         lv_timer_resume(s_poll);
        ui_keyboard_set_layout_cb(keyboard_layout_cb);
        ui_keyboard_set_trigger_visible(false);
        /* Re-sync view + suggestions against chat_store after hide.  Messages
         * pushed via /chat or voice handlers while the overlay was hidden
         * would otherwise show as empty-state until next interaction. */
        {
            uint8_t mode = tab5_settings_get_voice_mode();
            paint_header_and_view_for_mode(mode);
            if (s_sugg) chat_suggestions_set_mode(s_sugg, mode);
        }
        suggestions_sync_visibility();
        if (s_view) {
            chat_msg_view_refresh(s_view);
            chat_msg_view_scroll_to_bottom(s_view);
        }
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

    /* TT #328 Wave 10 — swipe-right-back on chat overlay.  Pre-Wave-10
     * the chat overlay had no swipe gesture; users were stuck with the
     * 60-px header back button or a deep nav.  GESTURE_BUBBLE clear
     * keeps the gesture from propagating to the home screen below. */
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_overlay, on_chat_gesture, LV_EVENT_GESTURE, NULL);

    s_hdr = chat_header_create(s_overlay, "Chat");
    chat_header_on_back(s_hdr, on_back, NULL);
    chat_header_on_chevron(s_hdr, on_chev, NULL);
    chat_header_on_plus(s_hdr, on_plus, NULL);
    chat_header_on_mode_long_press(s_hdr, on_mode_lp, NULL);

    s_view = chat_msg_view_create(s_overlay, 0, CHAT_VIEW_Y, CHAT_W, CHAT_VIEW_H);
    s_input = chat_input_bar_create(s_overlay, CHAT_H - NAV_H);
    chat_input_bar_on_ball_tap(s_input, on_ball_tap, NULL);
    /* TT #328 Wave 3 — long-press orb in chat opens dictation. */
    chat_input_bar_on_ball_long_press(s_input, on_ball_long_press, NULL);
    chat_input_bar_on_keyboard(s_input, on_keyboard, NULL);
    chat_input_bar_on_pill_tap(s_input, on_pill_tap, NULL);
    chat_input_bar_on_text_submit(s_input, on_text_submit, NULL);
    chat_input_bar_on_camera(s_input, on_camera_tap, NULL);

    s_sugg = chat_suggestions_create(s_overlay);
    chat_suggestions_on_pick(s_sugg, on_sugg_pick, NULL);

    s_drawer = chat_session_drawer_create(s_overlay);
    /* #120: drawer_create can now return NULL when the LVGL pool is
     * too tight to build its ~20 widgets.  Chat must still work
     * without it (the chevron/drawer is secondary UI). */
    if (s_drawer) {
        chat_session_drawer_on_pick(s_drawer, on_pick_session, NULL);
        chat_session_drawer_on_new(s_drawer, on_drawer_new, NULL);
        chat_session_drawer_on_dismiss(s_drawer, on_drawer_dismiss, NULL);
    } else {
        ESP_LOGW(TAG, "chat: session drawer unavailable (LVGL pool tight)");
    }

    uint8_t mode = tab5_settings_get_voice_mode();
    if (mode >= VOICE_MODE_COUNT) mode = 0;
    paint_header_and_view_for_mode(mode);
    chat_suggestions_set_mode(s_sugg, mode);
    suggestions_sync_visibility();
    chat_msg_view_refresh(s_view);
    chat_msg_view_scroll_to_bottom(s_view);   /* closes #107 */

    s_active = true;
    s_last_state = voice_get_state();
    s_poll = lv_timer_create(poll_voice, 150, NULL);
    ui_keyboard_set_layout_cb(keyboard_layout_cb);
    /* 2026-04-23: hide the floating keyboard trigger — the pill's kb
     * affordance is the only entry point we want visible in chat. */
    ui_keyboard_set_trigger_visible(false);

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
    /* 2026-04-23: hide the floating keyboard trigger while chat is
     * visible — the pill has its own kb button and two entry points
     * confused users. */
    ui_keyboard_set_trigger_visible(false);
    /* Re-sync view + suggestions against chat_store after a hide.  Without
     * this, navigating back to chat after a mode change, + New, or a turn
     * that landed while the overlay was hidden shows a stale view — empty-
     * state suggestions on top of bubbles, or vice versa.  The refresh is
     * a no-op when nothing changed. */
    {
        uint8_t mode = tab5_settings_get_voice_mode();
        paint_header_and_view_for_mode(mode);
        if (s_sugg) chat_suggestions_set_mode(s_sugg, mode);
    }
    suggestions_sync_visibility();
    if (s_view) {
        chat_msg_view_refresh(s_view);
        chat_msg_view_scroll_to_bottom(s_view);
    }
    lv_obj_invalidate(s_overlay);
    lv_refr_now(lv_display_get_default());
}

void ui_chat_hide(void)
{
    if (!s_overlay) return;
    /* Audit #2: tear down the K144 chain when leaving chat — otherwise
     * it keeps running for up to 10 minutes in the background, dropping
     * bubbles into the (hidden) chat store and playing TTS through the
     * speaker.  voice_stop_listening dispatches to voice_m5_chain_stop
     * when chain is active. */
    if (voice_onboard_chain_active()) {
       voice_stop_listening();
    }
    ui_keyboard_set_layout_cb(NULL);
    ui_keyboard_hide();
    /* U16 (#206): don't auto-restore the floating keyboard trigger on
     * chat hide.  Home explicitly hides it (its menu chip lives in the
     * trigger's hitbox).  Other screens that actually want it (notes /
     * wifi text fields) call set_trigger_visible(true) themselves. */
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

/* #299: scoped accessor for /input/text. Returns NULL when chat is not
 * active so the debug handler can refuse with a clear error instead of
 * defaulting to "whatever textarea was focused" (which previously
 * landed in Settings.dragon_host overnight). */
lv_obj_t *ui_chat_get_input_textarea(void)
{
    if (!s_active || !s_input) return NULL;
    return chat_input_bar_get_textarea(s_input);
}

/* v4·D Phase 4d async refresh: called from the WS rx task after a receipt
 * is attached to the last assistant bubble.  Hops to the LVGL thread via
 * lv_async_call so the paint happens under the lock. */
static void async_refresh_receipts_cb(void *arg)
{
    (void)arg;
    if (s_view) {
        chat_msg_view_refresh(s_view);
        /* closes #107: a receipt attached to the last bubble can change
         * its rendered height; scroll so the newest content stays in
         * view rather than drifting off-screen. */
        chat_msg_view_scroll_to_bottom(s_view);
    }
}

void ui_chat_refresh_receipts(void)
{
    /* Safe to queue even if the chat view is hidden -- refresh is a no-op
     * when s_view is NULL and will simply repaint the existing bubbles
     * when it's mounted. */
    tab5_lv_async_call(async_refresh_receipts_cb, NULL);
}

/* TT #328 Wave 1 — daily-spend badge refresh.  Reads NVS state and
 * paints chat_header's spend label.  Run on the LVGL thread (async
 * hop) so callers from the WS rx task can fire it safely. */
static void async_refresh_spend_cb(void *arg) {
   (void)arg;
   if (!s_hdr) return;
   uint32_t mils = tab5_budget_get_today_mils();
   uint32_t cap = tab5_budget_get_cap_mils();
   chat_header_set_spend(s_hdr, mils, cap);
}

void ui_chat_refresh_spend(void) {
   /* No-op when overlay isn't mounted (s_hdr NULL inside the cb). */
   tab5_lv_async_call(async_refresh_spend_cb, NULL);
}

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
        /* closes #129: dedupe against the most recent assistant bubble.
         * The streaming path (poll_voice) already appended an assistant
         * MSG_TEXT when llm tokens arrived; llm_done then fires another
         * push_message with the same final text and would create a
         * SECOND identical TINKER bubble.
         *
         * Walk back up to 6 messages looking for the most recent
         * assistant MSG_TEXT — tool-call activity arrives as MSG_SYSTEM
         * ('Calculating...', 'calculator done') which sits between the
         * streaming bubble and the final push, so simply checking the
         * last message misses this case (seen in retest screenshot).
         * If we find one, update its text instead of appending. */
        int count = chat_store_count();
        int merge_idx = -1;
        for (int k = count - 1; k >= 0 && k >= count - 6; k--) {
            const chat_msg_t *m = chat_store_get(k);
            if (!m) continue;
            if (m->type == MSG_TEXT && !m->is_user) {
                merge_idx = k;
                break;
            }
            /* Keep walking through system messages (tool activity). */
            if (m->type == MSG_SYSTEM) continue;
            /* User bubble hit — stop: newer assistant text goes in a
             * fresh bubble after the user's turn. */
            break;
        }
        if (merge_idx >= 0) {
            /* Update the assistant bubble we just found, whether it's
             * the last entry or something older with system messages
             * in between. */
            chat_msg_t *m = chat_store_get_mut(merge_idx);
            if (m) {
                safe_copy(m->text, sizeof(m->text), p->text ? p->text : "");
                m->height_px = -1;
            }
            if (s_view) {
                /* Invalidate the existing slot so refresh re-runs
                 * slot_bind and picks up the new text — without this,
                 * refresh sees data_idx==merge_idx already bound and
                 * skips, leaving the visible label stale. */
                chat_msg_view_invalidate_index(s_view, merge_idx);
                chat_msg_view_refresh(s_view);
                chat_msg_view_scroll_to_bottom(s_view);   /* closes #107 */
            }
        } else {
            /* No assistant bubble within the walkback window — append
             * fresh.  Also covers the case where the user bubble was
             * just added and this is the first AI response.
             *
             * Previously this branch fired `chat_store_update_last_text`
             * when s_streaming was true even though merge_idx was -1,
             * which overwrote a USER bubble with the AI text. */
            ui_chat_add_message(p->text, false);
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
    tab5_lv_async_call(async_push_msg_cb, p);
}

/* System / status bubble (tool-call activity, session notices). */
static void async_push_system_cb(void *arg)
{
    char *text = (char *)arg;
    if (!text) return;
    chat_msg_t msg = {0};
    msg.type = MSG_SYSTEM;
    msg.is_user = false;
    msg.timestamp = now_ts();
    msg.height_px = -1;
    safe_copy(msg.text, sizeof(msg.text), text);
    chat_store_add(&msg);
    suggestions_sync_visibility();
    if (s_view) {
        chat_msg_view_refresh(s_view);
        chat_msg_view_scroll_to_bottom(s_view);
    }
    free(text);
}

void ui_chat_push_system(const char *text)
{
    if (!text || !*text) return;
    char *copy = strdup(text);
    if (!copy) return;
    tab5_lv_async_call(async_push_system_cb, copy);
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
    /* Only kick the view refresh when chat is currently visible.  If
     * the user is on Camera/Home/etc., the view-refresh-while-hidden
     * tries to lay out + decode the image into widgets that aren't on
     * screen, which combined with the fragmented LV pool after camera
     * use is a known PANIC trigger.  ui_chat_show()'s render path
     * already calls chat_msg_view_refresh, so the bubble will surface
     * the next time chat opens. */
    if (s_view && s_active) {
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
    tab5_lv_async_call(async_push_media_cb, m);
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
    /* Phase 2 (#70): plumb action fields through to the renderer.
     * action_label[0]=='\0' is the no-action sentinel. */
    safe_copy(msg.card_id,      sizeof(msg.card_id),      c->card_id);
    safe_copy(msg.action_label, sizeof(msg.action_label), c->action_label);
    safe_copy(msg.action_event, sizeof(msg.action_event), c->action_event);
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
    /* Phase 2 (#70): legacy entry point — no action.  Delegate to the
     * full path with NULL action so there's a single async cb to
     * maintain. */
    ui_chat_push_card_action(title, subtitle, image_url, description,
                             NULL, NULL, NULL);
}

void ui_chat_push_card_action(const char *title, const char *subtitle,
                              const char *image_url, const char *description,
                              const char *card_id,
                              const char *action_label,
                              const char *action_event)
{
    push_card_t *c = calloc(1, sizeof(*c));
    if (!c) return;
    safe_copy(c->title,    sizeof(c->title),    title);
    safe_copy(c->subtitle, sizeof(c->subtitle), subtitle);
    safe_copy(c->img,      sizeof(c->img),      image_url);
    safe_copy(c->desc,     sizeof(c->desc),     description);
    /* Phase 2 (#70): only stamp action fields when ALL three are
     * present.  Skill emitting card_id without action would still
     * round-trip cleanly (id is informational), but rendering an
     * action button without an event to fire makes no sense. */
    if (card_id && card_id[0] && action_label && action_label[0]
        && action_event && action_event[0]) {
        safe_copy(c->card_id,      sizeof(c->card_id),      card_id);
        safe_copy(c->action_label, sizeof(c->action_label), action_label);
        safe_copy(c->action_event, sizeof(c->action_event), action_event);
    }
    tab5_lv_async_call(async_push_card_cb, c);
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
    tab5_lv_async_call(async_push_audio_cb, a);
}

/* Audit U5 (#206): tap-to-play for inline audio_clip rows.
 *
 * Flow:
 *   tap on chat row → ui_chat_play_audio_clip(url) (LVGL/voice thread)
 *      → tab5_worker_enqueue(audio_clip_dl_job)
 *         → HTTP GET into /sdcard/.audio_clip.wav (chunked, max 4 MB)
 *         → tab5_lv_async_call(async_open_audio_player_cb, path)
 *            → ui_audio_create("/sdcard/.audio_clip.wav") on UI thread.
 *
 * Cap chosen to bound SD-card writes — typical TTS clips are 30-300 KB.
 * If the URL is relative ("/api/media/..."), the configured Dragon
 * host:port is prepended (mirrors media_cache_fetch). */
#define AUDIO_CLIP_MAX_BYTES   (4 * 1024 * 1024)
/* Tab5's FATFS is built with CONFIG_FATFS_LFN_NONE — only 8.3 names are
 * accepted (longer basenames trip EINVAL inside f_open).  "ACLIP.WAV"
 * is 5+3 chars which is well within limits. */
#define AUDIO_CLIP_TMP_PATH    "/sdcard/ACLIP.WAV"

static void async_open_audio_player_cb(void *arg)
{
    char *path = (char *)arg;
    if (!path) return;
    /* ui_audio_create takes the WAV path and overlays the player on the
     * current screen.  It internally calls ui_audio_destroy() if a
     * previous player is open, so back-to-back taps Just Work. */
    ui_audio_create(path);
    free(path);
}

typedef struct {
    char url[384];
} audio_clip_job_t;

static void audio_clip_dl_job(void *arg)
{
    audio_clip_job_t *j = (audio_clip_job_t *)arg;
    if (!j) return;

    /* Resolve relative URLs against the configured Dragon host. */
    char full_url[512];
    if (strncasecmp(j->url, "http://", 7) == 0 ||
        strncasecmp(j->url, "https://", 8) == 0) {
        snprintf(full_url, sizeof(full_url), "%s", j->url);
    } else {
        char host[64];
        tab5_settings_get_dragon_host(host, sizeof(host));
        uint16_t port = tab5_settings_get_dragon_port();
        snprintf(full_url, sizeof(full_url), "http://%s:%u%s",
                 host, port, j->url[0] == '/' ? j->url : "/");
        if (j->url[0] != '/') {
            /* unusual — fall back to verbatim */
            snprintf(full_url, sizeof(full_url), "%s", j->url);
        }
    }
    free(j);
    j = NULL;

    ESP_LOGI(TAG, "audio_clip: GET %s", full_url);

    esp_http_client_config_t cfg = {
        .url        = full_url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "audio_clip: http_client_init OOM");
        return;
    }

    bool ok = false;
    FILE *fp = NULL;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(TAG, "audio_clip: http_client_open failed");
            break;
        }
        int cl = (int)esp_http_client_fetch_headers(client);
        if (cl > AUDIO_CLIP_MAX_BYTES) {
            ESP_LOGW(TAG, "audio_clip: too large (%d bytes)", cl);
            break;
        }
        fp = fopen(AUDIO_CLIP_TMP_PATH, "wb");
        if (!fp) {
            ESP_LOGW(TAG, "audio_clip: fopen %s failed errno=%d (%s)",
                     AUDIO_CLIP_TMP_PATH, errno, strerror(errno));
            break;
        }
        char chunk[2048];
        int total = 0;
        while (total < AUDIO_CLIP_MAX_BYTES) {
            int r = esp_http_client_read(client, chunk, sizeof(chunk));
            if (r <= 0) break;
            if (fwrite(chunk, 1, (size_t)r, fp) != (size_t)r) {
                ESP_LOGW(TAG, "audio_clip: short fwrite");
                break;
            }
            total += r;
            /* Yield so voice WS / LVGL keep ticking on long downloads. */
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        int status = esp_http_client_get_status_code(client);
        if (status == 200 && total > 64) {
            ESP_LOGI(TAG, "audio_clip: saved %d bytes to %s",
                     total, AUDIO_CLIP_TMP_PATH);
            ok = true;
        } else {
            ESP_LOGW(TAG, "audio_clip: status=%d total=%d", status, total);
        }
    } while (0);

    if (fp) fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ok) {
        char *path = strdup(AUDIO_CLIP_TMP_PATH);
        if (path) tab5_lv_async_call(async_open_audio_player_cb, path);
    } else {
        /* Soft-fail: a single toast is friendlier than silently doing
         * nothing.  ui_home_show_toast is the global toast surface. */
        extern void ui_home_show_toast(const char *text);
        ui_home_show_toast("Couldn't fetch audio");
    }
}

/* U12 (#206): live STT-partial caption above the chat pill.
 * Thread-safe — voice.c calls it from the WS task on every stt_partial
 * frame.  We strdup so the WS buffer is free to be reused, then
 * lv_async_call hops to the LVGL thread for the actual label update. */
static void async_set_partial_cb(void *arg)
{
    char *txt = (char *)arg;
    if (s_input) chat_input_bar_show_partial(s_input, txt);
    free(txt);
}

void ui_chat_show_partial(const char *partial)
{
    /* NULL/empty → hide.  Skip the strdup hop — pass NULL through. */
    if (!partial || !*partial) {
        tab5_lv_async_call(async_set_partial_cb, NULL);
        return;
    }
    char *copy = strdup(partial);
    if (!copy) return;
    tab5_lv_async_call(async_set_partial_cb, copy);
}

void ui_chat_play_audio_clip(const char *url)
{
    if (!url || !url[0]) return;
    audio_clip_job_t *j = calloc(1, sizeof(*j));
    if (!j) return;
    snprintf(j->url, sizeof(j->url), "%s", url);
    if (tab5_worker_enqueue(audio_clip_dl_job, j, "audio_clip_dl") != ESP_OK) {
        ESP_LOGW(TAG, "audio_clip: worker queue full, dropping");
        free(j);
    }
}

static void async_update_last_cb(void *arg)
{
    push_update_t *u = (push_update_t *)arg;
    if (!u) return;
    if (u->text) {
        if (*u->text) {
            chat_store_update_last_text(u->text);
        } else {
            /* Audit D6: Dragon stripped the whole response (pure code block
             * that was rendered as a JPEG media event). Remove the now-empty
             * MSG_TEXT bubble so the image breakout stands alone — otherwise
             * the user sees a blank bubble above the rendered code. */
            chat_store_pop_last();
        }
        if (s_view) {
            chat_msg_view_refresh(s_view);
            chat_msg_view_scroll_to_bottom(s_view);   /* closes #107 */
        }
    }
    free(u->text); free(u);
}

void ui_chat_update_last_message(const char *text)
{
    if (!text) return;
    push_update_t *u = calloc(1, sizeof(*u));
    if (!u) return;
    /* strdup("") is legal; async callback distinguishes empty → remove. */
    u->text = strdup(text);
    if (!u->text) { free(u); return; }
    tab5_lv_async_call(async_update_last_cb, u);
}
