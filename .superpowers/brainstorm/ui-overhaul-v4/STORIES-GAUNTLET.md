# v4·D — The Gauntlet

> Ten ruthless user stories. Each runs 10+ minutes of interaction time, crosses 5+ subsystems, pits real humans against real failure modes. The point is not to prove the design works — it's to prove it **doesn't break when people behave like people**: late, tired, rushed, interrupted, inconsistent, forgetful, emotional.
>
> Each story ends with a **Finding** → concrete gap, severity, and the design revision it forces.
>
> Personas are fictional but the scenarios are not. Every gotcha in this file has been observed in the wild on some voice-first device or has been predicted by operators who shipped one.

**Ground rules:**
- Stories start with weather, not a prompt. Context before input.
- Time matters. A 3 AM tap is different from a 3 PM tap.
- Interruptions are the norm, not the edge. Phones ring. Dogs bark. Partners walk in.
- Money is real. Budgets empty. Bills shock. Trust breaks when a silent $4 appears.
- Memory is unreliable. Six weeks ago Tinker learned something. Will it know today?

---

## G1 · "Thursday meltdown"
**Personas:** Emile (36, owner), Leila (partner, on a work Zoom), Nico (8), Amélie (5), Marigold (cat). **Time:** 07:42 AM, rain. **Mode:** Hybrid (studio voice, local brain), cap $1/day, spent so far today $0.

**Scenario.** Kitchen. Leila's Zoom is in 8 min. Emile's pouring cereal with one hand, Tab5 on the counter. Nico's asking how to spell "rhinoceros" for his homework. Amélie's crying because her milk is the wrong temperature. Emile long-presses Tab5 strip: "Grocery list — oat milk, rhinoceros, no wait — oat milk, coffee beans, olive oil." He releases. Dictation auto-stops. Pipeline processes: STT returns *"grocery list oat milk rhinoceros no weight oat milk coffee beans olive oil"*. Local qwen3 tries to make a note but confuses the intent. Amélie at the same time yells "I want MY cup!" — Tab5's mic picks it up and a second turn queues. Nico now walks up to Tab5 and says "what's a rhinoceros". Tinker, still processing the first dictation, queues the third turn. Four turns in flight. Voice overlay shows "thinking". Emile stares — no feedback what's happening. He taps cancel. WS sends `{"type":"cancel"}`. Pipeline drops current turn. The other two queued turns are still buffered server-side. One fires anyway: Tinker starts speaking *"A rhinoceros is a large…"* at full TTS volume over Leila's Zoom. Leila muting frantically, Zoom apologies all round.

**Finding.** ⚠ **CRITICAL.** Multi-turn queuing without user-visible state. No "turn 2 of 4 queued" indicator. Cancel only drops the current turn, not the queue. The voice overlay needs a **queue badge** (e.g. `+2 queued`) and Cancel needs to offer "cancel queue" vs "cancel current". Also: mic should pause-on-TTS-start, not during processing — it picks up kid chatter during the 30s processing window and queues phantom turns.
**Severity:** P0 for households. Breaks trust in ~4 minutes of normal family noise.
**Design revision:** Add `queue_depth` field to voice state, show in voice overlay, surface "cancel all" long-press. Mute mic input during processing by default, add "always listening" override in settings.

---

## G2 · "The frozen pipeline"
**Personas:** Emile, alone. **Time:** 14:15 on a Tuesday. **Mode:** Smart + Studio (Full Cloud). **Context:** Sonnet 4 is having a partial outage — OpenRouter returns 500 for ~30% of requests, random distribution. Haiku is stable. User doesn't know this.

**Scenario.** Emile is drafting a workshop plan across 20 turns with Sonnet. Turn 21: asks Sonnet to summarise the first half. OpenRouter returns 500. Dragon's retry logic catches it, trims the context aggressively (per the 429 handler), retries — succeeds on second attempt but with a smaller context window. Response is noticeably worse than turn 20's. Emile retries: "No, go deeper on the sensor-priors part." Sonnet 500s again. This time the trim throws away the priors discussion entirely. Response is generic. Emile mutters "why is she losing the plot". Turns 22–24: alternating successful Sonnet turns (full context, great) and truncated Haiku-like quality turns (when fallback to reduced-context retry hits). The CHAT BUBBLE STAMPS show `sonnet-4 · $0.04` on every turn even on the reduced ones — the receipt reports the *winning* model, not that a retry with degraded context happened. Emile blames "inconsistency", loses flow, saves, quits.

**Finding.** ⚠ **HIGH.** Quiet degradation. The user has no signal that what they got was "Sonnet-but-with-trimmed-context". Receipt lies by omission. Per STORIES.md G7 (silent auto-escalation violates trust), this is the same failure mode inverted: **silent auto-DEgradation violates trust**. A retry after 500 should either: surface a chip "↻ retried with shorter context", or escalate visible on a 2nd consecutive retry.
**Severity:** P1 for power users. Manifests only on partial outages, which are common for cheap-tier providers.
**Design revision:** Dragon's retry logic must emit a receipt flag `{"retried": true, "context_trimmed": 4096}` so Tab5 can stamp the bubble with a `↻ RETRY` chip. On 2nd consecutive trim, emit `{"type":"error","code":"degraded"}` and offer one-tap downgrade to Haiku with clear messaging.

---

## G3 · "Grandma's visit"
**Personas:** Emile, Nour (78, mother-in-law, visiting, hard of hearing, fragile English). **Time:** 16:00 Saturday, sunny. **Mode:** Local, quiet hours off.

**Scenario.** Nour sees Tab5 glowing on the counter. "Emile, what is it?" Emile hands her the device: "Tap the orange circle, say 'what's the weather'". Nour holds it too close to her face, taps the strip lightly — nothing registers (palm covered). She taps harder; voice overlay opens with TTS-loud chime. She JUMPS. The device reports LISTENING. She says, loudly because she's hard of hearing, "WHAT IS THE WEATHER". Mic clips at the peak of each consonant, Moonshine STT returns `"what is the weather."` correctly. Qwen3 processes — meanwhile she's tapped the device 3 more times by accident, which triggered 3 more LISTENING-STOP transitions. State machine is confused. TTS eventually plays: "It's sunny, 28 degrees." But by then Nour has put the device down face-up and the strip is covered by a dish towel. The TTS is muffled. Emile notices and uncovers. She says "oh, interesting" and doesn't touch it again all weekend. That night, Emile tries to use the device and it's still showing 3 orphan transcripts from her taps in the Notes queue. None of them are from her.

**Finding.** 🟡 **MEDIUM but cumulative.** First-time-user orientation is missing for the voice overlay. Critical: (a) loud chime on overlay-open startles, (b) palm-over-strip is a dead tap, (c) rapid re-taps create state-machine confusion, (d) no haptic to confirm tap, (e) orphan transcripts pile up silently. Per STORIES.md S1.03 + S1.20 combined: we lack both a first-time visual guide and cat/hand-rejection.
**Severity:** P1 for users who have elderly visitors or visitors who have never seen voice-first UI.
**Design revision:** First-time voice overlay = a subtle 2-second onboarding with caption "Tap the orb to speak, tap anywhere to cancel." Add chime-off quiet-hours expansion. Reject multi-taps within 500 ms (beyond the existing 300 ms nav debounce). Orphan transcripts older than 2 min in Notes queue get auto-pruned.

---

## G4 · "Across two devices"
**Personas:** Emile. **Devices:** Tab5-home (`device_id=a4cf12b38e42`), Tab5-studio (`device_id=aabbcc112233`), both registered with the same Dragon (192.168.1.91). **Time:** morning commute.

**Scenario.** At home, 07:55: Emile is in Hybrid mode. He says "Tinker, remember — Priya's birthday is June 14". Turn runs: `<tool>remember</tool>` fires, fact stored in Dragon's memory service with device_id `a4cf12b38e42`. Tinker confirms. Emile leaves home. At studio, 08:40: He hasn't used studio Tab5 in a week. It boots, reconnects WS to Dragon, resumes session. Session is same? Different? He taps strip: "when is Priya's birthday?" Turn runs on studio Tab5 → Dragon. memory service `recall` tool fires. Returns nothing — because memory is scoped per-device or per-session, depending on CLAUDE.md which says "facts stored via remember". Dragon's `MemoryService` stores per-session but falls back to per-user (device_id). Emile gets "I don't know". He says "what?! I just told you this morning!" Retries. Same answer. Loses confidence.

**Finding.** ⚠ **HIGH.** Memory scoping is ambiguous. Is memory per-device, per-session, or per-user? Per CLAUDE.md: "Facts are stored with Ollama embeddings for semantic search." No mention of scope. Looking at code, MemoryService doesn't appear to scope by device_id. It's a single pool. But each Tab5 registers as a separate device on Dragon → creates separate sessions → memory operations are likely session-scoped, meaning the home-device fact isn't available to studio-device. This is the classic **"single user, multiple devices = broken memory"** problem.
**Severity:** P0 for anyone with >1 device. Also P0 for anyone whose Tab5 sessions expire (30 min timeout) — fact is stored but next session is new.
**Design revision:** Memory must be **user-scoped**, not session-scoped. Add a `user_id` field (derived from a user setting, or the first-registered device's id-as-user). `remember` writes to user pool, `recall` reads from user pool. Session is just transient conversation context. Also: Tab5 should have an explicit "owner" concept — maybe NVS `owner_name` that all turns inherit.

---

## G5 · "The 47-widget day"
**Personas:** Emile (heavy skill user). **Skills emitting widgets:** Time Sense (Pomodoro, priority 5), Weather (priority 2), Calendar (priority 8 when meeting < 15 min away, 3 otherwise), EmailSummary (priority 4), GoalTracker (priority 6 if streak broken), Focus-break-reminder (priority 9 if in long block), 3 misc skills. **Time:** 09:00 → 18:00 Wednesday. Mode: Local.

**Scenario.** Through the day, 47 separate `widget_live` emissions. Priority queue always shows the highest-priority one. Calendar emits at 10:22 (8 min before 10:30 meeting): priority 8, wins. At that moment Time Sense was showing (priority 5, focus block). Emile sees Calendar for 3 min, then Time Sense returns (priority 5 is now highest). Emile interprets: Time Sense was briefly replaced and is now back, so everything is fine. At 10:30 he was supposed to JOIN the meeting — Calendar tried to tell him 8 min prior but he didn't look up from his keyboard. He misses the meeting. At 18:00 he asks Tinker "what did I miss today?" Tinker answers based on memory: "You had a focus block from 10:00 to 12:00." It doesn't know about the suppressed widgets — they came and went without being recorded as events.

**Finding.** ⚠ **P0.** Widgets are ephemeral. Our system-d-sovereign.html **F1 mockup shows "+2 MORE" chip** — but we haven't implemented it. Also: **widgets are UI-only, not events**. They don't append to the session event log, so there's no way to ask "what happened today" and get a timeline. Per STORIES.md G17 (composition chip) + G19 (urgency levels): we need both. And we need widgets to double-write to the event bus so "what did I miss" has data to draw on.
**Severity:** P0 for skill ecosystem credibility. If skills can silently lose notifications, skill devs stop trusting the platform.
**Design revision:** (a) Implement +N more chip today. (b) Every widget emission also writes an event row (`dragon_voice.db.events`) with skill_id, priority, tone, body. (c) Add a "Today's widgets" view accessible via long-press on the live slot. (d) Urgency field added to widget schema — `alert` widgets TTS-announce + persist until ack'd.

---

## G6 · "The international contractor"
**Personas:** Emile on a client trip. **Context:** Lisbon hotel, Wi-Fi with captive portal, Tab5 brought from home, Dragon at 192.168.1.91 (home LAN). **Time:** Tuesday 14:00 local (= 07:00 home). **Mode:** was last Hybrid.

**Scenario.** Tab5 boots in hotel, associates to "NH_Collection_Guest" Wi-Fi. WPA2-open. Captive portal blocks HTTP. Tab5 tries dragon at 192.168.1.91 — unreachable. Tries LAN for 2 handshakes, switches to ngrok at `tinkerclaw-voice.ngrok.dev:443`. SSL handshake starts. But ngrok hasn't been touched in weeks; the root-CA cert in Tab5's baked-in bundle is now 6 weeks past a rotation. TLS fails — same `mbedtls_ssl_handshake returned -0x3000` error we've seen. Tab5 flips back to LAN, fails, back to ngrok, fails. Infinite loop. Screen shows "OFFLINE" status. Emile has a client call in 20 min. He needs to record a quick voice memo: "follow up with Ahmed on the sensor-fusion quote". He taps orb, voice overlay opens, LISTENING. He speaks. Dictation auto-stops. Tab5 tries to send to Dragon — can't. Falls back to SD recording. File saved: `/sd/notes/offline-queue/dict-1400-ahmed.wav`. Good. But the auto-title + summary step was supposed to happen on Dragon post-processing. It didn't. The note is titled "untitled recording 2026-04-29 14:00:03.wav" — unsearchable. At home 3 weeks later, he asks Tinker "remind me about Ahmed" — nothing surfaces.

**Finding.** ⚠ **P1.** Offline recordings are write-only. Post-processing (title + summary) is Dragon-side, so offline notes get generic titles. Also: **no reliable TLS cert rotation story**. Tab5's cert bundle is in flash; only OTA rotates it.
**Severity:** P1 for travellers. Manifests whenever ngrok (or any cloud endpoint) rotates certs between firmware releases.
**Design revision:** (a) When offline, local whisper/moonshine-tiny does a best-effort title extraction (first 8 words). (b) Dragon post-processes on next reconnect + updates the note retroactively. (c) Cert bundle updates ship via OTA, and Tab5 logs a warning at startup if any bundle cert expires within 30 days. (d) Captive portal detection via HTTP 302 probe + UI prompt ("this network needs sign-in").

---

## G7 · "The budget surprise"
**Personas:** Emile. **Context:** Cap set to $2/day a month ago, forgotten. Cloud mode daily. **Time:** mid-afternoon, 6 days into the pattern.

**Scenario.** Monday 10 AM: Emile asks Tinker to help draft an email. Cloud mode, Sonnet. Gets great response. Cost that turn: $0.04. Similar turns through the morning. By 13:30 he's spent $2.01 (receipts visible but he never looks at the home live-line). Next turn trips cap, auto-downgrade fires, orb flips emerald, toast appears: "Budget cap hit — Local mode". He doesn't see the toast because he's looking at his laptop. He asks Tinker: "Can you rewrite that to sound more formal?" Turn runs on qwen3:1.7b. Response is noticeably worse. He tries again: "No, more formal." Same quality. By 14:00 he's muttering "Tinker's being dumb today". Does a `git log` to check — no deploy. Blames vibes. Continues to use local-quality responses all afternoon. Tuesday: same pattern. Wednesday: same. Thursday he complains to Leila: "I think Tinker learned bad habits." Leila asks "did you change anything?" He says no. Reports bug. Spends 15 min writing a bug report.

**Finding.** ⚠ **P0 for trust.** Toast notification is insufficient. A single ephemeral toast can be missed. The user thinks their assistant has **gotten dumber**, not that they've hit a budget cap. Per system-d-modes.html M7 mockup: the toast + downgraded strip copy + emerald orb was supposed to make this obvious. In practice, most users look at the screen ~5% of the time they interact — voice-first means they're looking elsewhere.
**Severity:** P0 for first-time-cloud users. Happens once per user, permanently breaks trust.
**Design revision:** (a) Auto-downgrade fires a SPOKEN TTS alert: *"Budget cap reached. I'll use the local model for the rest of today."* (3 sec, amber priority). (b) Mode chip on home grows a small persistent `CAPPED` badge until midnight. (c) First time this happens, show a one-time explainer screen on next tap (dismiss + don't show again). (d) Receipt stamp on next bubble reads `↓ CAPPED · LOCAL` for the first 3 turns after downgrade.

---

## G8 · "The 8-hour focus"
**Personas:** Emile writing a grant application. **Time:** 09:00–17:00 Friday. **Mode:** Local. **Skill:** Time Sense Pomodoro locked for a full 8 hours.

**Scenario.** 09:00 Emile says "Tinker, start a 4-hour focus block, no interruptions." Time Sense emits widget_live. Orb dims to 20% per quiet hours rule extended by focus mode. Tab5 display sleeps at 10 AM. 10:32 he surfaces for a quick fact: "define recursion in one sentence." He taps strip, orb wakes, voice overlay LISTENING. Speaks. Qwen3 processing… 12s for response (slow, but OK). Tinker speaks answer. Overlay auto-hides at 3s idle. Back to focus. 11:45 WiFi drops (router reboot). Tab5 tries to reconnect, backoff timer starts at 2s. Time Sense widget can't update from Dragon — Dragon is unreachable. Emile looks at his screen: the timer shows "2h 15m remaining" but hasn't ticked for 1 min. He worries. Taps screen — orb goes LISTENING. He says "time sense status". No response for 40s — Tab5's trying to complete the turn over a dead WS. Finally local LLM responds "Tinker is having trouble connecting." By then WiFi's back, but now Emile has broken flow. 30 min of flow state lost.

**Finding.** ⚠ **P0 for focus-mode users.** The Tab5 display **should not sleep during an active focus block** — this blocks glance-monitoring. Also the timer client-side must tick off a cached start+duration, not rely on Dragon for every update. The "Tab5 offline mid-focus" case leaves the user uncertain.
**Severity:** P1. Focus mode is aspirational but already promised in skill.
**Design revision:** (a) Time Sense timer is **local-authoritative** — Dragon emits `widget_live_update` only for state transitions (pause/resume/end), not per-second. Local Tab5 ticks from a cached `started_at + duration` fallback so offline just means "no external events", not "no clock". (b) During active focus block, Tab5 display dims to 40% (not 20%) and doesn't sleep. (c) Connectivity issues during focus render a small grey badge "offline · focus continues" near the timer, not a voice overlay interruption.

---

## G9 · "The stranger's voice"
**Personas:** Emile (37), Benjamin (his older brother, visiting, IT person, tendency to prank). **Time:** Saturday evening, Emile in the shower. Benjamin alone in the kitchen with Tab5.

**Scenario.** Benjamin picks up Tab5 to poke around. Long-presses the mode chip, sees the triple-dial sheet. Reads "Memory Bypassed" on the agent composite. Goes back. Looks in Settings, finds nothing he can't revert. Then says out loud "Hey Tinker, clear my memory". The wake word is parked — nothing happens. He taps strip, voice overlay LISTENING. Says "clear my memory of everything about Emile". Dragon processes: Sonnet (if Cloud) or qwen3 (if Local). The model interprets this as a real user command: calls `<tool>forget_about</tool>` or the lack of such a tool just means the LLM replies in prose "OK, I'll forget." No actual memory wipe. But wait — there IS a remember tool, and its mirror is NOT listed in our current tools. Benjamin tries more creative phrasings: "delete the stored fact about Priya's birthday." The LLM says "done." Again no real deletion. But Emile, coming back from the shower, notices Tinker says "I'll forget about Priya's birthday" (visible in chat). He panics. Asks Tinker "do you remember Priya's birthday?" Tinker (Sonnet, good recall) correctly answers June 14. So nothing was actually wiped — but the chat shows what *looks* like a wipe confirmation. Emile doesn't trust the system for a week.

**Finding.** 🟡 **MEDIUM.** LLMs can claim actions they can't perform. Specifically: our `remember` tool is wired; `forget` / `delete_fact` is not. The model hallucinated a wipe that didn't happen. But the user-facing transcript says "I'll forget" — which is the same output as if it HAD forgotten. No way to tell the difference.
**Severity:** P1 for trust. Especially bad because it also works in REVERSE — if we ship a real `forget` tool, anyone can wipe memory. Needs authn/confirmation for destructive actions.
**Design revision:** (a) Add an explicit `forget_fact` tool with a dry-run mode that says "I would forget X — confirm?". (b) Require owner-voice detection or PIN before destructive tools run. (c) ALL responses that imply action must trigger a tool call or be prefixed "(advisory)" — LLM prompt-engineered to never claim an action it didn't perform.

---

## G10 · "The double receipt"
**Personas:** Emile, alone, Haiku mode. **Context:** Flaky network — TCP retransmits sometimes, but still works. **Time:** Tuesday 11:30 AM.

**Scenario.** Emile asks: "Summarise the BMI270 datasheet section on magnetometer calibration." Sonnet turn starts. Response streams cleanly for 2 seconds, then the phone modem takes over from Wi-Fi for ~300ms (unannounced automatic failover on the home router). Aiohttp's session keeps the TLS socket open via TCP retransmits, but the SSE stream hiccups. The tail chunk with `usage` arrives TWICE — once via the originally queued packet (delivered after the modem failover) and once via the retransmit. Dragon's parser isn't idempotent on `usage` — it captures `_last_usage` both times, but only the SECOND capture counts. OK, no double-count at the LLM level. BUT: `_handle_text` in server.py reads `get_last_usage()` once and emits a receipt. Single receipt. So far so good. Then the same hiccup triggers aiohttp's client-side retry on the outer request — the whole LLM call is re-fired. Now Dragon sends the same question to OpenRouter twice. Two DIFFERENT responses stream back (LLMs aren't deterministic). Dragon's `ConversationEngine` handles the second as a new turn in the same session. Two separate receipts emitted. Tab5 accumulates both. Emile sees his spend jump by 2× what it should be. Checks OpenRouter dashboard next day: yes, two calls. Overcharged because of network flake.

**Finding.** ⚠ **P1 financial.** Idempotency isn't enforced on the outer LLM request. Any retry in the aiohttp client layer causes a duplicate billable call. We've already seen retries happen on 429 rate-limit (code path exists in `openrouter_llm.py`) — same mechanic applies transparently to TCP reconnects.
**Severity:** P1 for cost accuracy. User trust in receipt accuracy is shot the first time the math doesn't line up to the bill.
**Design revision:** (a) Attach a client-generated `X-Idempotency-Key` (UUID) per outgoing OpenRouter request. On retry, OpenRouter de-dupes. (b) Dragon caches seen idempotency keys for 5 min and rejects duplicate tail-usage emissions. (c) Receipt WS message carries the idempotency key so Tab5 can also de-dupe on its side (belt + braces). (d) Display a rolling "turns vs OpenRouter-billed turns" diff in Settings → Usage — if they ever drift by >3%, show a warning.

---

## Gap tally from the gauntlet

| # | Gap | Severity | Concept |
|---|-----|----------|---------|
| G1-F | Multi-turn queue badge + cancel-all | P0 | voice overlay |
| G1-F2 | Mute mic during TTS playback | P0 | voice pipeline |
| G2-F | Retry-trim receipt flag + degrade chip | P1 | receipts | ✅ SHIPPED (TB 5eab5a7 + TT on feat/ui-overhaul-v4, 2026-04-19) — "RETRIED" chip in stamp |
| G3-F | First-time overlay onboarding | P1 | voice UX |
| G3-F2 | Multi-tap 500 ms debounce | P1 | touch | ✅ SHIPPED (pre-existing in orb_click_cb:955 + 300 ms nav debounce per CLAUDE.md) |
| G3-F3 | Orphan-transcript TTL | P1 | notes |
| G4-F | User-scoped memory (not session) | P0 | memory |
| G5-F | Widgets write to event bus | P0 | widget platform |
| G5-F2 | +N more chip (designed, unbuilt) | P1 | home live-slot | ✅ SHIPPED (commit on feat/ui-overhaul-v4, 2026-04-19) — "SKILL +N" kicker format |
| G5-F3 | Urgency field on widget schema | P1 | widget platform |
| G6-F | Offline note title extraction | P1 | dictation |
| G6-F2 | Cert bundle expiry warning | P1 | TLS reliability |
| G6-F3 | Captive-portal detection | P1 | network |
| G7-F | Spoken TTS alert on auto-downgrade | P0 | budget UX | 🟡 pending |
| G7-F2 | Persistent CAPPED banner | P0 | status bar | ✅ SHIPPED (commit on feat/ui-overhaul-v4, 2026-04-19) |
| G8-F | Local-authoritative timer | P0 | focus mode |
| G8-F2 | Display persists during focus | P1 | focus mode |
| G9-F | `forget_fact` tool + auth gate | P1 | memory |
| G9-F2 | "(advisory)" marker for non-action responses | P1 | conversation engine |
| G10-F | Idempotency keys on LLM requests | P1 | cost accuracy | ✅ SHIPPED (TB 2026-04-19) — UUID per outer turn, reused on internal retries |
| G10-F2 | Usage dedupe cache on Dragon | P1 | cost accuracy |
| G10-F3 | "Turns vs billed" diff in Settings | P1 | cost accuracy |

**22 new gaps surfaced.** Nine P0s — stuff that breaks trust the first time it happens. Thirteen P1s — stuff that erodes trust over weeks.

## How to use this file

- Before shipping any major Phase 4+ feature, run at least 3 of these scenarios in a simulator or on-device. If the system doesn't handle it cleanly, don't ship.
- New stories go at the bottom as G11, G12, etc. Keep the "Finding → Design revision" shape.
- Cross-reference story IDs in PROGRESS.md so a fix knows what it unblocks.
