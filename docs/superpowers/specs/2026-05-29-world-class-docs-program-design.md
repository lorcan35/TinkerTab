# World-Class Documentation Program — Design Spec

- **Date:** 2026-05-29
- **Status:** Draft for review
- **Scope:** All four repos — TinkerTab, TinkerBox, PingOS, TinkerClaw
- **Deliverable:** Polished, in-repo Markdown (READMEs + `docs/`) — **no hosted site**
- **This spec covers:** the whole program structure **+ Wave 0 (Foundation) in full detail.** Waves 1–5 get their own specs.

---

## 1. Goal & context

Make the project's documentation **world-class** for four audiences — **Tinkerers, Developers, Integrators, Operators** — across all four repos, delivered as GitHub-rendered Markdown that ships with the code (no site to host or rot).

Today's reality (audited 2026-05-29):
- Good but **per-repo, README-bound, developer-centric** docs.
- `docs/` is dominated by **internal working docs** (PLAN-*/AUDIT-*/RETRO-*), not audience docs.
- The one site attempt (`TinkerTab/docs-site/`, PR #329) is an **empty Docusaurus shell** — to be removed.

**Owner decisions (2026-05-29):**
- Scope = **all four repos.**
- Home = **polished READMEs + `docs/`** (no site).
- Audiences = **all four** (tinkerer, developer, integrator, operator).
- Ambition = **full program** (decomposed into waves).
- Cross-repo = **each repo stands alone** — no single umbrella front door; repos cross-link as **peers**.

## 2. The documentation system — Diátaxis

Adopt **[Diátaxis](https://diataxis.fr)** (the framework behind Django, Gatsby, Cloudflare). Four content types, each with one job, **never mixed**:

| Type | Job | Reader is… | Primary audience lens |
|---|---|---|---|
| **Tutorial** | Learning by doing | a beginner being taught | Tinkerer: "unbox → working assistant" |
| **How-to guide** | Achieve a specific task | a user with a goal | All: build, flash, deploy, integrate, recover |
| **Reference** | Look up facts | someone who needs precision | Integrator: REST API, WS protocol, config, tools, code maps |
| **Explanation** | Understand why | someone building a mental model | Developer: architecture, data flow, design decisions |

**Audience → where they live:**

| Audience | Entry How-to | Heavy in |
|---|---|---|
| Tinkerer / end-user | "Get started" tutorial | Tutorials, How-to, troubleshooting |
| Developer / contributor | "Set up your dev env" | Explanation (architecture), How-to (build/contribute), Reference (code maps) |
| API / protocol integrator | "Make your first API call" | Reference (REST/WS/tools), How-to (integrate) |
| Operator | "Deploy on a Dragon" | How-to (deploy/recover), Reference (services/config), Explanation (the chain) |

## 3. Per-repo information architecture

Every repo (TinkerTab, TinkerBox, PingOS, TinkerClaw) gets the **same shape** so a reader who learns one repo can navigate any of them:

```
README.md                ← front door + audience router + "Part of the Tinker stack" peer block
GLOSSARY.md              ← canonical shared terms (+ repo-specific additions)
STYLE.md                 ← the docs writing standard (copy in each repo — they stand alone)
docs/
  README.md              ← docs index / map (links every page, grouped by Diátaxis type + audience)
  tutorials/             ← learning-oriented
  how-to/                ← task-oriented
  reference/             ← information-oriented
  explanation/           ← understanding-oriented
  _templates/            ← the 4 Diátaxis templates (tutorial/how-to/reference/explanation)
  internal/              ← existing PLAN-*/AUDIT-*/RETRO-* working docs (moved here, preserved, not audience-facing)
```

**README as audience router.** Each README opens with a one-paragraph "what is this", then an explicit router:

> **I want to…** → [**use it**](docs/tutorials/getting-started.md) · [**build/modify it**](docs/how-to/dev-setup.md) · [**integrate with it**](docs/reference/) · [**run it in production**](docs/how-to/deploy.md)

Deep README prose (Features/Hardware/etc.) stays for now; Wave 0 adds the routing + structure, content waves fill the linked pages.

## 4. The "Part of the Tinker stack" peer block (cross-repo glue)

Because each repo stands alone, the only cross-repo coupling is a **standard peer block** near the top of each README (and in each `docs/README.md`):

```markdown
> **Part of the Tinker stack** — four repos, each documented on its own:
> [**TinkerTab**](https://github.com/lorcan35/TinkerTab) (Tab5 device firmware) ·
> [**TinkerBox**](https://github.com/lorcan35/TinkerBox) (Dragon inference server — "the brain") ·
> [**PingOS**](https://github.com/lorcan35/PingOS) (portable Tab5 OS) ·
> [**TinkerClaw**](https://github.com/lorcan35/TinkerClaw) (agent sidecar).
> New here? Each repo's README is its own front door.
```

A short **`docs/explanation/how-the-stack-fits-together.md`** (with an ASCII system diagram) lives in **TinkerTab and TinkerBox** (the two halves of the live product) and is linked from the peer block — so "how do these relate?" is answered without designating a single owner.

## 5. Standard doc header

Every authored page opens with:

```markdown
---
audience: tinkerer | developer | integrator | operator
type: tutorial | how-to | reference | explanation
prerequisites: <links or "none">
last-verified: YYYY-MM-DD
est-time: <e.g. 15 min>   # tutorials/how-to only
---
```

`last-verified` is the freshness contract — every page states when its claims were last checked against the code/hardware.

## 6. STYLE.md (the standard, copied into each repo)

Outline:
1. **Voice & tone** — direct, second-person, active, no hype. Confident, not defensive.
2. **The Diátaxis rule** — name the type; don't mix (a tutorial never explains architecture; reference never teaches).
3. **The standard header** (§5) is mandatory.
4. **Naming** — `kebab-case.md`; verbs for how-to (`flash-firmware.md`), nouns for reference (`websocket-protocol.md`).
5. **Code blocks** — always language-tagged; commands copy-pasteable; show expected output where it aids verification.
6. **`last-verified` discipline** — touch the date when you re-verify; stale dates are a signal, not a lie.
7. **Cross-repo links** — use the peer block; link to a repo's README, not deep into its internals.
8. **Terminology** — use the GLOSSARY terms exactly; link first use.
9. **Accessibility** — alt text on images, descriptive link text (never "click here"), heading hierarchy.

## 7. Templates (`docs/_templates/`)

Four Markdown templates, each with the standard header + a Diátaxis-correct skeleton:
- `tutorial.md` — goal, prerequisites, numbered steps each with expected result, "what you learned", next steps.
- `how-to.md` — the task, prerequisites, steps, verification, troubleshooting.
- `reference.md` — scope, structured tables/fields, examples, no narrative teaching.
- `explanation.md` — the question, context, the model, trade-offs/alternatives, links to related how-to/reference.

## 8. GLOSSARY.md (unified)

A **canonical glossary** of cross-stack terms (Tab5, Dragon, K144/TinkerON, voice modes 0–5, ext_pcm, wakeword, native_tools, the WS protocol frames, etc.). Each repo carries the **full canonical core** (since repos stand alone) + repo-specific additions in a clearly-marked section. Existing `GLOSSARY.md` files (TinkerTab, TinkerBox) are merged into the canonical core.

## 9. Wave 0 deliverables (per repo × 4)

For **each** of TinkerTab, TinkerBox, PingOS, TinkerClaw:
1. Create `docs/{tutorials,how-to,reference,explanation,internal,_templates}/`.
2. **`git mv`** existing `docs/PLAN-*.md`, `AUDIT-*.md`, `RETRO-*.md` (and obvious working docs) → `docs/internal/` (history preserved). Leave already-categorizable docs (e.g. HARDWARE.md, protocol.md) for the content waves to slot into Reference.
3. Add `STYLE.md` + the 4 `docs/_templates/`.
4. Merge/author `GLOSSARY.md` (canonical core + repo additions).
5. Restructure `README.md`: add the **audience router** + the **peer block** at the top. (Existing prose stays; deep content is Waves 1–5.)
6. Author `docs/README.md` (the index/map).
7. **Seed pages** (TinkerTab only, to validate the templates end-to-end): one real page per Diátaxis type — `tutorials/getting-started.md`, `how-to/flash-firmware.md`, `reference/debug-server.md`, `explanation/how-the-stack-fits-together.md`.
8. Delete the empty `TinkerTab/docs-site/` shell.
9. Add `docs/ROADMAP.md` documenting the wave plan (so contributors see what's coming).
10. Update references that move: `CLAUDE.md` doc paths + the project's auto-memory entries that cite `docs/PLAN-*`/`AUDIT-*` paths now under `docs/internal/`.

## 10. Full program — waves after Wave 0

- **Wave 1 — TinkerTab content** (4 audiences): getting-started, hardware build/mods, build/flash, debug-server reference, voice/UI usage, troubleshooting, architecture.
- **Wave 2 — TinkerBox content**: getting-started, architecture/explanation, the big **REST API + WebSocket protocol reference**, backends/models, deploy/operate, the K144/TinkerON chain.
- **Wave 3 — PingOS content.**
- **Wave 4 — TinkerClaw content** — curate the ~719 inherited docs down to what matters (coordinate with the separate TinkerClaw↔OpenClaw merge/rebrand effort).
- **Wave 5 — Cross-cutting polish**: end-to-end tutorials spanning repos ("unboxing → working voice assistant"), full link-check, consistency pass against STYLE.md.

Once Wave 0 lands the templates + structure, content waves **fan out hard** (one agent per doc, given the template + source material) — this is what makes "full program" tractable.

## 11. Acceptance criteria (Wave 0)

- All 4 repos have the `docs/` Diátaxis structure + `docs/README.md` index + `STYLE.md` + 4 templates + `GLOSSARY.md` + restructured `README.md` (router + peer block).
- Internal working docs relocated to `docs/internal/` with git history preserved.
- All cross-repo + intra-repo links resolve (markdown link-check passes).
- TinkerTab has one validated seed page per Diátaxis type.
- The empty `docs-site/` shell is removed.
- `CLAUDE.md` + auto-memory references to moved docs are updated.

## 12. Non-goals (Wave 0)

- No Docusaurus / static site / hosting.
- No deep per-audience content (that's Waves 1–5).
- No wholesale rewrite of existing good README prose — restructure + route now, deepen in waves.
- No rebrand/merge work in TinkerClaw (that's the parallel TinkerClaw↔OpenClaw effort).

## 13. Risks & mitigations

- **Moving `docs/` files breaks links + memory/CLAUDE.md citations.** Many memory entries and `CLAUDE.md` cite `docs/PLAN-*.md` / `docs/AUDIT-*.md`. Mitigation: `git mv` (preserve history), update the high-traffic references in the same wave, run a link-check, and leave a one-line `docs/internal/README.md` mapping old→new. **DECISION (2026-05-29, owner): MOVE** internal docs to `docs/internal/` — the plan must include the reference-update + link-check steps.
- **Four repos × identical structure = duplication** (STYLE.md, GLOSSARY core, templates copied 4×). Accepted cost of "each repo stands alone." Mitigation: keep these files byte-identical; a future tiny sync script can diff them.
- **PingOS/TinkerClaw are moving targets** (PingOS active; TinkerClaw mid merge/rebrand). Mitigation: Wave 0 only lands structure/standards there; defer deep content (Waves 3/4) until those efforts settle.
- **Scope creep** — "world class" invites endless polish. Mitigation: the Diátaxis taxonomy + acceptance criteria are the definition of done per wave.

## 14. Resolved decisions

- **2026-05-29 (owner):** Internal `PLAN-*/AUDIT-*/RETRO-*` docs **move to `docs/internal/`** (via `git mv`); the Wave 0 plan includes updating `CLAUDE.md` + auto-memory references and a link-check.
- **2026-05-29 (owner):** Spec **approved** → proceed to the Wave 0 implementation plan.
