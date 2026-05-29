<!-- Design spec produced by a 12-agent design workflow (3 approaches -> adversarial
     judge panel -> synthesis -> completeness-critic -> finalize), ground-truthed
     against live source 2026-05-30. Builds on W0 (fix/dictation-w0).
     HUMAN SIGN-OFF 2026-05-30: D1 = Dragon notes/db.py is the sole dictation note
     owner (APPROVED). D2 = two-tier visible cap, Tab5 <=4hr / Dragon 5-min per
     segment / typed dictation_truncated (APPROVED). -->

# Dictation Structural Redesign — W1–W5 Design Spec (FINAL)

**Status:** decision-ready. Builds on W0 (`fix/dictation-w0`, both repos). Ground-truthed against current source 2026-05-30, including the completeness-critic's gap report.

---

## Summary + central design decisions

The dictation feature is sick because **nothing owns the FSM**: two (really three) uncoordinated producers drive one anonymous singleton with no identity, no self-liveness, a resolution contract built on blank-string inference, and a 450-line `if/else` RX dispatcher where a duplicate branch is invisible until it wedges production (the exact W0 bug). The fix makes `voice_dictation.c` the single authority by giving it (a) a `turn_id` session identity minted at start and echoed on every wire frame both directions so it can structurally drop stale frames, (b) self-driven liveness so terminal states decay with no UI mounted, and (c) a typed resolution contract dispatched through a table whose keys are checked for duplicates at host-test time. Everything else — UI as pure renderers, one Dragon-owned note de-duped by id, one retry verb, Dragon STT robustness — derives from those invariants.

This synthesizes the **minimal-coupling spine** (keep one FSM, additive wire fields, dedup-by-id not store-merge, two-tier cap made visible) and **grafts the fsm-protocol-first** turn-id guard + identity-born-in-FSM, plus the **experience-first** debunk that no turn_id guard exists today. The five fatal flaws and how this spec resolves them:

1. **LVGL-from-wrong-task crash (decisive).** `dict_dispatch_locked` fires subscriber callbacks synchronously under the mutex (voice_dictation.c:99-105); the LVGL subscriber marshalls via `tab5_lv_async_call` which `malloc`s — the thread-unsafe primitive PR #259 closed. An esp_timer-task decay that dispatches is a new crash vector. **Resolution: the self-decay timer does NOT dispatch from the timer task. It enqueues a job onto `task_worker` (the existing shared queue) which calls `voice_dictation_set_state(IDLE,...)` from the worker task — the same context existing producers use.** No new task ever touches the dispatch path. **(Critic fix: the real API is `tab5_worker_enqueue(fn, arg, tag)` returning `esp_err_t`, not the invented `tab5_worker_post`; the full-queue failure mode is handled — see §self-liveness.)**
2. **W1-before-W2 data-loss window (decisive).** Self-decay a full wave before identity means a real late `dictation_summary` (Local mode 60–90s/turn) arrives after a 2s SAVED decay → IDLE refuses SAVED → silent loss. **Resolution: identity ships in W1 alongside decay; decay is gated by `resolution_pending`; FAILED→SAVED is a legal late-correction edge.** **(Critic fix: in W1 the turn_id echo from Dragon does not exist yet, so the late-correction edge would fire under "missing=match" for ANY turn — see the W1 scoping note: in W1 FAILED→SAVED is gated additionally on `resolution_pending && origin==WS`, and the cross-turn guard becomes real only in W2 when Dragon echoes turn_id. S2-8 stays open until W2, as scheduled.)**
3. **Offline-queue gate on the wrong state variable + clobber race.** The queue gates on `voice_get_state()` (a separate enum, ui_notes.c:1620), not the dictation FSM, and self-decay can race the 15s poll. **Resolution: `voice_dictation_try_begin_offline()` is a single atomic compare-and-begin under one lock take; lands in W1.** **(Critic fix: the reverse race — a WS dictation starting mid-offline-upload, while the long HTTP POST runs OUTSIDE the lock — is closed by making `voice_dictation_begin(WS)` refuse while `origin==OFFLINE && state==UPLOADING`; see §two-producer reconciliation.)**
4. **Dedup orphans the offline + short-transcript paths.** `note_created` only fires on the WS auto-note path and is skipped for sub-`MIN_DICTATION_CHARS` transcripts. **Resolution: the offline path also creates a Dragon note and delivers `note_created`; the empty path emits `dictation_empty`; Tab5 keeps a turn_id-keyed local row as the always-present anchor.** **(Critic fix: the offline path is REST — `api/synthesize.py`, no WS connection — so `note_created` cannot reach Tab5 as a WS frame. The mechanism is now fully specified: the REST response body carries `note_id`+`turn_id` and Tab5's offline queue consumes them from the HTTP response, not the WS table — see §persistence.)**
5. **"Impossible states" overclaim.** The transition table is a runtime log-and-noop (voice_dictation.c:149-158), not a type guarantee, and a blank-summary fallback is retained for old-Dragon interop. **Resolution: the ONE structural guarantee is the dispatch-table duplicate-key host assertion; everything else is a runtime-guarded convention, stated honestly. The blank-summary inference is a permanent documented fallback.**

Also corrected: **S2-12 silent-truncation is partly stale** — Dragon already emits a latched buffer-full transient (pipeline.py ~465/489) and clears the segment buffer per VAD segment, so the cap bites only the un-segmented tail. Cap work shrinks accordingly (map the existing transient + a typed `dictation_truncated` for the genuine tail-overflow + doc reconciliation), not a buffer rewrite.

### Decisions the human must explicitly approve (2):

- **D1 — Persistence owner = Dragon `NotesService`/`notes/db.py` (`~/tinkerclaw/notes/notes.db`); do NOT merge the two SQLite stores.** It is the store the WS path already writes and the dashboard reads; the FK-bearing `db_notes.py` table is dead for dictation. We add a nullable `turn_id` column, no destructive migration. *Sign-off needed because it declares the foundation `notes` table out-of-scope for dictation forever.*
- **D2 — Cap policy = two-tier and visible, NOT unified to one number.** Tab5 records ≤4 hr; Dragon transcribes per-segment with a 5-min per-segment ceiling; genuine tail-overflow emits typed `dictation_truncated` → SAVED-with-warning. Raising Dragon's `MAX_AUDIO_BUFFER` to 4 hr (~460 MB) is memory-unsafe on the Q6A. *Sign-off needed because the alternative (a single Dragon-negotiated `dictation_max_ms` lowering Tab5's recording cap) changes power-user behavior.*

All other open decisions are resolved inline with rationale.

---

## The authoritative state model

### dict_event_t (additive — preserves field offsets for the by-name `/dictation_pipeline` contract and existing host tests)

```c
/* voice_dictation.h */
typedef enum {
   DICT_IDLE = 0, DICT_RECORDING, DICT_UPLOADING, DICT_TRANSCRIBING,
   DICT_SAVED, DICT_FAILED,
   DICT_CANCELLED,                 /* W1: first-class terminal, not a fail reason */
} dict_state_t;

typedef enum {
   DICT_FAIL_NONE = 0, DICT_FAIL_AUTH, DICT_FAIL_NETWORK,
   DICT_FAIL_EMPTY, DICT_FAIL_NO_AUDIO, DICT_FAIL_TOO_LONG,
   /* DICT_FAIL_CANCELLED retained for ABI through W4; never SET after W1
    * (cancel now uses the DICT_CANCELLED *state*, reason=DICT_FAIL_NONE).
    * Removed in W5. */
   DICT_FAIL_CANCELLED,
} dict_fail_t;

typedef enum { DICT_ORIGIN_NONE = 0, DICT_ORIGIN_WS, DICT_ORIGIN_OFFLINE } dict_origin_t;

#define DICT_TURN_ID_LEN 13        /* MUST equal voice.c TURN_ID_LEN (13); see note below */

typedef struct {
   dict_state_t  state;
   dict_fail_t   fail_reason;      /* meaningful only when state==DICT_FAILED */
   uint32_t      started_ms, stopped_ms, last_change_ms;
   int           note_slot;        /* offline-only; -1 sentinel (existing field) */
   /* --- W1 --- */
   dict_origin_t origin;
   char          turn_id[DICT_TURN_ID_LEN];
   bool          resolution_pending; /* true while awaiting a resolution that has not
                                        yet landed; gates self-decay. Per-origin meaning
                                        defined in §self-liveness. */
   /* --- W4 --- */
   char          note_id[DICT_NOTE_ID_LEN]; /* Dragon note id once known; "" otherwise */
} dict_event_t;
```

**`DICT_NOTE_ID_LEN` (critic type-consistency fix):** Dragon `NotesService.create_from_text` returns `note.id`. We do NOT assume it fits in 40 bytes. **W4 task: pin the id format.** Dragon note ids are short slugs/integers in `notes/db.py`; we set `#define DICT_NOTE_ID_LEN 40` AND add a Dragon-side assertion + a host-test fixture using the longest real id. If any real id exceeds 39 chars, bump the macro before W4 lands. The buffer is a verified bound, not an assumption.

**Struct-by-value cost:** grows ~28→~80 bytes; `voice_dictation_get()` returns by value and the LVGL marshaller `malloc`s `sizeof(dict_event_t)` per dispatch (voice_dictation_lvgl.c:22). **No new per-dispatch alloc is added** — the marshaller already copies the whole struct. Noted, not blocking.

### Transitions (new table replacing voice_dictation.c:111-137)

```
IDLE        → RECORDING (WS) | UPLOADING (offline, via try_begin_offline only)
RECORDING   → UPLOADING | TRANSCRIBING | FAILED | CANCELLED
UPLOADING   → TRANSCRIBING | SAVED | FAILED | CANCELLED
TRANSCRIBING→ SAVED | FAILED | CANCELLED
SAVED       → IDLE                                   (self-decay only)
FAILED      → IDLE | SAVED | UPLOADING | RECORDING   (decay→IDLE; SAVED=late-correction; UPLOADING/RECORDING=retry)
CANCELLED   → IDLE                                   (self-decay only)
```

Each change tied to a finding:
- **Remove `SAVED→RECORDING`** (S3-5): self-decay reaches IDLE first; `test_saved_to_recording_allowed` is rewritten to `test_saved_to_recording_now_refused`.
- **Add `FAILED→SAVED`** (S2-2): a late `dictation_summary` corrects a false-FAIL. **W1 guard (critic seq-risk fix): gated on `resolution_pending && origin==DICT_ORIGIN_WS`, so a stale summary cannot resurrect an unrelated FAILED. The turn_id-match guard becomes authoritative in W2.**
- **Add `UPLOADING→SAVED`** and **`IDLE→UPLOADING` (offline only)**: the offline REST queue has no RECORDING phase (the WAV already exists) and resolves straight from UPLOADING (S1-3).
- **`DICT_CANCELLED` is terminal** (S2-7), self-decays, renders neutral grey — not red "TAP TO RETRY".

The guard remains a runtime log-and-noop (voice_dictation.c:149-158). **We do not claim it makes illegal states unrepresentable** — it refuses-and-logs. The only compile/host-time structural guarantee is the dispatch-table duplicate-key assertion (§wire contract).

### turn_id identity — minted inside the FSM, ONE generator, ONE source of truth

**Critic type-consistency fix (split-brain elimination).** Today voice.c owns `static char s_current_turn_id[TURN_ID_LEN]` (voice.c:85), `gen_turn_id()` (voice.c:87) regenerates it into the static, and `voice_current_turn_id()` (voice.c:94) reads it. The FSM must not mint a *second* id. Reconciliation:

- The **generator** is extracted to `void voice_turn_id_gen(char out[DICT_TURN_ID_LEN])` (pure: writes 12 hex + NUL, no globals). `gen_turn_id()` in voice.c is rewritten to call it into `s_current_turn_id`, preserving the existing `tab5_debug_obs_event("turn.start", ...)` side-effect.
- The **single source of truth for a dictation turn's id is the FSM** (`voice_dictation_get().turn_id`). `voice_dictation_begin()` mints via `voice_turn_id_gen` AND copies the same string into `s_current_turn_id` (so `voice_current_turn_id()` and all wire frames stay consistent — no divergence). Non-dictation turns (ask/call/solo) keep using `gen_turn_id()` exactly as today; they do not touch the FSM. There is exactly one id per turn, written in one place.
- `DICT_TURN_ID_LEN` and voice.c's `TURN_ID_LEN` are the same concept. To avoid two macros: voice.c includes voice_dictation.h and `#define TURN_ID_LEN DICT_TURN_ID_LEN` (or vice-versa); a `_Static_assert(DICT_TURN_ID_LEN == 13, ...)` guards the width. One value, one assertion.

```c
/* Mints session identity AND transitions to RECORDING in one locked op (WS path).
 * Also writes the minted id into voice.c's s_current_turn_id via the shared generator,
 * so voice_current_turn_id() and the start/segment/stop/cancel frames all agree.
 * Returns the minted id (points into FSM state, valid until next IDLE). */
const char *voice_dictation_begin(dict_origin_t origin, const char *adopt_turn_id, uint32_t now_ms);

/* Atomic claim for the offline queue. Reads FSM state and, ONLY if IDLE/terminal,
 * transitions IDLE->UPLOADING with the adopted turn_id, under a single lock take.
 * Returns false (no state change) if a turn is live. Compare-and-begin primitive
 * that closes the queue<->decay race. */
bool voice_dictation_try_begin_offline(const char *adopt_turn_id, int note_slot, uint32_t now_ms);

/* Apply a resolution/terminal transition ONLY if turn_id matches the live turn.
 * Generalizes the EXISTING llm-handler late-arrival guard (voice_ws_proto.c:688,
 * which today guards by voice_state — there is NO turn_id guard yet). A MISSING/empty
 * turn_id field is treated as MATCH (forward-compat with old Dragon). */
bool voice_dictation_resolve_if_current(const char *turn_id, dict_state_t st,
                                        dict_fail_t reason, uint32_t now_ms);
```

The `start` frame (voice.c:1766/1879) already sends `s_current_turn_id`; after W1 that value is FSM-minted for dictation. Tab5 adds `turn_id` to `segment`/`stop`/`cancel` (W2). Dragon echoes it on every dictation response (W2).

### Self-liveness — decay WITHOUT dispatching from the timer task

```c
/* Module-level one-shot esp_timer. Host: driven by a fake clock — see below. */
#define DICT_DECAY_SAVED_MS     2000
#define DICT_DECAY_FAILED_MS    5000
#define DICT_DECAY_CANCELLED_MS 1500

/* Single shared monotonic-ms accessor. On target: esp_timer_get_time()/1000.
 * Used by decay arming, resolve_if_current callers, and (W5) the ~25 inlined
 * (uint32_t)(esp_timer_get_time()/1000) sites. Under host tests it reads a
 * settable test global g_dict_fake_now_ms (the "fake clock"); the host esp_timer
 * shim and voice_dictation_now_ms() both read THE SAME global, so advancing the
 * clock in a test deterministically fires the (synchronously-pumped) decay job. */
uint32_t voice_dictation_now_ms(void);

static void dict_decay_timer_cb(void *arg) {            /* esp_timer task */
   /* NO dispatch here. Enqueue onto the shared worker. */
   esp_err_t e = tab5_worker_enqueue(dict_decay_apply_job, NULL, "dict_decay");
   if (e != ESP_OK) {
      /* Critic fix: full-queue must NOT re-introduce the headless-block defect.
       * On enqueue failure, re-arm the one-shot for DICT_DECAY_RETRY_MS (250 ms)
       * so the terminal still decays once the queue drains. arg is NULL so there
       * is nothing to free (worker never frees arg per task_worker.h). */
      esp_timer_start_once(s_dict_decay_timer, DICT_DECAY_RETRY_MS * 1000);
   }
}
static void dict_decay_apply_job(void *arg) {           /* task_worker context */
   dict_event_t e = voice_dictation_get();
   if (DICT_IS_TERMINAL(e.state) && !e.resolution_pending)
      voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE, voice_dictation_now_ms());
}
```

This is the keystone fix for the #1 crash vector. Liveness no longer depends on any subscriber being mounted (closes S2-1 and the headless-block defect), no new task enters `dict_dispatch_locked`, and a full worker queue degrades to a 250 ms retry rather than a permanent stuck terminal. On target the decay deletes `saved_fade_to_idle_cb` (ui_orb.c:2459) and ui_home's auto-fade.

**`resolution_pending` gate (data-loss fix) — per-origin semantics (critic type-consistency fix):**
- **WS turns (`origin==DICT_ORIGIN_WS`):** set `true` when Tab5 sends `stop` and is awaiting a Dragon terminal frame; cleared when a terminal frame (any `dictation_*` terminal) resolves the turn. While `true`, SAVED/FAILED decay does NOT arm — guaranteeing a 60–90s-late summary lands.
- **Offline turns (`origin==DICT_ORIGIN_OFFLINE`):** there is NO Dragon WS terminal — the REST response IS the resolution. `resolution_pending` is set `true` at `try_begin_offline` (entering UPLOADING) and cleared by the offline queue task the instant it applies the SAVED/FAILED terminal from the HTTP response. So offline terminals decay normally; the field's meaning is "a resolution is in flight on this turn's transport," uniform across origins, with the transport differing by origin. This per-origin branch is documented, not implicit.

`DICT_DECAY_RETRY_MS` (250) and `s_dict_decay_timer` are added in W1.

### voice_state reconciliation — one finalize chokepoint

```c
/* voice.c — the ONLY site that resolves a dictation across BOTH state machines.
 * Idempotent, turn_id-guarded. Stops mic, snaps voice_state coherently
 * (READY if WS up, IDLE if down), drives the dict FSM terminal. */
void voice_dictation_finalize(dict_state_t terminal, dict_fail_t reason, const char *turn_id);
```

Called from exactly: (1) `voice_cancel()` (voice.c:2133) when mode is dictate, with `terminal=DICT_CANCELLED, reason=DICT_FAIL_NONE` (closes S1-5/S2-7); (2) the WS terminal handlers; (3) the 4hr cap and offline finalise.

**Critic type-consistency + ordering fixes:**
- **Which arg is CANCELLED:** `DICT_CANCELLED` is the *state* (1st arg `terminal`); `reason` (2nd arg) is `DICT_FAIL_NONE`. The prose elsewhere saying "with DICT_CANCELLED" means the terminal arg. `DICT_FAIL_CANCELLED` is never passed.
- **Cancel-path turn_id source:** `voice_cancel()` passes `voice_dictation_get().turn_id` (the FSM's live id — authoritative for dictation), NOT `voice_current_turn_id()`. A cancel with an EMPTY FSM turn_id (no live dictation) is a no-op for the FSM (correct). A user-initiated cancel is intent on the *current* turn by construction, so the guard always matches; cancel is never dropped by the guard.
- **Ordering vs existing voice_cancel internals:** `voice_dictation_finalize` runs **after** `voice_cancel`'s existing `voice_set_state`/`s_conv_active`/`VMODE_SOLO_DIRECT` logic, as the last step, so finalize has the final word on voice_state (snaps READY/IDLE) and the dict FSM. Documented call-order, not left to chance.

**Honest scoping:** this unifies the FSM↔voice_state boundary *for dictation only*. The ~25 non-dictation voice_state sync sites (ask/call/solo/tts) are out of scope; unifying the global voice_state is a separate effort. Dictation-internal coherence becomes structural at three chokepoints; cross-feature coherence remains convention.

### Two-producer reconciliation — one FSM, origin-tagged, atomic claim, symmetric refusal

One FSM (mic, SD, UI surfaces are singletons; two FSMs would still need mutual exclusion). `origin` tags ownership; `note_slot` is offline-only, `note_id` is the WS/offline note anchor — they never share a clobbered field (root of S1-3). The offline `transcription_queue_task` (ui_notes.c:1609) is rewired: it stops gating on `voice_get_state()` (line 1620) and calls `voice_dictation_try_begin_offline()` — the atomic compare-and-begin — **skipping this 15s tick if it returns false**.

**Critic seq-risk fix — the reverse race.** The offline WAV read + HTTP POST run for many seconds OUTSIDE the lock, in UPLOADING. If a WS dictation begins in that window, `voice_dictation_begin(DICT_ORIGIN_WS, ...)` **must refuse** while `origin==DICT_ORIGIN_OFFLINE && state==UPLOADING`. So `voice_dictation_begin` returns `NULL` (no transition) when a turn of the other origin is live; the caller (`voice_start_dictation`) then toasts "Busy transcribing — try again" (reusing the existing mic-busy toast path from TT #621 Wave A R7). Both directions of the producer collision are now closed: offline-claims-during-WS via `try_begin_offline` returning false, WS-claims-during-offline via `begin` returning NULL.

---

## Resolution-event + wire contract

### Typed terminal frames (Dragon → Tab5, WS path) — exactly one per turn_id

| Frame `type` | Fields | FSM result | Replaces |
|---|---|---|---|
| `dictation_summary` | `turn_id, title, summary, proposed_action?, note_id?` | `SAVED` | non-empty summary |
| `dictation_empty` (NEW) | `turn_id, reason:"silence"\|"too_short"` | `FAILED/EMPTY` | blank-string summary (S3-1) |
| `dictation_truncated` (NEW) | `turn_id, title, summary, note_id, dropped_ms` | `SAVED` + truncated caption/toast | silent tail truncation (S2-12) |
| `dictation_postprocessing_error` | `turn_id, note_id?` | `SAVED` (note exists) | existing (W0) |
| `dictation_postprocessing_cancelled` | `turn_id` | `CANCELLED` | existing |
| `dictation_warning` (NEW Tab5 handler) | `turn_id, code, message` | no terminal; toast only | unmapped on Tab5 (S2-3) |
| `note_created` | `turn_id, note_id, title` | attach note_id to FSM + upsert local row | log-only (S1-6) |

`dictation_postprocessing` (still-working) → keep-alive, clears nothing.

**Critic uncovered-finding fix — `dictation_warning` is NOT new on Dragon; it needs a discriminator.** Dragon already emits `dictation_warning` today (pipeline.py:741, the finish-tail-drop). W3 ALSO emits one on per-segment timeout. Two distinct semantics under one type would be indistinguishable. **Resolution: `dictation_warning` carries a required `code` field** — `code:"tail_dropped"` (existing finish-tail-drop) vs `code:"segment_timeout"` (new) vs `code:"stt_fallback"` (cloud→local degrade). Tab5's `rx_dict_warning` renders a `code`-specific toast string via a small map; unknown codes render the raw `message`. The taxonomy is explicit, not collapsed into one opaque string.

**Critic type-consistency fix — buffer-full frame shape.** Dragon does NOT emit a top-level `type:"buffer_full"`. It emits a transient/error event whose `type` is the generic transient type and whose **discriminator is a `code` field**: `code:"dictation_buffer_full"` (pipeline.py:465) and `code:"audio_buffer_full"` (pipeline.py:489). Therefore the dispatch table keys on the existing transient `type` (already handled by the W0 transient branch), and the *truncated-tail* mapping is done **inside that transient handler by inspecting `code`** — NOT via a `"buffer_full"` table key. Concretely: the transient handler, on `code∈{audio_buffer_full,dictation_buffer_full}` during an active dictation turn, sets a Tab5-side "truncation seen" flag; the genuine tail-overflow terminal is the NEW typed `dictation_truncated` frame (which DOES have its own `type` and table entry). The table-key target is consistent with the real frame shape.

**Exactly-one-terminal post-condition (server-side):** Dragon's `finish_dictation`/`_post_process_dictation` set a per-turn `_terminal_emitted_for: set[turn_id]` latch; a second terminal emit logs ERROR and is dropped. Belt to the FSM's suspenders.

**Backward compat (no lockstep deploy):** Dragon keeps emitting `dictation_summary` for the happy path. New frames are additive; an old Tab5 ignores unknown `type`. A new Tab5 that sees a blank `dictation_summary` from an old Dragon resolves via a **permanent** fallback branch — `if (title[0]||summary[0]) SAVED else FAILED/EMPTY`. This blank-string inference path survives as the old-Dragon interop floor; the typed path is primary. Missing `turn_id` = match.

### Dispatch-table RX (the one structural guarantee)

Convert `voice_ws_proto_handle_text` (voice_ws_proto.c:550, 27 linear `else if` branches) into a static table in a new `main/voice_ws_rx_table.{c,h}` (mirrors the debug_server_*.c family pattern):

```c
typedef void (*ws_rx_fn)(cJSON *root, const char *turn_id);
typedef struct { const char *type; ws_rx_fn fn; } ws_rx_entry_t;
static const ws_rx_entry_t WS_RX_TABLE[] = {
   {"stt_partial", rx_stt_partial}, {"stt", rx_stt}, {"llm", rx_llm}, /* ... */
   {"dictation_summary", rx_dict_summary}, {"dictation_empty", rx_dict_empty},
   {"dictation_truncated", rx_dict_truncated}, {"dictation_warning", rx_dict_warning},
   {"dictation_postprocessing", rx_dict_pp}, {"dictation_postprocessing_error", rx_dict_pp_error},
   {"dictation_postprocessing_cancelled", rx_dict_pp_cancelled}, {"note_created", rx_note_created},
   /* ...all existing ask/tts/error/transient/channel/widget types... */
};
```

Each `rx_*` is lifted **verbatim** from its current branch (extract-only PR first, per "extract before decompose"; logic changes ride separate commits). Terminals call `voice_dictation_resolve_if_current(turn_id,...)`. The existing transient/error branch (which inspects `code`) moves in as one `rx_transient` entry. Unknown `type` → `rx_unhandled` logs once. **Host test `test_ws_rx_table.c` asserts (a) no duplicate `type` key and (b) every dictation type in protocol.md has an entry** — making the S1-2 duplicate-handler class (the W0 bug) a host-test failure, not a production wedge.

### Legacy/progress double-write (S3-8) — decision + scoping

Tab5 stays on the typed legacy frames (now turn_id-stamped); Dragon keeps `progress_bus_emit_legacy=True`. **No `progress` handler is added to Tab5.** protocol.md marks typed legacy frames as the **normative** Tab5 contract, `progress` as observability-only.

**Critic uncovered-finding fix — W5 progress cleanup is precisely scoped, NOT "collapse dead branches."** `dictation_post.py` routes EVERY outcome (success / no-LLM / error) through one helper `emit_progress_pair(..., emit_legacy=)`, which emits BOTH the legacy frame Tab5 consumes AND the progress frame. There are no separable "progress-only" branches to delete. **W5 task, restated honestly:** (a) keep `emit_progress_pair` and the legacy emission it produces — that is load-bearing for Tab5; (b) the ONLY safe removal is the `progress`-frame half *iff* no consumer exists fleet-wide — but the dashboard MAY consume it, so W5 first greps consumers and, if any exist, does NOTHING but a doc note marking the dual-write intentional. The wave does not touch `emit_progress_pair`'s legacy path. Net: S3-8 becomes a documentation + consumer-audit task, with code change gated on proving the progress half is truly dead. No ambiguous "collapse."

### protocol.md deltas
§4: `turn_id` required + echoed on all dictation frames; document two-tier cap (D2). New §4.x resolution-events section: the 7 typed frames, each `turn_id` REQUIRED, the exactly-one-terminal invariant, the permanent blank-summary fallback, the `dictation_warning.code` taxonomy, and the buffer-full `code`-on-transient shape. §11: `dictation_empty`/`dictation_truncated`/`dictation_warning`; `note_created.turn_id`+`note_id` as the dedup anchor; **the REST `/api/v1/transcribe` response now returns `note_id`+`turn_id`** (see persistence). `/dictation_pipeline` (debug_server_dictation.c:73) gains `turn_id`, `origin`, `note_id`, `resolution_pending` — additive.

---

## Persistence + note model

**Store (D1):** Dragon `NotesService`/`notes/db.py` (`~/tinkerclaw/notes/notes.db`) is the single source of truth. Add a nullable, indexed `turn_id TEXT` column (W4 `ALTER TABLE`). The foundation `db_notes.py` `notes` table is dead for dictation, untouched. No store merge.

**Critic missing-interface fix — `turn_id` reaches the Dragon note row.** `stop_handler.py:157` calls `notes_svc.create_from_text(stripped, title="")`, which takes no `turn_id`. **W4 adds a `turn_id: str | None = None` kwarg to `NotesService.create_from_text` (and the underlying `notes/db.py` insert), written into the new column.** On the WS path, `turn_id` is read from `conn_state["turn_id"]` (the value W2 already echoes). On the REST path, see below.

**Critic uncovered-finding + seq-risk fix — the offline/REST path crosses note_id back to Tab5 over HTTP, not WS.** The offline transcription path is `api/synthesize.py` (`transcribe_audio` at :95, `retranscribe` at :195) — a REST handler with NO voice WS connection. A `note_created` *frame* cannot reach Tab5 here. The fully-specified mechanism:
1. **Tab5 sends turn_id IN the REST POST body.** The offline queue's POST to `/api/v1/transcribe` includes the `turn_id` minted by `voice_dictation_try_begin_offline` (W1 mints it; **W4 threads it into the POST body and W4 also adds it to the Tab5 offline-queue request builder** — this is the explicit cross-wave carrier the critic flagged as missing: W1 mints, W4 wires it onto REST. WS-only wiring in W2 does not cover REST, by design).
2. **Dragon's REST handler creates the note** via `create_from_text(..., turn_id=<body turn_id>)` and **returns `note_id` + `turn_id` in the HTTP JSON response body** (additive fields; old Tab5 ignores them). It does NOT emit a WS `note_created` — there is no socket. The empty/short case returns `{"empty": true, "reason": ...}` in the response body.
3. **Tab5's offline queue consumes the response body** (it already parses this HTTP response at ui_notes.c ~1697), reads `note_id`/`turn_id`/`empty`, and applies the terminal locally: `voice_dictation_resolve_if_current(turn_id, SAVED|FAILED/EMPTY, ...)` + upsert the local row's `note_id`. So the offline path resolves entirely via HTTP response + the FSM, never via the WS table.

The dedup story is therefore complete for the path that needs it most: every offline note carries the same `turn_id` Tab5 minted, so it dedups against the local anchor row by key.

**Dedup (note_id + turn_id), order-independent, never-orphan:**
1. WS path: Dragon creates the note once and emits `note_created{turn_id, note_id, title}` (stop_handler.py:163).
2. REST path: Dragon creates the note and returns `note_id`+`turn_id` in the HTTP response (above).
3. The empty/short path emits `dictation_empty` (WS) or `{empty:true}` (REST) — never a silent skip.
4. Tab5 keeps a **turn_id-keyed local row as the always-present anchor**, written at start (RECORDING entry for WS, UPLOADING entry for offline). `rx_note_created` and `rx_dict_summary` (and the REST response handler) all **upsert by turn_id** (order-independent); `note_id` attaches when it arrives. A missing `note_created` degrades to "unlinked local note" (renders local-only, identical to today), never to "no note". The old duplicate-maker `ui_notes_add_dictated_async` (ui_notes.c:1071) is removed from the resolution handlers — the upsert replaces it. `note_entry_t` (ui_notes.c:188) gains `turn_id`/`note_id` (JSON `"tid"`/`"nid"`).

**Audio linkage (enables WS-path retry, S1-8):** WS dictation arms a local SD WAV at RECORDING entry keyed `/sdcard/rec/<turn_id>.wav` (the offline path already writes `/sdcard/rec/NNNN.wav` via `ui_notes_pipeline_arm_recording`; extend to the orb/long-press WS path). On a WS FAILED terminal, the local row is written `FAILED` with `audio_path` populated → the Notes Retry button (gated on `audio_path[0]`) lights up and the offline queue can re-transcribe. WS and offline failures converge on one durable, retryable row.

**SD-write concurrency (regression risk — explicit mitigation):** always-write SD-WAV during a live WS dictation runs SD write concurrent with WS uplink — a known SDIO/Wi-Fi DMA contention surface (TT #621 note). Mitigations: (a) the WAV write is the same path the 4hr-cap already exercises; (b) **lands in W4; a 10-minute concurrent SD+WS soak is a W4 gate**; (c) the grace-timer deletion (W3) does NOT depend on it. If the soak regresses, revert to discard-on-WS-success (lose only WS-retry; rest stands).

**Migration safety:** `note_entry_t` JSON persistence (`/sdcard/notes.json`) MUST be **key-mapped, not index-mapped** — W4 verifies the (de)serializer reads by key and populates empty strings for absent `tid`/`nid` on OLD rows (the post-OTA upgrade path with 4hr-old queued recordings). The existing serializer already uses keyed fields (`"a"` for audio_path at ui_notes.c:527), so this is a verification + a host test, not a rewrite. A host test loads a pre-W4 notes.json fixture and asserts no field shift. Dragon `turn_id` column nullable → old notes never dedup, harmless.

---

## UI + retry model

**Pure renderers:** after W1, orb (ui_orb.c:2491), Dictate chip (ui_home.c), Notes proc-row (ui_notes.c:3120) hold zero liveness logic. Delete `saved_fade_to_idle_cb`+timer (ui_orb.c:2459), ui_home's FAILED/SAVED→IDLE block (ui_home.c:1938), and the pre-Ask FSM reset in `ui_home_start_voice_turn`. Each renders `dict_event_t` including the FSM's own IDLE decay and `DICT_CANCELLED` (neutral grey, "Cancelled", no retry affordance). Add the 4th subscriber slot for the offline surface (dynamic table in W5 removes the ceiling).

**One retry verb (S1-7):** new `main/dictation_notes.c` (the extracted module):

```c
void ui_dictation_retry(void) {
   dict_event_t e = voice_dictation_get();
   /* Retry is valid from FAILED, OR from IDLE when the row still carries a
      retryable WAV (the common case: FAILED self-decayed in ~5s before the tap). */
   if (e.state != DICT_FAILED && e.state != DICT_IDLE) return;
   const char *tid = (e.state == DICT_FAILED) ? e.turn_id : ui_notes_last_failed_turn_id();
   if (tid && audio_path_for_turn(tid)[0])
      ui_notes_requeue_by_turn(tid);   /* offline re-transcribe of captured WAV */
   else
      voice_start_dictation();         /* nothing recoverable -> fresh recording */
}
```

**Critic missing-interface fix — define `ui_notes_requeue_by_turn` / `audio_path_for_turn` and how they map onto the existing slot-keyed queue.** The existing queue scans `note_entry_t` slots in `NOTE_STATE_RECORDED` with non-empty `audio_path` (ui_notes.c:1638-1640), keyed by `slot`. The new helpers bridge turn_id→slot:
- `const char *audio_path_for_turn(const char *turn_id)` — linear scan of `s_notes[]` for the row whose `turn_id` matches; returns its `audio_path` or `""`.
- `void ui_notes_requeue_by_turn(const char *turn_id)` — find that row, set its `state = NOTE_STATE_RECORDED` (the state the existing `transcription_queue_task` already picks up), and let the existing queue do the work. No new queue; it re-enters the existing slot-scan path. If `audio_path` is empty it is a no-op (the FALLBACK branch in `ui_dictation_retry` handles "nothing recoverable").
- `const char *ui_notes_last_failed_turn_id(void)` — returns the turn_id of the most-recent FAILED row with a WAV (used when FAILED already decayed to IDLE before the user taps).

All three FAILED-tap surfaces bind to `ui_dictation_retry`. The orb FAILED-tap stops routing to an Ask turn; the chip stops blindly re-recording; the proc-row close stops dismissing. A separate dismiss gesture (long-press/swipe) maps to `voice_dictation_set_state(IDLE)`. **S3-4 honesty:** because FAILED self-decays in ~5s, the common retry is `IDLE→UPLOADING` via the row; `FAILED→UPLOADING` is real-but-uncommon (fast tap), exercised by a host test, documented as such, not claimed as the primary path.

**One reason caption (S2-11):** `const char *voice_dictation_fail_caption(dict_fail_t)` in voice_dictation.c (next to `voice_dictation_fail_name`) replaces the 4 divergent tables (ui_orb.c:2421 — which still says the stale "TOO LONG (5 min cap)"; chip; proc-row "FAILED TAP TO RETRY"; notes `note_fail_t`). The "TAP TO RETRY"/"Cancelled"/"capped, saved" suffix is appended by the renderer per state, not baked into the caption. Pure function, host-testable.

---

## Dragon STT robustness

**Critic type-consistency fix — reconcile the two overlapping fallback helpers and the existing timeout.** Ground truth: `_post_process_dictation` (the LLM-summary path) ALREADY wraps its transcribe in `asyncio.wait_for` (pipeline.py:893) and ALREADY calls `_ensure_fallback_stt_then_transcribe(audio_data)` (pipeline.py:915, no `timeout` kwarg). The gap (S2-4/S2-5) is real ONLY for the **STT segment path**: `process_segment` (pipeline.py:663/678) and `finish_dictation`'s tail transcribe (pipeline.py:705-743) have neither timeout nor fallback. Resolution that does NOT create a third redundant helper:

1. **One fallback helper survives.** Add an optional `timeout: float | None = None` kwarg to the EXISTING `_ensure_fallback_stt_then_transcribe` rather than introducing a new `_transcribe_with_fallback`. When `timeout` is set it wraps its own transcribe in `asyncio.wait_for`. The post-processing path keeps calling it (now optionally passing the timeout); `process_segment` and `finish_dictation` start calling it WITH `timeout=DICTATION_STT_TIMEOUT_S`. One helper, two call-sites gain timeout+fallback, the third is unchanged. No duplicate fallback logic.
2. **Per-segment + finish timeout (S2-4):** `DICTATION_STT_TIMEOUT_S = 30` (longer than ask-mode's 10s; segments are long). Segment timeout → `dictation_warning{code:"segment_timeout"}` + drop that segment, continue. Finish timeout → join what landed, emit `stt`, proceed to a terminal. This guarantees `finish_dictation` always reaches a terminal — which is *why* W3 can delete the 45s grace timer.
3. **Cloud→local fallback on dictation (S2-5):** via the same helper — on `stt.backend=="openrouter"` failure/timeout → local Moonshine (pre-warmed via `swap_backends`) + `dictation_warning{code:"stt_fallback"}`. Per-segment, so a cloud blip degrades that segment, not the dictation.
4. **Cap reconciliation (S2-12) — scoped to reality:** the segment-buffer cap already emits a latched transient (`code:"dictation_buffer_full"`/`"audio_buffer_full"`) and clears per VAD segment; the cap bites only a no-pause monologue tail. Work: (a) **map** the existing transient on Tab5 inside `rx_transient` via its `code` (it was only toasted); (b) on a genuine tail-overflow, set `_dictation_truncated` and emit the typed **`dictation_truncated`** terminal → Tab5 renders SAVED + "capped at N min, saved"; (c) **doc reconciliation** in protocol.md. No buffer rewrite. (D2.)

**Critic seq-risk fix — old-Dragon + grace-timer deletion needs a replacement watchdog.** W3 deletes the 45s grace timer (voice_ws_proto.c:61-92) because the new Dragon always terminates. But a new-Tab5 / old-Dragon pairing (no terminal guarantee) would then hang in TRANSCRIBING forever — the exact band-aid removed. Risk-6 promised a "stuck-watchdog" but no wave built it. **Resolution: W3 does not delete the grace timer outright; it RELOCATES the liveness floor into the FSM.** The grace timer's job — "if no terminal within N seconds of `stop`, force a terminal" — becomes a property of `resolution_pending`: when a WS turn enters `resolution_pending`, the self-decay machinery also arms a `DICT_STUCK_MS = 60000` one-shot; if it fires while still `resolution_pending`, it enqueues (same `tab5_worker_enqueue` path) a `voice_dictation_finalize(DICT_FAILED, DICT_FAIL_NETWORK, turn_id)`. So the watchdog is FSM-owned, bounded to a 60s-late FAILED (never a permanent hang), and works against any Dragon. The grace timer in voice_ws_proto.c is deleted; its responsibility is not — it moves into the FSM where identity + decay already live. This is built in W3 (it is the same enqueue plumbing W1 established), closing the gap that no wave built the watchdog.

5. **Frame mapping:** Tab5-side covered by the dispatch table + `rx_transient` (code inspection) + `rx_unhandled`; Dragon ensures buffer-full transients carry `turn_id` + structured `code`.

S3-7 (loads streaming Moonshine, batch-transcribes per segment) is **W5, doc-only-or-defer**: streaming was loaded for the live `stt_partial` waveform UX; switching to batch-load risks regressing it. W5 documents the mismatch and defers the model-load change unless free.

---

## Wave plan W1–W5

Every remaining S1/S2/S3 finding is assigned to exactly one wave. W0 items (S1-1, S1-2, S1-4, S3-2, S3-3, S3-6) are done. C tests are host tests; Dragon tests are pytest. Each wave leaves both repos green.

### W1 — FSM owns identity + liveness + atomic two-producer claim
**Goal:** identity born in the FSM (one generator, no split-brain); terminal self-decay via `task_worker` (never the timer task, full-queue-safe); CANCELLED terminal; offline-queue atomic claim with symmetric WS refusal. Identity ships WITH decay (closes the data-loss window). No wire/persistence change.
**Files/functions:** `voice_dictation.{c,h}` — add `origin`/`turn_id`/`resolution_pending`/`note_id` fields, `DICT_CANCELLED`, `DICT_NOTE_ID_LEN`, `_Static_assert` on turn-id width, `voice_dictation_begin` (returns NULL on cross-origin busy), `voice_dictation_try_begin_offline`, `voice_dictation_resolve_if_current`, `voice_dictation_now_ms`, `voice_dictation_fail_caption`; rewrite transition table (remove `SAVED→RECORDING`, add `FAILED→SAVED`[guarded `resolution_pending && origin==WS` in W1]/`UPLOADING→SAVED`/`IDLE→UPLOADING`-offline, CANCELLED edges); esp_timer self-decay via `tab5_worker_enqueue` + `DICT_DECAY_RETRY_MS` re-arm on `ESP_ERR_NO_MEM`; per-origin `resolution_pending`. `voice.c` — extract `voice_turn_id_gen(char out[13])`, rewrite `gen_turn_id` to use it, define `TURN_ID_LEN`↔`DICT_TURN_ID_LEN`; `voice_start_dictation` → `voice_dictation_begin(WS,...)` (toast on NULL) + send FSM turn_id on `start`. `ui_notes.c:1609` — offline queue uses `voice_dictation_try_begin_offline` + skip-if-false. `ui_orb.c`/`ui_home.c` — delete `saved_fade_to_idle_cb` + auto-fade + pre-Ask reset; add CANCELLED render. `tests/host/shim/esp_timer.h` (fake clock sharing `g_dict_fake_now_ms`); `tests/host/shim/task_worker.{c,h}` stub.
**Host test stub semantics (critic missing-interface fix):** the host `task_worker` stub enqueues jobs into an in-process FIFO and runs them ONLY when the test calls `tab5_worker_pump()` (synchronous, deterministic). `test_try_begin_offline_atomic_vs_decay` exercises the interleave by: arm decay → advance `g_dict_fake_now_ms` past `DICT_DECAY_SAVED_MS` (timer cb enqueues the decay job) → call `try_begin_offline` BEFORE `tab5_worker_pump()` → assert the claim wins/loses deterministically per the lock, then pump and assert no double-apply. The stub never runs jobs on a background thread, so interleavings are test-controlled.
**Tests:** host — `test_saved_decays_to_idle`, `test_failed_decays_to_idle`, `test_decay_suppressed_while_resolution_pending`, `test_failed_to_saved_late_correction_ws_only`, `test_cancelled_is_terminal_not_failed`, `test_try_begin_offline_refused_when_live`, `test_ws_begin_refused_during_offline_upload`, `test_try_begin_offline_atomic_vs_decay`, `test_decay_requeues_on_full_worker`. Rewrite `test_saved_to_recording_allowed`→`test_saved_to_recording_now_refused`. On-device: dictate → SAVED → `/dictation_pipeline` shows IDLE ~2s later with NO UI mounted; offline queue drains 3 queued notes.
**Closes:** S1-3, S2-1, S2-7, S3-5. (S2-2 partial via guarded `FAILED→SAVED`; finalized W3. S2-8 explicitly NOT closed here — needs W2 echo.)

### W2 — centralized cancel/finalize + turn_id echo + start-side WS symmetry
**Goal:** one resolver; real stale-frame drop activated (turn_id now echoed both ways); cancel resolves FSM; start-side WS failure symmetric with stop-side; `FAILED→SAVED` cross-turn guard becomes authoritative.
**Files/functions:** `voice.c` — `voice_dictation_finalize` (called LAST in `voice_cancel`, turn_id from `voice_dictation_get().turn_id`, `DICT_CANCELLED`/`DICT_FAIL_NONE`); `voice_cancel` (2133) calls it for dictate mode (S1-5); start-side `voice_ws_send_text(start)` failure (1880) → `finalize(FAILED,NETWORK)` (S2-10); route 4hr cap (908) + offline finalise through it; send `turn_id` on `segment`/`stop`/`cancel`. `voice_ws_proto.c` — resolution handlers call `voice_dictation_resolve_if_current` (activates S2-8/S2-9 drop). `debug_server_dictation.c` — add `turn_id`/`origin`/`resolution_pending`/`note_id` to JSON. Dragon `pipeline.py`/`stop_handler.py`/`dictation_post.py`/`start_handler.py` — echo `turn_id` (from `conn_state["turn_id"]`) on `stt`/`stt_partial`/`dictation_*`/`note_created`. Remove the W1 `origin==WS` qualifier on `FAILED→SAVED` (turn_id match now guards it).
**Tests:** host — `test_finalize_cancels_inflight`, `test_finalize_called_last_voice_state_coherent`, `test_resolve_if_current_drops_stale`, `test_missing_turn_id_treated_as_match`, `test_back_to_back_distinct_ids`, `test_late_summary_for_wrong_turn_rejected`. pytest — `test_dictation_turn_id_echoed_on_all_frames`. On-device: cancel mid-dictation → grey "Cancelled" (not red); back-to-back A/B → A's late summary dropped, B unaffected.
**Closes:** S1-5, S2-8, S2-9, S2-10.

### W3 — typed events + dispatch table + Dragon STT robustness + cap reconciliation + FSM stuck-watchdog
**Goal:** typed terminal contract; table-driven RX with duplicate-key assertion; per-segment timeout + fallback (one reconciled helper); truncation visible; relocate the grace-timer's liveness floor into the FSM (delete the timer, keep the responsibility).
**Files/functions:** new `main/voice_ws_rx_table.{c,h}` — table + `rx_*` (extract-only move first) + `rx_transient` (inspects `code` for buffer-full→truncation flag) + `rx_unhandled`; add `rx_dict_empty`/`rx_dict_truncated`/`rx_dict_warning` (code-keyed toast map); resolve on type not blank-string (keep permanent blank fallback). `voice_ws_proto.c` — **delete grace timer (61-92)**; `voice_dictation.c` — arm `DICT_STUCK_MS` one-shot when a WS turn enters `resolution_pending`, firing `voice_dictation_finalize(FAILED,NETWORK,turn_id)` via `tab5_worker_enqueue` (the FSM-owned watchdog replacing the grace timer). `tests/host/test_ws_rx_table.c` — no-dup-key + every-dictation-type-mapped. Dragon `pipeline.py` — add `timeout` kwarg to the EXISTING `_ensure_fallback_stt_then_transcribe`; use it (timeout+fallback) in `process_segment`+`finish_dictation`; emit `dictation_empty` (not blank summary) at ~772; `_dictation_truncated` latch + `dictation_truncated` terminal; `dictation_warning.code` discriminator; `_terminal_emitted_for` set. `docs/protocol.md` §4/§4.x/§11.
**Tests:** host — table dispatch, each new handler, `rx_transient` code routing, blank-summary fallback still resolves, `test_stuck_watchdog_fails_after_timeout`, `test_stuck_watchdog_disarmed_on_terminal`. pytest — `test_segment_timeout_emits_warning_code`, `test_below_min_emits_empty`, `test_tail_overflow_emits_truncated`, `test_cloud_stt_falls_back_local`, `test_exactly_one_terminal_per_turn`, `test_fallback_helper_single_definition` (asserts no `_transcribe_with_fallback` was added). On-device: force cloud-STT failure → resolves via local; no-pause >5-min tail → `dictation_truncated` → SAVED + toast; kill Dragon after `stop` → FSM stuck-watchdog FAILs the turn at ~60s (no permanent hang).
**Closes:** S2-2 (final), S2-3, S2-4, S2-5, S2-12, S3-1.

### W4 — single persistence owner + dedup + audio linkage + durable retry
**Goal:** one note per turn keyed note_id+turn_id (never orphaned, WS AND REST); WS audio captured; one `ui_dictation_retry`; turn_id threaded onto the REST POST.
**Files/functions:** Dragon — `ALTER TABLE notes ADD COLUMN turn_id TEXT` + index; add `turn_id` kwarg to `NotesService.create_from_text` + the `notes/db.py` insert; `note_created{turn_id}` on WS auto-note (stop_handler.py:163); **`api/synthesize.py` (`transcribe_audio`:95, `retranscribe`:195) reads `turn_id` from the POST body, creates the note with it, and returns `note_id`+`turn_id`+`empty?` in the HTTP response**; `dictation_empty`/`{empty:true}` for short transcripts. Tab5 — `note_entry_t` + `tid`/`nid` (JSON, **key-mapped serializer verified**); local turn_id-keyed anchor row at start; `rx_note_created` + `rx_dict_summary` + the offline HTTP-response handler upsert-by-turn_id; **offline queue POST body now includes `turn_id`**; remove `ui_notes_add_dictated_async` from handlers; arm `/sdcard/rec/<turn_id>.wav` for WS dictation; WS FAILED writes durable row + audio_path; new `main/dictation_notes.c` with `ui_dictation_retry`/`ui_notes_requeue_by_turn`/`audio_path_for_turn`/`ui_notes_last_failed_turn_id`; bind orb/chip/proc-row FAILED-tap. Pin `DICT_NOTE_ID_LEN` against the longest real Dragon note id.
**Tests:** host — `test_upsert_by_turn_id_dedups`, `test_missing_note_created_degrades_to_local_only`, `test_retry_requeues_captured_wav`, `test_requeue_by_turn_maps_to_recorded_slot`, `test_notes_json_old_fixture_no_field_shift`, `test_note_id_fits_buffer`. pytest — `test_note_created_on_ws_path`, `test_rest_transcribe_returns_note_id_and_turn_id`, `test_one_note_per_dictation`, `test_short_transcript_rest_returns_empty_not_orphan`, `test_create_from_text_persists_turn_id`. On-device: kill Dragon mid-WS → durable FAILED row w/ Retry → re-transcribes WAV → exactly one note; **10-min concurrent SD+WS soak (gate)**; 3 offline notes across an OTA load without field shift, each dedups by turn_id.
**Closes:** S1-6, S1-7, S1-8, S3-4 (edge documented real-but-uncommon, exercised by fast-tap test).

### W5 — de-bloat + de-fork + one caption + dynamic subscriber table + progress audit
**Goal:** extract the dictation engine from the 3722-LOC ui_notes.c; kill forked enums; remove subscriber ceiling; sweep timestamp class; audit (not blindly collapse) the progress double-write.
**Files/functions:** move `transcription_queue_task` + REST upload + SD WAV I/O + FSM-bridge subscriber into `main/dictation_notes.{c,h}` (extract-only first); ui_notes.c keeps list/edit/search/chips. Collapse `note_state_t`/`note_fail_t` (ui_notes.c:95-113) onto `dict_*` via `note_state_from_dict()`/`note_fail_from_dict()`; remove `DICT_FAIL_CANCELLED`. `voice_dictation.c` — dynamic subscriber table (grow-on-demand, S3-9 overflow no longer silent) + sweep the ~25 hand-copied `(uint32_t)(esp_timer_get_time()/1000)` onto `voice_dictation_now_ms()`. Dragon `dictation_post.py` — **grep `progress`-frame consumers fleet-wide; if none, remove ONLY the progress half and keep `emit_progress_pair`'s legacy emission; if any consumer exists, doc-note the dual-write as intentional and change nothing** (S3-8, precisely scoped). S3-7 — document streaming-load/batch-use mismatch; defer model-load change unless free.
**Tests:** host — `test_note_state_mapping_roundtrip`, `test_subscriber_table_grows` (subscribe 5, all fire), `test_now_ms_single_source`; full suite green post-extraction; `git-clang-format --diff origin/main` clean. pytest — `test_legacy_frame_still_emitted_after_progress_audit`. On-device: full Notes regression (record/save/fail/retry/search/edit).
**Closes:** S2-6, S2-11, S3-7 (documented/deferred), S3-8 (audited/scoped), S3-9, S3-10.

---

## Risks + live-verify checklist

**Risks (with mitigations):**
1. **LVGL-from-wrong-task** — decay enqueues to `task_worker` via `tab5_worker_enqueue`, never dispatches from the esp_timer task; full-queue re-arms (no headless block). Verify no `lv_*` reachable synchronously from `dict_decay_timer_cb`; host-test re-entrancy.
2. **W1 data-loss window** — identity ships in W1 with decay; per-origin `resolution_pending` suppresses decay until resolution lands; guarded `FAILED→SAVED` corrects a late summary (cross-turn-safe only from W2's echo, hence the W1 `origin==WS` qualifier).
3. **Offline-queue/decay race + reverse race** — `try_begin_offline` (offline claim) and `begin`-returns-NULL (WS refusal during offline UPLOADING) close both directions in W1.
4. **SD/Wi-Fi contention** — W4 10-min concurrent soak is a gate; grace-timer relocation (W3) does not depend on SD-write; reversible to discard-on-success.
5. **notes.json migration** — key-mapped serializer (already keyed) asserted by an old-fixture host test in W4.
6. **turn_id rollout skew** — missing field = match (host-tested); the FSM stuck-watchdog (W3, replacing the deleted grace timer) bounds any dropped-legit-terminal to a 60s-late FAILED, never a permanent hang — and it works against an old Dragon, closing the seq-risk that the grace timer's removal could strand new-Tab5/old-Dragon.
7. **note_id buffer** — `DICT_NOTE_ID_LEN` pinned against the longest real Dragon id + host test, not assumed.
8. **Struct-by-value growth** — ~80 bytes, no new per-dispatch alloc; noted, not blocking.

**Live-verify on device (Tab5 current DHCP lease via `/dictation_pipeline` + `/events`), per wave:**
- W1: SAVED→IDLE ~2s and FAILED→IDLE ~5s with all surfaces navigated away; offline queue drains 3 notes; a turn left awaiting resolution does NOT decay; starting a WS dictation while an offline upload is mid-flight is refused with a toast.
- W2: cancel mid-dictation → grey "Cancelled"; back-to-back A/B → distinct turn_ids, A's late summary dropped, B's correct.
- W3: cloud-STT failure → resolves via local fallback (warning code `stt_fallback`); no-pause >5-min tail → `dictation_truncated` → SAVED + "capped" toast; kill Dragon after `stop` → FSM stuck-watchdog FAILs at ~60s; confirm one terminal per turn_id in obs.
- W4: kill `tinkerclaw-voice` mid-WS → durable FAILED row + working Retry re-transcribing `<turn_id>.wav`; exactly one note on success; offline note returns note_id over HTTP and dedups by turn_id; 10-min SD+WS soak no WS reconnect; OTA-load old notes.json with no field shift.
- W5: full host suite + Notes on-device regression green; clang-format clean; one caption string per fail reason across all surfaces; legacy frame still emitted after the progress audit.

---

## Open decisions for human sign-off

- **D1 — Dragon `notes/db.py` is the dictation note owner; foundation `db_notes.py` table is out-of-scope for dictation; no store merge.** (Declares a store dead for dictation permanently.)
- **D2 — Two-tier cap kept visible (Tab5 ≤4 hr record / Dragon 5-min per-segment / typed `dictation_truncated`), NOT unified to one Dragon-negotiated number.** (The alternative would lower the power-user recording cap.)

Both are the soundest options given Q6A memory limits and the thin-client contract; flagged only because each forecloses an alternative a reviewer might otherwise expect. All other decisions — turn_id minted-in-FSM with one generator (no split-brain), decay-via-`tab5_worker_enqueue` with full-queue re-arm, identity-in-W1, per-origin `resolution_pending`, the FSM stuck-watchdog replacing the deleted grace timer, the single reconciled `_ensure_fallback_stt_then_transcribe` helper, offline note_id crossing over the REST HTTP response (not WS), permanent blank-summary fallback, dictation-only voice_state unification — are resolved in-spec with rationale and do not require sign-off.
