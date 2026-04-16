"""Interactive hardware tests — touches, screenshots, mode changes.

Marked @pytest.mark.hardware because they exercise POST endpoints and
screen state. v0.9.0 debug_server has a known stability issue where
back-to-back POSTs + screenshot can trigger a panic; v5 overhaul should
include handler reliability fixes (see issue tracker).

Run with:  pytest -m hardware
"""
from __future__ import annotations

import time
import pytest

from lib import Tab5Client, dominant_color
from lib.touch import SW, SH

pytestmark = [pytest.mark.hardware, pytest.mark.slow]


def test_screenshot_decodes(tab5: Tab5Client) -> None:
    img = tab5.screenshot()
    assert img.size == (720, 1280), f"wrong size: {img.size}"
    top_color = dominant_color(img, n=1)[0]
    r, g, b = top_color
    assert r < 60 and g < 60 and b < 80, f"dominant color not dark enough: {top_color}"


def test_touch_endpoint_accepted(tab5: Tab5Client) -> None:
    """Tap a neutral top-center point (shouldn't trigger navigation). Just
    verifies the /touch endpoint accepts a well-formed request."""
    tab5.tap(SW // 2, 100)


def test_navigate_round_trip(tab5: Tab5Client) -> None:
    """Use /navigate (programmatic screen switch) to verify state transitions.
    Cheaper than touch-based nav and works across both pre- and post-overhaul
    layouts."""
    tab5.navigate("settings")
    time.sleep(1.0)
    tab5.navigate("home")
    time.sleep(1.0)


def test_mode_cycle_round_trip(tab5: Tab5Client) -> None:
    """Cycle all 4 voice modes and confirm settings reflects each.

    NOTE: known to destabilise v0.9.0 firmware under rapid transitions — paced
    at 1.2 s between changes. Bumps pacing_s to avoid HTTP contention with the
    voice task sending config_update over WebSocket."""
    before = tab5.settings()
    original = int(before["voice_mode"])
    try:
        for m in (0, 1, 2, 3):
            tab5.set_voice_mode(m)
            time.sleep(1.2)
            now = tab5.settings()
            assert int(now["voice_mode"]) == m, f"mode didn't take: got {now['voice_mode']} expected {m}"
    finally:
        tab5.set_voice_mode(original)
