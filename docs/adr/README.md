# Architecture Decision Records

W10-C from the 2026-05-11 cross-stack audit.  ADRs capture
**non-obvious cross-stack architectural decisions** that took
real thinking to settle and that future contributors (or future
sessions of the same contributor) should not have to re-litigate.

## When to write one

Write an ADR when a decision satisfies **all three**:

1. **Cross-cutting** — touches Tab5 firmware *and* Dragon server,
   or touches >2 modules in either repo
2. **Non-reversible at low cost** — choosing it commits other code
   to its assumptions; reverting requires touching many sites
3. **Non-obvious justification** — a reader scanning the code
   without context would ask "why on earth did they do it that
   way?" and not get an answer from grep

## What NOT to ADR

- Pure refactors (no behavior change)
- Single-module patches
- Library version bumps
- One-line bug fixes
- Anything captured well in a commit message + `LEARNINGS.md` entry

## File naming

`NNNN-kebab-slug.md` — zero-padded sequential 4-digit number,
then a slug describing the decision.  Numbers never change once
assigned; ADRs are immutable once **Accepted** (status notes
below).  Superseded ADRs are kept in place with a status update
+ link to the replacement.

## Template

Use [`0000-template.md`](0000-template.md) — copy, rename, fill in.
Sections are deliberately short.

## Status values

- **Proposed** — under discussion; not yet implemented
- **Accepted** — in force; implementation lives in the repo
- **Superseded** — replaced by ADR-NNNN (links forward)
- **Deprecated** — no longer used, kept for historical context

## Index

| # | Title | Status |
|---|-------|--------|
| [0001](0001-host-test-infra-pattern.md) | Host-test infra: plain assert + ESP-IDF shims | Accepted |
