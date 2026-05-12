/*
 * ui_home.c — TinkerOS Home Screen — v4·C "Ambient Canvas"
 *
 * 720×1280 voice-first portrait layout for the ESP32-P4 Tab5.
 * See .superpowers/brainstorm/ui-overhaul-v4/c-ambient-canvas.html for the
 * source mockup and docs/WIDGETS.md §3.1 for the tone rendering contract.
 *
 * LVGL primitives only:
 *   - Orb: 156 px circle, 2-stop vertical gradient (existing paint_for_*).
 *   - Halo: 2 concentric dim amber circles (no shadow — LVGL 9 shadows can
 *     blow the 144 KB partial draw buffer at scale on ESP32-P4).
 *   - Rings: 3 border-only circles around the orb.
 *   - Now-slot card: widget_live renders directly into kicker/lede/stats.
 *   - Strip: 108 px say-pill + 56 px rail of 4 chips. One composed unit,
 *     replaces v5's stacked input+nav.
 *
 * Interactions (preserved from v5):
 *   Tap orb            → voice overlay + start listening
 *   Long-press orb     → cycle voice mode (Local → Hybrid → Cloud → Claw)
 *   Tap now-card       → Agents overlay (thread context)
 *   Tap sys label      → Memory overlay
 *   Long-press mode    → (future) open segmented picker sheet
 *   Swipe up           → Focus
 *   Swipe from left    → Notes
 *   Swipe from right   → Settings
 */

#include "ui_home.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "battery.h"
#include "config.h"
#include "debug_obs.h" /* W7-E.2: ui.notif.now obs events */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_cache.h"
#include "settings.h"
#include "spring_anim.h" /* Phase 2 of #42 — toast slide-in */
#include "tab5_rtc.h"
#include "task_worker.h"
#include "ui_agents.h"
#include "ui_chat.h"
#include "ui_core.h"
#include "ui_feedback.h" /* TT #328 Wave 10: ui_fb_* visual pressed states */
#include "ui_focus.h"
#include "ui_keyboard.h"
#include "ui_memory.h"
#include "ui_notes.h"
#include "ui_settings.h"
#include "ui_theme.h"
#include "ui_voice.h"
#include "voice.h"
#include "voice_onboard.h"
#include "widget.h"
#include "widget_icons.h"    /* TT #69 — icon library + render */
#include "widget_mode_dot.h" /* TT #328 Wave 6 */
#include "wifi.h"

static const char *TAG = "ui_home";

/* ── Layout constants ─────────────────────────────────────────── */
#define SW                720
#define SH                1280
#define SIDE_PAD          40

/* Orb stage — v4·D Sovereign Halo (Phase 1a: geometry bump only).
 * Orb 156→180 (voice-first identity), halos scaled 1.18x, rings scaled 1.16x.
 * Stage position unchanged (ORB_CY=320). No shadow primitives — glow comes
 * from the concentric ring + 2-stop radial recipe already in use. */
#define ORB_CX            (SW / 2)
#define ORB_CY            320
#define ORB_SIZE          180
#define HALO_OUTER        520
#define HALO_INNER        340
#define RING_OUTER        310
#define RING_MID          240
#define RING_INNER        196

/* v4·D Sovereign Halo live-line — replaces the 240 px NOW card.
 * Kicker + lede + optional right meta, top + bottom hairline borders,
 * no bg fill, no stats row, no accent bar. Sits just above the strip. */
#define CARD_X            SIDE_PAD
#define CARD_Y            956     /* 88 px tall, bottom sits at 1044 = strip_y - 18 gap */
#define CARD_W            (SW - 2 * SIDE_PAD)
#define CARD_H            88
#define CARD_PAD          20

/* Bottom strip — v4·D Sovereign Halo single-strip (Phase 1c).
 * Say pill alone, 104 px, with a 56 px 4-dot menu chip on its right edge.
 * Nav rail hidden (Sovereign spec: navigation lives behind the menu chip).
 * Total chrome: 40 pad + 104 strip = 144 px (was ~218 px with rail). */
#define STRIP_BOT_PAD     40
/* RAIL_H removed (U19 #206): the v4·D nav-rail was hidden then deleted;
 * its layout placeholder was already 0 px so removing it is a no-op. */
#define SAY_H             104
#define SAY_GAP           0

/* ── State ────────────────────────────────────────────────────── */
static lv_obj_t *s_screen          = NULL;

/* Status strip */
static lv_obj_t *s_sys_dot         = NULL;
static lv_obj_t *s_sys_label       = NULL;  /* left: "ONLINE" / "OFFLINE" / etc */
static lv_obj_t *s_time_label      = NULL;  /* right: "Thursday · 9:42" */

/* F1/F2 full-screen OFFLINE hero (audit 2026-04-20). Shown when NO_WIFI
 * or DRAGON_DOWN persists past 8 s. Auto-dismisses on recovery. */
static lv_obj_t *s_off_hero        = NULL;
static lv_obj_t *s_off_hero_title  = NULL;
static lv_obj_t *s_off_hero_body   = NULL;
static int64_t   s_degraded_since_ms = 0;

/* Orb stage */
static lv_obj_t *s_halo_outer      = NULL;
static lv_obj_t *s_halo_inner      = NULL;
static lv_obj_t *s_ring_outer      = NULL;
static lv_obj_t *s_ring_mid        = NULL;
static lv_obj_t *s_ring_inner      = NULL;
static lv_obj_t *s_orb             = NULL;
/* TT #328 Wave 11 — orb status pip.  P0 #16 said "the orb has 4 contracts
 * branching invisibly on voice_mode / chain_active."  This small badge
 * sits in the orb's top-right corner and flips glyph+color based on what
 * the next tap will actually do, so the contract is no longer hidden:
 *   muted     → red mute glyph     (tap will toast "mic muted")
 *   dragon ↓  → amber refresh      (tap will reconnect, not record)
 *   onboard A → violet loop        (tap will STOP an active K144 chain)
 *   normal    → hidden             (default tap-to-listen, no surprise) */
static lv_obj_t *s_orb_pip = NULL;

/* State word + greet line */
static lv_obj_t *s_state_word      = NULL;  /* "listening" / "offline" */
static lv_obj_t *s_greet_line      = NULL;  /* "morning, emile" */

/* Mode chip */
static lv_obj_t *s_mode_chip       = NULL;
static lv_obj_t *s_mode_dot        = NULL;
static lv_obj_t *s_mode_name       = NULL;  /* "Hybrid" */
static lv_obj_t *s_mode_sub        = NULL;  /* "LOCAL + CLOUD" */

/* Now-slot card (widget live target + empty-state) */
static lv_obj_t *s_now_card        = NULL;
static lv_obj_t *s_now_accent      = NULL;  /* 140×3 amber bar top-left */
static lv_obj_t *s_now_kicker      = NULL;
static lv_obj_t *s_now_lede = NULL;
/* W7-E.2: channel-message now-card overlay (lives on lv_layer_top). */
static lv_obj_t *s_channel_card = NULL;
/* TT #69 — icon slot top-right of the live card.  Container only;
 * `widget_icons_render` materialises the canvas + draws the
 * primitives inside on every refresh.  We track which icon id is
 * currently rendered so refresh is a no-op when unchanged. */
static lv_obj_t *s_now_icon = NULL;
static int s_now_icon_id = 0;         /* widget_icon_id_t value */
static uint32_t s_now_icon_color = 0; /* last render color (for change detect) */

/* Bottom strip */
static lv_obj_t *s_say_pill        = NULL;
static lv_obj_t *s_say_mic         = NULL;
static lv_obj_t *s_say_label_main  = NULL;
static lv_obj_t *s_say_label_sub   = NULL;
/* U19 (#206): the rail was already hidden post-create; deleted entirely. */

/* Toast + timer */
static lv_obj_t   *s_toast         = NULL;
static lv_timer_t *s_refresh_timer = NULL;

/* TT #328 Wave 6: s_mode_tint removed — widget_mode_dot owns color
 * resolution against the canonical th_mode_colors[] in ui_theme.c.
 * Kept s_mode_short here because the home pill uses different short
 * labels ("Local") than the canonical ui_theme th_mode_names[]
 * ("Local") — actually identical, future cleanup TODO. */
static const char *s_mode_short[VOICE_MODE_COUNT] = {"Local", "Hybrid", "Cloud", "Claw", "Onboard", "Solo"};
static const char *s_mode_tagline[VOICE_MODE_COUNT] = {
    "ON-DEVICE", "LOCAL + CLOUD", "CLOUD ONLY", "TINKERCLAW", "K144 ONBOARD", "OPENROUTER DIRECT",
};

static uint8_t s_badge_mode = 0;

/* v4·D Phase 4g widget_prompt tap → widget_action plumbing.
 * When a PROMPT widget claims the live slot, we cache its card_id +
 * per-choice event strings, build N invisible hit zones over the card
 * (one per choice row), and fire voice_send_widget_action() on tap. */
#define PROMPT_TAP_MAX  WIDGET_PROMPT_MAX_CHOICES
static lv_obj_t *s_prompt_tap[PROMPT_TAP_MAX] = {NULL};
static char      s_prompt_card_id[WIDGET_ID_LEN] = {0};
static char      s_prompt_events[PROMPT_TAP_MAX][WIDGET_PROMPT_EVENT_LEN] = {{0}};

/* v4·D Phase 4g widget_media inline image decode.
 * When a MEDIA widget claims the live slot with a new URL, we spawn a
 * one-shot FreeRTOS task to fetch + decode via media_cache (chat
 * already uses this path), then lv_async_call back on the LVGL thread
 * to bind the decoded RGB565 descriptor to s_media_img. */
static lv_obj_t        *s_media_img       = NULL;
static char             s_media_cur_url[256] = {0};
static lv_image_dsc_t   s_media_dsc       = {0};
static volatile bool    s_media_fetch_inflight = false;

/* ── Forward decls ───────────────────────────────────────────── */
static void refresh_timer_cb(lv_timer_t *t);
static void orb_click_cb(lv_event_t *e);
static void orb_long_press_cb(lv_event_t *e);
static void screen_gesture_cb(lv_event_t *e);
static void now_card_click_cb(lv_event_t *e);
static void now_card_long_press_cb(lv_event_t *e);
static void sys_click_cb(lv_event_t *e);
static void mode_chip_long_press_cb(lv_event_t *e);
static void mode_chip_click_cb(lv_event_t *e); /* TT #370 */
/* U19 (#206): rail callbacks removed with the rail itself. */
static bool any_overlay_visible(void);
static void show_toast_internal(const char *text);
static void orb_paint_for_mode(uint8_t mode);
static void orb_paint_for_tone(widget_tone_t tone);
/* TT #328 Wave 9 — first-launch mode-chip discoverability hint cb. */
static void mode_hint_fire_cb(lv_timer_t *t);
/* TT #328 Wave 11 — orb status pip painter (P0 #16). */
static void paint_orb_pip_for_context(void);
/* v4·D Phase 4g: widget_prompt hit-zone tap routing */
static void prompt_choice_tap_cb(lv_event_t *e);
static void refresh_prompt_hit_zones(const widget_t *w);
static void clear_prompt_hit_zones(void);
/* v4·D Phase 4g: widget_media inline image decode */
static void ensure_media_image_widget(void);
static void media_image_bind_cb(void *arg);
static void media_image_clear_cb(void *arg);
static void media_fetch_task(void *pv);
static void refresh_media_image(const widget_t *w);

/* ── Helpers ─────────────────────────────────────────────────── */

static const char *tone_name(widget_tone_t t)
{
    switch (t) {
        case WIDGET_TONE_CALM:        return "CALM";
        case WIDGET_TONE_ACTIVE:      return "ACTIVE";
        case WIDGET_TONE_APPROACHING: return "APPROACH";
        case WIDGET_TONE_URGENT:      return "URGENT";
        case WIDGET_TONE_DONE:        return "DONE";
        case WIDGET_TONE_INFO:        return "INFO";
        case WIDGET_TONE_SUCCESS:     return "OK";
        case WIDGET_TONE_ALERT:       return "ALERT";
        default:                      return "--";
    }
}

/* Sovereign Halo trail — single serif italic line that lives under the
 * state word.  Matches d-sovereign-halo.html:
 *   "Morning, Emile — where shall we begin?"
 * Time-of-day sensitive; "Emile" hardcoded until a user-name setting is
 * added.  em-dash is U+2014 (3-byte UTF-8 "\xe2\x80\x94"); the current
 * Fraunces italic font subset includes it. */
static const char *trail_for_hour(int hour)
{
    /* Em-dash (U+2014) is NOT in the Fraunces italic 22 font subset --
     * using it here renders as a tofu box on device.  Workaround lifted
     * from chat v4·C: substitute " -- " (ASCII).  Previously documented
     * in memory feedback_ui_overhaul_lessons. When a font re-subset is
     * done, swap these back to \xe2\x80\x94 for the real em-dash. */
    if (hour < 5)  return "Late, Emile -- still awake?";
    if (hour < 12) return "Morning, Emile -- where shall we begin?";
    if (hour < 17) return "Afternoon, Emile -- what's next?";
    if (hour < 21) return "Evening, Emile -- what's on your mind?";
    return "Tonight, Emile -- quiet thoughts?";
}

/* "Thursday · 9:42" — bullet is U+2022, which IS in the Montserrat subset. */
static void format_right_time(char *buf, size_t n, const struct tm *tm)
{
    static const char *wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday" };
    int w = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
    snprintf(buf, n, "%s \xe2\x80\xa2 %d:%02d",
             wd[w], tm->tm_hour, tm->tm_min);
}

/* ── Orb painting (preserved from v5) ─────────────────────────── */

static void orb_paint_for_mode(uint8_t mode)
{
    if (!s_orb) return;
    (void)mode;
    /* v4·D Sovereign Halo: the orb IS the brand -- treat it like a lantern.
     * Mode identity lives on the small mode-chip dot under the lede, not
     * on the orb.  Amber gradient always. Previously we tinted the orb
     * blue/rose/emerald per mode which fought the "voice-first brand
     * deserves maximum presence" rule from d-sovereign-halo.html. */
    const uint32_t top = 0xFFC75A;
    const uint32_t bot = 0xB9650A;
    lv_obj_set_style_bg_color(s_orb, lv_color_hex(top), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_orb, lv_color_hex(bot), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_orb, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_orb, LV_OPA_COVER, LV_PART_MAIN);
}

/* TT #328 Wave 11 P0 #16 — paint the orb status pip based on what the
 * next tap will actually do.  Hidden when the default tap-to-listen
 * contract holds; visible when something else will happen so the
 * branching is no longer invisible. */
static void paint_orb_pip_for_context(void) {
   if (!s_orb_pip) return;
   lv_obj_t *lbl = lv_obj_get_child(s_orb_pip, 0);
   if (!lbl) return;

   bool muted = tab5_settings_get_mic_mute() != 0;
   bool dragon = voice_is_connected();
   uint8_t vmode = tab5_settings_get_voice_mode();
   bool chain_active = (vmode == VOICE_MODE_ONBOARD) && voice_onboard_chain_active();

   /* Priority order: muted > chain-active > dragon-down > hidden.
    * Muted wins because privacy state is the most user-load-bearing
    * signal — even in vmode=4 a muted mic should override the
    * chain indicator. */
   const char *glyph = NULL;
   uint32_t border = TH_AMBER;
   if (muted) {
      glyph = LV_SYMBOL_MUTE;
      border = TH_STATUS_RED;
   } else if (chain_active) {
      glyph = LV_SYMBOL_LOOP;
      border = TH_MODE_ONBOARD; /* violet — matches Onboard chip */
   } else if (!dragon && vmode != VOICE_MODE_ONBOARD) {
      /* Dragon down + Onboard NOT active: tap will reconnect, not record. */
      glyph = LV_SYMBOL_REFRESH;
      border = TH_AMBER;
   }

   if (!glyph) {
      lv_obj_add_flag(s_orb_pip, LV_OBJ_FLAG_HIDDEN);
      return;
   }
   lv_label_set_text(lbl, glyph);
   lv_obj_set_style_text_color(lbl, lv_color_hex(border), 0);
   lv_obj_set_style_border_color(s_orb_pip, lv_color_hex(border), 0);
   lv_obj_clear_flag(s_orb_pip, LV_OBJ_FLAG_HIDDEN);
}

/* Widget-tone orb override — used when a live widget claims the slot.
 * calm=emerald, active=amber, approaching=amber-hot, urgent=rose, done=settled. */
static void orb_paint_for_tone(widget_tone_t tone)
{
    if (!s_orb) return;
    uint32_t top, bot;
    switch (tone) {
        case WIDGET_TONE_CALM:        top = 0x7DE69F; bot = 0x166C3A; break;
        case WIDGET_TONE_ACTIVE:      top = 0xFFC75A; bot = 0xB9650A; break;
        case WIDGET_TONE_APPROACHING: top = 0xFFB637; bot = 0xD97706; break;
        case WIDGET_TONE_URGENT:      top = 0xFF7E95; bot = 0xF43F5E; break;
        case WIDGET_TONE_DONE:        top = 0xBBFFCC; bot = 0x0E5E2A; break;
        default:                      top = 0xFFC75A; bot = 0xB9650A; break;
    }
    lv_obj_set_style_bg_color(s_orb, lv_color_hex(top), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_orb, lv_color_hex(bot), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_orb, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_orb, LV_OPA_COVER, LV_PART_MAIN);
}

/* Phase 4 of #42: bouncy dot pulse on mode cycle.
 *
 * Pre-fix the mode chip's text changed instantly on cycle (long-press
 * → next mode → label swap).  No visible feedback that the device
 * acknowledged the action before the user looked at the new label.
 *
 * Now the colour dot to the left of the mode name pulses via
 * SPRING_BOUNCY: snaps to peak (18 px) on cycle, springs back to
 * baseline (10 px) with a small undershoot.  ~1.2 s total cycle —
 * visible without being noisy.
 *
 * State invariants:
 *   s_dot_size_current is the live displayed size (= s_dot_spring.pos)
 *   s_dot_anim_t is non-NULL exactly while the spring is animating
 *   First-ever update_mode_ui call snaps to baseline without pulsing
 *   (the chip is being created; no prior mode to "feedback off") */
#define MODE_DOT_BASE_PX 10
#define MODE_DOT_PEAK_PX 18

static spring_anim_t s_dot_spring;
static lv_timer_t *s_dot_anim_t = NULL;
static float s_dot_size_current = 0.0f;
static bool s_dot_first_paint = true;

static void mode_dot_anim_cb(lv_timer_t *t) {
   if (!s_mode_dot) {
      s_dot_anim_t = NULL;
      lv_timer_delete(t);
      return;
   }
   float sz = spring_anim_update(&s_dot_spring, 1.0f / 60.0f);
   /* Floor at 4 px so an overshoot below baseline doesn't make the dot
    * visually disappear or flip negative; still leaves room for a
    * visible undershoot dip on the bounce. */
   if (sz < 4.0f) sz = 4.0f;
   s_dot_size_current = sz;
   int32_t isz = (int32_t)(sz + 0.5f);
   lv_obj_set_size(s_mode_dot, isz, isz);
   lv_obj_set_style_radius(s_mode_dot, isz / 2, 0);
   if (spring_anim_done(&s_dot_spring)) {
      s_dot_anim_t = NULL;
      lv_timer_delete(t);
   }
}

static void mode_dot_kick_pulse(void) {
   if (!s_mode_dot) return;
   /* Snap to peak immediately, then spring back to baseline with
    * SPRING_BOUNCY — the small undershoot below baseline gives the
    * iOS-style "boop" feel. */
   spring_anim_init(&s_dot_spring, SPRING_BOUNCY);
   spring_anim_retarget(&s_dot_spring, (float)MODE_DOT_PEAK_PX, (float)MODE_DOT_BASE_PX, 0.0f);
   s_dot_size_current = (float)MODE_DOT_PEAK_PX;
   lv_obj_set_size(s_mode_dot, MODE_DOT_PEAK_PX, MODE_DOT_PEAK_PX);
   lv_obj_set_style_radius(s_mode_dot, MODE_DOT_PEAK_PX / 2, 0);
   if (!s_dot_anim_t) {
      s_dot_anim_t = lv_timer_create(mode_dot_anim_cb, 16, NULL);
   }
}

static void update_mode_ui(uint8_t mode)
{
   /* TT #370 — was hardcoded `>= 4` which silently clamped vmode=4
    * (Onboard) to 0 (Local) in the home badge.  Now bounded by the
    * array size so vmode=4 + new vmode=5 (SOLO_DIRECT) both render
    * correctly. */
   if (mode >= VOICE_MODE_COUNT) mode = 0;
   bool changed = (s_badge_mode != mode);
   s_badge_mode = mode;
   /* TT #328 Wave 6 — recolor through shared widget.  mode_dot_anim_cb
    * still mutates size/radius directly for the pulse animation; that
    * stays open-coded since it's not shared. */
   if (s_mode_dot) widget_mode_dot_set_mode(s_mode_dot, mode);
   if (s_mode_name) lv_label_set_text(s_mode_name, s_mode_short[mode]);
   if (s_mode_sub) lv_label_set_text(s_mode_sub, s_mode_tagline[mode]);
   orb_paint_for_mode(mode);

   /* Phase 4 (#42): pulse the dot only on actual mode changes (not the
    * initial chip creation paint).  Without the first-paint guard, the
    * dot would visibly grow + bounce on every overlay open / home
    * re-create even when nothing changed. */
   if (s_dot_first_paint) {
      s_dot_first_paint = false;
      return;
   }
    if (changed) {
       mode_dot_kick_pulse();
    }
}

/* ── Centered placement helper ──────────────────────────────────
 * Parent is assumed to be s_screen (no flex), so absolute x = center - w/2. */
static void pos_centered(lv_obj_t *obj, int y, int w, int h)
{
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, (SW - w) / 2, y);
}

/* ── Screen build ───────────────────────────────────────────── */

lv_obj_t *ui_home_create(void)
{
    /* #173: defensive idempotent check.  Today's boot path only calls
     * ui_home_create() once, but if a future refactor ever adds a
     * second create (theme reload, locale switch, crash recovery that
     * re-calls boot sequence) the unguarded overwrite of s_screen
     * would silently leak the old home tree + every static object
     * pointer below.  Matches pattern already used by ui_notes,
     * ui_files, ui_camera (post-#172). */
    if (s_screen) {
        lv_screen_load(s_screen);
        return s_screen;
    }

    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, SW, SH);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_screen);
    lv_obj_add_event_cb(s_screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* ── Edge ticks (keep swipe affordance) ── */
    lv_obj_t *edge_l = lv_obj_create(s_screen);
    lv_obj_remove_style_all(edge_l);
    lv_obj_set_size(edge_l, 2, 40);
    lv_obj_set_pos(edge_l, 0, SH / 2 - 20);
    lv_obj_set_style_bg_color(edge_l, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(edge_l, 60, 0);
    lv_obj_set_style_radius(edge_l, 2, 0);

    lv_obj_t *edge_r = lv_obj_create(s_screen);
    lv_obj_remove_style_all(edge_r);
    lv_obj_set_size(edge_r, 2, 40);
    lv_obj_set_pos(edge_r, SW - 2, SH / 2 - 20);
    lv_obj_set_style_bg_color(edge_r, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(edge_r, 60, 0);
    lv_obj_set_style_radius(edge_r, 2, 0);

    /* ── Status strip (y=0..56) ────────────────────────────── */
    s_sys_dot = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_sys_dot);
    lv_obj_set_size(s_sys_dot, 8, 8);
    lv_obj_set_pos(s_sys_dot, SIDE_PAD, 32);
    lv_obj_set_style_radius(s_sys_dot, 4, 0);
    lv_obj_set_style_bg_color(s_sys_dot, lv_color_hex(TH_STATUS_GREEN), 0);
    lv_obj_set_style_bg_opa(s_sys_dot, LV_OPA_COVER, 0);

    s_sys_label = lv_label_create(s_screen);
    lv_label_set_text(s_sys_label, "ONLINE");
    lv_obj_set_pos(s_sys_label, SIDE_PAD + 18, 26);
    lv_obj_set_style_text_font(s_sys_label, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_sys_label, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(s_sys_label, 3, 0);
    lv_obj_add_flag(s_sys_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sys_label, sys_click_cb, LV_EVENT_CLICKED, NULL);

    s_time_label = lv_label_create(s_screen);
    lv_label_set_text(s_time_label, "");
    lv_obj_set_width(s_time_label, 320);
    lv_obj_set_pos(s_time_label, SW - SIDE_PAD - 320, 24);
    lv_obj_set_style_text_font(s_time_label, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_letter_space(s_time_label, 1, 0);

    /* ── Orb stage (halos + rings + orb) ───────────────────── */
    /* Halos: dim amber circles, stacked largest-first so opacity adds. */
    s_halo_outer = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_halo_outer);
    pos_centered(s_halo_outer, ORB_CY - HALO_OUTER / 2, HALO_OUTER, HALO_OUTER);
    lv_obj_set_style_radius(s_halo_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_halo_outer, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(s_halo_outer, 18, 0);   /* ~7% */
    lv_obj_clear_flag(s_halo_outer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_halo_inner = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_halo_inner);
    pos_centered(s_halo_inner, ORB_CY - HALO_INNER / 2, HALO_INNER, HALO_INNER);
    lv_obj_set_style_radius(s_halo_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_halo_inner, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(s_halo_inner, 38, 0);   /* ~15% */
    lv_obj_clear_flag(s_halo_inner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Rings: border-only circles for a "composed" orb. */
    s_ring_outer = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_ring_outer);
    pos_centered(s_ring_outer, ORB_CY - RING_OUTER / 2, RING_OUTER, RING_OUTER);
    lv_obj_set_style_radius(s_ring_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ring_outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring_outer, 1, 0);
    lv_obj_set_style_border_color(s_ring_outer, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_border_opa(s_ring_outer, 60, 0);
    lv_obj_clear_flag(s_ring_outer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_ring_mid = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_ring_mid);
    pos_centered(s_ring_mid, ORB_CY - RING_MID / 2, RING_MID, RING_MID);
    lv_obj_set_style_radius(s_ring_mid, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ring_mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring_mid, 1, 0);
    lv_obj_set_style_border_color(s_ring_mid, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_border_opa(s_ring_mid, 90, 0);
    lv_obj_clear_flag(s_ring_mid, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_ring_inner = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_ring_inner);
    pos_centered(s_ring_inner, ORB_CY - RING_INNER / 2, RING_INNER, RING_INNER);
    lv_obj_set_style_radius(s_ring_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ring_inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring_inner, 1, 0);
    lv_obj_set_style_border_color(s_ring_inner, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_border_opa(s_ring_inner, 140, 0);
    lv_obj_clear_flag(s_ring_inner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* The orb itself — 156 px, 2-stop vertical gradient, no shadow. */
    s_orb = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_orb);
    pos_centered(s_orb, ORB_CY - ORB_SIZE / 2, ORB_SIZE, ORB_SIZE);
    lv_obj_set_style_radius(s_orb, LV_RADIUS_CIRCLE, 0);
    orb_paint_for_mode(s_badge_mode);
    lv_obj_add_flag(s_orb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_orb, orb_click_cb, LV_EVENT_CLICKED, NULL);
    ui_fb_icon(s_orb); /* TT #328 Wave 10 — opacity dip on press */
    lv_obj_add_event_cb(s_orb, orb_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* TT #328 Wave 11 P0 #16 — orb status pip.  Small circular badge
     * anchored to the top-right of the orb that flips glyph+color
     * based on what the next tap will actually do.  Hidden by default
     * (the common case is "tap to listen" — no badge needed).  Refreshed
     * by paint_orb_pip_for_context() from ui_home_update_status. */
    s_orb_pip = lv_obj_create(s_orb);
    lv_obj_remove_style_all(s_orb_pip);
    lv_obj_set_size(s_orb_pip, 36, 36);
    lv_obj_align(s_orb_pip, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_radius(s_orb_pip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_orb_pip, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_orb_pip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_orb_pip, 2, 0);
    lv_obj_set_style_border_color(s_orb_pip, lv_color_hex(TH_AMBER), 0);
    lv_obj_clear_flag(s_orb_pip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_orb_pip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *pip_lbl = lv_label_create(s_orb_pip);
    lv_label_set_text(pip_lbl, "");
    lv_obj_set_style_text_font(pip_lbl, FONT_SMALL, 0);
    lv_obj_center(pip_lbl);

    /* ── State word + greet line (below orb) ───────────────── */
    s_state_word = lv_label_create(s_screen);
    lv_label_set_text(s_state_word, "ready");
    lv_obj_set_style_text_font(s_state_word, FONT_CLOCK, 0);  /* Montserrat 48 */
    lv_obj_set_style_text_color(s_state_word, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(s_state_word, -1, 0);
    lv_obj_set_width(s_state_word, SW);
    lv_obj_set_style_text_align(s_state_word, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_state_word, 0, 555);

    /* v4·D Sovereign Halo trail line — 24 px Fraunces italic serif that
     * sits under the state word.  Replaces the tiny all-caps "evening,
     * emile" label with an editorial subtitle matching d-sovereign-halo
     * mockup exactly.  Uses LABEL_LONG_WRAP so the em-dash clause can
     * break gracefully on narrow content if the greeting grows. */
    s_greet_line = lv_label_create(s_screen);
    lv_label_set_long_mode(s_greet_line, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_greet_line, trail_for_hour(9));  /* ui_home_update_status rewrites live */
    lv_obj_set_style_text_font(s_greet_line, FONT_CHAT_EMPH, 0);
    lv_obj_set_style_text_color(s_greet_line, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(s_greet_line, 0, 0);
    lv_obj_set_width(s_greet_line, SW - 2 * SIDE_PAD);
    lv_obj_set_style_text_align(s_greet_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_greet_line, SIDE_PAD, 640);

    /* ── Mode chip (pill, centered at y=680) ─────────────────── */
    s_mode_chip = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_mode_chip);
    pos_centered(s_mode_chip, 680, 360, 52);
    lv_obj_set_style_bg_color(s_mode_chip, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(s_mode_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mode_chip, 26, 0);
    lv_obj_set_style_border_width(s_mode_chip, 1, 0);
    lv_obj_set_style_border_color(s_mode_chip, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(s_mode_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mode_chip, LV_OBJ_FLAG_CLICKABLE);
    /* Audit U10 (UI-completeness): both short-tap and long-press open
     * the triple-dial sheet.  Pre-fix only long-press was wired, so
     * short-tap (the obvious affordance) silently did nothing and the
     * chip looked broken to new users. */
    lv_obj_add_event_cb(s_mode_chip, mode_chip_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    /* TT #370 — CLICKED routes through mode_chip_click_cb which
     * cycles K144↔SOLO when current is one of those tiers, otherwise
     * falls through to opening the 3-dial sheet. */
    lv_obj_add_event_cb(s_mode_chip, mode_chip_click_cb, LV_EVENT_CLICKED, NULL);
    ui_fb_card(s_mode_chip); /* TT #328 Wave 10 */

    /* TT #328 Wave 6 — shared mode-dot widget (was 6 lines of open-coded
     * LVGL primitives also duplicated in chat_header.c + drawer). */
    s_mode_dot = widget_mode_dot_create(s_mode_chip, 10, s_badge_mode);
    if (s_mode_dot) lv_obj_set_pos(s_mode_dot, 24, 21);

    s_mode_name = lv_label_create(s_mode_chip);
    lv_label_set_text(s_mode_name, s_mode_short[s_badge_mode]);
    lv_obj_set_pos(s_mode_name, 44, 15);
    lv_obj_set_style_text_font(s_mode_name, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s_mode_name, lv_color_hex(TH_TEXT_PRIMARY), 0);

    s_mode_sub = lv_label_create(s_mode_chip);
    lv_label_set_text(s_mode_sub, s_mode_tagline[s_badge_mode]);
    lv_obj_set_pos(s_mode_sub, 140, 19);
    lv_obj_set_style_text_font(s_mode_sub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_mode_sub, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(s_mode_sub, 2, 0);

    /* TT #328 Wave 9 — discoverability chevron.  Pre-Wave-9 the mode
     * chip looked like a passive status label — nothing on it
     * communicated "I'm tappable, tap to switch tier."  Adding a
     * subtle ▾ glyph at the right edge of the chip makes the
     * tap-to-open affordance visible, matching the chevron pattern
     * the chat header already uses (h->chev). */
    lv_obj_t *chip_chev = lv_label_create(s_mode_chip);
    lv_label_set_text(chip_chev, LV_SYMBOL_DOWN);
    lv_obj_set_pos(chip_chev, 330, 19);
    lv_obj_set_style_text_font(chip_chev, FONT_SMALL, 0);
    lv_obj_set_style_text_color(chip_chev, lv_color_hex(TH_TEXT_DIM), 0);

    /* ── Now-slot card ─────────────────────────────────────── */
    /* Live-line: no fill, 1 px borders on TOP + BOTTOM only (hairline rails) */
    s_now_card = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_now_card);
    lv_obj_set_pos(s_now_card, CARD_X, CARD_Y);
    lv_obj_set_size(s_now_card, CARD_W, CARD_H);
    lv_obj_set_style_bg_opa(s_now_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_now_card, 0, 0);
    lv_obj_set_style_border_width(s_now_card, 1, 0);
    lv_obj_set_style_border_side(s_now_card, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_now_card, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(s_now_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_now_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_now_card, now_card_click_cb, LV_EVENT_CLICKED, NULL);
    /* U24 (#206): long-press to dismiss the active live widget.  Hidden
     * gesture matches the orb / mode-chip long-press idiom — discoverable
     * once the user knows the platform.  Dismiss is local-store immediate
     * + Dragon notify-best-effort (skill is informed via widget_action so
     * it can stop re-emitting). */
    lv_obj_add_event_cb(s_now_card, now_card_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* Accent bar top-left (140×3 amber) */
    /* Accent bar removed in Sovereign Halo live-line -- amber pip inline with
     * kicker provides the equivalent colour cue.  Keep the s_now_accent
     * pointer as a 0-size placeholder so later code that touches it is a no-op. */
    s_now_accent = lv_obj_create(s_now_card);
    lv_obj_remove_style_all(s_now_accent);
    lv_obj_set_size(s_now_accent, 0, 0);
    lv_obj_set_pos(s_now_accent, 0, 0);
    lv_obj_add_flag(s_now_accent, LV_OBJ_FLAG_HIDDEN);

    /* Kicker */
    /* Kicker now sits on the left, vertically centred in the live-line band.
     * Width clamped to 106 px with LONG_DOT so long skill IDs ("timesense.
     * pomodoro") truncate instead of colliding with the lede to the right. */
    s_now_kicker = lv_label_create(s_now_card);
    lv_label_set_long_mode(s_now_kicker, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_now_kicker, "READY");
    lv_obj_set_pos(s_now_kicker, CARD_PAD, (CARD_H - 14) / 2);
    lv_obj_set_width(s_now_kicker, 106);
    lv_obj_set_style_text_font(s_now_kicker, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_now_kicker, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(s_now_kicker, 4, 0);

    /* Lede */
    /* Lede: single line, positioned after the ~80 px kicker column,
     * vertically centred. Wrapping disabled -- use ellipsis on overflow. */
    s_now_lede = lv_label_create(s_now_card);
    lv_label_set_long_mode(s_now_lede, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_now_lede, "Tap the orb to ask something.");
    lv_obj_set_pos(s_now_lede, CARD_PAD + 116, (CARD_H - 24) / 2);
    lv_obj_set_width(s_now_lede, CARD_W - CARD_PAD * 2 - 120);
    lv_obj_set_style_text_font(s_now_lede, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_now_lede, lv_color_hex(TH_TEXT_PRIMARY), 0);

    /* TT #69 — icon slot top-right of the live card.  Empty on
     * boot; populated when a LIVE widget arrives with widget.icon
     * set.  WIDGET_ICONS_SIZE (48 px) container; right-aligned with
     * CARD_PAD margin from the card edge. */
    s_now_icon = lv_obj_create(s_now_card);
    lv_obj_remove_style_all(s_now_icon);
    lv_obj_set_size(s_now_icon, WIDGET_ICONS_SIZE, WIDGET_ICONS_SIZE);
    lv_obj_set_pos(s_now_icon, CARD_W - WIDGET_ICONS_SIZE - CARD_PAD, (CARD_H - WIDGET_ICONS_SIZE) / 2);
    lv_obj_set_style_bg_opa(s_now_icon, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_now_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_now_icon, LV_OBJ_FLAG_CLICKABLE);
    /* Hidden until a widget with a known icon name arrives. */
    lv_obj_add_flag(s_now_icon, LV_OBJ_FLAG_HIDDEN);

    /* ── Say pill (108 px) ─────────────────────────────────── */
    const int strip_y = SH - STRIP_BOT_PAD - SAY_GAP - SAY_H;
    s_say_pill = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_say_pill);
    lv_obj_set_pos(s_say_pill, SIDE_PAD, strip_y);
    lv_obj_set_size(s_say_pill, CARD_W, SAY_H);
    lv_obj_set_style_bg_color(s_say_pill, lv_color_hex(TH_CARD), 0);
    lv_obj_set_style_bg_opa(s_say_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_say_pill, 54, 0);
    lv_obj_set_style_border_width(s_say_pill, 1, 0);
    lv_obj_set_style_border_color(s_say_pill, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(s_say_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_say_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_say_pill, orb_click_cb, LV_EVENT_CLICKED, NULL);
    ui_fb_card(s_say_pill); /* TT #328 Wave 10 */

    /* 84 px amber mic disc — inner element, left-aligned inside pill */
    s_say_mic = lv_obj_create(s_say_pill);
    lv_obj_remove_style_all(s_say_mic);
    lv_obj_set_size(s_say_mic, 84, 84);
    lv_obj_set_pos(s_say_mic, 12, 12);
    lv_obj_set_style_radius(s_say_mic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_say_mic, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_grad_color(s_say_mic, lv_color_hex(TH_AMBER_DARK), 0);
    lv_obj_set_style_bg_grad_dir(s_say_mic, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_say_mic, LV_OPA_COVER, 0);

    /* W8 (cross-stack audit 2026-05-11): replaced the U+2022 placeholder
     * dot with LV_SYMBOL_AUDIO, which IS in the Montserrat-48 subset
     * (already used by ui_audio.c).  Brand affordance is now a real
     * microphone glyph instead of an ambiguous bullet — closes the
     * audit's "mic glyph is a bullet dot" finding. */
    lv_obj_t *mic_mark = lv_label_create(s_say_mic);
    lv_label_set_text(mic_mark, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(mic_mark, FONT_CLOCK, 0);
    lv_obj_set_style_text_color(mic_mark, lv_color_hex(TH_BG), 0);
    lv_obj_align(mic_mark, LV_ALIGN_CENTER, 0, -6);

    /* W8: copy was "Hold to speak" but the say-pill is bound to tap
     * (click) only — long-press is on the orb above.  Renamed to match
     * the actual gesture. */
    s_say_label_main = lv_label_create(s_say_pill);
    lv_label_set_text(s_say_label_main, "Tap to talk");
    lv_obj_set_pos(s_say_label_main, 116, 26);
    lv_obj_set_style_text_font(s_say_label_main, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s_say_label_main, lv_color_hex(TH_TEXT_PRIMARY), 0);

    /* TT #328 Wave 7 — was 'OR SAY "DRAGON"'.  Wake-word stack was
     * retired in #162 (TDM-slot mapping unworkable without a custom
     * "Hey Tinker" WakeNet model); the sub-pill text was stale and
     * promised a feature the firmware doesn't have.  Replaced with a
     * concrete HOLD-vs-TAP hint that matches actual orb semantics.
     *
     * TT #328 Wave 1 (2026-04-30 audit) — was 'HOLD FOR DICTATE', but
     * orb_long_press_cb at the bottom of this file actually opens the
     * triple-dial mode sheet (Wave 8 of the prior audit landed that
     * binding deliberately).  Dictation is reachable from Notes →
     * Record (and from Wave 3 will land in chat).  The label was
     * outright lying about the binding; renamed to match what the
     * gesture actually does. */
    /* W8: sub-label clarified.  The long-press gesture lives on the
     * ORB above, not on the pill.  Pre-W8 "HOLD FOR MODES" was
     * ambiguous about which surface the hold applies to — visible
     * confusion in audit screenshots. */
    s_say_label_sub = lv_label_create(s_say_pill);
    lv_label_set_text(s_say_label_sub, "HOLD ORB FOR MODES");
    lv_obj_set_pos(s_say_label_sub, 116, 60);
    lv_obj_set_style_text_font(s_say_label_sub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_say_label_sub, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(s_say_label_sub, 4, 0);

    /* ── Menu chip (56×56, right edge of say pill) ─────────────
     * v4·D Sovereign Halo: the 4-dot grid replaces the full nav rail.
     * Tap opens chat (most-used second screen); long-press will open a
     * full navigation sheet in a later phase. The chip's MouseDown
     * propagation is stopped so taps on it don't also trigger the
     * say-pill mic press. */
    {
        const int chip_w = 56;
        const int chip_x = SAY_H /* 104 */ - chip_w + 12;  /* roughly centred-right vs 12 px outer pad */
        (void)chip_x; /* unused, we use set_pos explicitly below */
        lv_obj_t *menu = lv_obj_create(s_say_pill);
        lv_obj_remove_style_all(menu);
        lv_obj_set_size(menu, 56, 56);
        /* say pill inner = CARD_W = 640. chip at x = 640 - 56 - 12 = 572 */
        lv_obj_set_pos(menu, CARD_W - 56 - 12, (SAY_H - 56) / 2);
        lv_obj_set_style_bg_color(menu, lv_color_hex(TH_CARD_ELEVATED), 0);
        lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(menu, 18, 0);
        lv_obj_set_style_border_width(menu, 1, 0);
        lv_obj_set_style_border_color(menu, lv_color_hex(0x1E1E2A), 0);
        lv_obj_clear_flag(menu, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(menu, LV_OBJ_FLAG_CLICKABLE);
        /* v4·D Sovereign Halo nav sheet -- the 4-dot chip is the menu
         * hub (Chat / Notes / Settings / Camera / Files / Memory), not a
         * chat shortcut.  See d-sovereign-halo.html: "menu hides inside
         * 4-dot chip". */
        extern void menu_chip_click_cb(lv_event_t *e);
        lv_obj_add_event_cb(menu, menu_chip_click_cb, LV_EVENT_CLICKED, NULL);
        ui_fb_card(menu); /* TT #328 Wave 10 */
        /* 2×2 dot grid -- draw 4 dim dots inside the chip */
        for (int r = 0; r < 2; r++) {
            for (int c = 0; c < 2; c++) {
                lv_obj_t *dot = lv_obj_create(menu);
                lv_obj_remove_style_all(dot);
                lv_obj_set_size(dot, 6, 6);
                lv_obj_set_pos(dot, 18 + c * 14, 18 + r * 14);
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(dot, lv_color_hex(TH_TEXT_SECONDARY), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            }
        }
    }

    /* U19 (#206): rail removed.  v4·D Sovereign Halo replaced it with
     * the bottom-right menu chip → ui_nav_sheet, so the rail container
     * + 4 chip widgets + 4 labels (~80 LV objects) were just sitting
     * hidden, eating internal-SRAM headroom and flex-layout cycles
     * during ui_home_create.  The rail's per-chip callbacks
     * (rail_home_cb / rail_threads_cb / rail_notes_cb / rail_settings_cb
     * + their two async trampolines) had no other callers and went
     * with it. */

    /* Initial fill */
    ui_home_update_status();

    if (s_refresh_timer == NULL) {
        /* 2 s safety-net poll — the real state push is from voice.c via
         * voice_set_state() + ws_receive_task cleanup, which fires within
         * one LVGL tick on disconnect. This timer just catches anything
         * the push missed (e.g. a state change during LVGL-lock contention
         * where the fallback ui_home_refresh_sys_label() also got delayed). */
        s_refresh_timer = lv_timer_create(refresh_timer_cb, 2000, NULL);
    }

    /* Force full-screen invalidate — LVGL PARTIAL mode only paints dirty
     * regions, and lv_screen_load doesn't always dirty the entire 720×1280
     * on the first create. Without this, stale PSRAM framebuffer content
     * from a previous screen (or a prior boot) persists in areas the home
     * widgets didn't explicitly cover.
     *
     * Also tell the display that the whole area is "invalid" — belt-and-
     * braces so the next refresh cycles through every strip on a PARTIAL
     * render configuration (draw buffer is only 144 KB so each cycle
     * handles ~100 rows; the dirty area must stay marked dirty until
     * every strip is painted). */
    lv_obj_invalidate(s_screen);
    /* lv_refr_now forces LVGL to cycle through every strip of the draw
     * buffer before returning, guaranteeing all 1280 rows are painted to
     * the PSRAM framebuffer — otherwise on a 144 KB buffer (~100 rows)
     * only the first strip lands per lv_timer_handler tick and stale
     * content in subsequent strips is visible via /screenshot. */
    lv_refr_now(lv_display_get_default());

    /* U16 (#206): boot-time hide of the floating keyboard trigger so
     * its (640, 1136) hitbox doesn't shadow the home menu chip at
     * (612..668, 1160..1216).  See ui_home_go_home for the rationale. */
    extern void ui_keyboard_set_trigger_visible(bool);
    ui_keyboard_set_trigger_visible(false);

    /* TT #328 Wave 10 — hide the persistent floating home button.
     * Home doesn't need an escape hatch to itself; the chip + say-pill
     * + 4-dot menu chip already cover navigation intent here. */
    extern void ui_chrome_set_home_visible(bool visible);
    ui_chrome_set_home_visible(false);
    /* TT #328 Wave 11 follow-up — restore the global error banner if
     * one is up.  Non-home screens hide it on entry to keep their own
     * topbars unshadowed; home shows it because the user might
     * otherwise miss the K144-unavailable-style warning. */
    ui_home_set_error_banner_visible(true);

    /* TT #328 Wave 9 — first-launch mode-chip discoverability hint.
     * Pre-Wave-9 the dials were reachable only via long-press on orb
     * or chip; brand-new users had no signal that the mode chip was
     * interactive at all.  Fires ONCE per device (NVS-gated), ~6 s
     * after home renders so the user has settled in before the toast
     * lands.  Covered by tap-to-dismiss replacement if anything else
     * shows a toast in the interim. */
    if (!tab5_settings_mode_hint_seen()) {
       lv_timer_t *hint_t = lv_timer_create(mode_hint_fire_cb, 6000, NULL);
       if (hint_t) lv_timer_set_repeat_count(hint_t, 1);
    }

    ESP_LOGI(TAG, "v4 Ambient Canvas home created (orb %dpx, mode %d)",
             ORB_SIZE, s_badge_mode);
    return s_screen;
}

/* ── Periodic status refresh ─────────────────────────────────── */

/* F1/F2 OFFLINE hero — lazy-create big centered card on lv_layer_top. */
static void offline_hero_show(const char *title, const char *body)
{
    if (!s_off_hero) {
        s_off_hero = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_off_hero);
        lv_obj_set_size(s_off_hero, 720, 1280);
        lv_obj_set_pos(s_off_hero, 0, 0);
        lv_obj_set_style_bg_color(s_off_hero, lv_color_hex(0x08080E), 0);
        lv_obj_set_style_bg_opa(s_off_hero, 220, 0);
        lv_obj_clear_flag(s_off_hero, LV_OBJ_FLAG_SCROLLABLE);

        /* Card */
        lv_obj_t *card = lv_obj_create(s_off_hero);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 600, 420);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x13131F), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 28, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(TH_STATUS_RED), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* Red accent bar */
        lv_obj_t *bar = lv_obj_create(card);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 140, 4);
        lv_obj_set_pos(bar, 36, 40);
        lv_obj_set_style_bg_color(bar, lv_color_hex(TH_STATUS_RED), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 2, 0);

        s_off_hero_title = lv_label_create(card);
        lv_obj_set_style_text_font(s_off_hero_title, FONT_TITLE, 0);
        lv_obj_set_style_text_color(s_off_hero_title, lv_color_hex(TH_TEXT_PRIMARY), 0);
        lv_obj_set_pos(s_off_hero_title, 36, 68);

        s_off_hero_body = lv_label_create(card);
        lv_label_set_long_mode(s_off_hero_body, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_off_hero_body, 520);
        lv_obj_set_style_text_font(s_off_hero_body, FONT_BODY, 0);
        lv_obj_set_style_text_color(s_off_hero_body, lv_color_hex(TH_TEXT_DIM), 0);
        lv_obj_set_style_text_line_space(s_off_hero_body, 6, 0);
        lv_obj_set_pos(s_off_hero_body, 36, 140);
    }
    lv_label_set_text(s_off_hero_title, title ? title : "Offline");
    lv_label_set_text(s_off_hero_body, body ? body : "");
    lv_obj_clear_flag(s_off_hero, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_off_hero);
}

static void offline_hero_hide(void)
{
    if (s_off_hero) lv_obj_add_flag(s_off_hero, LV_OBJ_FLAG_HIDDEN);
}

void ui_home_update_status(void)
{
    time_t now = 0; time(&now);
    struct tm tm_local;
    localtime_r(&now, &tm_local);

    /* Trail line (shifts with hour) — v4·D Sovereign Halo editorial
     * subtitle.  Full serif italic sentences per d-sovereign-halo mockup. */
    if (s_greet_line) {
        const char *g = trail_for_hour(tm_local.tm_hour);
        const char *cur = lv_label_get_text(s_greet_line);
        if (!cur || strcmp(cur, g) != 0) lv_label_set_text(s_greet_line, g);
    }

    /* Right-side time label */
    if (s_time_label) {
        char buf[48];
        format_right_time(buf, sizeof(buf), &tm_local);
        const char *cur = lv_label_get_text(s_time_label);
        if (!cur || strcmp(cur, buf) != 0) lv_label_set_text(s_time_label, buf);
    }

    /* TT #328 Wave 11 P0 #16 — keep the orb status pip in sync with the
     * branching state so the user sees what tap will do at a glance. */
    paint_orb_pip_for_context();

    /* Edge-state detection — same priority as v5. */
    bool wifi_ok = tab5_wifi_connected();
    bool dragon  = voice_is_connected();
    bool mic_off = tab5_settings_get_mic_mute() != 0;
    bool quiet   = tab5_settings_quiet_active(tm_local.tm_hour);

    enum { ST_NORMAL, ST_NO_WIFI, ST_DRAGON_DOWN, ST_MUTED, ST_QUIET } state;
    if (!wifi_ok)     state = ST_NO_WIFI;
    else if (!dragon) state = ST_DRAGON_DOWN;
    else if (mic_off) state = ST_MUTED;
    else if (quiet)   state = ST_QUIET;
    else              state = ST_NORMAL;

    /* F1/F2 OFFLINE hero — shown only on GENUINE user-actionable outages.
     *
     * Wave 15 UX revision: previously the hero fired on any 20-s WS blip
     * to Dragon.  In practice that meant every Dragon restart, ngrok
     * handshake retry, router blip, or Tab5 Wi-Fi flap produced a big
     * "Dragon unreachable" full-screen card that the user could not
     * usefully act on — the device was already auto-reconnecting and
     * the card just added noise.  Feedback: "it's stupid because it
     * just doesn't work" (the DEVICE works; the BANNER was the problem).
     *
     * New policy:
     *   - Hero only fires for ST_NO_WIFI (user CAN act: go into
     *     Settings → Wi-Fi and pick a network).
     *   - ST_DRAGON_DOWN surfaces ONLY in the small top-left status
     *     pill (`NO DRAGON` / `Reconnecting...` via
     *     voice_get_degraded_reason).  If the user taps the orb while
     *     Dragon is down, orb_click_cb shows a brief toast instead.
     *     That's enough signal without hijacking the screen. */
    {
        bool degraded = (state == ST_NO_WIFI);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (degraded) {
            if (s_degraded_since_ms == 0) s_degraded_since_ms = now_ms;
            if (now_ms - s_degraded_since_ms >= 20000) {
                offline_hero_show("No Wi-Fi",
                    "Tab5 can't reach the network.\nVoice, memory, "
                    "and chat will come back as soon as Wi-Fi reconnects.\n"
                    "Notes still save to the SD card.");
            }
        } else {
            s_degraded_since_ms = 0;
            offline_hero_hide();
        }
    }

    /* Voice sub-state (overrides "ready" when something is happening) */
    voice_state_t vs = voice_get_state();
    const char *state_hint = NULL;
    if (dragon) {
        switch (vs) {
            case VOICE_STATE_LISTENING:  state_hint = "listening"; break;
            case VOICE_STATE_PROCESSING: state_hint = "thinking";  break;
            case VOICE_STATE_SPEAKING:   state_hint = "speaking";  break;
            default: break;
        }
    }

    /* State word below the orb */
    if (s_state_word) {
        const char *word;
        uint32_t col = TH_AMBER;
        switch (state) {
            case ST_NO_WIFI:      word = "no wi-fi";  col = TH_STATUS_RED; break;
            case ST_DRAGON_DOWN:  word = "offline";   col = TH_STATUS_RED; break;
            case ST_MUTED:        word = "muted";     col = 0x7A7A82;      break;
            case ST_QUIET:        word = "quiet";     col = 0x7A7A82;      break;
            default:              word = state_hint ? state_hint : "ready"; break;
        }
        const char *cur = lv_label_get_text(s_state_word);
        if (!cur || strcmp(cur, word) != 0) lv_label_set_text(s_state_word, word);
        lv_obj_set_style_text_color(s_state_word, lv_color_hex(col), 0);
    }

    /* Sys label (left of status strip) */
    if (s_sys_label) {
        const char *sys;
        uint32_t col = TH_TEXT_SECONDARY;
        /* Audit #10: vmode=4 doesn't depend on Dragon.  Suppress
         * "NO DRAGON" / "Reconnecting…" when the user has deliberately
         * picked Onboard AND the K144 is healthy; show a calm "ONBOARD"
         * word instead.  When K144 is unavailable in vmode=4 we fall
         * through to normal degraded reasoning. */
        const bool onboard_healthy = (tab5_settings_get_voice_mode() == VMODE_LOCAL_ONBOARD) &&
                                     (voice_onboard_failover_state() == 2 /* M5_FAIL_READY */);
        /* v4·D Gauntlet G7 fix: budget-capped state beats ONLINE so the
         * user sees it without looking for a missed toast.  Rose colour
         * matches the other "trust-broken" states (NO DRAGON, OFFLINE). */
        bool capped = (state == ST_NORMAL)
                      && (tab5_budget_get_cap_mils() > 0)
                      && (tab5_budget_get_today_mils() >= tab5_budget_get_cap_mils());
        /* v4·D connectivity audit T1.2: honest degraded-state status.
         * When we're mid-reconnect or hitting link issues, the old
         * "ONLINE" string lied to the user.  voice_get_degraded_reason
         * returns NULL when truly healthy; otherwise it returns a
         * short phrase we can paint directly. */
        const char *degraded = voice_get_degraded_reason();
        switch (state) {
            case ST_NO_WIFI:     sys = "OFFLINE";   col = TH_STATUS_RED; break;
            case ST_DRAGON_DOWN:
               if (onboard_healthy) {
                  sys = "ONBOARD";
                  col = TH_MODE_ONBOARD;
               } else {
                  sys = "NO DRAGON";
                  col = TH_STATUS_RED;
               }
               break;
            case ST_MUTED:       sys = "MUTED";     col = 0x7A7A82;      break;
            case ST_QUIET:       sys = "QUIET";     col = 0x7A7A82;      break;
            default:
                if (capped) {
                    sys = "CAPPED \xe2\x80\xa2 LOCAL TODAY";
                    col = TH_STATUS_RED;
                } else if (onboard_healthy && degraded) {
                   /* Onboard mode: Dragon-side degraded reasons aren't
                    * relevant to user-facing chat — show ONBOARD. */
                   sys = "ONBOARD";
                   col = TH_MODE_ONBOARD;
                } else if (degraded) {
                   /* Reconnecting / Remote / Dragon-unreachable, etc. */
                   sys = degraded;
                   col = 0xFBBF24; /* amber-hot -- attention, not error */
                } else if (onboard_healthy) {
                   sys = "ONBOARD";
                   col = TH_MODE_ONBOARD;
                } else {
                   sys = "ONLINE";
                   col = TH_TEXT_SECONDARY;
                }
                break;
        }
        const char *cur = lv_label_get_text(s_sys_label);
        if (!cur || strcmp(cur, sys) != 0) lv_label_set_text(s_sys_label, sys);
        lv_obj_set_style_text_color(s_sys_label, lv_color_hex(col), 0);
    }
    if (s_sys_dot) {
        uint32_t col;
        switch (state) {
            case ST_NO_WIFI:
            case ST_DRAGON_DOWN: col = TH_STATUS_RED;   break;
            case ST_MUTED:
            case ST_QUIET:       col = 0x5C5C68;        break;
            default:             col = TH_STATUS_GREEN; break;
        }
        lv_obj_set_style_bg_color(s_sys_dot, lv_color_hex(col), 0);
    }

    /* Now-slot — widget live wins; otherwise empty-state. */
    widget_t *live_w = widget_store_live_active();
    /* v4·D Phase 4c: LIST widgets need more vertical room to show their
     * items.  Grow the card height from 88->168 and switch lede to
     * LONG_WRAP.  Non-list widgets get the compact live-line. */
    if (live_w && s_now_card && s_now_lede && s_now_kicker) {
        bool is_list   = (live_w->type == WIDGET_TYPE_LIST   && live_w->items_count   > 0);
        bool is_chart  = (live_w->type == WIDGET_TYPE_CHART  && live_w->chart_count  > 0);
        bool is_media  = (live_w->type == WIDGET_TYPE_MEDIA);
        bool is_prompt = (live_w->type == WIDGET_TYPE_PROMPT && live_w->choices_count > 0);
        /* Auto-grow to the tall card when the body alone would wrap past one
         * line (> 48 chars) or when the caller included a title alongside
         * the body -- compact live-line mode only has room for a single
         * line of body text to the right of the kicker. */
        bool body_long = (live_w->body[0] && strlen(live_w->body) > 48);
        bool has_title_and_body = (live_w->title[0] && live_w->body[0]);
        bool is_tall  = is_list || is_chart || is_media || is_prompt
                        || body_long || has_title_and_body;
        int want_h = is_tall ? 168 : 88;
        if (lv_obj_get_height(s_now_card) != want_h) {
            lv_obj_set_height(s_now_card, want_h);
            /* Card bottom stays anchored at (CARD_Y + 88) -- taller
             * list/chart cards grow upward into the air below the mode chip. */
            lv_obj_set_y(s_now_card, CARD_Y + (88 - want_h));
            lv_label_set_long_mode(s_now_lede,
                is_tall ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
            /* Kicker moves to TOP in tall mode so it doesn't collide
             * with row 1 of the wrapping lede.  Compact live-line mode
             * keeps the kicker vertically-centred next to the lede. */
            lv_obj_set_y(s_now_kicker,
                is_tall ? 6 : (want_h - 14) / 2);
            lv_obj_set_y(s_now_lede,
                is_tall ? 30 : (want_h - 24) / 2);
            lv_obj_set_width(s_now_lede,
                is_tall ? (CARD_W - CARD_PAD * 2 - 20) : (CARD_W - CARD_PAD * 2 - 120));
            lv_obj_set_x(s_now_lede,
                is_tall ? CARD_PAD : (CARD_PAD + 116));
        }
    }
    /* v4·D Phase 4g: refresh prompt hit zones whenever the live slot
     * changes.  refresh_ = idempotent: clears + recreates per call. */
    if (live_w && live_w->type == WIDGET_TYPE_PROMPT
        && live_w->choices_count > 0) {
        refresh_prompt_hit_zones(live_w);
    } else {
        clear_prompt_hit_zones();
    }

    /* v4·D Phase 4g: inline image for widget_media.  refresh_media_image
     * kicks off a FreeRTOS fetch task if the URL changed, and hides the
     * image when the live slot isn't a MEDIA widget. */
    refresh_media_image(live_w);

    /* TT #69 — icon refresh.  When a live widget is active and its
     * `icon` field names one of the 16 built-in glyphs, render it
     * top-right of the card.  Cheap when unchanged (early-exit on
     * matching id + color) — re-render only fires when the widget
     * changes its icon or tone. */
    if (s_now_icon) {
       widget_icon_id_t want_id = WIDGET_ICON_NONE;
       lv_color_t want_color = lv_color_hex(0xC0C0C8);
       if (live_w && live_w->icon[0]) {
          want_id = widget_icons_lookup(live_w->icon);
          want_color = widget_icons_color_for_tone(live_w->tone);
       }
       uint32_t want_color_v = lv_color_to_int(want_color);
       if ((int)want_id != s_now_icon_id || want_color_v != s_now_icon_color) {
          s_now_icon_id = (int)want_id;
          s_now_icon_color = want_color_v;
          /* Clean any prior rendering.  lv_obj_clean cascades through
           * the canvas's DELETE event which frees the PSRAM pixel
           * buffer (canvas_delete_cb in widget_icons.c). */
          lv_obj_clean(s_now_icon);
          if (want_id != WIDGET_ICON_NONE) {
             widget_icons_render(s_now_icon, want_id, want_color, 0);
             lv_obj_remove_flag(s_now_icon, LV_OBJ_FLAG_HIDDEN);
          } else {
             lv_obj_add_flag(s_now_icon, LV_OBJ_FLAG_HIDDEN);
          }
       }
    }

    if (live_w) {
        /* v4·D Gauntlet G5 fix: reveal suppressed widgets.
         * When the priority queue is hiding N-1 other live widgets, the
         * user has no way to know.  Append "+N MORE" to the kicker so a
         * 10:30 calendar widget at priority 8 doesn't silently evict the
         * running focus timer at priority 5 without the user noticing.
         * See STORIES-GAUNTLET.md G5 "The 47-widget day". */
        int total_live = widget_store_live_count();
        int suppressed = total_live > 1 ? total_live - 1 : 0;

        /* When suppressed > 0 we drop the "NOW ." prefix -- the kicker
         * already clearly implies "now" by being the active live-slot.
         * This keeps the kicker under the ~90 px reserved column so the
         * lede next to it doesn't get collided with by a long
         * "NOW . CALENDAR . +2 MORE" string. */
        char kicker[96];
        if (suppressed > 0) {
            snprintf(kicker, sizeof(kicker),
                     "%.12s +%d",
                     live_w->skill_id[0] ? live_w->skill_id : "WIDGET",
                     suppressed);
        } else {
            snprintf(kicker, sizeof(kicker), "NOW \xe2\x80\xa2 %.12s",
                     live_w->skill_id[0] ? live_w->skill_id : "WIDGET");
        }
        for (int i = 0; kicker[i]; i++) {
            if (kicker[i] >= 'a' && kicker[i] <= 'z') kicker[i] -= 32;
        }
        if (s_now_kicker) {
            const char *cur = lv_label_get_text(s_now_kicker);
            if (!cur || strcmp(cur, kicker) != 0) lv_label_set_text(s_now_kicker, kicker);
        }
        if (s_now_lede) {
            char buf[400];
            if (live_w->type == WIDGET_TYPE_LIST && live_w->items_count > 0) {
                /* v4·D Phase 4c: render up to 3 list items in the lede.
                 * Each row reads "1  <text>  <value>" on its own line.
                 * Top 3 only; skill is expected to emit ranked order so
                 * the best hits render first.  Title lives on line 1. */
                int p = 0;
                if (live_w->title[0]) {
                    p += snprintf(buf + p, sizeof(buf) - p,
                                  "%.63s\n", live_w->title);
                }
                int show = live_w->items_count < 3 ? live_w->items_count : 3;
                for (int i = 0; i < show && p < (int)sizeof(buf) - 1; i++) {
                    const widget_list_item_t *it = &live_w->items[i];
                    if (it->value[0]) {
                        p += snprintf(buf + p, sizeof(buf) - p,
                                      "%d  %.48s  %s\n",
                                      i + 1, it->text, it->value);
                    } else {
                        p += snprintf(buf + p, sizeof(buf) - p,
                                      "%d  %.60s\n",
                                      i + 1, it->text);
                    }
                }
            } else if (live_w->type == WIDGET_TYPE_MEDIA) {
                /* v4·D Phase 4g: render media widget as title + body + a
                 * captioned URL line.  Inline image decode via
                 * media_cache_fetch is deferred -- first pass proves the
                 * wire + store + render loop. */
                int p = 0;
                if (live_w->title[0]) {
                    p += snprintf(buf + p, sizeof(buf) - p,
                                  "%.63s\n", live_w->title);
                }
                if (live_w->body[0]) {
                    p += snprintf(buf + p, sizeof(buf) - p,
                                  "%.180s\n", live_w->body);
                }
                if (live_w->media_alt[0] || live_w->media_url[0]) {
                    p += snprintf(buf + p, sizeof(buf) - p,
                                  "IMAGE \xe2\x80\xa2 %.63s",
                                  live_w->media_alt[0] ? live_w->media_alt
                                                        : live_w->media_url);
                }
            } else if (live_w->type == WIDGET_TYPE_PROMPT
                       && live_w->choices_count > 0) {
                /* v4·D Phase 4g: prompt widget renders as title + each
                 * choice as a numbered row.  Buttons are deferred --
                 * this first pass proves the store + render path. */
                int p = 0;
                if (live_w->title[0]) {
                    p += snprintf(buf + p, sizeof(buf) - p,
                                  "%.63s\n", live_w->title);
                }
                int n = live_w->choices_count;
                if (n > WIDGET_PROMPT_MAX_CHOICES) n = WIDGET_PROMPT_MAX_CHOICES;
                for (int i = 0; i < n && p < (int)sizeof(buf) - 1; i++) {
                    p += snprintf(buf + p, sizeof(buf) - p,
                                  "%d  %.44s\n",
                                  i + 1, live_w->choices[i].text);
                }
            } else if (live_w->type == WIDGET_TYPE_CHART
                       && live_w->chart_count > 0) {
                /* v4·D Phase 4f: render a mini bar chart as an ASCII
                 * histogram in the lede.  Five levels (" .:+#") give
                 * enough resolution to see trends without requiring a
                 * new LVGL object hierarchy or font glyphs. */
                int p = 0;
                if (live_w->title[0]) {
                    p += snprintf(buf + p, sizeof(buf) - p,
                                  "%.63s\n", live_w->title);
                }
                /* Determine the normalization bound. */
                float maxv = live_w->chart_max;
                if (maxv <= 0.0f) {
                    for (int i = 0; i < live_w->chart_count; i++) {
                        if (live_w->chart_values[i] > maxv) {
                            maxv = live_w->chart_values[i];
                        }
                    }
                }
                if (maxv <= 0.0f) maxv = 1.0f;
                static const char *levels[5] = { " ", ".", ":", "+", "#" };
                for (int i = 0; i < live_w->chart_count
                                && p < (int)sizeof(buf) - 8; i++) {
                    float f  = live_w->chart_values[i] / maxv;
                    int   lv = (int)(f * 4.0f + 0.5f);
                    if (lv < 0) lv = 0;
                    if (lv > 4) lv = 4;
                    p += snprintf(buf + p, sizeof(buf) - p, "%s ", levels[lv]);
                }
                p += snprintf(buf + p, sizeof(buf) - p, "\n");
                if (live_w->body[0] && p < (int)sizeof(buf) - 1) {
                    snprintf(buf + p, sizeof(buf) - p, "%.180s", live_w->body);
                }
            } else {
                /* title on top, body below (matches widget spec §3.1) */
                snprintf(buf, sizeof(buf), "%.63s\n%.180s",
                         live_w->title[0] ? live_w->title : "",
                         live_w->body[0]  ? live_w->body  : "");
            }
            const char *cur = lv_label_get_text(s_now_lede);
            if (!cur || strcmp(cur, buf) != 0) lv_label_set_text(s_now_lede, buf);
        }
        orb_paint_for_tone(live_w->tone);
    } else {
        /* Empty-state kicker + lede — Sovereign Halo live-line uses a
         * single short word on the left so the lede has room to breathe. */
        const char *kicker = "READY";
        const char *lede;
        /* Phase 3c: if the user has spent real cloud money today, show a
         * running total in place of the generic "tap to ask" line.  Buffer
         * lives in static storage so the pointer remains valid after
         * ui_home_update_status returns (LVGL keeps a reference until the
         * next label_set_text). */
        static char s_budget_lede[96];
        uint32_t spent_mils = tab5_budget_get_today_mils();
        uint32_t cap_mils   = tab5_budget_get_cap_mils();
        bool show_budget    = (state == ST_NORMAL && spent_mils > 0);
        switch (state) {
            case ST_NO_WIFI:
                lede = "Can't reach the network.\nTap the sys label to scan.";
                break;
            case ST_DRAGON_DOWN:
                lede = "Dragon is unreachable.\nVoice notes save locally.";
                break;
            case ST_MUTED:
                lede = "Mic off.\nTap the orb when you want me.";
                break;
            case ST_QUIET:
                lede = "Quiet hours.\nI won't speak until 07:00.";
                break;
            default:
                if (show_budget) {
                    int spent_cent = (int)((spent_mils + 500) / 1000);
                    int cap_cent   = (int)((cap_mils   + 500) / 1000);
                    int spent_d = spent_cent / 100;
                    int spent_c = spent_cent % 100;
                    int cap_d   = cap_cent   / 100;
                    int cap_c   = cap_cent   % 100;
                    snprintf(s_budget_lede, sizeof(s_budget_lede),
                             "Today  $%d.%02d  of  $%d.%02d",
                             spent_d, spent_c, cap_d, cap_c);
                    lede = s_budget_lede;
                    /* When we switch to showing money, the kicker reads
                     * "TODAY" instead of the generic "READY" to match the
                     * Sovereign spec M3 budget widget. */
                    kicker = "TODAY";
                } else {
                    lede = "Tap the orb to ask something.";
                }
                break;
        }
        if (s_now_kicker) {
            const char *cur = lv_label_get_text(s_now_kicker);
            if (!cur || strcmp(cur, kicker) != 0) lv_label_set_text(s_now_kicker, kicker);
        }
        if (s_now_lede) {
            const char *cur = lv_label_get_text(s_now_lede);
            if (!cur || strcmp(cur, lede) != 0) lv_label_set_text(s_now_lede, lede);
        }
        orb_paint_for_mode(s_badge_mode);
    }

    /* Mode chip (re-read from NVS) */
    uint8_t current_mode = tab5_settings_get_voice_mode();
    if (current_mode != s_badge_mode) update_mode_ui(current_mode);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_home_update_status();

    /* v4·D audit P0 fix: widget_store GC had zero callers.  Inactive
     * slots leaked until the 32-slot cap was hit, at which point the
     * eviction policy could throw out a running LIVE widget.  Tick
     * the GC once every ~30 s (refresh timer fires every 2 s, gate
     * on modulo so we don't burn CPU on every tick). */
    static uint32_t s_gc_tick = 0;
    if (++s_gc_tick >= 15) {
        s_gc_tick = 0;
        extern void widget_store_gc(uint32_t now_ms);
        widget_store_gc(lv_tick_get());
    }
}

/* ── Interactions ────────────────────────────────────────────── */

/* TT #328 Wave 4 — orb-overload swallow flag.  A borderline 380-450 ms tap
 * that triggers LV_EVENT_LONG_PRESSED still fires LV_EVENT_CLICKED on
 * release.  Pre-Wave-4 a 400 ms tap could simultaneously cycle the voice
 * mode (long-press path) AND open the listening overlay (click path) —
 * the user got a free Local→Hybrid jump and then immediately started
 * recording against the wrong tier.  Audit P0 #9.  Set by orb_long_press_cb;
 * consumed at orb_click_cb entry. */
/* Wave 8 P0 #10: the long-press now opens the mode-sheet (was: cycled
 * NVS directly with an undo pill).  Swallow flag still needed so the
 * trailing CLICKED doesn't also open the listening overlay. */
static bool s_orb_long_pressed = false;

static void orb_click_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;

    /* TT #328 Wave 4 — swallow the trailing CLICKED that LVGL emits after
     * a LONG_PRESSED.  See s_orb_long_pressed above for full rationale. */
    if (s_orb_long_pressed) {
       s_orb_long_pressed = false;
       return;
    }

    /* Tap debounce — prevents SDIO TX copy_buff exhaustion from repeat-taps.
     * Same 500 ms window as v5. */
    static uint32_t last_tap_ms = 0;
    uint32_t now = lv_tick_get();
    if (now - last_tap_ms < 500) {
        ESP_LOGI(TAG, "orb/pill tap debounced (dt=%lums)",
                 (unsigned long)(now - last_tap_ms));
        return;
    }
    last_tap_ms = now;

    ESP_LOGI(TAG, "orb/pill tap -> open voice");
    /* Wave 15 banner-UX: if the WS is currently disconnected we don't
     * want to open the LISTENING overlay and then silently drop audio
     * frames (they go to a closed socket).  Instead, kick off a
     * reconnect + show a brief toast so the user knows why their tap
     * didn't do anything yet, then return.  They can tap again when
     * the top-left pill reads "ready". */
    if (!voice_is_connected()) {
        char dhost[64];
        tab5_settings_get_dragon_host(dhost, sizeof(dhost));
        if (dhost[0]) voice_connect_async(dhost, TAB5_VOICE_PORT, false);
        ui_home_show_toast("Reconnecting to Dragon… try again in a moment.");
        return;
    }
    ui_voice_show();
    voice_start_listening();
}

/* v4·D Sovereign Halo: 4-dot chip opens the nav sheet (menu hub) so the
 * user can reach Settings / Notes / Camera / Files / Memory without
 * visible nav rail.  The chip used to jump straight to Chat; the nav
 * sheet puts Chat alongside its siblings where it belongs. */
void menu_chip_click_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    extern void ui_nav_sheet_show(void);
    ui_nav_sheet_show();
}

/* v4·D Phase 4g widget_prompt tap plumbing.
 *
 * refresh_prompt_hit_zones() builds (or re-parents) N invisible clickable
 * rectangles over the live card, one per choice row.  Each hit zone
 * carries its choice index in user_data.  The tap callback looks up the
 * cached event string and fires voice_send_widget_action back to Dragon.
 * That closes the prompt → user-taps-answer → skill-receives-answer
 * loop for the v4·D Phase 4g widget_prompt type.
 *
 * Layout assumes the tall 168 px card.  Title sits at y=30-54, choices
 * at y=54-90, 90-126, 126-162 (36 px each).  Hit zones span the full
 * card width so tapping anywhere on the row registers. */
static void prompt_choice_tap_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= PROMPT_TAP_MAX) return;
    if (!s_prompt_card_id[0])            return;
    const char *ev = s_prompt_events[idx];
    if (!ev || !ev[0])                    return;
    extern esp_err_t voice_send_widget_action(const char *, const char *, const char *);
    ESP_LOGI(TAG, "prompt tap: card=%s idx=%d event=%s",
             s_prompt_card_id, idx, ev);
    voice_send_widget_action(s_prompt_card_id, ev, NULL);
    show_toast_internal("Sent");
}

static void clear_prompt_hit_zones(void)
{
    for (int i = 0; i < PROMPT_TAP_MAX; i++) {
        if (s_prompt_tap[i]) {
            lv_obj_del(s_prompt_tap[i]);
            s_prompt_tap[i] = NULL;
        }
    }
    s_prompt_card_id[0] = '\0';
    for (int i = 0; i < PROMPT_TAP_MAX; i++) s_prompt_events[i][0] = '\0';
}

static void refresh_prompt_hit_zones(const widget_t *w)
{
    /* Always reset.  Caller re-invokes on every update_status tick. */
    clear_prompt_hit_zones();
    if (!w || !s_now_card) return;
    if (w->type != WIDGET_TYPE_PROMPT) return;
    if (w->choices_count == 0) return;
    int n = w->choices_count;
    if (n > PROMPT_TAP_MAX) n = PROMPT_TAP_MAX;

    snprintf(s_prompt_card_id, sizeof(s_prompt_card_id), "%s", w->card_id);
    for (int i = 0; i < n; i++) {
        snprintf(s_prompt_events[i], sizeof(s_prompt_events[i]),
                 "%s", w->choices[i].event);
    }

    /* Card layout: title row at 30-54 px; each choice row 36 px below,
     * starting at y=54.  Card width = CARD_W.  Make zones 2 px inside
     * the border so they don't swallow the corner radius. */
    const int zone_y0 = 54;
    const int zone_h  = 36;
    for (int i = 0; i < n; i++) {
        lv_obj_t *z = lv_obj_create(s_now_card);
        lv_obj_remove_style_all(z);
        lv_obj_set_pos(z, 2, zone_y0 + i * zone_h);
        lv_obj_set_size(z, CARD_W - 4, zone_h);
        lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(z, 0, 0);
        lv_obj_clear_flag(z, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(z, prompt_choice_tap_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_prompt_tap[i] = z;
    }
    ESP_LOGI(TAG, "prompt hit zones: %d rows for card=%s", n, s_prompt_card_id);
}

/* v4·D Phase 4g widget_media inline image decode.
 *
 * ensure_media_image_widget() lazy-creates the LVGL image object over
 * the live card.  refresh_media_image() is called from
 * ui_home_update_status() whenever the live slot is a MEDIA widget;
 * if the URL changed since the last fetch, it spawns a FreeRTOS task
 * to download + decode via media_cache_fetch, then hops back to the
 * LVGL thread via lv_async_call to bind the decoded buffer.
 *
 * The image sits below the title/body in the tall card, width 620 px
 * centered.  Failures leave the caption fallback visible, so there is
 * always something on screen. */
static void ensure_media_image_widget(void)
{
    if (s_media_img || !s_now_card) return;
    s_media_img = lv_image_create(s_now_card);
    lv_obj_set_size(s_media_img, 620, 96);  /* actual size overwritten on bind */
    lv_obj_set_pos(s_media_img, CARD_PAD, 30);
    lv_obj_add_flag(s_media_img, LV_OBJ_FLAG_HIDDEN);
}

static void media_image_bind_cb(void *arg)
{
    (void)arg;
    if (!s_media_img) return;
    if (s_media_dsc.data == NULL || s_media_dsc.header.w == 0) {
        lv_obj_add_flag(s_media_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Lede is at y=30 in tall mode.  Parent the image ON TOP of the
     * lede region so the text gets covered when decode succeeds. */
    int w = s_media_dsc.header.w;
    int h = s_media_dsc.header.h;
    if (w > 620) w = 620;
    if (h > 100) h = 100;
    lv_obj_set_size(s_media_img, w, h);
    lv_obj_set_pos(s_media_img, (CARD_W - w) / 2, 30);
    lv_image_set_src(s_media_img, &s_media_dsc);
    lv_obj_clear_flag(s_media_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_media_img);
}

static void media_image_clear_cb(void *arg)
{
    (void)arg;
    if (s_media_img) {
        lv_obj_add_flag(s_media_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_media_img, NULL);
    }
    memset(&s_media_dsc, 0, sizeof(s_media_dsc));
}

/* Wave 14 W14-H06: dispatched via tab5_worker; no longer its own
 * FreeRTOS task.  Plain function — early returns instead of
 * vTaskSuspend(NULL), and the trailing suspend is gone. */
static void media_fetch_task(void *pv)
{
    char *url = (char *)pv;
    if (!url) return;
    ESP_LOGI(TAG, "widget_media fetch: %s", url);
    lv_image_dsc_t dsc = {0};
    esp_err_t err = media_cache_fetch(url, &dsc);
    if (err == ESP_OK && dsc.data && dsc.header.w > 0) {
        s_media_dsc = dsc;
        tab5_lv_async_call((lv_async_cb_t)media_image_bind_cb, NULL);
    } else {
        ESP_LOGW(TAG, "widget_media fetch failed (%d) for %s", err, url);
        tab5_lv_async_call((lv_async_cb_t)media_image_clear_cb, NULL);
    }
    free(url);
    s_media_fetch_inflight = false;
}

static void refresh_media_image(const widget_t *w)
{
    if (!w || w->type != WIDGET_TYPE_MEDIA || !w->media_url[0]) {
        /* Non-media -- hide the image widget and reset tracking. */
        s_media_cur_url[0] = '\0';
        tab5_lv_async_call((lv_async_cb_t)media_image_clear_cb, NULL);
        return;
    }
    ensure_media_image_widget();
    if (strncmp(s_media_cur_url, w->media_url, sizeof(s_media_cur_url)) == 0) {
        /* Same URL already fetched/inflight -- no work. */
        return;
    }
    if (s_media_fetch_inflight) {
        /* Let the prior fetch finish; next update_status tick will
         * notice a URL mismatch and kick off the new one. */
        return;
    }
    strncpy(s_media_cur_url, w->media_url, sizeof(s_media_cur_url) - 1);
    s_media_cur_url[sizeof(s_media_cur_url) - 1] = '\0';

    char *copy = strdup(w->media_url);
    if (!copy) return;
    s_media_fetch_inflight = true;
    /* W14-H06: shared worker. */
    esp_err_t enq = tab5_worker_enqueue(media_fetch_task, copy, "widget_media");
    if (enq != ESP_OK) {
        ESP_LOGW(TAG, "widget_media: enqueue failed (%s), dropping", esp_err_to_name(enq));
        free(copy);
        s_media_fetch_inflight = false;
    }
}

/* TT #328 Wave 8 P0 #10 — orb long-press now opens the mode-sheet
 * (same as chip long-press) instead of cycling NVS directly.
 * Pre-Wave-8 there were two parallel mode-control systems:
 *   - orb long-press cycle (vmode 0..4 % VOICE_MODE_COUNT)
 *   - mode-sheet (3-dial: int_tier × voi_tier × aut_tier)
 * Both wrote the same NVS state via different math; user couldn't tell
 * which was source of truth.  K144 mode (4) was unreachable via the
 * dial sheet.  Audit P0 #10 (WCAG 3.3.4 + IA).
 *
 * Collapse: orb long-press = open the mode-sheet.  No NVS write until
 * the user actually picks a tier in the sheet.  Wave-4's undo-pill +
 * snapshot machinery is no longer needed — cancellation is "tap
 * outside the sheet" — so the s_orb_undo_prev_* state + cb are
 * removed.  The s_orb_long_pressed swallow flag stays (still needed
 * to suppress the trailing CLICKED from opening the listening
 * overlay on long-press release). */
static void orb_long_press_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    s_orb_long_pressed = true;
    extern void ui_mode_sheet_show(void);
    ui_mode_sheet_show();
}

static void mode_chip_long_press_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    /* v4·D Sovereign Halo: open the triple-dial sheet instead of
     * cycling. The sheet persists tier changes + fires config_update
     * directly, so nothing else to do here. */
    /* TT #328 Wave 9 — user has discovered the chip; suppress the
     * pending hint toast so we don't tell them what they just did. */
    tab5_settings_set_mode_hint_seen(true);
    extern void ui_mode_sheet_show(void);
    ui_mode_sheet_show();
}

/* TT #370 — mode-pill TAP behaviour.  When current mode is K144
 * (vmode=4) or SOLO (vmode=5), tap cycles between them — both are
 * Dragon-free tiers and a fast tap-to-flip is the design intent of
 * the standalone-SKU mode pill (see spec).  For other modes, fall
 * through to the existing 3-dial sheet UX (long-press behaviour).
 *
 * No config_update fires for vmode 4↔5 — neither tier involves
 * Dragon, so Dragon doesn't need to know.  NVS write + UI refresh
 * is the entire transition. */
static void mode_chip_click_cb(lv_event_t *e) {
   (void)e;
   if (any_overlay_visible()) return;
   uint8_t cur = tab5_settings_get_voice_mode();
   if (cur == VMODE_LOCAL_ONBOARD || cur == VMODE_SOLO_DIRECT) {
      uint8_t next = (cur == VMODE_LOCAL_ONBOARD) ? VMODE_SOLO_DIRECT : VMODE_LOCAL_ONBOARD;
      tab5_settings_set_voice_mode(next);
      update_mode_ui(next);
      ESP_LOGI(TAG, "mode pill: cycled %u -> %u", cur, next);
      return;
   }
   /* Default: open the 3-dial mode-sheet (was the long-press UX). */
   tab5_settings_set_mode_hint_seen(true);
   extern void ui_mode_sheet_show(void);
   ui_mode_sheet_show();
}

/* U24 (#206): set by now_card_long_press_cb when a long-press just
 * dismissed the live widget — swallows the trailing CLICKED event
 * that LVGL still emits on release so the dismiss doesn't accidentally
 * also navigate to Agents. */
static bool s_now_card_long_pressed = false;

static void now_card_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_now_card_long_pressed) {
        s_now_card_long_pressed = false;
        return;
    }
    if (any_overlay_visible()) return;

    /* When there's no agent activity to show, the navigation to the
     * Agents overlay just lands the user on the empty-state which
     * reads as "dummy page" — they expect substantive content from
     * tapping the now-card.  Surface a hint via toast instead so the
     * tap isn't silent and the surface stays honest about what's
     * available.  Once any tool fires the count flips and the tap
     * navigates as before. */
    extern int tool_log_count(void);
    if (tool_log_count() == 0) {
       ESP_LOGI(TAG, "now-card tapped — no agent activity yet, showing toast");
       show_toast_internal("Nothing running yet. Try voice.");
       return;
    }
    ESP_LOGI(TAG, "now-card tapped -> Agents");
    ui_agents_show();
}

static void now_card_long_press_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    widget_t *live_w = widget_store_live_active();
    if (!live_w) {
        /* No live widget → long-press is a no-op (don't show a toast,
         * don't suppress the trailing CLICKED — let it fall through to
         * the Agents-open path). */
        return;
    }
    /* Stash card_id before we mutate the store. */
    char card_id[WIDGET_ID_LEN];
    strncpy(card_id, live_w->card_id, sizeof(card_id) - 1);
    card_id[sizeof(card_id) - 1] = '\0';

    s_now_card_long_pressed = true;

    /* Notify Dragon best-effort.  Skill receives widget_action with
     * event="dismiss" and can stop re-emitting; if Dragon is offline
     * the dismiss is still local-immediate which is the right UX. */
    voice_send_widget_action(card_id, "dismiss", NULL);

    /* Local-store dismiss → ui_home_update_status will re-render the
     * empty state (or surface the next-priority queued widget if any). */
    widget_store_dismiss(card_id);
    ui_home_update_status();

    ESP_LOGI(TAG, "now-card long-press → dismissed widget %s", card_id);
    show_toast_internal("Widget dismissed");
}

static void sys_click_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    ESP_LOGI(TAG, "sys label tapped -> Memory");
    ui_memory_show();
}

/* U19 (#206): rail callbacks (rail_home_cb / rail_threads_cb /
 * rail_notes_cb / rail_settings_cb) and their two async trampolines
 * (async_open_chat / async_open_notes_rail) were exclusive to the
 * deleted rail and had no other callers — removed with the rail. */

static bool any_overlay_visible(void)
{
    extern bool ui_notes_is_visible(void);
    extern bool ui_settings_is_visible(void);
    extern bool ui_sessions_is_visible(void);
    if (ui_chat_is_active())      return true;
    if (ui_agents_is_visible())   return true;
    if (ui_memory_is_visible())   return true;
    if (ui_focus_is_visible())    return true;
    if (ui_notes_is_visible())    return true;
    if (ui_settings_is_visible()) return true;
    if (ui_sessions_is_visible()) return true;
    return false;
}

/* Async open callbacks — deferred so the originating gesture fully dispatches
   before we create the overlay. Prevents swipe-right on home from opening
   Notes, then bubbling to Notes' own swipe-right=back and tearing it down. */
static void async_open_focus(void *arg)    { (void)arg; ui_focus_show(); }
static void async_open_notes(void *arg)
{
    (void)arg;
    extern lv_obj_t *ui_notes_create(void);
    ui_notes_create();
}
static void async_open_settings(void *arg)
{
    (void)arg;
    extern lv_obj_t *ui_settings_create(void);
    ui_settings_create();
}

static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    if (any_overlay_visible()) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    switch (dir) {
        case LV_DIR_TOP:    tab5_lv_async_call(async_open_focus,    NULL); break;
        case LV_DIR_RIGHT:  tab5_lv_async_call(async_open_notes,    NULL); break;
        case LV_DIR_LEFT:   tab5_lv_async_call(async_open_settings, NULL); break;
        default: break;
    }
}

/* ── Toast ───────────────────────────────────────────────────── */

/* Phase 2 of #42: toasts slide in from +50 px below their resting
 * position via SPRING_SNAPPY rather than appearing instantly.  The
 * lifetime + replacement plumbing also got a bit safer along the way:
 * the prior replacement path (`if (s_toast) { lv_obj_del; s_toast = NULL; }`)
 * leaked the previous ctx + lifetime timer.  Now both ctx pointers are
 * tracked via s_toast_ctx and torn down together via toast_ctx_destroy. */
#define TOAST_FINAL_Y 200 /* y-offset from CENTER when settled */
#define TOAST_START_Y 250 /* spawn 50 px below final (slides up) */
/* TT #328 Wave 3 — baseline lifetime per tone, plus 60 ms/char read time.
 * Capped at 8 s so a runaway long error doesn't lock the toast forever. */
#define TOAST_LIFETIME_INFO_MS 2200
#define TOAST_LIFETIME_WARN_MS 3500
#define TOAST_LIFETIME_ERROR_MS 5000
#define TOAST_LIFETIME_INCOMING_MS 3000 /* W7-E.1 — slightly longer than INFO so sender + preview both register */
#define TOAST_BORDER_INCOMING 0x6B5BFF  /* W7-E.1 — blue-violet per PLAN §4.1 */
#define TOAST_LIFETIME_PER_CHAR_MS 60
#define TOAST_LIFETIME_CAP_MS 8000
#define TOAST_FRAME_MS 16 /* ~60 fps */

typedef struct {
   lv_timer_t *life_t; /* one-shot lifetime → toast_remove_cb */
   lv_timer_t *anim_t; /* per-frame slide cb; deleted on convergence */
   lv_obj_t *obj;      /* the toast itself */
   spring_anim_t spring;
} toast_ctx_t;

static uint32_t toast_lifetime_for(ui_toast_tone_t tone, const char *text) {
   uint32_t base = TOAST_LIFETIME_INFO_MS;
   if (tone == UI_TOAST_WARN) base = TOAST_LIFETIME_WARN_MS;
   if (tone == UI_TOAST_ERROR) base = TOAST_LIFETIME_ERROR_MS;
   if (tone == UI_TOAST_INCOMING) base = TOAST_LIFETIME_INCOMING_MS;
   uint32_t scale = text ? (uint32_t)strlen(text) * TOAST_LIFETIME_PER_CHAR_MS : 0;
   uint32_t total = base + scale;
   if (total > TOAST_LIFETIME_CAP_MS) total = TOAST_LIFETIME_CAP_MS;
   return total;
}

static uint32_t toast_border_for(ui_toast_tone_t tone) {
   if (tone == UI_TOAST_WARN) return TH_MODE_HYBRID;            /* yellow */
   if (tone == UI_TOAST_ERROR) return TH_STATUS_RED;            /* rose-red */
   if (tone == UI_TOAST_INCOMING) return TOAST_BORDER_INCOMING; /* W7-E.1 blue-violet */
   return TH_CARD_BORDER;                                       /* legacy subtle */
}

/* Track the currently-displayed toast so a back-to-back show_toast can
 * tear down BOTH timers + the obj before spawning the replacement. */
static toast_ctx_t *s_toast_ctx = NULL;

/* TT #328 Wave 3 — persistent error banner.  Sits at the top of the home
 * screen (just below the sys-label area) until the user taps it (firing
 * the dismiss callback if any) OR until ui_home_clear_error_banner()
 * fires from the recovery code path.  Single banner — a second show
 * replaces the first.  Lives on lv_layer_top so it survives screen
 * navigations (banner is global, not home-specific). */
static lv_obj_t *s_err_banner = NULL;
static ui_banner_dismiss_cb_t s_err_banner_cb = NULL;

static void toast_ctx_destroy(toast_ctx_t *ctx) {
   if (!ctx) return;
   if (ctx->anim_t) {
      lv_timer_delete(ctx->anim_t);
      ctx->anim_t = NULL;
   }
   if (ctx->life_t) {
      lv_timer_delete(ctx->life_t);
      ctx->life_t = NULL;
   }
   if (ctx->obj) {
      lv_obj_del(ctx->obj);
      ctx->obj = NULL;
   }
   free(ctx);
}

static void toast_anim_cb(lv_timer_t *t) {
   toast_ctx_t *ctx = (toast_ctx_t *)lv_timer_get_user_data(t);
   if (!ctx || !ctx->obj) {
      lv_timer_delete(t);
      return;
   }
   float y = spring_anim_update(&ctx->spring, TOAST_FRAME_MS / 1000.0f);
   lv_obj_align(ctx->obj, LV_ALIGN_CENTER, 0, (int32_t)y);
   if (spring_anim_done(&ctx->spring)) {
      ctx->anim_t = NULL; /* mark before deleting so destroy is idempotent */
      lv_timer_delete(t);
   }
}

static void toast_remove_cb(lv_timer_t *t) {
   toast_ctx_t *ctx = (toast_ctx_t *)lv_timer_get_user_data(t);
   if (!ctx) {
      lv_timer_delete(t);
      return;
   }
   /* The lifetime timer is the one that just fired — null it before
    * destroy so toast_ctx_destroy doesn't try to delete a timer we're
    * already inside the callback of. */
   ctx->life_t = NULL;
   if (s_toast == ctx->obj) s_toast = NULL;
   if (s_toast_ctx == ctx) s_toast_ctx = NULL;
   toast_ctx_destroy(ctx);
   lv_timer_delete(t);
}

static void show_toast_internal_tone(const char *text, ui_toast_tone_t tone) {
   if (!text) return;
   /* Replace any in-flight toast: tear down ctx + both timers + obj,
    * not just the obj (which the pre-#42 path leaked). */
   if (s_toast_ctx) {
      toast_ctx_destroy(s_toast_ctx);
      s_toast_ctx = NULL;
      s_toast = NULL;
   } else if (s_toast) {
      /* Defensive: an obj somehow exists without a ctx (shouldn't
       * happen post-Phase-2, but ui_home_destroy may have nulled
       * s_toast_ctx independently).  Free the obj to keep state
       * consistent. */
      lv_obj_del(s_toast);
      s_toast = NULL;
   }

   lv_obj_t *t = lv_obj_create(lv_layer_top());
   lv_obj_remove_style_all(t);
   lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
   lv_obj_set_style_bg_color(t, lv_color_hex(TH_CARD_ELEVATED), 0);
   lv_obj_set_style_bg_opa(t, 240, 0);
   lv_obj_set_style_radius(t, 18, 0);
   lv_obj_set_style_pad_hor(t, 22, 0);
   lv_obj_set_style_pad_ver(t, 14, 0);
   /* TT #328 Wave 3 — tone-aware border colour + width.  Warn/Error get a
    * 2 px border in the matching mode-tone hue so they stand out without
    * the full toast bg getting tinted (keeps text contrast unchanged). */
   uint32_t border = toast_border_for(tone);
   /* W7-E.1 — INCOMING falls into the "not INFO" branch, gets the 2 px border. */
   int bw = (tone == UI_TOAST_INFO) ? 1 : 2;
   lv_obj_set_style_border_width(t, bw, 0);
   lv_obj_set_style_border_color(t, lv_color_hex(border), 0);

   lv_obj_t *lbl = lv_label_create(t);
   lv_label_set_text(lbl, text);
   lv_obj_set_style_text_font(lbl, FONT_SECONDARY, 0);
   lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_PRIMARY), 0);
   lv_obj_set_style_text_letter_space(lbl, 1, 0);

   /* Spawn at the start position so the first frame paints correctly
    * even if the anim timer is delayed by a busy LVGL queue. */
   lv_obj_align(t, LV_ALIGN_CENTER, 0, TOAST_START_Y);
   s_toast = t;

   toast_ctx_t *ctx = (toast_ctx_t *)calloc(1, sizeof(*ctx));
   if (!ctx) return; /* OOM — toast still renders, just won't auto-remove. */
   ctx->obj = t;
   spring_anim_init(&ctx->spring, SPRING_SNAPPY);
   spring_anim_retarget(&ctx->spring, (float)TOAST_START_Y, (float)TOAST_FINAL_Y, 0.0f);
   ctx->anim_t = lv_timer_create(toast_anim_cb, TOAST_FRAME_MS, ctx);
   ctx->life_t = lv_timer_create(toast_remove_cb, toast_lifetime_for(tone, text), ctx);
   if (ctx->life_t) lv_timer_set_repeat_count(ctx->life_t, 1);
   s_toast_ctx = ctx;
}

static void show_toast_internal(const char *text) { show_toast_internal_tone(text, UI_TOAST_INFO); }

/* ── Public API ──────────────────────────────────────────────── */

void ui_home_destroy(void)
{
    if (s_refresh_timer) { lv_timer_delete(s_refresh_timer); s_refresh_timer = NULL; }
    /* Phase 2 (#42): tear down any in-flight toast ctx so its anim
     * timer doesn't fire on a freed obj after the screen is gone. */
    if (s_toast_ctx) {
       toast_ctx_destroy(s_toast_ctx);
       s_toast_ctx = NULL;
    }
    /* Phase 4 (#42): tear down the mode-dot pulse timer so it doesn't
     * fire on a freed s_mode_dot after the screen is gone.  Reset the
     * first-paint flag so a re-created home doesn't mistakenly skip
     * the pulse on its first real cycle. */
    if (s_dot_anim_t) {
       lv_timer_delete(s_dot_anim_t);
       s_dot_anim_t = NULL;
    }
    s_dot_size_current = 0.0f;
    s_dot_first_paint = true;
    /* W7-E.2: tear down any in-flight channel-now overlay so its
     * obj on lv_layer_top doesn't outlive the home screen. */
    if (s_channel_card) {
       lv_obj_del(s_channel_card);
       s_channel_card = NULL;
    }
    if (s_screen) { lv_obj_del(s_screen); s_screen = NULL; }
    s_sys_dot = s_sys_label = s_time_label = NULL;
    s_halo_outer = s_halo_inner = NULL;
    s_ring_outer = s_ring_mid = s_ring_inner = NULL;
    s_orb = NULL;
    s_state_word = s_greet_line = NULL;
    s_mode_chip = s_mode_dot = s_mode_name = s_mode_sub = NULL;
    s_now_card = s_now_accent = s_now_kicker = s_now_lede = NULL;
    s_say_pill = s_say_mic = s_say_label_main = s_say_label_sub = NULL;
    s_toast = NULL;
}

lv_obj_t *ui_home_get_screen(void) { return s_screen; }

void ui_home_go_home(void)
{
    if (ui_chat_is_active()) ui_chat_hide();
    /* Secondary screens (camera, files) are independent lv_screen objects
     * that replace the active screen.  Hiding an overlay like chat is not
     * enough when the user is on one of those -- we also have to load the
     * home screen back onto the display.  Without this, /navigate?screen=home
     * from camera just hides invisible overlays while camera keeps drawing. */
    if (s_screen && lv_screen_active() != s_screen) {
        lv_screen_load(s_screen);
    }
    /* Wave 4 UX: drop a stale idle voice overlay so the user sees home. */
    extern void ui_voice_dismiss_if_idle(void);
    ui_voice_dismiss_if_idle();
    /* U16 (#206): the floating keyboard trigger sits on lv_layer_top()
     * at (640, 1136) — directly on top of home's menu chip
     * (612..668, 1160..1216).  Home has no text-input use case (the
     * menu chip + orb-mic are the only affordances), so the trigger
     * eats taps that should open the nav sheet.  Hide it on home;
     * other screens (notes, wifi, settings textareas, etc.) restore
     * it via ui_keyboard_set_trigger_visible(true). */
    extern void ui_keyboard_set_trigger_visible(bool);
    ui_keyboard_set_trigger_visible(false);
}

/* TT #328 Wave 7 — ui_home_get_tileview / ui_home_get_tile removed.
 * Both were already neutered (return NULL / return s_screen on page=0)
 * since v4·C retired the 4-page tileview in favour of the single-screen
 * Ambient Canvas (CLAUDE.md "4-page tileview" line was stale).  Deleting
 * the hollow shells now that no caller has used them since #155. */

void ui_home_nav_settings(void)
{
    /* Same fix as ui_home_go_home: make sure the home screen is actually
     * displayed before we overlay Settings on it.  Otherwise Settings
     * parents to a hidden home screen while camera/files keeps rendering. */
    if (s_screen && lv_screen_active() != s_screen) {
        lv_screen_load(s_screen);
    }
    extern void ui_voice_dismiss_if_idle(void);
    ui_voice_dismiss_if_idle();
    ui_settings_create();
}

void ui_home_refresh_mode_badge(void)
{
    uint8_t m = tab5_settings_get_voice_mode();
    update_mode_ui(m);
}

void ui_home_show_toast(const char *text) { show_toast_internal(text); }
void ui_home_show_toast_ex(const char *text, ui_toast_tone_t tone) { show_toast_internal_tone(text, tone); }

/* ── TT #328 Wave 9 — first-launch mode-chip hint ─────────────── */

static void mode_hint_fire_cb(lv_timer_t *t) {
   lv_timer_delete(t);
   /* Re-check: another caller may have racily marked it seen
    * (e.g. user tapped the chip in the 6-s window — we don't want a
    * "tap the chip" toast firing AFTER they already used it). */
   if (tab5_settings_mode_hint_seen()) return;
   /* Don't intrude on overlays — the user is doing something else. */
   if (any_overlay_visible()) return;
   show_toast_internal_tone("Tip: tap the mode chip to switch tier", UI_TOAST_INFO);
   tab5_settings_set_mode_hint_seen(true);
}

/* ── TT #328 Wave 4 — undo-pill toast ─────────────────────────── */

static ui_undo_cb_t s_undo_cb = NULL;
static lv_obj_t *s_undo_obj = NULL;
static lv_obj_t *s_undo_countdown = NULL;
static lv_timer_t *s_undo_life_t = NULL; /* full-lifetime expiry */
static lv_timer_t *s_undo_tick_t = NULL; /* 1Hz countdown updater */
static int s_undo_secs_left = 0;
static char s_undo_label_buf[48];

static void undo_destroy(void) {
   if (s_undo_tick_t) {
      lv_timer_delete(s_undo_tick_t);
      s_undo_tick_t = NULL;
   }
   if (s_undo_life_t) {
      lv_timer_delete(s_undo_life_t);
      s_undo_life_t = NULL;
   }
   if (s_undo_obj) {
      lv_obj_del(s_undo_obj);
      s_undo_obj = NULL;
   }
   s_undo_countdown = NULL;
   s_undo_cb = NULL;
   s_undo_label_buf[0] = '\0';
}

static void undo_expired_cb(lv_timer_t *t) {
   (void)t;
   /* Lifetime fired without a tap — finalize without invoking the cb. */
   s_undo_life_t = NULL;
   undo_destroy();
}

static void undo_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_undo_countdown) return;
   if (s_undo_secs_left > 0) s_undo_secs_left--;
   char buf[80];
   snprintf(buf, sizeof(buf), "%s  \xc2\xb7  Undo (%ds)", s_undo_label_buf, s_undo_secs_left);
   lv_label_set_text(s_undo_countdown, buf);
}

static void undo_click_cb(lv_event_t *e) {
   (void)e;
   ui_undo_cb_t cb = s_undo_cb;
   undo_destroy();
   if (cb) cb();
}

void ui_home_show_undo_toast(const char *label, int seconds, ui_undo_cb_t undo_cb) {
   if (!label || !undo_cb || seconds <= 0) return;
   /* Replace any in-flight undo toast WITHOUT firing its cb (user moved on). */
   undo_destroy();
   /* Replace any plain toast too — they share the same screen real estate. */
   if (s_toast_ctx) {
      toast_ctx_destroy(s_toast_ctx);
      s_toast_ctx = NULL;
      s_toast = NULL;
   }

   if (seconds > 30) seconds = 30;
   strncpy(s_undo_label_buf, label, sizeof(s_undo_label_buf) - 1);
   s_undo_label_buf[sizeof(s_undo_label_buf) - 1] = '\0';

   lv_obj_t *t = lv_obj_create(lv_layer_top());
   lv_obj_remove_style_all(t);
   lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
   lv_obj_set_style_bg_color(t, lv_color_hex(TH_CARD_ELEVATED), 0);
   lv_obj_set_style_bg_opa(t, 240, 0);
   lv_obj_set_style_radius(t, 18, 0);
   lv_obj_set_style_pad_hor(t, 22, 0);
   lv_obj_set_style_pad_ver(t, 14, 0);
   lv_obj_set_style_border_width(t, 2, 0);
   lv_obj_set_style_border_color(t, lv_color_hex(TH_AMBER), 0);
   lv_obj_align(t, LV_ALIGN_CENTER, 0, TOAST_FINAL_Y);

   s_undo_countdown = lv_label_create(t);
   lv_obj_set_style_text_font(s_undo_countdown, FONT_SECONDARY, 0);
   lv_obj_set_style_text_color(s_undo_countdown, lv_color_hex(TH_TEXT_PRIMARY), 0);
   lv_obj_set_style_text_letter_space(s_undo_countdown, 1, 0);
   char buf[80];
   snprintf(buf, sizeof(buf), "%s  \xc2\xb7  Undo (%ds)", label, seconds);
   lv_label_set_text(s_undo_countdown, buf);

   lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
   lv_obj_add_event_cb(t, undo_click_cb, LV_EVENT_CLICKED, NULL);

   s_undo_obj = t;
   s_undo_cb = undo_cb;
   s_undo_secs_left = seconds;

   s_undo_life_t = lv_timer_create(undo_expired_cb, seconds * 1000, NULL);
   if (s_undo_life_t) lv_timer_set_repeat_count(s_undo_life_t, 1);
   s_undo_tick_t = lv_timer_create(undo_tick_cb, 1000, NULL);
}

/* ── TT #328 Wave 3 — persistent error banner ─────────────────── */

static void err_banner_destroy(void) {
   if (s_err_banner) {
      lv_obj_del(s_err_banner);
      s_err_banner = NULL;
   }
   s_err_banner_cb = NULL;
}

static void err_banner_click_cb(lv_event_t *e) {
   (void)e;
   ui_banner_dismiss_cb_t cb = s_err_banner_cb;
   err_banner_destroy();
   if (cb) cb();
}

void ui_home_show_error_banner(const char *text, ui_banner_dismiss_cb_t dismiss_cb) {
   if (!text) return;
   /* Replace any existing banner. */
   err_banner_destroy();

   /* Layout: full-width strip at top of lv_layer_top.  Sits above all
    * overlays so the user sees it from any screen.  Rose accent border
    * so it reads as a fault state at a glance.
    *
    * Wave 10 follow-up: was 56 px tall, which covered the top half of
    * the chat header's 60-px back button (chat header lives at y=0..96,
    * back button center at y=48).  Shrunk to 32 px so the banner sits
    * above the back button's hit area; the K144-unavailable warning
    * still reads cleanly at FONT_SECONDARY 18 px with 7 px ver padding. */
   lv_obj_t *b = lv_obj_create(lv_layer_top());
   lv_obj_remove_style_all(b);
   lv_obj_set_size(b, 720, 32);
   lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 0);
   lv_obj_set_style_bg_color(b, lv_color_hex(TH_CARD_ELEVATED), 0);
   lv_obj_set_style_bg_opa(b, 240, 0);
   lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
   lv_obj_set_style_border_width(b, 2, 0);
   lv_obj_set_style_border_color(b, lv_color_hex(TH_STATUS_RED), 0);
   lv_obj_set_style_pad_hor(b, 22, 0);
   lv_obj_set_style_pad_ver(b, 7, 0);

   lv_obj_t *lbl = lv_label_create(b);
   lv_label_set_text(lbl, text);
   lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
   lv_obj_set_width(lbl, 600);
   lv_obj_set_style_text_font(lbl, FONT_SECONDARY, 0);
   lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_PRIMARY), 0);
   lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

   if (dismiss_cb) {
      /* Tappable row + dismiss hint */
      lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(b, err_banner_click_cb, LV_EVENT_CLICKED, NULL);
      lv_obj_t *hint = lv_label_create(b);
      lv_label_set_text(hint, "Tap to dismiss");
      lv_obj_set_style_text_font(hint, FONT_SMALL, 0);
      lv_obj_set_style_text_color(hint, lv_color_hex(TH_TEXT_DIM), 0);
      lv_obj_align(hint, LV_ALIGN_RIGHT_MID, 0, 0);
   }

   s_err_banner = b;
   s_err_banner_cb = dismiss_cb;
}

void ui_home_clear_error_banner(void) { err_banner_destroy(); }

/* ── W7-E.2: channel-message now-card overlay ─────────────────────── */
/* s_channel_card is declared at file scope near the other now-card statics. */

static void channel_card_destroy(void) {
   if (s_channel_card) {
      lv_obj_del(s_channel_card);
      s_channel_card = NULL;
   }
}

static void channel_card_dismiss_cb(lv_event_t *e) {
   (void)e;
   tab5_debug_obs_event("ui.notif.now", "dismiss");
   channel_card_destroy();
}

static void channel_card_reply_cb(lv_event_t *e) {
   (void)e;
   tab5_debug_obs_event("ui.notif.now", "reply_stub");
   channel_card_destroy();
   show_toast_internal_tone("Reply flow coming in W7-E.4", UI_TOAST_INFO);
}

static void channel_card_snooze_cb(lv_event_t *e) {
   (void)e;
   tab5_debug_obs_event("ui.notif.now", "snooze");
   channel_card_destroy();
   /* W7-E.3: real snooze ring — entry stays in PSRAM for 15 min, then
    * re-fires through the standard routing path.  Forward-declared
    * extern so ui_home doesn't pull the full ui_notification.h. */
   extern void ui_notification_snooze_current(void);
   ui_notification_snooze_current();
}

/* Build one of the three action pills on the card.  Returns the pill obj. */
static lv_obj_t *channel_card_make_button(lv_obj_t *parent, int x, int y, int w, const char *text, lv_event_cb_t cb) {
   lv_obj_t *btn = lv_obj_create(parent);
   lv_obj_remove_style_all(btn);
   lv_obj_set_size(btn, w, 48);
   lv_obj_set_pos(btn, x, y);
   lv_obj_set_style_bg_color(btn, lv_color_hex(TH_CARD), 0);
   lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(btn, 24, 0);
   lv_obj_set_style_border_width(btn, 1, 0);
   lv_obj_set_style_border_color(btn, lv_color_hex(TH_MODE_CLAW), 0);
   lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
   lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

   lv_obj_t *lbl = lv_label_create(btn);
   lv_label_set_text(lbl, text);
   lv_obj_set_style_text_font(lbl, FONT_SMALL, 0);
   lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_PRIMARY), 0);
   lv_obj_set_style_text_letter_space(lbl, 2, 0);
   lv_obj_center(lbl);
   return btn;
}

void ui_home_show_channel_now(const char *channel, const char *sender, const char *preview) {
   /* Replace any in-flight now-card.  W7-E.3 will stack into a queue
    * with a "1 more" badge; v0 just replaces. */
   channel_card_destroy();

   const char *ch_safe = (channel && channel[0]) ? channel : "?";
   const char *sender_safe = (sender && sender[0]) ? sender : "Someone";
   const char *preview_safe = (preview && preview[0]) ? preview : "(no preview)";

   /* Card: 640 × 260, centered, rose border per PLAN §4.1. */
   lv_obj_t *c = lv_obj_create(lv_layer_top());
   lv_obj_remove_style_all(c);
   lv_obj_set_size(c, 640, 260);
   lv_obj_align(c, LV_ALIGN_CENTER, 0, -120);
   lv_obj_set_style_bg_color(c, lv_color_hex(TH_CARD_ELEVATED), 0);
   lv_obj_set_style_bg_opa(c, 245, 0);
   lv_obj_set_style_radius(c, 24, 0);
   lv_obj_set_style_border_width(c, 2, 0);
   lv_obj_set_style_border_color(c, lv_color_hex(TH_MODE_CLAW), 0);
   lv_obj_set_style_pad_all(c, 22, 0);
   lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

   /* Kicker row: [ch]  sender */
   char kicker[96];
   snprintf(kicker, sizeof(kicker), "[%.12s]  %.60s", ch_safe, sender_safe);
   lv_obj_t *kick = lv_label_create(c);
   lv_label_set_text(kick, kicker);
   lv_label_set_long_mode(kick, LV_LABEL_LONG_DOT);
   lv_obj_set_width(kick, 596);
   lv_obj_set_pos(kick, 0, 0);
   lv_obj_set_style_text_font(kick, FONT_HEADING, 0);
   lv_obj_set_style_text_color(kick, lv_color_hex(TH_TEXT_PRIMARY), 0);

   /* Multi-line preview body */
   lv_obj_t *body = lv_label_create(c);
   lv_label_set_text(body, preview_safe);
   lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
   lv_obj_set_width(body, 596);
   lv_obj_set_pos(body, 0, 48);
   lv_obj_set_style_text_font(body, FONT_BODY, 0);
   lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT_BODY), 0);
   lv_obj_set_style_max_height(body, 96, 0);

   /* Button row at the bottom of the card (3 pills, 180 px wide each). */
   const int btn_w = 180;
   const int btn_y = 260 - 22 - 48 - 22;  /* card_h - pad - btn_h - extra-margin */
   const int gap = (596 - btn_w * 3) / 2; /* even spacing within 596 px inner */
   channel_card_make_button(c, 0, btn_y, btn_w, "REPLY", channel_card_reply_cb);
   channel_card_make_button(c, btn_w + gap, btn_y, btn_w, "SNOOZE", channel_card_snooze_cb);
   channel_card_make_button(c, (btn_w + gap) * 2, btn_y, btn_w, "DISMISS", channel_card_dismiss_cb);

   s_channel_card = c;
}

void ui_home_hide_channel_now(void) { channel_card_destroy(); }

void ui_home_set_error_banner_visible(bool visible) {
   if (!s_err_banner) return;
   if (visible)
      lv_obj_clear_flag(s_err_banner, LV_OBJ_FLAG_HIDDEN);
   else
      lv_obj_add_flag(s_err_banner, LV_OBJ_FLAG_HIDDEN);
}

/* Wave 7 F5 crash surface: paint the orb rose for ~2.5 s so a mid-turn
 * Dragon drop has a visual signal beyond the toast. Restores the
 * mode-default orb paint afterwards. Runs on the LVGL thread. */
static void orb_pulse_revert_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    if (s_orb) orb_paint_for_mode(s_badge_mode);
}

void ui_home_pulse_orb_alert(void)
{
    if (!s_orb) return;
    orb_paint_for_tone(WIDGET_TONE_URGENT);
    lv_timer_t *rev = lv_timer_create(orb_pulse_revert_cb, 2500, NULL);
    if (rev) lv_timer_set_repeat_count(rev, 1);
}

/* Voice-state-driven state-word refresh — marshalled onto the LVGL thread. */
static void sys_label_refresh_async_cb(void *arg)
{
    (void)arg;
    ui_home_update_status();
}

void ui_home_refresh_sys_label(void)
{
    tab5_lv_async_call(sys_label_refresh_async_cb, NULL);
}
