---
title: Set a reminder
sidebar_label: Set a reminder
---

# Set a reminder

Reminders are the canonical example of a **skill emitting a live widget**. The Time Sense skill on Dragon listens for time-related natural-language requests, schedules a notification, and emits a typed `widget_live` over WebSocket. Tab5 promotes the highest-priority widget to the home-screen live slot. The orb's breathing colour changes to match the widget's tone.

## Just say it

From the home screen, tap the orb and say:

> "Hey, remind me to take the laundry out in 30 minutes."

What happens:

1. **STT** transcribes the audio
2. **LLM** sees the request, calls the `timesense` tool with `{"action": "remind", "in_minutes": 30, "label": "take the laundry out"}`
3. **Time Sense skill** schedules the notification in Dragon's SQLite-backed scheduler (survives reboots) and emits a `widget_live` to your Tab5 with the countdown
4. **Tab5** receives the widget and replaces the home-screen now-card with `take the laundry out · 30:00 remaining`
5. **LLM** speaks back: *"Got it. I'll remind you in 30 minutes."*

When the timer fires, Dragon emits a notification — you see a toast on Tab5 and (optionally) hear an audible chime.

## Other natural-language phrasings

The Time Sense skill handles:

- **Relative offsets**: "in 30 minutes", "in 2 hours", "in 90 seconds"
- **Absolute times**: "at 3 PM", "at 14:30", "tomorrow at 9 AM"
- **Day names**: "next Monday at noon", "Friday afternoon"
- **Recurring**: "every weekday at 7 AM" (Phase 5 — supported, watch the docs for full coverage)

If the time parser can't make sense of a phrase, the LLM falls back to asking you for clarification.

## Pomodoros and timers

Time Sense also exposes:

- **`start_pomodoro`** — 25 / 5 / 25 / 5 / 25 / 15 cycle with breathing-orb sync
- **`start_timer`** — single countdown
- **`alarm`** — fires once at a specific clock time

The widget is shared — only one live widget shows at a time, and Time Sense will queue subsequent timers behind the active one. You can see all pending timers via Settings → Time Sense.

## Cancel a reminder

Two ways:

1. **Voice**: *"cancel the laundry reminder"* — the LLM resolves which one and calls Time Sense to cancel
2. **Tap the live widget** on the home screen — opens the widget detail with a *Cancel* button

## Where reminders live on Dragon

Notifications are stored in `dragon_voice/scheduler/store.py`'s SQLite table:

```sql
SELECT * FROM scheduled_notifications WHERE status='pending';
```

The scheduler replays pending notifications on boot, so a Dragon reboot doesn't lose your laundry reminder.

[REST API for scheduler →](/docs/dragon-reference/rest-endpoints)
