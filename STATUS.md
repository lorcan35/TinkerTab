# TinkerTab Session State — ARCHIVED

> **This document is no longer the source of truth.**  Last live update was 2026-03-27, 4 weeks before
> the rich-media chat, widget platform, voice-mode rework, and stability investigation work landed.
> Anything below this line describes a March-27 snapshot and **does not reflect current main**.

## Where to read instead

| What you want | Where it lives now |
|---|---|
| Big-picture project status, build/flash, debug server | [`README.md`](README.md) |
| Active investigations, repo layout, NVS schema, voice modes, OTA flow, key fixes | [`CLAUDE.md`](CLAUDE.md) — also loaded on every Claude session |
| LVGL pool / SW-reset stability work | [`docs/STABILITY-INVESTIGATION.md`](docs/STABILITY-INVESTIGATION.md) |
| Voice pipeline architecture | [`docs/VOICE_PIPELINE.md`](docs/VOICE_PIPELINE.md) |
| Widget platform spec + plan | [`docs/WIDGETS.md`](docs/WIDGETS.md), [`docs/PLAN-widget-platform.md`](docs/PLAN-widget-platform.md) |
| Institutional knowledge (every gotcha learned during development) | [`LEARNINGS.md`](LEARNINGS.md) |

If you're picking this project up cold, start with `CLAUDE.md` → `README.md` → `LEARNINGS.md`.

---

## Snapshot (2026-03-27, kept for history)

The original content of this file documented the very first end-to-end voice pipeline coming up
in late March: Voice UI overlay (`ui_voice.c`), Phase 1 "Box → Product" Dragon services, and an
early stability fix push.  None of that work is gone — it's still in `git log`, and the surfaces
it built (voice overlay, Dragon link, NVS settings) are all live.  But every concrete number,
endpoint URL, partition size, and "Coming Up Next" entry in the original snapshot has since drifted.

If you really want the original text it's in git history at any commit before 2026-04-24
(`git log --follow STATUS.md`).
