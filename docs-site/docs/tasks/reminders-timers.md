---
title: Reminders, timers, alarms
sidebar_label: Reminders, timers, alarms
---

# Reminders, timers, alarms

The **Time Sense** skill handles all of these. It's the reference example of a widget-emitting skill: schedules a notification, emits a `widget_live` for the live-slot display, and updates Tab5 every second until the timer fires.

## Three flavors

### Reminder

Fires once at a future time, delivers a message.

> "Remind me to take the laundry out in 30 minutes."

> "Remind me at 7 PM to call my mom."

> "Remind me next Tuesday at 9 AM to renew the parking permit."

### Timer / countdown

Fires once when a duration elapses, plays a chime + shows a toast.

> "Set a 25-minute timer."

> "Start a 10-minute timer for the pasta."

### Alarm

Fires at an absolute clock time, repeats daily / weekly if specified.

> "Wake me up at 7 AM."

> "Alarm every weekday at 6:30 AM."

### Pomodoro

The Time Sense skill has a built-in pomodoro mode that cycles work/break intervals automatically with breathing-orb sync.

> "Start a pomodoro."

Defaults: 25 min work, 5 min break, repeat 4 cycles, then a 15-minute long break.

## What you see

Each Time Sense activation:

1. **Live widget** — top of the home screen, replaces the now-card. Title + countdown + optional progress ring.
2. **Orb breathing colour** — matches the widget tone (calm/done = emerald, active/approaching = amber, urgent = rose)
3. **Spoken ack** — *"Got it. I'll remind you in 30 minutes."*

When the timer fires:

1. Live widget shows *"Done!"* with the urgent rose tone
2. Audible chime through the speaker
3. Optional voice message via TTS (if you said "remind me to ..." with a message)
4. Toast at the top of the screen

## Cancel

Three ways:

1. **Voice**: *"cancel the laundry reminder"* — LLM resolves which one
2. **Tap the live widget** on the home screen → opens detail with a *Cancel* button
3. **REST API**: `DELETE /api/v1/scheduler/notifications/<id>`

## Reschedule

> "Push the laundry reminder back 10 minutes."

LLM calls `<tool>timesense</tool><args>{"action":"reschedule",...}</args>`. The widget updates in place; the underlying scheduler row is rewritten with a new `when` timestamp.

## Snooze

When a notification fires, the toast has a snooze button (default 5 minutes; configurable in Settings → Time Sense). Tap snooze and a new notification is created for `now + 5 min`.

## Persistence across reboots

Notifications are stored in `dragon_voice/scheduler/store.py`'s SQLite table. The scheduler replays pending notifications on boot — so if Dragon reboots in the middle of a 30-minute timer, the timer survives.

## Multiple at once

Time Sense supports many concurrent reminders + timers. The live widget shows the *highest-priority* one (default: the soonest-firing). The rest are queued; you can browse them in Settings → Time Sense → Pending.

## Quiet hours interaction

If a notification fires inside the configured quiet hours, the chime + voice message are suppressed; only the silent toast + widget update happen. The underlying notification is still recorded. NVS keys `quiet_on`, `quiet_start`, `quiet_end`.

## Limits

There's a per-device runaway cap (`RUNAWAY_CAP_PER_DEVICE` = 100). If a misbehaving skill / LLM tries to schedule more than 100 active notifications for one device, new ones are rejected with a clear error event. This caught a real issue once where a tool-loop kept calling `timesense` instead of acknowledging.
