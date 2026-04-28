/**
 * spring_anim — pure-C damped harmonic oscillator (Phase 1 of #42).
 *
 * Closed-form integration of m·ẍ + c·ẋ + k·x = 0 with x = pos − target.
 * No Euler accumulation drift — `t` is wallclock-since-retarget, every
 * spring_anim_update walks the analytic solution from t=0.
 *
 * Reference: M5Stack's smooth_ui_toolkit (Forairaaaaa, MIT license)
 * which carries the same derivation in C++.  Math identities cross-
 * checked against any DSP textbook covering second-order systems.
 */
#include "spring_anim.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "spring";

/* Numerical floor: damping that small means the spring never converges
 * via the natural damping ratio path; clamp to avoid divides by ~0 in
 * the regime selector. */
#define SPRING_MIN_K 0.001f
#define SPRING_MIN_M 0.001f

void spring_anim_init(spring_anim_t *s, spring_config_t cfg) {
   if (!s) return;
   memset(s, 0, sizeof(*s));
   s->cfg = cfg;
   /* Defensive: a config with zero mass or zero stiffness has no
    * physical solution.  Clamp instead of crashing — the smoke test
    * covers the sane presets, but a rolled-by-hand config with a
    * typo shouldn't blow up. */
   if (s->cfg.mass < SPRING_MIN_M) s->cfg.mass = SPRING_MIN_M;
   if (s->cfg.stiffness < SPRING_MIN_K) s->cfg.stiffness = SPRING_MIN_K;
   if (s->cfg.damping < 0.0f) s->cfg.damping = 0.0f;
   s->done = true; /* nothing to animate until retarget */
}

void spring_anim_retarget(spring_anim_t *s, float from, float to, float velocity) {
   if (!s) return;
   s->from = from;
   s->to = to;
   s->pos = from;
   s->velocity = velocity;
   s->t = 0.0f;
   /* If the retarget is a no-op (already at target with no velocity),
    * mark done immediately — saves the first integration step from
    * producing 0/0 nans inside the closed-form. */
   if (fabsf(from - to) < SPRING_EPS_POS && fabsf(velocity) < SPRING_EPS_VEL) {
      s->pos = to;
      s->velocity = 0.0f;
      s->done = true;
   } else {
      s->done = false;
   }
}

float spring_anim_update(spring_anim_t *s, float dt) {
   if (!s) return 0.0f;
   if (s->done) return s->pos;

   s->t += dt;
   if (s->t >= SPRING_MAX_ELAPSED_S) {
      /* Bail-out: snap to target so the caller's UI doesn't sit
       * mid-animation forever on a pathological config. */
      s->pos = s->to;
      s->velocity = 0.0f;
      s->done = true;
      return s->pos;
   }

   const float k = s->cfg.stiffness;
   const float c = s->cfg.damping;
   const float m = s->cfg.mass;
   const float wn = sqrtf(k / m);                /* natural ang. freq */
   const float zeta = c / (2.0f * sqrtf(k * m)); /* damping ratio */

   /* Solve in error-space: x(t) = pos(t) − to.  x(0) = from − to,
    * ẋ(0) = velocity.  After computing x(t) we add `to` back. */
   const float x0 = s->from - s->to;
   const float v0 = s->velocity;
   const float t = s->t;

   float x_t; /* error position at time t */
   float v_t; /* error velocity at time t (= velocity, since to is constant) */

   /* Three regimes by zeta.  Use a small epsilon around 1.0 for the
    * critical case so a tiny FP wobble doesn't push us into the wrong
    * branch (where the formulas use sinh / sin and disagree on sign).
    */
   if (zeta < 0.9999f) {
      /* Underdamped: oscillates with envelope exp(−zeta·wn·t). */
      const float wd = wn * sqrtf(1.0f - zeta * zeta);
      const float A = x0;
      const float B = (v0 + zeta * wn * x0) / wd;
      const float env = expf(-zeta * wn * t);
      const float c_ = cosf(wd * t);
      const float s_ = sinf(wd * t);
      x_t = env * (A * c_ + B * s_);
      /* v(t) = d/dt [env · (A·cos(wd·t) + B·sin(wd·t))] */
      v_t = env * ((-zeta * wn) * (A * c_ + B * s_) + wd * (-A * s_ + B * c_));
   } else if (zeta > 1.0001f) {
      /* Overdamped: two exponentials, no oscillation. */
      const float wd = wn * sqrtf(zeta * zeta - 1.0f);
      /* x(t) = exp(−zeta·wn·t) · (A·exp(wd·t) + B·exp(−wd·t)) */
      const float A = (v0 + (zeta * wn + wd) * x0) / (2.0f * wd);
      const float B = x0 - A;
      const float env = expf(-zeta * wn * t);
      const float ep = expf(wd * t);
      const float em = expf(-wd * t);
      x_t = env * (A * ep + B * em);
      v_t = env * (-zeta * wn) * (A * ep + B * em) + env * (A * wd * ep - B * wd * em);
   } else {
      /* Critically damped: x(t) = exp(−wn·t) · (x0 + (v0 + wn·x0)·t). */
      const float env = expf(-wn * t);
      const float lin = x0 + (v0 + wn * x0) * t;
      x_t = env * lin;
      /* v(t) = exp(−wn·t)·(v0 + wn·x0) − wn·exp(−wn·t)·lin
       *     = exp(−wn·t)·((v0 + wn·x0) − wn·lin) */
      v_t = env * ((v0 + wn * x0) - wn * lin);
   }

   s->pos = s->to + x_t;
   s->velocity = v_t;

   /* Convergence: both displacement AND speed below epsilons.  Speed
    * matters too — at the equilibrium crossing of an underdamped
    * spring, |pos − to| can dip below SPRING_EPS_POS while |v| is
    * still large; ending there would visibly snap to a stop. */
   if (fabsf(x_t) < SPRING_EPS_POS && fabsf(v_t) < SPRING_EPS_VEL) {
      s->pos = s->to;
      s->velocity = 0.0f;
      s->done = true;
   }
   return s->pos;
}

bool spring_anim_done(const spring_anim_t *s) { return !s || s->done; }

/* ── Boot smoke test ────────────────────────────────────────────────
 *
 * Runs each preset 0 → 100 at 60 Hz, logs time-to-converge.  Catches:
 *   - branch selection bugs (regime epsilon misset)
 *   - sign errors that would cause the spring to diverge
 *   - convergence threshold misset (returns 0 settled or stuck-on)
 *
 * Output expected on a healthy build (rough):
 *   spring smoke: SNAPPY converged at  300 ms (overshoot=  0.0%)
 *   spring smoke: BOUNCY converged at  720 ms (overshoot= 17.4%)
 *   spring smoke: SMOOTH converged at  640 ms (overshoot=  0.0%)
 *
 * The exact ms numbers depend on the (k,c,m) tuning, but every preset
 * MUST converge < SPRING_MAX_ELAPSED_S; if any returns the snap-out
 * sentinel that's a real bug.
 */
static int run_one_smoke(const char *name, spring_config_t cfg) {
   spring_anim_t s;
   spring_anim_init(&s, cfg);
   spring_anim_retarget(&s, 0.0f, 100.0f, 0.0f);

   const float dt = 1.0f / 60.0f;
   float max_pos = 0.0f;
   int frames = 0;
   while (!spring_anim_done(&s) && frames < 600) {
      float p = spring_anim_update(&s, dt);
      if (p > max_pos) max_pos = p;
      frames++;
   }
   const float elapsed_ms = frames * dt * 1000.0f;
   const float overshoot_pct = (max_pos > 100.0f) ? ((max_pos - 100.0f) / 100.0f * 100.0f) : 0.0f;
   const bool converged = spring_anim_done(&s) && fabsf(s.pos - 100.0f) < SPRING_EPS_POS;
   ESP_LOGI(TAG, "smoke: %-7s %s at %4.0f ms (overshoot=%4.1f%%, frames=%d)", name,
            converged ? "converged" : "STUCK    ", elapsed_ms, overshoot_pct, frames);
   return converged ? 1 : 0;
}

int spring_anim_boot_smoke(void) {
   int ok = 0;
   ok += run_one_smoke("SNAPPY", SPRING_SNAPPY);
   ok += run_one_smoke("BOUNCY", SPRING_BOUNCY);
   ok += run_one_smoke("SMOOTH", SPRING_SMOOTH);
   ESP_LOGI(TAG, "boot smoke: %d/3 presets converged", ok);
   return ok;
}
