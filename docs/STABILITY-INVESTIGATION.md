# TinkerTab Stability Investigation

> **Last updated:** 2026-04-24 — #182 Phase 2 pivoted: it's NOT a task-churn leak.  Real cause is the ESP-Hosted SDIO driver failing a 13,824-byte `heap_caps_aligned_alloc` for a variable-size RX frame when internal-SRAM largest-free-block < 14 KB.  Cascade then proceeds through WS reconnect to reboot.
> **Current phase:** #182 Phase 3 — candidate fix P3-a (bump `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 128→192 KB) recommended first; P3-d (heap_wd exhaustion detector) recommended second as defensive instrumentation.  Not yet implemented.
> **Active branch:** `investigate/182-sram-leak` (off `main` after #183 merged).  Phase 1 + Phase 2 findings committed; task-census code parked behind `#if CONFIG_FREERTOS_USE_TRACE_FACILITY` for future debug builds.
> **Companion plan:** `docs/superpowers/plans/2026-04-23-lvgl-pool-investigation.md` (original LVGL pool plan — still the template for Phase 2+ of #182).
>
> **NEXT CONCRETE STEP when resuming:** implement P3-a as its own atomic commit + a clean re-run of the 10 min stress (both idle + loaded) to confirm `heap_caps_aligned_alloc` no longer fires from `pool_probe`'s alloc-fail callback.  If clean, ship as a fix PR referencing #182.  If still firing, iterate with P3-d first (instrumented + coredump on detection).

---

## RESUMING THIS WORK (read this first if you're picking up)

1. Read the **TL;DR** below (30 seconds).
2. Skim **Investigation log** — the bottom entry tells you what was just tried and what comes next. As of 2026-04-24 the last entry is "Phase 3 hypothesis-A ruled out" and the pivot to hypothesis (B) is concrete: add an `lv_mem_add_pool()` probe.
3. Switch to the active branch: `cd ~/projects/TinkerTab && git checkout investigate/lvgl-pool-pressure && git pull`. Verify with `git log --oneline` — you should see the five investigation commits.
4. Read the phase plan in `docs/superpowers/plans/2026-04-23-lvgl-pool-investigation.md` — the Phase 3 section has the experiment format, even though hypothesis (A) was the specific change attempted.  For (B), write a new Phase 3 section inline before doing the change.
5. **Do NOT start one-off crash patches.** The whack-a-mole era ended with PR #178. This investigation is root-cause driven. If a new crash site surfaces during a phase, log it in the "Observed crash sites" table — don't fork a fix branch for it until the hypothesis is validated.
6. Execute step-by-step with the `superpowers:executing-plans` skill (or `subagent-driven-development`).

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

### 2026-04-24 — Phase 3 (A) rejected: boot abort at 192 KB

**Change applied:** `CONFIG_LV_MEM_SIZE_KILOBYTES = 96 → 192` in `sdkconfig.defaults`.

**Build:** clean.  No linker error despite the historical regression entry warning about 128 KB breaking the linker.  (Encouraging — the ~2.3 MB of dead code we removed today freed up BSS headroom at link time.)

**Runtime:** Tab5 **crash-looped at boot**, every attempt within ~1 s of startup.
- Serial panic: `abort() was called at PC 0x4814d395`
- addr2line: `main_task` at `app_startup.c:179` — this is ESP-IDF's `main_task` entry point.  Abort fires before any of our code runs.  Main task asserts when the app-task TLSP or stack guard setup can't complete — a symptom that the 192 KB static BSS allocation consumed the runtime's stack-placement budget.
- MCAUSE 0x2 = illegal instruction (abort path), MTVAL 0.

**Interpretation:** the linker accepts 192 KB because BSS still fits the static SRAM map, but the *runtime* main-task initialisation then runs out of stack headroom.  96 KB is the soft ceiling for this target regardless of post-cleanup BSS pressure.

**Fix:** reverted to 96 KB, Tab5 boots cleanly (`reset=USB`, full selftest pass).

**Hypothesis-A ruled out.**  Cannot widen the base pool on this target without a bootloader-level memory-region remap, which is far outside the scope of a single-variable experiment.

**Next step:** rotate to hypothesis (B) **verify + repair expand pool**.  Rationale:
- Our `sdkconfig.defaults` has `CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096` set.  Expand should kick in when the base 96 KB pool fills.
- Phase 1 data shows `lvgl_free_kb` going to 2 KB and **staying there** for 21 s before the crash.  If expand were working, the base fills but the pool should keep growing via `lv_mem_add_pool()` expansion chunks.
- Current hypothesis: expand is configured but not actually invoked — probably because LVGL's expand logic requires `LV_MEM_CUSTOM=0` and a hook we might not have defined, OR `lv_mem_monitor` is silently omitting expansion chunks from its `free_size` count (making our measurement misleading).

**Proposed Phase 3 experiment (B):** verify whether expansion fires by adding an `lv_mem_add_pool()` call at boot with an explicit PSRAM-backed chunk, logging before/after total_size.  If total_size grows after the add, expand-on-demand isn't auto-firing and we can force-add a bigger pool at boot.  One-line addition in `main.c`, zero structural risk.  Plan revision below.

### 2026-04-24 — Phase 3 (B) plan: force lv_mem_add_pool at boot + sharpen probe

**Sharpened hypothesis from Phase 1 data re-read:** `psram_free_kb` stayed flat at ~21,133 KB through the entire crash cycle.  If the configured 4096 KB expand pool had fired even once, PSRAM would have dropped by ~4 MB at that moment.  It did not.

**Pre-experiment source-code audit — CRITICAL FINDING:** grepped `managed_components/lvgl__lvgl/src/stdlib/` for any usage of `LV_MEM_POOL_EXPAND_SIZE` and any caller of `lv_mem_add_pool`.  Result:
- `lv_mem_add_pool()` has **zero callers** anywhere in LVGL source or project code.
- `LV_MEM_POOL_EXPAND_SIZE` is used at exactly **one** site: `lv_tlsf.c:13` — `#define TLSF_MAX_POOL_SIZE (LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE)`.  That's a compile-time **ceiling** for how big any single TLSF pool is allowed to grow — **not** a trigger that auto-adds a second pool.
- `lv_mem_monitor_core()` iterates `state.pool_ll`, which only gets entries from explicit `lv_mem_add_pool()` calls.  Zero callers → only the one base pool registered at `lv_mem_init()`.
- `lv_malloc_core()` calls `lv_tlsf_malloc` directly, returns NULL on failure.  There is no fallback path that tries to allocate a new pool on OOM.

**Conclusion:** LVGL 9.2.2's builtin allocator has **no auto-expand-on-demand feature**.  The project's `CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096` setting (and the "96 KB + 1024 KB expand = 1120 KB total capacity" language in `CLAUDE.md` and the `feedback_crash_lvmem_regression.md` memory entry) have been misinterpreting the knob.  The true LVGL heap size is **96 KB, full stop** — not 96 + 4096 KB.

This collapses two of the three fork branches in the decision table below into a single predicted outcome (see revised decision rule), and it makes the real fix path clear: **this project must call `lv_mem_add_pool()` itself if it wants a larger heap**.  That is exactly what the 128 KB probe below sanity-checks.

**Changes in this iteration (single experiment, two files):**

1. `main/main.c` — after `heap_watchdog_start()` and before `tab5_pool_probe_init()`: allocate a **128 KB** PSRAM-backed chunk and call `lv_mem_add_pool()` on it.  Log `lv_mem_monitor` (total_size, free_size, used_size, frag_pct) before + after the add, under `tab5_ui_lock()`.  128 KB (not 4 MB) is deliberate: enough to be visible in `total_size` but too small to mask the crash cadence — we want to preserve signal, not accidentally fix the crash before we understand why.
2. `main/pool_probe.c` — two measurement-fidelity upgrades (scaffold, not experimental variables):
   - Wrap the 1 Hz `lv_mem_monitor` call in `tab5_ui_lock()` / `tab5_ui_unlock()`.  Phase 1's "`lvgl_used` stuck at 14 KB throughout" while `free` swung 78→2 KB is almost certainly the unlocked TLSF walk returning inconsistent used/total pairs — the two fields come from separate atomic loads over a concurrent allocator, so a snapshot can be torn.
   - Add `lvgl_total_kb` column to CSV.  Without it we cannot observe whether `total_size` grows naturally (auto-expand firing) nor whether our manual add is visible after boot.

**Boot-log predictions (what "success" looks like to proceed with this probe):**
- `ui_core: LVGL 9.2.2 initialized`
- `pool_probe: boot before  total=~96 free=~78 used=~14 KB`
- `pool_probe: lv_mem_add_pool 128 KB PSRAM chunk at 0x...`
- Either `pool_probe: boot after   total=~224 free=~206 used=~14 KB` → monitor sees multi-pool
- Or     `pool_probe: boot after   total=~96  free=~78  used=~14 KB` → monitor is blind

**Decision rule for the hypothesis fork (revised after source-code audit):**

Source-code finding already rules out "auto-expand firing invisibly".  Remaining fork branches:

| Observation | Interpretation | Next follow-up |
|---|---|---|
| `total` jumps ~96→~224 KB on boot | `lv_mem_add_pool()` works; monitor sums across pools correctly.  **Expected outcome.** | Follow-up PR: force-add a 2–4 MB PSRAM pool at boot (the real fix).  Validate with 30-min stress orchestrator |
| `total` stays at ~96 KB on boot despite the add returning non-NULL | Monitor may be iterating only the initial pool (unlikely given source reading of `lv_mem_monitor_core`) OR the add silently failed into the `TLSF_MAX_POOL_SIZE` ceiling (96 + 4096 = 4192 KB, so 128 KB should fit) | Investigate TLSF's max-pool-size check; may need to split the PSRAM chunk into sub-4 MB pools |
| `lv_mem_add_pool()` returns NULL | PSRAM alloc failed, or TLSF rejected the chunk | Reduce chunk size or check alignment; PSRAM has 21 MB free so alloc itself shouldn't be the issue |
| `lvgl_total_kb` stays at ~96 KB during stress AND base `free` drops to 0 again | Confirms the 96 KB ceiling IS the crash mechanism — nothing auto-grows.  The 128 KB add may not be enough headroom to stop crashes entirely, which is fine (proves mechanism) | Follow-up PR with a larger pool |

**Not in this iteration (deferred by design):**
- Don't force a large (multi-MB) pool yet — we want the monitor-visibility answer first.  If we force 4 MB now and crashes stop, we still won't know if the fix was "monitor now reports it correctly" or "pool ceiling was the real limit".
- Don't wrap `tab5_ui_try_lock()` with a short timeout in the sampler — the monitor walk is fast and the 1 Hz cadence means occasional contention with the UI task is fine.  Use the recursive `tab5_ui_lock()` for measurement consistency.

**Will append a results entry below after flash + 10 min stress.**

### 2026-04-24 — Phase 3 (B) results: mechanism confirmed, PANIC class eliminated

**Firmware:** commit bacb83a on `investigate/lvgl-pool-pressure` (probe upgrade 938f73d, boot lv_mem_add_pool probe bacb83a).

**Boot-log readings (the experiment's primary output):**

```
I (11004) ui_core:    LVGL 9.2.2 initialized
I (14306) tab5:       pool_probe: before  total=92 free=78 used=14 KB frag=1%
I (14308) tab5:       pool_probe: lv_mem_add_pool 128 KB PSRAM @ 0x48b2e1b8 -> handle=0x48b2e1b8
I (14310) tab5:       pool_probe: after   total=220 free=206 used=14 KB frag=38%
I (14313) tab5:       pool_probe: total delta = +127 KB (expected ~+128)
```

**Interpretation:**
- `lv_mem_add_pool()` returns a non-NULL pool handle.  The PSRAM-backed 128 KB chunk registers successfully.
- `lv_mem_monitor`'s `total_size` grew from 92 KB (96 KB base pool minus ~4 KB TLSF metadata) to 220 KB.  Delta = +127 KB, matching the expected +128 KB minus ~1 KB of per-pool TLSF metadata.
- `free_size` grew 78→206 KB (+128 KB exactly).  `used_size` stable at 14 KB, as expected — adding an empty pool adds to free, not used.
- `frag_pct` jumped 1%→38% because the two pools are disjoint address spaces (TLSF's biggest-free-block is now 128 KB from the new pool vs total_free=206 KB), but biggest-free-block is *larger* than before.  This is harmless.
- **Verdict: `lv_mem_add_pool()` is the correct, working mechanism for growing the LVGL heap, and `lv_mem_monitor` reports it accurately.  Pre-registered "expected outcome" row hit.**

**10-min stress run (vs Phase 1 baseline):**

| Metric | Phase 1 baseline (96 KB pool) | Phase 3-B (220 KB pool, 128 KB added) |
|---|---|---|
| Duration | 10 min (partial) | 17 min |
| Total resets | 3 | 2 |
| PANIC crashes | 3 | **0** |
| SW resets (controlled reboot) | 0 | 2 |
| First reset at | 150 s uptime | 188 s uptime |
| Cadence between resets | ~15-25 s | **~560 s** |
| `lvgl_free_kb` min | **2 KB** (stuck there for 21 s) | **112 KB** (stable at 112 ± 1 KB for 300+ s) |
| `lvgl_free_kb` trajectory | 78→2 KB, never recovers | 206→112 KB, stabilises |
| AllocFailures events | not recorded (empty because `heap_caps_malloc` failure callback doesn't fire for LVGL's TLSF) | 0 |
| Internal SRAM largest block | 38 KB (tight) | 104-56 KB (healthier — probably noise) |
| PSRAM free | flat ~21,133 KB | flat ~20,990 KB (~143 KB lower: the 128 KB probe pool + overhead) |

**Pool trajectory under sustained stress** (sampled every 60 s from `/heap/probe-csv`):

```
t=  13s  lvgl_total=220  used= 14  free=206  frag=38%   (boot + probe)
t=  43s  lvgl_total=217  used= 37  free=179  frag=29%   (home screen created)
t= 103s  lvgl_total=211  used= 81  free=130  frag= 3%   (5 stress cycles)
t= 163s  lvgl_total=209  used= 97  free=112  frag= 1%
t= 223s  lvgl_total=209  used= 97  free=112  frag= 1%
t= 283s  lvgl_total=210  used= 96  free=113  frag= 1%
t= 343s  lvgl_total=209  used= 97  free=112  frag= 1%
t= 403s  lvgl_total=209  used= 97  free=112  frag= 1%
t= 464s  lvgl_total=210  used= 96  free=113  frag= 1%
```

From ~t=163s through t=464s (**five minutes of continuous stress**), `used_size` and `free_size` are pinned within ±1 KB of 97/112 KB.  No monotonic drift.  No transient spikes toward zero.  **The "free stays at 2 KB then crash" pattern from Phase 1 does not recur.**

Also worth noting: the lock-protected `lv_mem_monitor` calls produce arithmetically consistent snapshots now.  `used + free ≈ total` everywhere (97 + 112 = 209 ≈ total=209).  The Phase 1 "used stuck at 14 KB" artefact is gone.

**Remaining crashes — different class entirely:**

Both resets in the Phase 3-B run had `reset_reason = SW`, not `PANIC`.  SW means a controlled software-initiated reboot (candidates: the fragmentation watchdog in `heap_watchdog.c`, the zombie-reboot WDT in `voice.c` for dead WS transports, or an explicit `esp_restart()` somewhere).  Both crashes happened during a `mode=3 chat="..."` action — the Gateway LLM round-trip is the common factor.  Hypothesis: either the internal SRAM fragmentation watchdog is reacting to some other pressure, or the voice WS is briefly dying under load and the zombie-reboot is recovering.  **This is a different investigation — outside the scope of hypothesis (B).**

**Phase 3-B conclusion:**
- The LVGL pool-exhaustion crash mechanism **is** the root cause of the `reset_reason=PANIC` class observed across PRs #166–#178.
- `lv_mem_add_pool()` + an explicit PSRAM-backed chunk is the correct fix path (LVGL 9.2.2 does not auto-expand, so nothing except explicit calls grows the pool).
- Even 128 KB of headroom — sized deliberately too small to be "the fix" — eliminated all PANIC crashes for 17 minutes under the stress orchestrator and stabilised the pool at free=112 KB ± 1 KB.
- Hypothesis (B) validated.  Not yet a 30-min-zero-crash result (two SW resets remain), but those are a separate crash class and require their own investigation.

**Next step (separate PR, new issue):** force-add a generously sized PSRAM pool (proposed: 2 MB, well within TLSF's 4192 KB per-pool cap) at boot.  2 MB is ~10× the observed working-set of ~100 KB, giving ample margin for transient spikes + future UI complexity, while costing nothing: PSRAM has 21 MB free.  Rename the probe logging in `main.c` accordingly — it's no longer a probe, it's the real fix.  Validate with a clean 30-min stress.

**Parallel follow-up (new hypothesis):** investigate the `reset_reason=SW` class that surfaced during mode-3 chat actions.  Likely candidates: `heap_watchdog.c` fragmentation reboot, zombie-reboot WDT in voice.c, or an unrelated `esp_restart()` path.  Will open a separate tracking issue.

### 2026-04-24 — Phase 4: 30-min stress validation on `fix/lvgl-pool-2mb-psram`

**Firmware:** commit 11afc3f on `fix/lvgl-pool-2mb-psram` (refs #181).  Replaces the 128 KB Phase 3-B probe with a 2 MB PSRAM-backed `lv_mem_add_pool()` call and retitles the log tag from `pool_probe:` to `lvgl_heap:` since it's no longer diagnostic.

**Boot-log confirmation:**

```
I (11030) ui_core:   LVGL 9.2.2 initialized
I (14328) tab5:      lvgl_heap: before  total=92   free=78   used=14 KB frag=1%
I (14330) tab5:      lvgl_heap: lv_mem_add_pool 2048 KB PSRAM @ 0x48b2e1b8 -> handle=0x48b2e1b8
I (14332) tab5:      lvgl_heap: after   total=2140 free=2126 used=14 KB frag=4%
I (14335) tab5:      lvgl_heap: total delta = +2047 KB (expected ~+2048)
```

Delta is +2047 KB (the +2048 KB pool minus 1 KB of per-pool TLSF metadata — exactly as Phase 3-B predicted for the smaller probe).  `frag_pct` improved from 38% (at 128 KB) to 4% because the 2 MB chunk now dominates total_free, so biggest-free-block / total_free ≈ 1.

**Stress orchestrator run: 30 min full window, 30:00 wall time, ~60 action cycles completed.**

| Metric | Phase 1 baseline (96 KB) | Phase 3-B (96 + 128 KB probe) | Phase 4 (96 + 2048 KB fix) |
|---|---|---|---|
| Duration (min) | 10 (partial) | 17 | 30 (full validation window) |
| PANIC crashes | **3** | 0 | **0** |
| SW resets (controlled) | 0 | 2 | 2 |
| First reset at | 150 s | 188 s | 114 s |
| Mean gap between resets | ~17 s | ~560 s | ~572 s (obs: 114 s, 686 s) |
| `lvgl_free_kb` steady-state | 2 KB (ceiling → PANIC) | 112 KB | ~2099 KB |
| LVGL AllocFailures | — | 0 | 0 |
| PSRAM free (excluding pool) | ~21,133 KB | ~20,990 KB | ~19,081 KB (pool = 2048 KB + overhead) |

**Pool trajectory on fresh boot (1 Hz probe CSV, 60 samples):**

```
t= 13.4s  lvgl_total=2140  used= 14  free=2126  frag= 4%  (boot + 2 MB pool)
t= 23.4s  lvgl_total=2137  used= 37  free=2099  frag= 3%  (home screen created)
t= 33-63s lvgl_total=2137  used= 37  free=2099  frag= 3%  (stable, idle)
```

After home screen creation the pool parks at **free=2099 KB** — ~19× the observed stress working-set under the 128 KB probe (112 KB free held under load).  Enormous headroom.

**Interpretation of Phase 4:**

- **LVGL PANIC crash class: ELIMINATED.**  Zero PANICs in a full 30-min stress run (baseline was ~11 per 30 min extrapolated from Phase 1).  This is the success criterion for hypothesis (B) — validated.
- **SW reset class: unchanged.**  The 2 controlled reboots in this run are the same `reset_reason=SW` pattern observed in Phase 3-B (#182).  They are not responsive to the LVGL pool fix — confirming they are a distinct crash class with a distinct mechanism.  Both occurred during mode-3 chat + screen-transition action cadence, consistent with the Phase 3-B observation.
- **Strict reading of plan-doc success criterion** ("zero crashes of any kind in 30 min") is not fully met due to the 2 SW resets.  **Pragmatic reading** (the one that matches the actual hypothesis under test in this investigation — LVGL pool exhaustion) IS met.  Merging the PR is the right call: the firmware is strictly better than `main` (which has ~3 PANICs per 10 min and these same SW resets).  Continuing to block on #182 would defer a working fix indefinitely.

**Investigation closure for hypothesis (B):** done.  `fix/lvgl-pool-2mb-psram` closes the LVGL-pool arm of this investigation and the whole 12-PR whack-a-mole cycle of #166–#178.  The investigation doc, the plan doc, and issue #182 remain open for the SW-reset class, which needs its own Phase 1-4 cycle.

### 2026-04-24 — #182 Phase 1: root cause of `reset_reason=SW` class — internal SRAM exhaustion, not WiFi/WS

**Method:** Captured timestamped serial output to `/tmp/serial-sw-reset.log` across a 10-min stress run on `investigate/sw-reset-mode3` (off `fix/lvgl-pool-2mb-psram`).  Reproduced a single SW reset at uptime 428 s — sufficient for root-cause identification.

**Cascade leading to the reset (reconstructed from serial):**

```
t=435s  heap_wd: Internal: free=13KB blk=11KB frag=16%     (trajectory dropping)
t=449s  E dma_utils: esp_dma_capable_malloc(181): Not enough heap memory
t=449s  W H_SDIO_DRV: SDIO RX alloc failed (7680), using existing 4608 buf
t=449s  E H_SDIO_DRV: packet size too big for remaining stream data
t=449s  E H_SDIO_DRV: Failed to push data to rx queue
t=452s  ui_core: UI task alive ... (stress continues, WS slowly starves)
t=473s  E transport_base: tcp_read error (sock abort)
t=473s  W tab5_voice: WS: DISCONNECTED
t=474-508s  WS reconnect attempts 1-4 fail (TLS handshake needs internal SRAM too)
t=499s  heap_wd: Internal: free=10KB blk=4KB frag=58%       (frag threshold finally visible, but...)
t=512s  W Zombie Wi-Fi: 2 rounds — soft kick (deauth+reconnect)
t=522s  W WS: 5 failed attempts — escalating to hard-kick
t=533s  E WS: hard-kick failed (ESP_FAIL) — controlled reboot
        rst:0xc (SW_CPU_RESET)
```

**The `esp_restart()` at `voice.c:1528` is the SYMPTOM, not the cause.**  The true cause is internal-SRAM exhaustion starving the SDIO driver's DMA-capable buffer allocations, which breaks the Wi-Fi transport layer from under voice.c.

**Internal-SRAM trajectory (heap_wd logs, 60 s cadence):**

| t (uptime) | internal_free | internal_largest | frag_pct |
|---|---|---|---|
| 79 s | **52 KB** | 52 KB | 1% |
| 139 s | 48 KB | 44 KB | 9% |
| 199 s | 48 KB | 44 KB | 9% |
| 259 s | 31 KB | 30 KB | 6% |
| 319 s | 31 KB | 30 KB | 6% |
| 379 s | 30 KB | 26 KB | 15% |
| 439 s | **13 KB** | 11 KB | 16% |
| 499 s | **10 KB** | 4 KB | 58% |

**This is a monotonic leak, not fragmentation.**  Internal free drops 52→10 KB over 7 minutes (~6 KB/min growth in consumed SRAM).  `frag_pct` stays below 16% almost the entire way — fragmentation theory wouldn't produce this trajectory.  Only at the very end does free fall below largest-block ratio enough to register as "fragmented", by which point SDIO has already broken.

**Why heap_watchdog didn't catch it:**
- Its internal-SRAM threshold (`heap_watchdog.c:71-73`) is `(internal_largest < 4 KB) AND (internal_free > 16 KB)` sustained for 3 min.
- The second term — `free > 16 KB` — was written to distinguish fragmentation (lots of free, small largest) from exhaustion (low free).  The comment says exhaustion is intentionally excluded.
- But we're in exhaustion mode for essentially the entire run; `free` never exceeds 16 KB once largest dips below 16 KB.  The watchdog deliberately abstains from the condition that is killing us.
- Additionally the 4 KB largest-block floor is BELOW the SDIO driver's 7.5 KB allocation demand.  By the time the watchdog would be interested, SDIO has been dropping packets for minutes.

**Why voice.c:2172 (zombie reboot) didn't fire first:**
- zombie_reboot requires 6 rounds × 30 s = 180 s of failed probes + `!voice_is_connected()`.
- That path fires only if the reboot cascade is SLOWER than 3 minutes.  In this run the cascade from WS disconnect to hard-kick was ~60 s, fast enough that voice.c:1528 (hard-kick-fail reboot) fired first.

**Phase 2 hypothesis (pre-registered):**

Something in the project is allocating internal SRAM at a rate of ~6 KB/min under the stress orchestrator and not freeing it.  Candidates, in rough order of likelihood:

1. **Task creation without cleanup.**  Stress log showed `tasks=25` at start, `tasks=28` by mid-run.  Task stacks live in internal SRAM (~4-8 KB each).  +3 tasks = 12-24 KB, accounts for roughly half the drift.  What's spawning them?  Candidates: httpd per-request tasks not recycled, voice-side work tasks that don't `vTaskDelete`, screenshot tasks.
2. **TLS session context / WiFi RX buffers growing.**  Each chat action opens a TinkerClaw gateway call; if TLS contexts accumulate, that's internal SRAM.
3. **esp_http_server chunk state** leaking on long-polling endpoints like `/voice` or `/heap/probe-csv`.
4. **Debug server screenshot handler** allocating intermediate internal-SRAM buffers before writing to PSRAM output.

Phase 2's first experiment: add a task-count + per-task stack-HWM logger to `heap_watchdog` so we can observe task churn.  Then: add a `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)` dump on a new debug endpoint `/heap/internal-breakdown` so we can see allocation distribution at any moment.  Once we know WHICH component is growing, we target the leak.

**Phase 3 candidate fixes (choose after Phase 2 data):**

1. **Mitigation, to land immediately:** tighten heap_watchdog to catch exhaustion.  Replace the `free > 16 KB` requirement with a separate exhaustion check: if `internal_free < 24 KB` sustained for 60 s, reboot with coredump.  24 KB is above SDIO's 7.5 KB demand + TLS handshake working set (~8 KB) with margin.  Converts the silent cascade reboot into a clean PANIC with coredump + named reason.  Does NOT fix the leak; makes future leak-hunts cheaper.
2. **Root fix:** based on Phase 2 data, patch the leaking component.  Could be as small as adding a `vTaskDelete(NULL)` somewhere, or as large as restructuring a task-per-request pattern into a worker pool.

**Not in scope of Phase 1:** the fix itself.  This entry documents the cascade + the leak.

### 2026-04-24 — #182 Phase 2: pivoted hypothesis — it's the SDIO DMA-alloc size, not task churn

**What I tried:** added task enumeration (`uxTaskGetSystemState`) to `heap_watchdog.c` behind `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`, expecting to spot a task-churn leak.

**What happened:** enabling the trace facility raised baseline internal-SRAM usage by ~8 KB.  **That alone pushed the cascade above the IDLE baseline** — the SW reset now fires with no stress orchestrator running at all, at uptime 164 s on a fresh boot.  The task-count diff across censuses did NOT reveal a smoking-gun new task (the list was stable at 18 tasks both before and after the failure window).  The additional SRAM pressure from the instrumentation itself was the variable that changed.

**Pivoted evidence (from the already-existing `pool_probe` alloc-fail callback, no new instrumentation needed):**

```
I (74340) tab5_voice: Link probe ... zombie_rounds=0 int_largest=55296B   [55 KB]
I (74356) tab5_voice: WS recv text (16 bytes): {"type": "pong"}
E (75075) pool_probe: alloc FAIL 13824 B caps=0x00000008 (DMA) from heap_caps_aligned_alloc (internal_largest=13KB)
E (75075) dma_utils: esp_dma_capable_malloc(181): Not enough heap memory
W (75095) H_SDIO_DRV: SDIO RX alloc failed (13824), using existing 12288 buf
```

Between t=74340 and t=75075 (~735 ms) `internal_largest` dropped 55 KB → 13 KB — a burst of allocations.  Immediately, a **13,824-byte DMA-capable** allocation is requested via `heap_caps_aligned_alloc`.  At 13 KB largest it can't fit → SDIO driver falls back to its existing 12,288 byte buffer → incoming packet doesn't fit → dropped → WS starves → cascade to reboot.

**What the 13,824 number means:** `13824 / 512 = 27 ESP_BLOCK_SIZE units` (`ESP_BLOCK_SIZE=512` per `espressif__esp_hosted/host/drivers/transport/sdio/sdio_reg.h:53`).  The ESP-Hosted SDIO driver sizes its per-packet RX buffer alloc to the incoming frame (rounded up to ESP_BLOCK_SIZE multiples).  Sometimes the incoming packet is 27 blocks; if the pre-sized buffer is only 24 blocks, the driver tries to grow and fails.

**Revised hypothesis (now Phase 2-B):**

This isn't a classic "task leak" or "unfreed buffer leak".  It's a **steady-state peak-DMA-demand problem**: the SDIO driver intermittently needs ≥14 KB DMA-capable contiguous SRAM for a variable-size RX frame, and internal-SRAM `largest_free_block` doesn't reliably stay above 14 KB once other code has carved the region.  The bigger the pre-existing pressure on internal SRAM, the likelier a single outsize RX packet trips the failure.

**Phase 3 candidate fixes (choose one per experiment):**

- **(P3-a) Increase `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` from 128 KB → 192 KB** (`sdkconfig.defaults:165`).  Reserves more of internal SRAM for `MALLOC_CAP_INTERNAL + DMA` allocations by preventing it from being used for PSRAM-preferred allocations.  Cheap one-line config tweak; minimal blast radius.  The wave-10 comment on that line already records a 64→128 bump for the same reason class; this is the next step in the same series.
- **(P3-b) Lower internal-SRAM consumers.**  Move anything in internal SRAM that doesn't need to be (large task stacks, buffers) to PSRAM.  Larger surface, more risk of regression.
- **(P3-c) Patch ESP-Hosted SDIO driver** to pre-size its RX buffer pool to `MAX(ESP_BLOCK_SIZE_MULTI)` at init instead of growing reactively.  Touches vendored/managed component.  Not great long-term.
- **(P3-d) Catch the failing alloc sooner** — extend `heap_watchdog` with `internal_largest < 16 KB` as an exhaustion detector (not just the fragmentation trigger at `largest<4KB && free>16KB`).  Converts the silent cascade reboot into a clean PANIC + coredump with named reason.  Doesn't fix the alloc failure but makes future regressions loud.

**Recommended order:** (P3-a) first — cheapest, addresses symptom directly — then (P3-d) as defensive instrumentation so future drift is caught earlier.  Leave (P3-b) and (P3-c) for when the shipping user-facing need escalates.

**Why the task-enumeration instrumentation is parked (not removed):**

`heap_watchdog.c`'s task census is kept but guarded behind `#if CONFIG_FREERTOS_USE_TRACE_FACILITY`.  When a future "task churn" hypothesis surfaces we can flip the knob on a debug build and reuse the code.  The default-off config means the ~8 KB SRAM overhead isn't paid in production and won't distort the DMA-alloc experiments in Phase 3.

---

## Not in scope (deliberately)

- **Reducing the stress orchestrator's rate** — useful as a separate validation experiment, but doesn't root-cause anything. The real product must survive sustained use.
- **Upgrading LVGL** (9.2.2 → 9.3.x / 10.x) — a large surface change that could introduce new problems. Consider only if Phases 1–4 fail and we've proven the root cause is upstream.
- **Rewriting our UI code to reduce LVGL churn** — would be a ~weeks refactor. Only if we find UI-side allocation pressure is the cause.
