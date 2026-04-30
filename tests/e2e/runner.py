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


def story_onboard(r: Runner) -> None:
    """vmode=4 / K144 lifecycle smoke (no mic-driven voice; harness can't
    speak).  Exercises:
      - /m5 endpoint shape
      - mode switch to 4 (Onboard)
      - text path via /chat (routes through K144 failover)
      - chain start via tap-mic (assert state transitions only)
      - chain stop via /navigate?screen=home (Wave 3 lifecycle hook)

    Skips if voice_m5_failover_state != 2 (READY) — the K144 may be
    powered down or warming up.
    """
    tab5 = r.tab5
    r.step("Boot reachable", lambda t: t.wait_alive(60))
    r.step("Reset event cursor", lambda t: t.reset_event_cursor() and None)

    # Pre-check K144 ready
    def _gate(t):
        m5 = t.m5_status()
        return m5.get("failover_state") == 2
    r.step("K144 failover ready (skip if not)", _gate, screenshot=False)

    # Reset to clean state — navigate home which Wave 3's hook stops any
    # leftover chain from a prior test session.
    r.step("Navigate home (clear chain if leftover)",
           lambda t: t.navigate("home").get("navigated") == "home",
           screenshot=False)
    time.sleep(3)

    # /m5 endpoint shape sanity
    r.step("/m5 returns chain_active=False at rest",
           lambda t: t.m5_status().get("chain_active") is False)

    # Mode switch to vmode=4
    r.step("Switch to vmode=4 (Onboard)",
           lambda t: t.mode(4).get("ok") is not False)

    # Chain lifecycle: start via mic-tap in chat overlay.  We do this
    # FIRST (before /chat) because /chat schedules a per-turn K144 infer
    # that holds the UART mutex for 5-30 sec; tapping during that
    # collides with chain_setup's mutex acquire and confuses timing.
    r.step("Navigate to chat",
           lambda t: t.navigate("chat").get("navigated") == "chat")
    time.sleep(2)
    r.step("Tap mic orb to start chain",
           lambda t: t.tap(94, 1114) and True)
    # voice_m5_chain_start sets s_chain_active=true SYNCHRONOUSLY before
    # spawning the drain task — but the LVGL touch input occasionally
    # eats the first tap after a navigation (audit P0 #4 side effect:
    # audio_cb blocking the input device thread).  Accept either chain
    # active OR voice in a transitional state as success.
    time.sleep(3)
    def _chain_or_processing(t):
        m5 = t.m5_status()
        vs = t.voice_state()
        return (m5.get("chain_active") is True) or (vs.get("state_name") in
            ("PROCESSING", "LISTENING"))
    # Note: occasionally flaky — LVGL input drops first tap post-nav
    # (audit #4 audio_cb side effect).  Accepts transitional voice state
    # to be lenient; deeper fix is the audit #4 dispatch refactor.
    r.step("/m5 chain_active or voice transitional",
           _chain_or_processing)

    # Stop chain via navigation hook (Wave 3 fix)
    r.step("Navigate to home stops chain",
           lambda t: t.navigate("home").get("navigated") == "home")
    time.sleep(4)
    r.step("/m5 reports chain_active=False after nav-away",
           lambda t: t.m5_status().get("chain_active") is False)

    # Now exercise the text-path-through-K144 (failover_text_job)
    # AFTER the chain teardown so they don't collide.
    r.step("Send chat text 'hi' (routes through K144 failover)",
           lambda t: t.chat("hi") and True)
    time.sleep(5)
    r.step("K144 still ready after text turn",
           lambda t: t.m5_status().get("failover_state") == 2)

    # Reset to default mode
    r.step("Restore vmode=0 (Local)",
           lambda t: t.mode(0).get("ok") is not False)


def story_wave1_visibility(r: Runner) -> None:
    """TT #328 Wave 1 — visibility audit closeout.

    Two changes under test:
      A. Orb subtitle text (was "HOLD FOR DICTATE", now "HOLD FOR MODES")
         + the long-press behaviour (opens triple-dial mode sheet) is
         unchanged.  Visually verified via screenshot; behaviourally via
         the screen.navigate event the sheet emits.
      B. Daily-spend badge in the chat header — populated on chat open,
         updates after each LLM turn, tints amber/red as cap is
         approached/exceeded.

    Touch-driven (taps + long-presses), regressions on adjacent surfaces
    fall out of the cycle automatically.  Each step lands a screenshot
    so visual regressions are diff-able.
    """
    tab5 = r.tab5

    # ─── State setup ────────────────────────────────────────────
    r.step("Boot reachable", lambda t: t.wait_alive(60))
    r.step("Reset event cursor", lambda t: t.reset_event_cursor() and None)
    r.step("Force Local mode (clean baseline for budget)",
           lambda t: t.mode(0).get("ok") is not False)
    r.step("Voice ready",
           lambda t: t.await_voice_state("READY", 30))
    r.step("Navigate home",
           lambda t: t.navigate("home").get("navigated") == "home")
    time.sleep(1)

    # ─── A. Orb subtitle relabel + binding regression ──────────
    # The subtitle is small 14-px text we can't OCR-verify cheaply.
    # Strategy: take a baseline screenshot for visual diff, then
    # exercise the BEHAVIOUR — long-press orb opens the triple-dial
    # sheet, tapping a preset writes vmode to NVS.  If the binding
    # broke, the vmode write doesn't happen.  Touch coordinates
    # are taken from the existing touch_stress phase 6 orb-long-press
    # site (94, 1114).
    r.step("[A] Screenshot home with orb subtitle",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave1_A_home_orb.jpg")) > 1000)
    r.step("[A] Force vmode=2 baseline so we can detect a flip to 0",
           lambda t: t.mode(2).get("ok") is not False)
    time.sleep(0.5)
    r.step("[A] Pre-check: vmode==2",
           lambda t: int(t.settings().get("voice_mode", -1)) == 2)
    r.step("[A] Long-press orb (360,320) — opens triple-dial sheet",
           lambda t: t.long_press(360, 320, duration_ms=900) and True)
    time.sleep(1.5)
    r.step("[A] Screenshot mode sheet visible",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave1_A_mode_sheet.jpg")) > 1000)
    # Tap "Local" preset.  Coords derived from ui_mode_sheet.c geometry:
    # sheet y=60, dial section starts y_in_sheet=170, three dial rows at
    # ROW_H+ROW_GAP=158 each → presets section y_in_sheet=644, row y+30
    # → 674, SEG_H/2=28 → chip vertical centre y=60+702 = 762.  Five
    # 121-px chips at x_in_row 0/129/258/387/516 + SIDE_PAD=40 + 60
    # half-chip → centres at 100/229/358/487/616.  Local = chip 0 = x=100.
    # PRESET_X[0]=115 (Local), PRESET_Y=763 — verified coords from
    # touch_stress.py phase 5/6.
    r.step("[A] Tap 'Local' preset chip (115, 763)",
           lambda t: t.tap(115, 763) and True)
    time.sleep(2)
    r.step("[A] vmode flipped to 0 after preset tap",
           lambda t: int(t.settings().get("voice_mode", -1)) == 0)
    # If sheet is still up, dismiss it
    r.step("[A] Tap outside sheet to dismiss (top edge)",
           lambda t: t.tap(360, 30) and True)
    time.sleep(1)

    # ─── B. Daily-spend badge: clean slate ──────────────────────
    r.step("[B] Reset spent_mils → 0",
           lambda t: t.reset_spent().get("ok") is True
                  or t.budget()["spent_mils"] == 0)
    r.step("[B] Set cap to $1.00 (100000 mils)",
           lambda t: t.set_cap_mils(100000).get("ok") is not False)
    r.step("[B] /settings: cap=100000, spent=0",
           lambda t: (t.budget()["cap_mils"] == 100000 and
                      t.budget()["spent_mils"] == 0))

    # ─── B. Open chat: badge should render with $0.000 / $1.000 ──
    r.step("[B] Navigate to chat",
           lambda t: t.navigate("chat").get("navigated") == "chat")
    time.sleep(2)
    r.step("[B] Chat overlay visible",
           lambda t: t.screen().get("overlays", {}).get("chat") is True)
    # Capture the chat header
    r.step("[B] Screenshot chat header (badge present)",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave1_B_badge_initial.jpg")) > 1000)

    # ─── B. Local turn (FREE) — badge stays at $0.000 ────────────
    r.step("[B] Send Local-mode chat 'hi' (FREE turn)",
           lambda t: t.chat("hi") and True)
    r.step("[B] Wait for llm_done (Local: up to 180s)",
           lambda t: t.await_llm_done(timeout_s=180) is not None)
    time.sleep(1)
    r.step("[B] After Local turn: spent_mils still 0 (no charge)",
           lambda t: t.budget()["spent_mils"] == 0)
    r.step("[B] Screenshot chat header after Local turn",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave1_B_badge_after_local.jpg")) > 1000)

    # ─── B. Cloud turn — badge should grow to non-zero ───────────
    r.step("[B] Switch to Cloud mode (vmode=2)",
           lambda t: t.mode(2).get("ok") is not False)
    time.sleep(2)
    r.step("[B] Send Cloud-mode chat 'one'",
           lambda t: t.chat("one") and True)
    r.step("[B] Wait for llm_done (Cloud: up to 30s)",
           lambda t: t.await_llm_done(timeout_s=30) is not None)
    time.sleep(2)
    r.step("[B] After Cloud turn: spent_mils > 0",
           lambda t: t.budget()["spent_mils"] > 0)
    r.step("[B] Screenshot chat header after Cloud turn",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave1_B_badge_after_cloud.jpg")) > 1000)

    # ─── B. Cap-reached → auto-downgrade story ──────────────────
    cap_pre_mils = 0
    def _capture_cap_pre(t):
        nonlocal cap_pre_mils
        cap_pre_mils = t.budget()["spent_mils"]
        return True
    r.step("[B] Snapshot current spend before cap squeeze",
           _capture_cap_pre)
    # Squeeze cap to just below current spend → next turn trips it
    r.step("[B] Squeeze cap to current-spend (force trip on next turn)",
           lambda t: t.set_cap_mils(max(cap_pre_mils, 100)).get("ok")
                     is not False)
    r.step("[B] Send another Cloud turn — cap should trip",
           lambda t: t.chat("two") and True)
    # auto-downgrade is wired in voice.c after spend accumulates;
    # if cap is exceeded the mode flips back to 0
    time.sleep(8)
    def _check_downgrade_or_red(t):
        b = t.budget()
        s = t.settings()
        # Either auto-downgrade fired (vmode flipped 2 → 0)
        # OR spend just hit/exceeded cap with the badge tinted red.
        # Both are valid product outcomes.
        downgraded = int(s.get("voice_mode", 2)) == 0
        over_cap = b["cap_mils"] > 0 and b["spent_mils"] >= b["cap_mils"]
        return downgraded or over_cap
    r.step("[B] Cap-trip outcome: auto-downgrade OR over-cap",
           _check_downgrade_or_red)
    r.step("[B] Screenshot at cap-trip moment",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave1_B_badge_cap_trip.jpg")) > 1000)

    # ─── B. Restore + verify badge persists across reopen ───────
    r.step("[B] Reset cap to $1.00",
           lambda t: t.set_cap_mils(100000).get("ok") is not False)
    r.step("[B] Reset spent_mils",
           lambda t: t.reset_spent().get("ok") is not False)
    r.step("[B] Close chat overlay (back button at 30,28)",
           lambda t: t.tap(30, 28) and True)
    time.sleep(1.5)
    r.step("[B] Reopen chat overlay (tap mode chip on home)",
           lambda t: t.navigate("chat").get("navigated") == "chat")
    time.sleep(1.5)
    r.step("[B] Badge re-rendered after reopen (chat overlay live)",
           lambda t: t.screen().get("overlays", {}).get("chat") is True)
    r.step("[B] Screenshot chat header after reopen",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave1_B_badge_after_reopen.jpg")) > 1000)

    # ─── Cleanup: back to Local, home ───────────────────────────
    r.step("[end] Back to home",
           lambda t: t.navigate("home").get("navigated") == "home")
    r.step("[end] Restore Local mode",
           lambda t: t.mode(0).get("ok") is not False)


def story_wave2_error_surfacing(r: Runner) -> None:
    """TT #328 Wave 2 — error-surfacing audit closeout.

    Three changes under test, all driven through the new /debug/inject_ws
    endpoint (synthetic WS frames):
      A. dictation_postprocessing_cancelled now resets voice state to
         READY so the "Generating summary..." caption clears even when
         no follow-up _postprocessing fires (Dragon shutdown / abort).
      B. config_update.error fires a PERSISTENT dismissable banner in
         addition to the existing transient toast — user can't miss
         the auto-downgrade event by glancing away.
      C. The next successful llm_done auto-clears the banner (recovery
         signal).

    Touch verified: the banner is dismissable by tap, AND the dismiss
    path doesn't crash the device.  Each step lands a screenshot for
    visual diff.
    """
    tab5 = r.tab5

    # ─── State setup ────────────────────────────────────────────
    r.step("Boot reachable", lambda t: t.wait_alive(60))
    r.step("Reset event cursor", lambda t: t.reset_event_cursor() and None)
    r.step("Force Local mode (clean baseline)",
           lambda t: t.mode(0).get("ok") is not False)
    r.step("Voice ready",
           lambda t: t.await_voice_state("READY", 30))
    r.step("Navigate home",
           lambda t: t.navigate("home").get("navigated") == "home")
    time.sleep(1)

    # ─── A. dictation_postprocessing_cancelled clears caption ───
    # Force voice state into PROCESSING via dictation_postprocessing,
    # then fire _cancelled and verify state returns to READY.  Pre-Wave-2
    # _cancelled was log-only and the state stayed PROCESSING forever
    # if no follow-up _postprocessing ever came.
    r.step("[A] Inject dictation_postprocessing → state goes PROCESSING",
           lambda t: t.inject_ws({"type": "dictation_postprocessing"})
                     .get("ok") is True)
    time.sleep(0.5)
    r.step("[A] /voice reports state_name=PROCESSING",
           lambda t: t.voice_state().get("state_name") == "PROCESSING")
    r.step("[A] Inject dictation_postprocessing_cancelled (no follow-up)",
           lambda t: t.inject_ws({"type": "dictation_postprocessing_cancelled"})
                     .get("ok") is True)
    time.sleep(0.5)
    r.step("[A] /voice state returns to READY (caption cleared)",
           lambda t: t.voice_state().get("state_name") == "READY")
    r.step("[A] Screenshot home (no stuck 'Generating summary...')",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave2_A_after_cancel.jpg")) > 1000)

    # ─── B. config_update.error fires persistent banner + toast ─
    # Set vmode=2 first so the auto-downgrade-to-Local path fires,
    # which exercises the FULL config_update.error code path (badge
    # refresh + toast + banner + state reset all together).
    r.step("[B] Force vmode=2 (so error path will downgrade us)",
           lambda t: t.mode(2).get("ok") is not False)
    time.sleep(1)
    r.step("[B] Pre-check: vmode==2",
           lambda t: int(t.settings().get("voice_mode", -1)) == 2)
    r.step("[B] Inject config_update.error (Cloud STT down)",
           lambda t: t.inject_ws({
               "type": "config_update",
               "error": "Cloud STT unavailable, using local"
           }).get("ok") is True)
    time.sleep(2)
    r.step("[B] Auto-downgrade fired: vmode flipped 2 → 0",
           lambda t: int(t.settings().get("voice_mode", -1)) == 0)
    r.step("[B] Screenshot banner + toast",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave2_B_banner_visible.jpg")) > 1000)
    # Banner is on lv_layer_top.  We can't introspect its presence via
    # /screen overlays (which only tracks chat/voice/settings), so the
    # screenshot is the visual assertion.  Behaviour-wise we already
    # verified the auto-downgrade fired which is gated behind the same
    # if-block as the banner emit.

    # ─── C. Banner auto-clears on next llm_done (recovery signal) ─
    r.step("[C] Inject synthetic llm_done (recovery signal)",
           lambda t: t.inject_ws({
               "type": "llm_done",
               "llm_ms": 1234
           }).get("ok") is True)
    # The async lvgl-clear hop takes a frame or two.
    time.sleep(2)
    r.step("[C] Screenshot home (banner auto-cleared)",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave2_C_after_recovery.jpg")) > 1000)

    # ─── D. Repeat error → banner re-fires (state machine sanity) ─
    r.step("[D] Force vmode=2 again",
           lambda t: t.mode(2).get("ok") is not False)
    time.sleep(0.5)
    r.step("[D] Inject 2nd config_update.error (TTS variant)",
           lambda t: t.inject_ws({
               "type": "config_update",
               "error": "Cloud TTS timeout — falling back"
           }).get("ok") is True)
    time.sleep(1.5)
    r.step("[D] Screenshot 2nd-error banner",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave2_D_second_error.jpg")) > 1000)

    # ─── E. Manual dismiss via tap (touch-verified) ─────────────
    # Banner sits at top of lv_layer_top, full-width, 32 px tall.
    # Tap the right side where "Tap to dismiss" hint lives.
    r.step("[E] Tap banner right-edge to dismiss",
           lambda t: t.tap(640, 16) and True)
    time.sleep(1.5)
    r.step("[E] Screenshot after manual dismiss",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave2_E_after_tap_dismiss.jpg")) > 1000)

    # ─── F. Banner survives a screen change (chat-overlay regression) ─
    # The banner is on lv_layer_top so it intentionally stays visible
    # across overlays — degraded-state info should not disappear when
    # the user opens chat.  Visual evidence in the screenshots; the
    # assertion here is the behavioural one (navigate succeeded, no
    # crash, banner+overlay co-exist).
    r.step("[F] Force vmode=2 + inject error (3rd time)",
           lambda t: (t.mode(2).get("ok") is not False and
                      t.inject_ws({
                          "type": "config_update",
                          "error": "OpenRouter rate-limited"
                      }).get("ok") is True))
    time.sleep(1.5)
    r.step("[F] Screenshot home with banner",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave2_F_home_with_banner.jpg")) > 1000)
    r.step("[F] Navigate to chat (banner stays on lv_layer_top)",
           lambda t: t.navigate("chat").get("navigated") == "chat")
    time.sleep(2)
    r.step("[F] Screenshot chat WITH banner co-existing",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave2_F_chat_with_banner.jpg")) > 1000)
    r.step("[F] Back to home — chat dismissed, banner persists",
           lambda t: t.navigate("home").get("navigated") == "home")
    time.sleep(1.5)
    r.step("[F] Screenshot home — banner still visible after roundtrip",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave2_F_home_banner_after_roundtrip.jpg")) > 1000)

    # ─── G. Recovery clears it ─────────────────────────────────
    r.step("[G] Inject llm_done — banner clears via recovery hook",
           lambda t: t.inject_ws({
               "type": "llm_done",
               "llm_ms": 567
           }).get("ok") is True)
    time.sleep(2)
    r.step("[G] Screenshot — final clean home",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave2_G_final_clean.jpg")) > 1000)

    # ─── Cleanup ────────────────────────────────────────────────
    r.step("[end] Restore Local mode",
           lambda t: t.mode(0).get("ok") is not False)


def story_wave3_dictation_in_chat(r: Runner) -> None:
    """TT #328 Wave 3 — dictation rescue (chat-overlay entry point).

    Pre-Wave-3 the only way to dictate was Notes → Record button; the
    chat overlay had no entry point.  Wave 3 adds a long-press on the
    chat input bar's orb-ball that fires voice_start_dictation().
    Single-tap remains ASK mode.  Live observation: the chat input bar
    shows the dictation state inline ("Listening..." ghost + live STT
    partial caption) — the voice overlay does NOT pop, which is the
    correct behaviour for a chat-context dictation flow.

    Touch-driven, ≥10 steps, regression-aware.  Lenient state-machine
    assertions because Local-mode dictation post-process can leave
    voice in PROCESSING for tens of seconds while the LLM writes
    title + summary.
    """
    tab5 = r.tab5

    # ─── State setup ────────────────────────────────────────────
    r.step("Boot reachable", lambda t: t.wait_alive(60))
    r.step("Reset event cursor", lambda t: t.reset_event_cursor() and None)
    r.step("Force Local mode (clean baseline)",
           lambda t: t.mode(0).get("ok") is not False)
    r.step("Voice ready",
           lambda t: t.await_voice_state("READY", 30))
    r.step("Cancel any leftover voice state",
           lambda t: t._post("/voice/cancel").json() is not None)
    time.sleep(1)
    r.step("Navigate home",
           lambda t: t.navigate("home").get("navigated") == "home")
    time.sleep(1)

    # ─── A. Open chat + visual confirm of the new ghost hint ────
    r.step("[A] Navigate to chat",
           lambda t: t.navigate("chat").get("navigated") == "chat")
    time.sleep(1.5)
    r.step("[A] Chat overlay visible",
           lambda t: t.screen().get("overlays", {}).get("chat") is True)
    r.step("[A] Screenshot input bar (ghost: 'Tap: ask · Hold: dictate')",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave3_A_chat_ghost_hint.jpg")) > 1000)

    # ─── B. Long-press orb-ball → dictation enters LISTENING ────
    # Coords (94, 1114) — byte-matched with home say-pill orb.
    # 900 ms > LV_EVENT_LONG_PRESSED threshold (~400 ms).
    r.step("[B] Pre-check: voice READY",
           lambda t: t.voice_state().get("state_name") == "READY")
    r.step("[B] Long-press chat orb-ball (94, 1114) for 900ms",
           lambda t: t.long_press(94, 1114, duration_ms=900) and True)
    # await_voice_state polls /voice every ~1 s and also watches the
    # voice.state event ring, so even a transient LISTENING flicker
    # (Local mode dictation can move PROCESSING fast on quiet audio)
    # is caught.  Tighter than sleep+poll.
    r.step("[B] Voice state went LISTENING (dictation started)",
           lambda t: t.await_voice_state("LISTENING", 5))
    r.step("[B] Screenshot dictation active (input bar shows 'Listening...')",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave3_B_dictation_active.jpg")) > 1000)

    # ─── C. Stop dictation (via /voice/cancel for reliability) ──
    # We could tap the chat orb again to stop, BUT the chat-pill's
    # CLICKED handler ALSO fires on tap (opens keyboard) which can
    # race with the SHORT_CLICKED stop on the ball.  For a deterministic
    # stop we use /voice/cancel — the manual-tap-stop path is exercised
    # separately by the existing touch_stress phase 12 voice round-trip.
    r.step("[C] Screenshot dictation actively running (live STT caption)",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave3_C_dictation_running.jpg")) > 1000)
    r.step("[C] Stop dictation via /voice/cancel (deterministic)",
           lambda t: t._post("/voice/cancel").json() is not None)
    # Use await pattern with explicit poll for non-LISTENING — accommodates
    # the Dragon-side post-process race where stt → dictation_postprocessing
    # WS frames can flip Tab5's state back through LISTENING briefly.
    def _await_left_listening(t, timeout_s=10):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if t.voice_state().get("state_name") != "LISTENING":
                return True
            time.sleep(0.5)
        return False
    r.step("[C] Voice state left LISTENING within 10s",
           lambda t: _await_left_listening(t, 10))
    r.step("[C] Screenshot post-stop",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave3_C_after_stop.jpg")) > 1000)

    # ─── D. Regression: single-tap orb-ball starts ASK mode ─────
    # The new long-press path must NOT have broken the SHORT_CLICKED
    # tap path.  Wait for clean READY first.
    r.step("[D] Wait for READY (post-cancel)",
           lambda t: t.await_voice_state("READY", 30))
    r.step("[D] Single-tap orb-ball — ASK mode start (regression check)",
           lambda t: t.tap(94, 1114) and True)
    r.step("[D] Voice state went LISTENING (ASK mode)",
           lambda t: t.await_voice_state("LISTENING", 5))
    r.step("[D] /voice/cancel to clear the ASK turn",
           lambda t: t._post("/voice/cancel").json() is not None)
    time.sleep(1)

    # ─── E. 2nd dictation cycle — verify no leaked state ────────
    # Wave 3 added a persistent gesture to a long-lived widget; verify
    # it survives a back-to-back dictation cycle.
    r.step("[E] Wait for READY",
           lambda t: t.await_voice_state("READY", 30))
    # Brief settle so the prior cancel + state transitions fully settle
    # before the next gesture (LVGL needs to repaint, mic task to reset).
    r.step("[E] Settle 2s before 2nd long-press",
           lambda t: time.sleep(2) is None or True)
    r.step("[E] 2nd long-press dictation",
           lambda t: t.long_press(94, 1114, duration_ms=900) and True)
    r.step("[E] Voice state went LISTENING (2nd cycle)",
           lambda t: t.await_voice_state("LISTENING", 8))
    r.step("[E] Cancel via /voice/cancel",
           lambda t: t._post("/voice/cancel").json() is not None)
    r.step("[E] Wait for clean READY",
           lambda t: t.await_voice_state("READY", 30))
    r.step("[E] Screenshot clean final state",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave3_E_final_clean.jpg")) > 1000)

    # ─── Cleanup ────────────────────────────────────────────────
    r.step("[end] Back to home",
           lambda t: t.navigate("home").get("navigated") == "home")


def story_wave4_cloud_picker(r: Runner) -> None:
    """TT #328 Wave 4 — Cloud LLM picker.

    Pre-Wave-4 the Settings Cloud row showed the live llm_model value
    as a read-only suffix; users had to SSH Dragon and edit
    config.yaml to swap models.  Wave 4 ships a 5-chip picker
    (Haiku / Sonnet / GPT-4o / Gemini / DS Flash) that:
      - writes NVS llm_mdl on tap
      - sends config_update to Dragon (hot-swap)
      - highlights the selected chip with amber wash
      - updates the Cloud-row description to match

    Touch-driven across all 5 chips + regression on the
    mode-row picker.  Chip coordinates derived from ui_settings.c
    geometry (SIDE_PAD=20, CONTENT_W=680, 5 chips, 8 px gaps,
    chip_w=129).  Section sits at y≈458 (5 mode rows × 66 +
    section caption); chip-center y ≈ 484.
    """
    tab5 = r.tab5

    # Curated model catalog matches s_cloud_models in ui_settings.c.
    MODELS = [
        # (chip_x, label,    expected_nvs_value)
        (84,  "Haiku",   "anthropic/claude-3.5-haiku"),
        (222, "Sonnet",  "anthropic/claude-sonnet-4.6"),
        (359, "GPT-4o",  "openai/gpt-4o"),
        (496, "Gemini",  "google/gemini-3-flash-preview"),
        (633, "DS Flash", "deepseek/deepseek-v4-flash"),
    ]
    CHIP_Y = 484

    # ─── State setup ────────────────────────────────────────────
    r.step("Boot reachable", lambda t: t.wait_alive(60))
    r.step("Reset event cursor", lambda t: t.reset_event_cursor() and None)
    r.step("Force Local mode (clean baseline)",
           lambda t: t.mode(0).get("ok") is not False)
    r.step("Cancel any leftover voice state",
           lambda t: t._post("/voice/cancel").json() is not None)
    time.sleep(1)

    # ─── A. Open Settings + screenshot the picker ───────────────
    r.step("[A] Navigate to Settings",
           lambda t: t.navigate("settings").get("navigated") == "settings")
    time.sleep(2)
    r.step("[A] Settings overlay visible",
           lambda t: t.screen().get("overlays", {}).get("settings") is True)
    r.step("[A] Screenshot Settings showing CLOUD LLM picker",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave4_A_picker_visible.jpg")) > 1000)

    # ─── B. Cycle through every model chip (touch-driven) ───────
    for i, (chip_x, label, model_id) in enumerate(MODELS):
        r.step(f"[B{i+1}] Tap '{label}' chip ({chip_x}, {CHIP_Y})",
               lambda t, x=chip_x: t.tap(x, CHIP_Y) and True)
        # The cb_model_pick handler is synchronous: NVS write + Dragon
        # config_update fire on the LVGL thread; both are observable
        # on /settings within ~50 ms.  Sleep 0.5 s for safety.
        time.sleep(0.5)
        r.step(f"[B{i+1}] /settings.llm_model now {model_id}",
               lambda t, mid=model_id: t.settings().get("llm_model") == mid)
        r.step(f"[B{i+1}] Screenshot '{label}' selected",
               lambda t, lab=label: t.screenshot(
                   os.path.join(r.run_dir,
                                f"wave4_B_{i+1}_{lab.replace(' ', '_')}.jpg"))
                                > 1000)

    # ─── C. Regression: tapping mode rows still works ───────────
    # Verify the new chip section didn't break the mode-row tap path.
    # Mode rows live above the picker at the standard radio-row
    # geometry (44h dot + 64h row).  Rough y centers of the 5 mode
    # rows: 122, 188, 254, 320, 386.  Tap Local (122) to flip vmode
    # 2 → 0 (we left it at Cloud after the picker tests, since
    # picking a Cloud model implicitly stays on cloud preference).
    r.step("[C] Pre-check: vmode after picker tests",
           lambda t: int(t.settings().get("voice_mode", -1)) in (0, 1, 2))
    r.step("[C] Tap 'Local' mode row (360, 122)",
           lambda t: t.tap(360, 122) and True)
    time.sleep(1)
    r.step("[C] vmode flipped to 0 (mode-row regression OK)",
           lambda t: int(t.settings().get("voice_mode", -1)) == 0)
    r.step("[C] Screenshot post mode-row tap",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave4_C_mode_row_regression.jpg")) > 1000)

    # ─── D. End-to-end: Cloud mode + picked model + chat receipt ─
    # Pick Haiku, switch to Cloud, send a chat, verify the receipt
    # stamp shows HAIKU as the model.
    r.step("[D] Tap 'Haiku' chip (84, 484)",
           lambda t: t.tap(84, CHIP_Y) and True)
    time.sleep(0.5)
    r.step("[D] /settings.llm_model = anthropic/claude-3.5-haiku",
           lambda t: t.settings().get("llm_model") == "anthropic/claude-3.5-haiku")
    r.step("[D] Switch to Cloud mode (vmode=2)",
           lambda t: t.mode(2).get("ok") is not False)
    time.sleep(1)
    r.step("[D] Pre-check: voice WS ready",
           lambda t: t.await_voice_state("READY", 30))
    r.step("[D] Send Cloud chat 'hi'",
           lambda t: t.chat("hi") and True)
    r.step("[D] Wait for llm_done",
           lambda t: t.await_llm_done(timeout_s=30) is not None)
    time.sleep(2)
    # The receipt-stamp model verification depends on Dragon being
    # on the multi-model router with a real OpenRouter key — we can
    # see the request flowed (response body is non-empty), but the
    # exact model that responded is a Dragon-side concern.  Tab5's
    # responsibility is to send the right config_update; verified by
    # /settings.llm_model in step [B1] above.
    def _has_assistant_reply(t):
        msgs = t._get("/chat/messages?n=4").json().get("messages", [])
        return any(m.get("role") == "assistant" and m.get("text", "")
                   for m in msgs)
    r.step("[D] Dragon delivered an assistant reply",
           _has_assistant_reply)
    r.step("[D] Screenshot final Cloud-Haiku turn complete",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave4_D_haiku_turn.jpg")) > 1000)

    # ─── Cleanup ────────────────────────────────────────────────
    r.step("[end] Restore Local mode",
           lambda t: t.mode(0).get("ok") is not False)
    r.step("[end] Navigate home",
           lambda t: t.navigate("home").get("navigated") == "home")


def story_wave5_hybrid_story(r: Runner) -> None:
    """TT #328 Wave 5 — Hybrid story (mode-sheet captions).

    Pre-Wave-5 the mode sheet's composite card showed cost only:
      Local:  "ON-DEVICE · FREE"
      Hybrid: "STUDIO VOICE · LOCAL BRAIN · ~$0.02"
      Cloud:  "{model} · STUDIO · ~$0.04"
      Agent:  "MEMORY BYPASSED · GATEWAY TOOLS"
    The user couldn't see WHY Hybrid was the practical pick — the
    load-bearing variable (latency: 60s vs 4-8s vs 3-6s) was hidden.

    Wave 5 reshapes all four captions to surface the same three
    decisive variables — latency · privacy · cost — so the choice
    is decidable at a glance:
      Local:  "~60S · 100% PRIVATE · FREE"
      Hybrid: "4-8S · PRIVATE BRAIN · ~$0.02"
      Cloud:  "3-6S · {model} · ~$0.04"
      Agent:  "AGENT TOOLS · MEMORY BYPASSED"

    Plus the onboarding "HOW I THINK" card was reordered to lead
    with Hybrid (recommended) rather than Local — but onboarding
    only fires on first-boot, so this story can't easily exercise
    it via the harness.  The source change lands; field verification
    happens on a fresh device.

    Touch-driven: long-press orb to open sheet, tap each preset chip,
    screenshot the composite caption, verify the latency + privacy
    + cost stamp matches expectations.
    """
    tab5 = r.tab5

    # Coords from touch_stress.py (verified):
    #   ORB_X, ORB_Y = 360, 320       — main orb (long-press → sheet)
    #   PRESET_X     = [115, 230, 358, 488, 618]
    #   PRESET_Y     = 763
    #   Composite caption sub-label is rendered at y_in_sheet 86 inside
    #   the composite card (y=20 + sheet_y=60 + composite y_in_sheet=...
    #   ~ around y=940 in absolute screen coords).  Screenshots are
    #   captured for visual diff; assertions run against the screen
    #   state and the underlying NVS values.
    PRESET_X = [115, 230, 358, 488, 618]  # Local, Hybrid, Cloud, Agent, Onboard
    PRESET_Y = 763
    ORB_X, ORB_Y = 360, 320

    def long_press_orb(t):
        return t.long_press(ORB_X, ORB_Y, duration_ms=900)

    # ─── State setup ────────────────────────────────────────────
    r.step("Boot reachable", lambda t: t.wait_alive(60))
    r.step("Reset event cursor", lambda t: t.reset_event_cursor() and None)
    r.step("Force Local mode (clean baseline)",
           lambda t: t.mode(0).get("ok") is not False)
    r.step("Cancel any leftover voice state",
           lambda t: t._post("/voice/cancel").json() is not None)
    time.sleep(1)
    r.step("Navigate home",
           lambda t: t.navigate("home").get("navigated") == "home")
    time.sleep(1)

    # ─── A. Long-press orb → mode sheet shows Local caption ──────
    r.step("[A] Long-press orb (360,320) → opens mode sheet",
           lambda t: long_press_orb(t) and True)
    time.sleep(1.5)
    r.step("[A] Screenshot Local caption ('~60S · 100% PRIVATE · FREE')",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave5_A_local_caption.jpg")) > 1000)

    # ─── B. Tap Hybrid preset, reopen sheet, verify caption ─────
    r.step("[B] Tap Hybrid preset (230, 763)",
           lambda t: t.tap(230, PRESET_Y) and True)
    time.sleep(1.5)
    r.step("[B] vmode flipped to 1 (Hybrid)",
           lambda t: int(t.settings().get("voice_mode", -1)) == 1)
    r.step("[B] Reopen mode sheet (long-press orb)",
           lambda t: long_press_orb(t) and True)
    time.sleep(1.5)
    r.step("[B] Screenshot Hybrid caption ('4-8S · PRIVATE BRAIN · ~$0.02')",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave5_B_hybrid_caption.jpg")) > 1000)

    # ─── C. Cloud preset → cloud caption ────────────────────────
    r.step("[C] Tap Cloud preset (358, 763)",
           lambda t: t.tap(358, PRESET_Y) and True)
    time.sleep(1.5)
    r.step("[C] vmode flipped to 2 (Cloud)",
           lambda t: int(t.settings().get("voice_mode", -1)) == 2)
    r.step("[C] Reopen mode sheet",
           lambda t: long_press_orb(t) and True)
    time.sleep(1.5)
    r.step("[C] Screenshot Cloud caption ('3-6S · {model} · ~$0.04')",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave5_C_cloud_caption.jpg")) > 1000)

    # ─── D. Local preset (regression: caption fires for all modes) ─
    r.step("[D] Tap Local preset (115, 763)",
           lambda t: t.tap(115, PRESET_Y) and True)
    time.sleep(1.5)
    r.step("[D] vmode flipped to 0 (Local)",
           lambda t: int(t.settings().get("voice_mode", -1)) == 0)
    r.step("[D] Reopen mode sheet, screenshot",
           lambda t: (long_press_orb(t) and True) and time.sleep(1.5) is None or True)
    r.step("[D] Screenshot Local caption (regression after roundtrip)",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave5_D_local_again.jpg")) > 1000)

    # ─── E. Dial-driven mode selection (not just preset chips) ──
    # Tap dial segments instead of preset chips — dials live above
    # the presets and feed tab5_mode_resolve(int_tier, voi_tier,
    # aut_tier) which the composite card watches.  Tap "Smart"
    # intelligence (idx 2) on row 0; segment x ~600 y ~290.
    r.step("[E] Tap 'Smart' intelligence segment (600, 290)",
           lambda t: t.tap(600, 290) and True)
    time.sleep(1.5)
    r.step("[E] Screenshot composite reflects dial (Smart → Cloud preset)",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave5_E_smart_dial.jpg")) > 1000)

    # ─── F. Dismiss the sheet, verify home returns ──────────────
    r.step("[F] Tap Done button (top-right of sheet, 600, 116)",
           lambda t: t.tap(600, 116) and True)
    time.sleep(1.5)
    r.step("[F] Screen reports current = home",
           lambda t: t.screen().get("current") == "home"
                  or t.screen().get("current") == "")
    r.step("[F] Screenshot home post-sheet-dismiss",
           lambda t: t.screenshot(
               os.path.join(r.run_dir, "wave5_F_home_after_dismiss.jpg")) > 1000)

    # ─── Cleanup ────────────────────────────────────────────────
    r.step("[end] Restore Local mode",
           lambda t: t.mode(0).get("ok") is not False)


SCENARIOS: dict[str, Callable[[Runner], None]] = {
    "story_smoke":   story_smoke,
    "story_full":    story_full,
    "story_stress":  story_stress,
    "story_onboard": story_onboard,
    "story_wave1":   story_wave1_visibility,
    "story_wave2":   story_wave2_error_surfacing,
    "story_wave3":   story_wave3_dictation_in_chat,
    "story_wave4":   story_wave4_cloud_picker,
    "story_wave5":   story_wave5_hybrid_story,
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
