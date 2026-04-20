# TinkerTab — UI Overhaul v4 (home screen)

**Target:** 720×1280 voice-first ESP32-P4 tablet, LVGL v9.
**Brand:** amber `#F59E0B` on near-black `#08080E`. Card `#111119`, elevated `#13131F`.
**Constraint:** all visuals must translate to LVGL primitives — no SVG filters, no blur, no gradients beyond 2-stop.

Three rounds of mockups live here. Latest round (**Round 3**) is what to show.

---

## v3 critique being solved

1. **Triple-redundant "what's recent"** — agent-summary card + 2 info cards + greeting all competed.
2. **Orb undersized** (~66–88px) for a voice-first product.
3. **Timer quick-action** was filler.
4. **Too much chrome** — input pill + nav stacked = 128px.
5. **Mode pill too tiny** for a behavior-changing setting.

---

## Round 3 (latest — SHOW THESE)

| File                       | Name                | Core bet                                                                                           |
|----------------------------|---------------------|----------------------------------------------------------------------------------------------------|
| `a-orb-sovereign.html`     | Orb Sovereign       | Orb dominates (160px). One glance card replaces three. One 88px tap pill replaces input+nav.       |
| `b-editorial-horizon.html` | Editorial Horizon   | 52px serif sentence replaces all 3 context cards. Orb offset as magazine hero. Unified 144px dock. |
| `c-ambient-canvas.html`    | Ambient Canvas      | Orb + ambient halo. The single "Now" card **IS the Widget Platform live slot**.                    |

Each file carries a sidecar critique in the left margin explaining bet / fixes / LVGL translation.

### Side-by-side fixes

| Problem         | A (Sovereign)                          | B (Horizon)                                   | C (Canvas)                                     |
|-----------------|----------------------------------------|-----------------------------------------------|------------------------------------------------|
| Redundant info  | One glance card, 3 rows                | One serif sentence                            | One live-slot card (widget_t-shaped)           |
| Orb size        | 160px + 2 rings + 2 halos              | 150px offset right, label stacked             | 156px + ambient backdrop + 3 rings             |
| Timer filler    | "Yesterday's dictation" row            | "Pick up where you left off" continuity card  | Replaced by Now-Thread live slot               |
| Chrome height   | 88px tap pill                          | 144px dock, merges input+nav+kb chips         | 176px strip, but singular and composed         |
| Mode control    | Full-width 4-segment control (top)     | Quiet chip in topbar + horizon divider        | 14px readable chip under state word            |

---

## Recommendation: **C · Ambient Canvas**

### Why

- **Architectural fit.** The Widget Platform already defines a priority-resolved live slot on the home screen (see `main/widget_store.c` + `main/ui_home.c`). Iteration C's "Now" card IS that slot. When Time Sense pushes `widget_live`, no layout breaks — `kicker` ← `skill_id`, `lede` ← `body`, `stats` ← `progress/meta`. Home screen and widget slot become one.
- **Orb–state–mode stack reads as a single expressive unit.** The orb tells you state via tone; the word below names it; the mode chip reads as environment. This mirrors how LVGL already updates them (three independent objects reacting to different signals).
- **Ambient halo is a single cached ARGB8888 buffer.** 2-stop radial, drawn once at boot, blit to the background. LV_MEM pool isn't hammered by per-frame gradient fills — critical given the regression history (memory: `feedback_crash_lvmem_regression.md`, and the 96KB + 1024KB expand config in `sdkconfig.defaults`).
- **Fat tap zone (84px inside 108px pill).** Exceeds 44pt Apple HIG / 48dp Material floor with room for an across-the-room tap.

### Trade-offs

- Chrome is tallest (176px) — but it's one composed unit, not a stacked input+nav, and it does more (say, VU wave, rail).
- Mode chip smaller than A's segmented control. Mitigation: long-press → opens full segmented picker as a sheet (iOS control-center idiom).

### Fallbacks

- Prefer **A** if Emile wants maximum familiarity with v3 structure — rebalanced, not reimagined.
- Prefer **B** if Emile wants the strongest editorial differentiation — nothing looks like this in the AI-tablet space.

---

## LVGL translation cheatsheet (Iteration C)

```
┌─ ambient halo ────────────────────────────────────────────┐
│  lv_canvas (ARGB8888, 720×760)                            │
│  2-stop radial rendered once via lv_draw_arc with          │
│  bg_grad_dir=LV_GRAD_DIR_RADIAL at boot                    │
└───────────────────────────────────────────────────────────┘
┌─ orb stage ───────────────────────────────────────────────┐
│  3x lv_obj circle rings (radius, border_width 1)           │
│  lv_obj orb: bg_grad 2-stop amber→amber_hot, radial        │
│  shadow_width 52, shadow_color amber, shadow_opa 81        │
│  highlight: inner lv_obj inset 10, bg_grad white→transp    │
└───────────────────────────────────────────────────────────┘
┌─ state word ──────────────────────────────────────────────┐
│  lv_label "listening" — Fraunces-italic-36 (subset font)   │
│  color = orb_paint_for_tone(current_tone)                  │
│  (calm→emerald, active→amber, urgent→rose, done→emerald)   │
└───────────────────────────────────────────────────────────┘
┌─ mode chip ───────────────────────────────────────────────┐
│  lv_obj rounded rect (radius 100), border 1, pad 14/24     │
│  3 children: dot (8px circle), label "Hybrid", mono label  │
│  long-press → opens lv_menu sheet with 4 voice_modes       │
└───────────────────────────────────────────────────────────┘
┌─ Now slot (live widget target) ───────────────────────────┐
│  lv_obj card, radius 24, border 1                          │
│  amber accent bar: lv_obj 140×3 top-left                   │
│  kicker: lv_label (Manrope-600-11, tracking 0.18em)        │
│  lede: lv_label (Fraunces-300-30, line-height 1.1)         │
│  stats: lv_obj flex row, 3 cells with label+value          │
│  binding: on widget_live event, map widget_t fields        │
└───────────────────────────────────────────────────────────┘
┌─ strip ───────────────────────────────────────────────────┐
│  say: lv_btn 108px rounded, 84px amber inner circle        │
│  wave: 5 lv_bar vertical, heights updated from VU meter    │
│  rail: lv_obj flex row, 4 lv_btn 16px radius each          │
└───────────────────────────────────────────────────────────┘
```

---

## Archive — earlier rounds (for reference, not for showing)

**Round 1** — exploratory typography sketches:
- `01-instrument.html`, `02-almanac.html`, `03-console.html` (+ PNGs)

**Round 2** — aggressive structural cuts:
- `A_monolith.html` — 440px orb, one invite line, four corner glyphs (max voice-first)
- `B_editorial.html` — 240px orb + typographic feed, zero nav bar (prior pick)
- `C_console.html` — full-width 4-segment mode bar + 360px orb + Notes/Focus tiles

Round 3 supersedes both rounds because it composes the widget-platform live slot directly into the home screen — a synthesis neither earlier round achieved.

---

## Next step

Show `a-orb-sovereign.html`, `b-editorial-horizon.html`, `c-ambient-canvas.html` to Emile. Once he picks (default: **C**), port to `main/ui_home.c` on branch `feat/ui-overhaul-v4`, build, flash via `/dev/ttyACM0`, and verify with a `/screenshot` via the debug endpoint.

Ref: TinkerTab issue #54.
