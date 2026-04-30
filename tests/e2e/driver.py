"""Tab5Driver — HTTP+events client for the Tab5 debug server.

Built for the e2e user-story harness (#293/#294).  Wraps the bearer-
auth'd HTTP API into a thin Python class with composable building
blocks (tap, swipe, navigate, screenshot, await_event, …) so scenarios
can read like UX walkthroughs rather than curl scripts.

Usage:

    from driver import Tab5Driver
    tab5 = Tab5Driver("http://192.168.1.90:8080", token="…")
    tab5.reboot()
    tab5.await_voice_state("READY", timeout_s=30)
    tab5.navigate("camera")
    tab5.tap(360, 1100)              # capture button
    tab5.await_event("camera.capture", timeout_s=8)
    tab5.screenshot("/tmp/run/01.jpg")
"""

from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

import requests


@dataclass
class Event:
    ms: int
    kind: str
    detail: str = ""

    @classmethod
    def from_dict(cls, d: dict) -> "Event":
        return cls(ms=int(d.get("ms", 0)), kind=d.get("kind", ""),
                   detail=d.get("detail", ""))


@dataclass
class Tab5Driver:
    base_url: str
    token: str
    timeout: float = 10.0
    _last_event_ms: int = field(default=0, init=False)

    # ── Low-level HTTP ────────────────────────────────────────────
    def _headers(self) -> dict:
        return {"Authorization": f"Bearer {self.token}"}

    def _get(self, path: str, **kw) -> requests.Response:
        return requests.get(f"{self.base_url}{path}",
                            headers=self._headers(),
                            timeout=kw.pop("timeout", self.timeout), **kw)

    def _post(self, path: str, **kw) -> requests.Response:
        return requests.post(f"{self.base_url}{path}",
                             headers=self._headers(),
                             timeout=kw.pop("timeout", self.timeout), **kw)

    # ── Liveness ──────────────────────────────────────────────────
    def info(self) -> dict:
        return self._get("/info", timeout=5).json()

    def is_alive(self) -> bool:
        try:
            return self.info().get("uptime_ms", 0) > 0
        except Exception:
            return False

    def wait_alive(self, timeout_s: float = 60) -> bool:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.is_alive():
                return True
            time.sleep(1)
        return False

    # ── Touch / gesture ───────────────────────────────────────────
    def tap(self, x: int, y: int) -> dict:
        return self._post("/touch", json={
            "x": x, "y": y, "action": "tap"
        }).json()

    def long_press(self, x: int, y: int, duration_ms: int = 1000) -> dict:
        return self._post("/touch", json={
            "x": x, "y": y, "action": "long_press",
            "duration_ms": duration_ms,
        }).json()

    def swipe(self, x1: int, y1: int, x2: int, y2: int,
              duration_ms: int = 300) -> dict:
        return self._post("/touch", json={
            "action": "swipe",
            "x1": x1, "y1": y1, "x2": x2, "y2": y2,
            "duration_ms": duration_ms,
        }).json()

    # ── Navigation ────────────────────────────────────────────────
    _last_nav_t: float = field(default=0.0, init=False)

    def navigate(self, screen: str) -> dict:
        # Debug-server has a 500ms debounce on /navigate; sleep just
        # enough to clear it so consecutive navigations succeed.
        elapsed = time.time() - self._last_nav_t
        if elapsed < 0.6:
            time.sleep(0.6 - elapsed)
        self._last_nav_t = time.time()
        return self._post(f"/navigate?screen={screen}").json()

    def screen(self) -> dict:
        return self._get("/screen").json()

    # ── Voice / chat ──────────────────────────────────────────────
    def voice_state(self) -> dict:
        return self._get("/voice").json()

    def chat(self, text: str) -> dict:
        return self._post("/chat", json={"text": text}).json()

    def voice_text(self, text: str) -> dict:
        return self._post("/voice/text", json={"text": text}).json()

    def voice_cancel(self) -> dict:
        return self._post("/voice/cancel").json()

    def voice_clear(self) -> dict:
        return self._post("/voice/clear").json()

    def voice_reconnect(self) -> dict:
        return self._post("/voice/reconnect").json()

    # ── M5 / K144 chain (vmode=4) ─────────────────────────────────
    def m5_status(self) -> dict:
        """GET /m5 — chain_active, failover_state, uart_baud."""
        return self._get("/m5").json()

    # ── Mode + settings ───────────────────────────────────────────
    def mode(self, m: int, model: str | None = None) -> dict:
        q = f"/mode?m={m}"
        if model:
            q += f"&model={model}"
        return self._post(q).json()

    def settings(self) -> dict:
        return self._get("/settings").json()

    # ── Budget helpers (TT #328 Wave 1) ──────────────────────────
    def budget(self) -> dict:
        """Returns {spent_mils, cap_mils} from /settings."""
        s = self.settings()
        return {
            "spent_mils": int(s.get("spent_mils", 0) or 0),
            "cap_mils":   int(s.get("cap_mils",   0) or 0),
        }

    def set_cap_mils(self, cap: int) -> dict:
        return self._post("/settings",
                          json={"cap_mils": int(cap)}).json()

    def reset_spent(self) -> dict:
        return self._post("/settings",
                          json={"reset_spent": True}).json()

    # ── Camera / video ────────────────────────────────────────────
    def camera_frame(self, save_path: str | None = None) -> bytes:
        r = self._get("/camera", timeout=15)
        data = r.content
        if save_path:
            os.makedirs(os.path.dirname(save_path) or ".", exist_ok=True)
            with open(save_path, "wb") as f:
                f.write(data)
        return data

    def video_call_start(self, fps: int = 10) -> dict:
        return self._post(f"/video/call/start?fps={fps}").json()

    def video_call_end(self) -> dict:
        return self._post("/video/call/end").json()

    def call_status(self) -> dict:
        return self._get("/call/status").json()

    def call_mute(self) -> dict:
        return self._post("/call/mute").json()

    def call_unmute(self) -> dict:
        return self._post("/call/unmute").json()

    # ── Screenshot ────────────────────────────────────────────────
    def screenshot(self, save_path: str) -> int:
        """Capture screenshot.  Returns size in bytes."""
        r = self._get("/screenshot.jpg", timeout=15)
        os.makedirs(os.path.dirname(save_path) or ".", exist_ok=True)
        with open(save_path, "wb") as f:
            f.write(r.content)
        return len(r.content)

    # ── System / heap / metrics ───────────────────────────────────
    def heap(self) -> dict:
        return self._get("/heap").json()

    def metrics(self) -> dict:
        return self._get("/metrics").json()

    def reboot(self) -> dict:
        try:
            return self._post("/reboot", timeout=5).json()
        except Exception:
            # Reboot kills the connection mid-response, that's expected.
            return {"ok": True, "rebooting": True}

    # ── Events ────────────────────────────────────────────────────
    def events(self, since_ms: int | None = None,
               peek: bool = False) -> list[Event]:
        """Return events newer than `since_ms` (default: last cursor).

        Side-effect: advances `_last_event_ms` so the next call only sees
        new events.  Pass `peek=True` to skip the cursor advance — used
        by diagnostic snapshots so they don't steal events that
        await_event() in a later step needs to see.
        """
        if since_ms is None:
            since_ms = self._last_event_ms
        r = self._get(f"/events?since={since_ms}", timeout=5).json()
        events = [Event.from_dict(e) for e in r.get("events", [])]
        if events and not peek:
            self._last_event_ms = max(self._last_event_ms,
                                       events[-1].ms + 1)
        return events

    def reset_event_cursor(self) -> int:
        """Reset the event cursor to NOW so subsequent await_event() only
        sees future events, not history."""
        info = self.info()
        self._last_event_ms = int(info.get("uptime_ms", 0))
        return self._last_event_ms

    def await_event(self, kind: str,
                    timeout_s: float = 30,
                    detail_match: Callable[[str], bool] | None = None,
                    poll_ms: int = 250) -> Event | None:
        """Block until an event of `kind` appears, or timeout.  Optional
        `detail_match` filter on the detail string.

        Uses simple polling.  An experimental server-side long-poll
        was reverted (see debug_server.c #296 follow-up note) — ESP-IDF
        httpd is single-task and held all other debug requests for
        the wait duration.
        """
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                for e in self.events():
                    if e.kind == kind and (
                        detail_match is None or detail_match(e.detail)
                    ):
                        return e
            except Exception:
                pass
            time.sleep(poll_ms / 1000.0)
        return None

    def input_text(self, text: str, submit: bool = False) -> dict:
        """Type `text` into the currently-focused LVGL textarea.
        Optional `submit=True` dispatches LV_EVENT_READY (form submit)."""
        return self._post("/input/text",
                          json={"text": text, "submit": submit}).json()

    def await_voice_state(self, state: str, timeout_s: float = 60) -> bool:
        """Wait for voice state == `state` — re-checks /voice every
        ~1s in addition to watching voice.state events, so a transition
        that fired before our cursor still gets picked up."""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                cur = self.voice_state().get("state_name", "")
                if cur == state:
                    return True
            except Exception:
                pass
            # Watch the event ring for up to ~1s, then re-check current state
            evt = self.await_event(
                "voice.state",
                timeout_s=min(1.0, max(0.1, deadline - time.time())),
                detail_match=lambda d: d == state,
            )
            if evt is not None:
                return True
        return False

    def await_screen(self, screen: str, timeout_s: float = 5) -> bool:
        try:
            cur = self.screen().get("current", "")
            if cur == screen:
                return True
        except Exception:
            pass
        evt = self.await_event(
            "screen.navigate", timeout_s=timeout_s,
            detail_match=lambda d: d == screen,
        )
        return evt is not None

    def await_llm_done(self, timeout_s: float = 90) -> Event | None:
        return self.await_event("chat.llm_done", timeout_s=timeout_s)
