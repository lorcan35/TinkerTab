#!/usr/bin/env python3
"""TinkerClaw stack audit — no /screenshot.jpg (which knocks Tab5 over).

Drives Tab5 via debug HTTP, captures /info + /events + /voice snapshots,
exercises voice modes 0 (LOCAL/Ollama) and 3 (TINKERCLAW/MiniMax), and
runs a touch+keyboard stress pattern. Correlates with Dragon-side voice
log over SSH.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

import requests

TAB5_URL = os.environ.get("TAB5_URL", "http://192.168.70.128:8080")
TAB5_TOKEN = os.environ.get("TAB5_TOKEN", "05eed3b13bf62d92cfd8ac424438b9f2")
DRAGON_HOST = os.environ.get("DRAGON_HOST", "192.168.70.242")
DRAGON_PASS = os.environ.get("DRAGON_PASS", "thedragon")

OUT_DIR = Path("/tmp/tc-audit") / time.strftime("%Y%m%d-%H%M%S")
OUT_DIR.mkdir(parents=True, exist_ok=True)

H = {"Authorization": f"Bearer {TAB5_TOKEN}"}
S = requests.Session()
S.headers.update(H)


def _t(name: str) -> float:
    return time.monotonic()


def info() -> dict:
    r = S.get(f"{TAB5_URL}/info", timeout=4)
    r.raise_for_status()
    return r.json()


def voice_state() -> dict:
    r = S.get(f"{TAB5_URL}/voice", timeout=4)
    r.raise_for_status()
    return r.json()


def events(since: int = 0, limit: int = 40) -> list[dict]:
    r = S.get(f"{TAB5_URL}/events", params={"since": since, "limit": limit}, timeout=4)
    r.raise_for_status()
    return r.json().get("events", [])


def screen() -> dict:
    r = S.get(f"{TAB5_URL}/screen", timeout=4)
    r.raise_for_status()
    return r.json()


def settings() -> dict:
    r = S.get(f"{TAB5_URL}/settings", timeout=4)
    r.raise_for_status()
    return r.json()


def set_mode(m: int) -> dict:
    r = S.post(f"{TAB5_URL}/mode", params={"m": m}, timeout=10)
    r.raise_for_status()
    return r.json()


def navigate(target: str) -> dict:
    r = S.post(f"{TAB5_URL}/navigate", params={"screen": target}, timeout=8)
    r.raise_for_status()
    return r.json()


def tap(x: int, y: int) -> dict:
    r = S.post(f"{TAB5_URL}/touch", json={"x": x, "y": y, "action": "tap"}, timeout=4)
    r.raise_for_status()
    return r.json()


def chat(text: str) -> dict:
    r = S.post(f"{TAB5_URL}/chat", json={"text": text}, timeout=8)
    r.raise_for_status()
    return r.json()


def voice_text(text: str) -> dict:
    r = S.post(f"{TAB5_URL}/voice/text", json={"text": text}, timeout=8)
    r.raise_for_status()
    return r.json()


def dragon_journal(unit: str, since_sec: int = 60) -> str:
    cmd = (
        f"sshpass -p {DRAGON_PASS} ssh -o StrictHostKeyChecking=no "
        f"-o ConnectTimeout=10 radxa@{DRAGON_HOST} "
        f"\"sudo journalctl -u {unit} --since '{since_sec} sec ago' --no-pager 2>&1 | tail -20\""
    )
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=20).stdout


def health_snapshot(label: str) -> dict:
    snap = {"label": label, "ts": time.time()}
    try:
        i = info()
        snap.update({
            "uptime_s": i["uptime_ms"] / 1000,
            "heap_kb": i["heap_free"] / 1024,
            "heap_min_kb": i["heap_min"] / 1024,
            "lvgl_fps": i["lvgl_fps"],
            "tasks": i["tasks"],
            "dragon": i["dragon_connected"],
            "voice": i["voice_connected"],
            "chat_ev": i.get("chat_evictions_total", 0),
            "widget_ev": i.get("widget_evictions_total", 0),
            "reset": i.get("reset_reason", "?"),
        })
    except Exception as e:
        snap["error"] = str(e)
    print(f"  [{label}] {snap}", flush=True)
    return snap


def section(title: str):
    bar = "═" * 60
    print(f"\n{bar}\n  {title}\n{bar}", flush=True)


# ────────────────────────────────────────────────────────────────────
# Audit run
# ────────────────────────────────────────────────────────────────────

def main():
    results: dict = {"start": time.time(), "sections": []}
    section("0. Pre-audit baseline")
    results["sections"].append({"baseline": health_snapshot("baseline")})
    try:
        s = settings()
        results["sections"].append({"settings": s})
        print(f"  voice_mode persisted: {s.get('voice_mode')} (0=LOCAL,1=HYBRID,2=CLOUD,3=TINKERCLAW)")
    except Exception as e:
        print(f"  settings failed: {e}")

    try:
        evs = events(since=0, limit=200)
        print(f"  {len(evs)} early events (showing types)")
        kinds: dict[str, int] = {}
        for e in evs:
            kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
        for k, n in sorted(kinds.items(), key=lambda kv: -kv[1]):
            print(f"    {k}: {n}")
        results["sections"].append({"event_kinds": kinds})
    except Exception as e:
        print(f"  events failed: {e}")

    section("1. Mode 0 LOCAL voice turn (Ollama path)")
    try:
        set_mode(0)
        time.sleep(2)
        h0 = health_snapshot("pre-mode0-chat")
        t0 = time.monotonic()
        chat("hi from audit, reply with 5 words please")
        # poll for response (state transitions THINKING → SPEAKING → IDLE)
        states = []
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                vs = voice_state()
                states.append((round(time.monotonic() - t0, 1), vs.get("state_name")))
                if vs.get("state_name") == "READY":
                    if len(states) > 3 and states[-1][1] == "READY":
                        # likely returned to READY after THINKING/SPEAKING
                        break
            except Exception:
                pass
            time.sleep(1)
        print(f"  state timeline: {states}")
        h1 = health_snapshot("post-mode0-chat")
        results["sections"].append({"mode0_chat": {"timeline": states, "pre": h0, "post": h1}})
    except Exception as e:
        print(f"  mode0 chat failed: {e}")
        results["sections"].append({"mode0_chat": {"error": str(e)}})

    section("2. Mode 3 TINKERCLAW voice turn (gateway → MiniMax)")
    try:
        set_mode(3)
        time.sleep(3)
        h0 = health_snapshot("pre-mode3-chat")
        t0 = time.monotonic()
        chat("audit ping: respond with 5 words")
        states = []
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                vs = voice_state()
                states.append((round(time.monotonic() - t0, 1), vs.get("state_name")))
                if vs.get("state_name") == "READY" and len(states) > 3:
                    break
            except Exception:
                pass
            time.sleep(1)
        print(f"  state timeline: {states}")
        h1 = health_snapshot("post-mode3-chat")
        results["sections"].append({"mode3_chat": {"timeline": states, "pre": h0, "post": h1}})
        # capture dragon-side voice log for this turn
        dlog = dragon_journal("tinkerclaw-voice", since_sec=120)
        (OUT_DIR / "dragon_voice_mode3.log").write_text(dlog)
        glog = dragon_journal("tinkerclaw-gateway", since_sec=120)
        (OUT_DIR / "dragon_gateway_mode3.log").write_text(glog)
    except Exception as e:
        print(f"  mode3 chat failed: {e}")
        results["sections"].append({"mode3_chat": {"error": str(e)}})

    section("3. Touch stress (10 navigations, no screenshots)")
    nav_targets = ["home", "chat", "settings", "home", "camera",
                    "home", "settings", "home", "chat", "home"]
    nav_results = []
    for i, t in enumerate(nav_targets):
        try:
            t_start = time.monotonic()
            navigate(t)
            time.sleep(0.5)
            sc = screen()
            elapsed = round((time.monotonic() - t_start) * 1000)
            nav_results.append({"step": i, "target": t, "got": sc.get("screen"),
                                "ok": sc.get("screen") == t, "ms": elapsed})
        except Exception as e:
            nav_results.append({"step": i, "target": t, "error": str(e)})
    print(f"  nav: {sum(1 for r in nav_results if r.get('ok'))}/{len(nav_results)} ok")
    h_after_nav = health_snapshot("post-nav-stress")
    results["sections"].append({"nav_stress": {"results": nav_results, "post": h_after_nav}})

    section("4. Rapid-fire chat (10 messages, mode 0, watch heap)")
    try:
        set_mode(0)
        time.sleep(1)
        rapid_results = []
        for i in range(10):
            t_start = time.monotonic()
            try:
                chat(f"audit msg {i+1}: brief")
                ms = round((time.monotonic() - t_start) * 1000)
                inf = info()
                rapid_results.append({"i": i, "ms": ms, "heap_kb": inf["heap_free"] // 1024,
                                     "fps": inf["lvgl_fps"]})
            except Exception as e:
                rapid_results.append({"i": i, "error": str(e)})
            time.sleep(2)  # don't completely spam
        print(f"  rapid-fire: {len(rapid_results)} messages")
        for r in rapid_results:
            print(f"    {r}")
        results["sections"].append({"rapid_chat": rapid_results})
    except Exception as e:
        results["sections"].append({"rapid_chat": {"error": str(e)}})

    section("5. Memory + LVGL behaviour over 60s")
    try:
        samples = []
        for i in range(12):
            inf = info()
            samples.append({
                "t": i * 5,
                "heap_kb": inf["heap_free"] // 1024,
                "heap_min_kb": inf["heap_min"] // 1024,
                "fps": inf["lvgl_fps"],
                "tasks": inf["tasks"],
                "chat_ev": inf.get("chat_evictions_total", 0),
                "widget_ev": inf.get("widget_evictions_total", 0),
            })
            time.sleep(5)
        for s in samples:
            print(f"    t={s['t']:3d}s heap={s['heap_kb']}KB heap_min={s['heap_min_kb']}KB fps={s['fps']} tasks={s['tasks']} chat_ev={s['chat_ev']} widget_ev={s['widget_ev']}")
        results["sections"].append({"telemetry_60s": samples})
    except Exception as e:
        results["sections"].append({"telemetry_60s": {"error": str(e)}})

    section("6. Final snapshot + recent events")
    final = health_snapshot("final")
    results["sections"].append({"final": final})
    try:
        evs = events(since=0, limit=300)
        # bucket by kind
        kinds: dict[str, int] = {}
        errors = []
        for e in evs:
            k = e["kind"]
            kinds[k] = kinds.get(k, 0) + 1
            if k.startswith("error.") or e.get("detail") in ("fail", "ack_fail", "probe_fail",
                                                                "reset_probe_fail", "unavailable"):
                errors.append(e)
        print(f"  total events seen: {len(evs)}")
        print(f"  kinds: {dict(sorted(kinds.items(), key=lambda kv: -kv[1])[:15])}")
        print(f"  errors/fails: {len(errors)}")
        for e in errors[-15:]:
            print(f"    {e}")
        results["sections"].append({"final_events": {"kinds": kinds, "errors": errors[-30:]}})
    except Exception as e:
        results["sections"].append({"final_events": {"error": str(e)}})

    section("7. Solo mode (vmode=5 SOLO_DIRECT) — text turn + heap stability")
    or_key = os.environ.get("OPENROUTER_KEY") or os.environ.get("OPENROUTER_API_KEY", "")
    if not or_key:
        print("  SKIP: no OPENROUTER_KEY / OPENROUTER_API_KEY in env")
        results["sections"].append({"solo_mode": {"skipped": "no key"}})
    else:
        try:
            S.post(f"{TAB5_URL}/settings", json={"or_key": or_key}, timeout=4)
            print(f"  or_key provisioned ({len(or_key)} chars)")
            set_mode(5)
            time.sleep(2)
            h0 = health_snapshot("pre-solo-llm")
            t0 = time.monotonic()
            # Hit /solo/llm_test directly — bypasses chat dispatch + voice_modes
            # to give a clean wire-test of the OpenRouter HTTP path.
            try:
                r = S.post(f"{TAB5_URL}/solo/llm_test",
                           json={"prompt": "reply with the single word: pong"},
                           timeout=60)
                r.raise_for_status()
                d = r.json()
                ok = bool(d.get("ok"))
                reply = d.get("reply", "")
                deltas = d.get("delta_count", 0)
                print(f"  solo LLM: ok={ok}  reply={reply[:80]!r}  deltas={deltas}")
            except Exception as e:
                print(f"  solo LLM call failed: {e}")
                ok, reply, deltas = False, "", 0

            h1 = health_snapshot("post-solo-llm")

            # RAG smoke: remember → recall round-trip
            rag_remember_ok = False
            rag_recall_ok = False
            try:
                rr = S.post(f"{TAB5_URL}/solo/rag_test",
                            json={"action": "remember",
                                  "text": "audit-canary fact: my favourite colour is teal"},
                            timeout=30)
                rag_remember_ok = bool(rr.json().get("ok"))
                rr = S.post(f"{TAB5_URL}/solo/rag_test",
                            json={"action": "recall",
                                  "query": "what colour do I like"},
                            timeout=30)
                hits = rr.json().get("hits", [])
                rag_recall_ok = (len(hits) >= 1
                                 and "teal" in (hits[0].get("text", "").lower()))
                print(f"  RAG remember={rag_remember_ok}  recall_finds_teal={rag_recall_ok}")
            except Exception as e:
                print(f"  RAG test failed: {e}")

            results["sections"].append({"solo_mode": {
                "llm": {"ok": ok, "reply_head": reply[:120], "deltas": deltas,
                        "wall_s": round(time.monotonic() - t0, 2)},
                "rag": {"remember": rag_remember_ok, "recall_finds_teal": rag_recall_ok},
                "pre": h0, "post": h1,
            }})

            # Restore something useful
            set_mode(2)
        except Exception as e:
            print(f"  solo mode section errored: {e}")
            results["sections"].append({"solo_mode": {"error": str(e)}})

    # save raw report
    (OUT_DIR / "audit.json").write_text(json.dumps(results, indent=2, default=str))
    print(f"\n  audit json: {OUT_DIR / 'audit.json'}")
    print(f"  dragon logs: {OUT_DIR}/dragon_*.log")
    return results


if __name__ == "__main__":
    main()
