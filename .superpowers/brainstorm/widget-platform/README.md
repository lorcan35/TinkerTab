# Widget Platform v1 · Design Mockups

**Status:** Design lock, pre-implementation.  
**Audience:** Emile (owner), Claude (implementer), future skill authors.

This directory is the **visual + design spec** for the Widget Platform, the
new extensibility layer for TinkerTab (and every other device that
eventually ships the renderer).

The idea in one sentence:

> **Skills emit typed state; devices render it through six widgets. Write a
> Python file, pick a widget, done. No firmware flash. Portable across
> hardware.**

## What's in here

| file | what it shows |
|---|---|
| `00-hero.html` | The concept: three-layer architecture, six-widget vocabulary, author experience, principles, explicit non-goals. The "pitch deck" of the platform. |
| `01-time-sense-flow.html` | The reference skill (Time Sense, AI-first Pomodoro) rendered across five device states — idle, active, approaching, urgent, done — plus the multi-timer patterns (counter pill, stack sheet, auto-promote) and the full WebSocket message lifecycle. |
| `02-icon-library.html` | The initial 16-icon library drawn as SVG primitives, the authoring system (5 lines of Python per new icon), and the design rules for future additions. |

## How to read it

Start with **`00-hero.html`** — it frames everything. Then **`01-time-sense-flow.html`**
shows what a real skill actually looks like on the device in five different
states; that's the concrete artifact. **`02-icon-library.html`** documents the
icon system and how to extend it.

Every frame in `01-time-sense-flow.html` renders at actual Tab5 resolution
(720×1280) scaled to fit. Fonts on-device = Montserrat Bold + JetBrains Mono
(per v5 design doctrine). The HTML uses Fraunces as the display font for
presentation only — on device that same weight is carried by Montserrat Bold
at 48 / 56 px.

## The six widgets

| widget | when to use | primary slots |
|---|---|---|
| `live` | one thing happening right now — timer, drafting, heartbeat | title, body, icon, tone, progress, action, priority |
| `card` | moment-in-conversation notification or completion | title, body, image?, tone, action? |
| `list` | pick-one-of-N menu | title, items[], on_select |
| `chart` | a data shape — spark / bar / gauge | title, series, kind, unit, range |
| `media` | image + caption (reuses existing rich-media pipeline) | url, alt, caption, action? |
| `prompt` | one question with a typed input — number, text, choice | question, input, on_answer |

See `../../../docs/WIDGETS.md` for the protocol spec (created alongside the
first implementation).

## What's deliberately NOT in scope for v1

- **No YAML-declared layouts.** Widget vocabulary is the layout contract.
- **No runtime interpreter on Tab5.** No JS, no Lua. Skills run on the brain.
- **No app install flow.** Skills sync from Dragon's registry; "install" is `git pull`.
- **No per-skill chrome overrides.** v5 theme tokens are centrally enforced.
- **No multi-user / per-widget user_id.** Single-user v1; multi-user is a later pass.

## Design rules (from `00-hero.html`)

1. **One live, full stop.** Exactly one widget owns the orb at a time.
2. **Amber is the only accent.** Other colors are reserved for status semantics.
3. **No layout from skills.** Manifests declare surfaces; never coordinates.
4. **Renderer degrades gracefully.** Chart on OLED becomes "avg 42 ↑".
5. **Typography carries meaning.** Letter-spaced caption = state. Serif = headline.
6. **Actions are always single.** Widgets expose at most one primary action.
7. **Idle has no widget.** Calm orb by default.
8. **Skills never block.** Async everything.

## Where this lives next

- Protocol spec: `docs/WIDGETS.md` (TinkerTab)
- Protocol addition: `docs/protocol.md` §17 (TinkerBox)
- Skill authoring: `docs/tools/skills.md` widget surface section (OpenClaw)
- Tab5 implementation: `main/ui_widget.c` + `main/widget_store.c` + `main/ui_home.c`
  (live-slot integration) + `main/ui_skills.c` (stack sheet) + `main/chat_msg_view.c`
  (card action extension)
- Dragon implementation: `dragon_voice/surfaces/base.py` + `surfaces/manager.py`
- Reference skill: `dragon_voice/tools/timesense_tool.py`
