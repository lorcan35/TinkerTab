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
#include <time.h>

#include "esp_log.h"
#include "imu.h" /* tab5_imu_read for tilt-driven specular drift */
#include "lvgl.h"
#include "ui_core.h" /* tab5_lv_async_call for cross-thread repaints */
#include "widget.h"  /* widget_tone_t for paint_for_tone */

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

/* ── Module state ────────────────────────────────────────────────────── */

static lv_obj_t *s_root = NULL; /* parent container (= the home screen) */
static lv_obj_t *s_body = NULL; /* the lit sphere itself */
static lv_obj_t *s_spec = NULL; /* tilt-driven specular highlight (child of s_body) */
static lv_timer_t *s_tilt_timer = NULL;
static float s_tilt_dx_ema = 0.0f;
static float s_tilt_dy_ema = 0.0f;
static ui_orb_state_t s_state = ORB_STATE_IDLE;
static bool s_presence_near = true;
static int8_t s_force_hour = -1; /* -1 = use real RTC */
static int s_last_painted_hour = -1;
static uint8_t s_last_painted_mode = 0;

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

   lv_obj_set_pos(s_spec, ORB_SPEC_REST_X + (int)dx, ORB_SPEC_REST_Y + (int)dy);
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
      lv_obj_set_pos(s_spec, ORB_SPEC_REST_X, ORB_SPEC_REST_Y);
   }
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

void ui_orb_create(lv_obj_t *parent, int cx, int cy) {
   if (s_body || !parent) {
      ESP_LOGW(TAG, "ui_orb_create skipped (s_body=%p parent=%p)", s_body, parent);
      return;
   }
   s_root = parent;
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

   /* IDLE state arms tilt drift by default. */
   tilt_start();

   ESP_LOGI(TAG, "orb created at (%d, %d), size %d", cx, cy, ORB_SIZE);
}

void ui_orb_destroy(void) {
   /* Stop tilt timer first — its callback dereferences s_spec, so we
    * must not leave it scheduled past the screen tear-down. */
   tilt_stop();
   /* The body's parent (the home screen) owns the actual delete via its
    * own destroy path; here we only clear our handles + reset state so
    * a subsequent ui_orb_create on a fresh screen starts clean. */
   s_root = NULL;
   s_body = NULL;
   s_spec = NULL;
   s_state = ORB_STATE_IDLE;
   s_last_painted_hour = -1;
   s_tilt_dx_ema = 0.0f;
   s_tilt_dy_ema = 0.0f;
}

/* ── State driver ────────────────────────────────────────────────────── */

void ui_orb_set_state(ui_orb_state_t s) {
   if (s == s_state) return;
   ESP_LOGD(TAG, "state %d → %d", (int)s_state, (int)s);
   s_state = s;

   /* Step 4: tilt drift runs in IDLE + LISTENING (creates the
    * always-physical feel + lets the user point at the orb during
    * voice).  In PROCESSING + SPEAKING the highlight freezes — the
    * state's own motion (comet, halo) carries the action, and a
    * drifting highlight would compete. */
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
}

ui_orb_state_t ui_orb_get_state(void) { return s_state; }

/* ── Hardware-aware hooks ────────────────────────────────────────────── */

void ui_orb_set_presence(bool near) {
   if (s_presence_near == near) return;
   s_presence_near = near;
   /* Step 8 wires opa override on s_body via tab5_lv_async_call. */
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
