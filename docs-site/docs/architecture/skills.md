---
title: Skill registry
sidebar_label: Skill registry
---

# Skill registry

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Skills are Python files on Dragon. Each is a `Tool` subclass with a name, a JSON-schema args contract, capability tags, and an `execute()` method. The LLM invokes them via XML markers; the result is threaded back into the response.

## Anatomy of a skill

```python
# dragon_voice/tools/my_skill.py
from .base import Tool

class MySkill(Tool):
    name = "my_skill"
    description = "Does the thing the skill is named after."
    args_schema = {
        "type": "object",
        "properties": {
            "input": {"type": "string", "description": "the thing to operate on"}
        },
        "required": ["input"]
    }

    async def execute(self, args, ctx):
        result = do_the_thing(args["input"])
        return {"output": result}
```

Then in `dragon_voice/tools/__init__.py`:

```python
from .my_skill import MySkill

def register_all(registry):
    ...
    registry.register(MySkill())
```

Restart `tinkerclaw-voice` and the new skill is live. No firmware changes.

## Three accepted dialects

The LLM emits XML markers; the parser tolerates three dialects (`dragon_voice/tools/registry.py`):

1. **Legacy** — `<tool>NAME</tool><args>{json}</args>` (TinkerBox system-prompt format; ministral, gemma3 emit this)
2. **Standard FC** — `<tool_call>{"name":"...", "arguments":{...}}</tool_call>` (Qwen-FC, Gemma-FC, distil-* emit this regardless of system prompt)
3. **Bracketed-name** — `[NAME]{json}</NAME>` or `[NAME]` (xLAM quirk)

The parser is gated on `NAME` being in the registered tool set so prose like `[note]` in chat doesn't false-fire.

## ToolRegistry.execute is the chokepoint

Every tool execution flows through `ToolRegistry.execute()`. That's a deliberate single instrumentation site — the agent_log ring buffer (`/api/v1/agent_log`) is populated here, so REST + WS + dashboard callers all surface in the same activity log.

If you find yourself adding instrumentation at a per-tool site, you're probably doing it wrong. Add it at `execute()` instead.

## Empty-reply guard

Some FC-trained models (xLAM, distil-functiongemma, LFM2.5-Nova) emit a tool call and stop — leaving the user-visible reply empty after the parser strips the markup. `dragon_voice/tools/response_wrap.py` synthesises a one-line natural-language ack from the tool result when this happens. No extra LLM call.

Each tool can register a wrap function — `synthesize_wrap` looks them up by tool name and calls the matching wrap to produce the ack text. New tools that frequently fail this way should ship a wrap.

## Memory + RAG context injection

Before every LLM call, `ConversationEngine.build_context()` queries:

1. **Memory facts** — semantic search via Ollama `nomic-embed-text` (768-dim) over the user's stored facts, top-k by cosine similarity
2. **Document chunks** — same embedding model, ranked by similarity to the user's query
3. **Active widgets** — current timer, pending reminders, etc.

Relevant items are injected into the system prompt as `<memory>` and `<documents>` blocks. The LLM sees these as ambient context — no tool call needed.

## Widget-emitting skills

Some skills don't just return a result — they also emit typed widget state via the Surface abstraction. Reference examples: `timesense_tool.py` (live timer countdown) and `quick_poll_tool.py` (multiple-choice prompt).

```python
async def execute(self, args, ctx):
    notification_id = await self.scheduler.add(...)
    await ctx.surface.emit_live(
        title="Coffee timer",
        body="3:00 remaining",
        tone="active",
        priority=10,
        widget_id=f"timer-{notification_id}"
    )
    return {"scheduled": notification_id}
```

Tab5 receives the `widget_live` event over WS and promotes it to the home-screen live slot.

[Skill SDK reference →](/docs/dragon-reference/skill-sdk)

## When NOT to write a skill

- **Hardware features** that talk to Tab5 sensors directly. Those belong in firmware.
- **Stateful per-device features** that require Tab5-side coordination (camera viewfinder, voice mode picker). Those need both halves; the skill might be one piece, but the camera screen itself stays in C.
- **Things the LLM already does well** — translation, summarisation, simple Q&A. Don't wrap an LLM call in a tool just to call it from another LLM call.
