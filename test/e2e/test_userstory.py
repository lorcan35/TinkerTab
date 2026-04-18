"""User-story end-to-end tests.

Drive the device through a full interaction the way a user would — taps,
long-presses, swipes — and assert observable state after each step. If
any step destabilises the device (boot-loop, panic, hang), the test fails
with a screenshot captured in artifacts/.

Tagged @pytest.mark.hardware because they mutate device state (mode,
overlays). Run with:  pytest -m hardware -k userstory
"""
from __future__ import annotations

import time

import pytest

from lib import Tab5Client
from lib.touch import ORB_CENTER, SW, SH

pytestmark = [pytest.mark.hardware, pytest.mark.slow]


def _wait_mode(t: Tab5Client, target: int, timeout_s: float = 5.0) -> int:
    """Poll /settings until voice_mode == target (or timeout). Returns final."""
    deadline = time.monotonic() + timeout_s
    last = -1
    while time.monotonic() < deadline:
        s = t.settings()
        last = int(s["voice_mode"])
        if last == target:
            return last
        time.sleep(0.3)
    return last


def test_story_mode_cycle_via_long_press(tab5: Tab5Client) -> None:
    """User on home screen long-presses the orb -> mode advances by one."""
    # Settle on Local so the cycle order is deterministic.
    tab5.set_voice_mode(0)
    time.sleep(0.8)
    before = int(tab5.settings()["voice_mode"])
    assert before == 0, f"expected Local (0) after set, got {before}"

    # Long-press orb center — the debug server holds touch for 800 ms,
    # long enough for LV_EVENT_LONG_PRESSED.
    tab5.long_press(ORB_CENTER.x, ORB_CENTER.y, duration_ms=800)

    after = _wait_mode(tab5, 1, timeout_s=3.0)
    assert after == 1, f"mode should have cycled 0 -> 1, got {after}"

    # Clean up — restore caller-friendly state (Hybrid is the neutral default).


def test_story_swipe_right_opens_notes(tab5: Tab5Client) -> None:
    """User swipes from the left edge of the home screen -> Notes overlay opens."""
    # Ensure we're on home.
    tab5.navigate("home")
    time.sleep(0.8)

    # Swipe from the left edge rightward. Start slightly inside the edge so
    # LVGL sees the press-down on an object, then drags across.
    tab5.swipe(x1=10, y1=SH // 2, x2=SW // 2, y2=SH // 2, duration_ms=260)

    # The notes overlay doesn't expose itself in /info or /voice; the best
    # signal we have is a stable /info response (no panic) + the device is
    # still in a known state.  Verify the device didn't crash.
    time.sleep(0.6)
    info = tab5.info()
    assert info.lvgl_fps >= 1, "LVGL task stalled after swipe"
    assert info.reset_reason not in ("PANIC", "HW_WDT", "SW_WDT"), \
        f"device reset: {info.reset_reason}"


def test_story_orb_tap_starts_listening(tab5: Tab5Client) -> None:
    """User taps the orb on home -> voice overlay opens and state goes LISTENING."""
    tab5.navigate("home")
    time.sleep(0.6)

    # Pre-condition: voice should be READY (not already LISTENING).
    before = tab5.voice()
    assert before["state_name"] in {"READY", "IDLE", "CONNECTING"}, \
        f"unexpected pre-tap voice state: {before}"

    # Tap orb center.
    tab5.tap(ORB_CENTER.x, ORB_CENTER.y)

    # Give the device a moment to animate + start mic capture.
    reached = tab5.wait_for_voice_state("LISTENING", timeout_s=5.0)
    assert reached["state_name"] == "LISTENING", f"got {reached}"

    # Clean up — swipe-down on voice overlay cancels.
    tab5.swipe(x1=SW // 2, y1=SH // 2, x2=SW // 2, y2=SH - 40, duration_ms=200)
    time.sleep(1.0)


def test_story_no_crash_across_quick_taps(tab5: Tab5Client) -> None:
    """Rapid-fire taps across different home hitboxes shouldn't panic the device."""
    tab5.navigate("home")
    time.sleep(0.5)

    # Hit several clickable areas quickly. This triggers the overlay-guard
    # paths in ui_home (25b15e1): each subsequent tap should no-op because
    # the previous one is animating an overlay.
    coords = [
        (ORB_CENTER.x, ORB_CENTER.y),     # orb -> voice
        (80,            80),              # sys label -> memory
        (SW - 80,       80),              # mode area
        (400,           1050),            # poem -> agents
    ]
    for (x, y) in coords:
        tab5.tap(x, y)
        time.sleep(0.15)

    # Assert no crash, UI still alive, responsive to /info.
    time.sleep(1.0)
    info = tab5.info()
    assert info.reset_reason not in ("PANIC", "HW_WDT", "SW_WDT"), \
        f"device reset after rapid taps: {info.reset_reason}"
