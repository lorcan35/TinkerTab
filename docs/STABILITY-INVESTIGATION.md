# TinkerTab Stability Investigation

> **Last updated:** 2026-04-23 (initial scaffold)
> **Current phase:** Phase 0 — scaffolding, not started measuring yet
> **Active branch (next):** `investigate/lvgl-pool-pressure`
> **Companion plan:** `docs/superpowers/plans/2026-04-23-lvgl-pool-investigation.md`

---

## RESUMING THIS WORK (read this first if you're picking up)

1. Read the **TL;DR** below (30 seconds).
2. Skim **Investigation log** — jump to the most recent dated entry. That tells you what was just tried and what came next.
3. Check open GitHub issues on TinkerTab tagged `stability` + the state of the active branch. `git log investigate/lvgl-pool-pressure --oneline` if it exists.
4. **Do NOT start one-off crash patches.** The whack-a-mole era ended with PR #178. This investigation is root-cause driven. If a new crash site surfaces during a phase, log it in the "Observed crash sites" table — don't fork a fix branch for it until Phase 3.
5. Read the phase plan in `docs/superpowers/plans/` and execute step-by-step with the `superpowers:executing-plans` skill.

---

## TL;DR

Under sustained stress (orchestrated nav + chat + screenshot + mode-switch cycles), Tab5 panics roughly every 170–200 seconds. Every panic's crash site is inside LVGL's draw pipeline, at a different NULL-deref each time (see "Observed crash sites" below). 12 PRs have been merged today, each closing one specific NULL-deref — and each has been followed by a new one surfacing deeper in the LVGL internals.

The pattern says root cause is **LVGL's working pool or draw scratch space is getting transiently exhausted under our stress rate**. Patching individual NULL-deref sites is whack-a-mole. This investigation takes a measurement-first approach to find which pool is the bottleneck and tune ONE config knob.

**Success criteria:** 30-minute stress run (same orchestrator at `/tmp/stress_orchestrator.sh`) with **zero crashes**.

---

## Problem statement

### Symptoms
- Under normal user pace (~1 UI action/minute): Tab5 restarts approximately every **20–30 minutes**.
- Under orchestrated stress (1 action every ~30 s, rotating through 6 action types): Tab5 restarts every **~3 minutes**.
- Every crash is `reset_reason = PANIC` (zombie-reboot `reset=SW` family closed in #169).
- Coredumps consistently land inside LVGL draw/mask/style code with a NULL source or destination.

### Reproducer
`/tmp/stress_orchestrator.sh` — runs 60 s cycles of: touch-nav sweep / mode-switch + chat / settings drill / mode-switch + chat / camera+files / screenshot. Logs to `/tmp/stress.csv` + `/tmp/stress.log`. Repros a panic within ~3 min of starting.

### Tab5 at idle = stable
With no orchestrator running, Tab5 survived >6 minutes continuously (measured earlier today) with no crashes. Panics only fire under sustained nav/render load. That rules out a slow memory leak in background tasks as the primary mechanism.

---

## What we've tried (PRs merged on main, 2026-04-23)

Each row: what was fixed, whether crashes continued after.

| PR | Title | Fixed | Crashes after? |
|---|---|---|---|
| #155 | chore(streaming): remove dead Dragon browser-streaming code | 1.8k LoC of dead surface | N/A — unrelated |
| #157 | chore: tidy post-streaming dead code + doc drift | 45 LoC of dead code | N/A |
| #164 | chore: remove parked wake-word / AFE stack | 527 LoC of dead code | N/A |
| #165 | chore(partitions): reclaim 3 MB model SPIFFS slot | flash reclaim | N/A |
| #166 | fix(lvgl): bounds-check get_next_line v1 | `get_next_line` OOB read | **Yes** — check was inlined + elided |
| #168 | fix(nav): destroy camera/files screens on nav-away | 1.8 MB PSRAM leak per camera cycle | Yes — unrelated crash class surfaced |
| #169 | fix(stability): zombie-reboot WS-alive override + LVGL v2 | `reset=SW` cluster + OOB v2 (`noinline`) + 2 alloc-fail guards in `lv_draw_sw_mask_radius_init` | Yes — new sites |
| #174 | fix(ui-notes): guard refresh_list against use-after-destroy from background task | cross-thread race on `s_list` from `transcription_queue_task` | Yes — different in same function |
| #175 | fix(ui-memory): guard render_hits_cb on overlay visibility | latent Class C | Yes |
| #176 | fix(ui-camera): idempotent create | latent double-create leak | Yes |
| #177 | fix(ui-home): idempotent create (defensive) | latent | Yes |
| #178 | fix(ui-notes): NULL-check every `lv_*_create()` in `add_note_card` | 9 unchecked `lv_*_create()` returns inside our widget builder | **Yes — still crashing** (current state) |

After #178: stress still crashes every ~170 s. Coredump moved from `add_note_card` → inside LVGL's own `lv_draw_sw_fill` (see "Observed crash sites" below).

---

## Current hypothesis

**LVGL's internal working memory (`work_mem_int` + draw-buffer pool + mask-buffer scratch) is being transiently exhausted or fragmented below the threshold needed for a rounded-border / mask-radius / draw-fill allocation.**

Why we think this:
- Every coredump post-#178 lands inside LVGL's draw pipeline on a NULL source or destination passed to `lv_memset` / `lv_label_set_text` / `draw_mask_radius`.
- Our code's `lv_*_create()` callers (fixed in #178) are now guarded, so the NULL is coming from LVGL's internal sub-allocations, not top-level widget creation.
- `lv_mem_monitor` (heap_wd log) shows base pool `used=54 KB free=33 KB` — the base 96 KB pool is ~half-exhausted under normal load, and the expand pool (`CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096`) status is NOT visible via `lv_mem_monitor`, so we don't know if it's actually being used.
- `/selftest` internal-heap: free 65 KB / largest block 62 KB at steady state. Under stress that largest block dropped to 36 KB in the CSV — still above the heap_wd 4 KB threshold but tight for anything allocating multi-KB scratch.
- Known LVGL internals that use `work_mem_int`: circle cache (pinned 32 entries), mask buffers, draw task descriptors.

### Why whack-a-mole won't converge
Each NULL-deref patch just pushes the failure one level deeper into LVGL's alloc chain. LVGL's internal code was not written to handle OOM — it assumes allocs succeed. Patching individual sites in managed_components is also fragile (fresh clones need the `tools/apply_patches.sh` hook, every new LVGL version re-breaks).

Better: make the alloc succeed. Find the pool that's exhausting and either grow it, reduce contention, or identify an actual leak reducing effective capacity.

---

## Context we've gathered

### Current LVGL config (sdkconfig + sdkconfig.defaults)
```
CONFIG_LV_MEM_SIZE_KILOBYTES=96                    base pool, statically allocated in BSS (SRAM-heavy)
CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096      expansion from system heap (PSRAM preferred)
CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=32             was 4, bumped after a prior crash
CONFIG_LV_DISPLAY_DEFAULT_REFRESH_PERIOD_MS=16     ~60 Hz target
CONFIG_LV_USE_ASSERT_MALLOC=n                      alloc-fail doesn't assert — returns NULL silently
CONFIG_LV_USE_ASSERT_NULL=n                        same for NULL derefs
```
Render mode: `LV_DISPLAY_RENDER_MODE_PARTIAL` with **two 144 KB draw buffers in PSRAM** (per CLAUDE.md).

### Observed crash sites (all LVGL-internal)

| Stress run | MEPC symbol | File:line | Nature | Status |
|---|---|---|---|---|
| 1 | `get_next_line` → `opa_start_on_y[y+1]` OOB read | `lv_draw_sw_mask.c:1249` | read past radius-sized array | Guarded in #166/#169 (NULL-return + noinline) |
| 2 | `circ_calc_aa4` → `cir_x[cir_size] = ...` | `lv_draw_sw_mask.c:1123` | alloc-fail → store to NULL | Guarded in #169 |
| 3 | `lv_label_set_text(NULL, ...)` via `add_note_card` | `ui_notes.c:1934` | unchecked `lv_*_create()` return | Guarded in #178 |
| 4 | `lv_memset(dst=NULL, v=0xFF, len=640)` via `lv_draw_sw_fill` | `lv_draw_sw_fill.c:161` | draw-op mask buffer NULL | **Current — unresolved** |

Registers at run 4: `a0=0x0` (dst to memset), ra `<lv_draw_sw_fill+438>`, pc `<lv_memset+68>`. Full dump at `/tmp/crash_v4.bin` (decoded against `build/tinkertab.elf`; ELF SHA may drift as firmware rebuilds).

### Baseline health numbers

| Metric | Idle | Under stress |
|---|---|---|
| Internal heap free (`/selftest`) | 65 KB | 36–50 KB |
| Internal heap largest block | 62 KB | 36–44 KB |
| PSRAM free (`/info`) | 21,171 KB | 21,170–21,172 KB (flat) |
| LVGL pool (`heap_wd`) | `used=54 free=33` (87 KB visible of 96 KB base) | drops to ~`free=20 KB` under heavy redraw |
| Tasks running | 24 | 24 |

Heap trend on PSRAM is flat across 30 min — no accumulating leak on the PSRAM side. Variation is confined to internal SRAM largest-block.

---

## Investigation phases

### Phase 1 — Measure (adds instrumentation only)
Log every time an LVGL-internal alloc returns NULL, with: which pool, requested size, current free + largest-block, calling context. Track frame-by-frame LVGL pool usage. Run the stress orchestrator for 10 minutes with instrumentation on.

**Deliverable:** `docs/STABILITY-INVESTIGATION.md` gets a new log entry with concrete data: which allocator fails, how close to the threshold it is when failing, whether base pool exhausts before expand kicks in.

### Phase 2 — Hypothesize
Based on Phase 1 data, identify exactly one of:
- (a) Base 96 KB pool exhausts before expand pool kicks in (expansion broken or too slow)
- (b) Expand pool hits its 4 MB cap
- (c) Internal SRAM outside LVGL's pool (the draw scratch in `work_mem_int`) is the bottleneck
- (d) Actual leak — some path keeps growing LVGL footprint over time without releasing

**Deliverable:** A single concrete hypothesis + one proposed config or code change.

### Phase 3 — Fix (single variable change)
Apply exactly one change from Phase 2 hypothesis. Discipline: if we change two things at once, we can't isolate. Tempting to stack config changes; resist.

**Deliverable:** One PR with one change. PR description quotes Phase 1 measurements + Phase 2 hypothesis + expected effect.

### Phase 4 — Validate
Run the 30-min stress orchestrator post-fix. Success = zero crashes. If still crashes, coredump tells us if we addressed the right bottleneck; return to Phase 2 with new data.

---

## Investigation log

Dated entries of every experiment. Append-only — never edit past entries.

### 2026-04-23 — Scaffolding (Phase 0, no experiment yet)
Scaffold doc committed on `docs/stability-investigation-scaffold` branch. No firmware change yet. Next action: cut `investigate/lvgl-pool-pressure` from main (after this doc lands) and start Phase 1 instrumentation per the writing-plans plan in `docs/superpowers/plans/`.

### 2026-04-24 — Phase 1: instrumentation + data

**Firmware:** commit 23b1def on `investigate/lvgl-pool-pressure` (pool_probe header + impl + wired to boot + `/heap/probe-csv`).

**Run:** 10 min of stress orchestrator (sleep was interrupted at 10 min, not the full 12; captured what we had).
- Cycles completed: 20
- Crashes observed: **3** (cadence unchanged from baseline)
  - uptime 150 s → 28 s
  - uptime 178 s → 28 s
  - uptime 188 s → 28 s

**Probe data:** 145 lines captured (samples since latest reboot, ~140 s worth).

**AllocFailures section:** empty.
- Interpretation: LVGL allocates from its internal TLSF pool via `lv_malloc`, NOT via `heap_caps_malloc`.  Our `heap_caps_register_failed_alloc_callback` can't see those failures.  That's consistent with all the crash MEPCs being inside LVGL's allocator chain rather than callers of `heap_caps_*`.

**Sample data — the smoking gun is in `lvgl_free_kb`:**

```
  14s (boot)    lvgl_free = 78 kB   pool fresh
 123s           lvgl_free = 51 kB   after ~1.5 min stress (5 cycles)
 125s           lvgl_free = 21 kB   -30 kB in 2 s (single screen transition)
 131s           lvgl_free =  2 kB   -19 kB more (another transition)
 131s → 152s    lvgl_free =  2 kB   *holds* at 2 kB for 21 s
 152s           CRASH
```

Other metrics during the slide to 2 kB free:
- `internal_free_kb`: flat at 40-41 kB
- `internal_largest_kb`: flat at 38 kB
- `psram_free_kb`: flat at 21,133 kB
- `lvgl_used_kb`: stuck at 14 kB throughout (suspect `lv_mem_monitor` call without UI lock returns partial view — see Q4 in Phase 2)

**Interpretation:** LVGL pool is the bottleneck, and it's decreasing **in sudden ~20-30 kB drops aligned with screen transitions**, not monotonically eroding.  It also does NOT recover — sits at 2 kB until the next crash-triggering allocation.

**Phase 1 exit criteria met.** Advancing to Phase 2.

### 2026-04-24 — Phase 2: hypothesis

From Phase 1 data, answering the plan's four concrete questions:

- **Q1** (internal_largest monotonic within cycle): **no**.  Internal-SRAM largest stays flat at 38 kB; it is NOT the bottleneck. Fragmentation watchdog was a red herring.
- **Q2** (lvgl_free drops to ~0): **yes**, and decisively.  78 kB → 2 kB across ~5 screen cycles, in discrete ~20-30 kB drops aligned with transitions.  Stays at 2 kB until crash.
- **Q3** (MALLOC_CAP_INTERNAL allocs fail before crash): **not detectable by our callback** — LVGL's internal TLSF allocator doesn't route through `heap_caps_*`.  Absence is consistent with the pool-exhaustion hypothesis, not evidence against it.
- **Q4** (PSRAM decreases): **no**.  PSRAM flat across the whole run.  So whatever is consuming LVGL's pool isn't backed by PSRAM.

**Hypothesis (a+c combined):** the **LVGL base pool (`CONFIG_LV_MEM_SIZE_KILOBYTES=96`) is getting exhausted by a ~20-30 kB-per-transition "leak"** — allocations made during `ui_*_create()` that don't get fully released when the old screen is torn down / hidden.  The configured `LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096` expand pool is either:
- not firing (LVGL's expand requires `LV_MEM_CUSTOM=0` + certain timing),
- or firing but into a region that our `lv_mem_monitor` reads don't see (making the total_size number we're logging unreliable).

Regardless of which, the **observable pool headroom goes to 2 kB** before every crash.  That's the fix surface.

**Candidate Phase 3 change:** there are three mutually exclusive options; plan discipline says pick ONE.

- **(A) Bump the base pool:** `CONFIG_LV_MEM_SIZE_KILOBYTES=96 → 128` (or larger).  Note: `feedback_crash_lvmem_regression.md` memory entry says 128 KB has historically caused a linker error on this target.  If that still holds, this is not a viable single-variable experiment.
- **(B) Verify + repair expand:** check the LVGL build is actually compiled with custom-expand support (`LV_MEM_ADR=0` + `LV_MEM_CUSTOM=0` + relevant alloc hooks).  If expand is silently disabled, re-enable.  One-line config change if that's the issue.
- **(C) Chase the transition leak:** instrument `ui_*_create/destroy/hide` pairs to log `lv_mem_monitor` deltas, identify which transition leaks, patch the leak at source.  Larger investigation but attacks the root cause rather than raising the ceiling.

**Chosen:** (A).  Rationale:
1. **Highest signal for lowest risk.**  If the pool bump doubles the crash cadence (from ~170 s to ~340 s), we've confirmed pool exhaustion is the root cause *without* needing to pinpoint which code path leaks.  If cadence doesn't double, it rules out the pool-size hypothesis and directs us to (C).
2. **The memory entry about 128 KB linker error is from April 14 and was about a prior firmware layout.**  We've removed ~2.3 MB of dead code today + reclaimed the 3 MB model SPIFFS partition; BSS pressure is likely much lower now.  A rebuild will tell us in <3 min.
3. **If 128 KB fails to link**, we have instant data: pool bump not available without BSS surgery — pivot to (B) or (C).

**Proposed change:** `sdkconfig.defaults`:
```diff
-CONFIG_LV_MEM_SIZE_KILOBYTES=96
+CONFIG_LV_MEM_SIZE_KILOBYTES=192
```

Going straight to 192 (vs intermediate 128) to give the experiment more signal — if 170 s → ~340 s, confirmed; if 170 s → ~680 s, the curve is linear in pool size; if 170 s stays 170 s, rules out pool size entirely.  The extra 96 kB comes from internal BSS.

**Expected effect in Phase 4 validation:**
- Crash cadence should extend proportional to the pool bump if hypothesis holds.
- If no crashes in 30 min → pool size was the limit AND we have enough headroom for normal use.
- If same cadence → return to Phase 2 with the new probe data to pick (C) directly.

---

## Not in scope (deliberately)

- **Reducing the stress orchestrator's rate** — useful as a separate validation experiment, but doesn't root-cause anything. The real product must survive sustained use.
- **Upgrading LVGL** (9.2.2 → 9.3.x / 10.x) — a large surface change that could introduce new problems. Consider only if Phases 1–4 fail and we've proven the root cause is upstream.
- **Rewriting our UI code to reduce LVGL churn** — would be a ~weeks refactor. Only if we find UI-side allocation pressure is the cause.
