---
title: Video calling stack
sidebar_label: Video calling stack
---

# Video calling stack

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Tab5 ↔ Dragon ↔ peer (Tab5 or browser). Dragon is a dumb broadcast relay — no transcoding, no recording, no per-pair routing. Just fan-out.

## Wire format (binary WS frames)

Two magic-tagged formats added to the existing audio path:

| Magic | Direction | Length | Payload |
|-------|-----------|--------|---------|
| `VID0` (0x56494430) | both | 4 byte BE u32 | JPEG frame |
| `AUD0` (0x41554430) | both | 4 byte BE u32 | raw 16 kHz mono int16 PCM |

Untagged binary frames remain raw 16 kHz PCM bound for STT — that's the legacy mic-uplink path and stays magic-less for backward compat.

## Tab5 side modules

[`main/voice_video.{c,h}`](https://github.com/lorcan35/TinkerTab/blob/main/main/voice_video.h)
: Uplink JPEG encode (HW JPEG engine, mutex-guarded — only one engine on the chip), downlink TJPGD decode into an LVGL canvas. Atomic helpers `voice_video_start_call()` / `voice_video_end_call()` wrap mode flip + camera open/close + UI show/hide.

[`main/ui_video_pane.{c,h}`](https://github.com/lorcan35/TinkerTab/blob/main/main/ui_video_pane.h)
: Full-screen video overlay. In-call adds a 240×135 local-camera PIP in the corner + a red "End Call" pill.

`VOICE_MODE_CALL`
: New voice mode that reroutes mic frames through `AUD0` framing instead of untagged STT-bound PCM. When the call ends, the mode reverts to whatever `vmode` was before.

## Dragon side modules

`dragon_voice/video_upstream.py`
: Receives VID0 frames from one Tab5 and **broadcasts verbatim to all other connected clients**. Same fan-out applies to AUD0 frames. No re-encode, no buffering beyond the WS write queue.

`dragon_voice/static/call.html`
: Minimal browser call client. `getUserMedia` for camera + mic, Web Audio API for 16 kHz PCM capture and playback. The web client is just another participant on the relay — Dragon doesn't know or care it's a browser.

`POST /api/video/inject`
: Debug endpoint. Pushes a JPEG into the relay as if it had come from a Tab5. Useful for testing the Tab5 downlink decode path without a second device.

## The browser side at `/call`

When you open `http://<dragon>:3502/call`:

1. JS opens a WS to `/ws/voice` with a Special "browser-call" device_id
2. `getUserMedia({video, audio})` grabs the camera + mic
3. A canvas captures every Nth video frame, JPEG-encodes via `toBlob`, prepends VID0+len, sends as binary WS frame
4. Web Audio captures 16 kHz mono PCM via an `AudioWorkletNode`, prepends AUD0+len, sends
5. Inbound VID0 frames decoded via `<canvas>.drawImage(blob)`; inbound AUD0 played back via `AudioBufferSourceNode`

## Why JPEG + WS instead of WebRTC

- Tab5's ESP32-P4 doesn't have hardware H.264 — only hardware JPEG. Software H.264 wouldn't keep up.
- WebRTC's setup cost (signaling + ICE + DTLS) is significantly higher than just opening one more WS frame
- Dragon as a broadcast relay is ~30 lines of code; a SFU implementation would be thousands

The tradeoff: bandwidth. JPEG is much heavier than H.264. We've measured ~1.5 Mbps per Tab5 uplink at 10 fps quality 60 — fine on Wi-Fi 6, would be tight on Wi-Fi 4.

## Audio sync

Loose. There's no PTS / sync mechanism between VID0 and AUD0 frames; both are best-effort. Audio drift is usually under 100 ms but can grow over a long call. If sync becomes a problem in practice, end and re-call. (We've considered an "audio-led" sync where video frames are dropped to match audio cadence; not worth the complexity for v1.)

## Multi-party

Dragon's relay is N-to-N within a session group. Three Tab5s + a browser = every frame from any one is broadcast to the other three. There's no UI to manage participants; it's a free-for-all relay.

## Hard limits

- **One HW JPEG engine on Tab5** — the recording path and the call uplink share. Mutex-guarded inside `voice_video.c`.
- **Frame rate cap** — Tab5 uplink is 10 fps default. Higher rates work but the WS uplink gets tight on Wi-Fi.
- **No call recording.** Dragon doesn't persist call frames. By design.
