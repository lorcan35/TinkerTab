"""Scenario runner: drives a Tab5Driver through a list of `Step`s and
records pass/fail + screenshots + events for each.

A scenario is a Python function that yields Step instances.  Each Step
either calls an action and asserts, or just captures state.  Failures
don't abort the run — we want to know how far the firmware gets and
what the failure looks like in screenshot+events.

Usage:
    python3 -m tests.e2e.runner story_smoke
    python3 -m tests.e2e.runner story_full
    python3 -m tests.e2e.runner story_stress
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import traceback
from dataclasses import asdict, dataclass, field
from typing import Callable, Iterable

# Allow `python3 -m tests.e2e.runner` from project root, or direct script run.
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

from driver import Event, Tab5Driver  # noqa: E402


@dataclass
class StepResult:
    name: str
    ok: bool
    elapsed_s: float
    note: str = ""
    screenshot: str | None = None
    events: list[dict] = field(default_factory=list)


@dataclass
class RunReport:
    scenario: str
    started_at: str
    ended_at: str = ""
    elapsed_s: float = 0.0
    steps: list[StepResult] = field(default_factory=list)
    pass_count: int = 0
    fail_count: int = 0
    run_dir: str = ""

    def add(self, sr: StepResult) -> None:
        self.steps.append(sr)
        if sr.ok:
            self.pass_count += 1
        else:
            self.fail_count += 1

    def to_dict(self) -> dict:
        return asdict(self)


class Runner:
    def __init__(self, tab5: Tab5Driver, scenario_name: str,
                 base_dir: str = "tests/e2e/runs") -> None:
        self.tab5 = tab5
        ts = time.strftime("%Y%m%d-%H%M%S")
        self.run_dir = os.path.join(base_dir, f"{scenario_name}-{ts}")
        os.makedirs(self.run_dir, exist_ok=True)
        self.report = RunReport(scenario=scenario_name,
                                started_at=ts,
                                run_dir=self.run_dir)
        self._step_idx = 0
        self._t0 = time.monotonic()

    # ── Step helpers ──────────────────────────────────────────────
    def step(self, name: str, fn: Callable[[Tab5Driver], None | bool],
             screenshot: bool = True) -> StepResult:
        self._step_idx += 1
        prefix = f"{self._step_idx:02d}_{_slug(name)}"
        print(f"  [{self._step_idx:02d}] {name} ...", end=" ", flush=True)
        t0 = time.monotonic()
        ok = True
        note = ""
        events_before = self.tab5._last_event_ms
        try:
            ret = fn(self.tab5)
            if ret is False:
                ok = False
                note = "step returned False"
        except AssertionError as e:
            ok = False
            note = f"AssertionError: {e}"
        except Exception as e:
            ok = False
            note = f"{type(e).__name__}: {e}\n{traceback.format_exc()}"
        elapsed = time.monotonic() - t0

        shot_path = None
        if screenshot:
            shot_path = os.path.join(self.run_dir, f"{prefix}.jpg")
            try:
                self.tab5.screenshot(shot_path)
                shot_path = os.path.relpath(shot_path, self.run_dir)
            except Exception as e:
                note = (note + f" | screenshot failed: {e}").strip(" |")
                shot_path = None

        # Snapshot events that occurred during the step. Use peek=True
        # so diagnostic capture doesn't advance the cursor — otherwise
        # the next step's await_event() would never see events fired
        # during this step (they'd already be "consumed").
        try:
            evts = self.tab5.events(since_ms=events_before, peek=True)
            evts_dict = [asdict(e) for e in evts]
        except Exception:
            evts_dict = []

        sr = StepResult(name=name, ok=ok, elapsed_s=elapsed,
                        note=note, screenshot=shot_path,
                        events=evts_dict)
        self.report.add(sr)
        symbol = "✓" if ok else "✗"
        print(f"{symbol} ({elapsed:.1f}s)")
        if not ok and note:
            print(f"     {note.splitlines()[0]}")
        return sr

    def soft_step(self, name: str, fn: Callable[[Tab5Driver], None | bool],
                  screenshot: bool = False) -> StepResult:
        """Same as step() but failure isn't counted hard — used for state
        reads / diagnostics."""
        sr = self.step(name, fn, screenshot=screenshot)
        if not sr.ok:
            self.report.fail_count -= 1
            self.report.pass_count += 1
            sr.ok = True
            sr.note = "(soft) " + sr.note
        return sr

    def finish(self) -> RunReport:
        self.report.ended_at = time.strftime("%Y%m%d-%H%M%S")
        self.report.elapsed_s = time.monotonic() - self._t0
        report_path = os.path.join(self.run_dir, "report.json")
        with open(report_path, "w") as f:
            json.dump(self.report.to_dict(), f, indent=2)
        # Markdown summary
        md_path = os.path.join(self.run_dir, "report.md")
        with open(md_path, "w") as f:
            self._write_markdown(f)
        print(f"\n  Report: {report_path}")
        print(f"  Markdown: {md_path}")
        return self.report

    def _write_markdown(self, f) -> None:
        r = self.report
        f.write(f"# Scenario: {r.scenario}\n\n")
        f.write(f"- Started: {r.started_at}\n")
        f.write(f"- Ended: {r.ended_at}\n")
        f.write(f"- Elapsed: {r.elapsed_s:.1f}s\n")
        f.write(f"- Steps: **{r.pass_count} pass, {r.fail_count} fail**\n\n")
        f.write("## Steps\n\n")
        f.write("| # | Step | Result | Elapsed | Screenshot | Events |\n")
        f.write("|---|------|--------|---------|------------|--------|\n")
        for i, s in enumerate(r.steps, 1):
            mark = "PASS" if s.ok else "**FAIL**"
            shot = f"![]({s.screenshot})" if s.screenshot else "-"
            evt_summary = ", ".join(
                f"{e['kind']}({e['detail'][:20]})" if e['detail'] else e['kind']
                for e in s.events[:3]
            ) or "-"
            note = s.note.splitlines()[0] if s.note else ""
            f.write(f"| {i} | {s.name} | {mark} | {s.elapsed_s:.1f}s | "
                    f"{shot} | {evt_summary} |\n")
            if note:
                f.write(f"|   |  | {note} |  |  |  |\n")


def _slug(s: str) -> str:
    out = []
    for ch in s.lower():
        if ch.isalnum():
            out.append(ch)
        elif out and out[-1] != "_":
            out.append("_")
    return "".join(out).strip("_")[:32]


# ════════════════════════════════════════════════════════════════════
# SCENARIOS
# ════════════════════════════════════════════════════════════════════
def story_smoke(r: Runner) -> None:
    """5-min smoke: nav + voice + camera basics."""
    tab5 = r.tab5

    # Bring up known state
    r.step("Boot reachable", lambda t: t.wait_alive(60))
    r.step("Reset event cursor", lambda t: t.reset_event_cursor() and None)
    r.step("Force voice mode = Local (0)",
           lambda t: t.mode(0).get("ok") is not False)
    r.step("Voice ready",
           lambda t: t.await_voice_state("READY", 20))
    r.step("Home screen visible",
           lambda t: t.screen()["current"] in ("home", ""))

    # Send a text chat (works without speaker, fast feedback)
    r.step("Send chat: 'what time is it?'",
           lambda t: t.chat({"text": "what time is it?"} if False
                            else "what time is it?")
                     and True)
    r.step("Wait for LLM done (Local mode, can take 60-180s)",
           lambda t: t.await_llm_done(timeout_s=180) is not None)

    # Navigate to camera, verify, take a frame
    r.step("Navigate to Camera",
           lambda t: t.navigate("camera").get("navigated") == "camera")
    time.sleep(2)
    r.step("Screen reports camera",
           lambda t: t.screen()["current"] == "camera")
    r.step("Capture a camera frame",
           lambda t: len(t.camera_frame()) > 1000)

    # Back to home
    r.step("Navigate home",
           lambda t: t.navigate("home").get("navigated") == "home")

    # Settings
    r.step("Navigate Settings",
           lambda t: t.navigate("settings").get("navigated") == "settings")
    time.sleep(2)
    r.step("Read settings JSON",
           lambda t: "wifi_ssid" in t.settings())
    r.step("Navigate home",
           lambda t: t.navigate("home").get("navigated") == "home")


def story_full(r: Runner) -> None:
    """10-min full-feature: modes + chat + photo+vision + record + call."""
    tab5 = r.tab5
    r.step("Boot reachable", lambda t: t.wait_alive(60))
    r.step("Reset event cursor", lambda t: t.reset_event_cursor() and None)

    # ── Mode tour ─────────────────────────────────────────────
    for mode in (0, 1, 0, 2, 0):  # Local, Hybrid, Local, Cloud, Local
        r.step(f"Switch to voice_mode={mode}",
               lambda t, m=mode: t.mode(m).get("ok") is not False,
               screenshot=False)
        time.sleep(2)
    r.step("Voice still ready after mode rotation",
           lambda t: t.voice_state().get("connected", False))

    # ── Local text turn ───────────────────────────────────────
    r.step("Local: send chat 'hello'",
           lambda t: t.chat("hello") and True, screenshot=False)
    r.step("Local: await llm_done (≤180s)",
           lambda t: t.await_llm_done(timeout_s=180) is not None)

    # ── Camera capture ────────────────────────────────────────
    r.step("Navigate Camera",
           lambda t: t.navigate("camera").get("navigated") == "camera")
    time.sleep(3)
    r.step("Camera frame size > 5KB",
           lambda t: len(t.camera_frame()) > 5000)

    # Tap the shutter button (around 360,1100 — center capture circle)
    r.step("Tap shutter (capture photo)",
           lambda t: t.tap(360, 1100) and True)
    r.step("Await camera.capture event",
           lambda t: t.await_event("camera.capture", timeout_s=10) is not None)

    # ── Video record cycle ────────────────────────────────────
    r.step("Tap REC button (470,1100)",
           lambda t: t.tap(470, 1100) and True)
    r.step("Await camera.record_start",
           lambda t: t.await_event("camera.record_start",
                                   timeout_s=8) is not None)
    r.step("Record 6 seconds", lambda t: time.sleep(6) or True,
           screenshot=False)
    r.step("Tap REC again (stop)",
           lambda t: t.tap(470, 1100) and True)
    r.step("Await camera.record_stop",
           lambda t: t.await_event("camera.record_stop",
                                   timeout_s=8) is not None)

    # ── Back to home, exit camera ─────────────────────────────
    r.step("Navigate home",
           lambda t: t.navigate("home").get("navigated") == "home")

    # ── Mode swap to Cloud and ask vision-capable question ────
    # (skipped unless OPENROUTER key present + Cloud key works)
    r.step("Switch to Cloud mode (2)",
           lambda t: t.mode(2).get("ok") is not False, screenshot=False)
    time.sleep(3)
    r.step("Cloud: send chat 'what is 9+10?'",
           lambda t: t.chat("what is 9+10?") and True, screenshot=False)
    r.step("Cloud: await llm_done (≤30s)",
           lambda t: t.await_llm_done(timeout_s=30) is not None)

    # Back to Local
    r.step("Switch back to Local (0)",
           lambda t: t.mode(0).get("ok") is not False, screenshot=False)


def story_stress(r: Runner) -> None:
    """20-min stress: rapid nav + voice + record cycles + mode swaps."""
    tab5 = r.tab5
    r.step("Boot reachable", lambda t: t.wait_alive(60))
    r.step("Heap baseline (internal largest > 20 KB)",
           lambda t: t.heap().get("internal", {}).get("largest_kb", 0) > 20,
           screenshot=False)
    r.step("Reset event cursor",
           lambda t: t.reset_event_cursor() and None, screenshot=False)

    cycle_count = 6  # ~3 minutes per cycle = 18 minutes total
    for cycle in range(1, cycle_count + 1):
        print(f"\n== Stress cycle {cycle}/{cycle_count} ==")
        # Mode rotation
        for mode in (0, 2, 0):
            r.step(f"C{cycle}: voice_mode={mode}",
                   lambda t, m=mode: t.mode(m).get("ok") is not False,
                   screenshot=False)
            time.sleep(1.5)
        # Nav rotation through screens
        for screen in ("camera", "settings", "home", "notes", "home"):
            r.step(f"C{cycle}: nav={screen}",
                   lambda t, s=screen: t.navigate(s).get("navigated") == s,
                   screenshot=False)
            time.sleep(1)
        # Quick text chat (Local mode)
        r.step(f"C{cycle}: text chat",
               lambda t: t.chat(f"cycle {cycle} ping") and True,
               screenshot=False)
        r.step(f"C{cycle}: await llm_done (≤180s)",
               lambda t: t.await_llm_done(timeout_s=180) is not None,
               screenshot=False)
        # One screenshot per cycle
        r.step(f"C{cycle}: screenshot heartbeat",
               lambda t: True, screenshot=True)
        # Heap check (internal SRAM largest free block, in KB)
        r.step(f"C{cycle}: heap healthy (largest > 15 KB)",
               lambda t: t.heap().get("internal", {}).get("largest_kb", 0) > 15,
               screenshot=False)

    # Final state
    r.step("Final voice connected",
           lambda t: t.voice_state().get("connected", False))
    r.step("Final heap > 20 KB largest internal block",
           lambda t: t.heap().get("internal", {}).get("largest_kb", 0) > 20,
           screenshot=False)


SCENARIOS: dict[str, Callable[[Runner], None]] = {
    "story_smoke":   story_smoke,
    "story_full":    story_full,
    "story_stress":  story_stress,
}


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("scenario", choices=list(SCENARIOS.keys()) + ["all"])
    p.add_argument("--base-url", default=os.environ.get(
        "TAB5_URL", "http://192.168.1.90:8080"))
    p.add_argument("--token",
                   default=os.environ.get("TAB5_TOKEN",
                                          "05eed3b13bf62d92cfd8ac424438b9f2"))
    p.add_argument("--reboot", action="store_true",
                   help="Reboot Tab5 before running (clean state)")
    args = p.parse_args()

    tab5 = Tab5Driver(base_url=args.base_url, token=args.token)
    if args.reboot:
        print("Rebooting Tab5…")
        tab5.reboot()
        time.sleep(20)

    scenarios = (SCENARIOS.keys() if args.scenario == "all"
                 else [args.scenario])
    overall_fail = 0
    for name in scenarios:
        print(f"\n══════ {name} ══════")
        runner = Runner(tab5, name)
        try:
            SCENARIOS[name](runner)
        except KeyboardInterrupt:
            print("Aborted.")
            runner.finish()
            return 130
        except Exception as e:
            print(f"\nScenario crashed: {e}")
            traceback.print_exc()
        report = runner.finish()
        print(f"  → {report.pass_count} PASS, {report.fail_count} FAIL "
              f"in {report.elapsed_s:.0f}s")
        overall_fail += report.fail_count

    return 1 if overall_fail else 0


if __name__ == "__main__":
    sys.exit(main())
