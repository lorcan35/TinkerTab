---
title: Microphones & speaker
sidebar_label: Microphones & speaker
---

# Microphones & speaker

The Tab5's audio stack is the most subtle hardware on the board. Both mic and speaker run on a *single* I2S peripheral with mixed STD-TX / TDM-RX, which the ESP32-P4 supports natively but few example projects use.

## The microphone array

- **ES7210** quad-mic ADC
- 4 microphones, all front-facing, in a small linear array
- TDM 4-slot mode, 48 kHz native, 16-bit
- I2S_NUM_1, RX direction
- Slot 0 is the primary mic (used for STT). Slots 1-3 are spares for AEC/beamforming experiments.

In code, the mic capture path is:

```
ES7210 ─I2S TDM─→ esp_codec_dev → mic.c → 48 kHz int16 → 3:1 downsample → 16 kHz frames
                                                       │
                                                       ↓
                                        voice.c WS uplink → Dragon STT
```

The downsample is plain decimation with a low-pass front; STT only needs 16 kHz mono.

## The speaker

- **ES8388** stereo codec (only mono speaker connected on Tab5)
- 3 W rear-firing speaker
- I2S_NUM_1, TX direction, **STD Philips** mode (NOT TDM)
- 48 kHz native, downsampled from 16 kHz via 1:3 upsample on the way out

TTS audio comes off Dragon at 16 kHz (Piper) or 24 kHz (OpenRouter), gets resampled on Dragon to a fixed 16 kHz before sending, then 1:3 upsampled on Tab5 for playback.

:::warning Don't write ES8388 registers manually
Use `es8388_codec_new()` from `esp_codec_dev` library — never custom register init. The ESP-IDF official driver has 5 register differences from naive init that prevent any audio from coming out. (Issue #46.)
:::

## The speaker buzzes at boot

Fixed in firmware: IO Expander 1 P1 (`SPK_EN`) is now driven LOW at boot, then HIGH when audio is ready. If you see buzzing, you're running pre-2026 firmware — flash the latest.

## Mic mute

The Settings overlay has a **master mic mute** toggle (NVS key `mic_mute`, 0 or 1). When mute is on, `voice_start_listening()` refuses to start with a toast — the mic is fully gated, not just software-muted.

The hardware orange LED next to the speaker grille lights up when the mic is *active* (recording or transcribing). It is OFF when the mic is muted or idle.

## Volume

NVS key `volume` (0–100, default 70). Adjustable in Settings → Audio → Volume, or via the side volume rocker, or via the debug HTTP endpoint:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/audio?volume=80"
```

## Audio playback latency

There's an audible ~200 ms delay between Dragon emitting a TTS chunk and Tab5 playing it. That's the upsample buffer + producer-consumer drain task. It's well-tuned now (was 1.5× speed before the upsample buffer fix) but you'll hear it on short replies.
