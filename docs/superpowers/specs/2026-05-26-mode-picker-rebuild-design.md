# Mode Picker Rebuild — Design Spec (2026-05-26)

Wave 3.2 + 3.3 + 3.4 of the UI/UX remediation epic (#720, tracking #724). Builds
on the agreed direction in `docs/PLAN-ui-ux-remediation.md` → "Mode picker —
agreed design". Slices 3.0 (naming), 3.1 (entry-point discipline) and 3.5
(capability gating) already shipped; this spec is the picker UI rebuild + the
mode-primary selection model + the Advanced drawer.

## Problem

The mode UX is spread across two pickers that disagree and a 3-dial abstraction
that leaks:
- The mode-sheet uses **Intelligence × Voice × Autonomy dials** → `tab5_mode_resolve`
  → a vmode (0–3 only). TinkerON (4) and Solo (5) bypass the dials with direct
  vmode writes, leaving the dials showing a stale state.
- Settings has a separate flat VOICE-MODE radio.
- The dials force users to hand-operate routing machinery (int/voi/aut) to express
  a simple intent ("which mode"), and can't represent 2 of the 6 modes.

Goal: one picker where the **mode is the primary, legible choice**, each mode shows
*why you'd pick it*, smartness is a separate within-mode knob, and the plumbing
(exact model, engine pins, privacy, cap) lives under Advanced.

## Key insight that de-risks the change

Runtime routing (`voice_modes.c::voice_modes_route_text`) keys off the persisted
**`vmode`** (0–5) — NOT the dials. `tab5_mode_resolve(int,voi,aut)` only converts
dials → vmode *before* the vmode is written. The mode-sheet presets for 4/5 already
write vmode directly.

Therefore: **the rebuilt picker sets `vmode` directly for all six modes** (extending
the existing preset-4/5 pattern). This does **not** touch the routing core. The
int/voi/aut dials + `tab5_mode_resolve` are *retired from the picker UI* but left
intact for the debug `POST /mode`-from-tiers path and back-compat. "Decoupling
smartness from mode" becomes a UI change, not a routing rewrite.

## Architecture

`ui_mode_sheet.c` is rebuilt from the 3-dial layout to a **mode-row picker**. New
internal pieces, each with a single responsibility:

- **Mode metadata table** (`static const mode_info_t s_mode_info[VOICE_MODE_COUNT]`):
  per-mode display data — canonical name (from `th_mode_names`), one-line reason,
  `leaves` (what leaves the device), `speed`, `cost`, and a `requires` enum
  (NONE / ADDON / OR_KEY / DRAGON). One source for the picker copy.
- **Availability check** (`mode_available(vmode)`): reuses the 3.5 logic —
  ADDON → `voice_onboard_failover_state()==READY`; OR_KEY → `or_key` non-empty
  (≥128-byte buffer). Drives dimming + the refuse-to-select guard.
- **Mode-row list**: six rows; the selected row expands to show reason/leaves/
  speed/cost; unavailable rows dim and refuse selection with a toast.
- **Smartness segment**: Fast / Balanced / Smart, persisted to the existing
  `int_tier` NVS key. Greyed for fixed-brain modes.
- **Advanced drawer**: collapsed by default; expands to model pick + engine pins +
  privacy lock + spend cap.
- **Commit path** (`commit_mode(vmode)`): `tab5_settings_set_voice_mode` +
  `voice_send_config_update` + `ui_agents_on_mode_change` + hide. (Same calls the
  current preset-4/5 path makes — proven.)

## Layout (Option B — compact rows, detail-on-select)

```
  HOW SHOULD SHE THINK?                                   [ Done ]
  SMARTNESS   [ Fast ] Balanced  Smart        (greyed if mode is fixed-brain)
  ─────────────────────────────────────────────────────────────
  (•) Hybrid                                        recommended
       Private brain, fast voice
       leaves: your voice (STT) · ~5s · ~2¢
  ( ) Local                          Nothing leaves · free · ~60s
  ( ) Cloud                          Voice+text · smartest · needs key
  ( ) TinkerAgent                    Tools + memory · agentic
  ( ) TinkerON  ⚠ attach addon       (dimmed when not warm)
  ( ) Solo                           No Dragon · needs key (dimmed if unset)
  ─────────────────────────────────────────────────────────────
  Advanced ▸   model · engine pins · privacy · spend cap
```

- Only the **selected** row shows the full reason/leaves/speed/cost block;
  unselected rows show a single dim meta line. Keeps all six visible without
  scrolling on 720×1280.
- Tapping an available row selects + expands it and **commits** (writes vmode +
  notifies Dragon). Tapping a dimmed row toasts the fix instead.
- "recommended" tag on Hybrid.

## Smartness

Fast / Balanced / Smart, persisted to `int_tier` (0/1/2). It selects the *brain
within the chosen mode*, it does NOT change the mode:
- **Cloud / Solo**: maps to a model tier (cheap / mid / top) applied to the
  cloud-model field used by that mode.
- **Local / Hybrid**: Dragon's model (single) — knob is shown but has limited
  range; acceptable.
- **TinkerON**: one K144 model → smartness greyed/disabled.
- **TinkerAgent**: gateway picks its own → greyed.

This deliberately breaks today's coupling where `int_tier ≥ Smart` silently forced
Cloud (`settings.c:861`). The mode is now explicit; "Smart" picks the smartest brain
*available in that mode*. Want cloud-smart → pick Cloud.

The concrete `int_tier → model` map per cloud mode (which model is "cheap/mid/top")
is an implementation detail decided in the plan — the spec only fixes the behavior:
3 tiers, applied to the mode's cloud-model field, overridable by Advanced → exact
model. For Local/Hybrid/TinkerON/TinkerAgent the knob has no model range and is
disabled (greyed), not hidden.

## Advanced drawer (3.4)

Collapsed by default. Expanded shows:
- **Exact model** — overrides the smartness default (the raw model list currently
  in Settings "CLOUD LLM" moves here).
- **Engine pins** — the existing ENGINES segmented (LLM AUTO/K144/OpenRouter +
  STT/TTS labels) relocates here; this is where "K144" as a hardware label is
  acceptable (technical/advanced surface).
- **Privacy lock** (on-device-only) + **spend cap**.
- Setting any override → the mode chip elsewhere reads "Mode · modified".

## Entry points

Already disciplined in 3.1: the home pill, chat-header chip, and Settings VOICE
MODE all open *this* picker (no cycling). Settings' inline radio is replaced by a
read-only "current mode" row that opens the picker. (Settings change tracked here;
it removes the duplicate radio at `ui_settings.c:1864`.)

## Capability gating

Reuse 3.5: dim + refuse unavailable modes (TinkerON needs addon, Solo needs key).
Already implemented for the preset chips; carries to the new rows.

## Effective route vs intent (3.6 — flagged follow-up, not in first build)

The picker = **intent**. Failover / budget-cap / availability can change the
**effective route** at runtime. The chat bubble already stamps the served model.
Follow-up: when the effective route ≠ intent, surface a one-line "answered by
{X} — {reason}" on the orb/per-turn badge. Specced here; built after the picker.

## Edge cases / error handling

- Tap a dimmed row → toast the fix ("Attach TinkerON module" / "Add OpenRouter key"),
  no mode change.
- Mode becomes unavailable while the picker is open → next open reflects it; an
  active selection that goes unavailable falls back via existing failover (Local↔
  TinkerON) — unchanged.
- Smartness on a fixed-brain mode → control disabled, not hidden (read-only
  distinction).
- Picker opened over a content screen or chat overlay → renders on `lv_layer_top`
  as today.

## Testing

- Build clean off main + `git-clang-format` clean.
- Live (Tab5 192.168.1.90): each of the 6 modes selects + commits (verify `/voice`
  + `/m5` reflect it); dimmed TinkerON/Solo refuse with toast; smartness greys for
  fixed-brain modes; Advanced drawer opens; entry points (home pill, chat chip,
  Settings) all open the one picker.
- Regression: `story_smoke` (nav + a chat turn) stays green.
- Verify routing unaffected: a turn in each mode routes to the expected backend
  (`eng.route` obs events).

## Out of scope

- The 3.6 effective-route badge (follow-up).
- Removing `tab5_mode_resolve` / the int/voi/aut NVS keys (left for the debug
  endpoint; the picker simply stops using the dial UI).
- Wave 4/5/6 (Settings IA, timestamps, polish).
