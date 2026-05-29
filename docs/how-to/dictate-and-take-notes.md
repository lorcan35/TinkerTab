---
audience: tinkerer
type: how-to
prerequisites: A booted [Tab5](../../GLOSSARY.md) connected to a [Dragon](../../GLOSSARY.md), SD card inserted
last-verified: 2026-05-29
est-time: 5 min
---
# How to dictate and take notes

Use this when you want to capture a longer thought hands-free — dictate a few
sentences and have the [Tab5](../../GLOSSARY.md) transcribe them, generate a
title and summary, and file them in Notes. Unlike a normal voice turn (which is
a short question/answer), **dictation** is STT-only: no LLM reply plays, the
transcript is the product.

Assumes the Tab5 is connected to a [Dragon](../../GLOSSARY.md) (dictation uses
the Dragon's speech-to-text) and an SD card is inserted (for offline recording
fallback).

## Steps

### Dictate from anywhere

1. **Long-press** the mic/orb. This enters dictation mode and sends
   `{"type":"start","mode":"dictate"}` to the Dragon.
2. Speak — the orb tints and a **live waveform** tracks your voice. Partial
   transcript text (`stt_partial` frames) appears as you talk, accumulating in a
   64 KB buffer.
3. **Stop** by pausing. After ~5 seconds of silence the device auto-stops (an
   adaptive VAD calibrates the noise floor first). You can also tap stop.
4. The Dragon post-processes the transcript and returns a `dictation_summary`
   with an auto-generated **title + summary**. The note is saved.

### Dictate from the Notes screen

1. Open **Notes** (nav sheet → **Notes**).
2. Tap **Record** to start the same dictation flow, scoped to a new note.
3. Stop the same way. The note lands in the timeline with its title, summary,
   and a play button for the captured audio.

### Watch the pipeline state

Dictation runs a small state machine: `IDLE → RECORDING → UPLOADING →
TRANSCRIBING → SAVED` (or `FAILED`). The orb body tints per state and a caption
shows the elapsed `M:SS` while recording. Read it live:

```bash
export TOKEN="abcdef1234567890abcdef1234567890"
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/dictation_pipeline \
     | python3 -m json.tool
# → {"state":"RECORDING","reason":"NONE","started_ms":52065, ...}
```

## Verify it worked

A successful dictation leaves a `SAVED` state and a note row. Check the pipeline
state went green and that the note exists in the Notes timeline. The `reason`
field carries the failure detail when `state` is `FAILED` (one of `CANCELLED`,
`NETWORK`, `EMPTY`, `AUTH`, `NO_AUDIO`, `TOO_LONG`).

## Troubleshooting

- **Long-press refuses with a toast** → The mic is already busy (a voice turn or
  another dictation is in flight), or `mic_mute` is set in
  [NVS](../../GLOSSARY.md). Wait for the orb to return to idle, or clear
  `mic_mute`.
- **Note row shows a red FAILED chip** → Read the reason chip — `AUTH` means the
  transcribe request was missing its bearer header (a historic bug, fixed); `NETWORK`
  means the Dragon was unreachable; `NO AUDIO` / `EMPTY` means nothing was
  captured; `TOO LONG` means it hit the 5-minute recording cap. Tap the per-row
  **Retry** button (amber) to re-run a FAILED-with-audio note.
- **Recording never auto-stops** → The adaptive VAD could not find a quiet
  baseline (noisy room). Tap stop manually; the 5-minute hard cap will also
  stop a runaway recording.
- **Counter stuck on "N unprocessed"** → FAILED-with-audio notes that the queue
  refuses to retry are not counted as unprocessed (fixed in a prior wave). Use
  the top-bar **CLEAR N FAILED** button to clear them.

## See also

- [Your first voice conversation](../tutorials/your-first-voice-conversation.md)
- [Switch voice modes](switch-voice-modes.md) ·
  [Debug server reference](../reference/debug-server.md) (`/dictation_pipeline`)
- [The voice pipeline](../explanation/the-voice-pipeline.md) — how STT, LLM, and
  TTS chain together.
