/* W9-A slice 3: host tests for the closed-form damped harmonic
 * oscillator at main/spring_anim.c.
 *
 * Bugs the regression net is sized to catch:
 *   - regime selector wrong sign (critical / over / underdamped use
 *     different formulas — sin vs sinh, etc).  A typo there silently
 *     produces wrong-feeling animations, not crashes.
 *   - convergence threshold drift — must end with BOTH pos and velocity
 *     under epsilon, never just pos (orbital-pass-through bug).
 *   - SPRING_MAX_ELAPSED_S bail-out must fire on pathological configs
 *     instead of looping the UI thread forever.
 *   - retarget with from==to and velocity==0 must short-circuit done=true
 *     so the first integration step doesn't produce 0/0 NaN.
 *   - the three named presets (SNAPPY/BOUNCY/SMOOTH) must converge
 *     within the documented envelope (~300/720/640 ms over 100 px).
 */

#include "spring_anim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0;

#define ASSERT_TRUE(expr)                                                                      \
   do {                                                                                        \
      if (!(expr)) {                                                                           \
         fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);                        \
         return 1;                                                                             \
      }                                                                                        \
      g_pass++;                                                                                \
   } while (0)

#define ASSERT_FLOAT_NEAR(actual, expected, tol)                                               \
   do {                                                                                        \
      float a_ = (actual);                                                                     \
      float e_ = (expected);                                                                   \
      if (fabsf(a_ - e_) > (tol)) {                                                            \
         fprintf(stderr, "FAIL %s:%d expected~%g actual=%g tol=%g\n", __FILE__, __LINE__,      \
                 (double)e_, (double)a_, (double)(tol));                                       \
         return 1;                                                                             \
      }                                                                                        \
      g_pass++;                                                                                \
   } while (0)

/* Drive the spring to convergence at 60 Hz; return frames elapsed (or
 * -1 if it timed out past 3 s). */
static int drive_to_done(spring_anim_t *s) {
   const float dt = 1.0f / 60.0f;
   for (int i = 0; i < 200; i++) {
      spring_anim_update(s, dt);
      if (spring_anim_done(s)) return i + 1;
   }
   return -1;
}

static int test_init_state_is_done(void) {
   spring_anim_t s;
   spring_anim_init(&s, SPRING_SNAPPY);
   /* After init, no animation is running — done must be true so the
    * caller doesn't schedule pointless frames. */
   ASSERT_TRUE(spring_anim_done(&s));
   ASSERT_FLOAT_NEAR(s.pos, 0.0f, 0.001f);
   return 0;
}

static int test_retarget_zero_distance_short_circuits(void) {
   /* The "retarget to where I already am with no velocity" path is
    * the one that historically produced 0/0 NaN inside the closed-
    * form (the regime selector reaches an underflow branch).  Must
    * short-circuit done=true. */
   spring_anim_t s;
   spring_anim_init(&s, SPRING_SNAPPY);
   spring_anim_retarget(&s, 50.0f, 50.0f, 0.0f);
   ASSERT_TRUE(spring_anim_done(&s));
   ASSERT_FLOAT_NEAR(s.pos, 50.0f, 0.001f);
   return 0;
}

static int test_snappy_converges_under_500ms(void) {
   /* SNAPPY's documented envelope is ~300 ms; allow 30 frames (500 ms)
    * headroom to keep this test stable across compiler FP differences. */
   spring_anim_t s;
   spring_anim_init(&s, SPRING_SNAPPY);
   spring_anim_retarget(&s, 0.0f, 100.0f, 0.0f);
   int frames = drive_to_done(&s);
   ASSERT_TRUE(frames > 0);
   ASSERT_TRUE(frames <= 30);
   ASSERT_FLOAT_NEAR(s.pos, 100.0f, SPRING_EPS_POS);
   return 0;
}

static int test_bouncy_overshoots(void) {
   /* BOUNCY is underdamped → must exceed the target at peak.  The
    * issue body documents ~17.4% overshoot; we accept anything > 5%. */
   spring_anim_t s;
   spring_anim_init(&s, SPRING_BOUNCY);
   spring_anim_retarget(&s, 0.0f, 100.0f, 0.0f);
   const float dt = 1.0f / 60.0f;
   float max_pos = 0.0f;
   for (int i = 0; i < 200 && !spring_anim_done(&s); i++) {
      float p = spring_anim_update(&s, dt);
      if (p > max_pos) max_pos = p;
   }
   ASSERT_TRUE(spring_anim_done(&s));
   ASSERT_TRUE(max_pos > 105.0f); /* must overshoot the 100 target by 5+ */
   ASSERT_FLOAT_NEAR(s.pos, 100.0f, SPRING_EPS_POS);
   return 0;
}

static int test_smooth_no_overshoot(void) {
   /* SMOOTH is critically damped / mildly over — must never exceed
    * the target.  A regime-selector sign flip would show up here as
    * overshoot or divergence. */
   spring_anim_t s;
   spring_anim_init(&s, SPRING_SMOOTH);
   spring_anim_retarget(&s, 0.0f, 100.0f, 0.0f);
   const float dt = 1.0f / 60.0f;
   float max_pos = 0.0f;
   for (int i = 0; i < 200 && !spring_anim_done(&s); i++) {
      float p = spring_anim_update(&s, dt);
      if (p > max_pos) max_pos = p;
   }
   ASSERT_TRUE(spring_anim_done(&s));
   /* Allow a hair of FP slack but no real overshoot. */
   ASSERT_TRUE(max_pos < 100.5f);
   return 0;
}

static int test_max_elapsed_bailout(void) {
   /* Pathological config: tiny stiffness + huge mass = oscillation
    * with a multi-minute period.  Must bail at SPRING_MAX_ELAPSED_S
    * instead of looping the UI thread forever. */
   spring_config_t pathological = {.stiffness = 0.01f, .damping = 0.0f, .mass = 100.0f, .bounce = 0};
   spring_anim_t s;
   spring_anim_init(&s, pathological);
   spring_anim_retarget(&s, 0.0f, 100.0f, 0.0f);
   const float dt = 1.0f / 60.0f;
   for (int i = 0; i < 250; i++) { /* 250 frames @ 60 Hz = 4.17 s > 3 s ceiling */
      spring_anim_update(&s, dt);
      if (spring_anim_done(&s)) break;
   }
   ASSERT_TRUE(spring_anim_done(&s));
   /* Bail-out path snaps to target. */
   ASSERT_FLOAT_NEAR(s.pos, 100.0f, 0.001f);
   return 0;
}

static int test_done_is_idempotent(void) {
   /* Calling update after done must return s->to without recomputing. */
   spring_anim_t s;
   spring_anim_init(&s, SPRING_SNAPPY);
   spring_anim_retarget(&s, 0.0f, 100.0f, 0.0f);
   drive_to_done(&s);
   float pos1 = spring_anim_update(&s, 1.0f / 60.0f);
   float pos2 = spring_anim_update(&s, 1.0f / 60.0f);
   ASSERT_FLOAT_NEAR(pos1, pos2, 0.001f);
   ASSERT_FLOAT_NEAR(pos1, 100.0f, SPRING_EPS_POS);
   return 0;
}

static int test_zero_mass_clamped_no_crash(void) {
   /* spring_anim_init clamps mass < SPRING_MIN_M instead of crashing
    * on div-by-zero downstream.  A typo'd preset shouldn't reboot
    * the device. */
   spring_config_t broken = {.stiffness = 100.0f, .damping = 20.0f, .mass = 0.0f, .bounce = 0};
   spring_anim_t s;
   spring_anim_init(&s, broken);
   spring_anim_retarget(&s, 0.0f, 50.0f, 0.0f);
   drive_to_done(&s);
   ASSERT_TRUE(spring_anim_done(&s));
   return 0;
}

static int test_smoke_runner_all_presets_converge(void) {
   /* The boot_smoke() helper must report 3/3 — that's the exact
    * invariant a healthy build needs at startup. */
   int ok = spring_anim_boot_smoke();
   if (ok != 3) {
      fprintf(stderr, "FAIL %s:%d boot_smoke returned %d/3\n", __FILE__, __LINE__, ok);
      return 1;
   }
   g_pass++;
   return 0;
}

int main(void) {
   struct {
      const char *name;
      int (*fn)(void);
   } tests[] = {
      {"init_state_is_done", test_init_state_is_done},
      {"retarget_zero_distance_short_circuits", test_retarget_zero_distance_short_circuits},
      {"snappy_converges_under_500ms", test_snappy_converges_under_500ms},
      {"bouncy_overshoots", test_bouncy_overshoots},
      {"smooth_no_overshoot", test_smooth_no_overshoot},
      {"max_elapsed_bailout", test_max_elapsed_bailout},
      {"done_is_idempotent", test_done_is_idempotent},
      {"zero_mass_clamped_no_crash", test_zero_mass_clamped_no_crash},
      {"smoke_runner_all_presets_converge", test_smoke_runner_all_presets_converge},
   };
   size_t n = sizeof(tests) / sizeof(tests[0]);
   for (size_t i = 0; i < n; i++) {
      int r = tests[i].fn();
      if (r != 0) {
         fprintf(stderr, "test_spring_anim: FAILED at %s\n", tests[i].name);
         return r;
      }
      printf("test_spring_anim: %s OK\n", tests[i].name);
   }
   printf("test_spring_anim: %d assertions, %zu tests passed\n", g_pass, n);
   return 0;
}
