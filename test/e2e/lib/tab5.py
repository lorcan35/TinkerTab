"""Tab5Client — HTTP wrapper around debug_server.c.

Driven by environment variables (see README). No internal state assumptions —
every call is a fresh HTTP request or serial read.
"""
from __future__ import annotations

import io
import os
import re
import time
from dataclasses import dataclass
from typing import Any, Callable, Optional

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry
from PIL import Image, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = True  # some builds occasionally cut the BMP stream


def _make_session() -> requests.Session:
    """Session with connect-retry + backoff. Tab5's debug server is single-threaded
    and drops connections under load. Use a fresh TCP connection per request
    (Connection: close) and retry aggressively on connect failures. Never retry on
    read — a successful read that we didn't see yet would duplicate a touch."""
    s = requests.Session()
    s.headers.update({"Connection": "close"})  # don't keep-alive; device is fragile
    retry = Retry(
        total=3,
        connect=3,
        read=0,
        backoff_factor=0.3,   # 0.3s, 0.6s, 1.2s — caps at ~2s total
        status_forcelist=(502, 503, 504),
        allowed_methods=frozenset(["GET"]),
        respect_retry_after_header=False,
    )
    s.mount("http://", HTTPAdapter(max_retries=retry, pool_connections=1, pool_maxsize=1))
    return s


class Tab5Error(RuntimeError):
    """Raised for HTTP / timeout / decode failures against the device."""


@dataclass(frozen=True)
class Tab5Info:
    heap_free: int
    psram_free: int
    lvgl_fps: int
    battery_pct: int
    wifi_ip: str
    dragon_connected: bool
    voice_connected: bool
    auth_required: bool
    raw: dict


class Tab5Client:
    """Touch-based client for the Tab5 debug server.

    Usage:
        t = Tab5Client.from_env()
        t.tap(360, 640)                 # tap near orb center
        s = t.screenshot()              # PIL.Image
        t.wait_for_voice_state("READY")
    """

    def __init__(
        self,
        host: str,
        port: int = 8080,
        token: Optional[str] = None,
        serial_port: str = "/dev/ttyACM0",
        http_timeout_s: float = 5.0,
        pacing_s: float = 0.35,
    ):
        self.host = host
        self.port = port
        self._token = token
        self.serial_port = serial_port
        self.http_timeout_s = http_timeout_s
        self.pacing_s = pacing_s
        self._session = _make_session()
        self._last_req_ts = 0.0

    def _pace(self) -> None:
        """Sleep so subsequent requests don't hammer the device — the debug_server
        is single-threaded and gets wedged under contention with the LVGL lock."""
        if self.pacing_s <= 0:
            return
        dt = time.monotonic() - self._last_req_ts
        if dt < self.pacing_s:
            time.sleep(self.pacing_s - dt)
        self._last_req_ts = time.monotonic()

    def warm_up(self, attempts: int = 3) -> bool:
        """Probe /info until the HTTP stack responds — the first request after
        a cold period often drops. Returns True on success."""
        for _ in range(attempts):
            try:
                self._session.get(f"{self.base}/info", timeout=self.http_timeout_s)
                return True
            except requests.RequestException:
                time.sleep(0.5)
        return False

    # ---------- construction ----------

    @classmethod
    def from_env(cls) -> "Tab5Client":
        return cls(
            host=os.environ.get("TAB5_HOST", "192.168.70.128"),
            port=int(os.environ.get("TAB5_PORT", "8080")),
            token=os.environ.get("TAB5_TOKEN") or None,
            serial_port=os.environ.get("TAB5_SERIAL", "/dev/ttyACM0"),
        )

    @property
    def base(self) -> str:
        return f"http://{self.host}:{self.port}"

    @property
    def token(self) -> str:
        if self._token:
            return self._token
        self._token = self._serial_token()
        if not self._token:
            raise Tab5Error("could not obtain auth token via serial")
        return self._token

    # ---------- low-level HTTP ----------

    def _headers(self, need_auth: bool = True) -> dict:
        h = {}
        if need_auth:
            h["Authorization"] = f"Bearer {self.token}"
        return h

    def _get(self, path: str, need_auth: bool = True, stream: bool = False) -> requests.Response:
        self._pace()
        try:
            r = self._session.get(
                f"{self.base}{path}",
                headers=self._headers(need_auth),
                timeout=self.http_timeout_s,
                stream=stream,
            )
        except requests.RequestException as e:
            raise Tab5Error(f"GET {path} failed: {e}") from e
        if r.status_code >= 400:
            raise Tab5Error(f"GET {path} -> {r.status_code}: {r.text[:200]}")
        return r

    def _post(self, path: str, *, json: Any = None, params: dict | None = None, need_auth: bool = True) -> requests.Response:
        self._pace()
        try:
            # POST: bounded manual retry for connect errors only (never duplicate side-effects on success)
            attempts = 3
            last_exc: requests.RequestException | None = None
            r = None
            for i in range(attempts):
                try:
                    r = self._session.post(
                        f"{self.base}{path}",
                        headers=self._headers(need_auth),
                        params=params,
                        json=json,
                        timeout=self.http_timeout_s,
                    )
                    break
                except (requests.ConnectTimeout, requests.ConnectionError) as e:
                    last_exc = e
                    if i + 1 < attempts:
                        time.sleep(0.4 * (i + 1))
            if r is None:
                raise last_exc  # type: ignore[misc]
        except requests.RequestException as e:
            raise Tab5Error(f"POST {path} failed: {e}") from e
        if r.status_code >= 400:
            raise Tab5Error(f"POST {path} -> {r.status_code}: {r.text[:200]}")
        return r

    # ---------- public endpoints (no auth) ----------

    def info(self) -> Tab5Info:
        j = self._get("/info", need_auth=False).json()
        return Tab5Info(
            heap_free=j.get("heap_free", 0),
            psram_free=j.get("psram_free", 0),
            lvgl_fps=j.get("lvgl_fps", 0),
            battery_pct=j.get("battery_pct", 0),
            wifi_ip=j.get("wifi_ip", ""),
            dragon_connected=j.get("dragon_connected", False),
            voice_connected=j.get("voice_connected", False),
            auth_required=j.get("auth_required", True),
            raw=j,
        )

    def selftest(self) -> dict:
        return self._get("/selftest", need_auth=False).json()

    # ---------- authenticated state ----------

    def settings(self) -> dict:
        return self._get("/settings").json()

    def voice(self) -> dict:
        return self._get("/voice").json()

    # ---------- screenshot ----------

    def screenshot(self, retries: int = 3, timeout_s: float = 20.0) -> Image.Image:
        """Fetch a BMP framebuffer dump and return as PIL RGB image.

        Screenshot is ~1.8MB; needs its own longer timeout. Uses a direct GET
        bypassing the retry-on-connect session logic since this request streams."""
        last: Exception | None = None
        for attempt in range(retries + 1):
            try:
                self._pace()
                r = requests.get(
                    f"{self.base}/screenshot",
                    headers=self._headers(need_auth=True),
                    timeout=timeout_s,
                    stream=True,
                )
                if r.status_code >= 400:
                    raise Tab5Error(f"/screenshot -> {r.status_code}")
                data = r.content  # buffers the full stream
                img = Image.open(io.BytesIO(data))
                img.load()
                return img.convert("RGB")
            except Exception as e:  # noqa: BLE001
                last = e
                time.sleep(0.8 * (attempt + 1))
        raise Tab5Error(f"screenshot failed after {retries + 1} tries: {last}")

    # ---------- touch ----------

    def tap(self, x: int, y: int) -> None:
        self._post("/touch", json={"x": int(x), "y": int(y), "action": "tap"})

    def long_press(self, x: int, y: int, duration_ms: int = 800) -> None:
        self._post("/touch", json={"x": int(x), "y": int(y), "action": "long_press", "duration_ms": duration_ms})

    def swipe(self, x1: int, y1: int, x2: int, y2: int, duration_ms: int = 250) -> None:
        self._post(
            "/touch",
            json={
                "action": "swipe",
                "x1": int(x1), "y1": int(y1),
                "x2": int(x2), "y2": int(y2),
                "duration_ms": duration_ms,
            },
        )

    # ---------- navigation / mode ----------

    def navigate(self, screen: str) -> None:
        self._post("/navigate", params={"screen": screen})

    def set_voice_mode(self, mode: int, model: Optional[str] = None) -> None:
        params: dict[str, Any] = {"m": mode}
        if model:
            params["model"] = model
        self._post("/mode", params=params)

    def send_chat(self, text: str) -> None:
        self._post("/chat", json={"text": text})

    def force_reconnect(self) -> None:
        self._post("/voice/reconnect")

    # ---------- waiting helpers ----------

    def wait_for_voice_state(self, state_name: str, timeout_s: float = 10.0, poll_s: float = 0.3) -> dict:
        deadline = time.monotonic() + timeout_s
        last: dict = {}
        while time.monotonic() < deadline:
            last = self.voice()
            if last.get("state_name") == state_name:
                return last
            time.sleep(poll_s)
        raise Tab5Error(f"voice state never reached {state_name}; last={last}")

    def wait_for(self, predicate: Callable[[], bool], timeout_s: float = 5.0, poll_s: float = 0.25, what: str = "predicate") -> None:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(poll_s)
        raise Tab5Error(f"timed out waiting for {what}")

    # ---------- serial ----------

    def _serial_token(self) -> Optional[str]:
        try:
            import serial  # type: ignore[import-untyped]
        except ImportError:
            return None
        try:
            s = serial.Serial(self.serial_port, 115200, timeout=0.5)
        except Exception:
            return None
        try:
            time.sleep(0.2)
            s.reset_input_buffer()
            s.write(b"\n")
            s.flush()
            time.sleep(0.15)
            s.reset_input_buffer()
            s.write(b"token\n")
            s.flush()
            buf = b""
            t0 = time.time()
            while time.time() - t0 < 3.0:
                if s.in_waiting:
                    buf += s.read(s.in_waiting)
                else:
                    time.sleep(0.05)
            m = re.search(r"[Aa]uth [Tt]oken[: ]+([0-9a-f]{32})", buf.decode("utf-8", errors="replace"))
            return m.group(1) if m else None
        finally:
            try:
                s.close()
            except Exception:
                pass

    def serial_command(self, cmd: str, read_s: float = 2.0) -> str:
        """Send a command to the serial prompt and return accumulated output."""
        try:
            import serial  # type: ignore[import-untyped]
        except ImportError:
            raise Tab5Error("pyserial not installed")
        s = serial.Serial(self.serial_port, 115200, timeout=0.5)
        try:
            time.sleep(0.2)
            s.reset_input_buffer()
            s.write(f"{cmd}\n".encode())
            s.flush()
            buf = b""
            t0 = time.time()
            while time.time() - t0 < read_s:
                if s.in_waiting:
                    buf += s.read(s.in_waiting)
                else:
                    time.sleep(0.05)
            return buf.decode("utf-8", errors="replace")
        finally:
            try:
                s.close()
            except Exception:
                pass
