---
title: Take a photo
sidebar_label: Take a photo
---

# Take a photo

The Tab5 has a 2 MP camera that does live preview, JPEG capture, motion-JPEG video recording, and two-way video calls. Here's the basic photo flow.

![Tab5 camera screen](/img/camera.jpg)

## Open the camera screen

From the home screen:

1. Tap the chevron at the bottom to open the **nav sheet**
2. Tap **Camera**

The camera screen shows a live 1280×720 viewfinder at ~30 fps. Bottom bar has the controls.

## Take a still photo

Tap the **white circular shutter button** in the centre of the bottom bar. The viewfinder briefly flashes white; the photo saves to the SD card as `/sdcard/IMG_NNNN.jpg` with a 4-digit auto-incrementing counter.

:::tip Counter persistence
The counter is recovered from existing files on the SD card at boot, so reinserting the card after editing files won't overwrite anything.
:::

The capture also fires a `camera.capture` observability event with the saved path — useful if you're scripting Tab5 from the dashboard or e2e harness.

## Send a photo to chat

Want the LLM to look at the photo? From the chat overlay:

1. Open **Chat** from the nav sheet (or tap the chevron and pick *Chat*)
2. Tap the **camera icon** in the input bar at the bottom
3. The camera screen opens with a special **"Send photo"** button instead of the regular shutter
4. Frame your shot, tap *Send photo*

What happens next:

- The photo is captured and rotated (per `cam_rot` NVS setting)
- It's auto-uploaded to Dragon's `/api/media/upload` endpoint
- Dragon stores it in `~/.tinkerclaw/media/` (24-hour TTL)
- A `user_image` WS event is announced
- The chat overlay threads the image inline as a user message
- The LLM (which must be vision-capable in your active voice mode) sees the image and responds

You can ask follow-ups: *"what colour was the chair?"* — Dragon's MessageStore keeps the image in conversation context so cross-modal continuity works.

## Rotate the camera

If you've mounted the Tab5 on a tripod sideways, the viewfinder is rotated. Two ways to fix it:

- **Settings → Camera → Rotation** — pick 0° / 90° / 180° / 270° (CW)
- **In-viewfinder "Rot" button** — bottom-right corner of the camera screen, cycles through the 4 options

The rotation is applied in software after capture, so saved photos are stored at the rotated orientation (matches what you saw on screen).

## Record a video

Tap the **red-bordered REC pill button** to the right of the shutter. Records concatenated motion-JPEG to `/sdcard/VID_NNNN.MJP` at 5 fps, JPEG quality 60. Tap REC again to stop.

:::note 3-char extension
The file extension is `.MJP` (not `.mjpeg`) because `CONFIG_FATFS_LFN_NONE=1` forces 8.3 short names. ffmpeg + VLC sniff the magic bytes and play `.MJP` files natively: `ffplay -f mjpeg VID_NNNN.MJP`.
:::

Hard cap: 1500 frames (5 minutes at 5 fps). Recordings auto-stop at the cap. On stop, the file is auto-uploaded to Dragon's media endpoint and announced to chat — same path as photo uploads.

## Use the camera for video calls

See [Video calling](/docs/tasks/video-calling).
