---
title: First voice command
sidebar_label: First voice command
---

# First voice command

Time to actually talk to it.

![Voice overlay thinking state](/img/voice-thinking.jpg)

## Push to talk

1. From the home screen, **tap the central orb**
2. The voice overlay slides in. The orb pulses green ("listening")
3. **Hold and speak** — "What time is it?"
4. **Release** when you're done

What happens next:

```
mic   → STT (Moonshine on Dragon)     → "what time is it"
text  → LLM (ministral-3:3b)           → "It's 3:42 PM."
reply → TTS (Piper)                    → audio bytes
audio → playback through speaker
```

The whole loop is 60–90 seconds in Local mode. In Cloud mode it's 3–6 seconds.

## Long-press for dictation

Need to capture a longer thought without the LLM responding? **Long-press** (hold for 1+ seconds before releasing). Tab5 enters **dictation mode**:

- The orb shows a waveform instead of a pulse
- Speech accumulates in a 64 KB transcript buffer
- 5 seconds of silence auto-stops the recording
- Dragon post-processes the transcript: auto-titles it, summarises it, saves it as a note

Open the **Notes** screen from the nav sheet to find your dictated note.

## Cancel mid-turn

Tap the X at the top-right of the voice overlay to cancel. The current turn is aborted; the conversation context is preserved.

## What you can ask

The LLM has access to a small but useful tool palette:

- **`datetime`** — "what time is it", "what's today's date"
- **`web_search`** — "what's the weather forecast" (uses self-hosted SearXNG)
- **`remember`** — "remember my dog's name is Mochi"
- **`recall`** — "what did I tell you about Mochi"
- **`unit_converter`** — "convert 50 miles to km"
- **`weather`** — "what's the weather in Paris"
- **`stock_ticker`** — "AAPL price"
- **`timesense`** — "start a 25 minute pomodoro"
- **`quick_poll`** — "ask me a multiple-choice question about Python"

Plus the regular LLM things — explain a concept, summarise a paragraph, write a poem, translate. In Local mode the small model (`ministral-3:3b`) is best for short turns and tool use; in Cloud mode you can pick a frontier model from Settings.

## When the LLM uses a tool

You'll see a small bubble: *"Looking that up…"* with a spinning dots. The LLM calls the tool, the tool result is injected back into the conversation, the LLM continues its response. Up to 3 tool calls per turn.

Tool calls are logged on the [Dragon dashboard](/docs/dragon/logs-monitoring) under the Logs tab.

## What if it doesn't hear you?

- The voice overlay has a live RMS meter — if it's flat-lined, the mic isn't picking you up
- Settings → Audio → check that **Mic mute** is off
- Move closer to the device. The 4-mic array is good but not magic.
- If you see "Voice WS not connected" on the orb, see [Connect to Dragon](/docs/get-started/connect-to-dragon)

[Voice modes →](/docs/tasks/voice-modes)
