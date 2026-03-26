#!/usr/bin/env python3
"""
Dragon Streaming Server — CDP Browser Screencast + Touch Input

Connects to Chromium via Chrome DevTools Protocol (port 9222) and:
  GET /stream  — MJPEG multipart stream of the browser (via Page.startScreencast)
  WS  /ws/touch — Receives touch from Tab5, dispatches as CDP mouse events

This is the bridge between Tab5 (ESP32-P4 720x1280 display) and Dragon's browser.
Tab5 sees a live view of the browser. Taps on Tab5 = clicks in the browser.
"""

import asyncio
import json
import time

import aiohttp
from aiohttp import web

# Config
HOST = "0.0.0.0"
PORT = 3501
CDP_HOST = "127.0.0.1"
CDP_PORT = 18800
SCREENCAST_QUALITY = 60   # JPEG quality (0-100)
SCREENCAST_MAX_W = 720
SCREENCAST_MAX_H = 1280
SCREENCAST_FPS = 15

# State
cdp_ws = None                # CDP WebSocket connection
cdp_session_id = None        # Target session
latest_frame = None          # Latest JPEG bytes
latest_frame_event = None    # asyncio.Event for new frame notification
frame_metadata = None        # {offsetTop, pageScaleFactor, deviceWidth, deviceHeight}
browser_size = (720, 1280)   # Actual browser viewport size for coordinate mapping
frame_count = 0
fps = 0.0


async def get_cdp_target():
    """Get the first available page target from CDP."""
    async with aiohttp.ClientSession() as session:
        async with session.get(f"http://{CDP_HOST}:{CDP_PORT}/json") as resp:
            targets = await resp.json()
    # Prefer a real web page (http/https), not chrome:// internal pages
    for t in targets:
        url = t.get("url", "")
        if t.get("type") == "page" and (url.startswith("http://") or url.startswith("https://")):
            return t
    # Fallback: any non-internal page
    for t in targets:
        url = t.get("url", "")
        if t.get("type") == "page" and not url.startswith("chrome://"):
            return t
    # Last resort: any page
    for t in targets:
        if t.get("type") == "page":
            return t
    return targets[0] if targets else None


async def cdp_connect():
    """Connect to Chrome CDP WebSocket and start screencast."""
    global cdp_ws, latest_frame_event, frame_metadata, browser_size

    latest_frame_event = asyncio.Event()

    target = await get_cdp_target()
    if not target:
        print("[CDP] No browser targets found! Is Chromium running with --remote-debugging-port=9222?")
        return False

    ws_url = target["webSocketDebuggerUrl"]
    print(f"[CDP] Connecting to: {ws_url}")
    print(f"[CDP] Target: {target.get('title', 'unknown')} — {target.get('url', '')}")

    session = aiohttp.ClientSession()
    cdp_ws = await session.ws_connect(ws_url, max_msg_size=10*1024*1024)

    msg_id = 1

    async def send_cdp(method, params=None):
        nonlocal msg_id
        msg = {"id": msg_id, "method": method}
        if params:
            msg["params"] = params
        await cdp_ws.send_json(msg)
        msg_id += 1
        return msg_id - 1

    # Bring tab to front (screencast only works on visible tabs)
    await send_cdp("Page.bringToFront")

    # Force browser viewport to match Tab5 portrait display (720x1280)
    await send_cdp("Emulation.setDeviceMetricsOverride", {
        "width": SCREENCAST_MAX_W,
        "height": SCREENCAST_MAX_H,
        "deviceScaleFactor": 1,
        "mobile": True,
    })
    print(f"[CDP] Browser viewport set to {SCREENCAST_MAX_W}x{SCREENCAST_MAX_H} (portrait)")

    # Start screencast
    await send_cdp("Page.startScreencast", {
        "format": "jpeg",
        "quality": SCREENCAST_QUALITY,
        "maxWidth": SCREENCAST_MAX_W,
        "maxHeight": SCREENCAST_MAX_H,
        "everyNthFrame": 1,
    })
    print(f"[CDP] Screencast started ({SCREENCAST_MAX_W}x{SCREENCAST_MAX_H} @ ~{SCREENCAST_FPS}fps, q={SCREENCAST_QUALITY})")

    # Process CDP messages in background
    asyncio.create_task(cdp_message_loop(send_cdp))
    return True


async def cdp_message_loop(send_cdp_fn):
    """Process incoming CDP messages, extract screencast frames."""
    global latest_frame, frame_metadata, browser_size, frame_count, fps

    fps_start = time.time()
    fps_count = 0

    while True:
        try:
            msg = await cdp_ws.receive()
            if msg.type == aiohttp.WSMsgType.TEXT:
                data = json.loads(msg.data)
                method = data.get("method", "")

                if method == "Page.screencastFrame":
                    params = data["params"]
                    import base64
                    latest_frame = base64.b64decode(params["data"])
                    frame_metadata = params.get("metadata", {})
                    browser_size = (
                        frame_metadata.get("deviceWidth", SCREENCAST_MAX_W),
                        frame_metadata.get("deviceHeight", SCREENCAST_MAX_H),
                    )

                    # Acknowledge frame so CDP sends the next one
                    session_id = params.get("sessionId", 0)
                    await send_cdp_fn("Page.screencastFrameAck", {"sessionId": session_id})

                    latest_frame_event.set()
                    latest_frame_event.clear()

                    fps_count += 1
                    frame_count += 1
                    elapsed = time.time() - fps_start
                    if elapsed >= 5.0:
                        fps = fps_count / elapsed
                        print(f"[CDP] {fps:.1f} FPS, frame: {len(latest_frame)//1024}KB, "
                              f"viewport: {browser_size[0]}x{browser_size[1]}")
                        fps_count = 0
                        fps_start = time.time()

            elif msg.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                print("[CDP] WebSocket closed/error")
                break
        except Exception as e:
            print(f"[CDP] Message loop error: {e}")
            break


async def mjpeg_handler(request):
    """Serve MJPEG stream from CDP screencast frames."""
    response = web.StreamResponse(
        status=200,
        reason='OK',
        headers={
            'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
            'Cache-Control': 'no-cache',
            'Connection': 'keep-alive',
        }
    )
    await response.prepare(request)
    print(f"[MJPEG] Client connected: {request.remote}")

    try:
        while True:
            if latest_frame:
                jpeg = latest_frame
                await response.write(
                    b'--frame\r\n'
                    b'Content-Type: image/jpeg\r\n'
                    b'Content-Length: ' + str(len(jpeg)).encode() + b'\r\n'
                    b'\r\n' + jpeg + b'\r\n'
                )
            # Wait for next frame or timeout
            try:
                await asyncio.wait_for(latest_frame_event.wait(), timeout=0.2)
            except asyncio.TimeoutError:
                pass
    except (ConnectionResetError, ConnectionError):
        print(f"[MJPEG] Client disconnected: {request.remote}")

    return response


async def touch_ws_handler(request):
    """WebSocket endpoint for touch events from Tab5.

    Receives: {"t":[{"x":123,"y":456,"s":7}]}
    Maps Tab5 coordinates (720x1280) to browser viewport coordinates.
    Dispatches as CDP Input.dispatchMouseEvent.
    """
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    print(f"[WS] Touch client connected: {request.remote}")

    msg_id = 10000

    async def dispatch_mouse(x, y, event_type, button="left"):
        nonlocal msg_id
        if not cdp_ws:
            return
        await cdp_ws.send_json({
            "id": msg_id,
            "method": "Input.dispatchMouseEvent",
            "params": {
                "type": event_type,
                "x": x,
                "y": y,
                "button": button,
                "clickCount": 1 if event_type == "mousePressed" else 0,
            }
        })
        msg_id += 1

    last_touch = None
    last_touch_time = 0
    release_task = None

    async def auto_release():
        """Auto-release mouse if no touch for 150ms (Tab5 doesn't send release events)."""
        nonlocal last_touch
        await asyncio.sleep(0.15)
        if last_touch:
            await dispatch_mouse(last_touch[0], last_touch[1], "mouseReleased")
            print(f"[TOUCH] Auto-release at ({last_touch[0]},{last_touch[1]})")
            last_touch = None

    async for msg in ws:
        if msg.type == aiohttp.WSMsgType.TEXT:
            try:
                data = json.loads(msg.data)
                touches = data.get('t', [])
                if not touches:
                    # Explicit touch release
                    if release_task:
                        release_task.cancel()
                    if last_touch:
                        await dispatch_mouse(last_touch[0], last_touch[1], "mouseReleased")
                        last_touch = None
                    continue

                t = touches[0]  # Primary touch
                tx, ty = t.get('x', 0), t.get('y', 0)

                # Map Tab5 coords (720x1280) to browser viewport
                bw, bh = browser_size
                bx = int(tx * bw / SCREENCAST_MAX_W)
                by = int(ty * bh / SCREENCAST_MAX_H)

                # Cancel pending auto-release
                if release_task:
                    release_task.cancel()

                if last_touch is None:
                    # Touch down
                    await dispatch_mouse(bx, by, "mousePressed")
                else:
                    # Touch move
                    await dispatch_mouse(bx, by, "mouseMoved")

                last_touch = (bx, by)

                # Schedule auto-release (fires if no more touch events arrive)
                release_task = asyncio.create_task(auto_release())

                print(f"[TOUCH] Tab5({tx},{ty}) → Browser({bx},{by})")

            except json.JSONDecodeError:
                pass
        elif msg.type == aiohttp.WSMsgType.ERROR:
            print(f"[WS] Error: {ws.exception()}")

    # Touch release on disconnect
    if last_touch:
        await dispatch_mouse(last_touch[0], last_touch[1], "mouseReleased")

    print(f"[WS] Touch client disconnected: {request.remote}")
    return ws


async def index_handler(request):
    """Status page."""
    status = "connected" if cdp_ws and not cdp_ws.closed else "disconnected"
    return web.Response(
        text=f"Dragon Streaming Server (TinkerClaw)\n"
             f"CDP: {status} (port {CDP_PORT})\n"
             f"MJPEG: http://{request.host}/stream\n"
             f"Touch WS: ws://{request.host}/ws/touch\n"
             f"Frames: {frame_count}, FPS: {fps:.1f}\n"
             f"Browser viewport: {browser_size[0]}x{browser_size[1]}\n",
        content_type='text/plain')


async def on_startup(app):
    """Connect to CDP on server start."""
    for attempt in range(10):
        try:
            ok = await cdp_connect()
            if ok:
                return
        except Exception as e:
            print(f"[CDP] Connection attempt {attempt+1} failed: {e}")
        print(f"[CDP] Retrying in 2s...")
        await asyncio.sleep(2)
    print("[CDP] WARNING: Could not connect to Chrome. MJPEG stream will be empty until Chrome is available.")


app = web.Application()
app.router.add_get('/', index_handler)
app.router.add_get('/stream', mjpeg_handler)
app.router.add_get('/ws/touch', touch_ws_handler)
app.on_startup.append(on_startup)

if __name__ == '__main__':
    print(f"Dragon Streaming Server (TinkerClaw)")
    print(f"  CDP: ws://{CDP_HOST}:{CDP_PORT}")
    print(f"  MJPEG: http://0.0.0.0:{PORT}/stream")
    print(f"  Touch: ws://0.0.0.0:{PORT}/ws/touch")
    print(f"  Screencast: {SCREENCAST_MAX_W}x{SCREENCAST_MAX_H} q={SCREENCAST_QUALITY}")
    web.run_app(app, host=HOST, port=PORT, print=None)
