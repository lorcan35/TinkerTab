# Reference

Authoritative, lookup-oriented documentation for integrators and developers —
endpoints, fields, commands, and protocols. No tutorials here; for a guided
first run see [Get started](../tutorials/getting-started.md).

## Pages

- [WebSocket protocol](websocket-protocol.md) — the Tab5 client side of the
  Dragon WebSocket: every frame type and binary magic tag.
- [Debug server](debug-server.md) — the Tab5's `:8080` HTTP control API:
  endpoints, bearer-token auth, and the post-Wave-23b handler module map.
- [Voice modes](voice-modes.md) — the six tiers, on-the-wire behavior, routing
  codes, and failover guards.
- [NVS settings](nvs-settings.md) — every persistent settings key in the
  `"settings"` namespace.
- [Observability events](observability-events.md) — the `/events` ring: every
  event kind and detail format.
- [Hardware](hardware.md) — the SoC, peripherals, audio pipeline, M5-Bus, and
  memory rules.
- [Firmware file map](firmware-file-map.md) — where everything lives under
  `main/`.

The canonical wire spec is the Dragon's
[`docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md);
see [`../ROADMAP.md`](../ROADMAP.md) for the documentation program waves.
