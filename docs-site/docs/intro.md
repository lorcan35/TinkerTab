---
title: Welcome
sidebar_label: Welcome
---

# Welcome to TinkerTab

**TinkerTab** is the firmware that turns the [M5Stack Tab5](https://shop.m5stack.com/products/m5stack-tab5-iot-development-kit) into a voice-first home assistant. Tab5 is the face you talk to. The **Dragon Q6A server** does the thinking — speech-to-text, the LLM, text-to-speech, memory, skills. Everything sensitive stays on hardware you own.

![TinkerTab home screen](/img/home.jpg)

## Pick a track

This site is layered for two audiences. Pick whichever fits.

### 🟢 I just want to use it

You've been handed (or built) a Tab5 + Dragon. You want it on your shelf, talking back, telling timers. Start here:

1. [Power on the Tab5](/docs/get-started/power-on)
2. [Connect to Dragon](/docs/get-started/connect-to-dragon)
3. [First voice command](/docs/get-started/first-voice-command)

You'll be in conversation in five minutes.

### ⚙️ I want to look under the hood

You're a developer, a security researcher, or you want to add a skill, a sensor, or a new LLM. Start here:

1. [Tab5 firmware overview](/docs/architecture/tab5-firmware) — what runs on the device
2. [Dragon server overview](/docs/architecture/dragon-server) — what runs on the brain
3. [WebSocket protocol](/docs/firmware-reference/ws-protocol) — how they talk
4. [Skill SDK](/docs/dragon-reference/skill-sdk) — write a Python skill, see it on Tab5

## What this isn't

- **Not a polished consumer product.** Tab5 + Dragon are enthusiast hardware. You'll need to be comfortable with USB flashing, SSH, systemd, and at least one programming language.
- **Not a hardened appliance.** Default deployment trusts your LAN, runs without secure boot, persists conversation history in plaintext. Fine for "home AI in my house", not fine for a public deployment.
- **Not a managed service.** No cloud control plane, no auto-updates, no support hotline. When something breaks, you'll be reading logs.
- **Not voice-only.** There's a chat screen, a camera, a notes app, a video-call client. Voice is the *most important* loop, not the only one.

## Help

- File issues: [TinkerTab](https://github.com/lorcan35/TinkerTab/issues) for the firmware, [TinkerBox](https://github.com/lorcan35/TinkerBox/issues) for the server.
- Stuck on terminology? [Glossary](/docs/glossary) covers the jargon.
- Something broken? [Troubleshooting](/docs/troubleshooting).
