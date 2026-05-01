---
title: Contributing
sidebar_label: Contributing
---

# Contributing

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Both repos use the same workflow. The full versions are in [TinkerTab CONTRIBUTING.md](https://github.com/lorcan35/TinkerTab/blob/main/CONTRIBUTING.md) and [TinkerBox CONTRIBUTING.md](https://github.com/lorcan35/TinkerBox/blob/main/CONTRIBUTING.md). This is the cross-repo summary.

## Workflow

```
1. Issue first
   gh issue create --title "..." --body "..."

2. Branch from main
   feat/<slug> | fix/<slug> | chore/<slug> | docs/<slug>

3. Commit with issue ref
   feat: short description (refs #N)
   or
   fix: short description (closes #N)

4. Push, open PR
   gh pr create --title "..." --body "..."
   Let CI run. Squash-merge. Delete the branch.
```

## Issue format

```markdown
## Bug / Problem
What the user sees. Symptoms, frequency, impact.

## Root Cause
Technical explanation of WHY this happens.

## Culprit
Exact file(s) and line(s) responsible.

## Fix
What was changed and why this fixes it.

## Resolved
Commit hash + PR if applicable.
```

## Commit messages

- **Reference issues**: `fix: description (closes #N)` or `feat: description (refs #N)`
- **Atomic**: one fix per commit. Push after each.
- **Never batch unrelated fixes** in one commit.

Conventional-commit prefixes: `feat:` / `fix:` / `chore:` / `docs:` / `test:` / `refactor:`.

## PR scope discipline

The hard rule: **one concern per PR**.

- Refactors don't contain bug fixes
- Bug fixes don't contain "while we're here" cleanups
- Doc updates don't contain behavior changes

If you find something else that needs fixing mid-PR, open a new issue and move on.

**Extract before decompose.** When splitting a large file, the first PR *moves* code to its new home with identical behavior — no restructuring of the internals. Decomposing the internals is follow-up PRs. This keeps each diff reviewable in minutes, not hours.

**Tests move with code.** If the thing you're extracting has tests, move them in the same commit. If it has no tests, add at least one before the extraction lands.

**Small is kind.** Prefer 5 small PRs over 1 big one. Reviewers are more generous to a 200-line diff than a 2,000-line diff.

## CI gates

### TinkerTab

- **clang-format** runs against changed C/H lines. Pre-existing format violations elsewhere are intentionally ignored, but **your diff must be clean**.

```bash
git fetch origin main:refs/remotes/origin/main
git-clang-format --binary clang-format-18 --diff origin/main main/*.c main/*.h
# Empty diff → green. Any output → fix:
git-clang-format --binary clang-format-18 origin/main
git add -u && git commit --amend --no-edit && git push -f
```

### TinkerBox

- **Ruff** with a narrow gate: `F821,F722,F811,F823,B006,B904,E722,B007,RUF006`
- **Named pytest set** — E2E tests (`test_api_e2e.py`, `test_e2e_dragon.py`) are local-only. New tests added without a live-server requirement should be added to the CI list in `.github/workflows/ci.yml`.

## Plan docs for multi-week features

For anything that won't ship in a single PR (Grove, K144, widget platform, multi-model router), **write a plan doc first**:

```
docs/PLAN-<feature-name>.md
```

Include:

- Hardware reality / constraint
- Code architecture
- Phased breakdown with file:line refs
- Honest unknowns
- Code anchors

File a tracking issue that links to the doc as source-of-truth. **Branches die; docs + issues persist.**

Examples: [`docs/PLAN-k144-recovery.md`](https://github.com/lorcan35/TinkerTab/blob/main/docs/PLAN-k144-recovery.md), [`docs/PLAN-widget-platform.md`](https://github.com/lorcan35/TinkerTab/blob/main/docs/PLAN-widget-platform.md).

## Anti-slop rules

Apply equally to human and AI contributors:

- **No defensive code for impossible scenarios.** Trust internal callers. Validate at system boundaries (HTTP, WS frame, NVS read).
- **Delete, don't comment out.** Git remembers.
- **No comments that restate well-named code.** Comments explain *why*, or warn about non-obvious constraints.
- **No speculative abstractions.** A factory class with one caller is a one-caller class pretending.
- **No "helpful" refactors next to the feature.** Separate PR.
- **Name things for the reader.**

## LEARNINGS.md is not optional

Every bug fix with a non-obvious root cause adds an entry to `LEARNINGS.md`:

```markdown
## Date — Symptom
**Root cause**: ...
**Fix**: ...
**Prevention**: ...
```

Skip it only if the fix is genuinely one-line and self-explaining. Future-you will thank you.

## What this project is *not* taking PRs for

Out-of-scope contributions (filed as won't-fix if submitted):

- Wake-word feature in shipped firmware (parked permanently; revival path is K144 KWS)
- CDP browser-streaming (retired in PR #155 — voice-first is the product)
- Mooncake-style application framework (our service_registry is better-suited)
- Rewriting any subsystem to C++

If you're not sure something fits, file an issue with `[Q]` prefix and we'll talk before you start.
