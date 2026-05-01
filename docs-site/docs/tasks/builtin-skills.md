---
title: Built-in skills
sidebar_label: Built-in skills
---

# Built-in skills

Skills are Python files on Dragon. Each declares a JSON-schema args contract, a capability tag, and an `execute()` method. The LLM invokes them via XML markers; the result is threaded back into the response.

10 ship by default. Browse and toggle them on the **Skills** screen (nav sheet → Agents).

## The shipped set

| Tool | What it does | Example |
|------|--------------|---------|
| `web_search` | SearXNG metasearch (44 results, DDG fallback) | "what's the news about Mars?" |
| `remember` | Store a fact in memory | "remember my dog's name is Mochi" |
| `recall` | Semantic search over stored facts | "what's my dog's name?" |
| `forget` | Delete a fact (confirm-gated) | "forget what I said about Bob" |
| `datetime` | Current date/time | "what time is it?" |
| `calculator` | Arithmetic via Python eval (sandboxed) | "what's 456 × 789?" |
| `unit_converter` | Length/mass/temp conversion | "convert 50 miles to km" |
| `weather` | Open-meteo lookup | "what's the weather in Paris?" |
| `stock_ticker` | Yahoo Finance | "AAPL price?" |
| `system_info` | Dragon CPU/RAM/uptime | "how's the server doing?" |
| `timesense` | Pomodoro / timer / reminder (emits widget) | "remind me in 30 min" |
| `note_tool` | Create / search notes | "make a note titled 'shopping list'" |
| `quick_poll` | Multiple-choice prompt (emits widget) | "ask me a Python multiple-choice question" |

## How invocation works

The LLM emits XML markers in its response:

```xml
<tool>web_search</tool><args>{"query":"weather in Paris"}</args>
```

Dragon parses the marker, executes the tool, injects the result back into the conversation. The LLM continues the response with the tool result in context. Up to 3 tool calls per turn.

Three accepted marker dialects (parser tolerates all three):

1. **Legacy** — `<tool>NAME</tool><args>{json}</args>`
2. **Standard FC** — `<tool_call>{"name":"...", "arguments":{...}}</tool_call>`
3. **Bracketed-name** — `[NAME]{json}</NAME>` (xLAM quirk)

For local models with limited context, tool definitions are sent in a compact format to keep token usage tight.

## Empty-reply guard

Some FC-trained models (xLAM, distil-functiongemma) emit a tool call and stop — leaving the user-visible reply empty after the parser strips the markup. Dragon's `response_wrap.py` synthesises a one-line natural-language ack from the tool result when this happens. No extra LLM call, no latency.

## Memory + RAG context

Every LLM call in Local / Hybrid / Cloud mode gets enriched with:

1. **Relevant facts** — semantic search via Ollama `nomic-embed-text` over stored facts
2. **Document chunks** — same embedding model, ranked by cosine similarity
3. **Active widgets** — current timer, pending reminders, etc. (so the LLM knows what's already in flight)

This injection happens in `ConversationEngine.build_context()` and is invisible to the LLM-author beyond a "you may receive a `<memory>` block in your system prompt" note.

## Writing your own skill

See [Skill SDK](/docs/dragon-reference/skill-sdk). The TL;DR:

1. Subclass `Tool` in `dragon_voice/tools/`
2. Declare `name`, `description`, `args_schema` (JSON schema)
3. Implement `execute(args, ctx) -> result`
4. Register in `dragon_voice/tools/__init__.py`

Restart `tinkerclaw-voice` and the new tool is live. No firmware changes.

## Skills that emit widgets

Some skills don't just return a result — they emit typed `widget_live` / `widget_card` events that Tab5 renders opinionatedly. `timesense` and `quick_poll` are the reference examples.

[Skill registry → architecture](/docs/architecture/skills)
