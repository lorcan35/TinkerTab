/*
 * ui_voice.c — TinkerOS voice overlay
 * 720x1280 portrait, LVGL v9
 *
 * Full-screen voice interaction overlay with animated orb feedback.
 * Two parts:
 *   1. Floating mic button (always visible, bottom-left)
 *   2. Full-screen voice overlay (shown during any active voice state)
 *
 * Sits on lv_layer_top() alongside the keyboard overlay.
 * Wires into voice.h state callbacks for real-time visual feedback.
 *
 * The orb is the soul of Tinker — it breathes, pulses, and responds.
 */

#include "ui_voice.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "md_strip.h" /* #116: inline markdown cleanup */
#include "mode_manager.h"
#include "settings.h"
#include "spring_anim.h" /* Phase 3 of #42 — spring-driven orb size */
#include "task_worker.h"
#include "ui_core.h" /* tab5_lv_async_call (#258) */
#include "ui_notes.h"
#include "ui_orb.h" /* PR 2 polish: ui_orb_pipeline_active() for overlay suppression */
#include "ui_theme.h" /* TT #328 Wave 4: TH_MODE_*/ TH_STATUS_ *tokens for state - icon hues * /
#include "voice.h" /* W7-E.4c: voice_is_channel_reply_armed + getters */

static const char *TAG = "ui_voice";

/* Forward declarations for mode switch helper tasks */
static void mode_switch_voice_job(void *arg);
static void mode_switch_idle_job(void *arg);
/* TT #328 Wave 4 — state icon helper, defined below near show_state_*. */
static void set_state_icon(const char *glyph, uint32_t color_hex);

/* ── Palette — Voice overlay (v5 Zero Interface — amber-led) ─────
   Names kept for minimal-diff; values map to the v5 ui_theme palette.
   TT #328 Wave 4 — pre-Wave-4 the state ball used motion alone to
   convey LISTENING / PROCESSING / SPEAKING (all amber-family); a
   colorblind-sim audit + still-screenshot check showed each state was
   visually identical at rest, failing WCAG 1.4.1.  Now state is also
   conveyed by a per-state icon glyph (s_lbl_state_icon: mic/refresh/
   speaker/check) in distinct hues (green/violet/blue/green-check).
   The amber palette below remains the orb's identity colour. */
#define VO_BG              0x08080E   /* TH_BG — not pure black */
#define VO_BG_OPA          LV_OPA_COVER

/* "Cyan" slot — now amber hot, used for LISTENING accents */
#define VO_CYAN            0xF59E0B   /* TH_AMBER */
#define VO_CYAN_DIM        0x3E2A0A   /* amber at ~15% on dark */
#define VO_CYAN_BORDER     0x5E3F0C   /* amber at ~25% on dark */
#define VO_CYAN_GLOW       0x7A4A06   /* TH_AMBER_DEEP — inner glow */

/* "Purple" slot — processing; use the Claw rose so the orb picks up the
   mode identity during thinking without introducing a new hue family. */
#define VO_PURPLE          0xFFB637   /* amber hot — warmer processing */
#define VO_PURPLE_DIM      0x5E3F0C

/* "Green" slot — speaking; amber-hot for the speaking breathe */
#define VO_GREEN           0xFFD88A   /* amber light, for speaking highlight */
#define VO_GREEN_DIM       0x7A4A06

/* Text — v5 theme */
#define VO_TEXT_BRIGHT     0xEDEDEF   /* TH_TEXT_PRIMARY */
#define VO_TEXT_MID        0xAAAAAA   /* TH_TEXT_BODY */
#define VO_TEXT_DIM        0x666666   /* TH_TEXT_SECONDARY */
#define VO_TEXT_FAINT      0x44444c   /* TH_TEXT_DIM */
#define VO_CLOSE_TEXT      0x7a7a82

/* ── Layout constants ─────────────────────────────────────────── */
#define SW                 720
#define SH                 1280

#define MIC_BTN_SZ         72
#define MIC_BTN_MARGIN     20
#define MIC_BTN_BOTTOM     140       /* above 120px nav bar + 20px margin */
#define MIC_DOT_SZ         12        /* inner dot indicator */

/* v5: orb carries the identity. Push from 200 -> 300 for LISTENING and
   320 for SPEAKING so presence matches the home orb. Ring stroke slightly
   heavier (2 -> 3) so the arc reads from across the desk. */
/* closes #114: distinct orb sizes per state — LISTENING medium,
 * PROCESSING smaller (visually "compressed" while thinking), SPEAKING
 * largest (emphasis while the device is talking). */
#define ORB_SZ_LISTEN      300
#define ORB_SZ_PROCESS     240
#define ORB_SZ_SPEAK       340
#define ORB_RING_W         3
#define ORB_GLOW_LAYERS    4         /* concentric circles for radial gradient */

#define CLOSE_BTN_SZ       56
/* TT #511 wave-1.7: X close button moved BELOW the status bar (was
 * 16 px from top, overlapping the "Thursday • HH:MM" time text).  80 px
 * margin clears the status bar entirely. */
#define CLOSE_BTN_MARGIN_X 16
#define CLOSE_BTN_MARGIN_Y 80

/* Chat area — between orb and send button */
/* Wave-1.8: orb moved back to y=320 (upper-third); voice text offset
 * returns to anchor below it.  -260 → status label at 640 + 150 -
 * 260 + 30 = 560, sitting ~50 px below the orb's bottom edge (~410). */
#define ORB_Y_OFFSET -260
#define CHAT_TOP           440        /* y where chat area starts */
#define CHAT_BOTTOM        1020       /* y where chat area ends (above send btn) */
#define CHAT_PAD_X         24         /* horizontal padding */
#define CHAT_BUBBLE_RAD    16         /* bubble corner radius */
#define CHAT_BUBBLE_PAD    14         /* text padding inside bubble */
#define CHAT_BUBBLE_MAX_W  500        /* max bubble width */
#define CHAT_GAP           12         /* vertical gap between bubbles */

/* Bubble colors — v5: user bubble is the only "surface" (amber, user-led),
   AI "bubble" is really just body text so the bg is elevated-card. */
#define VO_USER_BG         0x5E3F0C   /* amber-dim tint for user bubble bg */
#define VO_USER_BORDER     0x7A4A06   /* amber border */
#define VO_AI_BG           0x13131F   /* TH_CARD_ELEVATED */
#define VO_AI_BORDER       0x1A1A24   /* hairline border */

#define WAVE_BARS          5
#define WAVE_BAR_W         6
#define WAVE_BAR_GAP       10
#define WAVE_BAR_MAX_H     48
#define WAVE_BAR_MIN_H     10

/* Send/Stop button (shown during LISTENING) */
#define SEND_BTN_SZ        80
/* Wave-1.8: orb back to upper-third → status text now ends around
 * y=620, so the stop button at y=750 keeps a tight ~80 px pairing
 * with the text without falling to the very bottom of the screen. */
#define SEND_BTN_Y 750               /* y-center from top */
#define SEND_ICON_SZ       24        /* inner square "stop" icon */

/* Mic dot pulse animation */
#define MIC_DOT_PULSE_MS   800       /* mic dot pulse cycle */

/* Animation timing */
#define ANIM_FADE_IN_MS    200
#define ANIM_FADE_OUT_MS   150
#define ANIM_BREATHE_MS    1500      /* listening orb breathe cycle */
#define ANIM_PULSE_MS      800       /* processing orb pulse cycle */
#define ANIM_COLOR_MS      300       /* state transition color morph */
#define ANIM_WAVE_MS       400       /* wave bar animation cycle */
#define ANIM_DOT_MS        600       /* thinking dots cycle */
#define ANIM_HIDE_DELAY_MS 1500      /* delay before auto-hide after speaking done */

/* ── Forward declarations ─────────────────────────────────────── */
static void build_mic_button(void);
static void build_overlay(void);
static void build_orb(lv_obj_t *parent);
static void build_wave_bars(lv_obj_t *parent);
static void build_close_button(lv_obj_t *parent);
static void build_send_button(lv_obj_t *parent);
/* U20 (#206): build_chat_area + bubble pointers removed.  The voice
 * overlay now relies entirely on s_response_label for visible text;
 * the offscreen flex chat container was dead since v5. */

static void mic_click_cb(lv_event_t *e);
static void mic_long_press_cb(lv_event_t *e);
static void close_click_cb(lv_event_t *e);
static void send_click_cb(lv_event_t *e);
static void orb_speak_click_cb(lv_event_t *e);
static void orb_ready_click_cb(lv_event_t *e);
static void mic_dot_pulse_cb(void *obj, int32_t val);

static void start_breathe_anim(void);
static void start_pulse_anim(void);
/* W15-P02: RMS-driven "listening back" glow on the innermost orb layer
 * during LISTENING so the orb visibly reacts to the user's voice. */
static void start_rms_anim(void);
static void stop_rms_anim(void);
static void rms_tick_cb(lv_timer_t *t);
static void start_wave_anim(void);
static void stop_all_anims(void);

static void orb_breathe_cb(void *obj, int32_t val);
static void orb_ring_opa_cb(void *obj, int32_t val);
static void wave_bar_cb(void *obj, int32_t val);
static void fade_overlay_cb(void *obj, int32_t val);
static void dot_timer_cb(lv_timer_t *t);
static void auto_hide_timer_cb(lv_timer_t *t);
static void rec_timer_cb(lv_timer_t *t);

static void set_orb_color(uint32_t ring_hex, uint32_t glow_hex, lv_opa_t ring_opa);
static void set_orb_size(int32_t sz);
static void update_mic_button_state(voice_state_t state);
static void show_state_listening(void);
static void show_state_processing(const char *transcript);
static void show_state_speaking(void);
static void show_state_idle(void);

/* ── State ────────────────────────────────────────────────────── */

/* Mic button */
static lv_obj_t  *s_mic_btn       = NULL;
static bool       s_dictation_from_anywhere = false;
static bool       s_pending_ask   = false;  /* true = auto-start ASK when connected */
static lv_obj_t  *s_mic_dot       = NULL;

/* Overlay container */
static lv_obj_t  *s_overlay       = NULL;
static bool       s_visible       = false;

/* Orb elements */
static lv_obj_t  *s_orb_container = NULL;   /* invisible sizer for scaling */
static lv_obj_t  *s_orb_ring      = NULL;   /* outer ring stroke */
static lv_obj_t  *s_orb_glow[ORB_GLOW_LAYERS]; /* concentric gradient circles */
/* closes #113: home-parity halo rings.  The home screen's v4 Ambient
 * Canvas orb has 3 concentric border-only rings (310/240/196) that
 * make it feel alive.  Port them into the overlay so engaging the
 * device doesn't visually downgrade the orb. */
static lv_obj_t  *s_orb_halo_outer = NULL;     /* 310 px border-only */
static lv_obj_t  *s_orb_halo_mid   = NULL;     /* 240 px border-only */
static lv_obj_t  *s_orb_halo_inner = NULL;     /* 196 px border-only */

/* Text labels */
static lv_obj_t  *s_lbl_status    = NULL;   /* "Listening..." / "Thinking..." / "Speaking..." */
static lv_obj_t  *s_lbl_transcript = NULL;  /* STT transcript */
static lv_obj_t  *s_lbl_dots      = NULL;   /* "..." animated dots */
/* W7-E.4c: reply-context chip — shown at top of overlay when a
 * channel reply is armed (voice_is_channel_reply_armed() == true).
 * Hidden by default; populated in ui_voice_show via the armed-context
 * accessor (which doesn't consume — that happens at STT-complete). */
static lv_obj_t *s_lbl_reply_chip = NULL;

/* Wave bars */
static lv_obj_t  *s_wave_cont     = NULL;
static lv_obj_t  *s_wave_bars[WAVE_BARS];

/* Close button */
static lv_obj_t  *s_close_btn     = NULL;

/* Send/Stop button (LISTENING state) */
static lv_obj_t  *s_send_btn      = NULL;

/* Chat area — scrollable container with user/AI bubbles */
/* U20 (#206): chat container + bubble statics removed.  s_response_label
 * is the actually-visible text path; the offscreen scroll/flex tree
 * never rendered to the user since v5. */
/* closes #115: simple fixed-position response label below the orb.
 * The chat_cont/ai_bubble path (flex layout, scrollable) was not
 * rendering reliably during SPEAKING — bubble was hidden behind the
 * orb's z-order or sized to zero.  This label lives directly on the
 * overlay, positioned absolutely, and shows whatever voice_get_llm_text
 * returned during the turn.  Simple, visible, always on top. */
static lv_obj_t  *s_response_label = NULL;
static bool       s_has_llm_text  = false;  /* whether LLM response has started */

/* Recording duration label + timer */
static lv_obj_t   *s_lbl_rec_time = NULL;
static lv_obj_t   *s_lbl_sub      = NULL;   /* v5 spec shot-05 caption: "LISTENING • HOLD TO TALK" / "DICTATION • TAP TO STOP" */
/* TT #328 Wave 4 — per-state icon glyph.  Audit P0 #8 flagged that the
 * orb cycles through four amber shades (LISTENING/PROCESSING/SPEAKING/
 * READY) — colorblind users + still screenshots can't distinguish state.
 * Pre-Wave-4 the comment at line 43 openly said "state is conveyed by
 * motion, not hue", which fails WCAG 1.4.1 (Use of Color).  Adding an
 * icon next to the status word makes each state visually distinct in a
 * still frame: mic / refresh / speaker / check. */
static lv_obj_t *s_lbl_state_icon = NULL;
static lv_timer_t *s_rec_timer    = NULL;
static int         s_rec_seconds  = 0;

/* Timers */
static lv_timer_t *s_dot_timer    = NULL;
static lv_timer_t *s_hide_timer   = NULL;
static lv_timer_t *s_stuck_timer  = NULL;  /* watchdog for stuck PROCESSING state */
static lv_timer_t *s_auto_hide   = NULL;   /* READY-state auto-dismiss (was local static — caused UAF) */
static lv_timer_t *s_rms_timer   = NULL;   /* W15-P02: live RMS → inner-glow opa during LISTENING */
static int         s_dot_phase    = 0;

/* Current state tracking */
static voice_state_t s_cur_state  = VOICE_STATE_IDLE;
static bool s_initialized         = false;

/* Boot auto-connect: suppress overlay during initial voice WS connect */
static bool s_boot_connect        = false;

/* ================================================================
 *  Public API
 * ================================================================ */

void ui_voice_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return;
    }

    ESP_LOGI(TAG, "Initializing voice UI overlay");

    build_mic_button();
    build_overlay();

    /* Register as voice state callback */
    voice_init(ui_voice_on_state_change);

    s_initialized = true;
    ESP_LOGI(TAG, "Voice UI initialized — mic button at bottom-left");
}

void ui_voice_set_boot_connect(bool silent)
{
    s_boot_connect = silent;
    ESP_LOGI(TAG, "Boot connect mode: %s", silent ? "ON (silent)" : "OFF");
}

/* Watchdog: if stuck in PROCESSING/SPEAKING past the mode budget,
 * force-cancel.  This catches the case where the WS receive task
 * dies and the in-task timeout never fires.
 *
 * The budget is mode-aware -- local Ollama on Dragon ARM64 CPU is
 * ~7 tok/s, so a 200-token reply can take 30 s + tool-chain latency
 * + memory-augmented context building, easily 60-90 s.  The 10 s
 * timer we had before killed every Local turn before the first
 * token streamed back. */
#define STUCK_BUDGET_LOCAL_MS        300000   /* 5 min, tool chains */
#define STUCK_BUDGET_HYBRID_MS       240000   /* 4 min, local LLM */
#define STUCK_BUDGET_CLOUD_MS         75000   /* 75 s incl. TTS */
#define STUCK_BUDGET_TINKERCLAW_MS   240000   /* 4 min, agent steps */

static uint32_t stuck_budget_ms_for_current_mode(void)
{
    uint8_t m = tab5_settings_get_voice_mode();
    switch (m) {
        case VOICE_MODE_LOCAL:      return STUCK_BUDGET_LOCAL_MS;
        case VOICE_MODE_HYBRID:     return STUCK_BUDGET_HYBRID_MS;
        case VOICE_MODE_CLOUD:      return STUCK_BUDGET_CLOUD_MS;
        case VOICE_MODE_TINKERCLAW: return STUCK_BUDGET_TINKERCLAW_MS;
        default:                    return STUCK_BUDGET_CLOUD_MS;
    }
}

static void stuck_watchdog_cb(lv_timer_t *t)
{
    s_stuck_timer = NULL;
    if (s_cur_state == VOICE_STATE_PROCESSING || s_cur_state == VOICE_STATE_SPEAKING) {
        ESP_LOGW(TAG, "Stuck watchdog: force-cancelling after %lu ms in state %d",
                 (unsigned long)stuck_budget_ms_for_current_mode(), s_cur_state);
        voice_cancel();
        /* Show error and auto-hide */
        stop_all_anims();
        lv_label_set_text(s_lbl_status, "Timed out — try again");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xFF4444), 0);
        lv_obj_set_style_text_font(s_lbl_status, FONT_HEADING, 0);
        lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
        s_hide_timer = lv_timer_create(auto_hide_timer_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_hide_timer, 1);
    }
}

void ui_voice_on_state_change(voice_state_t state, const char *detail)
{
    ESP_LOGI(TAG, "State change: %d -> %d (%s)", s_cur_state, state,
             detail ? detail : "");

    /* Notify home so the sys-label picks up the transition within one LVGL
       tick instead of waiting for the 5 s poll. Safe from any core via
       lv_async_call. */
    extern void ui_home_refresh_sys_label(void);
    ui_home_refresh_sys_label();

    s_cur_state = state;
    update_mic_button_state(state);

    /* W7-E.4c: reply chip is meaningful only during the LISTENING phase.
     * Once the user releases the orb (→ PROCESSING/SPEAKING/READY/IDLE)
     * the reply context has either been consumed (real channel_reply
     * sent) or the user gave up — either way, the "Replying to X" hint
     * is stale.  Hide it. */
    if (s_lbl_reply_chip && state != VOICE_STATE_LISTENING && state != VOICE_STATE_CONNECTING) {
       lv_obj_add_flag(s_lbl_reply_chip, LV_OBJ_FLAG_HIDDEN);
    }

    /* Cancel any pending auto-hide */
    if (s_hide_timer) {
        lv_timer_delete(s_hide_timer);
        s_hide_timer = NULL;
    }

    /* Manage stuck watchdog timer */
    if (s_stuck_timer) {
        lv_timer_delete(s_stuck_timer);
        s_stuck_timer = NULL;
    }
    if (state == VOICE_STATE_PROCESSING || state == VOICE_STATE_SPEAKING) {
        uint32_t budget = stuck_budget_ms_for_current_mode();
        s_stuck_timer = lv_timer_create(stuck_watchdog_cb, budget, NULL);
        lv_timer_set_repeat_count(s_stuck_timer, 1);
        ESP_LOGI(TAG, "Stuck watchdog armed -> %lu ms (mode=%d)",
                 (unsigned long)budget, tab5_settings_get_voice_mode());
    }

    switch (state) {
    case VOICE_STATE_RECONNECTING:
        /* T1.2: RECONNECTING is a transient backoff state.  Don't show
         * the voice overlay, don't flash "Disconnected" in red -- the
         * home pill (voice_get_degraded_reason) handles user-visible
         * status.  Just let any active overlay dismiss quietly if
         * needed. */
        if (s_visible && !s_pending_ask && !s_dictation_from_anywhere) {
            ui_voice_hide();
        }
        break;
    case VOICE_STATE_IDLE:
        /* Clear boot connect flag — either connected or failed */
        if (s_boot_connect) {
            s_boot_connect = false;
            ESP_LOGI(TAG, "Boot connect ended (IDLE)");
        }
        /* Voice session ended — return to IDLE (no auto-streaming).
         * Must defer because we're inside the LVGL mutex here.
         * Wave 14 W14-H06: dispatch via the shared tab5_worker queue
         * instead of spawning a one-shot task that would leak its
         * 8 KB stack + TCB. */
        if (tab5_mode_get() == MODE_VOICE) {
            ESP_LOGI(TAG, "Voice ended, scheduling switch to IDLE");
            tab5_worker_enqueue(mode_switch_idle_job, NULL, "mode_idle");
        }
        /* C5: Offline recording fallback — if connect failed and user wanted
         * to record, fall back to local SD card recording via Notes. */
        if (detail && strstr(detail, "connect failed")
            && (s_pending_ask || s_dictation_from_anywhere)) {
            ESP_LOGI(TAG, "Dragon offline — falling back to local recording");
            s_pending_ask = false;
            s_dictation_from_anywhere = false;
            if (s_visible) ui_voice_hide();
            /* Start SD-only dictation via Notes module */
            const char *wav_path = ui_notes_start_recording();
            ESP_LOGI(TAG, "Offline recording started: %s", wav_path ? wav_path : "(null)");
            break;
        }
        if (detail && s_visible) {
            /* Show error briefly before hiding (e.g. "connect failed", Dragon error) */
            stop_all_anims();
            if (strstr(detail, "disconnect")) {
                lv_label_set_text(s_lbl_status, "Disconnected");
            } else if (strstr(detail, "timeout")) {
                lv_label_set_text(s_lbl_status, "Response timed out");
            } else {
                /* Dragon-side error or other failure — show the actual message */
                lv_label_set_text(s_lbl_status, detail);
            }
            lv_obj_set_style_text_color(s_lbl_status,
                lv_color_hex(0xFF4444), 0);
            lv_obj_set_style_text_font(s_lbl_status,
                FONT_BODY, 0);
            lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, ORB_Y_OFFSET + 40);
            lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
            /* Auto-hide after 3s (longer for Dragon errors so user can read) */
            s_hide_timer = lv_timer_create(auto_hide_timer_cb, 3000, NULL);
            lv_timer_set_repeat_count(s_hide_timer, 1);
        } else {
            show_state_idle();
        }
        break;
    case VOICE_STATE_CONNECTING:
        if (s_boot_connect) {
            /* Silent boot connect — only update mic dot, don't show overlay */
            ESP_LOGI(TAG, "Boot connect: CONNECTING (silent)");
            break;
        }
        /* closes #140: only pop the voice overlay on CONNECTING if the
         * user actually initiated something (mic tap pending, or
         * overlay was already visible mid-turn).  Previously EVERY
         * reconnect after boot would pop the overlay, even when the
         * user was on chat or home — the typing flow got hijacked
         * whenever WS bounced for ngrok/LAN auto-fallback.  Boot path
         * already had its own silent gate; this adds one for runtime
         * reconnects. */
        if (!s_visible && !s_pending_ask && !s_dictation_from_anywhere) {
            ESP_LOGI(TAG, "Silent reconnect: CONNECTING (no overlay popup)");
            break;
        }
        /* Show overlay immediately with "Connecting..." */
        if (!s_visible) {
            ui_voice_show();
        }
        stop_all_anims();
        lv_label_set_text(s_lbl_status, "Connecting...");
        lv_obj_set_style_text_font(s_lbl_status, FONT_HEADING, 0);
        lv_obj_set_style_text_color(s_lbl_status,
            lv_color_hex(VO_TEXT_DIM), 0);
        lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0,
                     ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 30);
        lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
        /* W15-P01 voice-flow polish: same hollow-outline bug as READY
         * — dark-amber glow on near-black bg renders as invisible.
         * Use VO_CYAN for both ring + glow; lower opa keeps it subtler
         * than LISTENING. */
        set_orb_color(VO_CYAN, VO_CYAN, LV_OPA_30);
        set_orb_size(ORB_SZ_LISTEN);
        start_pulse_anim();
        break;
    case VOICE_STATE_READY:
       /* TT #328 Wave 4: check icon for READY — green check matches the
        * "task complete / waiting for next" semantic. */
       set_state_icon(LV_SYMBOL_OK, TH_STATUS_GREEN);
       /* Dictation from anywhere: save note and hide overlay */
       if (s_dictation_from_anywhere && detail &&
           (strcmp(detail, "dictation_done") == 0 || strcmp(detail, "dictation_summary") == 0)) {
          const char *txt = voice_get_dictation_text();
          if (txt && txt[0]) {
             const char *title = voice_get_dictation_title();
             if (title && title[0]) {
                char buf[600];
                snprintf(buf, sizeof(buf), "[%s] %s", title, txt);
                ui_notes_add(buf, true);
             } else {
                ui_notes_add(txt, true);
             }
             ESP_LOGI(TAG, "Dictation note saved from anywhere (%u chars)", (unsigned)strlen(txt));
          }
          s_dictation_from_anywhere = false;
          ui_voice_hide();
          break;
       }

        /* Clear boot connect flag — successfully connected */
        if (s_boot_connect) {
            s_boot_connect = false;
            ESP_LOGI(TAG, "Boot connect succeeded — voice READY (silent)");
            /* Don't show overlay or auto-start — just sit ready */
            break;
        }

        /* Don't auto-show overlay on READY — only show when user taps mic/orb.
         * Just update the overlay state if it's already visible. */
        stop_all_anims();

        /* If chat bubbles have content, this is a follow-up turn.
         * Auto-hide overlay after 4s so user can see home/chat screen. */
        bool has_conversation = s_has_llm_text;
        if (has_conversation) {
            lv_label_set_text(s_lbl_status, "");
            /* Auto-dismiss: timeout scales with LLM response length.
             * Short responses (< ~130 chars) = 4s minimum.
             * ~30ms per character reading speed, capped at 15s max.
             * Uses file-scope s_auto_hide — cleaned up in stop_all_anims().
             * NEVER use auto_delete — leaves dangling pointer → UAF crash. */
            uint32_t hide_ms = 4000;
            const char *llm_txt = voice_get_llm_text();
            if (llm_txt && llm_txt[0]) {
                uint32_t len_ms = (uint32_t)(strlen(llm_txt)) * 30;
                if (len_ms < 4000) len_ms = 4000;
                if (len_ms > 15000) len_ms = 15000;
                hide_ms = len_ms;
            }
            ESP_LOGI(TAG, "Auto-hide in %lu ms (text len=%u)",
                     (unsigned long)hide_ms,
                     (unsigned)(llm_txt ? strlen(llm_txt) : 0));
            if (s_auto_hide) { lv_timer_delete(s_auto_hide); s_auto_hide = NULL; }
            s_auto_hide = lv_timer_create(auto_hide_timer_cb, hide_ms, NULL);
            lv_timer_set_repeat_count(s_auto_hide, 1);
        } else {
            lv_label_set_text(s_lbl_status, "Tap to speak.");
        }
        if (s_lbl_sub) lv_obj_add_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(VO_CYAN), 0);
        lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 30);
        lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
        /* W15-P01 voice-flow polish: was (VO_CYAN, VO_CYAN_DIM, LV_OPA_50)
         * → dark-amber glow on near-black renders as near-invisible,
         * leaving ONLY the ring stroke.  On-screen result was a hollow
         * orange outline at end-of-turn (screenshot 07), visually
         * indistinguishable from a rendering bug.  Use VO_CYAN for both
         * ring + glow so the orb has a visible warm fill in READY —
         * the breathe animation still provides motion, and brightness
         * stays subordinate to the hotter LISTENING/SPEAKING states
         * via a slightly lower ring opa. */
        set_orb_color(VO_CYAN, VO_CYAN, LV_OPA_50);
        set_orb_size(ORB_SZ_LISTEN);
        start_breathe_anim();
        /* Hide elements from other states */
        lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wave_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
        /* U20 (#206): drive the visible response label only — the
         * offscreen chat_cont/user_bubble/ai_bubble tree was deleted
         * since it never rendered to the user. closes #115. */
        if (has_conversation) {
            const char *llm_txt2 = voice_get_llm_text();
            if (llm_txt2 && llm_txt2[0] && s_response_label) {
                /* #78 + #160: tool-marker scrub before the markdown
                 * pass so raw <tool>...</tool> never flashes in the
                 * voice overlay caption either. */
                { char _m[256]; md_strip_tool_markers(llm_txt2, _m, sizeof(_m));
                  md_strip_inline_with_ellipsis(_m, _m, sizeof(_m));
                  lv_label_set_text(s_response_label, _m); }
                lv_obj_clear_flag(s_response_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_response_label);
            }
        }
        /* Make orb tappable to start recording.
         * Remove first to avoid stacking duplicate callbacks on re-entry. */
        lv_obj_add_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_event_cb(s_orb_container, orb_ready_click_cb);
        lv_obj_add_event_cb(s_orb_container, orb_ready_click_cb, LV_EVENT_CLICKED, NULL);

        /* Auto-start pending action from mic button tap/long-press */
        if (s_pending_ask) {
            s_pending_ask = false;
            ESP_LOGI(TAG, "READY: auto-starting Ask (pending from tap)");
            voice_start_listening();
        } else if (s_dictation_from_anywhere) {
            /* Don't clear flag here — cleared after dictation completes */
            ESP_LOGI(TAG, "READY: auto-starting Dictation (pending from long-press)");
            voice_start_dictation();
        }
        break;
    case VOICE_STATE_LISTENING:
        /* Only show voice overlay if user initiated from mic button.
         * If dictation started from Notes screen, Notes has its own UI. */
        if (!s_visible && (s_pending_ask || s_dictation_from_anywhere)) {
            ui_voice_show();
        }
        if (voice_get_mode() == VOICE_MODE_DICTATE && detail && detail[0]) {
            /* Dictation: partial transcript update — show running text */
            lv_label_set_text(s_lbl_status, "Dictating...");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(VO_TEXT_DIM), 0);
            lv_obj_set_style_text_font(s_lbl_status, FONT_BODY, 0);
            lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0,
                         ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 30);
            lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
            /* U20 (#206): partial-transcript bubble removed.  The
             * dictation status text in s_lbl_status above already
             * shows "Dictating..."; the partial preview lives in
             * the chat composer caption (#222 / U12). */
        } else {
            show_state_listening();
            /* Override status text for dictation mode */
            if (voice_get_mode() == VOICE_MODE_DICTATE) {
                lv_label_set_text(s_lbl_status, "Dictating...");
            }
        }
        break;
    case VOICE_STATE_PROCESSING:
        show_state_processing(detail);
        break;
    case VOICE_STATE_SPEAKING:
        show_state_speaking();
        break;
    }
}

void ui_voice_show(void)
{
    if (!s_overlay || s_visible) return;

    /* Cancel any in-flight fade-out animation */
    lv_anim_delete(s_overlay, fade_overlay_cb);

    s_visible = true;

    /* Wave 15 W15-C07: INSTANT show — removed the 200 ms fade-in
     * animation.  During fade-in, LVGL has to composite the partially-
     * transparent overlay against whatever is behind it (home screen
     * with a live chart widget, active chat history, etc.).  When the
     * background contains layered masks, the SW rasterizer crashes in
     * `lv_draw_sw_fill → lv_memset` with a bad dst pointer.
     * Repro: fire a chart widget → tap big orb → device panics in
     * ui_task during the 200 ms fade window.
     *
     * The HIDE path already went instant for the same class of bug
     * (see ui_voice_hide comment below).  This is the fade-IN
     * equivalent fix. */
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(s_overlay, VO_BG_OPA, 0);

    /* W7-E.4c: reveal the reply-context chip if the user just tapped
     * REPLY on a now-card.  Body extracted to
     * ui_voice_refresh_reply_chip so callers that arm context AFTER
     * the overlay is already shown (TT #481 — `ui_voice_show`
     * short-circuits at top) can trigger the repaint manually. */
    ui_voice_refresh_reply_chip();

    /* Child-opacity ramp is driven by fade_overlay_cb from start→end
     * in the old animation; skipping it means child content snaps on
     * too.  If a gentler reveal is needed later, animate individual
     * text labels' opacity instead of the container. */

    ESP_LOGI(TAG, "Voice overlay shown (instant, W15-C07)");
}

/* TT #481 (W7-E.4c follow-up): chip-repaint helper.  Was inlined inside
 * ui_voice_show; extracted so callers that arm context after the overlay
 * is already up (e.g. ui_notification_reply_current when chat → voice
 * overlay was already visible) can still trigger the visual repaint.
 * Idempotent — safe to call multiple times; bails when the chip widget
 * hasn't been built yet (overlay not constructed). */
void ui_voice_refresh_reply_chip(void) {
   if (!s_lbl_reply_chip) return;
   char ch[16] = {0}, thread[64] = {0}, sender[64] = {0};
   if (voice_peek_channel_reply(ch, thread, sender)) {
      char text[160];
      snprintf(text, sizeof(text), "Replying to %.40s (%.8s)", sender[0] ? sender : "?", ch[0] ? ch : "?");
      lv_label_set_text(s_lbl_reply_chip, text);
      lv_obj_clear_flag(s_lbl_reply_chip, LV_OBJ_FLAG_HIDDEN);
   } else {
      lv_obj_add_flag(s_lbl_reply_chip, LV_OBJ_FLAG_HIDDEN);
   }
}

void ui_voice_hide(void)
{
    if (!s_overlay || !s_visible) return;
    s_visible = false;

    stop_all_anims();

    /* Cancel any in-flight fade animation */
    lv_anim_delete(s_overlay, fade_overlay_cb);

    /* INSTANT hide — no fade animation.
     * Fade-out caused crash: 150ms window where fade_done_hide_cb fires
     * after navigation changed screen state underneath. */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(s_overlay, LV_OPA_COVER, 0);

    /* W7-E.4c: hide the reply chip on overlay-out so the next
     * unrelated open doesn't briefly flash stale text before
     * ui_voice_show's peek/repaint runs. */
    if (s_lbl_reply_chip) {
       lv_obj_add_flag(s_lbl_reply_chip, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Voice overlay hidden (instant)");
}

void ui_voice_dismiss_if_idle(void)
{
    if (!s_overlay || !s_visible) return;
    voice_state_t st = voice_get_state();
    /* Overlay is THE UI during LISTENING / PROCESSING / SPEAKING — leave it.
     * READY / IDLE / CONNECTING / RECONNECTING are "not actively interacting",
     * so hiding is safe and matches user intent when they navigate away. */
    if (st == VOICE_STATE_LISTENING
        || st == VOICE_STATE_PROCESSING
        || st == VOICE_STATE_SPEAKING) {
        ESP_LOGD(TAG, "dismiss_if_idle: skip (state=%d active)", (int)st);
        return;
    }
    ESP_LOGI(TAG, "dismiss_if_idle: hiding (state=%d)", (int)st);
    ui_voice_hide();
}

bool ui_voice_is_visible(void)
{
    return s_visible;
}

lv_obj_t *ui_voice_get_mic_btn(void)
{
    return s_mic_btn;
}

/* ================================================================
 *  Mic button (always visible on lv_layer_top)
 * ================================================================ */

static void build_mic_button(void)
{
    lv_obj_t *layer = lv_layer_top();

    s_mic_btn = lv_obj_create(layer);
    lv_obj_set_size(s_mic_btn, MIC_BTN_SZ, MIC_BTN_SZ);
    lv_obj_set_style_radius(s_mic_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mic_btn, lv_color_hex(VO_CYAN_DIM), 0);
    lv_obj_set_style_bg_opa(s_mic_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mic_btn, 1, 0);
    lv_obj_set_style_border_color(s_mic_btn, lv_color_hex(VO_CYAN_BORDER), 0);
    lv_obj_set_style_border_opa(s_mic_btn, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_mic_btn, 0, 0);
    lv_obj_clear_flag(s_mic_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mic_btn, LV_OBJ_FLAG_CLICKABLE);

    /* Position: bottom-left, above nav bar */
    lv_obj_align(s_mic_btn, LV_ALIGN_BOTTOM_LEFT, MIC_BTN_MARGIN,
                 -(int32_t)MIC_BTN_BOTTOM);

    /* Inner dot — small filled circle as mic indicator */
    s_mic_dot = lv_obj_create(s_mic_btn);
    lv_obj_set_size(s_mic_dot, MIC_DOT_SZ, MIC_DOT_SZ);
    lv_obj_set_style_radius(s_mic_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mic_dot, lv_color_hex(VO_CYAN), 0);
    lv_obj_set_style_bg_opa(s_mic_dot, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_mic_dot, 0, 0);
    lv_obj_set_style_pad_all(s_mic_dot, 0, 0);
    lv_obj_center(s_mic_dot);
    lv_obj_clear_flag(s_mic_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Click handler: short tap = Ask, long press = Dictation */
    lv_obj_add_event_cb(s_mic_btn, mic_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_mic_btn, mic_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* Pressed feedback */
    lv_obj_set_style_bg_color(s_mic_btn, lv_color_hex(VO_CYAN_GLOW), LV_PART_MAIN | LV_STATE_PRESSED);
}

/* ================================================================
 *  Full-screen overlay
 * ================================================================ */

static void build_overlay(void)
{
    lv_obj_t *layer = lv_layer_top();

    /* TT #511 wave-1.5 — in-place listening.  Voice overlay BG is now
     * FULLY TRANSPARENT so the new ui_orb on the home screen shows
     * through.  The home orb IS the orb during voice (with bloom +
     * lean + comet driven by the state machine added in step 5-7).
     * Only the text labels + buttons of this overlay render on top.
     * The overlay still has CLICKABLE so background taps don't fall
     * through to the home orb (preventing accidental re-trigger). */
    s_overlay = lv_obj_create(layer);
    if (!s_overlay) { ESP_LOGE(TAG, "OOM creating voice overlay backdrop"); return; }
    lv_obj_set_size(s_overlay, SW, SH);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Fix #6: background taps no longer cancel — only X button cancels.
     * Overlay still needs CLICKABLE to absorb taps (prevent pass-through
     * to widgets below), but no event handler attached. */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    /* Build child elements — bail out if any sub-builder fails (U17) */
    build_orb(s_overlay);
    if (!s_orb_container) { ESP_LOGE(TAG, "OOM creating voice orb"); return; }
    build_wave_bars(s_overlay);
    if (!s_wave_cont) { ESP_LOGE(TAG, "OOM creating voice wave bars"); return; }
    build_close_button(s_overlay);
    if (!s_close_btn) { ESP_LOGE(TAG, "OOM creating voice close btn"); return; }
    build_send_button(s_overlay);
    if (!s_send_btn) { ESP_LOGE(TAG, "OOM creating voice send btn"); return; }
    /* U20 (#206): build_chat_area() removed — see header note. */

    /* Status label — below orb (position adjusts per state) */
    s_lbl_status = lv_label_create(s_overlay);
    if (!s_lbl_status) { ESP_LOGE(TAG, "OOM creating voice status label"); return; }
    lv_label_set_text(s_lbl_status, "");
    lv_obj_set_style_text_font(s_lbl_status, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(VO_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(s_lbl_status, 3, 0);
    lv_obj_set_width(s_lbl_status, SW - 80);
    lv_obj_set_style_text_align(s_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 30);

    /* Transcript label — kept for backward compat / simple states */
    s_lbl_transcript = lv_label_create(s_overlay);
    if (!s_lbl_transcript) { ESP_LOGE(TAG, "OOM creating voice transcript label"); return; }
    lv_label_set_text(s_lbl_transcript, "");
    lv_obj_set_style_text_font(s_lbl_transcript, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_lbl_transcript, lv_color_hex(VO_TEXT_MID), 0);
    lv_obj_set_width(s_lbl_transcript, SW - 100);
    lv_obj_set_style_text_align(s_lbl_transcript, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_transcript, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 60);
    lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);

    /* W7-E.4c: reply-context chip at top-center.  Hidden by default;
     * ui_voice_show populates + reveals it when voice_is_channel_reply_armed
     * is true (set via voice_arm_channel_reply on the REPLY button tap). */
    s_lbl_reply_chip = lv_label_create(s_overlay);
    if (!s_lbl_reply_chip) {
       ESP_LOGE(TAG, "OOM creating voice reply chip");
       return;
    }
    lv_label_set_text(s_lbl_reply_chip, "");
    lv_obj_set_style_text_font(s_lbl_reply_chip, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_lbl_reply_chip, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_letter_space(s_lbl_reply_chip, 1, 0);
    lv_obj_set_style_bg_color(s_lbl_reply_chip, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(s_lbl_reply_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_lbl_reply_chip, 18, 0);
    lv_obj_set_style_pad_hor(s_lbl_reply_chip, 22, 0);
    lv_obj_set_style_pad_ver(s_lbl_reply_chip, 12, 0);
    lv_obj_set_style_border_width(s_lbl_reply_chip, 2, 0);
    lv_obj_set_style_border_color(s_lbl_reply_chip, lv_color_hex(TH_MODE_CLAW), 0);
    lv_obj_align(s_lbl_reply_chip, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_add_flag(s_lbl_reply_chip, LV_OBJ_FLAG_HIDDEN);

    /* Thinking dots label — centered with explicit width */
    s_lbl_dots = lv_label_create(s_overlay);
    if (!s_lbl_dots) { ESP_LOGE(TAG, "OOM creating voice dots label"); return; }
    lv_label_set_text(s_lbl_dots, "");
    lv_obj_set_style_text_font(s_lbl_dots, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_lbl_dots, lv_color_hex(VO_PURPLE_DIM), 0);
    lv_obj_set_width(s_lbl_dots, SW - 80);
    lv_obj_set_style_text_align(s_lbl_dots, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_dots, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 90);
    lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);

    /* closes #115: response text label below the orb.  Simple absolute-
     * positioned label so it's always visible during SPEAKING and for
     * the auto-hide window afterward. */
    s_response_label = lv_label_create(s_overlay);
    if (s_response_label) {
        lv_label_set_text(s_response_label, "");
        lv_label_set_long_mode(s_response_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_response_label, FONT_BODY, 0);
        lv_obj_set_style_text_color(s_response_label, lv_color_hex(VO_TEXT_BRIGHT), 0);
        lv_obj_set_style_text_align(s_response_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(s_response_label, 4, 0);
        lv_obj_set_width(s_response_label, SW - 80);
        /* Centered below the orb (orb y-center ~360 with halo out to ~515).
         * Place the label at y=580 to clear the orb on all sizes. */
        lv_obj_align(s_response_label, LV_ALIGN_TOP_MID, 0, 600);
        lv_obj_add_flag(s_response_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* TT #328 Wave 4 — state icon, sits just left of s_lbl_status.  Hidden
     * by default; each show_state_* sets the glyph + colour (LV_SYMBOL_*
     * resolves at compile time so the icon font is the same FONT_HEADING
     * the LVGL built-in icons live in). */
    s_lbl_state_icon = lv_label_create(s_overlay);
    lv_label_set_text(s_lbl_state_icon, "");
    lv_obj_set_style_text_font(s_lbl_state_icon, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s_lbl_state_icon, lv_color_hex(VO_TEXT_BRIGHT), 0);
    lv_obj_align(s_lbl_state_icon, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET - 14);
    lv_obj_add_flag(s_lbl_state_icon, LV_OBJ_FLAG_HIDDEN);

    /* v5 sub-caption: 'LISTENING • HOLD TO TALK' / 'DICTATION • TAP TO STOP'.
     * Appears below the main status, letter-spaced amber for v5 feel. */
    s_lbl_sub = lv_label_create(s_overlay);
    lv_label_set_text(s_lbl_sub, "");
    lv_obj_set_style_text_font(s_lbl_sub, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_lbl_sub, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_text_letter_space(s_lbl_sub, 4, 0);
    lv_obj_set_width(s_lbl_sub, SW - 80);
    lv_obj_set_style_text_align(s_lbl_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_sub, LV_ALIGN_CENTER, 0,
                 ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 85);
    lv_obj_add_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);

    /* Recording duration label — shown during LISTENING below status, centered */
    s_lbl_rec_time = lv_label_create(s_overlay);
    if (!s_lbl_rec_time) { ESP_LOGE(TAG, "OOM creating voice rec time label"); return; }
    lv_label_set_text(s_lbl_rec_time, "0:00");
    lv_obj_set_style_text_font(s_lbl_rec_time, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(s_lbl_rec_time, lv_color_hex(VO_TEXT_FAINT), 0);
    lv_obj_set_width(s_lbl_rec_time, SW - 80);
    lv_obj_set_style_text_align(s_lbl_rec_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_rec_time, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 60);
    lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);

    /* Start hidden */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ── Orb — the soul of Tinker ─────────────────────────────────── */

static void build_orb(lv_obj_t *parent)
{
    /* Invisible container for logical grouping (moved up to make room for chat) */
    s_orb_container = lv_obj_create(parent);
    if (!s_orb_container) { ESP_LOGE(TAG, "OOM creating orb container"); return; }
    /* #113: make the container big enough for home-parity halo rings
     * (outer = 310 px).  Was ORB_SZ_SPEAK+20=340 which only fit the
     * 300/320 glow+ring stack.  Now 360 px so the 310 halo nests
     * inside with room for the centered position. */
    lv_obj_set_size(s_orb_container, 360, 360);
    lv_obj_align(s_orb_container, LV_ALIGN_CENTER, 0, ORB_Y_OFFSET);
    lv_obj_set_style_bg_opa(s_orb_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_orb_container, 0, 0);
    lv_obj_set_style_pad_all(s_orb_container, 0, 0);
    lv_obj_clear_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* TT #511 wave-1.5 — in-place listening.  Skip the entire visual
     * composite below (4 glow layers + ring + 3 halos = 8 widgets).
     * The home orb (ui_orb on the home screen) is THE orb now; this
     * container exists only as a click-target placeholder so state-
     * machine code that adds CLICKABLE + event handlers to it (READY
     * tap-to-listen, SPEAKING tap-to-cancel) still has a valid obj
     * to attach to.  Earlier attempts:
     *   1. Setting LV_PART_MAIN opa to 0 on the container — children
     *      have their own bg_opa / border_opa so they rendered anyway.
     *   2. Adding LV_OBJ_FLAG_HIDDEN to the container — confirmed not
     *      sufficient to suppress the 8 children at draw time on this
     *      LVGL 9.2.2 build (rings still rendered).
     * The reliable fix is to NEVER create the visual children.
     * Container's own bg_opa = TRANSP (set above) so it's an invisible
     * 360 × 360 click box. */
    return;
#if 0  /* TT #511 wave-1.5 — dead-coded; ui_orb provides the orb visual. */
    int32_t base_sz = ORB_SZ_LISTEN;
    for (int i = ORB_GLOW_LAYERS - 1; i >= 0; i--) {
        /* Outermost layer is largest, innermost is smallest */
        float frac = (float)(ORB_GLOW_LAYERS - i) / (float)ORB_GLOW_LAYERS;
        int32_t sz = (int32_t)(base_sz * (0.4f + 0.6f * frac));
        /* Opacity: innermost ~38 (15%), outermost ~5 */
        lv_opa_t opa = (lv_opa_t)(10 + (int)(28.0f * frac));

        s_orb_glow[i] = lv_obj_create(s_orb_container);
        if (!s_orb_glow[i]) { ESP_LOGE(TAG, "OOM creating orb glow layer %d", i); return; }
        lv_obj_set_size(s_orb_glow[i], sz, sz);
        lv_obj_center(s_orb_glow[i]);
        lv_obj_set_style_radius(s_orb_glow[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_orb_glow[i], lv_color_hex(VO_CYAN), 0);
        lv_obj_set_style_bg_opa(s_orb_glow[i], opa, 0);
        lv_obj_set_style_border_width(s_orb_glow[i], 0, 0);
        lv_obj_set_style_pad_all(s_orb_glow[i], 0, 0);
        lv_obj_clear_flag(s_orb_glow[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Outer ring — thin stroke, breathing animation target */
    s_orb_ring = lv_obj_create(s_orb_container);
    if (!s_orb_ring) { ESP_LOGE(TAG, "OOM creating orb ring"); return; }
    lv_obj_set_size(s_orb_ring, ORB_SZ_LISTEN, ORB_SZ_LISTEN);
    lv_obj_center(s_orb_ring);
    lv_obj_set_style_radius(s_orb_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_orb_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_orb_ring, ORB_RING_W, 0);
    lv_obj_set_style_border_color(s_orb_ring, lv_color_hex(VO_CYAN), 0);
    lv_obj_set_style_border_opa(s_orb_ring, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_orb_ring, 0, 0);
    lv_obj_clear_flag(s_orb_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* closes #113: three concentric border-only halo rings that mirror
     * home's v4 Ambient orb (RING_OUTER=310, RING_MID=240, RING_INNER=196
     * from ui_home.c).  Low-intensity amber strokes that make the overlay
     * orb feel like a continuation of home, not a visual downgrade.
     * Each one is NULL-guarded — under tight LVGL pool the orb still
     * works without the decoration.
     *
     * Order matters: added AFTER the ring so they sit on top in z-order,
     * but none have bg fill (border-only) so they layer like home's. */
    struct { lv_obj_t **ptr; int sz; lv_opa_t opa; } halos[] = {
        { &s_orb_halo_outer, 310,  60 },
        { &s_orb_halo_mid,   240,  90 },
        { &s_orb_halo_inner, 196, 140 },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(s_orb_container);
        if (!r) {
            ESP_LOGW(TAG, "OOM creating halo ring %d — skipping", i);
            continue;
        }
        *halos[i].ptr = r;
        lv_obj_set_size(r, halos[i].sz, halos[i].sz);
        lv_obj_center(r);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(VO_CYAN), 0);
        lv_obj_set_style_border_opa(r, halos[i].opa, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        /* Halos render BEHIND the orb filling (glow layers) by
         * being drawn first.  Move them to background explicitly
         * in case LVGL's z-order derived from sibling order differs. */
        lv_obj_move_background(r);
    }
#endif /* TT #511 wave-1.5 — dead-coded section above. */
}

/* ── Wave bars — speaking visualization ───────────────────────── */

static void build_wave_bars(lv_obj_t *parent)
{
    int32_t total_w = WAVE_BARS * WAVE_BAR_W + (WAVE_BARS - 1) * WAVE_BAR_GAP;

    s_wave_cont = lv_obj_create(parent);
    if (!s_wave_cont) { ESP_LOGE(TAG, "OOM creating wave container"); return; }
    lv_obj_set_size(s_wave_cont, total_w, WAVE_BAR_MAX_H);
    lv_obj_align(s_wave_cont, LV_ALIGN_CENTER, 0, ORB_SZ_SPEAK / 2 + ORB_Y_OFFSET + 20);
    lv_obj_set_style_bg_opa(s_wave_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wave_cont, 0, 0);
    lv_obj_set_style_pad_all(s_wave_cont, 0, 0);
    lv_obj_set_style_layout(s_wave_cont, LV_LAYOUT_FLEX, 0);
    lv_obj_set_style_flex_flow(s_wave_cont, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(s_wave_cont, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_main_place(s_wave_cont, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_column(s_wave_cont, WAVE_BAR_GAP, 0);
    lv_obj_clear_flag(s_wave_cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < WAVE_BARS; i++) {
        s_wave_bars[i] = lv_obj_create(s_wave_cont);
        if (!s_wave_bars[i]) { ESP_LOGE(TAG, "OOM creating wave bar %d", i); return; }
        lv_obj_set_size(s_wave_bars[i], WAVE_BAR_W, WAVE_BAR_MIN_H);
        lv_obj_set_style_radius(s_wave_bars[i], WAVE_BAR_W / 2, 0);
        lv_obj_set_style_bg_color(s_wave_bars[i], lv_color_hex(VO_GREEN), 0);
        lv_obj_set_style_bg_opa(s_wave_bars[i], LV_OPA_40, 0);
        lv_obj_set_style_border_width(s_wave_bars[i], 0, 0);
        lv_obj_set_style_pad_all(s_wave_bars[i], 0, 0);
        lv_obj_clear_flag(s_wave_bars[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_add_flag(s_wave_cont, LV_OBJ_FLAG_HIDDEN);
}

/* ── Close button ─────────────────────────────────────────────── */

static void build_close_button(lv_obj_t *parent)
{
    s_close_btn = lv_obj_create(parent);
    if (!s_close_btn) { ESP_LOGE(TAG, "OOM creating close button"); return; }
    lv_obj_set_size(s_close_btn, CLOSE_BTN_SZ, CLOSE_BTN_SZ);
    lv_obj_align(s_close_btn, LV_ALIGN_TOP_RIGHT, -(int32_t)CLOSE_BTN_MARGIN_X, CLOSE_BTN_MARGIN_Y);
    lv_obj_set_style_radius(s_close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_close_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_close_btn, 0, 0);
    lv_obj_set_style_pad_all(s_close_btn, 0, 0);
    lv_obj_add_flag(s_close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(s_close_btn, 12);  /* extend tap target by 12px each side */

    lv_obj_t *x_lbl = lv_label_create(s_close_btn);
    if (!x_lbl) { ESP_LOGE(TAG, "OOM creating close label"); return; }
    lv_label_set_text(x_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x_lbl, lv_color_hex(VO_CLOSE_TEXT), 0);
    lv_obj_set_style_text_font(x_lbl, FONT_HEADING, 0);
    lv_obj_center(x_lbl);

    /* Pressed feedback */
    lv_obj_set_style_bg_opa(s_close_btn, LV_OPA_10, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_close_btn, lv_color_hex(VO_TEXT_BRIGHT), LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_add_event_cb(s_close_btn, close_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Send/Stop button — visible during LISTENING ──────────────── */

static void build_send_button(lv_obj_t *parent)
{
    s_send_btn = lv_obj_create(parent);
    if (!s_send_btn) { ESP_LOGE(TAG, "OOM creating send button"); return; }
    lv_obj_set_size(s_send_btn, SEND_BTN_SZ, SEND_BTN_SZ);
    lv_obj_set_style_radius(s_send_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_send_btn, lv_color_hex(0xFF5252), 0); /* coral-red */
    lv_obj_set_style_bg_opa(s_send_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_send_btn, 2, 0);
    lv_obj_set_style_border_color(s_send_btn, lv_color_hex(0xFF8A80), 0);
    lv_obj_set_style_border_opa(s_send_btn, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_send_btn, 0, 0);
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_send_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_send_btn, (SW - SEND_BTN_SZ) / 2, SEND_BTN_Y - SEND_BTN_SZ / 2);

    /* Inner square "stop" icon */
    lv_obj_t *stop_icon = lv_obj_create(s_send_btn);
    if (!stop_icon) { ESP_LOGE(TAG, "OOM creating stop icon"); return; }
    lv_obj_set_size(stop_icon, SEND_ICON_SZ, SEND_ICON_SZ);
    lv_obj_set_style_radius(stop_icon, 4, 0);
    lv_obj_set_style_bg_color(stop_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(stop_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stop_icon, 0, 0);
    lv_obj_set_style_pad_all(stop_icon, 0, 0);
    lv_obj_center(stop_icon);
    lv_obj_clear_flag(stop_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Pressed feedback */
    lv_obj_set_style_bg_color(s_send_btn, lv_color_hex(0xD32F2F),
                              LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_add_event_cb(s_send_btn, send_click_cb, LV_EVENT_CLICKED, NULL);

    /* Start hidden — only shown during LISTENING */
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
}

/* U20 (#206): create_bubble + build_chat_area removed.  v5 routed all
 * conversation to Chat (ui_chat); the voice overlay's offscreen flex
 * tree (~30 LV objects) was kept "in case" and was never reactivated.
 * Visible response text comes from s_response_label, set by the
 * READY-state handler from voice_get_llm_text(). */

/* ================================================================
 *  State visuals
 * ================================================================ */

/* TT #328 Wave 4 — set the per-state icon glyph + colour + visibility.
 * Pass NULL or empty string to hide.  Colour is hex only (callers don't
 * need to think about lv_color_t). */
static void set_state_icon(const char *glyph, uint32_t color_hex) {
   if (!s_lbl_state_icon) return;
   if (!glyph || !glyph[0]) {
      lv_obj_add_flag(s_lbl_state_icon, LV_OBJ_FLAG_HIDDEN);
      return;
   }
   lv_label_set_text(s_lbl_state_icon, glyph);
   lv_obj_set_style_text_color(s_lbl_state_icon, lv_color_hex(color_hex), 0);
   /* TT #511 wave-1.7: the new ui_orb state machine already conveys
    * state via comet (PROCESSING) / bloom (LISTENING) / steady halo
    * (SPEAKING) / circadian (IDLE).  Showing a separate purple
    * refresh icon + music-note + check glyph on top of the orb is
    * redundant.  Keep the obj for compat (state handlers still set
    * its text); force HIDDEN at the un-hide site so it never
    * renders. */
   lv_obj_add_flag(s_lbl_state_icon, LV_OBJ_FLAG_HIDDEN);
   (void)glyph;
}

static void show_state_listening(void)
{
    stop_all_anims();
    /* TT #328 Wave 4: distinct mic icon for LISTENING — green hue (matches
     * "we're capturing audio") so colorblind users distinguish from the
     * amber-family PROCESSING / SPEAKING. */
    set_state_icon(LV_SYMBOL_AUDIO, TH_MODE_LOCAL);

    /* PR 2 polish: when the dictation pipeline is on-flight, the orb is
     * already painted with pipeline visuals (red body, hero caption,
     * Dictate chip-as-cancel).  Suppress the voice overlay's "I'm here.
     * Go." status text + LISTENING sub-caption + red stop button — they
     * overlap the home-resident Dictate chip and double up the messaging.
     * The pipeline + chip own this surface during dictation. */
    if (ui_orb_pipeline_active()) {
       if (s_lbl_status) lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
       if (s_lbl_sub) lv_obj_add_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);
       if (s_send_btn) lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
       if (s_lbl_transcript) lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);
       if (s_lbl_dots) lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);
       if (s_wave_cont) lv_obj_add_flag(s_wave_cont, LV_OBJ_FLAG_HIDDEN);
       if (s_lbl_rec_time) lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
       return;
    }

    /* Clean up READY-state orb click handler if still attached */
    lv_obj_clear_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb(s_orb_container, orb_ready_click_cb);

    /* #115: clear + hide the previous turn's response label. */
    if (s_response_label) {
        lv_label_set_text(s_response_label, "");
        lv_obj_add_flag(s_response_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* Orb: cyan, 200px, breathing */
    set_orb_color(VO_CYAN, VO_CYAN, LV_OPA_60);
    set_orb_size(ORB_SZ_LISTEN);

    /* Spec shot-05: "I'm here. Go." with serif-style display copy.
     * Dictation uses "Dictating." with the same weight. */
    if (voice_get_mode() == VOICE_MODE_DICTATE) {
        lv_label_set_text(s_lbl_status, "Dictating.");
        if (s_lbl_sub) {
            lv_label_set_text(s_lbl_sub, "DICTATION  \xe2\x80\xa2  TAP TO STOP");
            lv_obj_clear_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_label_set_text(s_lbl_status, "I'm here. Go.");
        if (s_lbl_sub) {
           /* TT #511 wave-1.9 (smoothness fix #2): copy was
            * "LISTENING • RELEASE TO SEND" — but the gesture is
            * tap-once, not hold-to-talk (the latter was retired
            * years ago).  "RELEASE TO SEND" was a lie that
            * confused users about what gesture was active.  New
            * copy matches the actual affordance: tap-stop or
            * wait it out. */
           lv_label_set_text(s_lbl_sub, "LISTENING  \xe2\x80\xa2  TAP STOP TO SEND");
           lv_obj_clear_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(VO_TEXT_BRIGHT), 0);
    lv_obj_set_style_text_letter_space(s_lbl_status, 4, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 30);

    /* Hide transcript, dots, wave (chat tree removed in U20). */
    lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wave_cont, LV_OBJ_FLAG_HIDDEN);
    s_has_llm_text = false;

    /* Show send/stop button so user knows how to stop recording */
    lv_obj_clear_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);

    /* TT #511 wave-1.9 (smoothness fix #1): hide the countdown clock
     * in Ask mode.  The 30 s (now 60 s — see voice.c MAX_RECORD_FRAMES_ASK)
     * cap is a silent safety net, not something the user needs to
     * watch tick down.  The red panic-color at 5 s was actively
     * stressful for a voice assistant.  Dictation mode still shows
     * elapsed time (long-form recording flow where duration matters).
     * Timer + label obj kept alive so dictation works; just don't
     * show the label in Ask. */
    s_rec_seconds = 0;
    if (voice_get_mode() == VOICE_MODE_ASK) {
       lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_lbl_rec_time, "0:00");
        lv_obj_set_style_text_font(s_lbl_rec_time, FONT_HEADING, 0);
        lv_obj_set_style_text_color(s_lbl_rec_time, lv_color_hex(VO_CYAN), 0);
        lv_obj_align(s_lbl_rec_time, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 60);
        lv_obj_clear_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
    }
    s_rec_timer = lv_timer_create(rec_timer_cb, 1000, NULL);

    /* Show send/stop button */
    lv_obj_clear_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);

    start_breathe_anim();
    /* W15-P02: overlay the breathe anim's innermost layer with live
     * RMS so the orb visibly reacts to the user's voice. */
    start_rms_anim();
}

/* v4·D Gauntlet G1: render the "+N QUEUED" badge on the sub-caption slot
 * whenever voice.c has a pending text stashed.  Kept in one helper so
 * PROCESSING and SPEAKING both re-evaluate without duplicating code. */
static void render_queue_badge(void)
{
   /* TT #511 wave-1.7: "+N QUEUED" caption suppressed.  Confusing
    * noise alongside the clean status line + orb motion; the queued
    * turn is still processed, just not announced.  If queue depth
    * matters in a future flow, surface it via a toast or status-bar
    * indicator instead of stacking text under the orb. */
   if (s_lbl_sub) lv_obj_add_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);
   return;
#if 0  /* dead-coded; preserved for cherry-pick reference */
    if (!s_lbl_sub) return;
    int depth = voice_get_queue_depth();
    if (depth > 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "+%d QUEUED", depth);
        lv_label_set_text(s_lbl_sub, buf);
        lv_obj_set_style_text_color(s_lbl_sub, lv_color_hex(0xA78BFA), 0);
        lv_obj_clear_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);
    }
#endif /* dead-coded — see TT #511 wave-1.7 comment */
}

static void show_state_processing(const char *detail)
{
   /* TT #328 Wave 4: refresh icon for PROCESSING — violet hue so
    * "thinking" reads visually distinct from listening (green) and
    * speaking (blue). */
   set_state_icon(LV_SYMBOL_REFRESH, 0xA78BFA);
   render_queue_badge();

   /* PR 2 polish: pipeline UPLOADING / TRANSCRIBING own the surface.
    * Suppress overlay chrome so the hero caption + Dictate chip read
    * cleanly without overlap. */
   if (ui_orb_pipeline_active()) {
      if (s_lbl_status) lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
      if (s_lbl_sub) lv_obj_add_flag(s_lbl_sub, LV_OBJ_FLAG_HIDDEN);
      if (s_send_btn) lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
      if (s_lbl_transcript) lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);
      if (s_lbl_dots) lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);
      if (s_lbl_rec_time) lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
      if (s_response_label) lv_obj_add_flag(s_response_label, LV_OBJ_FLAG_HIDDEN);
      return;
   }
   /* Note: this is called repeatedly as LLM tokens arrive.
    * detail = STT text (first call), then LLM text (subsequent calls).
    * We use voice_get_stt_text() and voice_get_llm_text() to distinguish. */
   const char *stt = voice_get_stt_text();
   const char *llm = voice_get_llm_text();

   /* Only stop/restart anims on first entry to PROCESSING */
   if (s_cur_state != VOICE_STATE_PROCESSING || !s_has_llm_text) {
      /* First time or STT just arrived */
   }

    /* Hide listening-only elements */
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);

    /* Orb: purple tint, faster pulse (only restart if not already pulsing) */
    if (!s_has_llm_text) {
        stop_all_anims();
        set_orb_color(VO_PURPLE, VO_PURPLE, LV_OPA_50);
        /* #114: PROCESSING shrinks the orb so the state-change is
         * visible at a glance even without the label text changing. */
        set_orb_size(ORB_SZ_PROCESS);
        start_pulse_anim();
    }

    /* U20 (#206): user/AI bubble setting removed (offscreen tree was
     * dead).  STT text just stays in voice's status string; LLM tokens
     * drive the visible s_response_label. */
    (void)stt;

    /* If LLM tokens are streaming, show response */
    if (llm && llm[0]) {
        if (!s_has_llm_text) {
            /* First LLM token — switch orb to green, hide dots */
            s_has_llm_text = true;
            stop_all_anims();
            set_orb_color(VO_GREEN, VO_GREEN, LV_OPA_50);
            start_breathe_anim();

            /* Hide thinking dots */
            if (s_dot_timer) {
                lv_timer_delete(s_dot_timer);
                s_dot_timer = NULL;
            }
            lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);

            /* Update status */
            lv_label_set_text(s_lbl_status, "");
            lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
        }

        /* #115: drive the fixed-position response label.
         * #78 + #160: scrub tool markers first so streamed <tool>...
         * never flashes in the caption. */
        if (s_response_label) {
            { char _m[256]; md_strip_tool_markers(llm, _m, sizeof(_m));
              md_strip_inline_with_ellipsis(_m, _m, sizeof(_m));
              lv_label_set_text(s_response_label, _m); }
            lv_obj_clear_flag(s_response_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_response_label);
        }
    } else {
        /* STT arrived but no LLM yet — show thinking status.
         * TinkerClaw mode: "Agent thinking..." to signal agent reasoning */
        uint8_t vmode = tab5_settings_get_voice_mode();
        /* TT #511 wave-1.7: dropped the trailing period and the
         * separate dots-animation; the comet orbit is the motion
         * cue, the text is just the noun. */
        lv_label_set_text(s_lbl_status, vmode == VOICE_MODE_TINKERCLAW ? "Agent thinking" : "Thinking");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(VO_TEXT_MID), 0);
        lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_28, 0);
        lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0,
                     ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 30);
        lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);

        /* Show animated dots */
        if (!s_dot_timer) {
            s_dot_phase = 0;
            lv_label_set_text(s_lbl_dots, "   ");
            lv_obj_set_style_text_color(s_lbl_dots, lv_color_hex(VO_PURPLE), 0);
            lv_obj_align(s_lbl_dots, LV_ALIGN_CENTER, 0,
                         ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 60);
            /* TT #511 wave-1.7: animated dots redundant with the new
             * ui_orb's skill-rim comet (the orbit itself is the
             * "thinking" visual).  Keep obj + timer for compat;
             * force HIDDEN so it never renders. */
            lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);
            s_dot_timer = lv_timer_create(dot_timer_cb, ANIM_DOT_MS, NULL);
        }
    }

    /* Hide wave */
    lv_obj_add_flag(s_wave_cont, LV_OBJ_FLAG_HIDDEN);
}

static void show_state_speaking(void)
{
    stop_all_anims();
    /* TT #328 Wave 4: speaker icon for SPEAKING — blue (TH_MODE_CLOUD)
     * so it reads as "audio-out" and is distinguishable from the
     * listening-green and processing-violet glyphs. */
    set_state_icon(LV_SYMBOL_VOLUME_MAX, TH_MODE_CLOUD);

    /* Hide listening-only elements */
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);
    /* G1: queue badge keeps rendering through SPEAKING too. */
    render_queue_badge();

    /* Orb: green, slightly larger */
    set_orb_color(VO_GREEN, VO_GREEN, LV_OPA_50);
    set_orb_size(ORB_SZ_SPEAK);

    /* Make orb tappable to interrupt TTS.
     * Remove first to avoid stacking duplicate callbacks on re-entry. */
    lv_obj_add_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb(s_orb_container, orb_speak_click_cb);
    lv_obj_add_event_cb(s_orb_container, orb_speak_click_cb, LV_EVENT_CLICKED, NULL);

    /* Hide status label and dots — chat bubbles show the content now */
    lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);

    /* Keep chat area visible with both bubbles */
    const char *llm = voice_get_llm_text();
    /* U20 (#206): bubble paths removed.  STT is reflected by the
     * status string from voice.c; LLM tokens drive s_response_label.
     * #78 + #160: scrub tool markers before the markdown pass. */
    if (llm && llm[0] && s_response_label) {
        char _m[256];
        md_strip_tool_markers(llm, _m, sizeof(_m));
        md_strip_inline_with_ellipsis(_m, _m, sizeof(_m));
        lv_label_set_text(s_response_label, _m);
        lv_obj_clear_flag(s_response_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_response_label);
    }

    /* Show wave bars below orb */
    lv_obj_align(s_wave_cont, LV_ALIGN_CENTER, 0, ORB_SZ_SPEAK / 2 + ORB_Y_OFFSET + 20);
    lv_obj_clear_flag(s_wave_cont, LV_OBJ_FLAG_HIDDEN);

    start_breathe_anim();
    start_wave_anim();
}

static void show_state_idle(void)
{
    stop_all_anims();

    /* Clean up listening/speaking/ready elements (chat tree removed in U20). */
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
    s_has_llm_text = false;
    lv_obj_clear_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb(s_orb_container, orb_speak_click_cb);
    lv_obj_remove_event_cb(s_orb_container, orb_ready_click_cb);

    if (s_visible) {
        ui_voice_hide();
    }
}

/* ================================================================
 *  Orb helpers
 * ================================================================ */

static void set_orb_color(uint32_t ring_hex, uint32_t glow_hex, lv_opa_t ring_opa)
{
   /* TT #511 wave-1.5: orb widgets are dead-coded by build_orb (the
    * home ui_orb provides the visual now).  Every pointer here is
    * NULL; guard the writes so state handlers can keep calling this
    * harmlessly. */
   if (s_orb_ring) {
      lv_obj_set_style_border_color(s_orb_ring, lv_color_hex(ring_hex), 0);
      lv_obj_set_style_border_opa(s_orb_ring, ring_opa, 0);
   }

    for (int i = 0; i < ORB_GLOW_LAYERS; i++) {
       if (s_orb_glow[i]) {
          lv_obj_set_style_bg_color(s_orb_glow[i], lv_color_hex(glow_hex), 0);
       }
    }
}

/* Phase 3 of #42: spring-driven orb size transitions.
 *
 * Pre-fix the orb's diameter snapped instantly between LISTEN/PROCESS/
 * SPEAK sizes (300 / 240 / 340 px) on every state change.  Visible
 * artefact: rapid PROCESSING → SPEAKING transitions (LLM cold-start
 * then immediate token emit) made the orb visibly jump 100 px in one
 * frame.
 *
 * Post-fix: set_orb_size retargets a SPRING_SMOOTH-driven anim that
 * carries velocity across mid-flight retargets — feels physical
 * instead of teleporty.  SMOOTH (zeta = 1.0, critically damped) is
 * the right preset for this slot: no overshoot (orb shouldn't
 * jiggle), ~600 ms convergence on a 100 px swing.
 *
 * State invariants:
 *   s_orb_size_current is the live displayed size (= s_orb_spring.pos)
 *   s_orb_anim_t is non-NULL exactly while the spring is animating
 *   First-ever set_orb_size call snaps without animating (no velocity
 *   to carry, and instant "at-rest" first paint matches the existing
 *   visual contract on screen-create) */
static spring_anim_t s_orb_spring;
static lv_timer_t *s_orb_anim_t = NULL;
static float s_orb_size_current = 0.0f;

static void apply_orb_size_now(int32_t sz) {
   if (!s_orb_ring) return;
   lv_obj_set_size(s_orb_ring, sz, sz);
   lv_obj_center(s_orb_ring);

   for (int i = ORB_GLOW_LAYERS - 1; i >= 0; i--) {
      float frac = (float)(ORB_GLOW_LAYERS - i) / (float)ORB_GLOW_LAYERS;
      int32_t layer_sz = (int32_t)(sz * (0.4f + 0.6f * frac));
      lv_obj_set_size(s_orb_glow[i], layer_sz, layer_sz);
      lv_obj_center(s_orb_glow[i]);
   }
}

static void orb_size_anim_cb(lv_timer_t *t) {
   if (!s_orb_ring) {
      s_orb_anim_t = NULL;
      lv_timer_delete(t);
      return;
   }
   float sz = spring_anim_update(&s_orb_spring, 1.0f / 60.0f);
   s_orb_size_current = sz;
   apply_orb_size_now((int32_t)(sz + 0.5f));
   if (spring_anim_done(&s_orb_spring)) {
      s_orb_anim_t = NULL;
      lv_timer_delete(t);
   }
}

static void set_orb_size(int32_t sz) {
   if (s_orb_size_current <= 0.0f) {
      /* First call: snap without animating.  No velocity to carry,
       * and the first paint should be at-rest at the requested size
       * (matches the existing visual contract on overlay create). */
      spring_anim_init(&s_orb_spring, SPRING_SMOOTH);
      s_orb_size_current = (float)sz;
      apply_orb_size_now(sz);
      return;
   }
   if ((int32_t)(s_orb_size_current + 0.5f) == sz && spring_anim_done(&s_orb_spring)) {
      /* Already at target with no in-flight motion → no-op. */
      return;
   }
   /* Retarget mid-flight if still animating; carries the spring's
    * current velocity so back-to-back state changes feel physical. */
   spring_anim_retarget(&s_orb_spring, s_orb_size_current, (float)sz, s_orb_spring.velocity);
   if (!s_orb_anim_t) {
      s_orb_anim_t = lv_timer_create(orb_size_anim_cb, 16, NULL);
   }
}

static void update_mic_button_state(voice_state_t state)
{
    /* Stop any existing mic dot pulse animation */
    lv_anim_delete(s_mic_dot, mic_dot_pulse_cb);

    switch (state) {
    case VOICE_STATE_IDLE:
    case VOICE_STATE_RECONNECTING:
    case VOICE_STATE_READY:
        /* Default: small cyan dot, static */
        lv_obj_set_size(s_mic_dot, MIC_DOT_SZ, MIC_DOT_SZ);
        lv_obj_set_style_radius(s_mic_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_mic_dot, lv_color_hex(VO_CYAN), 0);
        lv_obj_set_style_bg_opa(s_mic_dot, LV_OPA_60, 0);
        lv_obj_center(s_mic_dot);
        break;
    case VOICE_STATE_CONNECTING:
    case VOICE_STATE_LISTENING: {
        /* Pulsing red dot — recording indicator (Fix #2) */
        lv_obj_set_size(s_mic_dot, MIC_DOT_SZ + 4, MIC_DOT_SZ + 4);
        lv_obj_set_style_bg_color(s_mic_dot, lv_color_hex(0xFF5252), 0);
        lv_obj_set_style_bg_opa(s_mic_dot, LV_OPA_COVER, 0);
        lv_obj_center(s_mic_dot);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_mic_dot);
        lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
        lv_anim_set_duration(&a, MIC_DOT_PULSE_MS);
        lv_anim_set_playback_duration(&a, MIC_DOT_PULSE_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, mic_dot_pulse_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
        break;
    }
    case VOICE_STATE_PROCESSING: {
        /* Pulsing purple dot (Fix #2) */
        lv_obj_set_size(s_mic_dot, MIC_DOT_SZ, MIC_DOT_SZ);
        lv_obj_set_style_bg_color(s_mic_dot, lv_color_hex(VO_PURPLE), 0);
        lv_obj_set_style_bg_opa(s_mic_dot, LV_OPA_70, 0);
        lv_obj_center(s_mic_dot);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_mic_dot);
        lv_anim_set_values(&a, LV_OPA_30, LV_OPA_80);
        lv_anim_set_duration(&a, MIC_DOT_PULSE_MS);
        lv_anim_set_playback_duration(&a, MIC_DOT_PULSE_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, mic_dot_pulse_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
        break;
    }
    case VOICE_STATE_SPEAKING: {
        /* Pulsing green dot (Fix #2) */
        lv_obj_set_size(s_mic_dot, MIC_DOT_SZ, MIC_DOT_SZ);
        lv_obj_set_style_bg_color(s_mic_dot, lv_color_hex(VO_GREEN), 0);
        lv_obj_set_style_bg_opa(s_mic_dot, LV_OPA_70, 0);
        lv_obj_center(s_mic_dot);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_mic_dot);
        lv_anim_set_values(&a, LV_OPA_30, LV_OPA_80);
        lv_anim_set_duration(&a, MIC_DOT_PULSE_MS);
        lv_anim_set_playback_duration(&a, MIC_DOT_PULSE_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, mic_dot_pulse_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
        break;
    }
    }
}

/* ================================================================
 *  Animations
 * ================================================================ */

static void start_breathe_anim(void)
{
    /* Orb ring opacity: breathing 30 -> 80 -> 30 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_orb_ring);
    lv_anim_set_values(&a, 30, 80);
    lv_anim_set_duration(&a, ANIM_BREATHE_MS);
    lv_anim_set_playback_duration(&a, ANIM_BREATHE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, orb_ring_opa_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    /* Glow layers: subtle scale breathing via opacity shift */
    for (int i = 0; i < ORB_GLOW_LAYERS; i++) {
        float frac = (float)(ORB_GLOW_LAYERS - i) / (float)ORB_GLOW_LAYERS;
        int32_t base_opa = (int32_t)(10 + 28.0f * frac);
        int32_t peak_opa = (int32_t)(base_opa * 1.5f);
        if (peak_opa > 80) peak_opa = 80;

        lv_anim_t ga;
        lv_anim_init(&ga);
        lv_anim_set_var(&ga, s_orb_glow[i]);
        lv_anim_set_values(&ga, base_opa, peak_opa);
        lv_anim_set_duration(&ga, ANIM_BREATHE_MS);
        lv_anim_set_playback_duration(&ga, ANIM_BREATHE_MS);
        lv_anim_set_repeat_count(&ga, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&ga, orb_breathe_cb);
        lv_anim_set_path_cb(&ga, lv_anim_path_ease_in_out);
        /* Stagger slightly for organic feel */
        lv_anim_set_delay(&ga, i * 80);
        lv_anim_start(&ga);
    }
}

static void start_pulse_anim(void)
{
    /* Faster pulse for processing state */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_orb_ring);
    lv_anim_set_values(&a, 20, 90);
    lv_anim_set_duration(&a, ANIM_PULSE_MS);
    lv_anim_set_playback_duration(&a, ANIM_PULSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, orb_ring_opa_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    /* Glow layers pulse faster too */
    for (int i = 0; i < ORB_GLOW_LAYERS; i++) {
        float frac = (float)(ORB_GLOW_LAYERS - i) / (float)ORB_GLOW_LAYERS;
        int32_t base_opa = (int32_t)(8 + 20.0f * frac);
        int32_t peak_opa = (int32_t)(base_opa * 2.0f);
        if (peak_opa > 100) peak_opa = 100;

        lv_anim_t ga;
        lv_anim_init(&ga);
        lv_anim_set_var(&ga, s_orb_glow[i]);
        lv_anim_set_values(&ga, base_opa, peak_opa);
        lv_anim_set_duration(&ga, ANIM_PULSE_MS);
        lv_anim_set_playback_duration(&ga, ANIM_PULSE_MS);
        lv_anim_set_repeat_count(&ga, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&ga, orb_breathe_cb);
        lv_anim_set_path_cb(&ga, lv_anim_path_ease_in_out);
        lv_anim_set_delay(&ga, i * 50);
        lv_anim_start(&ga);
    }
}

/* ================================================================
 * W15-P02: live RMS → inner-glow opacity.
 * Makes the orb visibly "listen back" to the user's voice during
 * LISTENING.  Without this, the overlay feels static/unresponsive —
 * all the user got was the breathe animation, which doesn't track
 * what they're saying.  voice.c computes s_current_rms every mic
 * frame (int16 PCM magnitude); typical values:
 *    ambient quiet room : ~200-400
 *    normal speech      : ~1500-3000
 *    loud / shout       : 6000+
 * Map RMS → opacity so the innermost (hottest) glow layer modulates
 * with voice energy.  Keep the other 3 layers on the existing breathe
 * anim so the orb still feels alive even in silence.
 * ================================================================ */
static void rms_tick_cb(lv_timer_t *t)
{
    (void)t;
    /* Only run when the overlay is visible and in LISTENING; the
     * LVGL timer system lives on the UI task, so we're serialized
     * with state transitions. */
    if (!s_visible) return;
    if (s_cur_state != VOICE_STATE_LISTENING
     && s_cur_state != VOICE_STATE_CONNECTING) return;

    float rms = voice_get_current_rms();
    /* Floor / ceiling clamp.  Below 200 = ambient noise, ignore.
     * Above 3000 = already loud, cap.  30-100 opa range so the
     * inner glow is always at least dimly visible (never goes dark). */
    int32_t opa = 30;
    if (rms > 200.0f) {
        float span = rms - 200.0f;
        if (span > 2800.0f) span = 2800.0f;
        opa = 30 + (int32_t)(70.0f * (span / 2800.0f));
    }
    if (opa < 30) opa = 30;
    if (opa > 100) opa = 100;

    /* Apply to the innermost (hottest) glow layer only.  The set_orb_size
     * loop uses i=ORB_GLOW_LAYERS-1 for the SMALLEST layer (frac=1/N),
     * which is the bright core of the orb — exactly the one the eye
     * reads as "intensity". */
    lv_obj_t *core = s_orb_glow[ORB_GLOW_LAYERS - 1];
    if (core) {
        lv_obj_set_style_bg_opa(core, (lv_opa_t)opa, 0);
    }
}

static void start_rms_anim(void)
{
    /* Remove the breathe-anim that would otherwise keep overwriting
     * the innermost layer's opa every frame.  The other 3 layers
     * keep breathing — they provide the "ambient alive" layer. */
    lv_anim_delete(s_orb_glow[ORB_GLOW_LAYERS - 1], orb_breathe_cb);

    if (s_rms_timer) {
        lv_timer_delete(s_rms_timer);
        s_rms_timer = NULL;
    }
    /* 60 ms cadence — fast enough to feel responsive to speech
     * envelope, slow enough to not thrash the LVGL style system. */
    s_rms_timer = lv_timer_create(rms_tick_cb, 60, NULL);
}

static void stop_rms_anim(void)
{
    if (s_rms_timer) {
        lv_timer_delete(s_rms_timer);
        s_rms_timer = NULL;
    }
}

static void start_wave_anim(void)
{
    /*
     * Each bar gets a sine-like animation with phase offset.
     * Bar heights cycle between min and max at different rates
     * to create an organic wave pattern.
     */
    static const int32_t phase_offsets[WAVE_BARS] = {0, 120, 60, 180, 90};

    for (int i = 0; i < WAVE_BARS; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_wave_bars[i]);
        lv_anim_set_values(&a, WAVE_BAR_MIN_H, WAVE_BAR_MAX_H);
        lv_anim_set_duration(&a, ANIM_WAVE_MS + (i * 60));
        lv_anim_set_playback_duration(&a, ANIM_WAVE_MS + (i * 60));
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, wave_bar_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_delay(&a, phase_offsets[i]);
        lv_anim_start(&a);
    }
}

static void stop_all_anims(void)
{
   /* Phase 3 of #42: also tear down the spring-driven orb size anim
    * so its 60 fps timer doesn't fire on a freed s_orb_ring after
    * overlay hide / screen change.  Resets s_orb_size_current so
    * the next overlay show snaps to the correct first-frame size
    * via the "first call" branch in set_orb_size. */
   if (s_orb_anim_t) {
      lv_timer_delete(s_orb_anim_t);
      s_orb_anim_t = NULL;
   }
   s_orb_size_current = 0.0f;

   /* Stop orb animations */
   lv_anim_delete(s_orb_ring, orb_ring_opa_cb);
   for (int i = 0; i < ORB_GLOW_LAYERS; i++) {
      lv_anim_delete(s_orb_glow[i], orb_breathe_cb);
   }

    /* Stop wave bar animations */
    for (int i = 0; i < WAVE_BARS; i++) {
        lv_anim_delete(s_wave_bars[i], wave_bar_cb);
    }

    /* Stop dot timer */
    if (s_dot_timer) {
        lv_timer_delete(s_dot_timer);
        s_dot_timer = NULL;
    }

    /* Stop recording duration timer (Fix #5) */
    if (s_rec_timer) {
        lv_timer_delete(s_rec_timer);
        s_rec_timer = NULL;
    }

    /* Cancel stuck watchdog if running */
    if (s_stuck_timer) {
        lv_timer_delete(s_stuck_timer);
        s_stuck_timer = NULL;
    }

    /* Cancel READY auto-hide timer — prevents dangling pointer UAF */
    if (s_auto_hide) {
        lv_timer_delete(s_auto_hide);
        s_auto_hide = NULL;
    }

    /* US-C10: Cancel error/auto-hide timer — fires auto_hide_timer_cb which
     * touches LVGL objects. Must be cleaned up on every exit path. */
    if (s_hide_timer) {
        lv_timer_delete(s_hide_timer);
        s_hide_timer = NULL;
    }

    /* W15-P02: cancel RMS tick if it's running. */
    stop_rms_anim();

    /* Remove orb click handler if set (Fix #4 cleanup) */
    lv_obj_clear_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb(s_orb_container, orb_speak_click_cb);
}

/* ── Animation callbacks ──────────────────────────────────────── */

static void orb_breathe_cb(void *obj, int32_t val)
{
   /* TT #511 wave-1.5: obj may be NULL (dead-coded orb widgets). */
   if (!obj) return;
   lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

static void orb_ring_opa_cb(void *obj, int32_t val)
{
   if (!obj) return;
   lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

static void wave_bar_cb(void *obj, int32_t val)
{
    lv_obj_set_height((lv_obj_t *)obj, val);
}

static void mic_dot_pulse_cb(void *obj, int32_t val)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

static void fade_overlay_cb(void *obj, int32_t val)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);

    /* Scale child content opacity proportionally */
    lv_opa_t child_opa = (lv_opa_t)((val * 255) / VO_BG_OPA);
    lv_obj_set_style_opa((lv_obj_t *)obj, child_opa, 0);
}

/* fade_done_hide_cb removed — ui_voice_hide() is now instant (no fade animation).
 * The 150ms fade window caused crash: callback fired after navigation changed state. */

/* ── Timer callbacks ──────────────────────────────────────────── */

static void dot_timer_cb(lv_timer_t *t)
{
    /* Cycle through: ".  " -> ".. " -> "..." -> ".  " */
    static const char *dots[] = {".  ", ".. ", "..."};
    s_dot_phase = (s_dot_phase + 1) % 3;
    lv_label_set_text(s_lbl_dots, dots[s_dot_phase]);
}

static void auto_hide_timer_cb(lv_timer_t *t)
{
    /* Clear whichever timer pointer triggered us */
    if (t == s_hide_timer) s_hide_timer = NULL;
    if (t == s_auto_hide) s_auto_hide = NULL;

    if (s_visible) {
        ESP_LOGI(TAG, "Auto-hiding overlay (state=%d)", s_cur_state);
        ui_voice_hide();
    }
}

static void rec_timer_cb(lv_timer_t *t)
{
    s_rec_seconds++;
    char buf[32];
    if (voice_get_mode() == VOICE_MODE_ASK) {
        /* Ask mode: show countdown from 30s */
        int remaining = 30 - s_rec_seconds;
        if (remaining <= 0) remaining = 0;
        snprintf(buf, sizeof(buf), "0:%02d left", remaining);
        if (remaining <= 5 && remaining > 0) {
            lv_obj_set_style_text_color(s_lbl_rec_time, lv_color_hex(0xFF5252), 0);
        }
    } else {
        /* Dictation: show elapsed time */
        snprintf(buf, sizeof(buf), "%d:%02d", s_rec_seconds / 60, s_rec_seconds % 60);
    }
    lv_label_set_text(s_lbl_rec_time, buf);
}

/* ================================================================
 *  Mode switch helper (runs outside LVGL context to avoid watchdog)
 * ================================================================ */
/* Wave 14 W14-H06: these are now plain job functions, not FreeRTOS
 * tasks.  The tab5_worker task runs them one at a time, so the old
 * "spawn 8 KB task + vTaskSuspend forever" leak pattern is gone.
 * Firing the mic orb 40 times no longer costs 320 KB of PSRAM + 40
 * permanently-suspended TCBs. */
static void mode_switch_voice_job(void *arg)
{
    (void)arg;
    tab5_mode_switch(MODE_VOICE);
}

static void mode_switch_idle_job(void *arg)
{
    (void)arg;
    tab5_mode_switch(MODE_IDLE);
}

/* ================================================================
 *  Click handlers
 * ================================================================ */

static void mic_click_cb(lv_event_t *e)
{
    (void)e;
    voice_state_t state = voice_get_state();

    ESP_LOGI(TAG, "Mic button tapped (state=%d)", state);

    switch (state) {
    case VOICE_STATE_IDLE:
        /* Not connected — connect to Dragon, then auto-start Ask mode.
         * W14-H06: dispatched on the shared worker. */
        ESP_LOGI(TAG, "Requesting VOICE mode (pending: ask)...");
        s_pending_ask = true;
        tab5_worker_enqueue(mode_switch_voice_job, NULL, "mode_voice");
        break;
    case VOICE_STATE_READY: {
        ESP_LOGI(TAG, "READY → Ask mode");
        voice_start_listening();
        break;
    }
    case VOICE_STATE_LISTENING:
        /* Recording — stop and send for processing */
        voice_stop_listening();
        break;
    case VOICE_STATE_PROCESSING:
    case VOICE_STATE_SPEAKING:
        /* Busy — cancel */
        voice_cancel();
        break;
    case VOICE_STATE_CONNECTING:
    case VOICE_STATE_RECONNECTING:
        /* Already connecting — ignore the tap; home pill will update. */
        break;
    }
}

static void mic_long_press_cb(lv_event_t *e)
{
    (void)e;
    voice_state_t state = voice_get_state();
    ESP_LOGI(TAG, "Mic long-press → dictation (state=%d)", state);

    s_dictation_from_anywhere = true;

    if (state == VOICE_STATE_IDLE) {
        tab5_worker_enqueue(mode_switch_voice_job, NULL, "mode_voice");
    } else if (state == VOICE_STATE_READY) {
        esp_err_t ret = voice_start_dictation();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "voice_start_dictation failed: %s", esp_err_to_name(ret));
        }
    }
}

static void close_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Close button tapped — cancelling");
    voice_cancel();
    ui_voice_hide();
}

static void send_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Send/stop button tapped — submitting recording");
    voice_stop_listening();
}

static void orb_ready_click_cb(lv_event_t *e)
{
    (void)e;
    if (voice_get_state() != VOICE_STATE_READY) {
        return;  /* Guard: only act in READY state */
    }
    ESP_LOGI(TAG, "Orb tapped in READY state — starting recording");
    esp_err_t err = voice_start_listening();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "voice_start_listening failed: %s", esp_err_to_name(err));
        lv_label_set_text(s_lbl_status, "Connection lost — tap to retry");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xFF4444), 0);
        lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
    }
}

static void orb_speak_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Orb tapped during speaking — interrupting TTS");
    voice_cancel();
}

/* ================================================================
 *  Dictation auto-stop countdown warning (US-PR18)
 *
 *  Called from voice.c mic task via lv_async_call. Updates the status
 *  label to warn user that dictation will auto-stop due to silence.
 * ================================================================ */

/* Async callback data — encoded as (void*)(intptr_t) seconds_remaining */
static void async_auto_stop_warning(void *arg)
{
    int secs = (int)(intptr_t)arg;

    /* Only update if we're still in LISTENING/dictation state */
    if (s_cur_state != VOICE_STATE_LISTENING) return;
    if (!s_lbl_status) return;

    if (secs <= 0) {
        /* Clear warning — user started speaking again */
        if (voice_get_mode() == VOICE_MODE_DICTATE) {
            lv_label_set_text(s_lbl_status, "Dictating...");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(VO_CYAN), 0);
            lv_obj_set_style_text_font(s_lbl_status, FONT_HEADING, 0);
        }
    } else {
        /* Show countdown: "Stopping in 2..." or "Stopping in 1..." */
        char buf[32];
        snprintf(buf, sizeof(buf), "Stopping in %d...", secs);
        lv_label_set_text(s_lbl_status, buf);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xFF5252), 0);
        lv_obj_set_style_text_font(s_lbl_status, FONT_HEADING, 0);
    }
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0,
                 ORB_SZ_LISTEN / 2 + ORB_Y_OFFSET + 30);
    lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
}

void ui_voice_show_auto_stop_warning(int seconds_remaining)
{
    /* Thread-safe: schedules LVGL update on Core 0 via async call */
    tab5_lv_async_call(async_auto_stop_warning, (void *)(intptr_t)seconds_remaining);
}
