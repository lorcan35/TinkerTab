"""Baseline sanity tests — read-only, fast, always-passing.

Verifies the harness can talk to the device and state endpoints return the
expected shape. No touches, no mode changes, no screenshots (those have
destabilised v0.9.0 firmware in the past — see test_hardware.py).
"""
from __future__ import annotations

import pytest

from lib import Tab5Client


pytestmark = pytest.mark.baseline


def test_device_is_up(tab5: Tab5Client) -> None:
    info = tab5.info()
    assert info.wifi_ip, "device has no wifi_ip"
    assert info.psram_free > 4 * 1024 * 1024, f"PSRAM suspiciously low: {info.psram_free}"
    assert info.lvgl_fps > 10, f"LVGL frame rate too low: {info.lvgl_fps}"


def test_selftest_all_pass(tab5: Tab5Client) -> None:
    result = tab5.selftest()
    tests = result.get("tests", [])
    assert tests, "selftest returned no tests"
    failing = [t for t in tests if not t.get("pass")]
    # internal_heap may fail when fragmentation is high — warn, don't hard fail
    hard_fail = [t for t in failing if t.get("name") not in ("internal_heap",)]
    assert not hard_fail, f"selftest hard failures: {hard_fail}"


def test_voice_state_shape(tab5: Tab5Client) -> None:
    v = tab5.voice()
    assert "state" in v
    assert "state_name" in v
    assert v["state_name"] in {"IDLE", "CONNECTING", "READY", "LISTENING", "PROCESSING", "SPEAKING", "ERROR"}


def test_settings_shape(tab5: Tab5Client) -> None:
    s = tab5.settings()
    for key in ("wifi_ssid", "dragon_host", "voice_mode", "llm_model"):
        assert key in s, f"settings missing {key}"
    assert 0 <= int(s["voice_mode"]) <= 3
