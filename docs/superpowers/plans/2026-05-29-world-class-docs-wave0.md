# World-Class Docs — Wave 0 (Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lay the world-class documentation foundation — Diátaxis structure, standards, templates, glossary, audience-routing READMEs, and peer cross-linking — across TinkerTab, TinkerBox, and PingOS, so the content waves (1–5) fan out consistently.

**Architecture:** Author the canonical shared assets once (in TinkerTab as the reference repo), then drop byte-identical copies into each repo (each repo stands alone). Restructure each `docs/` to Diátaxis, relocate internal working docs to `docs/internal/` via `git mv`, and turn each README into an audience router with a "Part of the Tinker stack" peer block. Validate with a markdown link-check.

**Tech Stack:** Markdown (GitHub-rendered), `git mv` for history-preserving moves, `npx markdown-link-check` (or `lychee`) for link validation.

**Spec:** `docs/superpowers/specs/2026-05-29-world-class-docs-program-design.md`

**Repos & branches:** work on a `docs/world-class-docs-program` branch in each repo (`~/projects/TinkerTab` branch already exists; create it in `~/projects/TinkerBox` and `~/projects/pingos`). **TinkerClaw Wave 0 is DEFERRED** until the parallel TinkerClaw↔OpenClaw merge/rebrand stabilizes — tracked in §Deferred, not executed here.

---

## File structure (what Wave 0 creates per repo)

```
README.md                 ← MODIFY: prepend peer block + audience router
GLOSSARY.md               ← CREATE/MERGE: canonical core + repo additions
STYLE.md                  ← CREATE: docs writing standard (identical across repos)
docs/
  README.md               ← CREATE: docs index/map
  ROADMAP.md              ← CREATE: the wave plan (identical across repos)
  tutorials/.gitkeep      ← CREATE
  how-to/.gitkeep         ← CREATE
  reference/.gitkeep      ← CREATE
  explanation/.gitkeep    ← CREATE
  _templates/{tutorial,how-to,reference,explanation}.md  ← CREATE (identical across repos)
  internal/README.md      ← CREATE: old→new map
  internal/<moved docs>   ← git mv of PLAN-*/AUDIT-*/RETRO-* etc.
```

Canonical assets authored in TinkerTab (Phase A), copied verbatim to TinkerBox/PingOS (Phases C/D): `STYLE.md`, `docs/_templates/*.md`, `docs/ROADMAP.md`, the GLOSSARY canonical core, the peer block, the standard doc header.

---

## Phase A — Canonical shared assets (authored in TinkerTab)

### Task A1: The standard doc header + peer block snippets

**Files:**
- Create: `~/projects/TinkerTab/docs/_templates/_HEADER.md` (reference snippet, not a page)

- [ ] **Step 1: Write the canonical doc-header + peer block** to `docs/_templates/_HEADER.md`:

```markdown
<!-- CANONICAL SNIPPETS — copy into every repo's docs/_templates/. Not a rendered page. -->

## Standard doc header (top of every authored page)
---
audience: tinkerer | developer | integrator | operator
type: tutorial | how-to | reference | explanation
prerequisites: <links or "none">
last-verified: YYYY-MM-DD
est-time: <e.g. 15 min>   # tutorials/how-to only
---

## Peer block (top of every README.md + docs/README.md)
> **Part of the Tinker stack** — four repos, each documented on its own:
> [**TinkerTab**](https://github.com/lorcan35/TinkerTab) (Tab5 device firmware) ·
> [**TinkerBox**](https://github.com/lorcan35/TinkerBox) (Dragon inference server — "the brain") ·
> [**PingOS**](https://github.com/lorcan35/PingOS) (website-to-API automation gateway) ·
> [**TinkerClaw**](https://github.com/lorcan35/TinkerClaw) (agent sidecar).
> New here? Each repo's README is its own front door.

## Audience router (top of every README.md, under the peer block)
**I want to…**
→ [**use it**](docs/tutorials/getting-started.md)
· [**build / modify it**](docs/how-to/dev-setup.md)
· [**integrate with it**](docs/reference/README.md)
· [**run it in production**](docs/how-to/deploy.md)
```

- [ ] **Step 2: Verify** the file renders (open in a Markdown previewer or `grep -c '^>' docs/_templates/_HEADER.md` → ≥1).
- [ ] **Step 3: Commit**

```bash
cd ~/projects/TinkerTab
git add docs/_templates/_HEADER.md
git commit -m "docs: canonical doc-header + peer block + audience router snippets"
```

### Task A2: The four Diátaxis templates

**Files:**
- Create: `~/projects/TinkerTab/docs/_templates/tutorial.md`, `how-to.md`, `reference.md`, `explanation.md`

- [ ] **Step 1: Write `tutorial.md`:**

```markdown
---
audience: tinkerer
type: tutorial
prerequisites: none
last-verified: YYYY-MM-DD
est-time: XX min
---
# <Tutorial title — "Build/Do X">

By the end you will have <concrete outcome>. This is a learning-by-doing walkthrough; every step has a visible result so you know it worked.

## Before you start
- <hardware/software you need>

## Steps
1. **<Action>** — <exact command/tap>.
   _You should see:_ <observable result>.
2. ...

## What you built
<recap + a screenshot/serial line proving success>

## Next
- [<related how-to>](../how-to/...)
```

- [ ] **Step 2: Write `how-to.md`:**

```markdown
---
audience: developer | operator | integrator
type: how-to
prerequisites: <links>
last-verified: YYYY-MM-DD
est-time: XX min
---
# How to <accomplish task>

Use this when you need to <goal>. Assumes you already <prerequisite knowledge>.

## Steps
1. <step with exact command>
2. ...

## Verify it worked
Run: `<command>` → expect `<output>`.

## Troubleshooting
- **<symptom>** → <cause/fix>.
```

- [ ] **Step 3: Write `reference.md`:**

```markdown
---
audience: integrator | developer
type: reference
prerequisites: none
last-verified: YYYY-MM-DD
---
# <Thing> reference

Authoritative, lookup-oriented. No tutorials here.

## <Section, e.g. Endpoints / Fields / Commands>
| Name | Type | Description | Notes |
|---|---|---|---|
| ... | ... | ... | ... |

## Examples
```<lang>
<minimal precise example>
```
```

- [ ] **Step 4: Write `explanation.md`:**

```markdown
---
audience: developer
type: explanation
prerequisites: none
last-verified: YYYY-MM-DD
---
# <Concept> — how it works and why

## The question
<what this page helps you understand>

## The model
<the architecture / data flow, with an ASCII diagram if useful>

## Why it's built this way
<trade-offs, alternatives considered, constraints>

## See also
- [<how-to>](../how-to/...) · [<reference>](../reference/...)
```

- [ ] **Step 5: Verify** all four exist: `ls docs/_templates/*.md | wc -l` → `5` (4 + `_HEADER.md`).
- [ ] **Step 6: Commit**

```bash
git add docs/_templates/*.md
git commit -m "docs: four Diátaxis page templates (tutorial/how-to/reference/explanation)"
```

### Task A3: STYLE.md (the writing standard)

**Files:**
- Create: `~/projects/TinkerTab/STYLE.md`

- [ ] **Step 1: Write `STYLE.md`** with these exact sections (full prose, no placeholders): (1) **Purpose** — "how we write docs so all four repos read as one voice"; (2) **Voice & tone** — direct, second-person, active, confident-not-defensive, no marketing hype (cross-ref the project's no-defeatist-language norm); (3) **Diátaxis rule** — the four-type table from the spec §2 + "never mix types in one page"; (4) **Standard header** — reproduce the §5 frontmatter and make it mandatory; (5) **Naming** — `kebab-case.md`, verbs for how-to, nouns for reference; (6) **Code blocks** — language-tagged, copy-pasteable, show expected output for verification; (7) **`last-verified` discipline**; (8) **Cross-repo links** — use the peer block; link to a repo's README not its internals; (9) **Terminology** — use GLOSSARY terms, link first use; (10) **Accessibility** — alt text, descriptive link text, heading hierarchy.
- [ ] **Step 2: Verify** all 10 sections present: `grep -c '^## ' STYLE.md` → `≥10`.
- [ ] **Step 3: Commit** `git add STYLE.md && git commit -m "docs: STYLE.md — the cross-repo documentation writing standard"`

### Task A4: GLOSSARY.md (canonical core)

**Files:**
- Create: `~/projects/TinkerTab/GLOSSARY.md`
- Read first: existing `~/projects/TinkerTab/GLOSSARY.md` (if present) + `~/projects/TinkerBox/GLOSSARY.md`

- [ ] **Step 1: Merge** both existing glossaries into one canonical core, then add any missing cross-stack terms. **Define at minimum:** Tab5, ESP32-P4, Dragon (Radxa Q6A), TinkerBox, TinkerTab, PingOS, TinkerClaw, K144 / TinkerON, StackFlow, voice modes 0–5 (Local/Hybrid/Cloud/TinkerClaw/Agent/Solo), wakeword / ext_pcm, `wake_src`, native tool-calling (`native_tools`), the WS protocol frames (register/config_update/text/user_media/media/text_update/channel_message), Diátaxis, LVGL, llama-server, Granite, Moonshine, Piper/Kokoro. Each entry: one-line definition + optional "see [doc]".
- [ ] **Step 2: Verify** `grep -c '^### \|^- \*\*' GLOSSARY.md` → `≥25`.
- [ ] **Step 3: Commit** `git add GLOSSARY.md && git commit -m "docs: unified canonical GLOSSARY"`

### Task A5: docs/ROADMAP.md (the wave plan)

**Files:**
- Create: `~/projects/TinkerTab/docs/ROADMAP.md`

- [ ] **Step 1: Write `docs/ROADMAP.md`** summarizing the program waves (from spec §10): Wave 0 Foundation (this), Wave 1 TinkerTab content, Wave 2 TinkerBox content, Wave 3 PingOS, Wave 4 TinkerClaw, Wave 5 cross-cutting polish — with a one-line status per wave (Wave 0 = in progress, rest = planned). Link the spec.
- [ ] **Step 2: Commit** `git add docs/ROADMAP.md && git commit -m "docs: documentation program roadmap"`

---

## Phase B — TinkerTab (reference repo, full Wave 0 incl. seed pages)

### Task B1: Create the Diátaxis directory skeleton

**Files:** Create `docs/{tutorials,how-to,reference,explanation,internal}/.gitkeep`

- [ ] **Step 1:** `cd ~/projects/TinkerTab && mkdir -p docs/{tutorials,how-to,reference,explanation,internal} && touch docs/{tutorials,how-to,reference,explanation,internal}/.gitkeep`
- [ ] **Step 2: Verify** `ls -d docs/{tutorials,how-to,reference,explanation,internal}` → all five exist.
- [ ] **Step 3: Commit** `git add docs/*/.gitkeep && git commit -m "docs: add Diátaxis directory skeleton"`

### Task B2: Relocate internal working docs

**Files:** `git mv docs/PLAN-*.md docs/AUDIT-*.md docs/RETRO-*.md → docs/internal/`; create `docs/internal/README.md`

- [ ] **Step 1: List** what moves: `ls docs/PLAN-*.md docs/AUDIT-*.md docs/RETRO-*.md` (record the names).
- [ ] **Step 2: Move** each with history preserved: `git mv docs/PLAN-*.md docs/internal/` then `git mv docs/AUDIT-*.md docs/internal/` then `git mv docs/RETRO-*.md docs/internal/` (run per-glob; if a glob matches nothing, skip it).
- [ ] **Step 3: Write `docs/internal/README.md`** — "Internal working docs (plans, audits, retros). Moved here 2026-05-29 from `docs/`. Not audience documentation — see `docs/README.md` for user-facing docs." + a list mapping each moved file.
- [ ] **Step 4: Verify** `ls docs/PLAN-* 2>/dev/null | wc -l` → `0` and `ls docs/internal/*.md | wc -l` → matches Step 1 count + 1.
- [ ] **Step 5: Commit** `git add -A docs/ && git commit -m "docs: relocate internal PLAN/AUDIT/RETRO docs to docs/internal/ (history preserved)"`

### Task B3: README audience router + peer block

**Files:** Modify `~/projects/TinkerTab/README.md` (prepend, under the `# TinkerTab` title)

- [ ] **Step 1: Insert** the peer block + audience router (from Task A1) immediately after the `# TinkerTab` heading and its one-line description, before the existing Table of Contents.
- [ ] **Step 2: Verify** `grep -c 'Part of the Tinker stack' README.md` → `1`.
- [ ] **Step 3: Commit** `git add README.md && git commit -m "docs: README audience router + Tinker-stack peer block"`

### Task B4: docs/README.md index

**Files:** Create `~/projects/TinkerTab/docs/README.md`

- [ ] **Step 1: Write `docs/README.md`** — peer block at top, then a map grouping every (current + planned) page by Diátaxis type and audience, linking the seed pages (Task B5) and `docs/internal/` + `STYLE.md` + `GLOSSARY.md` + `ROADMAP.md`.
- [ ] **Step 2: Commit** `git add docs/README.md && git commit -m "docs: docs/ index and map"`

### Task B5: Seed pages (validate the templates end-to-end)

**Files:** Create `docs/tutorials/getting-started.md`, `docs/how-to/flash-firmware.md`, `docs/reference/debug-server.md`, `docs/explanation/how-the-stack-fits-together.md`

- [ ] **Step 1: `getting-started.md`** (from `tutorial.md`) — real content: unbox → flash → first boot → "Hey Tinker" → first voice turn. Pull steps from the current README Quick Start; add observable results. `last-verified: 2026-05-29`.
- [ ] **Step 2: `flash-firmware.md`** (from `how-to.md`) — real content: source ESP-IDF, `idf.py set-target esp32p4`, build, flash, watchdog-reset recovery. From README Build/Flash. Add the verify + troubleshooting (ROM download mode → watchdog reset).
- [ ] **Step 3: `debug-server.md`** (from `reference.md`) — real content: the `:8080` debug server — `/info`, `/selftest`, `/screenshot`, `/touch`, bearer-token auth, the post-Wave-23b module map. (Source: the current README Debug Server section + memory `reference_tab5_debug_access`.)
- [ ] **Step 4: `how-the-stack-fits-together.md`** (from `explanation.md`) — real content: Tab5 (thin client) ↔ Dragon (brain) ↔ K144/TinkerON, with an ASCII diagram + voice-mode tiers. This is the page the peer block links for "how do these relate".
- [ ] **Step 5: Verify** each has the standard header: `head -8 docs/tutorials/getting-started.md | grep -c 'last-verified'` → `1` (repeat for each).
- [ ] **Step 6: Commit** `git add docs/tutorials docs/how-to docs/reference docs/explanation && git commit -m "docs: seed pages — one validated example per Diátaxis type"`

### Task B6: Remove the empty docs-site shell

**Files:** Delete `~/projects/TinkerTab/docs-site/`

- [ ] **Step 1: Confirm** it's empty/shell: `ls docs-site/` → only `build`/`node_modules` (no authored `docs/*.md`).
- [ ] **Step 2: Remove** `git rm -r docs-site/ 2>/dev/null || rm -rf docs-site/` then ensure it's not gitignored cruft; add `docs-site/` removal.
- [ ] **Step 3: Commit** `git add -A && git commit -m "docs: remove abandoned empty docs-site Docusaurus shell"`

### Task B7: Update references to moved docs

**Files:** Modify `~/projects/TinkerTab/CLAUDE.md` (if it cites `docs/PLAN-*`/`docs/AUDIT-*`) + flag memory updates

- [ ] **Step 1: Find** stale refs: `grep -rn 'docs/PLAN-\|docs/AUDIT-\|docs/RETRO-' CLAUDE.md README.md docs/ 2>/dev/null` (exclude `docs/internal/`).
- [ ] **Step 2: Update** each hit to `docs/internal/<file>`.
- [ ] **Step 3: Update auto-memory** — in `/home/rebelforce/.claude/projects/-home-rebelforce/memory/`, update entries citing `TinkerTab/docs/AUDIT-state-of-stack-2026-05-11.md` etc. to `docs/internal/`. (Do via the memory Write/Edit, not in-repo.)
- [ ] **Step 4: Commit** `git add CLAUDE.md README.md docs/ && git commit -m "docs: update references to relocated internal docs"`

### Task B8: Link-check TinkerTab

- [ ] **Step 1: Run** `npx --yes markdown-link-check -q README.md docs/README.md docs/tutorials/*.md docs/how-to/*.md docs/reference/*.md docs/explanation/*.md` (or `lychee README.md 'docs/**/*.md'`). Network links may warn — focus on broken **relative** links.
- [ ] **Step 2: Fix** any broken relative links.
- [ ] **Step 3: Commit** any fixes `git commit -am "docs: fix broken relative links (link-check)"`

---

## Phase C — TinkerBox (mirror, no seed pages)

### Task C1: Branch + Diátaxis skeleton + copy canonical assets

**Files:** `~/projects/TinkerBox` — create branch, dirs, copy assets from TinkerTab

- [ ] **Step 1:** `cd ~/projects/TinkerBox && git checkout -b docs/world-class-docs-program`
- [ ] **Step 2:** `mkdir -p docs/{tutorials,how-to,reference,explanation,internal,_templates} && touch docs/{tutorials,how-to,reference,explanation,internal}/.gitkeep`
- [ ] **Step 3: Copy** canonical assets verbatim: `cp ~/projects/TinkerTab/docs/_templates/*.md docs/_templates/ && cp ~/projects/TinkerTab/STYLE.md STYLE.md && cp ~/projects/TinkerTab/docs/ROADMAP.md docs/ROADMAP.md`
- [ ] **Step 4: Verify** `ls docs/_templates/*.md | wc -l` → `5`; `test -f STYLE.md && echo ok`.
- [ ] **Step 5: Commit** `git add -A && git commit -m "docs: Diátaxis skeleton + canonical STYLE/templates/roadmap"`

### Task C2: Merge GLOSSARY

- [ ] **Step 1: Copy** the canonical core: `cp ~/projects/TinkerTab/GLOSSARY.md GLOSSARY.md`, then append a `## TinkerBox-specific terms` section (backends, router fleet, `lmstudio`, `ConversationEngine`, MediaPipeline, scheduler, channels/gateway, `agent_log`).
- [ ] **Step 2: Commit** `git add GLOSSARY.md && git commit -m "docs: unified GLOSSARY (canonical core + TinkerBox terms)"`

### Task C3: Relocate internal docs

- [ ] **Step 1:** `git mv docs/PLAN-*.md docs/AUDIT-*.md docs/SOLID-AUDIT.md docs/WAVE-*.md docs/RFC-*.md docs/internal/` (per-glob; skip empty globs). Keep `protocol.md`, `ARCHITECTURE.md`, `router-cookbook.md`, `nph-setup.md` in place (Reference candidates for Wave 2).
- [ ] **Step 2: Write `docs/internal/README.md`** (same shape as B2).
- [ ] **Step 3: Commit** `git add -A docs/ && git commit -m "docs: relocate internal docs to docs/internal/ (history preserved)"`

### Task C4: README router + peer block + docs index

- [ ] **Step 1: Insert** peer block + audience router after the `# TinkerBox` title (router links: use → tutorials/getting-started.md [planned], build → how-to/dev-setup.md [planned], integrate → reference/README.md, run → how-to/deploy.md [planned]).
- [ ] **Step 2: Create `docs/README.md`** index (peer block + map; note content pages are Wave 2).
- [ ] **Step 3: Commit** `git add README.md docs/README.md && git commit -m "docs: README router + peer block + docs index"`

### Task C5: Update refs + link-check

- [ ] **Step 1:** `grep -rn 'docs/PLAN-\|docs/AUDIT-\|docs/SOLID-AUDIT\|docs/WAVE-\|docs/RFC-' CLAUDE.md README.md 2>/dev/null` → update hits to `docs/internal/`. (TinkerBox `CLAUDE.md` cites several — fix them.)
- [ ] **Step 2:** Update auto-memory entries citing TinkerBox `docs/` internal paths.
- [ ] **Step 3:** Link-check (as B8). Fix broken relative links.
- [ ] **Step 4: Commit** `git commit -am "docs: update references to relocated internal docs + link-check"`

---

## Phase D — PingOS (structure only)

### Task D1: Branch + skeleton + canonical assets + README router

**Files:** `~/projects/pingos`

- [ ] **Step 1:** `cd ~/projects/pingos && git checkout -b docs/world-class-docs-program`
- [ ] **Step 2:** `mkdir -p docs/{tutorials,how-to,reference,explanation,internal,_templates} && touch docs/{tutorials,how-to,reference,explanation,internal}/.gitkeep`
- [ ] **Step 3: Copy** assets: `cp ~/projects/TinkerTab/docs/_templates/*.md docs/_templates/ && cp ~/projects/TinkerTab/STYLE.md STYLE.md && cp ~/projects/TinkerTab/docs/ROADMAP.md docs/ROADMAP.md && cp ~/projects/TinkerTab/GLOSSARY.md GLOSSARY.md`
- [ ] **Step 4: Relocate** any internal docs (`git mv docs/*-LOG.md docs/internal/` etc.; PingOS has `DOCS-UPDATE-LOG.md` at root — leave root files, only move clearly-internal `docs/` files) + `docs/internal/README.md`.
- [ ] **Step 5: Insert** peer block + audience router in `README.md`; create `docs/README.md` index.
- [ ] **Step 6: Verify + link-check** (as B8).
- [ ] **Step 7: Commit** `git add -A && git commit -m "docs: Wave 0 foundation — Diátaxis structure, standards, peer block (PingOS)"`

---

## Deferred — TinkerClaw Wave 0

**Not executed in this plan.** TinkerClaw is mid TinkerClaw↔OpenClaw merge/rebrand (parallel effort; 2065 files pending rebrand). Landing docs structure now would collide with the rebrand. **Trigger:** after the merge/rebrand stabilizes, run a TinkerClaw-specific Wave 0 (same task shape as Phase D) that also curates the ~719 inherited docs. Tracked in `docs/ROADMAP.md` Wave 4.

---

## Finalization

### Task F1: Open PRs

- [ ] **Step 1:** For each repo (TinkerTab, TinkerBox, PingOS): push `docs/world-class-docs-program` and open a PR titled "docs: Wave 0 — world-class documentation foundation", body linking the spec + this plan. (Per each repo's contributing workflow; squash-merge.)
- [ ] **Step 2:** Cross-link the three PRs in each PR body so reviewers see the program.

---

## Self-Review

**Spec coverage:** spec §2 (Diátaxis) → A2/A4 templates+glossary + B5 seed pages; §3 (per-repo IA) → B1/C1/D1; §4 (peer block) → A1 + B3/C4/D1; §5 (header) → A1 + templates; §6 (STYLE) → A3; §7 (templates) → A2; §8 (glossary) → A4/C2; §9 (Wave 0 deliverables) → all of B/C/D; §11 acceptance → B8/C5/D1 link-checks + structure verifies; §12 non-goals respected (no site — B6 removes the shell; no deep content); §13 risk (moved-doc refs) → B7/C5. TinkerClaw (§10 Wave 4) explicitly deferred. **No gaps.**

**Placeholder scan:** template bodies use `<...>` as authoring slots **inside template files** (intended — they are templates), not plan placeholders; every task has exact paths, commands, and commit messages. Seed-page tasks (B5) specify real content sources. OK.

**Type consistency:** directory names (`tutorials/how-to/reference/explanation/internal/_templates`), branch name (`docs/world-class-docs-program`), and asset filenames are identical across A/B/C/D. OK.

---

## Execution Handoff

This plan is parallelizable: Phase A is sequential (authors the canonical assets), then Phases B/C/D are largely independent per-repo (can fan out). Recommended: execute Phase A inline, then fan out B/C/D.
