# Documentation program roadmap

This file is copied byte-identical into every repo. It lays out the
world-class-documentation program so contributors can see what is shipped and
what is coming. The full design lives in the spec:
[`docs/superpowers/specs/2026-05-29-world-class-docs-program-design.md`](superpowers/specs/2026-05-29-world-class-docs-program-design.md).

The program adopts [Diátaxis](https://diataxis.fr) and delivers polished,
in-repo Markdown for four audiences — **tinkerers, developers, integrators,
operators** — across all four repos. There is no hosted site; the docs ship
with the code. After Wave 0 lands the structure + standards + templates, the
content waves fan out hard (one author per page, given the template + source
material).

## Waves

| Wave | Scope | Status |
|---|---|---|
| **Wave 0 — Foundation** | Diátaxis structure, `STYLE.md`, the four templates, unified `GLOSSARY.md`, `docs/ROADMAP.md`, audience-router READMEs + the "Part of the Tinker stack" peer block, relocation of internal working docs to `docs/internal/`, and one validated seed page per Diátaxis type in TinkerTab. | **In progress** |
| **Wave 1 — TinkerTab content** | The four-audience Tab5 docs: getting-started, hardware build/mods, build/flash, debug-server reference, voice/UI usage, troubleshooting, architecture. | Planned |
| **Wave 2 — TinkerBox content** | The Dragon docs: getting-started, architecture/explanation, the REST API + WebSocket protocol reference, backends/models, deploy/operate, the K144/TinkerON chain. | Planned |
| **Wave 3 — PingOS content** | The portable-OS docs (deepened once the active PingOS effort settles). | Planned |
| **Wave 4 — TinkerClaw content** | Curate the ~719 inherited docs down to what matters. Deferred until the parallel TinkerClaw↔OpenClaw merge/rebrand stabilizes. | Planned (deferred) |
| **Wave 5 — Cross-cutting polish** | End-to-end tutorials spanning repos ("unboxing → working voice assistant"), full link-check, and a consistency pass against `STYLE.md`. | Planned |

## How to contribute to a content wave

1. Read [`STYLE.md`](../STYLE.md) — the writing standard.
2. Start from the matching template in [`docs/_templates/`](_templates/).
3. Put the page in the right Diátaxis directory (`tutorials/`, `how-to/`,
   `reference/`, `explanation/`).
4. Use [`GLOSSARY.md`](../GLOSSARY.md) terms exactly; link first use.
5. Set `last-verified` to the date you checked the page's claims against the
   code/hardware.
6. Add the page to [`docs/README.md`](README.md)'s map.
