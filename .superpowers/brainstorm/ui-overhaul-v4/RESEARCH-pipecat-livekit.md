# Pipecat vs LiveKit — research brief for TinkerClaw

Delivered 2026-04-20 by background research agent. Decision: **partial-adopt pipecat patterns, skip full LiveKit. Watch Xiaozhi-ESP32 for protocol cues.**

## TL;DR

- **Pipecat** (daily-co/pipecat): Python framework, frame-based pipeline, transport-agnostic. Speaks raw WebSocket too. Official `pipecat-esp32` SDK. Strong on VAD endpointing / interrupt handling / backpressure — exactly our stability pain points.
- **LiveKit** (livekit/livekit + livekit/agents): WebRTC SFU + Python/Node agent framework. Opus + AEC via WebRTC. Ships an ESP32-P4 client SDK (Developer Preview). Heavy for a single-client setup; full ICE/TURN/SFU infrastructure overhead.

## Recommendation

**Don't rewrite.** Keep aiohttp WebSocket + widget protocol. Steal these specific design patterns:

1. **Frame-based pipeline with typed frames** — replace ad-hoc `msg_type == "llm"` dispatch on Dragon with typed `AudioRawFrame / TranscriptionFrame / LLMResponseStartFrame / TTSStartedFrame / BotInterruptionFrame`. Our widget_* messages become additional frame types.
2. **Interruption as first-class frame** flowing upstream. Cancels in-flight LLM + TTS on barge-in and truncates bot transcript to "what the user actually heard" (LiveKit does this same truncation). Directly fixes our mid-turn interruption jankiness.
3. **Semantic turn detection on STT partials** (LiveKit's open-weights turn detector). Runs on the partial transcript, not just silence. Much better than our current ad-hoc silence frames. Drop-in idea, not a framework swap.
4. **OPUS @ 60 ms / 16 kHz / mono** frames. Cuts upload from ~256 kbps PCM to ~24 kbps. Kills ngrok bandwidth pain. `esp-opus` is well-trodden on ESP32-P4 (Xiaozhi ships it).
5. **Formalise control/data plane split** — JSON text frames for widgets/state, binary frames for Opus audio, same WS. We're almost there; versioning the protocol is a small step.

## What NOT to do

- **Don't adopt full LiveKit.** SFU/TURN/ICE for single-client is pure overhead, and their ESP32 SDK is Developer Preview — would throw out our binary widget protocol.
- **Don't swap aiohttp for pipecat wholesale.** We already have tool-calling, memory service, cost receipts, TinkerClaw gateway routing. Pipecat's LLM/tool abstractions are opinionated; memory story is thinner than openclaw.
- **Don't chase WebRTC for NAT traversal.** Tab5 ↔ Dragon are LAN most of the time. ngrok pain is remote-access specific and Tailscale/WireGuard solves it without touching the media stack.

## Related projects worth cloning / studying

- **[Xiaozhi-ESP32](https://github.com/78/xiaozhi-esp32)** — closest structural twin. ESP32-C3/S3/**P4**. Opus over WebSocket (unified) *or* MQTT+UDP dual-plane. JSON text + binary audio frame split. 70+ boards, MCP tool calling. Strongly worth mirroring their protocol layout for our Opus upgrade.
- [espressif/esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution) — Espressif's WebRTC stack. Watch for pipecat backend integration if we ever move to WebRTC on P4.
- [Willow](https://heywillow.io/) — still alive early 2026 but S3-BOX-bound and Home-Assistant-pipeline-focused. Less generalisable than Xiaozhi.
- [Home Assistant Voice PE](https://www.home-assistant.io/voice_control/) — good for local intent handling; not for free-form LLM chat.

## Sources

- https://github.com/pipecat-ai/pipecat (README)
- https://github.com/livekit/livekit (README)
- https://docs.livekit.io/agents/
- https://docs.livekit.io/agents/build/turns/
- https://docs.pipecat.ai/server/services/transport/small-webrtc
- https://www.daily.co/blog/you-dont-need-a-webrtc-server-for-your-voice-agents/
- https://github.com/pipecat-ai/pipecat-esp32
- https://github.com/livekit/client-sdk-esp32
- https://github.com/78/xiaozhi-esp32
- https://github.com/espressif/esp-webrtc-solution/issues/33
- https://heywillow.io/ + https://heywillow.io/hardware/
- https://www.home-assistant.io/voice_control/

## Actionable next-sprint items derived

| Priority | Item | Effort | Source |
|---|---|---|---|
| P0 | Interrupt frame type + cancel-on-barge-in | ~150 LOC (Tab5 + Dragon) | pipecat |
| P0 | Opus encoder on mic path, Opus decoder on TTS path | ~300 LOC + flash size | Xiaozhi |
| P1 | Semantic turn detection (open-weights model or STT-partial heuristic) | ~100 LOC Dragon | LiveKit |
| P1 | Typed frame taxonomy refactor on Dragon pipeline.py | ~200 LOC | pipecat |
| P2 | Move remote-access off ngrok to Tailscale | ops, ~0 LOC | our own |
