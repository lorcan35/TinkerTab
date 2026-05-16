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
/* Halo stayed sized at 260 (smaller than the body) intentionally —
 * pre-TT #547 the halo was meant to read through s_body's gradient
 * edge as a soft bleed, not to be a visible ring.  TT #553 follow-
 * up briefly tried 420 px for an explicit ring + user pivoted to
 * "in-orb" effects instead, so the halo stays its original size and
 * the always-alive motion now lives ON / IN the sphere. */
#define ORB_HALO_DIAM 260
/* TT #553 follow-up: in-orb core dot — sits visually INSIDE the sphere
 * and brightens with ambient sound.  130×130 = roughly 46 % of body
 * diameter — clearly visible but doesn't dominate.  Vertical gradient
 * (cream top, deeper amber bottom) reinforces the lit-sphere
 * direction. */
#define ORB_CORE_W 130
#define ORB_CORE_H 130
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
static lv_obj_t *s_inner_core = NULL; /* TT #553 follow-up: in-orb ambient core */
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
 * earlier.  TT #552 also forward-decls s_idle_breath_last_opa so the
 * ambient_apply helper can layer the ambient glow on top of the
 * existing idle breath value.  TT #553 follow-up forward-decls the
 * ambient EMA statics so ui_orb_get_motion_state can read them. */
static bool s_alive_running;
static int s_idle_breath_last_opa;
static float s_ambient_rms;
static float s_ambient_rms_target;
static int s_ambient_body_stop_last;

/* TT #555 FX state — all default off (current behaviour preserved). */
static ui_orb_fx_t s_fx = {0};
static lv_obj_t *s_glass_ring = NULL;      /* top-inside highlight when fx.glass */
static lv_timer_t *s_spin_timer = NULL;    /* drives transform_rotation when fx.spin */
static lv_timer_t *s_rainbow_timer = NULL; /* slow hue cycle when fx.rainbow */
static lv_timer_t *s_shake_timer = NULL;   /* IMU monitor for shake startle */
static int s_spin_angle_x10 = 0;           /* 0..3599, LVGL rotation is 0.1° units */
static float s_rainbow_hue = 0.0f;         /* 0..360 */
static uint32_t s_shake_recovery_until_ms = 0;
static int s_shake_scale_offset = 0;  /* 0 at rest, +N during startle */
static int s_body_baseline_opa = 255; /* restored when fx.glass turns off */

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

/* TT #559: banding-free body via lv_canvas + baked dither.  All paint
 * paths (circadian, tone, ambient brightening, rainbow, pipeline tint)
 * route through body_canvas_paint().  The canvas holds a pre-rendered
 * ARGB8888 image of the gradient with random ±3 noise dither applied
 * per pixel — visible RGB565 banding becomes high-frequency noise the
 * eye averages out as smooth color.  Outside-circle pixels written as
 * alpha=0 so the canvas reads as round without needing clip_corner. */
static uint8_t *s_body_canvas_buf = NULL;
#define BODY_CANVAS_BPP 4 /* ARGB8888 = B G R A in little-endian memory */
static uint32_t s_body_grad_top_color = 0xFFEBC4;
static uint32_t s_body_grad_bot_color = 0x5C3205;

static void body_canvas_paint(uint32_t top_hex, uint32_t bot_hex) {
   if (!s_body_canvas_buf || !s_body) return;
   s_body_grad_top_color = top_hex;
   s_body_grad_bot_color = bot_hex;
   int tr = (top_hex >> 16) & 0xFF;
   int tg = (top_hex >> 8) & 0xFF;
   int tb = top_hex & 0xFF;
   int br = (bot_hex >> 16) & 0xFF;
   int bg = (bot_hex >> 8) & 0xFF;
   int bb = bot_hex & 0xFF;
   const int N = ORB_SIZE;
   const int half = N / 2;
   const int r_lim = half - 1;
   const int r2 = r_lim * r_lim;
   /* Pseudo-random LCG for the dither — same dither pattern across
    * frames so the user doesn't see twinkling; we just want spatial
    * dither to break up bands, not temporal. */
   uint32_t seed = 0x1f9b3a7c;
   for (int y = 0; y < N; y++) {
      int dy = y - half;
      int dy2 = dy * dy;
      /* Per-row gradient interpolation (fixed point, 256× scale). */
      int t256 = (y * 256) / (N - 1);
      int row_r = tr + ((br - tr) * t256) / 256;
      int row_g = tg + ((bg - tg) * t256) / 256;
      int row_b = tb + ((bb - tb) * t256) / 256;
      for (int x = 0; x < N; x++) {
         int idx = (y * N + x) * BODY_CANVAS_BPP;
         int dx = x - half;
         if (dx * dx + dy2 > r2) {
            /* Outside the body circle — fully transparent. */
            s_body_canvas_buf[idx + 0] = 0;
            s_body_canvas_buf[idx + 1] = 0;
            s_body_canvas_buf[idx + 2] = 0;
            s_body_canvas_buf[idx + 3] = 0;
            continue;
         }
         /* LCG step + extract three small noise offsets. */
         seed = seed * 1664525u + 1013904223u;
         int n_r = (int)((seed >> 0) & 0x7) - 3; /* −3..+3 */
         int n_g = (int)((seed >> 8) & 0x7) - 3;
         int n_b = (int)((seed >> 16) & 0x7) - 3;
         int r = row_r + n_r;
         int g = row_g + n_g;
         int b = row_b + n_b;
         if (r < 0)
            r = 0;
         else if (r > 255)
            r = 255;
         if (g < 0)
            g = 0;
         else if (g > 255)
            g = 255;
         if (b < 0)
            b = 0;
         else if (b > 255)
            b = 255;
         s_body_canvas_buf[idx + 0] = (uint8_t)b;
         s_body_canvas_buf[idx + 1] = (uint8_t)g;
         s_body_canvas_buf[idx + 2] = (uint8_t)r;
         s_body_canvas_buf[idx + 3] = 255;
      }
   }
   lv_obj_invalidate(s_body);
}

static void paint_body_for_hour(int hour) {
   if (!s_body) return;
   uint32_t mid = 0, edge = 0;
   pick_circadian_palette(hour, &mid, &edge);
   (void)mid;
   uint32_t er = (edge >> 16) & 0xFF;
   uint32_t eg = (edge >> 8) & 0xFF;
   uint32_t eb = edge & 0xFF;
   er = (er * 7) / 10;
   eg = (eg * 7) / 10;
   eb = (eb * 7) / 10;
   uint32_t top = 0xFFEBC4;
   uint32_t bot = (er << 16) | (eg << 8) | eb;
   body_canvas_paint(top, bot);
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
   body_canvas_paint(top, bot);
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

   /* TT #559: lv_canvas-backed body for banding-free render.  ARGB8888
    * buffer in PSRAM, 280×280×4 = 313 KB.  Round shape is achieved by
    * writing alpha=0 to pixels outside the circle in body_canvas_paint,
    * so clip_corner is unnecessary (which was the TT #547 trap). */
   if (!s_body_canvas_buf) {
      s_body_canvas_buf = heap_caps_malloc(ORB_SIZE * ORB_SIZE * BODY_CANVAS_BPP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   }
   s_body = lv_canvas_create(parent);
   if (s_body_canvas_buf) {
      lv_canvas_set_buffer(s_body, s_body_canvas_buf, ORB_SIZE, ORB_SIZE, LV_COLOR_FORMAT_ARGB8888);
   }
   lv_obj_set_size(s_body, ORB_SIZE, ORB_SIZE);
   lv_obj_set_pos(s_body, cx - (ORB_SIZE / 2), cy - (ORB_SIZE / 2));
   lv_obj_add_flag(s_body, LV_OBJ_FLAG_CLICKABLE);
   lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
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

   /* TT #553 follow-up rev2: inner-core disc removed — read as a
    * "ball inside a ball" instead of the sphere lit from within.
    * The ambient drive now lives on s_body's bg_main_stop directly
    * (the sphere's lit-side hemisphere extends downward as sound
    * rises) for a genuinely in-sphere effect.  s_inner_core stays
    * declared NULL so any null-guard reads work.  */
   s_inner_core = NULL;

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
   s_inner_core = NULL;
   /* Keep s_body_canvas_buf allocated — it's PSRAM, reused across
    * create/destroy cycles; freeing + re-allocating would invite
    * heap fragmentation.  306 KB sits idle in PSRAM which has 16+ MB
    * free anyway. */
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

bool ui_orb_get_motion_state(ui_orb_motion_state_t *out) {
   if (!out) return false;
   if (!s_body) return false;
   out->ambient_rms = s_ambient_rms;
   out->ambient_rms_target = s_ambient_rms_target;
   /* `core_opa` field reused for the body-stop value when no inner
    * core is present.  Polling clients should plot it as either; we
    * use the same JSON key for backward compat. */
   out->core_opa = (uint8_t)s_ambient_body_stop_last;
   out->spec_opa = s_spec ? lv_obj_get_style_bg_opa(s_spec, LV_PART_MAIN) : 0;
   out->halo_opa = s_halo ? lv_obj_get_style_bg_opa(s_halo, LV_PART_MAIN) : 0;
   out->idle_breath_opa = (uint8_t)s_idle_breath_last_opa;
   out->state = (uint8_t)s_state;
   out->sleep_phase = (uint8_t)s_sleep_phase;
   out->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
   return true;
}

/* ── TT #555 FX playground ──────────────────────────────────────────── */

/* fx_apply_scale — combines the ambient-driven swell (when fx.expand)
 * with the shake startle offset (when fx.shake fires).  Both ride on
 * the body's transform_scale; if neither is active we set 256 (1.0×). */
static void fx_apply_scale(void) {
   if (!s_body) return;
   int scale = 256;
   if (s_fx.expand) {
      /* ±16 / 256 = ±6.25 % at rms=1; subtle, organic. */
      scale += (int)(s_ambient_rms * 16.0f);
   }
   if (s_shake_scale_offset) scale += s_shake_scale_offset;
   lv_obj_set_style_transform_pivot_x(s_body, ORB_SIZE / 2, LV_PART_MAIN);
   lv_obj_set_style_transform_pivot_y(s_body, ORB_SIZE / 2, LV_PART_MAIN);
   lv_obj_set_style_transform_scale_x(s_body, scale, LV_PART_MAIN);
   lv_obj_set_style_transform_scale_y(s_body, scale, LV_PART_MAIN);
}

/* fx_spin_tick — 30 Hz tick, increments rotation by ~0.6° per tick (so
 * full 360° revolution every 60 s).  Body's vertical gradient rotates
 * with it → reads as the sphere turning on its axis. */
static void fx_spin_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_body || !s_fx.spin) return;
   s_spin_angle_x10 = (s_spin_angle_x10 + 6) % 3600; /* 6 / 10 = 0.6° per 33 ms tick */
   lv_obj_set_style_transform_rotation(s_body, s_spin_angle_x10, LV_PART_MAIN);
}

static void fx_spin_start(void) {
   if (s_spin_timer) return;
   s_spin_angle_x10 = 0;
   s_spin_timer = lv_timer_create(fx_spin_tick_cb, 33, NULL);
}

static void fx_spin_stop(void) {
   if (s_spin_timer) {
      lv_timer_del(s_spin_timer);
      s_spin_timer = NULL;
   }
   if (s_body) {
      lv_obj_set_style_transform_rotation(s_body, 0, LV_PART_MAIN);
   }
   s_spin_angle_x10 = 0;
}

/* HSV → RGB hex.  h in degrees [0, 360], s + v in [0, 1]. */
static uint32_t fx_hsv_to_hex(float h, float s, float v) {
   float c = v * s;
   float hh = h / 60.0f;
   float x = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f));
   float r = 0, g = 0, b = 0;
   if (hh < 1) {
      r = c;
      g = x;
   } else if (hh < 2) {
      r = x;
      g = c;
   } else if (hh < 3) {
      g = c;
      b = x;
   } else if (hh < 4) {
      g = x;
      b = c;
   } else if (hh < 5) {
      r = x;
      b = c;
   } else {
      r = c;
      b = x;
   }
   float m = v - c;
   uint8_t R = (uint8_t)((r + m) * 255.0f);
   uint8_t G = (uint8_t)((g + m) * 255.0f);
   uint8_t B = (uint8_t)((b + m) * 255.0f);
   return ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
}

static void fx_rainbow_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_body || !s_fx.rainbow) return;
   if (ui_orb_pipeline_active() || s_state != ORB_STATE_IDLE) return;
   /* Full hue revolution every 30 s — 360° / 30000 ms × 200 ms tick
    * = 2.4° per tick.  Saturation/value picked so colours look
    * cinematic vs. cartoonish. */
   s_rainbow_hue = fmodf(s_rainbow_hue + 2.4f, 360.0f);
   uint32_t top = fx_hsv_to_hex(s_rainbow_hue, 0.45f, 0.95f);
   uint32_t bot = fx_hsv_to_hex(fmodf(s_rainbow_hue + 40.0f, 360.0f), 0.85f, 0.35f);
   body_canvas_paint(top, bot);
}

static void fx_rainbow_start(void) {
   if (s_rainbow_timer) return;
   s_rainbow_hue = 0.0f;
   s_rainbow_timer = lv_timer_create(fx_rainbow_tick_cb, 200, NULL);
}

static void fx_rainbow_stop(void) {
   if (s_rainbow_timer) {
      lv_timer_del(s_rainbow_timer);
      s_rainbow_timer = NULL;
   }
   /* Restore the circadian palette. */
   if (s_body) paint_body_for_hour(orb_effective_hour());
}

static void fx_glass_apply(bool on) {
   if (!s_body) return;
   if (on) {
      /* Translucent body so background bleeds through faintly.  The
       * existing spec + the new top highlight ring give the glass
       * read. */
      s_body_baseline_opa = lv_obj_get_style_bg_opa(s_body, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(s_body, 180, LV_PART_MAIN);
      if (!s_glass_ring) {
         s_glass_ring = lv_obj_create(s_body);
         lv_obj_remove_style_all(s_glass_ring);
         /* Thin top arc — a slim ring inscribed inside the body's
          * upper hemisphere.  Implemented as a 4-px-tall ellipse
          * positioned in the lit-side area. */
         /* Sized so the ring's corners stay inside the body's 140 px
          * radius circle even when fx.spin rotates the body — at
          * y=42 from top (= -98 from center), max half-width is
          * sqrt(140² − 98²) ≈ 100 px.  140-px width keeps a 30 px
          * margin from the edge. */
         lv_obj_set_size(s_glass_ring, 140, 4);
         lv_obj_set_pos(s_glass_ring, (ORB_SIZE - 140) / 2, 42);
         lv_obj_set_style_radius(s_glass_ring, 2, 0);
         lv_obj_set_style_bg_color(s_glass_ring, lv_color_hex(0xFFFFFF), 0);
         lv_obj_set_style_bg_opa(s_glass_ring, 60, 0);
         lv_obj_set_style_border_width(s_glass_ring, 0, 0);
         lv_obj_clear_flag(s_glass_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
      } else {
         lv_obj_clear_flag(s_glass_ring, LV_OBJ_FLAG_HIDDEN);
      }
   } else {
      lv_obj_set_style_bg_opa(s_body, s_body_baseline_opa, LV_PART_MAIN);
      if (s_glass_ring) lv_obj_add_flag(s_glass_ring, LV_OBJ_FLAG_HIDDEN);
   }
}

/* fx_shake_tick — polls IMU 10 Hz.  Computes the "gravity-adjusted"
 * accel magnitude (i.e. how far off from a constant 1g pull); a
 * spike above SHAKE_THRESHOLD_G fires a startle: spike scale +24
 * units for 200 ms, then linear ease back. */
#define SHAKE_THRESHOLD_G 0.6f
#define SHAKE_RECOVER_MS 600
static void fx_shake_tick_cb(lv_timer_t *t) {
   (void)t;
   if (!s_fx.shake) return;
   tab5_imu_data_t d;
   if (tab5_imu_read(&d) != ESP_OK) return;
   float mag = sqrtf(d.accel.x * d.accel.x + d.accel.y * d.accel.y + d.accel.z * d.accel.z);
   float jolt = fabsf(mag - 1.0f); /* deviation from rest gravity */
   uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
   if (jolt > SHAKE_THRESHOLD_G) {
      s_shake_recovery_until_ms = now_ms + SHAKE_RECOVER_MS;
      s_shake_scale_offset = 30; /* +12 % momentary swell */
   } else if (s_shake_recovery_until_ms && now_ms < s_shake_recovery_until_ms) {
      /* Linear decay over recovery window. */
      uint32_t remaining = s_shake_recovery_until_ms - now_ms;
      s_shake_scale_offset = (int)(30 * remaining / SHAKE_RECOVER_MS);
   } else {
      s_shake_recovery_until_ms = 0;
      s_shake_scale_offset = 0;
   }
   fx_apply_scale();
}

static void fx_shake_start(void) {
   if (s_shake_timer) return;
   s_shake_recovery_until_ms = 0;
   s_shake_scale_offset = 0;
   s_shake_timer = lv_timer_create(fx_shake_tick_cb, 100, NULL);
}

static void fx_shake_stop(void) {
   if (s_shake_timer) {
      lv_timer_del(s_shake_timer);
      s_shake_timer = NULL;
   }
   s_shake_recovery_until_ms = 0;
   s_shake_scale_offset = 0;
   fx_apply_scale();
}

void ui_orb_set_fx(const ui_orb_fx_t *fx) {
   if (!fx) return;
   ui_orb_fx_t old = s_fx;
   s_fx = *fx;
   if (fx->spin && !old.spin)
      fx_spin_start();
   else if (!fx->spin && old.spin)
      fx_spin_stop();
   if (fx->rainbow && !old.rainbow)
      fx_rainbow_start();
   else if (!fx->rainbow && old.rainbow)
      fx_rainbow_stop();
   if (fx->shake && !old.shake)
      fx_shake_start();
   else if (!fx->shake && old.shake)
      fx_shake_stop();
   if (fx->glass != old.glass) fx_glass_apply(fx->glass);
   if (!fx->expand && old.expand) fx_apply_scale(); /* snap back to 1.0× */
}

void ui_orb_get_fx(ui_orb_fx_t *out) {
   if (!out) return;
   *out = s_fx;
}

/* ── Always-alive sphere — ambient mic glow (TT #552) ────────────────── */

/* Replaced the TT #549 slow infinite LVGL anims (gradient-stop pan +
 * spec opa) with a mic-driven ambient sampler.  The previous motion
 * was too subtle to read.  Now: a 3 Hz timer reads a brief mic chunk,
 * computes RMS, and modulates the halo's bg_opa floor + spec bg_opa
 * proportional to room sound.  Quiet room → calm dim halo, chatter
 * or music → visible warm glow.
 *
 * Pauses when s_state != IDLE (voice owns the mic during LISTENING /
 * PROCESSING / SPEAKING) or when the dictation pipeline is active. */

#include <math.h> /* sqrtf */

#include "audio.h" /* tab5_mic_read for ambient sampling */

/* Two-stage timing for buttery motion:
 *   - MIC sampler ticks at 200 ms — reads ~1.3 ms of audio, computes
 *     RMS, sets a TARGET (s_ambient_rms_target).
 *   - SMOOTH ticker runs at 33 ms (30 Hz) — interpolates the live
 *     value (s_ambient_rms) toward target with a low alpha so the
 *     glow eases between samples instead of stepping.
 * Together: the mic provides the signal, the smoother provides the
 * smoothness. */
#define AMBIENT_MIC_TICK_MS 200
#define AMBIENT_SMOOTH_TICK_MS 33  /* 30 Hz tween */
#define AMBIENT_SMOOTH_ALPHA 0.18f /* per-tick approach rate toward target */
#define AMBIENT_FRAMES 64          /* 64 TDM slices = 256 int16_t total, ~1.3 ms @ 48 kHz */
#define AMBIENT_MIC_TDM_CHANNELS 4
#define AMBIENT_MIC1_OFFSET 0      /* mic 1 lives in slot 0 of the TDM frame */
#define AMBIENT_RMS_DIV 2500.0f    /* normalise raw RMS into 0..1 — quiet ~250 raw, chatter ~2000+ */
#define AMBIENT_TARGET_ALPHA 0.55f /* target follows new mic readings briskly */
/* TT #553 follow-up rev3: clearly visible body response to ambient
 * sound.  Two channels:
 *   - bg_main_stop position pushes the lit-side hemisphere DOWN as
 *     sound rises (range 0..100; was 0..38, too subtle).
 *   - bg_color top stop interpolates from baseline cream-amber
 *     toward bright cream-white at peak ambient.  Sphere visibly
 *     brightens, not just shifts.
 * Spec opa still brightens in parallel for wet-glint emphasis.
 *
 * Re-rasterizing the body gradient at 30 Hz (smoother tick) is now
 * affordable thanks to the PR #550 budget bump (CPU 400 + parallel
 * draw + shadow cache) — measured at 44 fps with this load. */
#define AMBIENT_BODY_STOP_FLOOR 0
#define AMBIENT_BODY_STOP_PEAK 100
#define AMBIENT_SPEC_FLOOR 130
#define AMBIENT_SPEC_PEAK 240
#define AMBIENT_TOP_COLOR_QUIET 0xFFEBC4 /* baseline lit cream-amber */
#define AMBIENT_TOP_COLOR_LOUD 0xFFFAEC  /* bright cream-white at peak */

static lv_timer_t *s_ambient_mic_timer = NULL;
static lv_timer_t *s_ambient_smooth_timer = NULL;
/* s_ambient_rms + s_ambient_rms_target + s_ambient_body_stop_last
 * forward-decl'd near top. */
static int s_ambient_spec_last = -1;

/* Lerp two 0xRRGGBB hex colors by t in [0, 1]. */
static uint32_t ambient_lerp_color(uint32_t a, uint32_t b, float t) {
   if (t < 0) t = 0;
   if (t > 1) t = 1;
   int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
   int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
   int r = ar + (int)((br - ar) * t);
   int g = ag + (int)((bg - ag) * t);
   int bl = ab + (int)((bb - ab) * t);
   return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
}

static uint32_t s_ambient_top_color_last = 0;

static void ambient_apply(void) {
   /* Rainbow FX owns the body palette while active. */
   bool body_palette_ok = (s_body && !s_fx.rainbow);
   if (body_palette_ok) {
      /* QUANTIZED so the 30 Hz smoother doesn't re-issue the style
       * change on every tick — only when the top color shifts by a
       * noticeable amount (mask low 3 bits per channel = 32 effective
       * levels per channel).  Without this guard the body invalidates
       * 30×/sec, which trips the mutex budget. */
      uint32_t top = ambient_lerp_color(AMBIENT_TOP_COLOR_QUIET, AMBIENT_TOP_COLOR_LOUD, s_ambient_rms);
      uint32_t top_quant = top & 0xF8F8F8;
      if (top_quant != s_ambient_top_color_last) {
         s_ambient_top_color_last = top_quant;
         body_canvas_paint(top_quant, s_body_grad_bot_color);
      }
      /* s_ambient_body_stop_last stays for the /orb/motion telemetry. */
      int stop = AMBIENT_BODY_STOP_FLOOR + (int)(s_ambient_rms * (AMBIENT_BODY_STOP_PEAK - AMBIENT_BODY_STOP_FLOOR));
      if (stop != s_ambient_body_stop_last) {
         s_ambient_body_stop_last = stop;
      }
   }
   if (s_spec) {
      int spec_opa = AMBIENT_SPEC_FLOOR + (int)(s_ambient_rms * (AMBIENT_SPEC_PEAK - AMBIENT_SPEC_FLOOR));
      if (spec_opa < AMBIENT_SPEC_FLOOR) spec_opa = AMBIENT_SPEC_FLOOR;
      if (spec_opa > AMBIENT_SPEC_PEAK) spec_opa = AMBIENT_SPEC_PEAK;
      if (spec_opa != s_ambient_spec_last) {
         s_ambient_spec_last = spec_opa;
         lv_obj_set_style_bg_opa(s_spec, (lv_opa_t)spec_opa, LV_PART_MAIN);
      }
   }
   /* TT #555: expand FX rides scale on top of the smooth ambient drive. */
   fx_apply_scale();
}

static void ambient_smooth_tick_cb(lv_timer_t *t) {
   (void)t;
   if (s_state != ORB_STATE_IDLE || ui_orb_pipeline_active()) return;
   /* Approach target each tick — 30 Hz × alpha 0.18 ≈ ~5 frames to
    * cover 60 % of any gap.  Buttery, not lurchy. */
   s_ambient_rms += (s_ambient_rms_target - s_ambient_rms) * AMBIENT_SMOOTH_ALPHA;
   ambient_apply();
}

static void ambient_mic_tick_cb(lv_timer_t *t) {
   (void)t;
   if (s_state != ORB_STATE_IDLE || ui_orb_pipeline_active()) return;

   static int16_t buf[AMBIENT_FRAMES * AMBIENT_MIC_TDM_CHANNELS];
   if (tab5_mic_read(buf, AMBIENT_FRAMES, 20) != ESP_OK) {
      /* Mic busy or not ready — let target decay so the glow gently
       * fades back to baseline rather than freezing. */
      s_ambient_rms_target *= 0.9f;
      return;
   }
   int64_t sqsum = 0;
   for (int i = 0; i < AMBIENT_FRAMES; i++) {
      int16_t v = buf[i * AMBIENT_MIC_TDM_CHANNELS + AMBIENT_MIC1_OFFSET];
      sqsum += (int64_t)v * v;
   }
   float rms = sqrtf((float)(sqsum / AMBIENT_FRAMES));
   float n = rms / AMBIENT_RMS_DIV;
   if (n < 0.0f) n = 0.0f;
   if (n > 1.0f) n = 1.0f;
   /* Update the target with light smoothing — the per-frame smoother
    * below handles the visible interpolation. */
   s_ambient_rms_target = (AMBIENT_TARGET_ALPHA * n) + ((1.0f - AMBIENT_TARGET_ALPHA) * s_ambient_rms_target);
}

static void alive_start(void) {
   if (s_alive_running) return;
   s_alive_running = true;
   s_ambient_rms = 0.0f;
   s_ambient_rms_target = 0.0f;
   s_ambient_body_stop_last = -1;
   s_ambient_spec_last = -1;
   if (!s_ambient_mic_timer) {
      s_ambient_mic_timer = lv_timer_create(ambient_mic_tick_cb, AMBIENT_MIC_TICK_MS, NULL);
   }
   if (!s_ambient_smooth_timer) {
      s_ambient_smooth_timer = lv_timer_create(ambient_smooth_tick_cb, AMBIENT_SMOOTH_TICK_MS, NULL);
   }
}

static void alive_stop(void) {
   if (!s_alive_running) return;
   s_alive_running = false;
   if (s_ambient_mic_timer) {
      lv_timer_del(s_ambient_mic_timer);
      s_ambient_mic_timer = NULL;
   }
   if (s_ambient_smooth_timer) {
      lv_timer_del(s_ambient_smooth_timer);
      s_ambient_smooth_timer = NULL;
   }
   /* Restore baselines.  Halo opa is owned by breath/bloom; the body
    * gradient stop snaps back to 0 (canonical full-range gradient);
    * spec returns to its baseline peak. */
   if (s_spec) lv_obj_set_style_bg_opa(s_spec, 140, LV_PART_MAIN);
   if (s_body) lv_obj_set_style_bg_main_stop(s_body, 0, LV_PART_MAIN);
   s_ambient_rms = 0.0f;
   s_ambient_rms_target = 0.0f;
   s_ambient_body_stop_last = -1;
   s_ambient_spec_last = -1;
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
/* s_idle_breath_last_opa forward-declared at top of file. */

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
   body_canvas_paint(color_hex, bot);
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
