---
audience: tinkerer
type: how-to
prerequisites: A booted [Tab5](../../GLOSSARY.md) with an SD card inserted
last-verified: 2026-05-29
est-time: 5 min
---
# How to use the camera

Use this when you want to take a photo, record a short video, or send an image
into a chat from a [Tab5](../../GLOSSARY.md). The Tab5 has a 2 MP SC202CS camera
behind a MIPI-CSI viewfinder running at 1280×720. Photos and videos are written
to the SD card; from a chat the capture can also upload to the
[Dragon](../../GLOSSARY.md) so it threads inline.

Assumes an SD card is inserted (captures need somewhere to write).

## Steps

### Take a photo

1. Open the **Camera** screen (nav sheet → **Camera**, or
   `POST /navigate?screen=camera` over the [debug server](../reference/debug-server.md)).
2. Tap the **white circular shutter** button in the center of the bottom control
   bar.
3. The photo is saved as `/sdcard/IMG_NNNN.jpg` (a 4-digit counter that resumes
   from existing files on boot).

### Rotate the frame

If the camera is mounted at an angle, set the software rotation. Tap the **Rot**
button in the viewfinder to cycle 0° → 90° → 180° → 270° (clockwise). The choice
persists to the `cam_rot` [NVS](../../GLOSSARY.md) key and applies to every
capture before display/upload. You can also set it in **Settings** or via
`POST /settings -d '{"cam_rot":1}'`.

### Record a video

1. On the Camera screen, tap the **red-bordered REC pill** (right of the
   shutter).
2. Recording captures motion-JPEG to `/sdcard/VID_NNNN.MJP` at 5 fps. The
   `.MJP` extension is required — the FAT32 filesystem is in 8.3 short-name mode,
   so `.mjpeg` would fail.
3. Tap **REC** again to stop. Recording auto-stops at the 1500-frame cap
   (5 minutes).
4. Play it back with `ffplay -f mjpeg VID_NNNN.MJP` or VLC (both sniff the JPEG
   magic bytes).

### Send a photo into a chat

Open the camera from the **chat overlay** with the **Send photo** button. After
you capture, the image auto-uploads to the Dragon's `/api/media/upload` and a
`user_media` [WebSocket](../../GLOSSARY.md) event threads it inline in the
conversation.

## Verify it worked

Confirm a capture fired by watching the observability event ring on the debug
server:

```bash
export TOKEN="abcdef1234567890abcdef1234567890"
curl -s -H "Authorization: Bearer $TOKEN" "http://<ip>:8080/events?since=0" \
     | python3 -m json.tool
# → photo:  {"kind":"camera.capture","detail":"/sdcard/IMG_0001.jpg"}
# → video:  {"kind":"camera.record_start","detail":"/sdcard/VID_0001.MJP"}
#           {"kind":"camera.record_stop","detail":"...MJP frames=30 bytes=..."}
```

Or pull a live frame straight off the camera (no SD round-trip):

```bash
curl -s -H "Authorization: Bearer $TOKEN" -o frame.jpg http://<ip>:8080/camera
```

## Troubleshooting

- **Camera screen is black / no frame** → The SC202CS needs
  `CONFIG_CAMERA_SC202CS=y` in `sdkconfig`. If you built without it the sensor
  will not initialize. The sensor is at SCCB address `0x36` (not `0x30`).
- **Capture button does nothing** → No SD card, or it is full. Check
  `GET /info` and the storage service. Captures need a writable
  `/sdcard`.
- **Video record crashes or won't start a second time** → The ESP32-P4 has a
  single hardware JPEG engine shared between recording and the video-call
  streamer; it is mutex-guarded. Do not record while a video call is active.
- **Image looks rotated wrong** → Adjust `cam_rot` (0–3) — see the rotate step.
- **Chat upload never threads** → Confirm the Dragon is reachable
  (`/voice` shows `connected: true`) — the upload POSTs to the Dragon's
  `/api/media/upload`.

## See also

- [Debug server reference](../reference/debug-server.md) — `/camera`,
  `/navigate`, `/events`.
- [NVS settings reference](../reference/nvs-settings.md) — `cam_rot`.
- [Hardware reference](../reference/hardware.md) — the SC202CS sensor + JPEG
  engine constraint.
