---
title: Video calling
sidebar_label: Video calling
---

# Video calling

Two-way video + audio between Tab5 ↔ Dragon ↔ peer (another Tab5 or a browser). Dragon is a dumb broadcast relay — no transcoding, no recording.

## What's in the loop

```
┌─────────┐                    ┌──────────┐                    ┌─────────┐
│  Tab5   │ ─VID0+AUD0─ JPEG  │  Dragon  │ ─VID0+AUD0─ JPEG  │  Peer   │
│         │ ─binary WS frames→│ broadcast│←─binary WS frames─│ Tab5 or │
│         │                    │  relay   │                    │ browser │
└─────────┘                    └──────────┘                    └─────────┘
   camera                                                       camera +
   + mic                                                          mic
```

Wire format:

- **Video** — `"VID0"` (4 bytes magic) + 4-byte BE u32 length + JPEG bytes
- **Audio** — `"AUD0"` (4 bytes magic) + 4-byte BE u32 length + raw 16 kHz mono int16 PCM
- Untagged binary frames remain mic PCM bound for STT (legacy path)

## Start a call from Tab5

1. From the home screen, open the nav sheet
2. Tap **Call**
3. The video pane opens fullscreen with a 240×135 PIP of your local camera
4. The other end (peer Tab5 or browser) has to also be connected and call back

## Start a call from the browser

Open `http://<dragon>:3502/call` in a browser on the LAN (Chrome/Edge/Safari/Firefox all work).

The page is `dragon_voice/static/call.html` — minimal HTML/JS:

- `getUserMedia` for camera + mic
- Web Audio API captures 16 kHz mono PCM
- WebSocket streams VID0 + AUD0 frames to Dragon
- Inbound VID0 frames decoded into a `<canvas>`; AUD0 frames played back through `AudioBufferSourceNode`

Grant camera + mic permissions when the browser prompts.

## End the call

- Tab5: tap the **red "End Call" pill** at the bottom of the video pane
- Browser: close the tab, or click *End Call*
- Dragon: doesn't need to do anything — it just stops relaying when one end disconnects

The atomic helper `voice_video_end_call()` in firmware reverts the voice mode (mic flips back to STT-bound PCM, camera closes, video pane hides) — no caller has to coordinate the three subsystems.

## Multi-party

Dragon's relay supports N participants. If three Tab5s + a browser are all connected to the same Dragon and they all start calling, every frame from any one of them is broadcast to the other three. (No per-pair routing — just fan-out.)

There's no UI to manage participants; it's a free-for-all relay. If you want directed calls, that's a future feature (filed as an open issue).

## Frame rate

Tab5 uplink — ~10 fps at JPEG quality 60. Higher rates work (the hardware JPEG encoder is fast) but the uplink WS bandwidth gets tight on Wi-Fi 6.

Dragon doesn't transcode. Whatever Tab5 sends, peers see at the same rate.

## Audio sync

Loose. There's no PTS / sync mechanism between VID0 and AUD0 frames; both are best-effort. Audio drift is usually &lt;100 ms but can grow over a long call. If sync becomes a problem, end and re-call.

## Debug injection

Test the downlink decode path without a second device:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
     --data-binary @test.jpg \
     http://<dragon>:3502/api/video/inject
```

Pushes a JPEG into the relay as if it had come from a Tab5. Useful for harness tests.
