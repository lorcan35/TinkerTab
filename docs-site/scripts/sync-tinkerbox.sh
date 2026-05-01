#!/usr/bin/env bash
#
# Pulls the current TinkerBox docs/ tree into docs-site/imported/tinkerbox/.
# Curator merges into the published tree by hand — see PR 3 of the docs-site
# plan for the curation rules.
#
# This is intentionally manual (run before edits) and gitignored.  Auto-sync
# via GitHub Action is filed as a follow-up but not required for v1.

set -euo pipefail

SRC="${TINKERBOX_DIR:-$HOME/projects/TinkerBox}"
DEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/imported/tinkerbox"

if [ ! -d "$SRC/docs" ]; then
  echo "error: $SRC/docs not found.  Set TINKERBOX_DIR to override."
  exit 1
fi

echo "Syncing $SRC/docs/  →  $DEST_DIR"
mkdir -p "$DEST_DIR"
rsync -a --delete --exclude='.git' "$SRC/docs/" "$DEST_DIR/"

# Optional extras worth pulling for curation
for f in CLAUDE.md LEARNINGS.md GLOSSARY.md README.md; do
  if [ -f "$SRC/$f" ]; then
    cp "$SRC/$f" "$DEST_DIR/_$f"
  fi
done

echo "Done.  Files now in $DEST_DIR/"
