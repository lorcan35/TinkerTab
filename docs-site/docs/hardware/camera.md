---
title: Camera
sidebar_label: Camera
---

# Camera

A 2 MP MIPI-CSI camera, used for stills, video recording, and bidirectional video calls.

## Hardware

- **Sensor** — Smartsens SC202CS 2 MP CMOS
- **Interface** — MIPI-CSI, 1 lane, 576 MHz, RAW8
- **ISP** — ESP32-P4's built-in ISP converts RAW8 → RGB565
- **Driver stack** — `esp_video` + `esp_cam_sensor` from M5Stack
- **Live frame size** — 1280×720 @ 30 fps

:::warning SC202CS at I2C 0x36, NOT 0x30
A common mistake: many Tab5 schematics show "SC2336" sensor but the actual hardware is SC202CS at SCCB I2C address **0x36**. Wrong address → driver init fails with no error → blank viewfinder. Verify with `i2cdetect` on the camera I2C bus if you're hacking the driver.

Also: `CONFIG_CAMERA_SC202CS=y` MUST be set in `sdkconfig.defaults` — the sensor driver won't compile without it.
:::

## Capture flow

```
SC202CS → MIPI CSI → ISP (RAW8→RGB565) → /dev/video0 (V4L2) → ui_camera viewfinder
                                                            │
                                                            ↓
                                                  JPEG encode (HW)
                                                            │
                                                            ↓
                                          /sdcard/IMG_NNNN.jpg or
                                          POST /api/media/upload (Dragon)
```

The JPEG encoder is a **hardware** engine (the ESP32-P4 has one). DMA buffers are allocated lazily on first record-start via `jpeg_alloc_encoder_mem()` — plain `heap_caps_malloc` doesn't satisfy the bit-stream alignment requirement.

## Single shot

Tap the white circle in the camera screen. JPEG saves to `/sdcard/IMG_NNNN.jpg`. NVS-stored counter ensures no overwrites across boots.

## Video recording

Red REC pill records motion-JPEG `.MJP` at 5 fps, quality 60. Hard cap 1500 frames (5 min). On stop, file is auto-uploaded to Dragon. See [Take a photo](/docs/get-started/take-a-photo).

## Software rotation

The viewfinder + saved frames are rotated according to NVS key `cam_rot` (0/1/2/3 for 0°/90°/180°/270° CW). The rotation runs in software after capture; saved JPEGs are stored at the rotated orientation. Settings has a dropdown; the camera screen has a live "Rot" cycle button.

## Two-way video calls

Tab5 ↔ Dragon ↔ peer (another Tab5 or a browser at `/call` on Dragon). Wire format: `"VID0"` magic + 4-byte BE length + JPEG. Same encoder, same upload path, just a higher rate.

[Video calling →](/docs/tasks/video-calling)

## Exposure

Indoor exposure is tuned in firmware via SCCB register writes after sensor init. If you're using Tab5 outdoors and the viewfinder is blown out, the easy fix is to add a graduated ND filter; the proper fix is to add an "outdoors" preset to `camera.c`'s post-init register block.
