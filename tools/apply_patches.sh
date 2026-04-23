#!/bin/bash
# TinkerTab — apply managed-component patches
#
# The ESP-IDF component manager wipes managed_components/ on every
# `idf.py reconfigure` and restores them from dependencies.lock.  Any
# edits we need to make to third-party source (e.g. defensive bounds
# checks for bugs that upstream hasn't fixed yet) live here as
# unified-diff patch files and are re-applied before each build.
#
# Idempotent: a sentinel string ("TinkerTab #<issue>") in the patched
# file indicates the patch is already applied.
#
# Called automatically from the top-level CMakeLists.txt at configure
# time.  Safe to invoke manually: ./tools/apply_patches.sh

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

apply_if_needed() {
    local patch_file="$1"
    local target_file="$2"
    local sentinel="$3"

    if [ ! -f "$target_file" ]; then
        echo "[patches] target not found (skipped — managed_components not yet fetched): $target_file"
        return 0
    fi

    if grep -q -F "$sentinel" "$target_file"; then
        echo "[patches] already applied: $(basename "$patch_file") → $(basename "$target_file")"
        return 0
    fi

    echo "[patches] applying $(basename "$patch_file") → $(basename "$target_file")"
    patch -p1 --directory="$(dirname "$target_file" | sed 's|/src/.*||')" --silent < "$patch_file" || {
        echo "[patches] ERROR: failed to apply $(basename "$patch_file")"
        return 1
    }
}

apply_if_needed \
    "$ROOT/patches/lvgl-get-next-line-bounds.patch" \
    "$ROOT/managed_components/lvgl__lvgl/src/draw/sw/lv_draw_sw_mask.c" \
    "TinkerTab #158"

echo "[patches] done."
