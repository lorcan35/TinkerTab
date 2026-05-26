# Ambient Orb Upgrades — "Living + Glanceable" — Design Spec (2026-05-26)

Builds on the orb redesign (`docs/superpowers/specs/2026-05-14-orb-redesign-design.md`,
shipped as `main/ui_orb.{c,h}`, PR #513). This spec adds **ambient** (resting-state)
upgrades that make the home orb feel like a living, glanceable, volumetric object —
without breaking the orb's two load-bearing invariants:

1. **One active motion per state** (`ui_orb.h` contract).
2. **The ESP32-P4 / LVGL 9 render budget** (see "Budget discipline" below).

## Goal

The orb is already rich: a lit-sphere with a 4-state machine
(IDLE / LISTENING / PROCESSING / SPEAKING), circadian palette, hardware reactivity
(BMI270 tilt-spec, ES7210 mic-RMS bloom, SC202CS presence-dim), a sleep cycle
(AWAKE / DROWSY / ASLEEP), and an FX playground (`expand/spin/glass/rainbow/shake`).

What's missing is **ambient quality at rest**: when nothing is happening, the orb is
a fairly static gradient sphere. We want the resting orb to (a) feel *alive*, (b) be
*glanceably informative* about the state of the user's world, and (c) read as a
*premium, volumetric* object — all three, chosen by the user.

This is scoped to the **IDLE / resting** experience. LISTENING / PROCESSING / SPEAKING
motion is unchanged; the premium base layer and the ambient accent still render in
those states, but no new per-state motion is added.

## Architecture: four additive layers

All layers attach to the existing `ui_orb` objects (`s_body`, `s_spec`, `s_halo`,
`s_inner_core`) and timers (`s_idle_breath_timer`, `s_tilt_timer`). No new state is
added to the 4-state machine.

### Layer A — Premium depth (base quality, always on, no new motion)

Static or palette-driven visual richness. Renders in every state.

- **A1 Secondary rim-light.** A thin, low-opacity **cool counter-light** along the
  edge *opposite* the warm specular highlight. Gives the sphere true two-light volume
  instead of a single top-lit gradient. Implemented as a thin edge element (arc or a
  clipped ring/border), repainted only on circadian palette changes — never per tick.
- **A2 Contact-glow.** A soft, low-opacity ellipse beneath the orb to ground it and
  suggest float/depth. **Pre-baked as a solid-color ellipse** with radial-ish opacity
  via a static asset or a single low-opa rounded object — **not** a live `shadow_width`
  blur (that busts the budget on a ≥200 px object). Static; repaints with palette only.
- **A3 Specular polish.** Soften the existing `s_spec` highlight's opacity falloff so it
  reads like light on a glossy surface rather than a flat blob. This refines the *target*
  of the existing tilt+lissajous drift; it does not add a motion channel.

### Layer B — Organic life (enhances the existing single idle motion)

The orb already has (1) an idle breath (`s_idle_breath_timer`, opa channel on the halo)
and (2) a lissajous specular drift (`s_spec` position via `s_tilt_timer`). Layer B makes
both feel organic. **No new motion channel is introduced** — both enhancements ride the
existing channels, so the "one motion per state" budget is preserved.

- **B1 Irregular breath.** Add ±~15 % jitter to the idle breath's period and amplitude so
  it breathes like a living thing rather than a metronome. Same opa channel; the jitter is
  a per-cycle modulation of `s_idle_breath_period_ms` / target opa.
- **B2 Organic drift.** Layer a slow secondary wander (sum of two slow sines, or a small
  value-noise walk) on top of the existing lissajous specular drift so the highlight never
  retraces the same path — the orb is subtly alive **even when the device is still and the
  room is dark** (i.e. when IMU tilt and mic RMS are both quiet). Same `s_spec` position
  channel; the wander is an additive offset to the lissajous target.

### Layer C — Ambient accent (one prioritized glanceable channel)

A single subtle **accent** at rest that surfaces only the **highest-priority** ambient
signal — never more than one at a time, so the orb never becomes a soup of competing
colors. The accent behaves like the existing **presence-dim** layer: a slow global
channel on top of the base, **not** counted as a per-state motion.

Rendered as a thin **rim** (border color + opacity on the body, or a thin ring sibling)
and/or a slow **ember** opacity breath, updated at low frequency (the existing ~2 s home
refresh tick, via a new `ui_orb_repaint_*`-style hook). Priority, resolved each tick:

| Pri | Signal | Source | Visual |
|-----|--------|--------|--------|
| 1 | Unread channel messages | new `ui_notification_active_count()` | faint colored ember (channel-tone or neutral amber), gentle breath |
| 2 | Degraded health | `voice_is_connected()` false in a Dragon mode, **or** `voice_onboard_failover_state() != READY` in vmode 4 | faint **cool desaturation** rim (calm, not alarming) |
| 3 | Near daily cap | `tab5_budget_get_today_mils() >= 0.8 * tab5_budget_get_cap_mils()` (cap > 0) | faint **warm** rim, opacity scales from 80 %→100 % of cap |
| — | none | — | no accent (clean orb) |

Notes:
- The accent is **suppressed during DROWSY/ASLEEP** (don't light up a sleeping orb) and
  during LISTENING/PROCESSING/SPEAKING (the state visuals own the orb then).
- Health "degraded" must be **mode-aware**: Solo (vmode 5) and TinkerON (vmode 4, warm)
  don't need Dragon, so Dragon-offline is *not* degraded in those modes.
- Dependency: notifications currently expose **no** unread count — add a small
  `ui_notification_active_count()` accessor (reads the existing dedupe/snooze ring state).

### Layer D — Event micro-pulse ("noticed")

A gentle **double-pulse** (two brief halo-opa bumps, ~150–250 ms each) when a notable
event lands, so the orb visibly "notices":

- Incoming `channel_message` (routed through `ui_notification.c`).
- Voice-mode change (the existing `ui_orb_paint_for_mode` hook).

One-shot transient on the existing pulse/halo opa channel — **not** a sustained motion,
so it does not violate the per-state motion budget. Suppressed while a higher-priority
state animation is mid-flight (PROCESSING comet) and during quiet hours.

## Budget discipline (non-negotiable)

Every effect MUST stay inside the documented LVGL 9 / ESP32-P4 render budget. From prior
incidents (LEARNINGS + reference memory):

- **No** `shadow_width` ≥ 16 px on objects ≥ 200 px at ≥ 5 Hz invalidation (or ≥ 8 px on a
  20 Hz tilt-driven object) → mutex-timeout stalls. (Drives A2 = pre-baked, not live blur.)
- **No** multi-stop (≥ 5) `lv_grad_dsc_t` on the ~280 px body at ≥ 30 Hz invalidation →
  continuous mutex timeout. (Rim-light A1 is a thin edge element, not a re-rastered body
  gradient.)
- **No** `transform_scale` on a gradient-bg object → re-rasterize per tick.
- `lv_arc` `bg_angles` must stay within 0–360 (use `lv_arc_set_rotation`).

Allowed techniques only: **opacity animations** on existing objects, **position moves of
small objects** (the specular), **thin rim/border** elements, **infrequent palette
repaints** (circadian / accent on the 2 s tick), and **pre-baked static** assets.

## Motion-budget accounting (the "one motion per state" rule)

| State | The one motion | Plus (non-motion global channels) |
|-------|----------------|-----------------------------------|
| IDLE | organic specular drift (B2, enhanced existing) | breath (B1, existing opa), presence-dim, ambient accent (C), premium base (A) |
| LISTENING | voice-bloom + listening-lean (unchanged) | tilt-spec, premium base (A) |
| PROCESSING | skill-comet (unchanged) | premium base (A) |
| SPEAKING | steady warm halo (unchanged) | premium base (A) |

The ambient accent (C) and presence-dim are slow global multipliers/tints, not motions —
the same accounting the orb redesign already uses for presence-dim. Event pulse (D) is a
transient one-shot, not a sustained motion.

## Observability + control

Extend the existing surfaces rather than add new ones:

- **`ui_orb_fx_t`** (`/orb/fx`): add toggles `rim_light`, `contact_glow`, `organic`
  (B1+B2), `ambient_accent`, `event_pulse` so each layer is independently togglable for
  tuning + A/B comparison.
- **`ui_orb_motion_state_t`** (`/orb/motion`): add `accent_signal` (enum: none/unread/
  health/cap), `accent_opa`, and `breath_jitter` so the live behavior is pollable.

## Phasing (for the implementation plan)

1. **Phase 1 — Layer A (premium depth):** rim-light, contact-glow, specular polish. Pure
   visual, low risk, no new data sources. Highest visual payoff per unit risk.
2. **Phase 2 — Layer B (organic life):** irregular breath + organic drift. Tuning-heavy
   (get the jitter/wander tasteful, not jittery).
3. **Phase 3 — Layer C (ambient accent):** add `ui_notification_active_count()`, the
   priority resolver, and the rim/ember render. Mode-aware health gating.
4. **Phase 4 — Layer D (event micro-pulse).**

Each phase builds + flashes + verifies on Tab5 (no LVGL unit harness): clean
`idf.py build`, `git-clang-format --diff origin/main` clean, on-device screenshot + a
short heap-stability check (the orb runs continuously — confirm no mutex-timeout / heap
drift after each phase, since render-budget regressions show up as sustained stalls).

## Out of scope (YAGNI)

- External-data tints (weather, calendar) — no data source on-device today.
- Simultaneous multi-signal display in Layer C (priority shows exactly one).
- Audio-reactive *idle* motion — mic RMS drives LISTENING bloom by design; idle stays
  calm.
- Any new orb state or change to the LISTENING/PROCESSING/SPEAKING motion.

## Success criteria

- At rest, in a still/dark room, the orb is visibly (subtly) alive — the highlight drifts
  organically and the breath is non-metronomic.
- The orb reads as volumetric (two-light) and grounded (contact glow), not a flat gradient.
- A single glance communicates the top ambient signal (unread / degraded / near-cap) via
  color, with no text and no competing signals.
- A notable event produces a clear "noticed" pulse.
- No render-budget regression: continuous-run heap + frame stability unchanged from
  baseline after all four layers.
