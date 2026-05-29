<!-- CANONICAL SNIPPETS — copy into every repo's docs/_templates/. Not a rendered page. -->

This file is the single source of truth for the three snippets every repo
reuses: the standard page header, the "Part of the Tinker stack" peer block,
and the README audience router. Copy these verbatim — they must stay
byte-identical across TinkerTab, TinkerBox, PingOS, and TinkerClaw so a reader
who learns one repo can navigate any of them.

## Standard doc header (top of every authored page)

```markdown
---
audience: tinkerer | developer | integrator | operator
type: tutorial | how-to | reference | explanation
prerequisites: <links or "none">
last-verified: YYYY-MM-DD
est-time: <e.g. 15 min>   # tutorials/how-to only
---
```

## Peer block (top of every README.md + docs/README.md)

```markdown
> **Part of the Tinker stack** — four repos, each documented on its own:
> [**TinkerTab**](https://github.com/lorcan35/TinkerTab) (Tab5 device firmware) ·
> [**TinkerBox**](https://github.com/lorcan35/TinkerBox) (Dragon inference server — "the brain") ·
> [**PingOS**](https://github.com/lorcan35/PingOS) (website-to-API automation gateway) ·
> [**TinkerClaw**](https://github.com/lorcan35/TinkerClaw) (agent sidecar).
> New here? Each repo's README is its own front door.
```

## Audience router (top of every README.md, under the peer block)

```markdown
**I want to…**
→ [**use it**](docs/tutorials/getting-started.md)
· [**build / modify it**](docs/how-to/dev-setup.md)
· [**integrate with it**](docs/reference/README.md)
· [**run it in production**](docs/how-to/deploy.md)
```
