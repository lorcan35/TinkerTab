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

#include <math.h> /* sinf for idle breath + lissajous spec drift (TT #543) */
#include <stdint.h>
#include <stdlib.h> /* malloc/free for presence-call marshalling */
#include <time.h>

#include "config.h"    /* FONT_CAPTION for pipeline-state caption (PR 2) */
#include "debug_obs.h" /* tab5_debug_obs_event for pipeline-cb tracing (PR 2) */
#include "esp_log.h"
#include "esp_timer.h" /* esp_timer_get_time for RECORDING caption timer (PR 2) */
#include "imu.h"       /* tab5_imu_read for tilt-driven specular drift */
#include "lvgl.h"
#include "ui_core.h"              /* tab5_lv_async_call for cross-thread repaints */
#include "voice.h"                /* voice_get_current_rms for the LISTENING bloom */
#include "voice_dictation.h"      /* pipeline-state types (PR 2) */
#include "voice_dictation_lvgl.h" /* LVGL-marshalled subscriber (PR 2) */
#include "widget.h"               /* widget_tone_t for paint_for_tone */

static const char *TAG = "ui_orb";

/* ── Sizing / layout constants ──────────────────────────────────────── */

#define ORB_SIZE 280 /* body diameter — TT #541 hero scale (was 180) */

/* ── Specular highlight (tilt-driven) ───────────────────────────────── */

/* Width × height of the soft cream-white ellipse that drifts inside the
 * orb to suggest a lit surface.  Clipped by s_body's radius-full corner.
 * Larger than #511's attempt (which was too small + had no working IMU
 * behind it) so the drift is unambiguously visible. */
/* TT #547: specular highlight beefed up for the sphere-native pass —
 * bigger ellipse + brighter peak opa + soft outer shadow that bleeds
 * a small glow into the body's lit side.  Reads like wet-sphere sun-
 * glint instead of a stamped oval. */
#define ORB_SPEC_W 96
#define ORB_SPEC_H 56

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
#define ORB_HALO_OPA_FLOOR 40    /* TT #541: was 24 — visible at rest during LISTENING */
#define ORB_HALO_OPA_CEILING 160 /* TT #541: was 90 — louder peak when user speaks */
#define ORB_BLOOM_PERIOD_MS 100  /* 10 Hz */
#define ORB_BLOOM_ALPHA 0.50f    /* TT #541: was 0.30 — faster reaction to speech onset */

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

/* PR 2 polish: TRANSCRIBING rotating-arc sizing — used both at widget
 * creation and inside the tick driver, so the defines live up here. */
#define THINKING_ARC_DIAM (ORB_SIZE + 28)
#define THINKING_ARC_PERIOD_MS 1800
#define THINKING_ARC_SWEEP 90

/* ── Module state ────────────────────────────────────────────────────── */

static lv_obj_t *s_root = NULL;  /* parent container (= the home screen) */
static lv_obj_t *s_body = NULL;  /* the lit sphere itself */
static lv_obj_t *s_spec = NULL;  /* tilt-driven specular highlight (child of s_body) */
static lv_obj_t *s_halo = NULL;  /* voice-bloom halo (sibling-BEHIND s_body) */
static lv_obj_t *s_ripple_a = NULL; /* PR 2 polish: sonar ripple A — outermost-expanding ring during RECORDING */
static lv_obj_t *s_ripple_b =
    NULL; /* PR 2 polish: sonar ripple B — phase-offset by 50% so a new ripple is always rising as the prior fades */
static lv_timer_t *s_ripple_timer = NULL;       /* 16 ms tick for ripple geometry */
static lv_obj_t *s_thinking_arc = NULL;         /* PR 2 polish: rotating arc segment around body during TRANSCRIBING */
static lv_timer_t *s_thinking_arc_timer = NULL; /* drives the arc's rotation */
/* TT #547: replaced the 2D scrubber arc with a sphere-native warm
 * overlay — a circular sibling of s_body sized identically; bg_opa
 * animated from 0 to peak over the SPEAKING duration so the sphere
 * visibly brightens + warms as TTS plays. */
static lv_obj_t *s_speak_fill = NULL;
#define TTS_SCRUBBER_DEFAULT_MS 6000
static lv_obj_t *s_saved_burst = NULL;          /* PR 2 polish: one-shot green ring on SAVED entry */
static lv_timer_t *s_saved_burst_timer = NULL;
static uint32_t s_saved_burst_t0 = 0;         /* ms uptime when burst started, for elapsed-frac math */
static lv_timer_t *s_body_pulse_timer = NULL; /* PR 2 polish: body heartbeat scale during RECORDING */
static lv_timer_t *s_idle_breath_timer = NULL; /* TT #543: idle-only slow breath */

/* TT #545 sleep cycle: breath period is runtime-tunable so DROWSY +
 * ASLEEP can slow it down without re-creating the timer.  Defaults to
 * IDLE_BREATH_AWAKE_MS but `sleep_apply_phase` writes a longer value
 * before the next tick reads it.  s_last_interaction_ms is bumped on
 * any user/voice activity and consulted by sleep_tick_cb to decide
 * drowsy/asleep transitions. */
#define IDLE_BREATH_AWAKE_MS 4000
#define IDLE_BREATH_DROWSY_MS 8000
#define IDLE_BREATH_ASLEEP_MS 14000
static uint32_t s_idle_breath_period_ms = IDLE_BREATH_AWAKE_MS;
static uint32_t s_last_interaction_ms = 0;

/* Forward declarations — body_pulse + idle_breath helpers are defined
 * alongside the paint helpers near the bottom of the file but the state
 * machine in ui_orb_set_state needs them earlier. */
static void body_pulse_stop(void);
static void idle_breath_start(void);
static void idle_breath_stop(void);
static void sleep_cycle_start(void);
static void sleep_cycle_stop(void);
static void orb_body_press_cb(lv_event_t *e);
static void tts_scrubber_start(uint32_t duration_ms);
static void tts_scrubber_stop(void);
static void alive_start(void);
static void alive_stop(void);
/* TT #549 alive-motion guard — defined alongside idle_breath statics
 * later in the file but the helpers + state machine reference it
 * earlier. */
static bool s_alive_running;
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
static lv_obj_t *s_orb_caption = NULL;        /* Label below the orb body */
static lv_timer_t *s_saved_fade_timer = NULL; /* SAVED → IDLE 2s timer */
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
   /* PR 2: pipeline-state paint takes precedence — don't shadow the
    * RECORDING/UPLOADING/TRANSCRIBING/SAVED/FAILED body tint with the
    * circadian palette while a dictation is on-flight.  Mode bookkeeping
    * above still records the latest mode so DICT_IDLE can repaint
    * correctly when the pipeline returns. */
   if (ui_orb_pipeline_active()) return;
   paint_body_for_hour(orb_effective_hour());
}

void ui_orb_paint_for_tone(widget_tone_t tone) {
   if (!s_body) return;
   /* PR 2: pipeline-state paint takes precedence over widget tone. */
   if (ui_orb_pipeline_active()) return;
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
   /* PR 2: clock-driven repaints stay parked until DICT_IDLE returns. */
   if (ui_orb_pipeline_active()) return;
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

   /* TT #543: autonomous lissajous drift on top of the tilt motion.  Two
    * incommensurate periods (3.5 s on x, 4.7 s on y, +π/4 phase offset)
    * so the highlight traces an aperiodic figure-eight even when the
    * device is dead still on a desk.  Amplitude 3 px keeps the motion
    * subliminal — barely visible at a glance, registers over seconds
    * as "real lit sphere under a slowly moving sun."  Real tilt input
    * rides on top + dominates when the user actually moves the device. */
   const float two_pi = 6.28318530718f;
   uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
   float px = (float)(t_ms % 3500) / 3500.0f;
   float py = (float)(t_ms % 4700) / 4700.0f;
   const float drift_amp = 3.0f;
   float drift_x = drift_amp * sinf(px * two_pi);
   float drift_y = drift_amp * sinf(py * two_pi + 0.7854f /* π/4 */);

   lv_obj_set_pos(s_spec, s_spec_rest_x_eff + (int)(dx + drift_x), s_spec_rest_y_eff + (int)(dy + drift_y));
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
   /* PR 2 polish: during pipeline RECORDING (vs ambient LISTENING) bump
    * the floor + ceiling so the halo reads as a confident "I'm listening"
    * glow even when the user isn't speaking, plus add a 2-second sin
    * breath so the orb feels alive instead of static.  Outside pipeline
    * RECORDING, fall back to the ambient bloom envelope.  */
   int floor_v = ORB_HALO_OPA_FLOOR;
   int ceil_v = ORB_HALO_OPA_CEILING;
   if (s_pipeline.state == DICT_RECORDING) {
      /* TT #541: louder bloom envelope for a hero-scale orb.  Floor is
       * a confident "I'm listening" glow even when the user is silent;
       * ceiling is roomy enough to read voice peaks against the bigger
       * body without saturating. */
      floor_v = 150;
      ceil_v = 220;
      /* Slow breath: 0.5 Hz triangle wave on the floor itself, ±20 opa. */
      uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
      uint32_t phase = t_ms % 2000;
      int delta = (phase < 1000) ? (int)(phase / 25) : (int)((2000 - phase) / 25); /* 0..40 */
      floor_v += (delta - 20);
   }
   int opa = floor_v + (int)((float)(ceil_v - floor_v) * s_bloom_rms_ema);
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
   if (s_halo) {
      lv_obj_set_style_bg_opa(s_halo, 0, LV_PART_MAIN);
   }
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

   /* PR 2 polish: sonar ripple rings — two phase-offset transparent
    * circles that grow outward from the orb body and fade.  Hidden by
    * default; shown only during DICT_RECORDING.  Created BEFORE s_halo
    * so their z-order is behind everything (the body fully masks the
    * inner portion; only the outer expanding ring is visible).  Two
    * rings 50% out of phase so the ripple cadence feels continuous
    * instead of pulsing once a cycle. */
   s_ripple_a = lv_obj_create(parent);
   lv_obj_remove_style_all(s_ripple_a);
   lv_obj_set_size(s_ripple_a, ORB_SIZE, ORB_SIZE);
   lv_obj_set_pos(s_ripple_a, cx - (ORB_SIZE / 2), cy - (ORB_SIZE / 2));
   lv_obj_set_style_radius(s_ripple_a, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_bg_opa(s_ripple_a, 0, 0); /* outline only */
   lv_obj_set_style_border_width(s_ripple_a, 3, 0);
   lv_obj_set_style_border_color(s_ripple_a, lv_color_hex(0xFF5C50), 0);
   lv_obj_set_style_border_opa(s_ripple_a, 0, 0); /* invisible at rest */
   lv_obj_clear_flag(s_ripple_a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(s_ripple_a, LV_OBJ_FLAG_HIDDEN);

   s_ripple_b = lv_obj_create(parent);
   lv_obj_remove_style_all(s_ripple_b);
   lv_obj_set_size(s_ripple_b, ORB_SIZE, ORB_SIZE);
   lv_obj_set_pos(s_ripple_b, cx - (ORB_SIZE / 2), cy - (ORB_SIZE / 2));
   lv_obj_set_style_radius(s_ripple_b, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_bg_opa(s_ripple_b, 0, 0);
   lv_obj_set_style_border_width(s_ripple_b, 3, 0);
   lv_obj_set_style_border_color(s_ripple_b, lv_color_hex(0xFF5C50), 0);
   lv_obj_set_style_border_opa(s_ripple_b, 0, 0);
   lv_obj_clear_flag(s_ripple_b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(s_ripple_b, LV_OBJ_FLAG_HIDDEN);

   /* PR 2 polish: SAVED burst — one-shot expanding green ring on
    * DICT_SAVED entry.  Created behind everything; only visible during
    * the 700 ms burst.  Same border-only outline style as the ripples. */
   s_saved_burst = lv_obj_create(parent);
   lv_obj_remove_style_all(s_saved_burst);
   lv_obj_set_size(s_saved_burst, ORB_SIZE, ORB_SIZE);
   lv_obj_set_pos(s_saved_burst, cx - (ORB_SIZE / 2), cy - (ORB_SIZE / 2));
   lv_obj_set_style_radius(s_saved_burst, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_bg_opa(s_saved_burst, 0, 0);
   lv_obj_set_style_border_width(s_saved_burst, 4, 0);
   lv_obj_set_style_border_color(s_saved_burst, lv_color_hex(0x4ADE80), 0);
   lv_obj_set_style_border_opa(s_saved_burst, 0, 0);
   lv_obj_clear_flag(s_saved_burst, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(s_saved_burst, LV_OBJ_FLAG_HIDDEN);

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
   /* TT #547 attempted halo corona via outer shadow but even 24 px
    * blur on a 380 px halo at 5 Hz invalidation crushed the UI task
    * mutex budget.  Reverted — sticking with the breath/bloom bg_opa
    * animation that already gives a soft halo-to-bg transition. */
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
   lv_obj_set_style_bg_color(s_spec, lv_color_hex(0xFFF8E8), 0);
   /* TT #547: bigger ellipse + brighter peak.  Tried adding a shadow
    * for sun-glint glow but the per-tilt-tick (20 Hz) re-render of a
    * blurred shadow blew the LVGL render budget — reverted to flat
    * fill.  The 96×56 size + 140 opa peak alone gives a brighter,
    * wetter highlight than the 70×44 / 110 baseline. */
   lv_obj_set_style_bg_opa(s_spec, 140, 0);
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

   /* PR 2 polish: rotating arc for TRANSCRIBING — sibling AFTER s_body
    * so the arc draws ON TOP of the body's edge.  Created with the
    * main/knob/indicator parts neutralised, indicator carries the
    * visible sweep that the tick callback rotates. */
   s_thinking_arc = lv_arc_create(parent);
   if (s_thinking_arc) {
      lv_obj_remove_style(s_thinking_arc, NULL, LV_PART_KNOB);
      lv_obj_set_size(s_thinking_arc, THINKING_ARC_DIAM, THINKING_ARC_DIAM);
      lv_obj_set_pos(s_thinking_arc, cx - THINKING_ARC_DIAM / 2, cy - THINKING_ARC_DIAM / 2);
      lv_arc_set_bg_angles(s_thinking_arc, 0, 360);
      lv_arc_set_angles(s_thinking_arc, 0, THINKING_ARC_SWEEP);
      lv_arc_set_value(s_thinking_arc, 0);
      lv_obj_set_style_arc_width(s_thinking_arc, 4, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(s_thinking_arc, lv_color_hex(0xFCD34D), LV_PART_INDICATOR);
      lv_obj_set_style_arc_opa(s_thinking_arc, LV_OPA_COVER, LV_PART_INDICATOR);
      lv_obj_set_style_arc_width(s_thinking_arc, 0, LV_PART_MAIN);
      lv_obj_set_style_arc_opa(s_thinking_arc, 0, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(s_thinking_arc, 0, LV_PART_MAIN);
      lv_obj_remove_flag(s_thinking_arc, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(s_thinking_arc, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(s_thinking_arc, LV_OBJ_FLAG_HIDDEN);
   }

   /* TT #547: sphere-native SPEAKING overlay.  A circular sibling that
    * sits exactly on top of s_body, animated bg_opa 0 → ~140 over the
    * SPEAKING duration.  Vertical bg_grad runs from cream-amber top to
    * brighter cream-amber bottom — combined with the body's existing
    * lit-sphere gradient underneath, the whole sphere appears to
    * brighten + warm uniformly as TTS plays.  Reads as the sphere
    * absorbing sound, no 2D chrome around it.  Same circle shape as
    * s_body so no clipping or bleed.  Drawing s_speak_fill BEFORE
    * s_spec ensures the specular highlight stays on top. */
   s_speak_fill = lv_obj_create(parent);
   if (s_speak_fill) {
      lv_obj_remove_style_all(s_speak_fill);
      lv_obj_set_size(s_speak_fill, ORB_SIZE, ORB_SIZE);
      lv_obj_set_pos(s_speak_fill, cx - ORB_SIZE / 2, cy - ORB_SIZE / 2);
      lv_obj_set_style_radius(s_speak_fill, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(s_speak_fill, lv_color_hex(0xFFD68A), 0);
      lv_obj_set_style_bg_grad_color(s_speak_fill, lv_color_hex(0xFFF0C8), 0);
      lv_obj_set_style_bg_grad_dir(s_speak_fill, LV_GRAD_DIR_VER, 0);
      lv_obj_set_style_bg_opa(s_speak_fill, 0, 0);
      lv_obj_set_style_border_width(s_speak_fill, 0, 0);
      lv_obj_remove_flag(s_speak_fill, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(s_speak_fill, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(s_speak_fill, LV_OBJ_FLAG_HIDDEN);
   }

   /* PR 2: hero caption below the orb for pipeline state — sized to read
    * as a primary status indicator (Montserrat Bold 22 px), not a
    * footnote.  Pure text, no unicode bullets — those rendered as
    * missing-glyph rectangles in the 16 px caption font and made the
    * label feel fragile.  Hidden by default; paint_pipeline_state shows
    * + colors the text per state. */
   s_orb_caption = lv_label_create(parent);
   if (s_orb_caption) {
      lv_label_set_text(s_orb_caption, "");
      lv_obj_set_style_text_color(s_orb_caption, lv_color_hex(0xF8F8FB), 0);
      lv_obj_set_style_text_font(s_orb_caption, FONT_HEADING, 0);
      lv_obj_set_style_text_letter_space(s_orb_caption, 1, 0);
      lv_obj_align_to(s_orb_caption, s_body, LV_ALIGN_OUT_BOTTOM_MID, 0, 22);
      lv_obj_move_foreground(s_orb_caption);
      lv_obj_add_flag(s_orb_caption, LV_OBJ_FLAG_HIDDEN);
   }

   /* IDLE state arms tilt drift + idle breath + always-alive motion. */
   tilt_start();
   idle_breath_start();
   alive_start();
   /* TT #545: arm sleep cycle + wake on every body press.  ui_home
    * already wires its own click/long-press handlers; this is a
    * separate LV_EVENT_PRESSED hook that fires earlier (on touch-
    * down, not on tap release) so wake feels instantaneous. */
   if (s_body) {
      lv_obj_add_event_cb(s_body, orb_body_press_cb, LV_EVENT_PRESSED, NULL);
   }
   sleep_cycle_start();

   /* PR 2: subscribe to the dictation pipeline.  Callbacks land on the
    * LVGL thread thanks to the _lvgl variant.  Static guard so a
    * subsequent create/destroy/create cycle doesn't double-subscribe. */
   static int s_pipeline_sub = -1;
   if (s_pipeline_sub < 0) {
      s_pipeline_sub = voice_dictation_subscribe_lvgl(orb_pipeline_cb, NULL);
      ESP_LOGI(TAG, "pipeline subscriber registered handle=%d", s_pipeline_sub);
   }
   /* Rehydrate from current pipeline state — the subscriber only fires on
    * future transitions, so a re-created orb stays at IDLE visuals even
    * when the pipeline is mid-cycle.  Read current state and drive paint
    * immediately. */
   {
      dict_event_t cur = voice_dictation_get();
      ui_orb_set_pipeline_state(&cur);
   }

   ESP_LOGI(TAG, "orb created at (%d, %d), size %d", cx, cy, ORB_SIZE);
}

void ui_orb_destroy(void) {
   /* Stop timers first — their callbacks dereference module statics,
    * so they must not be scheduled past the screen tear-down. */
   tilt_stop();
   bloom_stop();
   idle_breath_stop();
   body_pulse_stop();
   sleep_cycle_stop();
   tts_scrubber_stop();
   alive_stop();
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
   /* PR 2: tear down caption + saved-fade timer + record-elapsed timer. */
   if (s_orb_caption) {
      lv_obj_del(s_orb_caption);
      s_orb_caption = NULL;
   }
   if (s_saved_fade_timer) {
      lv_timer_del(s_saved_fade_timer);
      s_saved_fade_timer = NULL;
   }
   if (s_rec_timer_label) {
      lv_timer_del(s_rec_timer_label);
      s_rec_timer_label = NULL;
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
   /* TT #545: any non-IDLE state is a wake event — full opa + reset
    * idle timer.  Re-entry to IDLE just stamps the timer; sleep_tick
    * decides when to drowsy/sleep based on subsequent idle duration. */
   if (s != ORB_STATE_IDLE) {
      ui_orb_wake();
   } else {
      s_last_interaction_ms = (uint32_t)(esp_timer_get_time() / 1000);
   }

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

   /* TT #543: idle breath halo opa pulse runs in IDLE only.  Stop on
    * any other state so bloom (LISTENING) / speaking-halo (SPEAKING) /
    * pipeline state owners can drive s_halo without contention. */
   if (s == ORB_STATE_IDLE) {
      idle_breath_start();
      alive_start();
   } else {
      idle_breath_stop();
      alive_stop();
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
      /* TT #545: TTS scrubber arc fills clockwise across the orb rim
       * over the duration of the SPEAKING state.  6 s default; if TTS
       * runs longer the indicator plateaus at 100% (still reads as
       * "still talking").  If shorter, tts_scrubber_stop fires below. */
      tts_scrubber_start(TTS_SCRUBBER_DEFAULT_MS);
   } else if (s != ORB_STATE_SPEAKING && prev == ORB_STATE_SPEAKING) {
      speaking_exit();
      tts_scrubber_stop();
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

/* ── Sleep cycle (TT #545) ───────────────────────────────────────────── */

/* The orb tracks user/voice interactivity and progressively dims when
 * left alone.  Three phases:
 *
 *   AWAKE   — full opa (255), 4 s breath.
 *   DROWSY  — opa ~190, 8 s breath.  After 5 min idle.
 *   ASLEEP  — opa ~120, 14 s breath.  After 30 min idle.
 *
 * Wake events: any non-IDLE state transition, any orb body tap, or an
 * explicit ui_orb_wake() call from outside (e.g. screen touch from
 * ui_home).  Wake ramps opa back to full over 400 ms.
 *
 * Sleep dim runs ONLY while in ORB_STATE_IDLE and the pipeline is not
 * active — active dictation or any non-idle state keeps the orb at
 * full brightness regardless of idle duration. */
#define SLEEP_TICK_MS 5000 /* check every 5 s for snappy transitions */
#define SLEEP_DROWSY_AT_MS (5 * 60 * 1000)
#define SLEEP_ASLEEP_AT_MS (30 * 60 * 1000)
#define SLEEP_AWAKE_OPA 255
#define SLEEP_DROWSY_OPA 190
#define SLEEP_ASLEEP_OPA 120
#define SLEEP_FADE_MS 400

typedef enum {
   SLEEP_AWAKE = 0,
   SLEEP_DROWSY,
   SLEEP_ASLEEP,
} sleep_phase_t;

static lv_timer_t *s_sleep_timer = NULL;
static sleep_phase_t s_sleep_phase = SLEEP_AWAKE;

static void sleep_apply_phase(sleep_phase_t phase) {
   if (phase == s_sleep_phase) return;
   int target_opa;
   uint32_t breath_ms;
   switch (phase) {
      case SLEEP_DROWSY:
         target_opa = SLEEP_DROWSY_OPA;
         breath_ms = IDLE_BREATH_DROWSY_MS;
         break;
      case SLEEP_ASLEEP:
         target_opa = SLEEP_ASLEEP_OPA;
         breath_ms = IDLE_BREATH_ASLEEP_MS;
         break;
      case SLEEP_AWAKE:
      default:
         target_opa = SLEEP_AWAKE_OPA;
         breath_ms = IDLE_BREATH_AWAKE_MS;
         break;
   }
   s_sleep_phase = phase;
   s_idle_breath_period_ms = breath_ms;
   ESP_LOGI(TAG, "sleep phase → %d (opa=%d, breath=%lums)", (int)phase, target_opa, (unsigned long)breath_ms);

   if (!s_body) return;
   int cur = lv_obj_get_style_opa(s_body, LV_PART_MAIN);
   lv_anim_delete(s_body, presence_opa_anim_cb);
   lv_anim_t a;
   lv_anim_init(&a);
   lv_anim_set_var(&a, s_body);
   lv_anim_set_exec_cb(&a, presence_opa_anim_cb);
   lv_anim_set_values(&a, cur, target_opa);
   lv_anim_set_time(&a, SLEEP_FADE_MS);
   lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
   lv_anim_start(&a);
}

static void sleep_tick_cb(lv_timer_t *t) {
   (void)t;
   /* Active states keep the orb awake; dictation pipeline does too. */
   if (s_state != ORB_STATE_IDLE || ui_orb_pipeline_active()) {
      /* Refresh the interaction stamp so we don't fall straight into
       * DROWSY the instant we return to IDLE. */
      s_last_interaction_ms = (uint32_t)(esp_timer_get_time() / 1000);
      return;
   }
   uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
   uint32_t idle = now - s_last_interaction_ms;
   sleep_phase_t target;
   if (idle >= SLEEP_ASLEEP_AT_MS)
      target = SLEEP_ASLEEP;
   else if (idle >= SLEEP_DROWSY_AT_MS)
      target = SLEEP_DROWSY;
   else
      target = SLEEP_AWAKE;
   sleep_apply_phase(target);
}

static void sleep_cycle_start(void) {
   if (s_sleep_timer) return;
   s_last_interaction_ms = (uint32_t)(esp_timer_get_time() / 1000);
   s_sleep_phase = SLEEP_AWAKE;
   s_idle_breath_period_ms = IDLE_BREATH_AWAKE_MS;
   s_sleep_timer = lv_timer_create(sleep_tick_cb, SLEEP_TICK_MS, NULL);
}

static void sleep_cycle_stop(void) {
   if (s_sleep_timer) {
      lv_timer_del(s_sleep_timer);
      s_sleep_timer = NULL;
   }
   s_sleep_phase = SLEEP_AWAKE;
   s_idle_breath_period_ms = IDLE_BREATH_AWAKE_MS;
}

void ui_orb_wake(void) {
   s_last_interaction_ms = (uint32_t)(esp_timer_get_time() / 1000);
   if (s_sleep_phase != SLEEP_AWAKE) {
      sleep_apply_phase(SLEEP_AWAKE);
   }
}

/* ── Always-alive sphere (TT #549) ───────────────────────────────────── */

/* Two slow infinite anims layered on the body + spec so the sphere
 * never reads as static.  Both pause when the orb leaves IDLE
 * (state-owned motion takes priority) and when the dictation
 * pipeline is active (the pipeline tints/fills the body itself). */

static void alive_grad_anim_cb(void *obj, int32_t v) {
   if (!obj) return;
   /* Maps v in [0, 100] to bg_main_stop in [0, 28].  The lit-side stop
    * extends downward then retracts, slowly shifting the lit-shadow
    * boundary on the sphere — reads as a planet's terminator
    * breathing inward and outward. */
   int stop = v * 28 / 100;
   lv_obj_set_style_bg_main_stop((lv_obj_t *)obj, stop, LV_PART_MAIN);
}

static void alive_spec_anim_cb(void *obj, int32_t v) {
   if (!obj) return;
   lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

static void alive_start(void) {
   if (s_alive_running) return;
   s_alive_running = true;
   if (s_body) {
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, s_body);
      lv_anim_set_exec_cb(&a, alive_grad_anim_cb);
      lv_anim_set_values(&a, 0, 100);
      lv_anim_set_time(&a, 30000);
      lv_anim_set_playback_time(&a, 30000);
      lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
      lv_anim_start(&a);
   }
   if (s_spec) {
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, s_spec);
      lv_anim_set_exec_cb(&a, alive_spec_anim_cb);
      lv_anim_set_values(&a, 120, 160);
      lv_anim_set_time(&a, 7000);
      lv_anim_set_playback_time(&a, 7000);
      lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
      lv_anim_start(&a);
   }
}

static void alive_stop(void) {
   if (!s_alive_running) return;
   s_alive_running = false;
   if (s_body) {
      lv_anim_delete(s_body, alive_grad_anim_cb);
      /* Snap back to the canonical full-range gradient. */
      lv_obj_set_style_bg_main_stop(s_body, 0, LV_PART_MAIN);
   }
   if (s_spec) {
      lv_anim_delete(s_spec, alive_spec_anim_cb);
      lv_obj_set_style_bg_opa(s_spec, 140, LV_PART_MAIN);
   }
}

/* ── SPEAKING fill — sphere-native progress (TT #547) ────────────────── */

/* Opa animation drives the bright cream-amber overlay fading in over
 * the SPEAKING duration.  The orb visibly warms + brightens as TTS
 * plays.  Plateaus at peak opa if SPEAKING runs past the estimate;
 * tts_scrubber_stop fades it out + hides. */
static void speak_fill_anim_cb(void *obj, int32_t v) {
   if (!obj) return;
   lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

#define SPEAK_FILL_PEAK_OPA 140

static void tts_scrubber_start(uint32_t duration_ms) {
   if (!s_speak_fill) return;
   if (duration_ms == 0) duration_ms = TTS_SCRUBBER_DEFAULT_MS;
   lv_anim_delete(s_speak_fill, speak_fill_anim_cb);
   lv_obj_set_style_bg_opa(s_speak_fill, 0, LV_PART_MAIN);
   lv_obj_clear_flag(s_speak_fill, LV_OBJ_FLAG_HIDDEN);
   lv_anim_t a;
   lv_anim_init(&a);
   lv_anim_set_var(&a, s_speak_fill);
   lv_anim_set_exec_cb(&a, speak_fill_anim_cb);
   lv_anim_set_values(&a, 0, SPEAK_FILL_PEAK_OPA);
   lv_anim_set_time(&a, duration_ms);
   lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
   lv_anim_start(&a);
}

static void tts_scrubber_stop(void) {
   if (!s_speak_fill) return;
   lv_anim_delete(s_speak_fill, speak_fill_anim_cb);
   /* Quick fade-out instead of instant cut so the brightness doesn't
    * pop off the moment SPEAKING ends. */
   int cur = lv_obj_get_style_bg_opa(s_speak_fill, LV_PART_MAIN);
   if (cur > 0) {
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, s_speak_fill);
      lv_anim_set_exec_cb(&a, speak_fill_anim_cb);
      lv_anim_set_values(&a, cur, 0);
      lv_anim_set_time(&a, 300);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
      lv_anim_start(&a);
   } else {
      lv_obj_add_flag(s_speak_fill, LV_OBJ_FLAG_HIDDEN);
   }
}

static void orb_body_press_cb(lv_event_t *e) {
   (void)e;
   /* Any press on the orb body counts as an interaction — wake even if
    * the press doesn't escalate to a state transition (long-press menu,
    * cancel mid-listen, etc.). */
   ui_orb_wake();
}

void ui_orb_ripple_for_tool(const char *tool_name) {
   (void)tool_name;
   /* Step 6 replaces with comet anim into PROCESSING. */
   ui_orb_set_state(ORB_STATE_PROCESSING);
}

/* ── Accessors ───────────────────────────────────────────────────────── */

lv_obj_t *ui_orb_get_root(void) { return s_root; }

lv_obj_t *ui_orb_get_body(void) { return s_body; }

/* ── Pipeline state (PR 2) ───────────────────────────────────────────── */

/* PR 2: paint the halo (s_halo) in the pipeline-state hue so the
 * LISTENING bloom + steady SPEAKING halo blend with the body tint
 * instead of bleeding the default cream-amber over a red/amber/green
 * body.  Keep the existing opacity envelope (bloom timer + speaking
 * fade); we only retune the color stop. */
static void paint_pipeline_halo(uint32_t color_hex) {
   if (!s_halo) return;
   lv_obj_set_style_bg_color(s_halo, lv_color_hex(color_hex), LV_PART_MAIN);
}

/* Reset halo to its default cream-amber baseline (used on DICT_IDLE
 * so subsequent voice-state visuals — IDLE/LISTENING/PROCESSING/
 * SPEAKING — read with their original palette). */
static void reset_pipeline_halo(void) {
   if (!s_halo) return;
   lv_obj_set_style_bg_color(s_halo, lv_color_hex(0xFFB870), LV_PART_MAIN);
}

/* PR 2 polish: sonar ripple drive.  Each ring grows from the body's
 * diameter out to a comfortable margin past the halo, fading its
 * border opacity from a confident peak to invisible over the cycle.
 * Two rings 50% out of phase keep the cadence continuous.  Tick at
 * ~60 Hz for smooth interpolation. */
#define RIPPLE_CYCLE_MS 1800
#define RIPPLE_MIN_PX ORB_SIZE
#define RIPPLE_MAX_PX (ORB_HALO_DIAM + 60)
#define RIPPLE_PEAK_OPA 180

static void ripple_paint_one(lv_obj_t *o, float frac) {
   if (!o) return;
   /* Ease-out for the geometry growth so the ring decelerates as it
    * expands — feels like a real ripple slowing as it propagates. */
   float eased = 1.0f - (1.0f - frac) * (1.0f - frac);
   int span = RIPPLE_MAX_PX - RIPPLE_MIN_PX;
   int size = RIPPLE_MIN_PX + (int)(eased * (float)span);
   lv_obj_set_size(o, size, size);
   lv_obj_set_pos(o, s_orb_cx - size / 2, s_orb_cy - size / 2);
   /* Opacity falls linearly from peak to 0 over the cycle. */
   int opa = (int)((1.0f - frac) * RIPPLE_PEAK_OPA);
   if (opa < 0) opa = 0;
   if (opa > 255) opa = 255;
   lv_obj_set_style_border_opa(o, (lv_opa_t)opa, LV_PART_MAIN);
}

static void ripple_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_ripple_a || !s_ripple_b) return;
   uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
   uint32_t phase_a = t_ms % RIPPLE_CYCLE_MS;
   uint32_t phase_b = (t_ms + RIPPLE_CYCLE_MS / 2) % RIPPLE_CYCLE_MS;
   ripple_paint_one(s_ripple_a, (float)phase_a / (float)RIPPLE_CYCLE_MS);
   ripple_paint_one(s_ripple_b, (float)phase_b / (float)RIPPLE_CYCLE_MS);
}

static void ripple_start(uint32_t color_hex) {
   if (!s_ripple_a || !s_ripple_b) return;
   lv_obj_set_style_border_color(s_ripple_a, lv_color_hex(color_hex), LV_PART_MAIN);
   lv_obj_set_style_border_color(s_ripple_b, lv_color_hex(color_hex), LV_PART_MAIN);
   lv_obj_clear_flag(s_ripple_a, LV_OBJ_FLAG_HIDDEN);
   lv_obj_clear_flag(s_ripple_b, LV_OBJ_FLAG_HIDDEN);
   if (!s_ripple_timer) {
      s_ripple_timer = lv_timer_create(ripple_tick_cb, 16, NULL);
   }
}

static void ripple_stop(void) {
   if (s_ripple_timer) {
      lv_timer_del(s_ripple_timer);
      s_ripple_timer = NULL;
   }
   if (s_ripple_a) lv_obj_add_flag(s_ripple_a, LV_OBJ_FLAG_HIDDEN);
   if (s_ripple_b) lv_obj_add_flag(s_ripple_b, LV_OBJ_FLAG_HIDDEN);
}

/* ── TRANSCRIBING rotating arc ───────────────────────────────────────── */

/* A 90°-sweep amber arc orbits around the body.  Reads as a "loading
 * spinner" — clearer "I'm thinking" than the existing comet's tiny dot.
 * Geometry defines hoisted to the top of the file. */

static void thinking_arc_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_thinking_arc) return;
   uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
   int rotation = (int)((t_ms * 360 / THINKING_ARC_PERIOD_MS) % 360);
   lv_arc_set_angles(s_thinking_arc, rotation, (rotation + THINKING_ARC_SWEEP) % 360);
}

static void thinking_arc_start(uint32_t color_hex) {
   if (!s_thinking_arc) return;
   lv_obj_set_style_arc_color(s_thinking_arc, lv_color_hex(color_hex), LV_PART_INDICATOR);
   lv_obj_clear_flag(s_thinking_arc, LV_OBJ_FLAG_HIDDEN);
   if (!s_thinking_arc_timer) {
      s_thinking_arc_timer = lv_timer_create(thinking_arc_tick_cb, 16, NULL);
   }
}

static void thinking_arc_stop(void) {
   if (s_thinking_arc_timer) {
      lv_timer_del(s_thinking_arc_timer);
      s_thinking_arc_timer = NULL;
   }
   if (s_thinking_arc) lv_obj_add_flag(s_thinking_arc, LV_OBJ_FLAG_HIDDEN);
}

/* ── SAVED celebratory burst ─────────────────────────────────────────── */

/* On DICT_SAVED entry, a single green ring flies outward + fades in
 * ~700 ms.  Self-stops once the cycle completes. */
#define SAVED_BURST_DURATION_MS 700
#define SAVED_BURST_MIN_PX ORB_SIZE
#define SAVED_BURST_MAX_PX (ORB_HALO_DIAM + 80)
#define SAVED_BURST_PEAK_OPA 230

static void saved_burst_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_saved_burst) return;
   uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
   uint32_t elapsed = (now_ms >= s_saved_burst_t0) ? (now_ms - s_saved_burst_t0) : 0;
   if (elapsed >= SAVED_BURST_DURATION_MS) {
      if (s_saved_burst_timer) {
         lv_timer_del(s_saved_burst_timer);
         s_saved_burst_timer = NULL;
      }
      lv_obj_add_flag(s_saved_burst, LV_OBJ_FLAG_HIDDEN);
      return;
   }
   float frac = (float)elapsed / (float)SAVED_BURST_DURATION_MS;
   /* Cubic ease-out so the burst flies out fast then settles. */
   float inv = 1.0f - frac;
   float eased = 1.0f - inv * inv * inv;
   int span = SAVED_BURST_MAX_PX - SAVED_BURST_MIN_PX;
   int size = SAVED_BURST_MIN_PX + (int)(eased * (float)span);
   lv_obj_set_size(s_saved_burst, size, size);
   lv_obj_set_pos(s_saved_burst, s_orb_cx - size / 2, s_orb_cy - size / 2);
   int opa = (int)((1.0f - frac) * SAVED_BURST_PEAK_OPA);
   lv_obj_set_style_border_opa(s_saved_burst, (lv_opa_t)opa, LV_PART_MAIN);
}

static void saved_burst_fire(void) {
   if (!s_saved_burst) return;
   s_saved_burst_t0 = (uint32_t)(esp_timer_get_time() / 1000);
   lv_obj_set_size(s_saved_burst, SAVED_BURST_MIN_PX, SAVED_BURST_MIN_PX);
   lv_obj_set_pos(s_saved_burst, s_orb_cx - SAVED_BURST_MIN_PX / 2, s_orb_cy - SAVED_BURST_MIN_PX / 2);
   lv_obj_set_style_border_opa(s_saved_burst, SAVED_BURST_PEAK_OPA, LV_PART_MAIN);
   lv_obj_clear_flag(s_saved_burst, LV_OBJ_FLAG_HIDDEN);
   if (!s_saved_burst_timer) {
      s_saved_burst_timer = lv_timer_create(saved_burst_tick_cb, 16, NULL);
   }
}

/* ── Body heartbeat during RECORDING ─────────────────────────────────── */

/* Subtle ±2.3% scale pulse on s_body, 1.2 s cycle.  Adds a literal
 * heartbeat feel on top of sonar ripples + halo breath.  LVGL 9's
 * transform_scale uses 256 = 1.0×; pivot snapped to body centre. */
#define BODY_PULSE_PERIOD_MS 1200
#define BODY_PULSE_AMPLITUDE 6

static void body_pulse_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_body) return;
   uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
   uint32_t phase = t_ms % BODY_PULSE_PERIOD_MS;
   float half = BODY_PULSE_PERIOD_MS / 2.0f;
   float tri = (phase < half) ? ((float)phase / half) : (1.0f - ((float)phase - half) / half);
   int scale = 256 + (int)((tri * 2.0f - 1.0f) * (float)BODY_PULSE_AMPLITUDE);
   lv_obj_set_style_transform_scale_x(s_body, scale, LV_PART_MAIN);
   lv_obj_set_style_transform_scale_y(s_body, scale, LV_PART_MAIN);
}

static void body_pulse_start(void) {
   if (!s_body || s_body_pulse_timer) return;
   lv_obj_set_style_transform_pivot_x(s_body, ORB_SIZE / 2, LV_PART_MAIN);
   lv_obj_set_style_transform_pivot_y(s_body, ORB_SIZE / 2, LV_PART_MAIN);
   s_body_pulse_timer = lv_timer_create(body_pulse_tick_cb, 16, NULL);
}

static void body_pulse_stop(void) {
   if (s_body_pulse_timer) {
      lv_timer_del(s_body_pulse_timer);
      s_body_pulse_timer = NULL;
   }
   if (s_body) {
      lv_obj_set_style_transform_scale_x(s_body, 256, LV_PART_MAIN);
      lv_obj_set_style_transform_scale_y(s_body, 256, LV_PART_MAIN);
   }
}

/* ── Idle breath (TT #543) ───────────────────────────────────────────── */

/* Very slow, very subtle halo opacity pulse, IDLE-only.  Reads as
 * "alive without trying."  Initial implementation used transform_scale
 * on s_body but that forced gradient re-rasterization on every tick,
 * stalling the UI task long enough to trip the watchdog.  Halo opacity
 * is the same mechanism as the existing voice bloom — solid color, no
 * gradient, cheap repaint.  Animates between 0 and 28 opa over 4 s
 * sine path so the orb's outer rim softly respires.  Bloom and breath
 * share s_halo but never run concurrently (bloom = LISTENING only;
 * breath = IDLE only, plus pipeline-active yields). */
/* TT #545: AWAKE/DROWSY/ASLEEP breath periods + s_idle_breath_period_ms
 * are declared up at the top of the file with the other layout
 * constants (because sleep_apply_phase needs them earlier). */
#define IDLE_BREATH_AMPLITUDE 28 /* peak halo opa during breath */
static int s_idle_breath_last_opa = 0;

/* TT #549 always-alive sphere — slow body gradient-stop pan + slow
 * spec opa breath, both IDLE-only.  Pause during LISTENING /
 * PROCESSING / SPEAKING and any pipeline-active state so the
 * state-owned motion isn't competing.  (s_alive_running is forward-
 * declared near the top of the file.) */

static void idle_breath_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_halo) return;
   /* Pipeline states own the halo — RECORDING uses the bloom envelope,
    * the speaking-halo fade owns it during SPEAKING.  Yield. */
   if (ui_orb_pipeline_active()) return;
   if (s_state != ORB_STATE_IDLE) return;
   uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
   uint32_t period = s_idle_breath_period_ms ? s_idle_breath_period_ms : IDLE_BREATH_AWAKE_MS;
   float phase = (float)(t_ms % period) / (float)period;
   /* sin(2π·phase) → half-rectified so the halo only adds (never goes
    * negative); ramps 0 → A → 0 over the cycle. */
   float s = sinf(phase * 6.28318530718f);
   if (s < 0.0f) s = 0.0f;
   int opa = (int)(s * (float)IDLE_BREATH_AMPLITUDE);
   if (opa == s_idle_breath_last_opa) return;
   s_idle_breath_last_opa = opa;
   lv_obj_set_style_bg_opa(s_halo, (lv_opa_t)opa, LV_PART_MAIN);
}

static void idle_breath_start(void) {
   if (!s_halo || s_idle_breath_timer) return;
   s_idle_breath_last_opa = 0;
   /* 200 ms tick = 5 Hz; over a 4 s cycle that's 20 samples — smooth
    * enough for a ~28 opa pulse without burning render budget. */
   s_idle_breath_timer = lv_timer_create(idle_breath_tick_cb, 200, NULL);
}

static void idle_breath_stop(void) {
   if (s_idle_breath_timer) {
      lv_timer_del(s_idle_breath_timer);
      s_idle_breath_timer = NULL;
   }
   /* Don't force halo opa here — bloom_start / speaking halo / pipeline
    * state owners may be about to drive it. Each of those callers sets
    * the opa they want on enter. */
   s_idle_breath_last_opa = 0;
}

/* Paint the orb body in the pipeline-state tint.  Caller is responsible
 * for setting the caption text + showing the caption label.
 *
 * s_body is a vertical linear gradient (paint_body_for_hour /
 * paint_for_tone write both bg_color = top stop and bg_grad_color =
 * bottom stop).  Writing only bg_color leaves the bottom stop at the
 * previous state's edge color, which produces a muddy mix instead of
 * a clean state indicator.  Darken the requested hue ~45% for the
 * bottom stop to keep the lit-sphere luster instead of going flat;
 * stamp s_last_painted_hour = -1 so a clock-driven hour-repaint can't
 * immediately revert the tint. */
static void paint_pipeline_body(uint32_t color_hex) {
   if (!s_body) return;
   uint8_t r = (color_hex >> 16) & 0xFF;
   uint8_t g = (color_hex >> 8) & 0xFF;
   uint8_t b = color_hex & 0xFF;
   uint32_t bot = ((uint32_t)((r * 45) / 100) << 16) | ((uint32_t)((g * 45) / 100) << 8) | (uint32_t)((b * 45) / 100);
   lv_obj_set_style_bg_color(s_body, lv_color_hex(color_hex), LV_PART_MAIN);
   lv_obj_set_style_bg_grad_color(s_body, lv_color_hex(bot), LV_PART_MAIN);
   lv_obj_set_style_bg_grad_dir(s_body, LV_GRAD_DIR_VER, LV_PART_MAIN);
   lv_obj_set_style_bg_opa(s_body, LV_OPA_COVER, LV_PART_MAIN);
   s_last_painted_hour = -1;
}

static const char *fail_reason_caption(dict_fail_t r) {
   switch (r) {
      case DICT_FAIL_AUTH:
         return "AUTH";
      case DICT_FAIL_NETWORK:
         return "NETWORK";
      case DICT_FAIL_EMPTY:
         return "EMPTY — got silence";
      case DICT_FAIL_NO_AUDIO:
         return "NO AUDIO";
      case DICT_FAIL_TOO_LONG:
         return "TOO LONG (5 min cap)";
      case DICT_FAIL_CANCELLED:
         return "CANCELLED";
      default:
         return "FAIL";
   }
}

static void set_caption_text(const char *text) {
   if (!s_orb_caption || !text) return;
   lv_label_set_text(s_orb_caption, text);
   /* Re-anchor the label after its size changed — lv_obj_align_to is a
    * one-shot in LVGL 9.x, so the original create-time alignment was
    * computed against a 0-width empty label and the rendered text drifted
    * to the right.  Re-call here so each state's text re-centers under
    * the orb. */
   if (s_body) {
      lv_obj_align_to(s_orb_caption, s_body, LV_ALIGN_OUT_BOTTOM_MID, 0, 22);
   }
   lv_obj_clear_flag(s_orb_caption, LV_OBJ_FLAG_HIDDEN);
}

static void hide_caption(void) {
   if (s_orb_caption) lv_obj_add_flag(s_orb_caption, LV_OBJ_FLAG_HIDDEN);
}

/* Timer cb for SAVED → IDLE auto-fade. */
static void saved_fade_to_idle_cb(lv_timer_t *t) {
   (void)t;
   s_saved_fade_timer = NULL;
   voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, (uint32_t)(esp_timer_get_time() / 1000));
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
   uint32_t dur_ms = (s_pipeline.started_ms && now_ms >= s_pipeline.started_ms) ? (now_ms - s_pipeline.started_ms) : 0;
   uint32_t s = dur_ms / 1000;
   char buf[40];
   snprintf(buf, sizeof(buf), "RECORDING  %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
   if (s_orb_caption) {
      lv_label_set_text(s_orb_caption, buf);
      if (s_body) lv_obj_align_to(s_orb_caption, s_body, LV_ALIGN_OUT_BOTTOM_MID, 0, 22);
   }
}

/* Subscriber bridge: voice_dictation fires this on the LVGL thread
 * (because we subscribe via voice_dictation_subscribe_lvgl).  Just
 * forward to the public state-driver. */
static void orb_pipeline_cb(const dict_event_t *event, void *user_data) {
   (void)user_data;
   if (event) {
      char detail[48];
      snprintf(detail, sizeof(detail), "%s/%s", voice_dictation_state_name(event->state),
               voice_dictation_fail_name(event->fail_reason));
      tab5_debug_obs_event("pipeline.orb_cb", detail);
   }
   ui_orb_set_pipeline_state(event);
}

void ui_orb_set_pipeline_state(const dict_event_t *event) {
   if (!event) return;
   s_pipeline = *event;

   /* TT #549: pause always-alive motion while the pipeline owns the
    * body's paint — gradient-stop pan + spec opa breath compete with
    * paint_pipeline_body's tint.  Resume on DICT_IDLE. */
   if (event->state != DICT_IDLE) {
      alive_stop();
   } else {
      if (s_state == ORB_STATE_IDLE) alive_start();
   }

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
         reset_pipeline_halo();
         ripple_stop();
         thinking_arc_stop();
         body_pulse_stop();
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
         paint_pipeline_halo(0xFF5C50); /* warm red halo glow */
         ripple_start(0xFF7A6F);        /* sonar rings expanding outward */
         thinking_arc_stop();
         body_pulse_start(); /* heartbeat pulse on body scale */
         uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
         uint32_t dur_ms = (event->started_ms && now_ms >= event->started_ms) ? (now_ms - event->started_ms) : 0;
         uint32_t s = dur_ms / 1000;
         snprintf(buf, sizeof(buf), "RECORDING  %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
         lv_obj_set_style_text_color(s_orb_caption, lv_color_hex(0xFFD2CC), 0);
         set_caption_text(buf);
         /* Spawn the 200 ms tick that keeps the M:SS caption live. */
         if (!s_rec_timer_label) {
            s_rec_timer_label = lv_timer_create(rec_timer_label_cb, 200, NULL);
         }
         break;
      }

      case DICT_UPLOADING:
         ripple_stop();
         body_pulse_stop();
         thinking_arc_start(0xFCD34D); /* amber spinner */
         paint_pipeline_body(0xF59E0B);
         paint_pipeline_halo(0xFFB347); /* amber glow */
         lv_obj_set_style_text_color(s_orb_caption, lv_color_hex(0xFFE4B5), 0);
         set_caption_text("UPLOADING");
         break;

      case DICT_TRANSCRIBING:
         ripple_stop();
         body_pulse_stop();
         thinking_arc_start(0xFCD34D); /* amber spinner */
         paint_pipeline_body(0xF59E0B);
         paint_pipeline_halo(0xFFB347); /* amber glow */
         lv_obj_set_style_text_color(s_orb_caption, lv_color_hex(0xFFE4B5), 0);
         set_caption_text("TRANSCRIBING");
         /* Reuse the existing PROCESSING comet animation for visual
          * continuity. */
         ui_orb_set_state(ORB_STATE_PROCESSING);
         break;

      case DICT_SAVED:
         ripple_stop();
         body_pulse_stop();
         thinking_arc_stop();
         saved_burst_fire(); /* one-shot expanding green ring */
         paint_pipeline_body(0x22C55E);
         paint_pipeline_halo(0x4ADE80); /* mint glow */
         lv_obj_set_style_text_color(s_orb_caption, lv_color_hex(0xCFFFE0), 0);
         set_caption_text("SAVED");
         /* Schedule auto-fade back to IDLE after 2 s.  Idempotent — if
          * one already exists (rapid SAVED re-entry), don't stack. */
         if (!s_saved_fade_timer) {
            s_saved_fade_timer = lv_timer_create(saved_fade_to_idle_cb, 2000, NULL);
            if (s_saved_fade_timer) lv_timer_set_repeat_count(s_saved_fade_timer, 1);
         }
         break;

      case DICT_FAILED:
         ripple_stop();
         body_pulse_stop();
         thinking_arc_stop();
         paint_pipeline_body(0xE74C3C);
         paint_pipeline_halo(0xFF5C50);
         /* Drop the unicode mid-dot — FONT_HEADING (montserrat bold 22)
          * doesn't carry U+00B7, so it rendered as a missing-glyph box.
          * Use generous spaces to keep the parts visually separated. */
         snprintf(buf, sizeof(buf), "%s   TAP TO RETRY", fail_reason_caption(event->fail_reason));
         lv_obj_set_style_text_color(s_orb_caption, lv_color_hex(0xFFD2CC), 0);
         set_caption_text(buf);
         break;
   }
}

bool ui_orb_pipeline_active(void) {
   /* Poll the authoritative state machine directly rather than the
    * cached s_pipeline.  s_pipeline is updated by the LVGL-async
    * subscriber, which runs AFTER any synchronous caller that resets
    * the pipeline state (e.g., orb_click_cb's pipeline-clear-before-Ask
    * path).  Reading voice_dictation_get() avoids a window where
    * is-pipeline-active returns stale true and suppresses the Ask
    * overlay's chrome. */
   dict_event_t e = voice_dictation_get();
   return e.state != DICT_IDLE;
}
