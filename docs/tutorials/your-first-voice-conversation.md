---
audience: tinkerer
type: tutorial
prerequisites: A flashed, booted [Tab5](../../GLOSSARY.md) connected to Wi-Fi and a [Dragon](../../GLOSSARY.md) (finish [Get started](getting-started.md) first)
last-verified: 2026-05-29
est-time: 15 min
---
# Your first voice conversation

By the end you will have held a real back-and-forth conversation with your
[Tab5](../../GLOSSARY.md): asked a question out loud, heard it answer, asked a
follow-up that builds on the first, and dictated a longer note. This is a
learning-by-doing walkthrough; every step has a visible result so you know it
worked.

This picks up where [Get started](getting-started.md) leaves off — there you
flashed the firmware and confirmed a single voice turn. Here you learn the
day-to-day rhythm of talking to the device.

## Before you start

- A **Tab5** that boots to the home screen with the breathing orb.
- It is **on Wi-Fi** and connected to a **Dragon** (the status reads connected).
- The speaker volume is up — check **Settings → Audio** if you hear nothing.

## Steps

1. **Start listening.** With the K144/[TinkerON](../../GLOSSARY.md) module
   present, just say **"Hey Tinker."** Without it, **tap the orb** on the home
   screen.
   _You should see:_ the orb shift to its **LISTENING** state (it leans/ripples)
   and the say-pill prompt below it change to indicate it is hearing you.

2. **Ask a question.** Say something with a clear answer, e.g.
   _"What's the capital of France?"_ then stop talking.
   _You should see:_ the orb move to **PROCESSING** while the
   [Dragon](../../GLOSSARY.md) transcribes your speech and runs the LLM, then to
   **SPEAKING** as the answer plays through the speaker.

3. **Wait for it to finish, then ask a follow-up.** Say **"Hey Tinker"** again
   (or tap the orb) and ask something that depends on the first answer, e.g.
   _"And how many people live there?"_
   _You should see:_ the answer reference the previous turn — the conversation
   has memory because the Dragon keeps a session. The orb cycles
   LISTENING → PROCESSING → SPEAKING → READY again.

4. **Cancel a turn you didn't mean.** Start listening, then tap the **stop**
   button (or the X) under the orb before you finish.
   _You should see:_ the orb snap straight back to **READY/IDLE** — no answer
   plays. This is your escape hatch any time a turn goes sideways.

5. **Type instead of talk.** Open the **Chat** overlay, type a message, and send
   it.
   _You should see:_ your text appear as a bubble and the answer stream back
   in — same conversation context as voice, it just skips speech-to-text.

6. **Dictate a longer note.** **Long-press** the mic/orb to enter dictation
   mode and speak a few sentences, then pause.
   _You should see:_ a live waveform while you talk, partial transcript text
   appearing, and after ~5 seconds of silence it auto-stops. The Dragon then
   returns a generated **title + summary** and the note is saved.

## What you built

You now have the full conversational loop in muscle memory: wake or tap to
start, speak, let it process and answer, follow up with context, cancel when
needed, type when quiet, and dictate when you have a lot to say. A healthy
multi-turn session looks like this on the serial log (115200 baud), once per
turn:

```
I (xxxx) voice: state IDLE -> LISTENING
I (xxxx) voice: state LISTENING -> PROCESSING
I (xxxx) voice: state PROCESSING -> SPEAKING
I (xxxx) voice: state SPEAKING -> READY
```

## Next

- [Switch voice modes](../how-to/switch-voice-modes.md) — trade privacy, speed,
  and cost (local vs cloud vs onboard).
- [Dictate and take notes](../how-to/dictate-and-take-notes.md) — the full
  dictation + Notes workflow.
- [Enable the TinkerON wakeword](../how-to/enable-tinkeron-wakeword.md) — go
  fully hands-free with "Hey Tinker."
- [The voice pipeline](../explanation/the-voice-pipeline.md) — what actually
  happens between your words and the answer.
