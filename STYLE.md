# STYLE.md — the Tinker-stack documentation writing standard

This file is copied byte-identical into every repo (TinkerTab, TinkerBox,
PingOS, TinkerClaw). It is the contract that makes four separately-maintained
repos read as one product.

## Purpose

We write docs so all four repos read as one voice. A reader who learns how to
read one repo's documentation should be able to navigate any of them without
re-learning conventions. That requires a single, enforced set of rules for how
we structure pages, what tone we use, how we name files, and how we link across
the stack. This document is that set of rules. When in doubt, match what is
already here; when this document is wrong, fix it here first, then fix the docs
that followed the old rule.

We follow the [Diátaxis](https://diataxis.fr) documentation system. The whole
of this standard exists to keep our docs Diátaxis-correct and consistent across
repos.

## Voice & tone

Write directly, in the second person, in the active voice.

- **Direct.** Say the thing. "Flash the firmware with `idf.py flash`." Not "It
  may be possible to flash the firmware using a command such as `idf.py flash`."
- **Second person.** Address the reader as "you." "You should see the orb turn
  blue." Not "the user will observe the orb turning blue."
- **Active.** "Dragon transcribes the audio." Not "the audio is transcribed by
  Dragon."
- **Confident, not defensive.** State how things work. Do not hedge, apologize,
  or pad with disclaimers. This mirrors the project's no-defeatist-language
  norm — we do not write "unfortunately," "sadly," "this is tricky," or
  "hopefully this works." If a step is genuinely risky, say exactly what the
  risk is and how to recover, then move on.
- **No marketing hype.** No "blazing fast," "seamless," "powerful,"
  "revolutionary," or "world-class" inside the docs themselves. Describe what
  the thing does and let the facts carry it. Numbers beat adjectives: "~7 s per
  turn" is better than "fast."

## Diátaxis rule

Every authored page is exactly one of four types. Name the type in the header.
**Never mix types in one page.** A tutorial never stops to explain
architecture; a reference never teaches; an explanation never gives a numbered
recipe.

| Type | Job | The reader is… | Primary audience lens |
|---|---|---|---|
| **Tutorial** | Learning by doing | a beginner being taught | Tinkerer: "unbox → working assistant" |
| **How-to guide** | Achieve a specific task | a user with a goal | All: build, flash, deploy, integrate, recover |
| **Reference** | Look up facts | someone who needs precision | Integrator: REST API, WS protocol, config, tools, code maps |
| **Explanation** | Understand why | someone building a mental model | Developer: architecture, data flow, design decisions |

If a page is trying to do two of these jobs, split it into two pages and link
them. The four `docs/_templates/*.md` files give you a correct skeleton for
each type — start from the template, do not invent your own structure.

## Standard header

Every authored page opens with this YAML frontmatter. It is **mandatory** — no
page ships without it.

```markdown
---
audience: tinkerer | developer | integrator | operator
type: tutorial | how-to | reference | explanation
prerequisites: <links or "none">
last-verified: YYYY-MM-DD
est-time: <e.g. 15 min>   # tutorials/how-to only
---
```

- `audience` — pick the primary one; a page is allowed more than one but should
  not try to serve all four.
- `type` — one of the four Diátaxis types. Must match the page's actual job.
- `prerequisites` — links to pages the reader should have done first, or the
  literal word `none`.
- `last-verified` — the freshness contract (see below).
- `est-time` — tutorials and how-to guides only; omit on reference and
  explanation pages.

## Naming

- Files are `kebab-case.md` — lowercase, words separated by hyphens. No spaces,
  no underscores, no camelCase. `flash-firmware.md`, not `Flash_Firmware.md`.
- **Verbs for how-to guides.** A how-to guide is named for the task it
  accomplishes: `flash-firmware.md`, `deploy-on-a-dragon.md`,
  `recover-a-bricked-tab5.md`.
- **Nouns for reference pages.** A reference page is named for the thing it
  documents: `websocket-protocol.md`, `debug-server.md`, `nvs-settings.md`.
- Tutorials are named for the outcome: `getting-started.md`,
  `your-first-voice-turn.md`.
- Explanations are named for the concept: `how-the-stack-fits-together.md`,
  `the-multi-model-router.md`.

## Code blocks

- **Always language-tagged.** ` ```bash `, ` ```python `, ` ```json `,
  ` ```c `. Never an untagged fence. The tag drives syntax highlighting on
  GitHub and tells the reader what they are looking at.
- **Copy-pasteable.** A reader should be able to select a command and run it.
  Use absolute paths or state the working directory. Do not embed prose inside
  a command block.
- **Show expected output where it aids verification.** When a command's value
  is in its output, show what success looks like:

  ```bash
  curl -s http://192.168.1.90:8080/info | python3 -m json.tool
  # → {"auth_required": true, "version": "0.8.0", ...}
  ```

  Mark output lines with a leading `# →` or put them in a separate block so they
  are clearly not part of the command.

## `last-verified` discipline

`last-verified` is the freshness contract: it states the date someone last
checked the page's claims against the actual code or hardware.

- When you re-verify a page — you ran its steps, you confirmed the API still
  matches — bump the date to today.
- A stale date is a **signal, not a lie.** It tells the reader "this was true on
  this date; the code may have moved since." That is honest and useful. Do not
  bump the date without actually re-verifying — a fresh date you did not earn is
  worse than an old one.
- If you change behavior that a page documents, update the page and the date in
  the same PR.

## Cross-repo links

- Use the standard peer block (in `docs/_templates/_HEADER.md`) at the top of
  every `README.md` and `docs/README.md`. Do not hand-roll a different version.
- When linking to another repo, **link to that repo's `README.md` or a
  top-level doc, not deep into its internals.** Internal paths
  (`docs/internal/...`, source files) move and rename; a repo's front door is
  stable. "See [TinkerBox](https://github.com/lorcan35/TinkerBox)" or "see
  TinkerBox's [`docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md)"
  — not a link three directories deep into TinkerBox's `dragon_voice/`.
- The one short explanation that answers "how do these relate?"
  (`docs/explanation/how-the-stack-fits-together.md`) lives in TinkerTab and
  TinkerBox and is linked from the peer block. Do not duplicate that prose into
  other pages — link to it.

## Terminology

- Use the terms defined in `GLOSSARY.md` exactly as they are defined there.
  Do not invent a synonym for a term that already has a canonical name (it is
  "the Dragon," not "the server box"; it is "TinkerON," not "the K144 thing" in
  user-facing copy).
- **Link the first use** of a glossary term on a page to its `GLOSSARY.md`
  entry, so a reader who does not know the term can jump to the definition. Do
  not link every subsequent use — once per page is enough.
- If you need a term that is not in the glossary, add it to `GLOSSARY.md` in the
  same PR.

## Accessibility

- **Alt text on every image.** `![Tab5 home screen showing the idle orb](home-orb.png)`,
  never `![](home-orb.png)`. The alt text describes what the image shows, for
  readers who cannot see it.
- **Descriptive link text.** Link the words that describe the destination:
  "see the [WebSocket protocol reference](reference/websocket-protocol.md)."
  Never "click [here](...)" or a bare URL as link text.
- **Heading hierarchy.** One `#` H1 per page (the title). Sections are `##`,
  subsections `###`. Do not skip levels (no `###` directly under an `#`) and do
  not use heading size for visual emphasis — use the level that matches the
  document structure.
