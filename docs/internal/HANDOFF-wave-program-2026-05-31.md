# HANDOFF — Stabilization Wave Program (2026-05-31)

> **Read this first if you're a fresh Claude Code session picking up this work.**
> The prior session's memory does NOT transfer across machines/accounts — this doc
> is the bootstrap. Everything below is on GitHub; nothing else is needed except
> the hardware + a dev environment.

## What this is
Executing the cross-stack feature audit's "reconnect the wires" remediation,
wave by wave. Each fix is deployed to live hardware, verified, and shipped as its
own PR. Source-of-truth maps:
- `TinkerTab/docs/internal/AUDIT-feature-set-2026-05-30.md` — full feature audit (44-agent), §10 NOW/NEXT/LATER roadmap, §11 doc-vs-code drift (~40 items).
- `TinkerTab/docs/internal/AUDIT-verification-2026-05-30.md` — adversarial code-verification (~135 claims, ~90% confirmed, 1 FALSE, 11 OVERSTATED — read the corrections).
- `LEARNINGS.md` (both repos) — root causes for every fix below.

## The stack
- **TinkerTab** = Tab5 (ESP32-P4) firmware, C/ESP-IDF **5.5.2**. The FACE.
- **TinkerBox** = Dragon Q6A (Radxa, ARM64) Python server. The BRAIN.
- **TinkerClaw** = OpenClaw-fork agent gateway on Dragon (voice mode 3).
- Six voice tiers: 0 Local / 1 Hybrid / 2 Cloud / 3 TinkerClaw / 4 Onboard-K144 / 5 Solo-OpenRouter.

## DONE (verified live + PR'd)
| Wave | Fix | PR | Verified |
|---|---|---|---|
| 1 | heap_wd `sram_exhausted` false-reboot + YOLO throttle | TinkerTab **#748** | stressed SRAM→14KB, no abort, 0 reboots |
| 1 | WS-zombie reboot: Dragon heartbeat 60→180 | TinkerBox **#378** | 100s idle WS, 0 drops, no reboot |
| 2 | typed-turn K144-failover hijack (chat broken in ALL Dragon modes) | TinkerTab **#749** | all 4 modes reply now |
| 2 | Local `recall` + `schedule_reminder` in `_FAST_NATIVE_TOOLS` | TinkerBox **#379** | recall + reminder fire+deliver verified |
| 2 | photo→ask `user_image`→`user_media` | TinkerTab **#750** | Dragon vision turn describes captured photo (CORE; Tab5 chat-render is a follow-up) |

PR #750 also carries the LEARNINGS + audit docs. **None of these are merged to `main`** —
to reproduce the live "all-fixes" build you must merge/cherry-pick all 5, or
build from a tree with all of: `heap_watchdog.c`, `vision_service.c` (#748);
`voice_modes.c`, `voice_ws_proto.c` (#749); `voice.c` (#750) on the Tab5 side.

## REMAINING (the program continues here)
- **Wave 3 — Safety criticals** (audit §5.2, §9.1): OTA auto-rollback NOT compiled in (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` unset → bad build bricks to USB-reflash; gate `mark_valid` behind WS `session_start`+home-painted); default wakeword mic-leak (`wake_src=dragon` idle-streams mic, Dragon has no WAK0 handler → default to `ext_pcm`/`off` + Settings picker); privacy-lock no-op on voice (`voice_modes_route_text` only on the typed path); server-side spend cap dead (`cost_mils` computed, never `add_event`'d); write-tool confirm gate (`gmail_send`/`calendar_cancel` fire with prose-only confirm).
- **Wave 4 — Agentic**: TinkerClaw MCP servers fail to start (`Connection closed` — redundant with native tools but noisy) + **~32s/turn tool-bundle overhead** (`openclaw-tools:plugin-tools` in the gateway agent-run trace) + no native image-gen (needs `MINIMAX_API_KEY` env on the gateway).
- **Wave 5 — Legibility + cleanup**: effective-route mode label (pill shows stale model e.g. `GPT-5.5` while Granite/MiniMax run — the unshipped slice 3.6); Solo replies don't update `/voice last_llm_text` (render to chat only); FTS5 hybrid search built-but-never-queried; presence-dim orb behavior has no producer; **+ 2.3 photo-render last-mile** (camera-initiated vision response has no chat placeholder bubble to land in); **+ replayed native-tool messages show raw `<tool>` markers** in the chat.

## Set up a fresh machine to continue
1. **Clone both repos**: `lorcan35/TinkerTab`, `lorcan35/TinkerBox`. `git fetch --all` to get the 5 fix branches (`fix/heap-wd-sram-reboot`, `fix/text-turn-k144-failover-hijack`, `fix/photo-ask-user-media`, `fix/ws-heartbeat-zombie-reboot`, `fix/wave2-reconnect-wires`). Note the TinkerBox wave branches stack on `fix/dictation-w0` (PR #377), not main.
2. **ESP-IDF 5.5.2** pinned (matches `dependencies.lock`). `. <idf>/export.sh`; `idf.py set-target esp32p4`; `idf.py build`; `idf.py -p /dev/ttyACM0 flash` (then `python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac` to boot the app).
3. **Find the devices on the new network** — IPs are DHCP and WILL change. `nmap -p 8080 --open <subnet>/24` (Tab5 is the one whose `GET /info` returns `auth_required:true`); `ping radxa-dragon-q6a` / `nmap -p 22,3502 --open` for Dragon. Tab5 prints its IP on boot serial.
4. **Tab5 debug server**: port 8080, bearer token in `CLAUDE.md` (e2e section). Endpoints: `/info /screen /screenshot.jpg /touch /navigate /mode?m=0..5 /chat /voice /dictation /settings /vision/state /m5`.
5. **Dragon SSH**: user `radxa`, password in `CLAUDE.md` (use `sshpass -p '<pw>'` or copy your own key with `ssh-copy-id`). Voice service `tinkerclaw-voice`, gateway `tinkerclaw-gateway`. Deploy = `scp dragon_voice/<file> radxa@<ip>:/home/radxa/dragon_voice/` + clear `__pycache__` + `sudo systemctl restart tinkerclaw-voice`. Health: `curl <ip>:3502/health`.

## Gotchas the prior session hit (save yourself the time)
- **Dragon restart is SLOW** (~30-45s; the shutdown drain). `systemctl is-active` shows `deactivating` mid-restart — that's normal, wait + re-check `/health` for 200.
- **Multi-branch build trap**: each fix is on its own branch off main (or off dictation-w0). Building from one branch silently DROPS the others' fixes → flashing regresses the device. Merge them into an integration tree first; verify all fixes present (grep) before flashing.
- **Screenshots hit `screenshot_in_flight`** — the always-on YOLO/camera monopolizes the single HW JPEG engine. Retry 3-8×.
- **Verifying reboots**: check `uptime_ms` monotonicity, NOT the stale `reset_reason` (it reflects the LAST boot, not a new crash). `reset USB` = clean flash boot.
- **Workflow tool**: heavy parallel fan-out (16-wide) trips a server-side 429; pace to ~5-6 concurrent.
- **The owner must still ROTATE** the MiniMax/OpenRouter/coding-plan keys that passed through chat historically (noted in memory + the audit).
