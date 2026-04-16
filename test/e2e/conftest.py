"""Pytest fixtures — shared Tab5Client, artifact capture on failure."""
from __future__ import annotations

import os
from pathlib import Path

import pytest

from lib.tab5 import Tab5Client, Tab5Error

ARTIFACTS = Path(__file__).parent / "artifacts"


@pytest.fixture(scope="session")
def tab5() -> Tab5Client:
    """Single Tab5Client per session.  Token is resolved lazily on first auth call."""
    c = Tab5Client.from_env()
    c.warm_up(attempts=5)
    try:
        info = c.info()
    except Tab5Error as e:
        pytest.skip(f"Tab5 unreachable at {c.base}: {e}")
    if not info.auth_required:
        pytest.skip("device reports auth_required=false — unexpected")
    return c


@pytest.fixture(scope="session")
def artifacts_dir() -> Path:
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    return ARTIFACTS


@pytest.hookimpl(hookwrapper=True, tryfirst=True)
def pytest_runtest_makereport(item, call):
    """On test failure, capture a screenshot + selftest JSON into artifacts/."""
    outcome = yield
    rep = outcome.get_result()
    if rep.when != "call" or not rep.failed:
        return
    tab5_fixture = item.funcargs.get("tab5")
    if tab5_fixture is None:
        return
    safe_name = item.nodeid.replace("/", "_").replace(":", "_").replace("[", "_").replace("]", "_")
    test_dir = ARTIFACTS / safe_name
    test_dir.mkdir(parents=True, exist_ok=True)
    try:
        tab5_fixture.screenshot().save(test_dir / "failure.png")
    except Exception as e:  # noqa: BLE001
        (test_dir / "screenshot_err.txt").write_text(str(e))
    try:
        import json
        (test_dir / "info.json").write_text(json.dumps(tab5_fixture.info().raw, indent=2))
    except Exception as e:  # noqa: BLE001
        (test_dir / "info_err.txt").write_text(str(e))
    try:
        import json
        (test_dir / "selftest.json").write_text(json.dumps(tab5_fixture.selftest(), indent=2))
    except Exception as e:  # noqa: BLE001
        (test_dir / "selftest_err.txt").write_text(str(e))
