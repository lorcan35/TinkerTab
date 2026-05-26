# PLAN — UI/UX Remediation (2026-05-26)

> Source-of-truth for the post-audit UI/UX fix program. Tracking issue links here.
> Supersedes nothing — complements [`PLAN-ui-ux-hardening.md`](PLAN-ui-ux-hardening.md)
> (the 2026-04-29 wave that closed the *first* a11y/contrast round). This plan
> addresses a **second, deeper** audit focused on cross-surface coherence
> ("disjointedness") and a stability defect found while exercising the device.

## How we got here

Two grounded audit passes on the integrated build (branch `test/ux-integration`,
flashed to Tab5 `192.168.1.90`):

1. **Best-practices + disjointedness review** — rubric (ui-ux-pro-max) + the A–E
   journey screenshots. Surfaced cross-surface contradictions (naming, time,
   mute) and rubric gaps (contrast, stale copy, hierarchy).
2. **Day-in-the-life exhaustive pass** — drove *every* reachable surface and
   every state that could be safely induced (Settings full scroll, Notes
   lifecycle incl. dictation RECORDING/TRANSCRIBING, Files, Camera, Memory,
   Agents, Skills, Sessions, all voice states, reconnect, OTA). This pass
   **reproduced a crash** and a dead-home recovery state.

All findings below are screenshot- or code-grounded (file:line where a fix site
is known).

## Sequencing principle

**Fix what's broken before what's ugly; fix coherence before polish.**

Wave 0 (stability) gates everything — UX polish is moot on a build that crashes
under realistic load and leaves the home screen unusable. After that:
broken-looking screens → naming/coherence (the literal "disjointed" complaint)
→ control consolidation → Settings IA → time → polish.

Each wave ships as one or more discrete PRs (issue → branch → PR, `closes #N`).

---

## Wave 0 — Stability + crash recovery  `S1`

**Goal:** the device cannot crash under normal mixed use, and crash-recovery
must always restore a usable home (or fail loudly), never a stale frame.

**Finding (DITL #1):** During a realistic burst — start dictation (→ TRANSCRIBING)
+ several back-to-back `/chat` turns + heavy navigation + screenshots — the
device silently rebooted (uptime fell ~1.1M ms → ~327 s). After recovery,
`navigate?screen=home` rendered a **blank/stale frame** (orb/pill/title pixels
near-black `(15,16,20)`, confirmed via both JPEG and BMP codepaths), home taps
were dead, and `/screen` **falsely reported `"home"`**. Settings/Notes/Camera
rendered fine throughout. Only a manual full reboot restored home; fresh boot
renders home perfectly.

**Work:**
- Pull coredump (`/coredump`) + serial backtrace; reproduce the trigger.
- Fix the crash mechanism (suspect: dictation-transcribe overlapping rapid turns
  + overlay/nav churn + screenshot-under-render contention; verify, don't guess).
- Make crash-recovery rebuild home — or surface an explicit error — instead of
  leaving a stale lv_screen while the nav layer claims success.
- Reconcile `/screen` so it reports the *actually loaded* screen, not the target.

**Honest unknown:** effort is unknown until the backtrace is in hand — could be a
contained null-guard or a deeper render/nav-task issue.

---

## Wave 1 — The three visibly-broken Dragon-backed screens  `S2`

**Goal:** no screen looks broken; one honest, shared "Dragon services
unavailable" treatment. All three fail for the same root (Dragon REST /
`dragon_tok` auth) but present it three different ways today.

- **DITL #2 — Sessions** (`ui_sessions.c`): "Loading conversations…" never
  resolves — infinite skeleton, no timeout → error/empty fallback.
- **DITL #3 — Skills** (`ui_skills.c`): says **"OFFLINE"** while Dragon is online;
  dev-language "JSON parse failed"; instructs the user to "Set the Dragon API
  token under POST /settings (**dragon_api_token**)" — a debug call no normal
  user can do, **and the key name is wrong** (real NVS key is `dragon_tok`).
- **DITL #4 — Agents** (`ui_agents.c`): self-contradicts — header "0 LIVE · 0
  DONE / No tool activity yet" sits directly above "DRAGON: 42" with 5 entries;
  those entries **dump raw truncated JSON** into the UI; "TOOLS CATALOG: JSON
  parse failed" dev error.

**Work:** shared "can't reach Dragon services — Retry" component; remove dev
language; correct the `dragon_tok` copy; make the empty-state honest about the
populated bucket; format tool activity into readable rows (not raw JSON).

---

## Wave 2 — Naming unification (the core "disjointed" complaint)  `S2`

**Goal:** one canonical name per concept, on every surface.

- **Assistant / on-device LLM** has ≥4 names: home pill "Local / ON-DEVICE",
  status bar "TINKERON", chat header "ON-DEVICE" (modes 0/1) / "K144" (mode 4),
  chat bubble badge "K144 · FREE". And the labels are **backwards**: Hybrid
  (cloud STT/TTS) is tagged "ON-DEVICE" while the truly-on-device K144 is "K144".
  `chat_header.c:259-265`, `chat_msg_view.c:556-571` (`receipt_model_short`).
- **Modes disagree across the two pickers:** Settings VOICE MODE = Local/Hybrid/
  Cloud/**TinkerClaw**/Onboard (5, no Solo); mode-sheet PRESETS = Local/Hybrid/
  Cloud/**Agent**/Onboard/Solo (6). "TinkerClaw" ≡ "Agent"; Solo missing from
  Settings. `ui_settings.c`, `ui_mode_sheet.c`.
- **Conversation history** = Conversations (title) / SESSIONS (subtitle) /
  Chat·Threads·history (nav tile) / `sessions` (route). `ui_sessions.c`.
- **Active cloud model not selectable:** Settings shows Cloud = `claude-sonnet-4.6`
  but the CLOUD LLM chips are GPT/Gemini/Grok only, none marked active.

**Resolved — see "Resolved decisions" + "Mode picker — agreed design" below.**
Canonical table is the deliverable; the raw model list moves to Advanced.

---

## Wave 3 — Control consolidation + color/icon correctness  `S2`/`S3`

**Goal:** one source of truth per control; semantic color/icons.

- **Mute shown in 4 places** (status chip, red top-right button, orb pip,
  Settings→Audio toggle) and **red = "muted"** — semantically wrong; red should
  mean *active* (the dictation RECORDING orb correctly uses red). Mic-mute also
  uses **speaker/music-note glyphs** (output metaphor) on a *mic* (input) control,
  and flips icon between states.
- **Mode/privacy sprawl:** home pill ↔ mode-sheet (dials + presets) ↔ Settings
  VOICE MODE ↔ Settings ENGINES (AUTO/K144/OPENROUTER) ↔ PRIVACY On-device-lock.

**Resolved — see "Mode picker — agreed design" below.** One picker (mode +
smartness up front; exact-model + engine-pin + privacy + cap under Advanced);
intent-vs-effective-route split; `tab5_mode_resolve` change (mode primary).

---

## Wave 4 — Settings information architecture  `S3`

**Goal:** order by frequency-of-use; group correctly; no dev telemetry.

- **Section order inverted:** VOICE MODE → CLOUD LLM → ENGINES → PRIVACY → AUDIO
  → QUIET HOURS → CHANNELS → DISPLAY → NETWORK → STORAGE → BATTERY → ABOUT.
  Brightness/Volume (DISPLAY) are buried under 8 channel toggles.
- **Audio split:** Volume under DISPLAY, Mic-mute under AUDIO; DISPLAY overloaded
  (Volume, Camera rotation, Always-on vision, Vision rate).
- **Dev telemetry leaks:** BATTERY "1.96V"; ABOUT "Heap: 11730 KB | PSRAM:
  11.5 MB"; unlabeled IP "192.168.1.91" (Dragon's) under NETWORK; two
  equal-weight amber primary buttons (Check Update + Show intro again).
- **Onboard row** renders stray `--` placeholders + a detached "· READY" chip.
- File: `ui_settings.c`.

---

## Wave 5 — Timestamp unification  `S2`

**Goal:** one shared local-time formatter across all surfaces.

- ≥4 surfaces, ≥3 formats: top-bar (local, `ui_home.c:1343` localtime_r),
  **chat bubbles (UTC — bug)** `chat_msg_view.c:119-127` (`(ts/3600)%24`, no TZ),
  Notes (local), **Memory mixes relative + absolute in one list**
  ("20 H AGO" vs "May 24 · 11:01"). Chat is ~4 h off from the top-bar on the
  same screen.

**Work:** route all message/fact timestamps through a single `localtime_r`-based
formatter; pick a consistent relative-vs-absolute threshold for Memory.

---

## Wave 6 — Polish  `S3`

- **FAB occlusion:** amber home/dictate FAB overlaps list content on Notes
  (a note's play/delete) and Files (last row) — reserve list bottom-padding.
- **Camera control bar overcrowded:** shutter visually clips "DETECT" → "ƊETECT".
- **Em-dash tofu:** greeting/copy use `--` (font-subset substitute) — add the
  glyph to the Montserrat subset or restructure copy.
- **Lone green status dot** (vision indicator) has no label.
- **Cryptic abbreviations:** Settings tile "cap", Camera "Rot 1".

---

## Resolved decisions (2026-05-26)

1. **Canonical names — LOCKED.** One source-of-truth mode table replaces the six
   divergent arrays (`chat_header.c:37`, `ui_chat.c:256`, `ui_sessions.c:108`,
   `ui_theme.c:18`, `ui_settings.c:1864`, `ui_mode_sheet.c:309`):

   | vmode | Canonical | Was (scattered) |
   |-------|-----------|-----------------|
   | 0 | Local | Local |
   | 1 | Hybrid | Hybrid |
   | 2 | Cloud | Cloud |
   | 3 | **TinkerAgent** | Claw / TinkerClaw / Agent |
   | 4 | **TinkerON** | Onboard / K144 / ON-DEVICE |
   | 5 | Solo | Solo |

   - **TinkerClaw** stays the *platform/ecosystem* name (Agents screen, the
     gateway). **TinkerAgent** is the *mode*.
   - **Local's "ON-DEVICE" subtitle is retired** — it collides with TinkerON
     (the actual on-device addon). Local = the Dragon box on the LAN → retag
     "Dragon" / "Private".
2. **Two conversation surfaces — KEPT.** Voice-overlay thread + Chat screen stay
   (may be needed); they already share one store.
3. **Mode pickers — UNIFIED, see design below.**

## Mode picker — agreed design

**Principle: explicit control + smart defaults + honest reality.** Modes are not
redundant — each is a curated point on real axes (privacy / topology / capability),
and TinkerON + Solo are the two "no-Dragon" escape hatches. So we keep all six as
deliberate, first-class choices, but make them legible, single-sourced, and honest.

### The key insight: intent ≠ effective route
The selected mode is **intent**. What actually serves a turn is the **effective
route**, and three mechanisms change it at runtime without asking:
- Failover — Local with Dragon down → TinkerON (`voice_modes.c:174`).
- Budget cap — daily spend hit → force-downgrade to Local (`voice_billing.c:109`).
- Availability — TinkerON needs the addon warm (K144 cycles); Solo/Cloud need a key.

So the earlier "naming disjointedness" (chat badge said "K144" while mode was
"Local") was **not a bug — it showed the effective route.** The fix is to make the
distinction explicit:
- **Mode chip = intent** ("you're in Hybrid"). Read-only everywhere; opens the one
  picker. The chat-header chip stops *cycling* on tap → it opens the picker.
- **Orb / per-turn badge = reality** ("answered by TinkerON — Dragon offline").

### Two-level control
**Up front — "How should she think?" (one picker, rendered identically by the
orb-long-press sheet AND Settings, from one source):**
- **Mode** — six reason-cards. Each shows *why you'd pick it* + **what leaves your
  device** + speed + cost + requires/availability. Example layout:

  ```
  HOW SHOULD SHE THINK?                          you're in: Hybrid
  ──────────────────────────────────────────────────────────────
  (•) Hybrid      Private brain, fast voice            recommended
       leaves device: your voice (STT) ·  ~4–8s ·  ~2¢/turn
  ( ) Local       Nothing leaves ·  free ·  ~60s ·  Dragon
  ( ) Cloud       Voice + text leave ·  smartest ·  ~5s ·  needs key
  ( ) TinkerAgent Voice + text + tools ·  agentic ·  bypasses memory
  ( ) TinkerON    Nothing leaves ·  works offline       ⚠ attach addon
  ( ) Solo        Voice + text leave ·  no Dragon needed  needs key
  ──────────────────────────────────────────────────────────────
  Advanced ▸   pick exact model · pin engine (LLM/STT/TTS) · privacy · cap
  ```

- **Smartness** (Fast / Balanced / Smart) — *how hard she thinks, within the chosen
  mode's available brains.* **KEPT** as the legible knob.

**Advanced ▸**
- **Pick the exact model** — overrides the smartness default (e.g.,
  `claude-sonnet-4.6`). This is where the raw model list lives now (out of the main
  card; removes the tier-vs-raw-list "how smart" redundancy).
- Pin engines independently (LLM / STT / TTS), privacy lock, spend cap.
- Touching any of these → chip reads **"Hybrid · modified"** (preset-drift state).

### Behaviour rules
- **Smartness only has range where there's a choice of brains** — Cloud/Solo (and
  Dragon if multi-model). **TinkerON has one model (K144 qwen2.5-0.5B)** → smartness
  greys out there with a note. Same "honest availability" rule everywhere.
- **Decouple smartness from mode.** Today `int_tier ≥ Smart` *silently forces cloud*
  (`settings.c:861`). New model: mode is primary; "Smart" picks the smartest brain
  *available in that mode* — it does not jump topology. Want cloud-smart → pick
  Cloud. **This is a real change to `tab5_mode_resolve`** (mode primary, smartness =
  within-mode selector) — structural, lands in Wave 2/3.
- **Capability-gate the picker** — no addon → TinkerON greyed ("attach addon"); no
  key → Cloud/Solo prompt the key (QR). Selecting an unavailable mode prompts the
  fix; a mode going unavailable mid-use surfaces on the orb + auto-falls-back.
- **Entry-point discipline** — one picker. Home pill + chat-header chip + Settings
  all become read-only chips that *open* it. No surface owns its own picker.

### Maps to issues
- Naming table + single source of truth → #723 (Wave 2).
- Picker unification, intent-vs-route, smartness/model split, resolver change,
  entry-point discipline → #724 (Wave 3).

## Wins to protect (don't regress)

Sessions skeleton-loading pattern; Notes date-grouping + retry chips + failure-
reason chips; Files clean list; **correct** red on the dictation RECORDING orb;
clean voice state transitions (PROCESSING→SPEAKING→READY); consistent
conversational section headers ("Where to?", "How should she think?"); graceful
OTA error ("Check failed", not raw `ESP_ERR_NOT_SUPPORTED`).

## Already shipped this cycle

- Video calling **commented out** (`ui_nav_sheet.c` — `go_call` + Call tile under
  `#if 0`); voice-first, reduces crash surface. Module + `/call` `/video` debug
  endpoints retained for the e2e harness. Built + flashed + verified (nav sheet
  now 8 tiles, clean 3+3+2).

## Not re-exercised in the DITL pass (rely on prior validation)

Channel now-card/toast (inject endpoint compiled out of this build), sustained
RECONNECTING + error banner (Dragon healthy — not taking down a shared service),
video-call screen (heavy; avoided post-crash), quiet-hours dim (outside window).
All live-validated in the W7-E / WS-health audits.
