/**
 * @file ui_orb.c
 * @brief Home-screen orb implementation — see ui_orb.h for the API contract
 *        and docs/superpowers/specs/2026-05-14-orb-redesign-design.md for
 *        the design rationale + state-by-state motion budgets.
 *
 * Step 3 (this file's current scope): owns the lit-sphere body, the
 * circadian palette, and the orb paint helpers.  Click + long-press
 * remain wired by ui_home.c via ui_orb_get_body().
 *
 * Steps 4-7 will layer in motion (tilt-spec, bloom, comet) and the
 * 4-state machine arm/disarm logic.  Step 8 wires presence wake.
 */

#include "ui_orb.h"

#include <stdint.h>
#include <stdlib.h> /* malloc/free for presence-call marshalling */
#include <time.h>

#include "config.h" /* FONT_CAPTION for pipeline-state caption (PR 2) */
#include "esp_log.h"
#include "esp_timer.h"        /* esp_timer_get_time for RECORDING caption timer (PR 2) */
#include "imu.h"              /* tab5_imu_read for tilt-driven specular drift */
#include "lvgl.h"
#include "ui_core.h"          /* tab5_lv_async_call for cross-thread repaints */
#include "voice.h"            /* voice_get_current_rms for the LISTENING bloom */
#include "voice_dictation.h"       /* pipeline-state types (PR 2) */
#include "voice_dictation_lvgl.h"  /* LVGL-marshalled subscriber (PR 2) */
#include "widget.h"                /* widget_tone_t for paint_for_tone */

static const char *TAG = "ui_orb";

/* ── Sizing / layout constants ──────────────────────────────────────── */

#define ORB_SIZE 180 /* body diameter — matches v4·C Ambient Canvas */

/* ── Specular highlight (tilt-driven) ───────────────────────────────── */

/* Width × height of the soft cream-white ellipse that drifts inside the
 * orb to suggest a lit surface.  Clipped by s_body's radius-full corner.
 * Larger than #511's attempt (which was too small + had no working IMU
 * behind it) so the drift is unambiguously visible. */
#define ORB_SPEC_W 70
#define ORB_SPEC_H 44

/* Resting position: upper-left quadrant.  cx-shift = -18 px, cy-shift =
 * 16 px from top.  These should mirror the eye's expectation of "lit
 * from above + slightly to the left." */
#define ORB_SPEC_REST_X ((ORB_SIZE / 2) - (ORB_SPEC_W / 2) - 18)
#define ORB_SPEC_REST_Y 16

/* Pixels of highlight movement per g of gravity.  At ±1 g (device flat
 * to vertical edge) we want roughly ±22 px so the drift is visible but
 * never approaches the orb's edge (radius is ORB_SIZE/2 = 90, so 22 px
 * stays well inside the clip region even from the rest position).  */
#define ORB_TILT_K_PX_PER_G 22.0f

/* EMA smoothing factor for accel input.  0.12 ≈ 1 s settling time at
 * 20 Hz polling — feels responsive without being noisy. */
#define ORB_TILT_ALPHA 0.12f

/* Hard clamp on EMA output (px).  Belt + suspenders since k_xy * 1 g
 * already lands at 22 px; this guards against transient >1 g spikes. */
#define ORB_TILT_LIMIT 24

/* Poll period for the IMU/highlight tick (ms). */
#define ORB_TILT_PERIOD_MS 50 /* 20 Hz */

/* ── Skill-rim comet (PROCESSING state) ──────────────────────────────── */

/* A single bright disc orbits just outside the orb's body during
 * PROCESSING (LLM/tool active).  Replaces the ripple-shower from #501
 * — one steady motion reads as "thinking" without the noise of N
 * concurrent ripples for N tools. */
#define ORB_COMET_W 18
#define ORB_COMET_H 18
#define ORB_COMET_RADIUS_PX 102  /* just outside the body radius of 90 */
#define ORB_COMET_PERIOD_MS 3600 /* one orbit per 3.6 s */
#define ORB_COMET_FADE_MS 240    /* fade-in / fade-out duration */
#define ORB_COMET_PEAK_OPA 230   /* opacity at full visibility */

/* ── Listening lean ──────────────────────────────────────────────────── */

/* When LISTENING engages, the specular's REST point eases from the
 * upper-left IDLE position toward the screen-center (+down, +right) so
 * the orb appears to "turn to face" the user.  Tilt drift continues on
 * top — the lean changes the BASE point the drift orbits. */
#define ORB_LEAN_REST_X ((ORB_SIZE / 2) - (ORB_SPEC_W / 2) - 4)
#define ORB_LEAN_REST_Y 30
#define ORB_LEAN_ANIM_MS 220

/* ── Voice bloom ─────────────────────────────────────────────────────── */

/* Soft amber halo sibling-behind the orb body.  When LISTENING,
 * bg_opa is driven by mic RMS so the halo visibly pulses with the
 * user's voice volume.  At idle (and in all other states) it stays
 * at opa 0.  Color tracks circadian "top" stop so dawn-bloom looks
 * different from night-bloom; intensity is per-state. */
#define ORB_HALO_DIAM 260
#define ORB_HALO_OPA_FLOOR 24   /* always-on minimum during LISTENING */
#define ORB_HALO_OPA_CEILING 90 /* peak at full RMS */
#define ORB_BLOOM_PERIOD_MS 100 /* 10 Hz */
#define ORB_BLOOM_ALPHA 0.30f   /* EMA smoothing for RMS */

/* ── Speaking halo (SPEAKING state) ──────────────────────────────────── */

/* In SPEAKING, the halo is STEADY — no pulsing.  Reads as "I'm
 * speaking, my voice is the dynamism."  240 ms fade in / out so the
 * transition into/out of voice playback feels coupled. */
#define ORB_SPEAKING_HALO_OPA 70
#define ORB_SPEAKING_FADE_MS 240

/* ── Presence wake (camera-driven, cross-state dim) ──────────────────── */

/* When the room is empty (camera samples no face for ≥ N seconds), the
 * orb dims to ~33% opacity AND its motion timers slow ×0.4.  When a face
 * reappears, snap back to 100%.  The dim is a GLOBAL multiplier on top
 * of every state's own motion — not counted toward the per-state motion
 * budget (which is "exactly one active motion per state"). */
#define ORB_PRESENCE_AWAKE_OPA 255 /* LV_OPA_COVER */
#define ORB_PRESENCE_ASLEEP_OPA 85 /* ≈33 % */
#define ORB_PRESENCE_FADE_MS 600   /* gentle eyelid close/open */

/* ── Module state ────────────────────────────────────────────────────── */

static lv_obj_t *s_root = NULL;  /* parent container (= the home screen) */
static lv_obj_t *s_body = NULL;  /* the lit sphere itself */
static lv_obj_t *s_spec = NULL;  /* tilt-driven specular highlight (child of s_body) */
static lv_obj_t *s_halo = NULL;  /* voice-bloom halo (sibling-BEHIND s_body) */
static lv_obj_t *s_comet = NULL; /* skill-rim comet (sibling-AFTER s_body so it draws on top) */
static lv_timer_t *s_tilt_timer = NULL;
static lv_timer_t *s_bloom_timer = NULL;
static int s_orb_cx = 0; /* stashed at create — used by comet orbit math */
static int s_orb_cy = 0;
static float s_tilt_dx_ema = 0.0f;
static float s_tilt_dy_ema = 0.0f;
static float s_bloom_rms_ema = 0.0f;
/* Effective rest position for the specular highlight.  Animated via
 * `lean_anim_*` when entering/leaving LISTENING; tilt_tick reads these
 * as the base around which the IMU drift orbits.  Initialized at the
 * IDLE rest in ui_orb_create. */
static int s_spec_rest_x_eff = ORB_SPEC_REST_X;
static int s_spec_rest_y_eff = ORB_SPEC_REST_Y;
static ui_orb_state_t s_state = ORB_STATE_IDLE;
static bool s_presence_near = true;
static int8_t s_force_hour = -1; /* -1 = use real RTC */
static int s_last_painted_hour = -1;
static uint8_t s_last_painted_mode = 0;

/* PR 2: pipeline state overlay.  When != DICT_IDLE, all paint cycles
 * route through the pipeline-state painter and the voice-state painter
 * is suppressed.  IDLE → revert to voice-state painting. */
static dict_event_t s_pipeline = {
   .state = DICT_IDLE,
   .fail_reason = DICT_FAIL_NONE,
   .started_ms = 0,
   .stopped_ms = 0,
   .last_change_ms = 0,
   .note_slot = -1,
};
static lv_obj_t *s_orb_caption = NULL;  /* Label below the orb body */
static lv_timer_t *s_saved_fade_timer = NULL;  /* SAVED → IDLE 2s timer */
static lv_timer_t *s_rec_timer_label = NULL;  /* updates RECORDING caption every 200 ms */

/* ── Circadian palette ───────────────────────────────────────────────── */

static void pick_circadian_palette(int hour, uint32_t *top, uint32_t *bot) {
   /* Wrap any out-of-range into [0, 23] then fall through to midday. */
   if (hour < 0 || hour > 23) hour = 12;
   if (hour >= 5 && hour <= 6) {
      *top = 0xFFE4A8;
      *bot = 0xE89A6B; /* Dawn */
   } else if (hour >= 7 && hour <= 10) {
      *top = 0xFFD568;
      *bot = 0xD2811A; /* Morning */
   } else if (hour >= 11 && hour <= 14) {
      *top = 0xFFC75A;
      *bot = 0xB9650A; /* Midday */
   } else if (hour >= 15 && hour <= 17) {
      *top = 0xFF9E55;
      *bot = 0xA04A0E; /* Afternoon */
   } else if (hour >= 18 && hour <= 20) {
      *top = 0xFF8050;
      *bot = 0x8B3010; /* Sunset */
   } else if (hour >= 21 && hour <= 22) {
      *top = 0xD5A050;
      *bot = 0x804008; /* Dusk */
   } else {
      *top = 0x7F6535;
      *bot = 0x2A1F0F; /* Night */
   }
}

static int orb_effective_hour(void) {
   if (s_force_hour >= 0) return (int)s_force_hour;
   time_t now = 0;
   time(&now);
   struct tm tm_local;
   localtime_r(&now, &tm_local);
   return tm_local.tm_hour;
}

/* ── Paint ───────────────────────────────────────────────────────────── */

/* Vertical gradient: fixed cream-amber top stop (lit side) →
 * halved hour-palette edge color at the bottom (shadow side).
 * The wide luminance range makes the orb read as a 3D lit sphere
 * (TT #510 result, preserved as the body baseline).  */
static void paint_body_for_hour(int hour) {
   if (!s_body) return;
   uint32_t mid = 0, edge = 0;
   pick_circadian_palette(hour, &mid, &edge);
   (void)mid;
   uint8_t er = (edge >> 16) & 0xFF;
   uint8_t eg = (edge >> 8) & 0xFF;
   uint8_t eb = edge & 0xFF;
   er >>= 1;
   eg >>= 1;
   eb >>= 1;
   lv_obj_set_style_bg_color(s_body, lv_color_hex(0xFFEBC4), LV_PART_MAIN);
   lv_obj_set_style_bg_grad_color(s_body, lv_color_make(er, eg, eb), LV_PART_MAIN);
   lv_obj_set_style_bg_grad_dir(s_body, LV_GRAD_DIR_VER, LV_PART_MAIN);
   lv_obj_set_style_bg_opa(s_body, LV_OPA_COVER, LV_PART_MAIN);
   s_last_painted_hour = hour;
}

void ui_orb_paint_for_mode(uint8_t mode) {
   s_last_painted_mode = mode;
   if (!s_body) return;
   paint_body_for_hour(orb_effective_hour());
}

void ui_orb_paint_for_tone(widget_tone_t tone) {
   if (!s_body) return;
   uint32_t top, bot;
   switch (tone) {
      case WIDGET_TONE_CALM:
         top = 0x7DE69F;
         bot = 0x166C3A;
         break;
      case WIDGET_TONE_ACTIVE:
         top = 0xFFC75A;
         bot = 0xB9650A;
         break;
      case WIDGET_TONE_APPROACHING:
         top = 0xFFB637;
         bot = 0xD97706;
         break;
      case WIDGET_TONE_URGENT:
         top = 0xFF7E95;
         bot = 0xF43F5E;
         break;
      case WIDGET_TONE_DONE:
         top = 0xBBFFCC;
         bot = 0x0E5E2A;
         break;
      default:
         top = 0xFFC75A;
         bot = 0xB9650A;
         break;
   }
   lv_obj_set_style_bg_color(s_body, lv_color_hex(top), LV_PART_MAIN);
   lv_obj_set_style_bg_grad_color(s_body, lv_color_hex(bot), LV_PART_MAIN);
   lv_obj_set_style_bg_grad_dir(s_body, LV_GRAD_DIR_VER, LV_PART_MAIN);
   lv_obj_set_style_bg_opa(s_body, LV_OPA_COVER, LV_PART_MAIN);
   /* Tone overrides the hour-driven paint; bookkeeping for orb_repaint_if_hour_changed. */
   s_last_painted_hour = -1;
}

void ui_orb_repaint_if_hour_changed(void) {
   if (!s_body) return;
   int h = orb_effective_hour();
   if (h != s_last_painted_hour) paint_body_for_hour(h);
}

/* ── Tilt-driven specular drift ──────────────────────────────────────── */

static void tilt_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_spec) return;
   tab5_imu_data_t d;
   if (tab5_imu_read(&d) != ESP_OK) return;

   /* Light source fixed in world space → gravity along +x pulls the
    * lit side toward -x → highlight slides toward -x (left).  Same on y. */
   const float target_dx = -d.accel.x * ORB_TILT_K_PX_PER_G;
   const float target_dy = d.accel.y * ORB_TILT_K_PX_PER_G;
   s_tilt_dx_ema += ORB_TILT_ALPHA * (target_dx - s_tilt_dx_ema);
   s_tilt_dy_ema += ORB_TILT_ALPHA * (target_dy - s_tilt_dy_ema);

   /* Clamp */
   float dx = s_tilt_dx_ema;
   float dy = s_tilt_dy_ema;
   if (dx > (float)ORB_TILT_LIMIT) dx = (float)ORB_TILT_LIMIT;
   if (dx < -(float)ORB_TILT_LIMIT) dx = -(float)ORB_TILT_LIMIT;
   if (dy > (float)ORB_TILT_LIMIT) dy = (float)ORB_TILT_LIMIT;
   if (dy < -(float)ORB_TILT_LIMIT) dy = -(float)ORB_TILT_LIMIT;

   lv_obj_set_pos(s_spec, s_spec_rest_x_eff + (int)dx, s_spec_rest_y_eff + (int)dy);
}

static void tilt_start(void) {
   if (s_tilt_timer || !s_spec) return;
   s_tilt_timer = lv_timer_create(tilt_tick_cb, ORB_TILT_PERIOD_MS, NULL);
}

static void tilt_stop(void) {
   if (s_tilt_timer) {
      lv_timer_delete(s_tilt_timer);
      s_tilt_timer = NULL;
   }
   /* Snap back to rest so next state-machine entry starts clean. */
   if (s_spec) {
      s_tilt_dx_ema = 0.0f;
      s_tilt_dy_ema = 0.0f;
      lv_obj_set_pos(s_spec, s_spec_rest_x_eff, s_spec_rest_y_eff);
   }
}

/* ── Listening lean ──────────────────────────────────────────────────── */

static void lean_anim_rest_x_cb(void *obj, int32_t v) {
   (void)obj;
   s_spec_rest_x_eff = (int)v;
}
static void lean_anim_rest_y_cb(void *obj, int32_t v) {
   (void)obj;
   s_spec_rest_y_eff = (int)v;
   /* When tilt is paused (PROCESSING / SPEAKING) the spec wouldn't
    * otherwise repaint during the lean anim — directly drive it so
    * the user still sees the lean motion if it fires while paused. */
   if (s_spec && s_tilt_timer == NULL) {
      lv_obj_set_pos(s_spec, s_spec_rest_x_eff, s_spec_rest_y_eff);
   }
}

static void lean_anim_to(int target_x, int target_y) {
   if (!s_spec) return;
   /* Dual-axis anim — one var per axis */
   lv_anim_t ax;
   lv_anim_init(&ax);
   lv_anim_set_var(&ax, s_spec);
   lv_anim_set_exec_cb(&ax, lean_anim_rest_x_cb);
   lv_anim_set_values(&ax, s_spec_rest_x_eff, target_x);
   lv_anim_set_time(&ax, ORB_LEAN_ANIM_MS);
   lv_anim_set_path_cb(&ax, lv_anim_path_ease_out);
   lv_anim_start(&ax);

   lv_anim_t ay;
   lv_anim_init(&ay);
   lv_anim_set_var(&ay, s_spec);
   lv_anim_set_exec_cb(&ay, lean_anim_rest_y_cb);
   lv_anim_set_values(&ay, s_spec_rest_y_eff, target_y);
   lv_anim_set_time(&ay, ORB_LEAN_ANIM_MS);
   lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
   lv_anim_start(&ay);
}

static void lean_enter(void) { lean_anim_to(ORB_LEAN_REST_X, ORB_LEAN_REST_Y); }
static void lean_exit(void) { lean_anim_to(ORB_SPEC_REST_X, ORB_SPEC_REST_Y); }

/* ── Voice bloom ─────────────────────────────────────────────────────── */

static void bloom_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_halo) return;
   float rms = voice_get_current_rms();
   if (rms < 0.0f) rms = 0.0f;
   if (rms > 1.0f) rms = 1.0f;
   s_bloom_rms_ema = (ORB_BLOOM_ALPHA * rms) + ((1.0f - ORB_BLOOM_ALPHA) * s_bloom_rms_ema);
   int opa = ORB_HALO_OPA_FLOOR + (int)((float)(ORB_HALO_OPA_CEILING - ORB_HALO_OPA_FLOOR) * s_bloom_rms_ema);
   if (opa < 0) opa = 0;
   if (opa > 255) opa = 255;
   lv_obj_set_style_bg_opa(s_halo, (lv_opa_t)opa, LV_PART_MAIN);
}

static void bloom_start(void) {
   if (s_bloom_timer || !s_halo) return;
   s_bloom_rms_ema = 0.0f;
   lv_obj_set_style_bg_opa(s_halo, ORB_HALO_OPA_FLOOR, LV_PART_MAIN);
   s_bloom_timer = lv_timer_create(bloom_tick_cb, ORB_BLOOM_PERIOD_MS, NULL);
}

static void bloom_stop(void) {
   if (s_bloom_timer) {
      lv_timer_delete(s_bloom_timer);
      s_bloom_timer = NULL;
   }
   if (s_halo) lv_obj_set_style_bg_opa(s_halo, 0, LV_PART_MAIN);
   s_bloom_rms_ema = 0.0f;
}

/* ── Speaking halo (steady) ──────────────────────────────────────────── */

/* Reuses s_halo (sibling-behind s_body, same obj that bloom drives in
 * LISTENING).  bloom + speaking_halo are mutually exclusive — the
 * state machine guarantees only one is active at a time. */
static void halo_opa_anim_cb(void *obj, int32_t v) {
   (void)obj;
   if (!s_halo) return;
   lv_obj_set_style_bg_opa(s_halo, (lv_opa_t)v, LV_PART_MAIN);
}

static void halo_anim_to(int target_opa) {
   if (!s_halo) return;
   lv_anim_delete(s_halo, halo_opa_anim_cb);
   lv_anim_t a;
   lv_anim_init(&a);
   lv_anim_set_var(&a, s_halo);
   lv_anim_set_exec_cb(&a, halo_opa_anim_cb);
   int cur = lv_obj_get_style_bg_opa(s_halo, LV_PART_MAIN);
   lv_anim_set_values(&a, cur, target_opa);
   lv_anim_set_time(&a, ORB_SPEAKING_FADE_MS);
   lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
   lv_anim_start(&a);
}

/* TT #511 wave-1.6 (change H): SPEAKING also brightens the body's
 * top stop from cream-amber (0xFFEBC4) to near-white-warm (0xFFF8DC).
 * Reads as "voice is coming out, the sphere is glowing slightly
 * hotter."  Reverts to circadian palette on exit. */
static void speaking_body_tint(bool active) {
   if (!s_body) return;
   if (active) {
      lv_obj_set_style_bg_color(s_body, lv_color_hex(0xFFF8DC), LV_PART_MAIN);
   } else {
      paint_body_for_hour(orb_effective_hour());
   }
}

static void speaking_enter(void) {
   halo_anim_to(ORB_SPEAKING_HALO_OPA);
   speaking_body_tint(true);
}
static void speaking_exit(void) {
   halo_anim_to(0);
   speaking_body_tint(false);
}

/* ── Skill-rim comet ─────────────────────────────────────────────────── */

/* Orbit position anim: input v is angle * 10 in 0..3600 (one full
 * rotation per anim cycle).  Decoded to degrees via integer math and
 * fed to LVGL's fixed-point trigo helper, then scaled to pixel offset
 * around (s_orb_cx, s_orb_cy). */
static void comet_pos_cb(void *obj, int32_t v) {
   (void)obj;
   if (!s_comet) return;
   int angle = v / 10; /* 0..360 deg */
   int s_q = lv_trigo_sin(angle);
   int c_q = lv_trigo_sin(angle + 90); /* cos(x) = sin(x + 90) */
   int dx = (ORB_COMET_RADIUS_PX * c_q) / LV_TRIGO_SIN_MAX;
   int dy = (ORB_COMET_RADIUS_PX * s_q) / LV_TRIGO_SIN_MAX;
   lv_obj_set_pos(s_comet, s_orb_cx - (ORB_COMET_W / 2) + dx, s_orb_cy - (ORB_COMET_H / 2) + dy);
}

static void comet_opa_cb(void *obj, int32_t v) {
   (void)obj;
   if (!s_comet) return;
   lv_obj_set_style_bg_opa(s_comet, (lv_opa_t)v, LV_PART_MAIN);
}

static void comet_start(void) {
   if (!s_comet) return;
   /* Position cycle — infinite repeat. */
   lv_anim_delete(s_comet, comet_pos_cb);
   lv_anim_t pa;
   lv_anim_init(&pa);
   lv_anim_set_var(&pa, s_comet);
   lv_anim_set_exec_cb(&pa, comet_pos_cb);
   lv_anim_set_values(&pa, 0, 3600);
   lv_anim_set_time(&pa, ORB_COMET_PERIOD_MS);
   lv_anim_set_repeat_count(&pa, LV_ANIM_REPEAT_INFINITE);
   lv_anim_start(&pa);
   /* Fade in. */
   lv_anim_delete(s_comet, comet_opa_cb);
   lv_anim_t oa;
   lv_anim_init(&oa);
   lv_anim_set_var(&oa, s_comet);
   lv_anim_set_exec_cb(&oa, comet_opa_cb);
   int cur = lv_obj_get_style_bg_opa(s_comet, LV_PART_MAIN);
   lv_anim_set_values(&oa, cur, ORB_COMET_PEAK_OPA);
   lv_anim_set_time(&oa, ORB_COMET_FADE_MS);
   lv_anim_start(&oa);
}

static void comet_stop(void) {
   if (!s_comet) return;
   /* Cancel position cycle immediately; fade out opa. */
   lv_anim_delete(s_comet, comet_pos_cb);
   lv_anim_delete(s_comet, comet_opa_cb);
   lv_anim_t oa;
   lv_anim_init(&oa);
   lv_anim_set_var(&oa, s_comet);
   lv_anim_set_exec_cb(&oa, comet_opa_cb);
   int cur = lv_obj_get_style_bg_opa(s_comet, LV_PART_MAIN);
   lv_anim_set_values(&oa, cur, 0);
   lv_anim_set_time(&oa, ORB_COMET_FADE_MS);
   lv_anim_start(&oa);
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/* Forward decl for the pipeline subscriber bridge (PR 2) — body lives
 * near ui_orb_set_pipeline_state below; ui_orb_create takes its address. */
static void orb_pipeline_cb(const dict_event_t *event, void *user_data);

void ui_orb_create(lv_obj_t *parent, int cx, int cy) {
   if (s_body || !parent) {
      ESP_LOGW(TAG, "ui_orb_create skipped (s_body=%p parent=%p)", s_body, parent);
      return;
   }
   s_root = parent;
   s_orb_cx = cx;
   s_orb_cy = cy;

   /* Halo FIRST so it sits BEHIND s_body in z-order (LVGL draws siblings
    * in creation order).  s_body's opa-cover gradient masks the part of
    * the halo overlapping the orb; only the outer "bloom" ring shows. */
   s_halo = lv_obj_create(parent);
   lv_obj_remove_style_all(s_halo);
   lv_obj_set_size(s_halo, ORB_HALO_DIAM, ORB_HALO_DIAM);
   lv_obj_set_pos(s_halo, cx - (ORB_HALO_DIAM / 2), cy - (ORB_HALO_DIAM / 2));
   lv_obj_set_style_radius(s_halo, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_bg_color(s_halo, lv_color_hex(0xFFB870), 0);
   lv_obj_set_style_bg_opa(s_halo, 0, 0); /* invisible at rest */
   lv_obj_clear_flag(s_halo, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

   s_body = lv_obj_create(parent);
   lv_obj_remove_style_all(s_body);
   lv_obj_set_size(s_body, ORB_SIZE, ORB_SIZE);
   lv_obj_set_pos(s_body, cx - (ORB_SIZE / 2), cy - (ORB_SIZE / 2));
   lv_obj_set_style_radius(s_body, LV_RADIUS_CIRCLE, 0);
   lv_obj_add_flag(s_body, LV_OBJ_FLAG_CLICKABLE);
   lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
   /* Initial paint at the current hour.  paint_for_mode is the entry
    * point ui_home will call later if it wants to refresh on mode change. */
   paint_body_for_hour(orb_effective_hour());

   /* TT #511 wave-1 step 4: tilt-driven specular highlight.
    * Soft cream-white ellipse, opa intentionally low so it BLENDS with
    * the body's lit-side gradient instead of stamping a disc on top —
    * the #507 failure mode (read as ball-in-ball).  Clipped by the
    * body's radius-full so it can never paint outside the sphere. */
   s_spec = lv_obj_create(s_body);
   lv_obj_remove_style_all(s_spec);
   lv_obj_set_size(s_spec, ORB_SPEC_W, ORB_SPEC_H);
   lv_obj_set_pos(s_spec, ORB_SPEC_REST_X, ORB_SPEC_REST_Y);
   lv_obj_set_style_radius(s_spec, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_bg_color(s_spec, lv_color_hex(0xFFF5E0), 0);
   lv_obj_set_style_bg_opa(s_spec, 110, 0);
   lv_obj_clear_flag(s_spec, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
   s_tilt_dx_ema = 0.0f;
   s_tilt_dy_ema = 0.0f;

   /* Skill-rim comet — sibling AFTER s_body so it draws on top.
    * Starts invisible (opa 0); state machine drives fade-in on
    * PROCESSING enter, fade-out on exit. */
   s_comet = lv_obj_create(parent);
   lv_obj_remove_style_all(s_comet);
   lv_obj_set_size(s_comet, ORB_COMET_W, ORB_COMET_H);
   lv_obj_set_pos(s_comet, cx - ORB_COMET_W / 2 + ORB_COMET_RADIUS_PX, cy - ORB_COMET_H / 2);
   lv_obj_set_style_radius(s_comet, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_bg_color(s_comet, lv_color_hex(0xFFE0A0), 0);
   lv_obj_set_style_bg_opa(s_comet, 0, 0);
   lv_obj_clear_flag(s_comet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

   /* PR 2: caption label below the orb for pipeline state text
    * ("● RECORDING 0:23", "UPLOADING…", "TRANSCRIBING…", "✓ Saved").
    * Sits 24 px below the orb body, transparent background, caption
    * font.  Hidden by default — paint_pipeline_state shows it. */
   s_orb_caption = lv_label_create(parent);
   if (s_orb_caption) {
      lv_label_set_text(s_orb_caption, "");
      lv_obj_set_style_text_color(s_orb_caption, lv_color_hex(0xE8E8EF), 0);
      lv_obj_set_style_text_font(s_orb_caption, FONT_CAPTION, 0);
      lv_obj_set_style_text_letter_space(s_orb_caption, 2, 0);
      lv_obj_align_to(s_orb_caption, s_body, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);
      lv_obj_add_flag(s_orb_caption, LV_OBJ_FLAG_HIDDEN);
   }

   /* IDLE state arms tilt drift by default. */
   tilt_start();

   /* PR 2: subscribe to the dictation pipeline.  Callbacks land on the
    * LVGL thread thanks to the _lvgl variant.  Static guard so a
    * subsequent create/destroy/create cycle doesn't double-subscribe. */
   static int s_pipeline_sub = -1;
   if (s_pipeline_sub < 0) {
      s_pipeline_sub = voice_dictation_subscribe_lvgl(orb_pipeline_cb, NULL);
      ESP_LOGI(TAG, "pipeline subscriber registered handle=%d", s_pipeline_sub);
   }

   ESP_LOGI(TAG, "orb created at (%d, %d), size %d", cx, cy, ORB_SIZE);
}

void ui_orb_destroy(void) {
   /* Stop timers first — their callbacks dereference module statics,
    * so they must not be scheduled past the screen tear-down. */
   tilt_stop();
   bloom_stop();
   /* Cancel any in-flight lean anims targeting s_spec. */
   if (s_spec) {
      lv_anim_delete(s_spec, lean_anim_rest_x_cb);
      lv_anim_delete(s_spec, lean_anim_rest_y_cb);
   }
   /* Cancel any in-flight comet anims. */
   if (s_comet) {
      lv_anim_delete(s_comet, comet_pos_cb);
      lv_anim_delete(s_comet, comet_opa_cb);
   }
   /* Cancel any in-flight speaking-halo fade. */
   if (s_halo) {
      lv_anim_delete(s_halo, halo_opa_anim_cb);
   }
   /* PR 2: tear down caption + saved-fade timer. */
   if (s_orb_caption) {
      lv_obj_del(s_orb_caption);
      s_orb_caption = NULL;
   }
   if (s_saved_fade_timer) {
      lv_timer_del(s_saved_fade_timer);
      s_saved_fade_timer = NULL;
   }
   /* The body's parent (the home screen) owns the actual delete via its
    * own destroy path; here we only clear our handles + reset state so
    * a subsequent ui_orb_create on a fresh screen starts clean. */
   s_root = NULL;
   s_body = NULL;
   s_spec = NULL;
   s_halo = NULL;
   s_comet = NULL;
   s_state = ORB_STATE_IDLE;
   s_last_painted_hour = -1;
   s_tilt_dx_ema = 0.0f;
   s_tilt_dy_ema = 0.0f;
   s_bloom_rms_ema = 0.0f;
   s_spec_rest_x_eff = ORB_SPEC_REST_X;
   s_spec_rest_y_eff = ORB_SPEC_REST_Y;
}

/* ── State driver ────────────────────────────────────────────────────── */

void ui_orb_set_state(ui_orb_state_t s) {
   if (s == s_state) return;
   ui_orb_state_t prev = s_state;
   ESP_LOGD(TAG, "state %d → %d", (int)prev, (int)s);
   s_state = s;

   /* Step 4: tilt drift runs in IDLE + LISTENING (always-physical
    * feel + lets the user point at the orb during voice).  In
    * PROCESSING + SPEAKING the highlight freezes — the state's own
    * motion (comet, halo) carries the action; a drifting highlight
    * would compete. */
   switch (s) {
      case ORB_STATE_IDLE:
      case ORB_STATE_LISTENING:
         tilt_start();
         break;
      case ORB_STATE_PROCESSING:
      case ORB_STATE_SPEAKING:
         tilt_stop();
         break;
   }

   /* Step 5: voice bloom + listening lean are tied to LISTENING.
    * Edge-triggered: only on enter / exit transitions. */
   if (s == ORB_STATE_LISTENING && prev != ORB_STATE_LISTENING) {
      bloom_start();
      lean_enter();
   } else if (s != ORB_STATE_LISTENING && prev == ORB_STATE_LISTENING) {
      bloom_stop();
      lean_exit();
   }

   /* Step 6: skill-rim comet runs only during PROCESSING. */
   if (s == ORB_STATE_PROCESSING && prev != ORB_STATE_PROCESSING) {
      comet_start();
   } else if (s != ORB_STATE_PROCESSING && prev == ORB_STATE_PROCESSING) {
      comet_stop();
   }

   /* Step 7: SPEAKING — steady halo, no motion (tilt already paused
    * above).  Body's existing cream-amber top stop is already warm
    * enough to read as "speaking"; the halo carries the signal. */
   if (s == ORB_STATE_SPEAKING && prev != ORB_STATE_SPEAKING) {
      speaking_enter();
   } else if (s != ORB_STATE_SPEAKING && prev == ORB_STATE_SPEAKING) {
      speaking_exit();
   }
}

ui_orb_state_t ui_orb_get_state(void) { return s_state; }

/* ── Hardware-aware hooks ────────────────────────────────────────────── */

/* Anim cb for the presence-dim fade.  Applies the global opa multiplier
 * to s_body + s_halo + s_comet (s_spec is a child of s_body so it
 * inherits).  Run on the LVGL thread via tab5_lv_async_call from the
 * (eventually camera-driven) presence sampler. */
static void presence_opa_anim_cb(void *obj, int32_t v) {
   (void)obj;
   if (s_body) lv_obj_set_style_opa(s_body, (lv_opa_t)v, LV_PART_MAIN);
   if (s_halo) lv_obj_set_style_opa(s_halo, (lv_opa_t)v, LV_PART_MAIN);
   if (s_comet) lv_obj_set_style_opa(s_comet, (lv_opa_t)v, LV_PART_MAIN);
}

typedef struct {
   bool near;
} presence_call_t;

static void presence_apply_async(void *arg) {
   presence_call_t *c = (presence_call_t *)arg;
   if (!c) return;
   bool near = c->near;
   free(c);

   int target = near ? ORB_PRESENCE_AWAKE_OPA : ORB_PRESENCE_ASLEEP_OPA;
   /* Read current opa from s_body — they're all in sync because we
    * always write all three together. */
   int cur = s_body ? lv_obj_get_style_opa(s_body, LV_PART_MAIN) : target;
   if (s_body) lv_anim_delete(s_body, presence_opa_anim_cb);
   /* One anim drives the cb that touches all three objs; use s_body
    * as the var so the anim is delete-able as a single handle. */
   if (!s_body) return;
   lv_anim_t a;
   lv_anim_init(&a);
   lv_anim_set_var(&a, s_body);
   lv_anim_set_exec_cb(&a, presence_opa_anim_cb);
   lv_anim_set_values(&a, cur, target);
   lv_anim_set_time(&a, ORB_PRESENCE_FADE_MS);
   lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
   lv_anim_start(&a);
}

void ui_orb_set_presence(bool near) {
   if (s_presence_near == near) return;
   s_presence_near = near;
   /* Marshal to LVGL thread — caller may be tab5_worker (future
    * camera sampler) or the debug-server httpd task. */
   presence_call_t *c = (presence_call_t *)malloc(sizeof(*c));
   if (!c) return;
   c->near = near;
   tab5_lv_async_call(presence_apply_async, c);
}

static void orb_force_repaint_async_cb(void *arg) {
   (void)arg;
   ui_orb_repaint_if_hour_changed();
}

void ui_orb_force_hour(int hour) {
   if (hour < -1 || hour > 23) return;
   s_force_hour = (int8_t)hour;
   s_last_painted_hour = -1; /* force repaint on next tick */
   /* W14-H10: this can be called from httpd; never poke LVGL directly. */
   tab5_lv_async_call(orb_force_repaint_async_cb, NULL);
}

int ui_orb_get_effective_hour(void) { return orb_effective_hour(); }

void ui_orb_ripple_for_tool(const char *tool_name) {
   (void)tool_name;
   /* Step 6 replaces with comet anim into PROCESSING. */
   ui_orb_set_state(ORB_STATE_PROCESSING);
}

/* ── Accessors ───────────────────────────────────────────────────────── */

lv_obj_t *ui_orb_get_root(void) { return s_root; }

lv_obj_t *ui_orb_get_body(void) { return s_body; }

/* ── Pipeline state (PR 2) ───────────────────────────────────────────── */

/* Paint the orb body in the pipeline-state tint.  Caller is responsible
 * for setting the caption text + showing the caption label.
 *
 * Uses the same body widget that the voice-state painter paints into.
 * The previous body styling (circadian gradient, etc.) is shadowed by
 * the solid tint until pipeline returns to IDLE. */
static void paint_pipeline_body(uint32_t color_hex) {
   if (!s_body) return;
   lv_obj_set_style_bg_color(s_body, lv_color_hex(color_hex), LV_PART_MAIN);
   /* Keep the radial-gradient luster for depth — only tint, don't go
    * flat.  The existing body uses LV_GRAD_DIR_VER; preserve. */
}

static const char *fail_reason_caption(dict_fail_t r) {
   switch (r) {
   case DICT_FAIL_AUTH:      return "AUTH";
   case DICT_FAIL_NETWORK:   return "NETWORK";
   case DICT_FAIL_EMPTY:     return "EMPTY — got silence";
   case DICT_FAIL_NO_AUDIO:  return "NO AUDIO";
   case DICT_FAIL_TOO_LONG:  return "TOO LONG (5 min cap)";
   case DICT_FAIL_CANCELLED: return "CANCELLED";
   default:                  return "FAIL";
   }
}

static void set_caption_text(const char *text) {
   if (!s_orb_caption || !text) return;
   lv_label_set_text(s_orb_caption, text);
   lv_obj_clear_flag(s_orb_caption, LV_OBJ_FLAG_HIDDEN);
}

static void hide_caption(void) {
   if (s_orb_caption) lv_obj_add_flag(s_orb_caption, LV_OBJ_FLAG_HIDDEN);
}

/* Timer cb for SAVED → IDLE auto-fade. */
static void saved_fade_to_idle_cb(lv_timer_t *t) {
   (void)t;
   s_saved_fade_timer = NULL;
   voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE,
                             (uint32_t)(esp_timer_get_time() / 1000));
}

/* Update the RECORDING caption with live elapsed time.  Stops itself
 * if the pipeline has left RECORDING (defensive — set_pipeline_state
 * also tears it down explicitly). */
static void rec_timer_label_cb(lv_timer_t *t) {
   (void)t;
   if (s_pipeline.state != DICT_RECORDING) {
      if (s_rec_timer_label) {
         lv_timer_del(s_rec_timer_label);
         s_rec_timer_label = NULL;
      }
      return;
   }
   uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
   uint32_t dur_ms = (s_pipeline.started_ms && now_ms >= s_pipeline.started_ms)
                       ? (now_ms - s_pipeline.started_ms)
                       : 0;
   uint32_t s = dur_ms / 1000;
   char buf[40];
   snprintf(buf, sizeof(buf), "● RECORDING %lu:%02lu",
            (unsigned long)(s / 60), (unsigned long)(s % 60));
   if (s_orb_caption) lv_label_set_text(s_orb_caption, buf);
}

/* Subscriber bridge: voice_dictation fires this on the LVGL thread
 * (because we subscribe via voice_dictation_subscribe_lvgl).  Just
 * forward to the public state-driver. */
static void orb_pipeline_cb(const dict_event_t *event, void *user_data) {
   (void)user_data;
   ui_orb_set_pipeline_state(event);
}

void ui_orb_set_pipeline_state(const dict_event_t *event) {
   if (!event) return;
   s_pipeline = *event;

   /* Tear down the SAVED auto-fade timer when leaving SAVED. */
   if (event->state != DICT_SAVED && s_saved_fade_timer) {
      lv_timer_del(s_saved_fade_timer);
      s_saved_fade_timer = NULL;
   }

   /* Stop the live elapsed-time timer when leaving RECORDING. */
   if (event->state != DICT_RECORDING && s_rec_timer_label) {
      lv_timer_del(s_rec_timer_label);
      s_rec_timer_label = NULL;
   }

   char buf[64];
   switch (event->state) {
   case DICT_IDLE:
      hide_caption();
      /* Repaint via the voice-state painter so the orb returns to its
       * normal IDLE/LISTENING/PROCESSING/SPEAKING visuals.  Re-paint
       * body for current hour too — paint_pipeline_body() shadowed it. */
      paint_body_for_hour(orb_effective_hour());
      ui_orb_set_state(s_state);
      return;

   case DICT_RECORDING: {
      /* Engage the existing LISTENING state machine so the mic-RMS-driven
       * halo bloom fires.  Our pipeline body tint overrides the body
       * colour, but the bloom mechanic (halo opacity from mic RMS) still
       * runs because it manipulates s_halo, not s_body. */
      ui_orb_set_state(ORB_STATE_LISTENING);
      paint_pipeline_body(0xE74C3C);
      uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
      uint32_t dur_ms = (event->started_ms && now_ms >= event->started_ms)
                          ? (now_ms - event->started_ms)
                          : 0;
      uint32_t s = dur_ms / 1000;
      snprintf(buf, sizeof(buf), "● RECORDING %lu:%02lu",
               (unsigned long)(s / 60), (unsigned long)(s % 60));
      set_caption_text(buf);
      /* Spawn the 200 ms tick that keeps the M:SS caption live. */
      if (!s_rec_timer_label) {
         s_rec_timer_label = lv_timer_create(rec_timer_label_cb, 200, NULL);
      }
      break;
   }

   case DICT_UPLOADING:
      paint_pipeline_body(0xF59E0B);
      set_caption_text("UPLOADING…");
      break;

   case DICT_TRANSCRIBING:
      paint_pipeline_body(0xF59E0B);
      set_caption_text("TRANSCRIBING…");
      /* Reuse the existing PROCESSING comet animation for visual
       * continuity. */
      ui_orb_set_state(ORB_STATE_PROCESSING);
      break;

   case DICT_SAVED:
      paint_pipeline_body(0x22C55E);
      set_caption_text("✓ Saved");
      /* Schedule auto-fade back to IDLE after 2 s.  Idempotent — if
       * one already exists (rapid SAVED re-entry), don't stack. */
      if (!s_saved_fade_timer) {
         s_saved_fade_timer = lv_timer_create(saved_fade_to_idle_cb, 2000, NULL);
         if (s_saved_fade_timer) lv_timer_set_repeat_count(s_saved_fade_timer, 1);
      }
      break;

   case DICT_FAILED:
      paint_pipeline_body(0xE74C3C);
      snprintf(buf, sizeof(buf), "● %s — tap to retry",
               fail_reason_caption(event->fail_reason));
      set_caption_text(buf);
      break;
   }
}

bool ui_orb_pipeline_active(void) {
   return s_pipeline.state != DICT_IDLE;
}
