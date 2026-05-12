# PLAN — Agent-mode notification surface on Tab5

**Wave 7-0 — design pass.**  Cross-stack audit reference: [`docs/AUDIT-state-of-stack-2026-05-11.md`](AUDIT-state-of-stack-2026-05-11.md).

This is a brainstorm + spec.  **Not code.**  Implementing it is W7-E.
Channel-routing wire-up is W7-F; both gate on this doc landing.

The goal: when Tab5 is in mode 3 (TinkerClaw gateway) and a channel
the gateway is watching (Telegram, WhatsApp, Discord, Slack, Signal,
iMessage, Matrix, etc.) receives a message, the Tab5 should surface
that message in a way that feels native to the Sovereign Halo home
design, lets the user reply with mic or text, and respects ambient
context (quiet hours, mode mismatch, etc.).

---

## 1. Goal & scope

**In scope (this doc, implementable in W7-E):**

- Visual surface on the home screen for incoming channel messages
- Toast for low-priority / informational messages
- Persistent "now-card" extension for thread-shaped messages
- Sound cue (extend [`main/ui_audio_cues.{c,h}`](../main/ui_audio_cues.h))
- Dismiss + snooze + reply gestures
- Per-channel allow/deny in Settings (`channels_*` NVS keys)
- Wire-shape spec for the Dragon→Tab5 `channel_message` WS frame

**Out of scope (separate slices):**

- W7-F channel push **routing** (Dragon-side glue between gateway and
  WS emit; assumes W7-0 surface exists)
- W7-G browser-automation visibility
- Multi-channel UI (one inbox for all platforms) — v2
- Per-conversation history view — v2 (the agent's own log is enough
  for v1)
- Read receipts back to channel platforms — depends on gateway support

---

## 2. Surface placement

The Sovereign Halo home (v4·C Ambient Canvas in
[`main/ui_home.c`](../main/ui_home.c)) carries four surfaces that
can plausibly host an incoming-message notification:

| Surface | File anchor | Current role |
|---|---|---|
| Status strip (top-of-screen) | `ui_home.c:621` — "emile" label area | Greeting + clock |
| Now-card | `ui_home.c:147` `s_now_card` | Active widget / live slot |
| Toast (transient slide-in) | `ui_home.c:1920` `s_toast` | Mode-switch confirmations, errors |
| Error banner (persistent dismissable) | `ui_home_show_error_banner` | K144 down / Dragon unreachable |

**Pick:** toast + now-card.  Don't touch the status strip (clock-shaped
real estate, ambient by design) or error banner (semantically "system
unhappy", wrong frame for "message arrived").

### 2.1 Toast — low-priority / informational

Already exists.  Reuse via `ui_home_show_toast_ex(text, tone)` where
tone is one of `UI_TOAST_TONE_NEUTRAL / SUCCESS / WARNING / ERROR`.

For channel messages, introduce a new tone:
`UI_TOAST_TONE_INCOMING` — distinct visual (channel glyph + sender +
preview) but same 3-4 s slide-in behavior.

Use case: a Telegram message from someone the user hasn't whitelisted
as "high priority", OR a status-shaped message ("Mom is calling you
back in 5 minutes"), OR a notification the agent has already handled
("Replied to Jamie's question about lunch — said yes").

### 2.2 Now-card claim — thread-shaped messages

The "now-card" in the Sovereign Halo design is a slot at
roughly y=540-740 that displays a single live focus — usually
the active widget.  For mode-3 channel notifications, the now-card
gets a new claim state: `UI_NOW_CHANNEL_MESSAGE`.

Visual:

```
┌───────────────────────────────────────────────┐
│  [tg]  Jamie Park · 12:34                      │
│                                                 │
│  Are you still on for lunch Thursday? My       │
│  3 PM moved.                                   │
│                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐    │
│  │ Reply 🎤 │  │ Snooze   │  │ Dismiss  │    │
│  └──────────┘  └──────────┘  └──────────┘    │
└───────────────────────────────────────────────┘
```

- Channel-glyph in top-left (12px monochrome — `tg` for Telegram,
  `wa` for WhatsApp, `dc` for Discord, etc.; bundle into
  [`main/widget_icons.{c,h}`](../main/widget_icons.h))
- Sender name + age in the kicker line (right-justified age, like
  the existing recent-activity card)
- 2-3 line preview, no markdown render (those are W7-D w/ chat full
  view)
- Three action buttons.  Reply opens mic w/ context, Snooze defers
  for 15 min, Dismiss clears the now-card.

The now-card claim is exclusive — if a widget is currently in the
slot, the channel message **bumps the widget** and the widget moves
to a secondary queue. The widget returns when the message is
dismissed / snoozed / replied.

---

## 3. Notification lifecycle

```
Dragon → channel_message WS frame
       ↓
Tab5 receives in voice.c → routes to ui_notification.c
       ↓
   ┌──────────┬───────────────────┐
   ▼          ▼                   ▼
 Toast    Now-card claim    Persistent badge (unread count)
   ↓          ↓
 3-4 s     User reacts → reply / snooze / dismiss
 fade        ↓
            Reply: voice overlay opens with channel context;
                   user dictates reply; ack sent through gateway
            Snooze: card hides for 15 min, re-fires
            Dismiss: card clears immediately
```

### 3.1 Routing rule: toast vs now-card

| Signal | Goes to |
|---|---|
| `priority: "low"` OR no sender match in starred-contacts | Toast |
| `priority: "high"` OR sender starred OR @mention | Now-card |
| Status-shaped (no reply needed) | Toast |
| Question-shaped (needs reply) | Now-card |

Priority + sender-match come from the Dragon-side `channel_message`
frame.  Dragon (or the gateway) decides.

### 3.2 Snooze

Snooze stores `(channel, message_id, fire_at)` in a small PSRAM
ring.  On every 60 s tick, walk the ring + re-fire any `fire_at <=
now`.  Max 8 snoozed messages.  Overflow = drop oldest.

### 3.3 Reply flow

1. User taps "Reply 🎤" on the now-card
2. Voice overlay opens with a header chip: "Replying to Jamie (tg)"
3. User dictates; mic-orb behaves as usual
4. On orb release, Tab5 sends `{"type":"channel_reply",
   "channel":"telegram", "thread_id":"<id>", "text":"<transcript>"}`
   to Dragon
5. Dragon forwards to gateway via `chat.reply` RPC
6. On success, toast confirms "Replied to Jamie via Telegram"
7. On failure, error banner with retry

The chip "Replying to X" is critical — without it the user might
think they're starting a fresh conversation with the agent, not
replying to a specific channel thread.

---

## 4. Tone (visual + audio)

### 4.1 Visual

- Channel glyphs in `widget_icons.c` (8 glyphs: tg / wa / dc / sl / sg / im / ma / em)
- Now-card border colour = `TH_MODE_CLAW` (rose, same as agent-skills)
- Toast colour for `INCOMING` tone = blue-violet `0x6B5BFF` (distinct
  from existing SUCCESS=emerald / ERROR=rose / WARNING=amber)
- Sender name in `FONT_HEADING`; preview in `FONT_BODY`; age in
  `FONT_TINY TH_TEXT_SECONDARY`

### 4.2 Audio cue

Extend [`main/ui_audio_cues.h`](../main/ui_audio_cues.h) with:

```c
typedef enum {
    UI_CUE_MODE_SWITCH = 0,  /* existing */
    UI_CUE_CANCEL,           /* existing */
    UI_CUE_ERROR,            /* existing */
    UI_CUE_INCOMING_LOW,     /* new — 30 ms ping, single tone */
    UI_CUE_INCOMING_HIGH,    /* new — 80 ms two-tone bell */
    UI_CUE_COUNT,
} ui_cue_t;
```

`INCOMING_LOW` (for toasts) is short + soft — a 30 ms 1200 Hz ping at
30 % amplitude.  Pleasant, not startling.

`INCOMING_HIGH` (for now-card claims) is a two-tone bell — 40 ms
880 Hz followed by 40 ms 1320 Hz at 35 % amplitude.  Recognizable as
"this is a message", distinct from existing cues.

### 4.3 Quiet hours

`tab5_settings_get_quiet_on/_quiet_start/_quiet_end` already exist
(NVS keys `quiet_on`, `quiet_start`, `quiet_end`).  When quiet hours
are active:

- Suppress audio cues for both LOW and HIGH
- Toasts still appear visually but with halved opacity for 1 s before
  fading
- Now-card claims still fire (the user wanted high-priority through)
- Snoozed-message re-fires respect quiet hours: defer until quiet ends

---

## 5. Per-channel settings

New NVS keys (max 15 chars per the existing settings convention):

| Key | Type | Default | Range | Description |
|---|---|---|---|---|
| `ch_tg_on` | u8 | 0 | 0-1 | Telegram notifications enabled |
| `ch_wa_on` | u8 | 0 | 0-1 | WhatsApp notifications enabled |
| `ch_dc_on` | u8 | 0 | 0-1 | Discord notifications enabled |
| `ch_sl_on` | u8 | 0 | 0-1 | Slack notifications enabled |
| `ch_sg_on` | u8 | 0 | 0-1 | Signal notifications enabled |
| `ch_im_on` | u8 | 0 | 0-1 | iMessage notifications enabled |
| `ch_ma_on` | u8 | 0 | 0-1 | Matrix notifications enabled |
| `ch_em_on` | u8 | 0 | 0-1 | Email notifications enabled |

All default off — user opts in per-channel in Settings.  Settings UI
gets a "Channels" section under "Voice" with a toggle per channel.

Tab5 doesn't know about channels the user hasn't enabled.  Dragon
respects the toggle on its side via `config_update`.

---

## 6. Wire shape — Dragon → Tab5

```json
{
  "type": "channel_message",
  "channel": "telegram",
  "message_id": "tg:8675309:42",
  "thread_id": "tg:8675309",
  "sender": {
    "id": "jamie_park",
    "display_name": "Jamie Park",
    "starred": true
  },
  "text": "Are you still on for lunch Thursday? My 3 PM moved.",
  "preview": "Are you still on for lunch Thursday? My 3 PM moved.",
  "ts": 1715568000,
  "priority": "high",
  "needs_reply": true,
  "metadata": {
    "platform_thread_url": "https://t.me/c/...",
    "media": []
  }
}
```

- `channel` — short canonical name (matches NVS suffix: `tg`, `wa`, `dc`, etc.)
- `message_id` — globally unique; Tab5 dedupes against last 32 in PSRAM ring
- `thread_id` — used for reply routing
- `sender.starred` — pre-computed by Dragon based on contact list
- `priority` — `low` / `normal` / `high`; Tab5's router maps to surface
- `preview` — short version for toast; may equal `text` for short messages
- `metadata` — free-form, ignored if Tab5 doesn't recognize

Tab5 → Dragon reply:

```json
{
  "type": "channel_reply",
  "channel": "telegram",
  "thread_id": "tg:8675309",
  "text": "yes lunch thurs works, 12:30 ok?",
  "in_reply_to": "tg:8675309:42"
}
```

Dragon ACKs with:

```json
{
  "type": "channel_reply_ack",
  "thread_id": "tg:8675309",
  "ok": true,
  "platform_message_id": "tg:8675309:43"
}
```

---

## 7. File-level impl pointers (for W7-E)

New files:

- `main/ui_notification.{c,h}` — public API `ui_notification_show(msg)`;
  router that picks toast vs now-card; snooze ring; dedupe ring
- `main/voice_ws_proto.c` — add `channel_message` handler that calls
  `ui_notification_show()` on the LVGL thread via `tab5_lv_async_call`
- `main/voice.c` — `voice_send_channel_reply(channel, thread, text)`
  helper called from the voice overlay when in reply-mode

Modified files:

- `main/ui_home.c` — now-card claim hook (`s_now_card`-shaped) for
  `UI_NOW_CHANNEL_MESSAGE` state; reply-context chip in voice overlay
- `main/ui_audio_cues.{c,h}` — add `UI_CUE_INCOMING_LOW` + `_HIGH`
- `main/widget_icons.{c,h}` — 8 channel glyphs (path-step DSL,
  matching existing builtin glyph encoding)
- `main/settings.{c,h}` — add 8 `ch_*_on` NVS keys
- `main/ui_settings.c` — new "Channels" section with per-channel toggle
- [`docs/AUDIT-state-of-stack-2026-05-11.md`](AUDIT-state-of-stack-2026-05-11.md) — flip W7-0 + W7-E to closed
- [`docs/CHANGELOG.md`](CHANGELOG.md) — wave entry

---

## 8. Failure modes + recovery

| Failure | Behavior |
|---|---|
| Same `message_id` arrives twice | Second is silently dropped (32-entry dedupe ring in PSRAM) |
| Now-card slot is occupied by a widget | Widget moves to backstack; channel message takes precedence; widget returns on dismiss/reply |
| Now-card slot is already a channel message | New one stacks; older becomes backstack; "1 more" badge on the live one |
| User reply fails (gateway unreachable) | Toast: "Reply not sent — saved as draft"; reply persisted to SD via existing `voice_messages_sync` offline-queue mechanism |
| Quiet hours suppress audio but message is `priority: "high"` | Visual surface still fires; audio respects quiet (don't override user explicit prefs) |
| User taps Reply but mic permission gone (unlikely) | Toast: "Mic unavailable" + fall back to keyboard reply |
| Snooze ring full (>8) | Oldest snoozed re-fires immediately; pre-empts new snooze |

---

## 9. Risks + open questions

- **Starred-contact list lives on Dragon.**  Tab5 doesn't know who's
  "starred" — it trusts `sender.starred: true` in the frame.  This is
  fine for v1; future settings could expose a Tab5-side override
  ("treat all Jamie messages as low-priority right now").

- **Reply context preservation.**  When the user dictates a reply
  but takes 30 s thinking about it, the now-card may be gone by the
  time they finish.  Solution: the voice overlay's reply-context
  chip is the contract — once shown, it's locked until dismiss
  regardless of underlying now-card state.

- **Multi-message thread.**  If 3 Telegram messages from Jamie arrive
  in quick succession, v1 treats them as 3 separate now-card claims
  (oldest gets stacked, newest visible).  v2 could coalesce by
  `thread_id` into a "Jamie (3)" superpose.  Defer.

- **Mode mismatch.**  What if a channel message arrives while
  vmode=0 (Local) or vmode=5 (Solo)?  v1: still surface the
  notification (user wants to know), but the reply flow refuses with
  "Switch to TinkerClaw mode to reply" toast.  v2: cross-mode reply
  via Dragon's local LLM is conceivable but a different product
  decision.

- **Snooze persistence across reboot.**  v1: snoozed messages are
  in PSRAM only — lost on reboot.  Sufficient for the 15-min snooze
  window in most realistic cases (reboots are rare).  v2: persist to
  SD if usage shows a need.

---

## 10. Implementation slice ordering (for W7-E)

1. **W7-E.0** — `ui_audio_cues` adds LOW + HIGH cues, pre-allocated at
   boot.  Tested via debug-server cue-fire endpoint.
2. **W7-E.1** — `ui_notification.{c,h}` toast path only.  Dragon
   debug-injects a synthetic `channel_message` via existing
   `/debug/inject_ws` endpoint.  Verify toast visual + audio + tone.
3. **W7-E.2** — now-card claim path.  Synthetic high-priority
   message; visual verification of the card render + 3-button row.
4. **W7-E.3** — snooze + dismiss + dedupe rings.  Tests via
   repeated debug-injects.
5. **W7-E.4** — reply path.  Mic-orb dictation in reply mode; round-
   trip `channel_reply` → mocked Dragon ack.
6. **W7-E.5** — Settings UI Channels section + NVS keys + Dragon
   `config_update` of the toggle state.
7. **W7-E.6** — quiet-hours integration + final polish.

Each slice ~1-2 hours.  Total W7-E budget: ~6-12 hours across 6 PRs.

---

## 11. Anchors

- Sovereign Halo design rationale: [home critique memory](../.) — see
  `feedback_home_design_critique.md`
- Existing toast + banner mechanics: [`ui_home.c:1920-2100`](../main/ui_home.c)
- Existing audio cue infra (W8 audit Wave 8): [`ui_audio_cues.{c,h}`](../main/ui_audio_cues.h)
- Channel routing on Dragon (W7-F target): TinkerBox's voice handler dispatch
- Cross-stack audit anchor: [`AUDIT-state-of-stack-2026-05-11.md`](AUDIT-state-of-stack-2026-05-11.md)
