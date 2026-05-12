"""Tab5 host discovery — mDNS first, then env/arg, then bail.

The DGX dev host's LAN has flipped between 192.168.1.x and 192.168.70.x
multiple times and the Tab5 itself is DHCP-leased, so any hardcoded IP
rots within a week.  This module gives the runner + nightly cron a
single source-of-truth for "where is the Tab5 right now?".

Resolution order:

    1. Explicit URL passed to `find_tab5(prefer_url=...)`
    2. TAB5_URL environment variable
    3. mDNS lookup of `espressif.local` (Tab5 advertises this name by
       default via the IDF mDNS service — verified live 2026-05-04)
    4. Cached IP from previous successful run at ~/.tinker_tab5_url
    5. `None` — caller decides whether to error or scan

The function returns the full `http://host:8080` URL (port can be
overridden), not a bare IP, so the runner can drop it straight into
`Tab5Driver(base_url=...)`.  Last-known IPs are cached so a flaky
mDNS hop doesn't break a run.
"""

from __future__ import annotations

import os
import socket
import subprocess
from pathlib import Path
from typing import Optional

CACHE_PATH = Path.home() / ".tinker_tab5_url"
DEFAULT_HOSTNAME = "espressif.local"
DEFAULT_PORT = 8080


def _probe(url: str, timeout: float = 1.5) -> bool:
    """Return True iff GET ``{url}/info`` returns a JSON body with the
    Tab5 ``auth_required`` field.  Anything else (timeout, wrong
    server, non-Tab5 host) → False."""
    try:
        import urllib.request
        import json
        with urllib.request.urlopen(f"{url}/info", timeout=timeout) as r:
            body = json.loads(r.read().decode("utf-8", errors="replace"))
        return bool(body.get("auth_required"))
    except Exception:
        return False


def _resolve_mdns(name: str = DEFAULT_HOSTNAME, timeout: float = 2.0) -> Optional[str]:
    """Resolve an mDNS hostname to an IP.  Uses avahi-resolve-host-name
    if present (Linux), else stdlib socket.gethostbyname.  Returns the
    IP string or None."""
    try:
        out = subprocess.run(
            ["avahi-resolve-host-name", "-4", name],
            capture_output=True, text=True, timeout=timeout,
        )
        if out.returncode == 0 and out.stdout.strip():
            parts = out.stdout.strip().split()
            if len(parts) >= 2:
                return parts[-1]
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    try:
        return socket.gethostbyname(name)
    except OSError:
        return None


def _read_cache() -> Optional[str]:
    try:
        url = CACHE_PATH.read_text().strip()
        return url or None
    except (OSError, ValueError):
        return None


def _write_cache(url: str) -> None:
    try:
        CACHE_PATH.write_text(url)
    except OSError:
        pass  # cache is best-effort; never block a run on cache I/O


def find_tab5(
    prefer_url: Optional[str] = None,
    *,
    port: int = DEFAULT_PORT,
    hostname: str = DEFAULT_HOSTNAME,
) -> Optional[str]:
    """Return a verified Tab5 base URL, or None.

    `prefer_url` wins over every other source — used by `--base-url`
    callers who explicitly want to target one device.  A successfully
    resolved URL is written to the cache so the next run starts with a
    warm hint."""
    candidates = []
    if prefer_url:
        candidates.append(prefer_url)
    env_url = os.environ.get("TAB5_URL")
    if env_url and env_url not in candidates:
        candidates.append(env_url)
    mdns_ip = _resolve_mdns(hostname)
    if mdns_ip:
        candidates.append(f"http://{mdns_ip}:{port}")
    cached = _read_cache()
    if cached and cached not in candidates:
        candidates.append(cached)

    for url in candidates:
        if _probe(url):
            _write_cache(url)
            return url
    return None


def main() -> int:
    """CLI: print the discovered URL or exit non-zero."""
    url = find_tab5()
    if not url:
        print("Tab5 not found via env/mDNS/cache.  Try:")
        print("  TAB5_URL=http://<ip>:8080 python3 discover.py")
        return 1
    print(url)
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
