---
title: Glossary
sidebar_label: Glossary
---

# Glossary

Two FAQ stubs landed in PR 2 because user-facing pages link here. The full alphabetical glossary lands in PR 3 — merged from TinkerTab's `GLOSSARY.md` and TinkerBox's `GLOSSARY.md`.

## ESP32-CAM

A different microcontroller from M5Stack and Espressif's ESP32 family — *not* a Grove sensor. It has Grove ports for plugging *other* peripherals into it; it doesn't itself act as an I2C slave that Tab5's Port A could read from.

If you want to use an ESP32-CAM with Tab5, you'd need either custom firmware on the cam exposing an I2C-slave protocol, a UART bridge over a different pinout, or WiFi-based streaming (which duplicates K144's role).

For passive Grove I2C sensors that *do* work with Tab5's Port A, see [Grove Port A](/docs/hardware/grove-port-a).

## Wake word

A short phrase the device listens for *before* you speak the actual command — like "Hey Siri" or "Alexa". TinkerTab does **not** have a wake word in the shipped firmware.

Why: a custom WakeNet model would have to be procured from Espressif, the TDM slot mapping for AEC reference would have to be re-audited, and the partition table would need a 3 MB SPIFFS slot for the model. The previous attempt was retired in TT #162 after months of TDM blocker investigation.

The current product instead uses **push-to-talk**: tap the orb on the home screen, hold while speaking, release. No false-fire risk; no always-listening privacy concern.

A future revival path is filed in memory under "Sherpa-onnx KWS open-vocab" — open-vocabulary keyword spotting on the K144 NPU, which sidesteps the original blocker.

---

:::info More terms coming
The full alphabetical glossary lands in PR 3. Until then, the [TinkerTab GLOSSARY.md](https://github.com/lorcan35/TinkerTab/blob/main/GLOSSARY.md) and [TinkerBox GLOSSARY.md](https://github.com/lorcan35/TinkerBox/blob/main/GLOSSARY.md) are the source of truth.
:::
