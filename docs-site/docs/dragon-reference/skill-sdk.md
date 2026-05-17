---
title: Skill SDK
sidebar_label: Skill SDK
---

# Skill SDK

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Skills are Python files. Each is a `Tool` subclass with a name, JSON-schema args, capability tags, and an async `execute()`. Drop one into `dragon_voice/tools/`, register it, restart the voice service.

Source of truth: [TinkerBox `docs/SKILL_AUTHORING.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/SKILL_AUTHORING.md).

## Anatomy

```python
# dragon_voice/tools/coffee_meter.py
from .base import Tool

class CoffeeMeterTool(Tool):
    name = "coffee_meter"
    description = "Track today's coffee consumption."

    args_schema = {
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["log", "summary"],
                "description": "log = record one cup, summary = today's total"
            },
            "size_oz": {
                "type": "number",
                "description": "ounces of coffee, default 8"
            }
        },
        "required": ["action"]
    }

    async def execute(self, args, ctx):
        action = args["action"]
        if action == "log":
            size = args.get("size_oz", 8)
            await ctx.memory.add_fact(
                f"coffee logged: {size} oz at {ctx.now}",
                tags=["coffee", "today"]
            )
            return {
                "logged": True,
                "size_oz": size,
                "ack": f"Logged {size} oz of coffee."
            }
        elif action == "summary":
            facts = await ctx.memory.recall("coffee", tags=["today"])
            total = sum(_parse_oz(f) for f in facts)
            return {
                "today_oz": total,
                "today_cups": total / 8,
                "ack": f"You've had {total} oz today, {total/8:.1f} cups."
            }
```

Then register:

```python
# dragon_voice/tools/__init__.py
from .coffee_meter import CoffeeMeterTool

def register_all(registry):
    ...
    registry.register(CoffeeMeterTool())
```

`sudo systemctl restart tinkerclaw-voice` and the new skill is live.

## The `ctx` argument

`ctx` is the execution context. Available attrs:

| Attr | Type | Notes |
|------|------|-------|
| `ctx.session_id` | str | Active session |
| `ctx.device_id` | str | Calling device |
| `ctx.now` | datetime | Server clock |
| `ctx.memory` | MemoryService | `add_fact / recall / search` |
| `ctx.scheduler` | SchedulerManager | `add_notification / cancel / reschedule` |
| `ctx.surface` | Surface | `emit_live / emit_card / emit_dismiss` |
| `ctx.tools` | ToolRegistry | Other tools (use sparingly — mostly avoid) |

## Emitting widgets

A skill can emit typed widget state alongside its return value. Tab5 renders widgets opinionatedly from v5 theme tokens — skills can't override colors or layout.

```python
async def execute(self, args, ctx):
    # ... do work ...
    await ctx.surface.emit_live(
        widget_id="my-widget-001",
        title="Coffee meter",
        body="3 cups today",
        tone="active",     # calm | active | urgent | done
        icon="coffee",     # one of the 16 built-in icons
        priority=10
    )
    return {...}
```

There's only one live widget at a time on Tab5 — emitting a higher-priority one displaces the previous. Use `priority` thoughtfully.

For one-shot non-live widgets (cards, lists, charts, prompts), use `emit_card / emit_list / emit_chart / emit_prompt`.

## Tool result format

The `execute()` return value should be JSON-serializable. Add an `"ack"` field with a short human-readable acknowledgement — Dragon's `response_wrap` will use it as the user-visible reply if the LLM emits a tool call without any natural-language wrapper.

## Capability tags

Some skills only make sense in some voice modes (e.g. agentic-only tools). Declare:

```python
class MySkill(Tool):
    capabilities = ["text"]               # default — works in all modes
    # capabilities = ["text", "vision"]    # requires vision-capable LLM
```

The router uses these to gate tool exposure in the LLM's tool list.

## Testing a skill

```bash
# Direct execution via REST
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"args":{"action":"log","size_oz":8}}' \
     http://localhost:3502/api/v1/tools/coffee_meter/execute

# Verify it shows up in the registry
curl -H "Authorization: Bearer $TOKEN" \
     http://localhost:3502/api/v1/tools | jq '.[] | select(.name=="coffee_meter")'

# End-to-end via the dashboard's Tools tab
open http://localhost:3500/   # → Tools tab → coffee_meter
```

## Common pitfalls

- **Don't mutate `args`** — it's the LLM's parsed JSON; treat it as read-only
- **Don't block** — `execute()` must be `async`. CPU-heavy work goes through `await asyncio.to_thread(...)`
- **Don't catch and silently fail** — let exceptions propagate; the registry wraps them as a `tool_error` event
- **Don't import the LLM directly** — if you need to call an LLM, use `ctx.llm.complete()` so the active backend is used (and the per-mode timeout applies)

## Reference examples

- [`dragon_voice/tools/timesense_tool.py`](https://github.com/lorcan35/TinkerBox/blob/main/dragon_voice/tools/timesense_tool.py) — Pomodoro + widget_live emitter
- [`dragon_voice/tools/quick_poll_tool.py`](https://github.com/lorcan35/TinkerBox/blob/main/dragon_voice/tools/quick_poll_tool.py) — declarative widget-skill
- [`dragon_voice/tools/web_search.py`](https://github.com/lorcan35/TinkerBox/blob/main/dragon_voice/tools/web_search.py) — async HTTP + fallback
