# TinkerTab — Historical Documents

Archive of docs that were load-bearing during earlier phases but have
been superseded by current state. Kept for context — "why did we
choose X?" / "what shape did the product have before?" — but no
longer authoritative.

If something here contradicts CLAUDE.md or LEARNINGS.md, **trust the
live docs**.

## Index

| File | Era | Why archived |
|------|-----|--------------|
| `BUILD_PLAN.md` | Phases 0-15 | Hardware bring-up plan, all phases shipped. Later phases (widgets, video calls, etc.) are tracked in GitHub issues, not as plan documents. |
| `CHANGELOG.md` | Through 2026-04-01 | Manually-maintained changelog stopped updating four weeks before the rich-media + widget platform work landed. Use `git log --oneline` or GitHub Releases for current history. |
| `STREAMING_ARCHITECTURE.md` | Pre-PR-#155 | CDP/MJPEG browser-streaming feature retired in PR #155 ("voice-first is the product"). Documented for historical context only — there is no streaming code in the firmware today. |
| `TINKERTAB_OS_SPEC.md` | v1.0, March 2026 | UI/UX spec from the same CDP/MJPEG-streaming era as `STREAMING_ARCHITECTURE.md`. Every screen described here was rebuilt as native LVGL in C; the spec is preserved as inspiration but doesn't match any current screen. |
| `UI_AUDIT.md` | 2026-03-27 | Snapshot of pre-v4·C UI state. Superseded by [`../UI-COMPLETENESS.md`](../UI-COMPLETENESS.md) which is actively maintained. |

## What's NOT in this folder (and why)

- **`stitch-designs/`** — local-only design exploration (gitignored,
  never committed). Lives in your working tree if you have it; not
  part of the repo.
- **`STATUS.md`** — kept in repo root because it's already
  self-labeled "ARCHIVED" and serves as a redirect to current docs.
  Moving it would break external bookmarks.
- **`STASH_INDEX.md`** — deleted entirely.  Git stashes are
  ephemeral by design; documenting them is an anti-pattern.
- **Old `test/e2e/` harness** — deleted entirely. Superseded by
  `tests/e2e/` (Python harness from PR #295).
- **`dragon_voice/` Python tree** — deleted entirely. That code
  belongs in [TinkerBox](https://github.com/lorcan35/TinkerBox)
  (the Dragon-side server repo); was mistakenly committed here
  during early experimentation.
