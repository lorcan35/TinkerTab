/**
 * spring_anim — pure-C damped harmonic oscillator (Phase 1 of #42).
 *
 * Use case: physically-based UI transitions (screen slides, toast
 * slide-in, voice orb pulse, sheet drag-snap).  Compared to the
 * built-in lv_anim `path_cb` curves (linear / ease / overshoot),
 * a spring lets a single value config produce the right "feel"
 * across distances + initial velocities — flick-to-fling on touch
 * Just Works because the spring carries the lift-off velocity.
 *
 * Math: classical damped harmonic oscillator
 *
 *     m·ẍ + c·ẋ + k·x = 0
 *
 * with x = pos − target.  Three regimes by zeta = c / (2·sqrt(k·m)):
 *
 *   - underdamped   (zeta < 1):  oscillates, decays — bouncy
 *   - critical      (zeta == 1): fastest non-oscillating settle
 *   - overdamped    (zeta > 1):  slow approach, no overshoot
 *
 * Closed-form integration (no Euler accumulation drift):
 * see spring_anim.c::spring_anim_update.
 *
 * Phase 1 (this file) ships engine + smoke test only.  Phase 2
 * wires it into LVGL via lv_timer + a per-call-site update cb.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spring config — pick a preset or roll your own. */
typedef struct {
   float stiffness; /* k — higher = snappier */
   float damping;   /* c — higher = less oscillation */
   float mass;      /* m — usually 1 */
   float bounce;    /* 0..1 — informational only (not used in math; let the
                     * (k, c, m) ratios drive the regime).  Documents intent
                     * for callers reading the preset. */
} spring_config_t;

/* Three named presets matching the issue body's tuning targets.
 *
 *   SNAPPY  — fast, no bounce, settles in ~300 ms over a 100-px move.
 *             For action-confirmation feedback (button push, toast in).
 *   BOUNCY  — overshoots a few times before landing.  Playful.  ~700 ms.
 *             For success states ("note saved!"), reveal animations.
 *   SMOOTH  — slow, no overshoot, eased glide.  ~600 ms.
 *             For panel drags, screen transitions.
 */
#define SPRING_SNAPPY ((spring_config_t){.stiffness = 400.0f, .damping = 30.0f, .mass = 1.0f, .bounce = 0.0f})
#define SPRING_BOUNCY ((spring_config_t){.stiffness = 200.0f, .damping = 10.0f, .mass = 1.0f, .bounce = 0.4f})
#define SPRING_SMOOTH ((spring_config_t){.stiffness = 100.0f, .damping = 20.0f, .mass = 1.0f, .bounce = 0.1f})

/* Convergence epsilon: spring_anim_done() returns true when both
 * |pos − target| AND |velocity| drop below these for one frame.
 * Sized for pixel-space animation (sub-pixel residual is invisible). */
#define SPRING_EPS_POS 0.5f
#define SPRING_EPS_VEL 0.5f

/* Hard ceiling on elapsed time before we force-snap to target.  Catches
 * pathological configs (zero stiffness, huge mass) that would otherwise
 * loop forever consuming CPU.  3 s is well past every reasonable UI
 * transition; if your animation legitimately needs longer, you don't
 * want a spring, you want a tween. */
#define SPRING_MAX_ELAPSED_S 3.0f

typedef struct {
   spring_config_t cfg;
   float from;     /* anchor at last retarget */
   float to;       /* destination */
   float pos;      /* current position (output) */
   float velocity; /* current velocity */
   float t;        /* elapsed seconds since last retarget */
   bool done;      /* true once converged or SPRING_MAX_ELAPSED_S hit */
} spring_anim_t;

/**
 * Initialise the anim state.  cfg is copied (so the macro presets work
 * even with rvalue lifetimes).  After init, pos == 0, to == 0, done.
 * Call spring_anim_retarget() to start an actual animation.
 */
void spring_anim_init(spring_anim_t *s, spring_config_t cfg);

/**
 * Set a new from/to pair and an initial velocity.  Resets the elapsed
 * timer so the closed-form integration has a clean t=0.  Call this
 * mid-animation to redirect (the velocity argument carries the live
 * speed at the redirect moment, which is how flick-then-flick feels
 * physical instead of teleporty).
 *
 * @param from       Starting pixel value (or whatever unit you're using)
 * @param to         Target value
 * @param velocity   Initial velocity, same units per second.  Pass 0
 *                   for a "from rest" start.
 */
void spring_anim_retarget(spring_anim_t *s, float from, float to, float velocity);

/**
 * Advance the simulation by `dt` seconds.  Returns the current position
 * (also stored in s->pos).  Idempotent once done — further calls return
 * s->to without recomputing.
 *
 * Call this once per frame from your LVGL timer / VSync hook.  60 fps
 * dt = ~0.0167.  The math is closed-form so there's no accumulation
 * error from variable dt.
 */
float spring_anim_update(spring_anim_t *s, float dt);

/**
 * True iff the spring has converged (|pos − to| < SPRING_EPS_POS AND
 * |velocity| < SPRING_EPS_VEL) or hit SPRING_MAX_ELAPSED_S.  When done
 * the renderer can stop scheduling updates.
 */
bool spring_anim_done(const spring_anim_t *s);

/**
 * Smoke-test runner: simulates each preset settling from 0 → 100 at
 * 60 Hz and logs the time-to-converge.  Called once from main.c boot
 * if SPRING_BOOT_SMOKE is defined.  Returns the number of presets that
 * converged within SPRING_MAX_ELAPSED_S.
 */
int spring_anim_boot_smoke(void);

#ifdef __cplusplus
}
#endif
