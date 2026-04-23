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

---

## Not in scope (deliberately)

- **Reducing the stress orchestrator's rate** — useful as a separate validation experiment, but doesn't root-cause anything. The real product must survive sustained use.
- **Upgrading LVGL** (9.2.2 → 9.3.x / 10.x) — a large surface change that could introduce new problems. Consider only if Phases 1–4 fail and we've proven the root cause is upstream.
- **Rewriting our UI code to reduce LVGL churn** — would be a ~weeks refactor. Only if we find UI-side allocation pressure is the cause.
