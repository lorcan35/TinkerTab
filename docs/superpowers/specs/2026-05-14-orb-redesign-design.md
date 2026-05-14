# Orb redesign — design spec

**Date:** 2026-05-14
**Tracking issue:** TT #511 (in-flight); follow-up issue to be filed for the wave-1 implementation.
**Mental model:** Layered orb — material physics base / system mirror mid / creature top.
**Approach:** Extract orb code from `main/ui_home.c` into a new `main/ui_orb.{c,h}` module. One PR landing all wave-1 behaviors against a clean state machine.

---

## 1. Problem

The home-screen orb has been patched ~10 times across TT #501–#511 (ember, rings, halo, breath, scale, gradient, ground-shadow, IMU specular) and still doesn't feel right. Each fix shifted the visual baseline so the next critique landed on a different layer. Three root causes:

1. **No module boundary.** All orb state, animation, rendering, and event handling lives inside `main/ui_home.c` (~3,300 LOC). The orb has accreted dozens of state vars and ~6 animation paths inside that file. Nobody — including future-us — can reason about "what does the orb do?" without grepping a giant home-screen file.
2. **No state machine.** Behaviors are bolted on globally (always-on halo breath, always-on peak meter timer) so they collide when the user is talking, when a tool runs, when Dragon disconnects. Each surface drew on the orb but they didn't know about each other.
3. **Patches over redesign.** The "make it alive" arc kept adding small layers (ember, scale anim, ground shadow) without revisiting the composition. Each layer competed for the same visual surface; the orb stopped reading as one object.

## 2. Goal

Land a single, clean orb redesign that:

- Lives in its own module (`main/ui_orb.{c,h}`) so it can be reasoned about in isolation.
- Has an explicit 4-state machine (IDLE / LISTENING / PROCESSING / SPEAKING) where each state has a strict motion budget — only specific behaviors run in specific states, so layers never compete.
- Earns its keep with the unused Tab5 hardware: IMU (tilt → specular drift), mic array (RMS → bloom), camera (face presence → ambient brightness).
- Feels alive at three layers simultaneously: material (physics), system mirror (subtle state shifts), creature (interactive expression).

## 3. Decisions (locked from clarifying round)

- **Approach: A.** Extract `ui_orb.{c,h}` and ship one PR for all wave-1 behaviors.
- **Wave-1 behaviors (5):** tilt-driven specular, voice-volume bloom, listening lean, skill-rim comet, presence wake.
- **Out of scope for wave 1:** status-encoded ring, tap-heat, orb-as-timer / orb-shows-reminder / orb-grows-on-unread. Not picked; defer or drop.
- **Always-on behaviors that stay:** circadian palette (already shipped in #503), the orb's vertical gradient body (shipped in #510), `ui_home_orb_force_hour` debug endpoint.
- **The IMU bug from #511 must be diagnosed and fixed**, not skipped. Until tilt is provably working with a `/imu` debug endpoint reading live, none of the IMU-derived behaviors (tilt, listening-lean) are testable.

## 4. State machine

```
                                 +-----------+
                                 |   IDLE    |--- circadian + tilt-spec + (presence dim)
                                 +-----------+
                                  ^    |   ^
                voice_off / done  |    | mic_start  
                                  |    v   
                +------------+    +--------------+
                | PROCESSING |<---|  LISTENING   |--- voice-bloom + listening-lean + tilt-spec
                +------------+    +--------------+
                   ^      |
        llm_start  |      | llm_done with finish=true
                   |      v
                   |  +----------+
                   '--| SPEAKING |--- steady warm halo + bright spec
                      +----------+
```

State transitions are driven by the existing `voice_set_state()` callbacks in `voice.c`. The orb subscribes via a new `ui_orb_on_voice_state(voice_state_t)` API; current `voice_set_state` already invalidates the home screen, so the wiring is one extra call.

## 5. Motion budgets per state

Strict rule: **exactly one motion runs in each state, plus zero or one ambient drift.** Motion = anything that invalidates >1 LVGL obj per frame. Ambient drift = lv_obj_set_pos on a single child of s_orb. The presence-wake dim is a global multiplier; it does not count toward the budget.

| State | Active motion | Ambient drift | Static layers |
|-------|---------------|---------------|---------------|
| IDLE | Slow body breath: scale ±2 % over 8 s (sine path) | Tilt → specular drift (20 Hz IMU poll) | Circadian gradient |
| LISTENING | Voice bloom: separate halo-obj `bg_opa` pulses with mic RMS (10 Hz EMA, 30 ms write throttle). NOT LVGL `box_shadow` (too expensive on ESP32-P4 for big circles — known [[reference-lvgl-bestpractices]] regression). | Listening lean: specular eases toward (cx, cy + 8) over 200 ms; then resumes tilt drift | Body gradient |
| PROCESSING | Skill comet: 16 px disc orbits the orb's equator over 3.6 s (linear) | None — specular freezes at rest | Body gradient |
| SPEAKING | None | None — specular freezes at rest | Body gradient (warmer), halo at steady opa 60 |

## 6. Behavior implementations (5)

### 6.1 Tilt-driven specular *(material layer)*

**Diagnosis-first.** Before wiring anything, add `GET /imu` debug endpoint returning `{ok, accel: {x,y,z}, gyro: {x,y,z}, age_ms}`. Verify on live hardware:

- IMU init succeeded at boot (`s_imu_ok=true` in main.c).
- `tab5_imu_read()` returns ESP_OK with non-zero values when the Tab5 is held in different orientations.
- `accel.z ≈ 1.0` when Tab5 is face-up flat on a desk; `accel.x ≈ -1.0` when device is rotated 90° CW (USB-C on left).

If the diagnostic shows IMU is dead → file separate issue, downgrade tilt to a roadmap item. If diagnostic shows IMU live but values look wrong → rescale `k_xy` (currently 18 px/g, may need to be 30+ to be visible) and re-check sign of axis mapping. The #511 attempt failed for one of these reasons.

**Implementation.** Same as #511's `orb_specular_tick_cb` (20 Hz lv_timer, accel.x/y → low-pass EMA → lv_obj_set_pos on `s_orb_highlight` within `s_orb`), but:

- Move to `ui_orb.c` as `orb_tilt_tick`.
- Tunable `ORB_TILT_K_PX_PER_G` constant (start at 22).
- Sign-correct mapping verified against live `/imu` output.
- Disabled in PROCESSING + SPEAKING states (timer paused via `lv_timer_pause`).

### 6.2 Voice-volume bloom *(creature layer)*

**Replaces** the dead halo peak meter from #508 (which drove `s_halo_outer.bg_opa` but that obj is now gone after #511).

**Implementation.**
- New child obj `s_orb_halo` of `s_screen`, sibling-behind `s_orb`. 220 px diameter, radius full, `bg_color=TH_AMBER`, `bg_opa=0` at rest.
- 10 Hz `lv_timer` polls `voice_get_current_rms()` (returns 0.0..1.0 float).
- EMA-smoothed (`α=0.3`) RMS drives `bg_opa = 25 + (rms_ema * 55)` → range 25..80.
- Only ticks in LISTENING state. State transition out clamps halo back to 0.
- Set via `lv_obj_set_style_bg_opa` — only invalidates the halo's own bbox, not the orb.

### 6.3 Listening lean *(creature layer)*

**Implementation.**
- On LISTENING enter, snap target lean: specular rest position shifts from (cx − 18, 16) → (cx − 4, 30). Anim duration 220 ms, ease_out.
- On LISTENING exit, anim back to rest.
- During LISTENING, tilt drift is applied as offset on TOP of the leaned rest pos.

### 6.4 Skill-rim comet *(system mirror layer)*

**Replaces** the ripple shower in #501 (`ui_home_orb_ripple_for_tool`). Today's behavior: every tool_call spawns a new border-only expanding circle (up to 4 simultaneously, recycled). New behavior: ONE comet orbits the equator the entire time a tool is active.

**Implementation.**
- New child obj `s_orb_comet` of `s_screen`, 18 × 18 px circle, opa 0 at rest, color `0xFFE0A0`.
- On `tool_call` WS event in `voice_ws_proto.c`: enter PROCESSING state, animate `s_orb_comet` along a circular path at `ORB_COMET_RADIUS = 100 px` from orb center, 3.6 s linear, infinite repeat. opa 0 → 240 over 240 ms fade-in.
- On `tool_result` or LLM `finish=true`: fade comet out 240 ms, exit PROCESSING.
- The existing `ui_home_orb_ripple_for_tool` API stays as a no-op shim for one release for binary compat with `voice_ws_proto.c`.

### 6.5 Presence wake *(creature layer, cross-state)*

**The novel hardware-aware piece.** Camera samples face-presence at 0.2 Hz (every 5 s). When no face seen → orb-wide brightness multiplier drops to 0.35, all motion timers slow (×0.4). When face seen → snap back to 1.0.

**Implementation.**
- New module function `ui_orb_set_presence(bool near)`.
- Caller: a new periodic job posted to `tab5_worker` every 5 s, grabs one camera frame (320 × 180 ds), runs a cheap face-presence heuristic (NOT recognition — just "does the frame have a skin-tone connected blob ≥ N pixels at center 60 %?"). Single bool out.
- Frame is held in `task_worker` thread; never crosses LVGL thread.
- `ui_orb_set_presence(false)` posts a `tab5_lv_async_call` that sets `s_orb_dim_active = true` and `lv_obj_set_style_opa(s_orb_root, 90, LV_PART_MAIN)`; reverses on `true`.
- **Privacy + power:** 0.2 Hz polling, no upload, no recognition, no persistence. Easy NVS toggle `presence_on` (default true) so user can disable.
- **Settings UI** gets one row: "Presence wake — orb dims when room is empty. Off / On."

## 6.5b Face-presence heuristic — explicit non-goals

This is NOT face recognition. NOT identity. NOT counting people. It is a single bool: "did the lens see anything that *might* be a face within ~2 m." False positives (a salmon-colored cushion) are fine — they'll resolve next sample. False negatives (you walking out of frame) are fine — orb dims a few seconds later. The point is *probabilistic ambient awareness*, not a security camera.

Algorithm: downsample camera frame to 320 × 180, convert to HSV, threshold hue ∈ [5°, 50°] ∧ saturation ∈ [0.15, 0.85] ∧ value ∈ [0.25, 0.95] → binary mask. Largest connected component in the center 60 % rectangle. Return `area ≥ 600 px`.

CPU budget: < 30 ms per sample; runs on `tab5_worker` not LVGL thread.

## 7. Module structure

### 7.1 New: `main/ui_orb.h`

```c
#pragma once
#include "lvgl.h"
#include <stdbool.h>

typedef enum {
   ORB_STATE_IDLE,
   ORB_STATE_LISTENING,
   ORB_STATE_PROCESSING,
   ORB_STATE_SPEAKING,
} ui_orb_state_t;

/** Build the orb hierarchy (body + halo + specular + comet) as children of `parent`,
 *  positioned with center at (cx, cy). Idempotent: safe to call once per home screen. */
void ui_orb_create(lv_obj_t *parent, int cx, int cy);

/** Destroy all orb objects + stop all timers. Safe to call when not created. */
void ui_orb_destroy(void);

/** Drive the orb's state machine. Called from voice_set_state() callback. */
void ui_orb_set_state(ui_orb_state_t s);

/** Cross-state ambient dim hook for presence-wake. */
void ui_orb_set_presence(bool near);

/** Hour-of-day override for circadian palette debug. -1 = clear override. */
void ui_orb_force_hour(int hour);

/** Trigger a one-shot tool ripple — kept as a no-op shim for one release
 *  so voice_ws_proto.c links cleanly. Will be removed once that file moves
 *  to the new comet API. */
void ui_orb_ripple_for_tool(const char *tool_name);

/** Click + long-press attach point — returns the LVGL obj that should receive
 *  the click / long-press handlers. ui_home wires its existing callbacks. */
lv_obj_t *ui_orb_root(void);
```

### 7.2 New: `main/ui_orb.c`

Owns:
- All static obj handles (`s_orb_root`, `s_orb`, `s_orb_halo`, `s_orb_highlight`, `s_orb_comet`).
- The state machine (`s_state`, transitions, timer arm/disarm logic).
- The 5 behavior tick callbacks (`orb_tilt_tick`, `orb_bloom_tick`, `orb_comet_anim_cb`).
- Circadian palette logic (moved verbatim from `ui_home.c`).
- Presence-wake state (`s_orb_dim_active`, opa override).

Approximately 600–700 LOC.

### 7.3 Changes to `main/ui_home.c`

- Remove all orb code (the ~600 LOC of object creation, breath/peak/ripple anims, paint_for_mode).
- In `ui_home_create`, call `ui_orb_create(s_screen, 360 /* cx */, 320 /* cy */)`. Sizing constants (`ORB_SIZE = 180`, halo / comet radii) move to `ui_orb.c` private defines.
- In `ui_home_destroy`, call `ui_orb_destroy()`.
- The existing `ui_home_orb_*` public APIs forward to `ui_orb_*` for one release (binary compat shim).

### 7.4 Changes to `main/voice.c` / `main/voice_ws_proto.c`

- `voice_set_state()` calls a new `ui_orb_set_state(map_voice_to_orb(s))` shim (added in `ui_home.c` since that owns the home/voice screen distinction).
- `voice_ws_proto.c`'s `tool_call` handler keeps calling `ui_home_orb_ripple_for_tool` (which forwards to `ui_orb_ripple_for_tool`). One follow-up PR converts it to `ui_orb_set_state(ORB_STATE_PROCESSING)` directly.

### 7.5 New debug endpoint: `GET /imu`

Added to `main/debug_server_metrics.c` (lives alongside other live-state endpoints). Returns:

```json
{
  "ok": true,
  "accel": {"x": -0.02, "y": -0.01, "z": 0.99},
  "gyro":  {"x": 0.4, "y": -0.1, "z": 0.0},
  "age_ms": 12
}
```

Bearer-auth, same as other endpoints. Used during wave-1 implementation to diagnose the #511 IMU failure (and as a regression gate).

## 8. Open questions / risks

- **Hardware face-presence heuristic robustness.** Skin-tone-blob detection is brittle (clothing, lighting). If false positives are constant ("orb is always 'awake'") or false negatives ("orb dims even when I'm standing in front of it") tank the experience, we'll need a smarter detector. Path: ESP32-P4 has an unused HW accelerator for image ops; revisit if v1 heuristic fails. Fallback: disable presence wake by default for now (NVS `presence_on = false`).
- **lv_obj_set_pos on a child of a radius-full clipped parent.** Need to verify in code that LVGL actually invalidates the clipped region on partial-mode renders. If not, the tilt drift won't be visible — that's the most likely cause of the #511 failure. The `/imu` endpoint + a temporary `/orb/highlight_pos?x=N&y=M` debug poke will isolate this in 10 minutes.
- **Power.** Camera grab every 5 s for presence wake costs ~30 ms of CPU + ~12 mA brief draw. Acceptable. If the sample triggers MIPI lane init each time, we'll need to keep the camera warm. Path: profile during implementation; if cold-init cost is real, leave camera open in low-power mode.

## 9. Test plan

### 9.1 Unit-equivalent tests (run on host)

Skipped — this is all hardware-coupled rendering + I2C.

### 9.2 On-device acceptance

Live tests run against Tab5 at 192.168.1.90 via the debug HTTP server:

1. **`/imu` endpoint sanity:** With Tab5 flat-face-up, `accel.z` ∈ [0.85, 1.15]. Rotate 90° CW → `accel.x` ∈ [-1.15, -0.85].
2. **Tilt drift visible:** With orb in IDLE, tilt Tab5 30° to the right; highlight pos shifts ≥ 12 px to the left. Reverse → highlight returns ≥ 12 px right. (Diff via screenshots before/after.)
3. **State transitions:** Trigger each voice state via debug endpoints; screenshot at each; visually confirm exactly the behaviors documented in §5.
4. **Voice bloom:** Start listening; speak loudly into mic; halo opa visibly increases. Stop speaking; halo opa decays.
5. **Skill comet:** Send a chat that triggers `web_search`; confirm one comet orbits the orb until result. Confirm no ripple shower (old behavior gone).
6. **Presence wake:** With NVS `presence_on=true`, walk away from the Tab5 → after ~10 s the orb dims. Walk back → orb brightens within next sample.
7. **No regression:** voice mode cycle (0..5), nav to camera/notes/settings/home, take a photo, hold orb for mode sheet, all still work.
8. **Compaction proof:** Memory snapshot saved before merge. No live state references stale `s_halo_outer` / `s_halo_inner` / `s_ring_*` objects (those handles stay declared as NULL for one release for binary compat with #511 changes, then deleted).

### 9.3 Stability

- 30-minute idle soak: no reboot, heap stable within 1 MB, fragmentation watchdog quiet.
- `story_full` e2e harness pass (`tests/e2e/runner.py story_full`).

## 10. Out of scope (filed as follow-ups)

- Status-encoded rim (not picked). If the system-mirror layer matters more later, revisit as wave 3.
- Tap-heat (not picked).
- Orb-as-timer / orb-shows-next-reminder / orb-grows-on-unread (not raised during brainstorm; if needed later they're additive widget-style overlays, not orb-state).
- Smarter face-presence detector (defer until v1 heuristic fails empirically).
- Removal of `s_halo_outer` / `s_halo_inner` / `s_ring_*` static declarations (kept as NULL handles for one release for binary compat with `ui_home.c` callers; removed in a tiny cleanup PR after the orb redesign settles).

## 11. Implementation order (within the single PR)

1. Add `/imu` debug endpoint → verify on hardware → confirm IMU live.
2. Scaffold `main/ui_orb.{c,h}` with empty state machine + obj creation.
3. Move circadian palette + body gradient + click/long-press wiring from `ui_home.c` to `ui_orb.c`. Boot test: orb still appears, still tappable.
4. Add tilt-spec implementation; verify drift via screenshot diff.
5. Add bloom + listening-lean (LISTENING state).
6. Add skill comet (PROCESSING state).
7. Add SPEAKING state (warmer tint, steady halo).
8. Add presence-wake hook + camera-grab job + Settings row.
9. Run §9 test plan end-to-end. Snapshot memory. Open PR.

---

**Estimated effort:** 1 day of focused work for steps 1–7, plus 0.5 day for step 8 (camera coupling is the unknown), plus 0.5 day for §9.3 stability soak. Two days end-to-end.
