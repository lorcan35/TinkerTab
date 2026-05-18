# TinkerTab SOLID Audit — 2026-05-03

Fresh SOLID-principles pass over `main/` (138 files, ~46 KLOC).  Builds on the
2026-05-01 architecture audit; only flags things that audit either missed,
underweighted, or that May 2026 churn introduced.

## Executive summary

The codebase is in **noticeably better shape** than the prior audit found.  The
debug_server god-file shrank 4520 → 3339 LOC across nine clean family extracts
(#336/#340-#348), `ui_notes` BSS cache moved to PSRAM (#335), and the LVGL
async wrapper rule is still 100% enforced (zero direct `lv_async_call`
references outside `ui_core.c` itself).  Modularity-rule discipline around
K144 holds.

The dominant residual issue is **`voice.c` (4251 LOC, unchanged)**: its
`handle_text_message()` dispatcher is 970 LOC of `if/else-if` chain across 25
WS message types spanning chat, widgets, dictation, receipts, vision, and
config — and ~270 LOC of that is widget handling that is a **clean, low-risk
extract** today (already calls a stable widget_store.h surface).

Two broader patterns deserve attention this round:
1. **Persistent `extern` declarations at call sites** (203 occurrences across
   18 files) — DIP smell, redundant noise, and a real refactor headwind.
2. **Service registry ceremony** — the lifecycle scaffolding's `_stop()` API
   is **never called anywhere**, three of five `_stop` impls are no-ops, and
   the registry adds 187 LOC + 15 `extern` decls + boilerplate per service
   that buys nothing main.c couldn't do with five direct init calls.

P0 findings: **2** (one ISP/naming collision, one big-volume DIP smell).
P1 findings: **9**.  P2 findings: **11**.  Anti-findings: **6**.

---

## SRP — Single Responsibility

### SRP-1 [P1] — voice.c `handle_text_message()` dispatches 25 WS verb classes in one 970-LOC function

**Where:** [main/voice.c:926-1890](main/voice.c#L926)

**Smell:** A single static function owns the entire Dragon → Tab5 JSON message
surface.  25 verb branches: `stt_partial`, `stt`, `tts_start`, `tts_end`,
`llm`, `cancel_ack`, `error`, `session_start`, `session_messages`,
`dictation_postprocessing`, `dictation_postprocessing_error`,
`dictation_postprocessing_cancelled`, `dictation_summary`, `note_created`,
`llm_done`, `receipt`, `text_update`, `vision_capability`, `pong`,
`config_update`, `tool_call`, `tool_result`, `media`, `card`, `widget_card`,
`audio_clip`, `widget_live*`, `widget_list`, `widget_chart`, `widget_media`,
`widget_prompt`, `widget_dismiss`.

**Why it's a violation:** Each verb class is its own concern (chat rendering,
session lifecycle, dictation flow, billing, tool activity, widget store
mutation) but they all share the same `cJSON *root` + `if/else-if` chain, so
adding any new verb forces a touch in this exact function and makes the
control flow nearly impossible to reason about locally.

**Fix sketch:** Convert to a verb-table dispatch — `static const struct {const
char *name; void (*fn)(cJSON*);} s_verbs[]` populated by per-verb static
handlers.  The widget verbs (270 LOC, see SRP-2) split out first, dictation
verbs (~80 LOC) second, receipt + budget (~120 LOC) third.

**Risk:** Low.  Each branch is already self-contained — no shared locals
across branches.  Receipt/config_update branches mutate `s_*` statics; those
keep file scope.

---

### SRP-2 [P1] — voice.c widget WS handlers are a clean ~270 LOC extract

**Where:** [main/voice.c:1596-1865](main/voice.c#L1596)

**Smell:** Six `widget_*` JSON handlers (`widget_card`, `widget_live`,
`widget_live_update`, `widget_list`, `widget_chart`, `widget_media`,
`widget_prompt`, `widget_dismiss`) live inside `handle_text_message`.  Each
re-declares `extern widget_store_upsert` / `extern widget_store_update` /
`extern widget_tone_from_str` / `extern ui_home_update_status` — six identical
extern blocks (lines 1638-1646, 1699-1701, 1743-1745, 1783-1785, 1815-1817,
1856-1857) that should be a header include.

**Why it's a violation:** SRP — voice.c's job is the WS protocol, not
unmarshalling 7 widget JSON shapes.  Also DIP — the function reaches into the
widget_store implementation through repeated extern decls instead of
`#include "widget.h"`.

**Fix sketch:** New `voice_widget_ws.{c,h}`:
```c
/* voice_widget_ws.h */
bool voice_widget_ws_dispatch(const char *type_str, cJSON *root);
```
Returns `true` if the verb was handled, `false` otherwise; caller chains it
before its remaining else-if cases.  voice.c includes `widget.h` exactly
nowhere afterward.

**Risk:** Very low.  All widget verbs are self-contained, write through the
already-stable `widget_store_*` surface, and end with the same
`tab5_lv_async_call(ui_home_update_status, NULL)` tail.

**Estimated diff:** -270 LOC voice.c, +320 LOC voice_widget_ws.c.

---

### SRP-3 [P1] — voice.c receipt + budget + cap-downgrade is its own concern (~150 LOC)

**Where:** [main/voice.c:1305-1410](main/voice.c#L1305)

**Smell:** The `receipt` verb branch reads token counts, computes mils,
calls `tab5_budget_accumulate`, `tab5_budget_get_today_mils`, refreshes the
home + chat-header spend chips, AND auto-downgrades the voice mode when the
daily cap is hit (all in ~110 LOC inside `handle_text_message`, plus another
~40 LOC of `voice_defer_receipt_attach` machinery at line 391).  Mixes
billing/policy with WS protocol.

**Why it's a violation:** Billing policy (cap-triggered mode downgrade) is a
different axis of change than "Dragon sent me a receipt frame".  Today the
cap-downgrade rule is hardcoded ("Hybrid+Cloud → Local; Agent unchanged");
extending to "warn at 80% before downgrade" or "different policy per user"
forces an edit deep inside the WS dispatcher.

**Fix sketch:** Move the receipt → spend → cap-policy chain to
`voice_billing.{c,h}`:
```c
void voice_billing_record_receipt(const char *model, int ptok, int ctok,
                                  int cost_mils);
```
The function owns the NVS write, UI refresh hops, and the cap-downgrade rule.
`voice.c` becomes a 5-line caller.

**Risk:** Low.  Only one caller; behaviour is a pure function of its inputs +
NVS state.  Easy to unit-test offline.

---

### SRP-4 [P1] — main.c hosts a 430-LOC inline serial REPL with 40+ commands

**Where:** [main/main.c:731-1167](main/main.c#L731)

**Smell:** `app_main`'s "interactive console" path is one giant
`while/getchar` loop with ~40 nested `if/else if (strcmp(cmd_buf, ...))`
branches, including the entire K144 surface (`m5ping`, `m5lscmd`, `m5infer`,
`m5release`, `m5tts`, `m5recover`, `m5baud`, `m5chain`).  This is the only
caller of `voice_m5_llm_infer`, `voice_m5_llm_tts`, `voice_m5_llm_release`,
`voice_m5_llm_recover_baud`, `voice_m5_llm_set_baud`, `voice_m5_llm_chain_*`
outside of voice_onboard.c — meaning a "test bench surface" leaks
implementation calls main.c shouldn't know about.

**Why it's a violation:** SRP (boot-orchestration vs developer test bench),
plus DIP (main.c bypasses voice_onboard's lifecycle to poke voice_m5_llm
directly).

**Fix sketch:** Extract `serial_repl.{c,h}`:
```c
void serial_repl_start(void);   /* spawns the task; no return */
```
Internal handler table (`{name, fn}` rows).  K144 commands become thin
wrappers that go through `voice_onboard_*` so the policy gate (READY check,
mutex) is honoured.

**Risk:** Low if extraction is mechanical.  Only sensitive if any serial
command currently relies on something `app_main` only initialised in its
local stack (skim shows nothing).

**Estimated diff:** -430 LOC main.c (1172→742), +500 LOC serial_repl.c.

---

### SRP-5 [P1] — debug_server.c `selftest_handler` has 12 inline subsystem probes

**Where:** [main/debug_server.c:2094-2249](main/debug_server.c#L2094)

**Smell:** 155 LOC, one cJSON struct, twelve copy-pasted `cJSON_CreateObject
+ AddString + AddBool + counter` blocks for {wifi, voice_ws, sd_card, psram,
internal_sram, dma, lvgl_pool, audio, mic, camera, k144, etc}.  Each has
slightly different probe shape.

**Why it's a violation:** Adding a new subsystem means appending a 13th 13-line
copy-paste in this exact function.  The list of "things tab5 self-tests" is a
different concern from the HTTP handler that returns the JSON.

**Fix sketch:** Add a `selftest.{c,h}` table:
```c
typedef struct {
    const char *name;
    bool (*probe)(void *out_extra);  /* extra fields glued in */
    void (*augment_json)(cJSON *t);  /* optional, NULL ok */
} selftest_t;
```
Modules register at boot via `selftest_register()`.  Handler iterates the
table, dumps JSON.

**Risk:** Low — selftest results are a debug-only surface.  Risk is
underestimating what each existing probe needs to expose; first cut keep all
12 probes in selftest.c verbatim, then iterate.

---

### SRP-6 [P2] — voice.c dictation state surface is half-extracted

**Where:** [main/voice.c:326-329, 945-1245, 3323-3408, 4018-4030](main/voice.c#L326)

**Smell:** Dictation owns three top-level statics (`s_dictation_text`,
`s_dictation_title`, `s_dictation_summary`), four WS verb branches
(`stt_partial` accumulation, `dictation_postprocessing*`,
`dictation_summary`), `voice_start_dictation()`, `voice_get_dictation_*()` —
all interleaved with Ask-mode and Call-mode logic.  ~250 LOC scattered.

**Why it's a violation:** Dictation is a distinct user flow with its own state
machine (accumulating partial → silence → post-process → summary) that's
threaded through three different parts of voice.c.  Touching it always means
greping the file.

**Fix sketch:** `voice_dictation.{c,h}` owns the three buffers + `start`,
`stop`, `get_text/title/summary`, and the four WS handlers (called via
SRP-1's verb-table).  voice.c retains only the mic-task branch that consumes
the dictation flag.

**Risk:** Medium — the partial-text accumulation runs on the voice WS rx
task and reads/writes statics that voice.c also reads from the mic task.
Need careful audit of any cross-task reads (probably one volatile + the
existing state mutex covers it).

---

### SRP-7 [P2] — service_registry.c is dead-weight ceremony around 5 init calls

**Where:** [main/service_registry.c:1-187](main/service_registry.c#L1) +
[main/service_dragon.c](main/service_dragon.c#L1) +
[main/service_audio.c](main/service_audio.c#L1) etc.

**Smell:** `tab5_services_stop()` is **never called anywhere** in the codebase
(verified: `grep -rn 'tab5_services_stop' main/` returns only the definition
+ its prototype).  Three of five `_stop()` implementations are pure no-ops
(`network_service_stop` says "WiFi disconnect not implemented yet — just
log").  `service_dragon.c` is self-described as "nearly vestigial" with a
3-line `init` and a `start` that just logs.

**Why it's a violation:** SRP — the registry's stated job is "lifecycle
management" but only the `init` half is real.  The `_start/_stop` slots are
ceremony.  187 LOC + 15 `extern` decls + 5 boilerplate files implement what
`audio_init(); display_init(); ...` would in 5 lines of `app_main`.

**Fix sketch:** Two paths:
- **Aggressive:** Delete `service_registry.{c,h}`, `service_dragon.c`
  (mode-mutex init moves to main.c), shrink each `service_*` to a single
  `init()` function.  Net: ~250 LOC deleted.
- **Conservative:** Keep registry but delete `_start/_stop` slots until a real
  caller emerges; preserves the pattern as scaffolding.  Net: ~80 LOC
  deleted.

**Risk:** Very low (aggressive) — there is no behavioural reliance on the
state machine the registry tracks.  Only loss is `tab5_services_print_status()`
in the serial REPL, replaceable in ~10 lines.

---

### SRP-8 [P2] — voice.c LAN+ngrok probe task is a separate concern

**Where:** [main/voice.c:2881-3000](main/voice.c#L2881) +
[main/voice.c:215-218](main/voice.c#L215)

**Smell:** `voice_lan_probe_task` (130 LOC) implements a progressive WiFi
zombie-detection escalation ladder (soft-kick → hard-kick → reboot) along
with TCP probes for both LAN and ngrok endpoints.  Owns 4 file-static
probe-result globals.  Has nothing to do with the WS protocol; it's a
network-health watchdog that voice.c happens to host because it shares a few
WiFi helpers.

**Why it's a violation:** SRP — network watchdog logic is a peer of
heap_watchdog.c, not part of the voice client.

**Fix sketch:** Move to `network_watchdog.{c,h}` next to `heap_watchdog.{c,h}`.
voice.c reads the probe results via getter calls; everything else stays.

**Risk:** Low — the task already runs independently; only coupling is the
4 file-statics that get pulled into the new module.

---

## OCP — Open/Closed

### OCP-1 [P1] — debug_server.c URI registration is a 47-line `httpd_register_uri_handler` switchboard

**Where:** [main/debug_server.c:3243-3316](main/debug_server.c#L3243)

**Smell:** Adding a new endpoint means: (a) write the handler, (b) declare its
`httpd_uri_t uri_*` struct, (c) call `httpd_register_uri_handler(server,
&uri_*)`.  Step (c) is a single block of 47 nearly-identical lines that grows
linearly with endpoints.  The Wave 23b family extracts have NOT migrated
their registrations into the family files; instead each per-family `_register`
function gets called from this same block.

**Why it's a violation:** OCP — adding endpoints requires modifying a central
list rather than just dropping a new file.

**Fix sketch:** Convert each per-family registrar to a constructor-style hook:
```c
/* debug_server_internal.h */
void tab5_debug_register_family(void (*reg_fn)(httpd_handle_t));

/* In each debug_server_X.c */
__attribute__((constructor)) static void _register_X(void) {
    tab5_debug_register_family(register_X_handlers);
}
```
debug_server.c then iterates the family list at boot.  Or simpler: an
explicit `tab5_debug_server_families_init(server)` function that calls each
`debug_server_X_register(server)` — still central, but the dispatcher is one
line per family instead of one per endpoint.

**Risk:** Low.  Constructor-style adds boot-order dependency; the explicit
call path is safer.

---

### OCP-2 [P2] — `selftest` probe list (see SRP-5) — same fix-shape

**Where:** [main/debug_server.c:2099-2245](main/debug_server.c#L2099)

Same pattern as OCP-1 — the cJSON probe table grows by copy-paste.  The fix
sketched in SRP-5 (registry table) is the OCP fix.

---

### OCP-3 [P2] — config_update voice-mode fields use ad-hoc enum reading

**Where:** [main/voice.c:1438-1530](main/voice.c#L1438)

**Smell:** Adding a new config_update field (e.g., `voice_codec`,
`tts_voice_id`) means adding a new `cJSON_GetObjectItem` block + assignment
inside the type=="config_update" branch.  Each field has its own special
handling (cap-downgrade for vmode; codec name string for codec; etc.).

**Why it's a violation:** OCP — a new field is a new edit to the central
handler.

**Fix sketch:** Field registry + per-field setter callbacks.  Lower priority
than the others because this branch only grows occasionally.

---

## LSP — Liskov Substitution

### LSP-1 [P0] — `voice_mode_t` enum + `VOICE_MODE_*` macro family share the same prefix but are unrelated

**Where:** [main/voice.h:36-43](main/voice.h#L36) +
[main/config.h:52-57](main/config.h#L52)

**Smell:** Two completely different enumerations both named `VOICE_MODE_*`:
```c
/* voice.h */
typedef enum { VOICE_MODE_ASK, VOICE_MODE_DICTATE, VOICE_MODE_CALL } voice_mode_t;

/* config.h */
#define VOICE_MODE_LOCAL       0
#define VOICE_MODE_HYBRID      1
#define VOICE_MODE_CLOUD       2
#define VOICE_MODE_TINKERCLAW  3
#define VOICE_MODE_ONBOARD     4
```
They mean different things at different layers (mic-task dispatch vs user-tier
NVS pick) but a reader sees `VOICE_MODE_*` and can reasonably believe they're
the same family.  ui_voice.c switches on numeric values 0-4 against
`VOICE_MODE_LOCAL/HYBRID/CLOUD` macros even though `voice_mode_t` only
defines symbolic constants for {0,1,2}.

**Why it's a violation:** LSP — code that takes a `voice_mode_t` parameter
might be passed a `VOICE_MODE_LOCAL` macro by an unaware caller, with no
compiler warning (both are `int`-compatible) and silent semantic breakage
(`VOICE_MODE_LOCAL == VOICE_MODE_ASK == 0`, but `VOICE_MODE_TINKERCLAW == 3`
is undefined as a `voice_mode_t`).

**Fix sketch:** Rename the macros.  The user-tier semantic is "voice
**routing**", not "voice mode" — the enum is the real "mode" (mic-task
dispatch).  Rename `VOICE_MODE_LOCAL` → `VROUTE_LOCAL`, etc.; keep
`voice_mode_t` as-is.  ~50 sites; mechanical.  NVS key `vmode` stays
unchanged (it's a u8, not a typed enum).

**Risk:** Medium — a wide rename, but every site is greppable; CI catches
typos.  Worth doing exactly because the pun has already misled readers.

**Estimated diff:** ~50 file changes, +0 LOC net.

---

### LSP-2 [P2] — `voice_send_config_update` vs `voice_send_config_update_ex` aren't substitutable

**Where:** [main/voice.c:3923-3962](main/voice.c#L3923)

**Smell:** `voice_send_config_update(mode, model)` calls
`voice_send_config_update_ex(mode, model, NULL)` — fine.  But the `_ex` variant
adds a `reason` field to the JSON, and Dragon's behaviour differs based on
whether `reason == "cap_downgrade"` (triggers a TTS alert).  A caller who
only knows the public surface might assume the two are interchangeable when
they're not.

**Why it's a violation:** LSP — the "_ex" variant has side effects the base
variant doesn't.

**Fix sketch:** Either (a) make `_ex` the only public function and inline the
2-line `_simple` wrapper at its 3 callers, or (b) document the
"reason"-triggers-alert contract in voice.h.  (a) is cleaner.

**Risk:** Trivial.

---

## ISP — Interface Segregation

### ISP-1 [P2] — voice.h is 263 LOC exposing 60+ symbols; many consumers need only `voice_state_t`

**Where:** [main/voice.h:1-263](main/voice.h#L1)

**Smell:** voice.h exposes mic, TTS, dictation, call-audio, vision, billing,
config_update, link-health, K144 chain, history, cancel, reconnect,
deferred-receipt — every consumer of voice.h drags the whole protocol surface
into its translation unit.  heap_watchdog.c only needs `voice_get_state` +
`voice_state_t`; ui_chat.c needs maybe 8 functions; service_dragon.c needs 0.

**Why it's a violation:** ISP — modules depend on a header much wider than
their actual usage.

**Fix sketch:** Split into voice_state.h (4 symbols), voice_send.h (8
symbols), voice_health.h (4 symbols), keep voice.h as the umbrella.  Lower
priority because header bloat in C doesn't have the runtime cost it does in
C++; the refactor would be cosmetic.

**Risk:** Low but high churn.

---

### ISP-2 [P2] — voice_m5_llm.h is 434 LOC exposing 18 functions; ui_settings + debug_server_m5 use 3

**Where:** [main/voice_m5_llm.h:1-434](main/voice_m5_llm.h#L1)

**Smell:** Same shape as ISP-1.  ui_settings only calls `_sys_hwinfo`,
`_sys_version`, `_sys_lsmode`; debug_server_m5 the same plus `_get_baud`.
Everyone else (main.c REPL aside) goes through voice_onboard.c.  The chain
APIs (`_chain_setup`, `_chain_run`, `_chain_teardown`) and the streaming
infer/tts surface should arguably be `voice_m5_llm_internal.h` only seen by
voice_onboard.c.

**Fix sketch:** Carve `voice_m5_llm_telemetry.h` for the read-only `sys_*`
+ `get_baud` getters; keep heavyweight inference + chain in voice_m5_llm.h
seen only by main.c (REPL) and voice_onboard.c.

**Risk:** Low (header split, no behavioural change).  Worth doing IF SRP-4
(REPL extract) lands first, since the REPL is the only "innocent" consumer of
the heavyweight surface.

---

## DIP — Dependency Inversion

### DIP-1 [P0] — 203 `extern` declarations at call sites across 18 files

**Where:** widespread; top offenders:
- [main/debug_server.c:1129-3239](main/debug_server.c#L1129) — 47 occurrences
- [main/voice.c:382-3577](main/voice.c#L382) — 40 occurrences (most repeated)
- [main/ui_home.c:849-2160](main/ui_home.c#L849) — 17
- [main/ui_settings.c:98-854](main/ui_settings.c#L98) — 16
- [main/ui_camera.c:274-1002](main/ui_camera.c#L274) — 13

**Smell:** Code uses `extern void some_func(void);` inline at the call site
instead of `#include`-ing the header that already declares it.  Three
sub-flavors:
1. **Pure noise** — the header is already transitively included.  Example:
   [main/heap_watchdog.c:174](main/heap_watchdog.c#L174) does `extern
   voice_state_t voice_get_state(void);` THREE times even though
   `voice.h` is included at line 17 and already declares it.
2. **Bypassing missing headers** — `widget_store_*` is in `widget.h` but
   voice.c has six clusters of `extern widget_store_upsert/...` instead of
   `#include "widget.h"`.
3. **Cross-module reach-through** — `chat_store_attach_receipt_ex` is
   declared in `chat_msg_store.h` but voice.c + voice_onboard.c each carry
   their own extern decl rather than including the header.

**Why it's a violation:** DIP — high-level modules (voice.c, ui_*, debug_server.c)
reach into low-level modules (widget_store, chat_store, ui_home) through
ad-hoc declarations rather than depending on a stable abstraction (the
header).  Refactoring widget_store's signature breaks N callers
silently — the linker complains, but only after the build.  Also: every
extern is a documentation lie about what voice.c "really" depends on.

**Fix sketch:** Three mechanical sweeps:
1. Delete redundant externs whose header is already included
   (heap_watchdog.c × 3, ui_camera.c × ~6, ui_chat.c × ~3).  ~30 LOC.
2. Replace voice.c widget extern clusters with `#include "widget.h"` (one
   include, six 7-line blocks deleted).  ~40 LOC + cleaner.
3. Promote `chat_store_*` cross-module externs to honest includes.  ~10 LOC.

**Risk:** Trivial; pure code hygiene.  Total saving ~80 LOC + significant
readability.

---

### DIP-2 [P1] — voice_onboard.c reaches into ui_home + chat_store via extern

**Where:** [main/voice_onboard.c:96, 197, 284-286](main/voice_onboard.c#L96)

**Smell:** voice_onboard.c — a clean extracted module — does
```c
extern void tab5_debug_obs_event(const char *kind, const char *detail);
extern void ui_home_clear_error_banner(void);
extern int chat_store_attach_receipt_ex(...);
extern void ui_chat_refresh_receipts(void);
```
These are "I know voice_onboard works without this so I'd rather inline-extern
than make myself depend on ui_home.h" reasoning — but the extern STILL makes
voice_onboard depend on ui_home; it just hides the dependency from the
include graph.

**Why it's a violation:** DIP — module thinks it's free of UI deps because no
`#include "ui_home.h"` is visible, but it can't link without ui_home.

**Fix sketch:** Two options:
- **Honest includes** — add `#include "ui_home.h"`, `#include "chat_msg_store.h"`,
  `#include "ui_chat.h"`, `#include "debug_obs.h"`.  Same dependencies, no
  surprises.
- **Inversion** — voice_onboard publishes events; ui_home subscribes.  Adds a
  `voice_onboard_register_state_cb()` API.  Cleaner but more code; only
  worth it if there's a 2nd consumer.

**Risk:** Trivial for the includes path.

---

### DIP-3 [P1] — voice.c statelessly reaches into 18 different ui_*/chat_*/widget_* helpers

**Where:** [main/voice.c:382-4023](main/voice.c#L382) — 40 extern decls

**Smell:** voice.c is "the WS client".  But its extern decls show it actually
calls into: `ui_home_show_toast`, `ui_home_refresh_sys_label`,
`ui_home_refresh_mode_badge`, `ui_home_pulse_orb_alert`,
`ui_home_show_error_banner`, `ui_home_update_status`, `ui_chat_refresh_*`,
`ui_chat_push_*`, `chat_store_*`, `widget_store_*`, `tool_log_push_*`,
`ui_notes_*`, `tab5_debug_obs_event`, `voice_lan_probe_task`,
`voice_defer_receipt_attach` — voice.c effectively orchestrates the entire
device's response to a Dragon message.  This is the SRP-1 problem viewed
through the DIP lens.

**Why it's a violation:** DIP — voice.c is the highest-level WS protocol
module but it's hardwired to ~18 low-level UI/storage modules.  An
abstraction layer ("I just got a tool_call event" → published to N
subscribers) would invert the dependency.

**Fix sketch:** Long-term — events bus ("voice_event_emit") with pluggable
subscribers (ui_chat subscribes, ui_home subscribes, chat_store subscribes).
Short-term — see SRP-1/SRP-2/SRP-3, which constrain the blast radius.

**Risk:** High at the bus-extraction level (touches every UI module).
Pragmatic priority is the per-verb extracts first.

---

### DIP-4 [P2] — debug_server families reach back into debug_server.c via internal header

**Where:** [main/debug_server_internal.h](main/debug_server_internal.h#L1) +
all `debug_server_*.c`

**Smell:** Per-family extracts call back into debug_server.c through a
private "internal" header (`tab5_debug_check_auth`, `tab5_debug_send_json_resp`,
`tab5_debug_check_factory_reset_confirm`).  This is fine in itself, but
documents that the family files are NOT independent of the parent — they're
sub-files sharing a private contract.  The auth-token state lives in
debug_server.c; family files have no choice but to call back.

**Why it's a violation:** DIP — the auth gate is a cross-cutting concern that
should be a layered service, not a private back-channel.

**Fix sketch:** Promote `debug_server_internal.h` symbols to a real
`debug_auth.{c,h}` module with the auth-token statics.  All families depend on
debug_auth.h instead of debug_server_internal.h.  debug_server.c becomes a
pure URI dispatcher.

**Risk:** Low; the internal header is already a stable shared contract,
this is mostly a rename + relocate of state.

---

### DIP-5 [P2] — heap_watchdog.c declares `voice_get_state` extern despite including voice.h

**Where:** [main/heap_watchdog.c:174, 230, 266](main/heap_watchdog.c#L174)

**Smell:** Three-times-repeated `extern voice_state_t voice_get_state(void);`
inside `static void` callbacks, even though line 17 says `#include "voice.h"`
and voice.h:84 declares the same prototype.  Pure noise.

**Why it's a violation:** Anti-DIP cosmetic — the inclusion is correct, the
extern is wrong.  Misleads readers into thinking voice.h is missing this
function.

**Fix sketch:** Delete all 3.  ~3 LOC change.

**Risk:** None.

---

## Cross-cutting

### X-1 [P1] — Naming pun: `voice_mode_t` vs `vmode` vs `VOICE_MODE_*` macros — three terms, three meanings

**Where:** voice.h, config.h, settings.h, NVS key `vmode`

The same `VOICE_MODE_*` prefix names two enumerations (LSP-1).  Plus the NVS
key is `vmode` (the user-tier).  Plus settings has both a numeric (0-4) and
string ("VMODE_LOCAL_ONBOARD") form.  Reader confusion is constant — the prior
audit's A1 finding even used "five voice modes" and "five tier modes"
interchangeably without flagging that they're literally different enums.

**Fix:** Adopt LSP-1's rename (`VROUTE_*` for the user-tier) and standardize
the prose.

---

### X-2 [P2] — `tool_log_push_call`/`_result` are extern'd from debug_server.c instead of including tool_log.h

**Where:** [main/debug_server.c:2637-2638](main/debug_server.c#L2637)

Standalone DIP-1 instance worth its own line because tool_log.h is exactly 49
LOC and would pull in nothing else.

---

## Anti-findings (patterns considered + rejected)

### AF-1 — voice.c's 4 file-static mutexes are NOT a SRP violation
Same conclusion as the prior audit (A13).  Module-scoped sync primitives are
the right pattern; no refactoring needed.

### AF-2 — chat_*.c family is genuinely well-factored, NOT a god-cluster
Six files, max 920 LOC (chat_msg_view.c), clean single-direction includes
(view → store, ui_chat → all six).  The prior audit's A11 holds.

### AF-3 — service_audio.c's `extern i2c_master_bus_handle_t tab5_get_i2c_bus(void)` is justified
The BSP intentionally hides the bus handle from a public header (concurrency
discipline).  The extern is documented at the BSP boundary; main/ doesn't have
a better path.

### AF-4 — ui_chat.c's `extern lv_obj_t *ui_home_get_screen` calls are fine
ui_home.h ALREADY exposes `ui_home_get_screen()`.  ui_chat.c could drop the
`extern` and use the include — but it already includes ui_home.h transitively.
File this under DIP-1 sweep #1 (low-priority cleanup) rather than its own
finding.

### AF-5 — voice_video.c's single extern (`voice_ws_send_binary_public`) is the cleanest possible
voice_video.h would have to take a forward dependency on voice.h to expose
this, which would create a circular include.  The deliberate `extern` at
voice_video.c:53 with explanatory comment is the right tradeoff.

### AF-6 — `mode_manager.c` thin mutex wrapper is justified (per prior audit A10)
Confirmed unchanged.  4 concurrent caller tasks; mutex is necessary; not
ceremony.

---

## Recommended next 3 PRs

### PR-1: voice.c widget WS handlers → voice_widget_ws.{c,h}
- **What:** Extract the 8 widget verb branches (lines 1596-1865) into a new
  module with one entry point: `bool voice_widget_ws_dispatch(const char
  *type, cJSON *root)`.
- **Why:** Cleanest seam in voice.c.  All 8 verbs already use the stable
  widget_store.h surface; six clusters of redundant `extern` decls disappear.
- **Risk:** Very low — verbs are pure functions of their inputs + the
  widget_store global.  No mutex, no tasks, no UI refresh more complex than
  one `tab5_lv_async_call(ui_home_update_status, NULL)` tail.
- **Diff estimate:** -270 LOC voice.c (4251→3981), +320 LOC voice_widget_ws.c.
- **Closes findings:** SRP-2 + half of DIP-1 + half of DIP-3.

### PR-2: serial REPL → serial_repl.{c,h}; main.c stops calling voice_m5_llm directly
- **What:** Lift the `cmd_buf` switch (main.c:731-1167) into a dedicated
  task.  Replace direct `voice_m5_llm_*` calls with `voice_onboard_*` so the
  REPL goes through the same READY-gate the rest of the system uses.
- **Why:** SRP-4 + ISP-2 setup.  main.c shrinks back to a pure boot
  orchestrator (~740 LOC).
- **Risk:** Medium — the REPL is a developer-only surface, but each command
  needs verification it still works after the indirection.
- **Diff estimate:** -430 LOC main.c, +500 LOC serial_repl.c.
- **Closes findings:** SRP-4, sets up ISP-2.

### PR-3: extern-cleanup sweep (DIP-1, DIP-5)
- **What:** Three mechanical passes:
  1. Delete redundant externs where header is already included
     (heap_watchdog × 3, ui_camera × 6, ui_chat × 3).
  2. Replace voice.c widget extern clusters with `#include "widget.h"`
     (also closed by PR-1 if it lands first).
  3. Promote cross-module externs (`chat_store_attach_receipt_ex`,
     `tool_log_push_call/_result`) to honest header includes.
- **Why:** Pure hygiene; ~80 LOC deleted; readability wins.  No behaviour
  change.
- **Risk:** Trivial — CI catches anything that breaks.  Recommended as a
  weekend-throwaway PR; can land independent of PR-1/PR-2.
- **Diff estimate:** -80 LOC across ~12 files.
- **Closes findings:** DIP-1, DIP-5, X-2.

---

## Summary table

| ID    | Sev | Principle | Where | Title |
|-------|----|-----------|-------|-------|
| SRP-1 | P1 | SRP | voice.c:926-1890 | `handle_text_message` dispatches 25 WS verbs in 970 LOC |
| SRP-2 | P1 | SRP | voice.c:1596-1865 | Widget WS handlers — clean ~270 LOC extract candidate |
| SRP-3 | P1 | SRP | voice.c:1305-1410 | Receipt + budget + cap-downgrade should be voice_billing.c |
| SRP-4 | P1 | SRP | main.c:731-1167 | 430-LOC inline serial REPL with K144 reach-through |
| SRP-5 | P1 | SRP | debug_server.c:2094-2249 | `selftest_handler` 12 inline subsystem probes |
| SRP-6 | P2 | SRP | voice.c (4 regions) | Dictation half-extracted — 250 LOC scattered |
| SRP-7 | P2 | SRP | service_registry.c | Lifecycle ceremony around 5 init calls; `_stop` never called |
| SRP-8 | P2 | SRP | voice.c:2881-3000 | LAN+ngrok probe is its own watchdog, not WS-client business |
| OCP-1 | P1 | OCP | debug_server.c:3243-3316 | 47-line URI registration switchboard |
| OCP-2 | P2 | OCP | debug_server.c:2099-2245 | Selftest probe table grows by copy-paste |
| OCP-3 | P2 | OCP | voice.c:1438-1530 | config_update fields are ad-hoc |
| LSP-1 | **P0** | LSP | voice.h:36 + config.h:52 | `voice_mode_t` and `VOICE_MODE_*` macros are unrelated enums sharing a prefix |
| LSP-2 | P2 | LSP | voice.c:3923-3962 | `_send_config_update` vs `_ex` — `_ex` has Dragon-side side effects |
| ISP-1 | P2 | ISP | voice.h | 263 LOC / 60+ symbols; consumers use 4-8 |
| ISP-2 | P2 | ISP | voice_m5_llm.h | 434 LOC / 18 symbols; UI/debug consumers use 3-4 |
| DIP-1 | **P0** | DIP | 18 files, 203 occurrences | Persistent `extern` redeclarations at call sites |
| DIP-2 | P1 | DIP | voice_onboard.c:96-286 | Extracted module hides UI deps via extern |
| DIP-3 | P1 | DIP | voice.c (40 externs) | voice.c orchestrates 18 ui/chat/widget modules silently |
| DIP-4 | P2 | DIP | debug_server_internal.h | Auth-token statics should be a real `debug_auth.c` module |
| DIP-5 | P2 | DIP | heap_watchdog.c:174,230,266 | `extern voice_get_state` × 3 redundant w/ `#include "voice.h"` |
| X-1   | P1 | naming | voice.h + config.h + NVS | Three "voice mode" terms, three meanings |
| X-2   | P2 | DIP | debug_server.c:2637 | tool_log functions extern'd instead of including 49-LOC header |

---

## Methodology

- LOC + structural counts via `wc`, `grep`, `awk` over `main/`
- `extern` audit: `grep -rn '^[[:space:]]*extern [a-z]' main/*.c`
- WS verb audit: `grep -nE '"type"|strcmp|cJSON_GetObjectItemCaseSensitive' main/voice.c`
- Service registry callers: `grep -rn 'tab5_services_stop\|service_*_stop' main/`
- LVGL async wrapper enforcement: `grep -nE 'lv_async_call\b' main/*.c | grep -v 'tab5_lv_async_call'` — confirmed clean
- K144 modularity rules: re-verified per prior audit A6 — still locked
- Cross-stack drift: not re-audited (prior audit's A9 stands; Tab5/Dragon
  vmode 0-4 enumeration unchanged in May 2026)

**Files NOT examined in depth:** `bsp/tab5/*` (out of scope per prior audit),
`managed_components/*` (vendored), `dragon_voice/*` (separate repo mirror),
`tests/e2e/*` (Python harness — peer agent owns).
