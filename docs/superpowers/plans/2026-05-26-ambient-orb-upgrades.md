# Ambient Orb Upgrades Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (firmware on a physical device — stateful flash/verify, not subagent-parallelizable). Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make the resting home orb feel alive, glanceably informative, and premium-volumetric, via four additive layers — without breaking the orb's "one motion per state" rule or the ESP32-P4 / LVGL 9 render budget.

**Architecture:** All work lands in `main/ui_orb.{c,h}` (+ one small accessor in `main/ui_notification.{c,h}`). Layers attach to existing objects (`s_body`, `s_spec`, `s_halo`, `s_inner_core`) and the existing timers (`s_idle_breath_timer`, `s_tilt_timer`). No new orb state. Each layer is gated behind a flag in the existing `ui_orb_fx_t` so it's independently togglable via `/orb/fx`, and surfaced in `ui_orb_motion_state_t` via `/orb/motion`.

**Tech Stack:** ESP-IDF 5.5.2, LVGL 9.x, ESP32-P4, C. Spec: `docs/superpowers/specs/2026-05-26-ambient-orb-upgrades-design.md`.

**Verification model (firmware — read this):** No LVGL unit-test harness. Each phase's "test" is the real loop: `. /home/rebelforce/esp/esp-idf/export.sh && idf.py build` clean → `git-clang-format --binary clang-format-18 --diff origin/main main/ui_orb.c` clean → `idf.py -p /dev/ttyACM0 flash` → on-device checks via the debug server (`TOK=05eed3b13bf62d92cfd8ac424438b9f2`, Tab5 `192.168.1.90:8080`): `GET /screenshot.jpg` (pace + retry on 429; reboot clears a stuck busy-flag per #734), `GET /orb/fx`, `GET /orb/motion`, `POST /orb/fx?...`. Because the orb runs continuously, **also confirm no render-budget regression**: after each phase watch `GET /info` `heap_min` + serial for `mutex timeout` / `TASK_WDT` over ~60 s of IDLE.

**Branch:** `feat/ambient-orb-upgrades` (off main). Commit after each task.

**Budget rules (NON-NEGOTIABLE — these have caused mutex-timeout stalls before):**
- No `shadow_width` ≥16 px on objects ≥200 px (A2 contact-glow is a pre-baked/solid low-opa ellipse, NOT a live shadow).
- No ≥5-stop `lv_grad_dsc_t` on the ~280 px body at high invalidation (A1 rim-light is a thin edge element, not a body gradient).
- No `transform_scale` on a gradient-bg object.
- `lv_arc` `bg_angles` within 0–360 (use `lv_arc_set_rotation`).
- Allowed: opa animations on existing objects, small-object position moves, thin rim/border elements, palette repaints on the ~2 s tick, pre-baked static assets.

---

### Task 0: Extend fx + motion structs (shared scaffolding)

**Files:** Modify `main/ui_orb.h`, `main/ui_orb.c`.

- [ ] **Step 1:** In `ui_orb.h`, extend `ui_orb_fx_t` with the new layer toggles (append, don't reorder existing fields):
  ```c
  bool rim_light;     /* A1: secondary cool counter-light */
  bool contact_glow;  /* A2: grounding ellipse beneath the orb */
  bool organic;       /* B1+B2: irregular breath + organic drift */
  bool ambient_accent;/* C: one prioritized glanceable rim/ember */
  bool event_pulse;   /* D: "noticed" double-pulse on events */
  ```
- [ ] **Step 2:** In `ui_orb.h`, extend `ui_orb_motion_state_t` with: `uint8_t accent_signal; /* 0 none 1 unread 2 health 3 cap */`, `uint8_t accent_opa;`, `uint8_t breath_jitter_pct;`.
- [ ] **Step 3:** In `ui_orb.c`, default the new fx flags ON at init (so the upgrades are live by default) where `s_fx` is initialized; populate the new motion fields in `ui_orb_get_motion_state`.
- [ ] **Step 4:** Build + format. Commit `feat(orb): fx + motion scaffolding for ambient upgrades`.

---

### Task 1 — Phase A: Premium depth

**Files:** Modify `main/ui_orb.c` (object creation in `ui_orb_create`, palette repaint path).

- [ ] **Step 1 (A2 contact-glow):** In `ui_orb_create`, before the body is created (so it's behind), add `s_contact_glow`: a low-opa (≈22) solid rounded ellipse, width ≈ body_w*0.9, height ≈ 24 px, positioned just below the body's bottom edge, radius = height/2, color = a dark warm shadow tone derived from the circadian palette (NOT `shadow_width`). Gate render on `s_fx.contact_glow`.
- [ ] **Step 2 (A1 rim-light):** Add `s_rim` as a child sibling of the body: a thin ring/edge element (border-only `lv_obj`, `border_width`≈2, `bg_opa`=0) sized = body, `border_color` = a cool counter-light tint of the palette, `border_opa`≈40, **only the lower-opposite arc visible** — implement as a full thin ring at low opa (cheap) OR a small offset highlight on the opposite edge; choose the ring (simplest, budget-safe). Gate on `s_fx.rim_light`.
- [ ] **Step 3 (A3 specular polish):** Soften `s_spec` — lower its peak `bg_opa` slightly and/or increase radius so the highlight falloff reads glossier. Pure styling tweak at creation.
- [ ] **Step 4:** Wire `s_contact_glow` + `s_rim` colors into the existing circadian palette repaint function (so they re-tint on hour change) and NULL-reset them in `ui_orb_destroy`.
- [ ] **Step 5:** Build + format + flash. Verify via `/screenshot.jpg` on home IDLE: orb reads volumetric (cool rim opposite the warm highlight) + grounded (soft glow beneath). Toggle each via `POST /orb/fx` and screenshot to confirm independent on/off. Confirm `heap_min` stable + no `mutex timeout` over 60 s.
- [ ] **Step 6:** Commit `feat(orb): premium depth — rim-light + contact-glow + specular polish (Phase A)`.

---

### Task 2 — Phase B: Organic life

**Files:** Modify `main/ui_orb.c` (idle breath timer cb, tilt/drift timer cb).

- [ ] **Step 1 (B1 irregular breath):** In the idle-breath path, when `s_fx.organic`, modulate the breath: each cycle pick a new period within `IDLE_BREATH_*_MS * (1.0 ± 0.15)` and amplitude target within ±15 %, using a cheap LCG/`esp_random()`-seeded jitter. Store `breath_jitter_pct` for telemetry. Keep the same opa channel.
- [ ] **Step 2 (B2 organic drift):** In the specular drift target computation (the lissajous in `s_tilt_timer` cb), when `s_fx.organic`, add a slow secondary wander = sum of two low-freq sines with incommensurate periods (e.g. 0.013 Hz + 0.021 Hz) scaled to ±~6 px, added to the lissajous target so the highlight path never exactly repeats. Same `s_spec` position channel — no new timer.
- [ ] **Step 3:** Build + format + flash. Verify on home IDLE with the device held still in a quiet room (so tilt+mic are quiet): the highlight visibly drifts on a non-repeating path and the breath is non-metronomic. Toggle `organic` off via `/orb/fx` → confirm it reverts to the prior periodic motion. Confirm no budget regression (drift is small-object position moves; breath is opa — both cheap).
- [ ] **Step 4:** Commit `feat(orb): organic life — irregular breath + organic drift (Phase B)`.

---

### Task 3 — Phase C: Ambient accent

**Files:** Modify `main/ui_notification.{c,h}` (add accessor), `main/ui_orb.c` (accent resolver + render).

- [ ] **Step 1:** In `ui_notification.{c,h}` add `int ui_notification_active_count(void);` returning the count of un-dismissed/active channel notifications (reads the existing dedupe/now-card/snooze ring state — return the number currently considered "pending" for the user). Keep it cheap + lock-safe.
- [ ] **Step 2 (resolver):** In `ui_orb.c` add `static uint8_t accent_resolve(void)` returning the priority winner: `1` if `ui_notification_active_count() > 0`; else `2` if health degraded (`!voice_is_connected()` AND current vmode needs Dragon [0/1/2/3], OR vmode==4 AND `voice_onboard_failover_state() != READY`); else `3` if `tab5_budget_get_cap_mils() > 0 && tab5_budget_get_today_mils() >= (cap*8)/10`; else `0`. Suppress (return 0) when state != IDLE or sleep_phase != AWAKE.
- [ ] **Step 3 (render):** Reuse `s_rim` (from Phase A) OR add `s_accent` thin ring. Drive its `border_color` + `border_opa` from `accent_resolve()` on the existing ~2 s repaint hook: signal 1 → channel-tone/amber ember (slow opa breath), 2 → cool desaturated tint, 3 → warm rim with opa scaled 80→100 % of cap. Signal 0 → opa 0. Gate on `s_fx.ambient_accent`. Store `accent_signal`/`accent_opa` for telemetry.
- [ ] **Step 4:** Build + format + flash. Verify each branch on device: (a) inject a channel_message (`POST /debug/inject_ws` or trigger a notification) → unread ember; (b) force Dragon offline (kill WS / `POST /voice/reconnect` mid-down) in a Dragon mode → cool rim; (c) set `cap_mils` low + `spent_mils` near it via `POST /settings` → warm rim. Confirm only ONE shows at a time per priority. Confirm accent vanishes in non-IDLE + sleep.
- [ ] **Step 5:** Commit `feat(orb): ambient accent — one prioritized glanceable channel (Phase C)`.

---

### Task 4 — Phase D: Event micro-pulse

**Files:** Modify `main/ui_orb.c` (pulse helper + public hook), wire from `ui_home.c` notification + mode-change paths if a hook is needed.

- [ ] **Step 1:** In `ui_orb.c` add `void ui_orb_event_pulse(void)` (public, declared in `ui_orb.h`): a one-shot that runs two brief halo `bg_opa` bumps (~150–250 ms each, +~40 opa over baseline) via the existing pulse/anim path. Gate on `s_fx.event_pulse`. Suppress during PROCESSING (comet owns motion) + quiet hours.
- [ ] **Step 2:** Call `ui_orb_event_pulse()` from: (a) the incoming `channel_message` handler path (where `ui_notification` surfaces a new message), and (b) `ui_orb_paint_for_mode` (mode change). Use the minimal wiring; if the message path is in `ui_notification.c`/`voice_ws_proto.c`, call via the public hook.
- [ ] **Step 3:** Build + format + flash. Verify: trigger a mode change (`POST /mode?m=N`) → orb double-pulses; inject a channel message → orb double-pulses. Confirm no pulse during PROCESSING.
- [ ] **Step 4:** Commit `feat(orb): event micro-pulse — "noticed" reaction (Phase D)`.

---

### Task 5 — Final regression + docs

- [ ] **Step 1:** 60–120 s IDLE soak on home with all layers on: `GET /info` heap_min stable, serial shows no `mutex timeout` / `TASK_WDT`, `GET /orb/motion` shows live drift/breath/accent values changing.
- [ ] **Step 2:** `git-clang-format --diff origin/main main/ui_orb.c main/ui_orb.h main/ui_notification.*` clean; `idf.py build` clean.
- [ ] **Step 3:** Update `GET /orb/fx` / `/orb/motion` debug docs in CLAUDE.md if the endpoints' fields are documented there.
- [ ] **Step 4:** Commit any remaining + finish via superpowers:finishing-a-development-branch.

---

## Self-Review

- **Spec coverage:** Layer A → Task 1; Layer B → Task 2; Layer C → Task 3 (+ notification accessor); Layer D → Task 4; observability (fx/motion) → Task 0; budget discipline → enforced in each task's technique choice + verified in each phase's soak check. ✅
- **Placeholder scan:** Per-task code is described against named real objects/timers/accessors (`s_spec`, `s_idle_breath_*`, `s_tilt_timer`, `ui_orb_fx_t`, `voice_is_connected`, `tab5_budget_*`). Exact line-level code is written against the live functions during execution (the module is 2,436 LOC; tasks name the function + object + math + gate, which is the actionable unit for this codebase). No "TBD/handle edge cases." ✅
- **Type consistency:** `s_fx`/`ui_orb_fx_t` new flags, `ui_orb_motion_state_t` new fields, `ui_orb_event_pulse`, `ui_notification_active_count`, `accent_resolve` used consistently across tasks. ✅
- **Risk:** the only cross-file dependencies are `ui_notification_active_count` (Task 3) and the pulse wiring (Task 4); both have a clear single call site. Everything else is contained to `ui_orb.c`.
