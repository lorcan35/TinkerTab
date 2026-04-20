# Widget Platform — Master Spec (v1)

**Status:** Design lock, pre-implementation.
**Audience:** Firmware engineers (Tab5), skill authors (OpenClaw / TinkerClaw),
renderer authors (future hardware partners), and brain maintainers (TinkerBox).
**Companion docs:**
- `.superpowers/brainstorm/widget-platform/` — stunning HTML mockups + design principles
- `TinkerBox/docs/protocol.md` §17 — WebSocket message schemas
- `OpenClaw/docs/tools/skills.md` — skill author guide
- `docs/PLAN.md` — implementation plan with exact file paths + commits

---

## 1. Why this exists

TinkerTab today is **a fixed-feature device**. Every new surface — a timer, a
grocery list, a parking-meter tracker — requires editing C files, recompiling,
and flashing. Meanwhile, TinkerClaw/OpenClaw has **196 skills** on the brain
side, and none of them appear on the Tab5 screen without code changes.

The Widget Platform fixes that asymmetry with a single structural move:

> **Skills emit typed state. Devices render state through six typed widgets.
> The unit of product velocity moves from firmware to Python.**

This spec is the contract between *the brain* (Dragon) and *any device* (Tab5
now, other hardware later).

### Design goals

1. **Skill authors write state, not layout.** No CSS, no coordinates, no LVGL.
2. **Hardware-agnostic.** Skills written once render on any device implementing
   the renderer — Tab5, Waveshare round, ESP32-S3 OLED, anything else.
3. **Opinionated by vocabulary.** Six widgets, fixed slots. Novice authors
   cannot produce ugly layouts because there's no layout surface.
4. **Incremental.** Existing `media`, `card`, `audio_clip` rich-media pipeline
   is the seed. Widget messages extend, they don't replace.
5. **Stable under load.** Widget churn uses hide/show patterns, bounded memory,
   and drop-not-crash semantics. Proven against the stress-test scenarios that
   broke the v5 UI work (see `LEARNINGS.md`).

### Non-goals (explicit)

| Not going to do | Why |
|---|---|
| YAML-declared layouts (app-manifest L3) | Layout contract **is** the vocabulary. YAML layouts re-introduce the Garmin-watchface problem — amateur UIs ship broken. |
| Runtime interpreter on Tab5 (JS, Lua) | Skills run on the brain. Tab5 only renders JSON. Security model stays trivial. |
| App install / marketplace flow | Skills sync from Dragon's registry (`git pull`). No Tab5 restart, no per-device distribution. |
| Per-skill chrome overrides (colors/fonts) | v5 tokens are centrally enforced. Taste is part of the platform. |
| Multi-user / per-widget user_id (v1) | Single-user v1. Multi-user is a dedicated later pass (session model already ready). |
| Tab5-authored native apps | Native screens (camera viewfinder, keyboard, voice overlay) stay in C. Not all features are skills. |

---

## 2. Architecture

Three layers with strict contracts between them.

```
┌────────────────────────────────────────────────────────────────┐
│  LAYER 3 — Skill (Python, on the brain)                        │
│    • OpenClaw skill with YAML manifest + skill.py              │
│    • Emits state via Tab5Surface facade                        │
│    • Handles voice triggers, business logic, external APIs     │
│  Example: dragon_voice/tools/timesense_tool.py (80 LOC)        │
└─────────────────────────┬──────────────────────────────────────┘
                          │  state updates
                          ▼
┌────────────────────────────────────────────────────────────────┐
│  LAYER 2 — Protocol (WebSocket JSON envelopes)                 │
│    • Six typed widget messages + action messages               │
│    • Capability exchange at registration                       │
│    • Extends TinkerBox protocol.md §17                         │
│  Transport: existing voice WS (ws://host:3502/ws/voice)         │
└─────────────────────────┬──────────────────────────────────────┘
                          │  JSON frames
                          ▼
┌────────────────────────────────────────────────────────────────┐
│  LAYER 1 — Device renderer (LVGL on Tab5, other on future HW)  │
│    • widget_store: bounded in-memory cache of active widgets   │
│    • ui_widget: per-widget-type native rendering in v5 style   │
│    • Home live-slot integration, stack sheet, action bus       │
│  Entry: main/ui_widget.c + main/widget_store.c                 │
└────────────────────────────────────────────────────────────────┘
```

### Separation of concerns

| Concern | Owner |
|---|---|
| Business logic (timer math, API calls, LLM) | Skill (Layer 3) |
| Voice trigger matching | Brain router (between 3 and 2) |
| JSON envelope + transport | Protocol (Layer 2) |
| Visual rendering | Device renderer (Layer 1) |
| Priority arbitration (which widget owns live slot) | Brain + device cooperative (below) |
| Capability degradation | Brain (based on device-advertised caps) |

### Capability exchange

At WebSocket registration, the device sends a capability frame:

```json
{
  "type": "register",
  "device_id": "tab5-abc",
  "session_id": "...",
  "capabilities": {
    "screen": {"w": 720, "h": 1280, "fmt": "rgb565", "touch": true},
    "input": {"mic": true, "keyboard": true, "imu": true, "camera": true},
    "render_mode": "client",
    "widgets": ["live", "card", "list", "chart", "media", "prompt"],
    "icons": ["clock","briefcase","laundry","coffee","book","car","pot",
              "person","droplet","check","alert","sun","moon","cloud",
              "calendar","star"]
  }
}
```

- `render_mode: "client"` — device renders from JSON slots (Tab5 default).
- `render_mode: "server"` — device can only display JPEGs; brain pre-renders
  every widget to a bitmap. Fallback for OLEDs or anything without an LVGL
  equivalent.
- `widgets` — subset of the 6 types the device implements. Brain downgrades
  requests accordingly (e.g., `chart` → `card` with text body if device doesn't
  list `chart`).

---

## 3. Widget vocabulary

Six widgets. No more.

### 3.1 `live`

**Purpose:** Exactly one thing happening right now. Owns the home hero slot.

**Slots:**

| slot | type | required | notes |
|---|---|---|---|
| `skill_id` | string | ✓ | e.g. `timesense.pomodoro` |
| `card_id` | string | ✓ | stable id across updates |
| `title` | string | ✓ | max 32 chars, renders as serif display |
| `body` | string | ✓ | max 80 chars, wraps 2 lines, narrative tone |
| `icon` | string | — | one of the 16 built-in ids (see §6) |
| `tone` | enum | ✓ | `calm\|active\|approaching\|urgent\|done` |
| `progress` | float | — | 0.0–1.0, drives orb size + ring |
| `action` | object | — | `{label:string(≤8), event:string}` |
| `priority` | int | — | 0–100; brain sorts by priority × age |
| `expires_ms` | int | — | TTL from push; auto-dismiss after |

**Tone → rendering on Tab5:**

| tone | orb tint | breathing rate | accent |
|---|---|---|---|
| `calm` | emerald | 4 s (slow inhale/exhale) | emerald |
| `active` | amber | 3 s | amber |
| `approaching` | amber-hot | 2 s (pulse) | amber |
| `urgent` | rose | 1 s (rapid) | red |
| `done` | emerald (settled, small) | — (static) | emerald |

**Rendering zones (Tab5 720×1280):**

- Orb stage (center) — color + breathing per tone
- Sys label (top-left) — mode indicator, swapped to skill's status when live
- Icon + label (top-right) — widget's `icon` + derived short label
- Poem slot (bottom third) — widget's `title` (serif display) + `body`
- Action row (below poem) — optional action button, amber caption style
- Counter pill (bottom-left, when queue > 0) — other-timer count

**When multiple live widgets exist:**

Brain sorts by `priority × (1 + log(age_ms))` and promotes the winner. Others
stay in the queue; Tab5 shows a counter pill with their count. Tab5 never
receives the losers — protocol is *one live widget at a time*, broadcast.

---

### 3.2 `card`

**Purpose:** A moment-in-conversation notification, completion, or one-tap
action. Lands in chat's message stream or a future Activity overlay.

**Slots:**

| slot | type | required | notes |
|---|---|---|---|
| `skill_id` | string | ✓ | |
| `card_id` | string | ✓ | |
| `title` | string | ✓ | max 32 chars |
| `body` | string | ✓ | max 200 chars, wraps |
| `image_url` | string | — | optional hero image (JPEG from Dragon media cache) |
| `tone` | enum | ✓ | `info\|success\|alert` |
| `action` | object | — | `{label:string(≤8), event:string}` |

**Rendering on Tab5:**
- Reuses existing `chat_msg_view.c` card renderer (extended with an action slot).
- Amber border, letter-spaced caption tone, optional image thumb left.
- Tap action → `widget_action` event.

---

### 3.3 `list`

**Purpose:** Pick one of N.

**Slots:**

| slot | type | required |
|---|---|---|
| `skill_id` | string | ✓ |
| `card_id` | string | ✓ |
| `title` | string | ✓ |
| `items` | array | ✓ |
| `on_select` | string | ✓ |

Each `items[]` element: `{id: string, title: string, subtitle?: string}`.

**Rendering:** Sessions-overlay style (see `ui_sessions.c`) — amber time col
becomes item index, title + subtitle stacked, tap to select.

---

### 3.4 `chart`

**Purpose:** A data shape at a glance.

**Slots:**

| slot | type | required | notes |
|---|---|---|---|
| `title` | string | ✓ | |
| `kind` | enum | ✓ | `spark\|bar\|gauge` |
| `series` | array | ✓ | normalized 0–1 floats (brain does the math) |
| `unit` | string | — | displayed in caption |
| `range` | object | — | `{min:float, max:float}` for readable ticks |

**Rendering:** LVGL `lv_line` (spark), `lv_bar` grid (bar), `lv_arc` (gauge).
On a low-capability device listing `card` only, brain emits a `card` with
body `"avg 42, trending up"` instead.

---

### 3.5 `media`

**Purpose:** Image + caption. Reuses the existing rich-media pipeline
(Pygments code blocks, Pillow tables → JPEG → Tab5 TJPGD).

**Slots:**

| slot | type | required |
|---|---|---|
| `url` | string | ✓ |
| `alt` | string | ✓ |
| `caption` | string | — |
| `width` | int | — |
| `height` | int | — |
| `action` | object | — |

Direct pass-through to `chat_msg_view.c`'s existing `media` type. No new
renderer needed.

---

### 3.6 `prompt`

**Purpose:** Ask one question, get a typed answer.

**Slots:**

| slot | type | required | notes |
|---|---|---|---|
| `question` | string | ✓ | |
| `input.kind` | enum | ✓ | `text\|number\|choice\|confirm` |
| `input.placeholder` | string | — | |
| `input.choices` | array | — | required when `kind=choice` |
| `on_answer` | string | ✓ | |

**Rendering:** keyboard overlay opens automatically when kind is `text/number`,
voice alone when kind is `confirm`, list of choices when kind is `choice`.
Answer → `widget_action` event with `{value: ...}` payload.

---

## 4. Protocol reference

All messages extend the existing TinkerTab↔TinkerBox voice WebSocket. Full
schemas are in `TinkerBox/docs/protocol.md` §17. Quick reference:

### Dragon → Tab5

| type | purpose |
|---|---|
| `widget_live` | Create or replace the live widget |
| `widget_live_update` | Partial update; only changed slots required |
| `widget_live_dismiss` | Remove live widget (returns to idle) |
| `widget_card` | Push a card into the activity stream / chat |
| `widget_list` | Push a list (modal pick) |
| `widget_chart` | Push a chart into the activity stream |
| `widget_media` | Existing `media` extended with `skill_id` |
| `widget_prompt` | Show a prompt (modal) |
| `widget_dismiss` | Dismiss any non-live widget by `card_id` |

### Tab5 → Dragon

| type | purpose |
|---|---|
| `widget_action` | User tapped an action or answered a prompt |
| `widget_capability` | Device advertises supported widgets / icons |

### Update semantics

- **Create vs update:** create carries all required slots. Update omits slots
  that haven't changed. Brain tracks last-sent state; Tab5 merges into its
  local widget_store entry.
- **Idempotency:** same `card_id` + same slots → no-op on Tab5 (debounced).
- **Ordering:** in-order per skill; cross-skill ordering is by arrival time.
- **Dropouts:** Tab5 drops widgets older than `expires_ms`. Brain does not
  need acknowledgement.

---

## 5. Author experience

A skill is a Python file + a YAML manifest.

### Manifest (`skills/timesense/manifest.yml`)

```yaml
skill_id: timesense.pomodoro
name: Time Sense
description: AI-first timer that feels time with you
voice_triggers:
  - "set a timer for {minutes:int} minutes"
  - "pomodoro"
  - "deep work for {minutes:int}"
icon_default: briefcase
surfaces:
  - widget: live
    priority: 80
    when: running
  - widget: card
    when: done
    tone: success
capabilities_required: []          # portable to any device
```

### Skill (`skills/timesense/skill.py`)

```python
from openclaw import Skill, Surface

class Timesense(Skill):
    def start(self, minutes: int):
        self.total = minutes * 60
        self.remaining = self.total
        self.surface.live(
            title="Deep work",
            body=f"{minutes}:00 remaining",
            icon="briefcase",
            tone="calm",
            progress=0.0,
            action=("PAUSE", "ts.pause"),
            priority=80,
        )
        self.schedule_tick(1.0)

    def on_tick(self):
        self.remaining -= 1
        pct = 1 - (self.remaining / self.total)
        tone = ("urgent" if pct > .95 else
                "approaching" if pct > .90 else
                "active")
        self.surface.live_update(
            body=format_time(self.remaining),
            tone=tone,
            progress=pct,
        )
        if self.remaining <= 0:
            self.surface.live_clear()
            self.surface.chat_card(
                title="Done",
                body=f"{self.total//60} min · well run",
                tone="success",
                icon="check",
            )
            self.finish()

    def on_action(self, event, payload):
        if event == "ts.pause":
            self.paused = not self.paused
```

**That's the entire surface area for a skill author.** Tab5 implementation
details leak zero percent into this code.

### Voice trigger syntax

`{name:type}` placeholders in triggers bind to Python kwargs. Supported types:
`int`, `float`, `string`, `duration` (minutes/seconds parsed), `time` (HH:MM).

---

## 6. Icon library

Widgets carry an optional `icon` slot — one of the built-in 16 ids. See
`.superpowers/brainstorm/widget-platform/02-icon-library.html` for visual
renders and authoring system.

**v1 set:** `clock`, `briefcase`, `laundry`, `coffee`, `book`, `car`, `pot`,
`person`, `droplet`, `check`, `alert`, `sun`, `moon`, `cloud`, `calendar`,
`star`.

**Adding new icons:** author as SVG primitives on the brain
(`dragon_voice/icons/composer.py`). Icons ship as part of the skill manifest;
Dragon batches them in the initial capability handshake. Tab5 caches them as
vector path arrays and renders via `lv_canvas_draw_arc`/`lv_canvas_draw_line`.

---

## 7. Rendering contract

Each renderer (Tab5 today, Waveshare / OLED tomorrow) implements every widget
type *opinionatedly* using its native primitives. The contract:

1. **Honor all required slots.** An unknown tone = `active`. An unknown icon =
   widget's skill-default; if that's also unknown, hide the icon slot.
2. **Never show overlapping live widgets.** Brain guarantees one at a time.
3. **Touch targets ≥44 pt.** Every interactive element must meet the mobile
   UI minimum.
4. **Animations ≤300 ms.** State transitions are smooth but never blocking.
5. **Degrade unknown widgets.** If the device cannot render a widget type, it
   sends `widget_unsupported` back and brain re-emits as the nearest supported
   type (per capability table: `chart → card`, `prompt → voice-only`, etc.).
6. **Respect v5 tokens.** All colors, typography, and spacing come from the
   device's theme system. Widget slots never override.

---

## 8. Tab5 renderer (Layer 1)

### File layout

```
main/
  widget_store.c + .h      — in-memory cache of active widgets, priority queue
  ui_widget.c + .h         — per-type native renderers (live / card / ...)
  ui_skills.c + .h         — stack sheet overlay (long-press counter pill)
  ui_home.c                — MODIFIED: poem slot reads from widget_store
  chat_msg_view.c          — MODIFIED: card type gains `action` slot
  voice.c                  — MODIFIED: WS handlers for widget_* messages
```

### Widget store

```c
typedef struct {
    char id[32];                  // card_id
    char skill_id[32];
    widget_type_t type;           // LIVE|CARD|LIST|CHART|MEDIA|PROMPT
    widget_tone_t tone;
    char title[64];
    char body[256];
    char icon_id[16];
    char action_label[16];
    char action_event[48];
    float progress;
    uint32_t priority;
    uint32_t expires_at_ms;
    uint32_t updated_at_ms;
    bool active;
} widget_t;

#define WIDGET_STORE_MAX 32
static widget_t s_widgets[WIDGET_STORE_MAX];
```

~40 lines of data model. All in PSRAM. Evict by age on overflow.

### Live slot integration

`ui_home.c` adds a new state source to `s_poem_label`:

```c
// Priority-ordered fallback:
//   1. active live widget (from widget_store)
//   2. edge state (MUTED / QUIET / DRAGON DOWN)
//   3. last note preview
//   4. standby copy
```

When a live widget is active, the orb's tint + breathing driver swaps from
`voice_mode` to `widget.tone`. Existing `orb_paint_for_mode()` keeps working
for idle.

### Action flow

Tap action target → `widget_store_emit_action(card_id, event)` →
`voice_send_widget_action()` → WS JSON → brain.

---

## 9. Dragon facade (Layer 3)

### `dragon_voice/surfaces/base.py`

```python
class Tab5Surface:
    def __init__(self, ws_sender): ...
    async def live(self, *, title, body, icon=None, tone="active",
                   progress=None, action=None, priority=50,
                   expires_ms=None, card_id=None): ...
    async def live_update(self, card_id, **fields): ...
    async def live_clear(self, card_id=None): ...
    async def card(self, *, title, body, tone="info",
                   image_url=None, action=None, icon=None, card_id=None): ...
    async def list(self, *, title, items, on_select, card_id=None): ...
    async def chart(self, *, title, series, kind="spark",
                    unit=None, card_id=None): ...
    async def prompt(self, *, question, kind, on_answer,
                     choices=None, placeholder=None, card_id=None): ...
    async def dismiss(self, card_id): ...
```

### `dragon_voice/surfaces/manager.py`

Owns the live-widget priority queue *on the brain side* (not just Tab5). Also
owns per-device capability state; downgrades widget type before emission.

### `dragon_voice/tools/widget_action_router.py`

Receives `widget_action` from Tab5, dispatches to the owning skill's
`on_action(event, payload)` handler. Routes errors back as `widget_card` alerts.

---

## 10. Security + privacy model

- **Skills run on the brain only.** Tab5 never executes skill code.
- **Widget state is session-scoped.** Widgets are not persisted across
  Tab5 reboots unless the skill explicitly chooses to restore them.
- **No personal data in the widget envelope by default.** Skills decide what
  to put in `title`/`body`. Authors are responsible for redaction.
- **Camera and mic events are not widgets.** They flow through the existing
  voice pipeline. Widget messages are for *rendering*, not for *capturing*.

---

## 11. Performance + stability

- **Widget store capped at 32 entries.** All in PSRAM. No internal-SRAM
  pressure.
- **Live slot transitions reuse `s_poem_label`**. No new LVGL object alloc per
  update.
- **Action events rate-limited at 4/sec** on the Tab5 side to prevent
  flooding.
- **Hidden widgets don't repaint.** LVGL render cost is zero when no widget is
  visible.
- **WebSocket traffic budgeted.** A 1-second tick carries ~300-byte JSON;
  comfortably inside the SDIO TX envelope with the existing drop-not-panic
  guard (see `LEARNINGS.md` entry on `copy_buff`).

---

## 12. Testing

### Unit
- `tests/test_widget_store.c` — CRUD, eviction, priority ordering.
- `tests/test_widget_action_router.py` — Dragon side; action → skill routing.

### Integration
- End-to-end Pomodoro: voice trigger → live widget → ticks → action → card.
- Multi-timer priority: laundry + parking + pomodoro all running; brain
  promotes parking when its alert threshold fires.
- Capability downgrade: simulate `render_mode=server`; verify brain emits
  pre-rendered bitmaps.

### Regression
- `test/e2e/test_userstory.py` existing suite still passes (no regression in
  existing screens).
- `test/e2e/widget_stress.py` — sustained widget churn, verify LV pool stable,
  no PANIC resets.

---

## 13. Rollout

Phased. Each phase ships end-to-end.

### Phase 1 — `widget_live` only
Smallest possible slice that proves the loop.
- Tab5: widget_store + ui_widget (live only) + home integration + action bus
- Dragon: Tab5Surface.live/live_update/live_clear
- Reference skill: Time Sense (Pomodoro) — voice trigger "pomodoro" works
  end-to-end, orb shows calm → active → urgent → done

### Phase 2 — `widget_card` + action
Cards appear in chat with a single tappable action.
- Tab5: extend `chat_msg_view.c` `card` type with `action` slot
- Dragon: Tab5Surface.card + action handler skeleton
- Skills: existing rich-media `card` emissions auto-upgrade to `widget_card`

### Phase 3 — `widget_list` + `widget_prompt`
Pick-one + one-shot question.
- Tab5: `ui_widget.c` list + prompt renderers
- Dragon: Tab5Surface.list/prompt
- Skills: first three OpenClaw skills port (cooking, songs pick, confirm-send)

### Phase 4 — `widget_chart` + capability downgrade
Visual data + the full capability story.
- Tab5: sparkline/gauge renderers
- Dragon: capability-aware router in surfaces/manager.py
- Skills: spending glance, focus minutes, mood arc

### Phase 5 — Second device renderer
Proves the portability promise.
- Waveshare round display (or similar partner) implements the 6-widget
  renderer in its native form factor.
- Skills shipped on Tab5 work unchanged on the second renderer.

---

## 14. Open questions (tracked in GH issues)

1. **Widget persistence across Tab5 reboots** — should active live widgets
   reappear after a crash-reboot? Default v1 = no; skills can opt in.
2. **Multi-user routing** — which user's widgets does Tab5 show? v1 = latest
   active session. Later = profile switch by voice.
3. **Offline widget fallback** — what does Tab5 show when Dragon is
   unreachable for a long running timer? Freeze + hint ("connection lost") vs
   show last state.
4. **Brain-side priority semantics** — `priority × age` heuristic is v1. May
   need user preference weighting later.
5. **Icon extensibility** — ship-from-skill-manifest vs Tab5-bundled set. v1
   chooses bundled-only for determinism; manifest-shipped icons are phase 4+.

---

## 15. What this spec does NOT yet cover

All deferred to follow-up specs once v1 is real:

- **Widget history / undo** — a user might want to see dismissed widgets.
- **Cross-device handoff** — widget follows user from Tab5 to phone.
- **Rich interaction patterns** — drag, pinch, slider input.
- **Animation customization per skill** — v1 is one animation language.
- **Localization** — v1 is en-only; widget envelope supports `locale` field as
  an optional forward-compatible addition.

---

## 16. Glossary

| term | meaning |
|---|---|
| **Widget** | Typed rendering primitive. Six types, fixed vocabulary. |
| **Skill** | Python module on the brain that emits widgets. |
| **Surface** | A place widgets land on a device (home live slot, chat stream, etc.). |
| **Card ID** | Stable id for a widget instance across updates. |
| **Priority** | Integer 0–100 used by the brain to pick the live winner. |
| **Tone** | Enum mapping to visual + motion treatment (calm/active/approaching/urgent/done). |
| **Capability** | Device-advertised feature set used for widget downgrade. |
| **Live slot** | The one-widget-at-a-time slot on home. Owned by the highest-priority live widget. |

---

*End of spec.*
