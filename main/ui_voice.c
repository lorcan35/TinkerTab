/*
 * ui_voice.c — Glyph OS voice overlay
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
 * The orb is the soul of Glyph — it breathes, pulses, and responds.
 */

#include "ui_voice.h"
#include "config.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ui_voice";

/* ── Palette — Voice overlay ──────────────────────────────────── */
#define VO_BG              0x000000
#define VO_BG_OPA          242        /* ~95% opacity */

/* Cyan — listening */
#define VO_CYAN            0x00E5FF
#define VO_CYAN_DIM        0x003844   /* rgba(0,229,255,0.1) on black */
#define VO_CYAN_BORDER     0x0D3840   /* rgba(0,229,255,0.2) on black */
#define VO_CYAN_GLOW       0x005F6B   /* orb inner glow layers */

/* Purple — processing / thinking */
#define VO_PURPLE          0xB388FF
#define VO_PURPLE_DIM      0x5A4580   /* brighter purple dim for visibility */

/* Green — speaking */
#define VO_GREEN           0x34D399
#define VO_GREEN_DIM       0x1E5C44   /* brighter green dim for visibility */

/* Text */
#define VO_TEXT_BRIGHT     0xFFFFFF
#define VO_TEXT_MID        0xB3B3B3   /* rgba(255,255,255,0.7) */
#define VO_TEXT_DIM        0x808080   /* rgba(255,255,255,0.5) */
#define VO_TEXT_FAINT      0x404040   /* rgba(255,255,255,0.25) */
#define VO_CLOSE_TEXT      0x808080   /* rgba(255,255,255,0.5) — visible */

/* ── Layout constants ─────────────────────────────────────────── */
#define SW                 720
#define SH                 1280

#define MIC_BTN_SZ         72
#define MIC_BTN_MARGIN     20
#define MIC_BTN_BOTTOM     84        /* above nav dots */
#define MIC_DOT_SZ         12        /* inner dot indicator */

#define ORB_SZ_LISTEN      200
#define ORB_SZ_SPEAK       220
#define ORB_RING_W         2
#define ORB_GLOW_LAYERS    4         /* concentric circles for radial gradient */

#define CLOSE_BTN_SZ       56
#define CLOSE_BTN_MARGIN   16

#define WAVE_BARS          5
#define WAVE_BAR_W         6
#define WAVE_BAR_GAP       10
#define WAVE_BAR_MAX_H     48
#define WAVE_BAR_MIN_H     10

/* Send/Stop button (shown during LISTENING) */
#define SEND_BTN_SZ        80
#define SEND_BTN_Y         1050      /* y-center from top */
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

static void mic_click_cb(lv_event_t *e);
static void close_click_cb(lv_event_t *e);
static void send_click_cb(lv_event_t *e);
static void orb_speak_click_cb(lv_event_t *e);
static void mic_dot_pulse_cb(void *obj, int32_t val);

static void start_breathe_anim(void);
static void start_pulse_anim(void);
static void start_wave_anim(void);
static void stop_all_anims(void);

static void orb_breathe_cb(void *obj, int32_t val);
static void orb_ring_opa_cb(void *obj, int32_t val);
static void wave_bar_cb(void *obj, int32_t val);
static void fade_overlay_cb(void *obj, int32_t val);
static void fade_done_hide_cb(lv_anim_t *a);
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
static lv_obj_t  *s_mic_dot       = NULL;

/* Overlay container */
static lv_obj_t  *s_overlay       = NULL;
static bool       s_visible       = false;

/* Orb elements */
static lv_obj_t  *s_orb_container = NULL;   /* invisible sizer for scaling */
static lv_obj_t  *s_orb_ring      = NULL;   /* outer ring stroke */
static lv_obj_t  *s_orb_glow[ORB_GLOW_LAYERS]; /* concentric gradient circles */

/* Text labels */
static lv_obj_t  *s_lbl_status    = NULL;   /* "Listening..." / "Thinking..." / "Speaking..." */
static lv_obj_t  *s_lbl_transcript = NULL;  /* STT transcript */
static lv_obj_t  *s_lbl_dots      = NULL;   /* "..." animated dots */

/* Wave bars */
static lv_obj_t  *s_wave_cont     = NULL;
static lv_obj_t  *s_wave_bars[WAVE_BARS];

/* Close button */
static lv_obj_t  *s_close_btn     = NULL;

/* Send/Stop button (LISTENING state) */
static lv_obj_t  *s_send_btn      = NULL;

/* Recording duration label + timer */
static lv_obj_t   *s_lbl_rec_time = NULL;
static lv_timer_t *s_rec_timer    = NULL;
static int         s_rec_seconds  = 0;

/* Timers */
static lv_timer_t *s_dot_timer    = NULL;
static lv_timer_t *s_hide_timer   = NULL;
static int         s_dot_phase    = 0;

/* Current state tracking */
static voice_state_t s_cur_state  = VOICE_STATE_IDLE;
static bool s_initialized         = false;

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

void ui_voice_on_state_change(voice_state_t state, const char *detail)
{
    ESP_LOGI(TAG, "State change: %d -> %d (%s)", s_cur_state, state,
             detail ? detail : "");

    s_cur_state = state;
    update_mic_button_state(state);

    /* Cancel any pending auto-hide */
    if (s_hide_timer) {
        lv_timer_delete(s_hide_timer);
        s_hide_timer = NULL;
    }

    switch (state) {
    case VOICE_STATE_IDLE:
        if (detail && s_visible) {
            /* Show error briefly before hiding (e.g. "connect failed") */
            stop_all_anims();
            if (detail && strstr(detail, "disconnect")) {
                lv_label_set_text(s_lbl_status, "Disconnected");
            } else if (detail && strstr(detail, "timeout")) {
                lv_label_set_text(s_lbl_status, "Response timed out");
            } else {
                lv_label_set_text(s_lbl_status, "Connection failed");
            }
            lv_obj_set_style_text_color(s_lbl_status,
                lv_color_hex(0xFF4444), 0);
            lv_obj_set_style_text_font(s_lbl_status,
                &lv_font_montserrat_20, 0);
            lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, 40);
            /* Auto-hide after 2s */
            s_hide_timer = lv_timer_create(auto_hide_timer_cb, 2000, NULL);
            lv_timer_set_repeat_count(s_hide_timer, 1);
        } else {
            show_state_idle();
        }
        break;
    case VOICE_STATE_CONNECTING:
        /* Show overlay immediately with "Connecting..." */
        if (!s_visible) {
            ui_voice_show();
        }
        stop_all_anims();
        lv_label_set_text(s_lbl_status, "Connecting...");
        lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_lbl_status,
            lv_color_hex(VO_TEXT_DIM), 0);
        lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0,
                     ORB_SZ_LISTEN / 2 + 40);
        /* Show orb in dim cyan during connect */
        set_orb_color(VO_CYAN, VO_CYAN_DIM, LV_OPA_30);
        set_orb_size(ORB_SZ_LISTEN);
        start_pulse_anim();
        break;
    case VOICE_STATE_READY:
        if (s_visible) {
            /* Just finished speaking — auto-hide after delay */
            s_hide_timer = lv_timer_create(auto_hide_timer_cb, ANIM_HIDE_DELAY_MS, NULL);
            lv_timer_set_repeat_count(s_hide_timer, 1);
        }
        break;
    case VOICE_STATE_LISTENING:
        if (!s_visible) {
            ui_voice_show();
        }
        show_state_listening();
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
    if (s_visible) return;

    /* Cancel any in-flight fade-out animation */
    lv_anim_delete(s_overlay, fade_overlay_cb);

    s_visible = true;

    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_overlay, LV_OPA_TRANSP, 0);

    /* Fade in */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_overlay);
    lv_anim_set_values(&a, 0, VO_BG_OPA);
    lv_anim_set_duration(&a, ANIM_FADE_IN_MS);
    lv_anim_set_exec_cb(&a, fade_overlay_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "Voice overlay shown");
}

void ui_voice_hide(void)
{
    if (!s_visible) return;
    s_visible = false;

    stop_all_anims();

    /* Fade out */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_overlay);
    lv_anim_set_values(&a, VO_BG_OPA, 0);
    lv_anim_set_duration(&a, ANIM_FADE_OUT_MS);
    lv_anim_set_exec_cb(&a, fade_overlay_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, fade_done_hide_cb);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "Voice overlay hiding");
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

    /* Click handler */
    lv_obj_add_event_cb(s_mic_btn, mic_click_cb, LV_EVENT_CLICKED, NULL);

    /* Pressed feedback */
    lv_obj_set_style_bg_color(s_mic_btn, lv_color_hex(VO_CYAN_GLOW), LV_PART_MAIN | LV_STATE_PRESSED);
}

/* ================================================================
 *  Full-screen overlay
 * ================================================================ */

static void build_overlay(void)
{
    lv_obj_t *layer = lv_layer_top();

    /* Full-screen overlay backdrop */
    s_overlay = lv_obj_create(layer);
    lv_obj_set_size(s_overlay, SW, SH);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(VO_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, VO_BG_OPA, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Fix #6: background taps no longer cancel — only X button cancels.
     * Overlay still needs CLICKABLE to absorb taps (prevent pass-through
     * to widgets below), but no event handler attached. */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    /* Build child elements */
    build_orb(s_overlay);
    build_wave_bars(s_overlay);
    build_close_button(s_overlay);
    build_send_button(s_overlay);

    /* Status label — below orb */
    s_lbl_status = lv_label_create(s_overlay);
    lv_label_set_text(s_lbl_status, "");
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(VO_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(s_lbl_status, 3, 0);
    lv_obj_set_width(s_lbl_status, SW - 80);
    lv_obj_set_style_text_align(s_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + 40);

    /* Transcript label — below status */
    s_lbl_transcript = lv_label_create(s_overlay);
    lv_label_set_text(s_lbl_transcript, "");
    lv_obj_set_style_text_font(s_lbl_transcript, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_transcript, lv_color_hex(VO_TEXT_MID), 0);
    lv_obj_set_width(s_lbl_transcript, SW - 100);
    lv_obj_set_style_text_align(s_lbl_transcript, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_transcript, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + 70);
    lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);

    /* Thinking dots label — below transcript */
    s_lbl_dots = lv_label_create(s_overlay);
    lv_label_set_text(s_lbl_dots, "");
    lv_obj_set_style_text_font(s_lbl_dots, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_dots, lv_color_hex(VO_PURPLE_DIM), 0);
    lv_obj_set_style_text_align(s_lbl_dots, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_dots, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + 100);
    lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);

    /* Recording duration label — shown during LISTENING below status */
    s_lbl_rec_time = lv_label_create(s_overlay);
    lv_label_set_text(s_lbl_rec_time, "0:00");
    lv_obj_set_style_text_font(s_lbl_rec_time, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_lbl_rec_time, lv_color_hex(VO_TEXT_FAINT), 0);
    lv_obj_set_style_text_align(s_lbl_rec_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_rec_time, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + 70);
    lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);

    /* Start hidden */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ── Orb — the soul of Glyph ─────────────────────────────────── */

static void build_orb(lv_obj_t *parent)
{
    /* Invisible container for logical grouping (centered on screen) */
    s_orb_container = lv_obj_create(parent);
    lv_obj_set_size(s_orb_container, ORB_SZ_SPEAK + 20, ORB_SZ_SPEAK + 20);
    lv_obj_align(s_orb_container, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_bg_opa(s_orb_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_orb_container, 0, 0);
    lv_obj_set_style_pad_all(s_orb_container, 0, 0);
    lv_obj_clear_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /*
     * Fake radial gradient: concentric circles from center (brightest)
     * to outer (transparent). Each layer is slightly larger and dimmer.
     */
    int32_t base_sz = ORB_SZ_LISTEN;
    for (int i = ORB_GLOW_LAYERS - 1; i >= 0; i--) {
        /* Outermost layer is largest, innermost is smallest */
        float frac = (float)(ORB_GLOW_LAYERS - i) / (float)ORB_GLOW_LAYERS;
        int32_t sz = (int32_t)(base_sz * (0.4f + 0.6f * frac));
        /* Opacity: innermost ~38 (15%), outermost ~5 */
        lv_opa_t opa = (lv_opa_t)(10 + (int)(28.0f * frac));

        s_orb_glow[i] = lv_obj_create(s_orb_container);
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
    lv_obj_set_size(s_orb_ring, ORB_SZ_LISTEN, ORB_SZ_LISTEN);
    lv_obj_center(s_orb_ring);
    lv_obj_set_style_radius(s_orb_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_orb_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_orb_ring, ORB_RING_W, 0);
    lv_obj_set_style_border_color(s_orb_ring, lv_color_hex(VO_CYAN), 0);
    lv_obj_set_style_border_opa(s_orb_ring, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_orb_ring, 0, 0);
    lv_obj_clear_flag(s_orb_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* ── Wave bars — speaking visualization ───────────────────────── */

static void build_wave_bars(lv_obj_t *parent)
{
    int32_t total_w = WAVE_BARS * WAVE_BAR_W + (WAVE_BARS - 1) * WAVE_BAR_GAP;

    s_wave_cont = lv_obj_create(parent);
    lv_obj_set_size(s_wave_cont, total_w, WAVE_BAR_MAX_H);
    lv_obj_align(s_wave_cont, LV_ALIGN_CENTER, 0, ORB_SZ_SPEAK / 2 + 30);
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
    lv_obj_set_size(s_close_btn, CLOSE_BTN_SZ, CLOSE_BTN_SZ);
    lv_obj_align(s_close_btn, LV_ALIGN_TOP_RIGHT,
                 -(int32_t)CLOSE_BTN_MARGIN, CLOSE_BTN_MARGIN);
    lv_obj_set_style_radius(s_close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_close_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_close_btn, 0, 0);
    lv_obj_set_style_pad_all(s_close_btn, 0, 0);
    lv_obj_add_flag(s_close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_close_btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *x_lbl = lv_label_create(s_close_btn);
    lv_label_set_text(x_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x_lbl, lv_color_hex(VO_CLOSE_TEXT), 0);
    lv_obj_set_style_text_font(x_lbl, &lv_font_montserrat_24, 0);
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

/* ================================================================
 *  State visuals
 * ================================================================ */

static void show_state_listening(void)
{
    stop_all_anims();

    /* Orb: cyan, 200px, breathing */
    set_orb_color(VO_CYAN, VO_CYAN, LV_OPA_60);
    set_orb_size(ORB_SZ_LISTEN);

    /* Status text */
    lv_label_set_text(s_lbl_status, "Listening...");
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(VO_TEXT_DIM), 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + 40);

    /* Hide transcript, dots, wave */
    lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wave_cont, LV_OBJ_FLAG_HIDDEN);

    /* Show recording duration timer (Fix #5) */
    s_rec_seconds = 0;
    lv_label_set_text(s_lbl_rec_time, "0:00");
    lv_obj_align(s_lbl_rec_time, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + 70);
    lv_obj_clear_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
    s_rec_timer = lv_timer_create(rec_timer_cb, 1000, NULL);

    /* Show send/stop button (Fix #1) */
    lv_obj_clear_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);

    start_breathe_anim();
}

static void show_state_processing(const char *transcript)
{
    stop_all_anims();

    /* Hide listening-only elements */
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);

    /* Orb: purple tint, faster pulse */
    set_orb_color(VO_PURPLE, VO_PURPLE, LV_OPA_50);
    set_orb_size(ORB_SZ_LISTEN);

    /* Show transcript if available */
    if (transcript && transcript[0]) {
        /* Wrap in quotes */
        char buf[540];
        snprintf(buf, sizeof(buf), "\xE2\x80\x9C%s\xE2\x80\x9D", transcript);
        lv_label_set_text(s_lbl_transcript, buf);
        lv_obj_set_style_text_color(s_lbl_transcript,
            lv_color_hex(VO_TEXT_MID), 0);
        lv_obj_align(s_lbl_transcript, LV_ALIGN_CENTER, 0,
                     ORB_SZ_LISTEN / 2 + 40);
        lv_obj_clear_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);

        /* "Thinking..." below transcript */
        lv_label_set_text(s_lbl_status, "Thinking...");
        lv_obj_set_style_text_color(s_lbl_status,
            lv_color_hex(VO_PURPLE_DIM), 0);
        lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_20, 0);
        lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0,
                     ORB_SZ_LISTEN / 2 + 80);
    } else {
        lv_label_set_text(s_lbl_status, "Thinking...");
        lv_obj_set_style_text_color(s_lbl_status,
            lv_color_hex(VO_PURPLE_DIM), 0);
        lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_20, 0);
        lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0,
                     ORB_SZ_LISTEN / 2 + 40);
        lv_obj_add_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);
    }

    /* Show animated dots */
    s_dot_phase = 0;
    lv_label_set_text(s_lbl_dots, "   ");
    lv_obj_set_style_text_color(s_lbl_dots, lv_color_hex(VO_PURPLE), 0);
    lv_obj_align(s_lbl_dots, LV_ALIGN_CENTER, 0, ORB_SZ_LISTEN / 2 + 110);
    lv_obj_clear_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);
    s_dot_timer = lv_timer_create(dot_timer_cb, ANIM_DOT_MS, NULL);

    /* Hide wave */
    lv_obj_add_flag(s_wave_cont, LV_OBJ_FLAG_HIDDEN);

    start_pulse_anim();
}

static void show_state_speaking(void)
{
    stop_all_anims();

    /* Hide listening-only elements */
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);

    /* Orb: green, slightly larger */
    set_orb_color(VO_GREEN, VO_GREEN, LV_OPA_50);
    set_orb_size(ORB_SZ_SPEAK);

    /* Make orb tappable to interrupt TTS (Fix #4) */
    lv_obj_add_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_orb_container, orb_speak_click_cb, LV_EVENT_CLICKED, NULL);

    /* Status text — 20px to match "Thinking..." (Fix #3) */
    lv_label_set_text(s_lbl_status, "Speaking...");
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(VO_GREEN_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, ORB_SZ_SPEAK / 2 + 70);

    /* Keep transcript visible if it has content */
    const char *txt = lv_label_get_text(s_lbl_transcript);
    if (txt && txt[0]) {
        lv_obj_align(s_lbl_transcript, LV_ALIGN_CENTER, 0,
                     ORB_SZ_SPEAK / 2 + 90);
        lv_obj_clear_flag(s_lbl_transcript, LV_OBJ_FLAG_HIDDEN);
    }

    /* Hide dots */
    lv_obj_add_flag(s_lbl_dots, LV_OBJ_FLAG_HIDDEN);

    /* Show wave bars */
    lv_obj_align(s_wave_cont, LV_ALIGN_CENTER, 0, ORB_SZ_SPEAK / 2 + 30);
    lv_obj_clear_flag(s_wave_cont, LV_OBJ_FLAG_HIDDEN);

    start_breathe_anim();  /* gentle breathe on orb */
    start_wave_anim();
}

static void show_state_idle(void)
{
    stop_all_anims();

    /* Clean up listening/speaking elements */
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_rec_time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb(s_orb_container, orb_speak_click_cb);

    if (s_visible) {
        ui_voice_hide();
    }
}

/* ================================================================
 *  Orb helpers
 * ================================================================ */

static void set_orb_color(uint32_t ring_hex, uint32_t glow_hex, lv_opa_t ring_opa)
{
    lv_obj_set_style_border_color(s_orb_ring, lv_color_hex(ring_hex), 0);
    lv_obj_set_style_border_opa(s_orb_ring, ring_opa, 0);

    for (int i = 0; i < ORB_GLOW_LAYERS; i++) {
        lv_obj_set_style_bg_color(s_orb_glow[i], lv_color_hex(glow_hex), 0);
    }
}

static void set_orb_size(int32_t sz)
{
    lv_obj_set_size(s_orb_ring, sz, sz);
    lv_obj_center(s_orb_ring);

    for (int i = ORB_GLOW_LAYERS - 1; i >= 0; i--) {
        float frac = (float)(ORB_GLOW_LAYERS - i) / (float)ORB_GLOW_LAYERS;
        int32_t layer_sz = (int32_t)(sz * (0.4f + 0.6f * frac));
        lv_obj_set_size(s_orb_glow[i], layer_sz, layer_sz);
        lv_obj_center(s_orb_glow[i]);
    }
}

static void update_mic_button_state(voice_state_t state)
{
    /* Stop any existing mic dot pulse animation */
    lv_anim_delete(s_mic_dot, mic_dot_pulse_cb);

    switch (state) {
    case VOICE_STATE_IDLE:
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

    /* Remove orb click handler if set (Fix #4 cleanup) */
    lv_obj_clear_flag(s_orb_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb(s_orb_container, orb_speak_click_cb);
}

/* ── Animation callbacks ──────────────────────────────────────── */

static void orb_breathe_cb(void *obj, int32_t val)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

static void orb_ring_opa_cb(void *obj, int32_t val)
{
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

static void fade_done_hide_cb(lv_anim_t *a)
{
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    /* Reset overlay opacity for next show */
    lv_obj_set_style_opa(s_overlay, LV_OPA_COVER, 0);
}

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
    s_hide_timer = NULL;
    if (s_visible && (s_cur_state == VOICE_STATE_READY ||
                      s_cur_state == VOICE_STATE_IDLE)) {
        ESP_LOGI(TAG, "Auto-hiding overlay");
        ui_voice_hide();
    }
}

static void rec_timer_cb(lv_timer_t *t)
{
    s_rec_seconds++;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", s_rec_seconds / 60, s_rec_seconds % 60);
    lv_label_set_text(s_lbl_rec_time, buf);
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
        /* Not connected — async connect + auto-listen (non-blocking) */
        ESP_LOGI(TAG, "Connecting to Dragon voice server (async)...");
        {
            char dhost[64];
            tab5_settings_get_dragon_host(dhost, sizeof(dhost));
            voice_connect_async(dhost, TAB5_VOICE_PORT, true);
        }
        break;
    case VOICE_STATE_READY:
        /* Connected, idle — start listening */
        voice_start_listening();
        break;
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
        /* Already connecting — ignore */
        break;
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

static void orb_speak_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Orb tapped during speaking — interrupting TTS");
    voice_cancel();
}
