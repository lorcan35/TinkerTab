# K144 Chain Hardening Plan (2026-04-29)

> **For agentic workers:** Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task.  Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Source of findings:** [`docs/AUDIT-k144-chain-2026-04-29.md`](AUDIT-k144-chain-2026-04-29.md)
>
> **Tracking:** TT #317 (master) + chain-hardening tracking issue (TBD on first wave).

**Goal:** Close the 5 P0s + ~12 P1s flagged in the audit, in 5 wave-style PRs that can land independently.

**Architecture:** Each wave is one (sometimes two) PRs.  Waves are ordered by dependency:  Wave 1 unlocks Wave 3's TTS workaround;  Wave 4 (extract) is decoupled and can run in parallel with Wave 2/3.  Verification is build + flash + on-hardware exercise — no unit-test framework on Tab5 firmware, so each wave's "Verification" section lists the concrete commands (idf.py build / flash, curl, journalctl, serial) that prove it.

**Tech stack:** ESP-IDF v5.5.2, FreeRTOS, LVGL 9.x, C99.  Reference [CLAUDE.md](../CLAUDE.md) for build commands, debug-server endpoints, ADB-to-K144 access.

---

## Wave overview

| Wave | Theme | Status | Commit | Closes audit items |
|---|---|---|---|---|
| 1 | UART discipline + state hygiene + double-fire fix | **SHIPPED** | `900caa4` | #1, #3, #11, #12, #15 |
| 2 | Mode awareness + privacy polish | **SHIPPED** | `cd75662` | #5, #6, #9, #10 |
| 3 | TTS Eigen workaround + chain navigation lifecycle | **SHIPPED** | `1b2fa71` | #2, #16 (#4 mitigated as side-effect — TTS off drain path) |
| 4a | stop_flag in chain_setup_unit + chain-aware tap | **SHIPPED** | `bc1012f` | #13 |
| 4b | Architecture extract — `voice_onboard` module | **SHIPPED** | `b978ffb` | #7, #14, P2-Arch-2.2 |
| 5 | Observability + e2e tests | **SHIPPED** | `7c2b056` | #17, #18 |
| 6 | LLM streaming bubble parity + pill smooth slide | **SHIPPED** | `3c614f4` | #8 + UX polish |
| 7 | Chain UX polish + dead-code cleanup | **SHIPPED** | (this commit) | UX-#4 (error toasts) + `/m5.chain_uptime_ms` + cleanup |

**Total:** 14 of 18 audit findings closed across 8 commits on PR #326.  Remaining items are P2 polish (chain dispatch OCP gap; touch-after-nav LVGL flake) tracked as backlog.  Each wave referenced TT #317 + TT #327 in its commits.

---

# Wave 1 — UART discipline + state hygiene + double-fire fix

**Goal:** Add a transport-layer mutex around Port C UART, fix `s_rx_len` carry-over across chain runs, fix the `s_chain_active` ordering race, and refuse text-while-chain-active to close the silent double-fire path.

**PR title:** `feat(k144): UART mutex + chain state hygiene (refs #317)`

**Files modified:**
- `bsp/tab5/uart_port_c.h` — new lock/unlock API
- `bsp/tab5/uart_port_c.c` — mutex creation + lock/unlock impl
- `main/voice_m5_llm.c` — wrap public entry points + chain_run iteration; reset `s_rx_len` + flush on chain entry/teardown
- `main/voice.c` — fix `s_chain_active = false` ordering; wire `voice_m5_chain_is_active()` into `voice_send_text`

### Task 1.1 — Mutex API in uart_port_c

**Files:**
- Modify: `bsp/tab5/uart_port_c.h`
- Modify: `bsp/tab5/uart_port_c.c`

- [ ] **Step 1: Add API to header**

Add to `bsp/tab5/uart_port_c.h` near the existing API:

```c
/**
 * @brief Acquire exclusive access to Port C UART.
 *
 * The K144 module + StackFlow protocol uses a single TCP-style UART link
 * shared by every voice_m5_llm_* caller.  Concurrent send/recv from
 * multiple FreeRTOS tasks corrupts the response buffer (see
 * docs/AUDIT-k144-chain-2026-04-29.md item #1 for the silent collision
 * scenario).  Every public entry point in voice_m5_llm.c must wrap its
 * send/recv pair with this lock.  Long-lived consumers (chain drain
 * loop) hold for the full session.
 *
 * Recursive — same task may take the lock multiple times safely.
 *
 * @param timeout_ms   Wait budget.  0 = non-blocking probe; portMAX_DELAY
 *                     is acceptable for callers that must serialise.
 * @return ESP_OK on acquisition, ESP_ERR_TIMEOUT on contention.
 */
esp_err_t tab5_port_c_lock(uint32_t timeout_ms);

/**
 * @brief Release the Port C UART lock.  Must be paired with a prior
 *        successful tab5_port_c_lock().  Safe to call on a recursively-
 *        held lock; releases one level.
 */
void tab5_port_c_unlock(void);
```

- [ ] **Step 2: Implement in uart_port_c.c**

Add at the top of `bsp/tab5/uart_port_c.c` near other static state:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_port_c_uart_mutex = NULL;
```

Inside `tab5_port_c_uart_init()`, before the `uart_driver_install` call, add:

```c
   if (s_port_c_uart_mutex == NULL) {
      s_port_c_uart_mutex = xSemaphoreCreateRecursiveMutex();
      if (s_port_c_uart_mutex == NULL) {
         ESP_LOGE(TAG, "Port C UART mutex create failed");
         return ESP_ERR_NO_MEM;
      }
   }
```

Add the implementations at the end of the file:

```c
esp_err_t tab5_port_c_lock(uint32_t timeout_ms)
{
    if (s_port_c_uart_mutex == NULL) return ESP_ERR_INVALID_STATE;
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY
                                                  : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(s_port_c_uart_mutex, ticks) == pdTRUE)
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

void tab5_port_c_unlock(void)
{
    if (s_port_c_uart_mutex == NULL) return;
    xSemaphoreGiveRecursive(s_port_c_uart_mutex);
}
```

- [ ] **Step 3: Build and verify the symbol resolves**

```bash
cd /home/rebelforce/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build 2>&1 | grep -E "error:|tinkertab.bin binary" | head -5
```

Expected: no errors, binary size reported.

### Task 1.2 — Wrap voice_m5_llm.c public entry points

**Files:**
- Modify: `main/voice_m5_llm.c`

- [ ] **Step 1: Add `#include "uart_port_c.h"` is already present.  Add helper macros at top**

Below the existing `static const char *TAG` line, add:

```c
/* All public entry points serialise on the Port C UART.  Long-lived
 * callers (chain_run) take the lock at start of each iteration so a
 * stop-flag flip can land between frames without holding the wire
 * across a 100-ms recv slice.  See docs/AUDIT-k144-chain-2026-04-29.md
 * item #1. */
#define M5_LOCK_OR_RETURN(timeout_ms)                                     \
   do {                                                                   \
      esp_err_t _le = tab5_port_c_lock(timeout_ms);                       \
      if (_le != ESP_OK) {                                                \
         ESP_LOGW(TAG, "uart busy (timeout %u ms)", (unsigned)timeout_ms); \
         return _le;                                                      \
      }                                                                   \
   } while (0)

#define M5_UNLOCK() tab5_port_c_unlock()
```

- [ ] **Step 2: Wrap `voice_m5_llm_probe`**

```c
esp_err_t voice_m5_llm_probe(void) {
   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;
   M5_LOCK_OR_RETURN(2000);

   /* ... existing body unchanged ... */

   bool ok = match && resp.error_code == 0;
   m5_stackflow_response_free(&resp);
   M5_UNLOCK();
   return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
```

Make the same wrap in:
- `voice_m5_llm_infer` (lock before `do_llm_setup`, unlock on every return path)
- `voice_m5_llm_release` (lock before the `send_and_recv_one_frame`)
- `voice_m5_llm_set_baud` (lock for the full negotiation including the verify-loop)
- `voice_m5_llm_recover_baud` (lock for the full recovery)
- `voice_m5_llm_tts` (lock before `do_tts_setup`)
- `voice_m5_llm_chain_setup` (lock for the four sequential setups)
- `voice_m5_llm_chain_teardown` (lock for the four sequential exits)

- [ ] **Step 3: Wrap `voice_m5_llm_chain_run` per-iteration**

`chain_run` runs up to 10 minutes;  holding the lock for the full duration would starve any concurrent text turn.  Take/release per outer-loop iteration:

```c
esp_err_t voice_m5_llm_chain_run(voice_m5_chain_handle_t *handle, ...) {
   /* ... existing input checks ... */

   while (!(stop_flag != NULL && *stop_flag) && esp_timer_get_time() < deadline_us) {
      M5_LOCK_OR_RETURN(5000);
      /* refill rx_buf until we have at least one \n */
      char *nl = memchr(s_rx_buf, '\n', s_rx_len);
      while (nl == NULL && s_rx_len < M5_RX_BUF_BYTES - 1 && !(stop_flag != NULL && *stop_flag) &&
             esp_timer_get_time() < deadline_us) {
         int n = tab5_port_c_recv(s_rx_buf + s_rx_len, M5_RX_BUF_BYTES - 1 - s_rx_len, 100);
         if (n > 0) {
            s_rx_len += (size_t)n;
            nl = memchr(s_rx_buf, '\n', s_rx_len);
         }
      }
      if (nl == NULL) { M5_UNLOCK(); continue; }

      /* ... existing frame-parse body unchanged ... */

      m5_stackflow_response_free(&resp);
      M5_UNLOCK();
   }

   heap_caps_free(pcm);
   heap_caps_free(b64_acc);
   /* ... existing return ... */
}
```

The recursive mutex permits text/audio callbacks to themselves call into voice_m5_llm_* functions if needed (none currently do).

- [ ] **Step 4: Build, flash, smoke test**

```bash
cd /home/rebelforce/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build && idf.py -p /dev/ttyACM0 flash
```

Expected:  boot loop NOT triggered, `m5ping` REPL still works, `m5infer` still works, `m5tts hello world` still plays audio.  Verify via:

```bash
sudo adb shell "systemctl restart llm-llm llm-asr llm-tts llm-audio"; sleep 5
# in serial REPL:  m5ping  → expect "[ping] tx=N rx=N error=0"
# in serial REPL:  m5tts hello world  → expect speech through Tab5 speaker
```

- [ ] **Step 5: Commit**

```bash
git add bsp/tab5/uart_port_c.h bsp/tab5/uart_port_c.c main/voice_m5_llm.c
git commit -m "$(cat <<'EOF'
feat(k144): Port-C UART mutex (refs #317)

Closes silent UART-collision class flagged in docs/AUDIT-k144-chain-2026-04-29.md
item #1. Every public voice_m5_llm_* entry point now serialises on a
recursive mutex; long-lived chain_run takes/releases per outer-loop
iteration so a concurrent text turn can land between frames.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 1.3 — `s_rx_len` reset + UART flush on chain entry / teardown

**Files:**
- Modify: `main/voice_m5_llm.c`

- [ ] **Step 1: Reset on chain entry**

In `voice_m5_llm_chain_run`, immediately after `if (ensure_rx_buf() != ESP_OK) return ESP_ERR_NO_MEM;`, add:

```c
   /* Discard any stale bytes left over from a prior session — without
    * this, the first iteration parses garbage as a frame.  See
    * docs/AUDIT-k144-chain-2026-04-29.md item #11. */
   tab5_port_c_flush();
   s_rx_len = 0;
```

- [ ] **Step 2: Flush on teardown**

In `voice_m5_llm_chain_teardown`, before the four `chain_exit_unit` calls, add:

```c
   /* Drain any in-flight publisher frames so the next chain run starts
    * clean.  Same hygiene as on entry. */
   tab5_port_c_flush();
   s_rx_len = 0;
```

- [ ] **Step 3: Build + flash + verify with two consecutive m5chain runs**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
sudo adb shell "systemctl restart llm-llm llm-asr llm-tts llm-audio"; sleep 5
# in serial REPL:  m5chain 30  → exit
# repeat:           m5chain 30  → second run shouldn't see stale frames
```

Expected:  no "chain: dropping frame work_id='asr.NNNN'" lines for unrelated old work_ids on second run.

- [ ] **Step 4: Commit**

```bash
git add main/voice_m5_llm.c
git commit -m "fix(k144): flush UART RX + reset s_rx_len on chain entry/teardown (refs #317)"
```

### Task 1.4 — `s_chain_active` ordering fix

**Files:**
- Modify: `main/voice.c`

- [ ] **Step 1: Reorder teardown in `voice_m5_chain_drain_task`**

Find the block (currently at `voice.c:3717-3729`):

```c
   voice_m5_llm_chain_teardown(h);
   s_chain_handle = NULL;
   s_chain_active = false;
   chain_free_buffers();

   voice_set_state(VOICE_STATE_READY, NULL);
```

Change to:

```c
   voice_m5_llm_chain_teardown(h);
   chain_free_buffers();
   s_chain_handle = NULL;
   s_chain_active = false;       /* gate flips LAST so re-entry sees freed buffers */

   voice_set_state(VOICE_STATE_READY, NULL);
```

- [ ] **Step 2: Build + smoke test (rapid double-tap doesn't crash)**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
# manually verify on device: rapid double-tap mic in chat under vmode=4
```

- [ ] **Step 3: Commit**

```bash
git add main/voice.c
git commit -m "fix(k144): order s_chain_active=false after chain_free_buffers (refs #317)"
```

### Task 1.5 — Wire `voice_m5_chain_is_active` into `voice_send_text`

**Files:**
- Modify: `main/voice.c`

- [ ] **Step 1: Refuse text-while-chain-active**

Find the existing vmode=4 short-circuit at `voice.c:3787` (the `if (tab5_settings_get_voice_mode() == VMODE_LOCAL_ONBOARD)` block).  Add a guard at the top:

```c
    if (tab5_settings_get_voice_mode() == VMODE_LOCAL_ONBOARD) {
       /* Audit #3: chain owns the K144 LLM unit while active.  A
        * concurrent voice_failover_schedule would race the chain on the
        * same UART (now mutex-protected, but still produces a duplicate
        * reply).  Refuse cleanly with a user-readable toast. */
       if (s_chain_active) {
          if (tab5_ui_try_lock(100)) {
             ui_home_show_toast("Stop onboard chat first to send text");
             tab5_ui_unlock();
          }
          return ESP_ERR_INVALID_STATE;
       }

       esp_err_t fe = voice_failover_schedule(text);
       /* ... rest of existing block unchanged ... */
    }
```

- [ ] **Step 2: Build, flash, verify**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
TOKEN="..."
# Drive: vmode=4, navigate to chat, tap mic to start chain, then POST /chat with text.
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/mode?m=4"
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/touch -d '{"x":94,"y":1114,"action":"tap"}'
sleep 3
# Now try to send text — should get refused
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/chat -d '{"text":"hi there"}'
```

Expected:  toast "Stop onboard chat first to send text"; no second TINKER bubble appears.

- [ ] **Step 3: Commit + push + open PR**

```bash
git add main/voice.c
git commit -m "fix(k144): refuse voice_send_text while chain active (refs #317)"
git push
# Open PR if not already open:
gh pr create --title "feat(k144): UART mutex + chain state hygiene + double-fire fix" \
  --body "Wave 1 of chain-hardening — closes audit items #1, #3, #11, #12, #15."
```

### Wave 1 acceptance

- [ ] `m5ping`, `m5infer`, `m5tts`, `m5chain` REPL commands all still work after rebase
- [ ] Second `m5chain` after first cleanly exits — no stale-frame log spam at start
- [ ] Tap-mic + `/chat` text + tap-mic-again sequence does NOT produce duplicate TINKER bubbles
- [ ] No boot regressions (heap_wd shows internal SRAM healthy)

---

# Wave 2 — Mode awareness + privacy polish

**Goal:**  Make vmode=4 visible in the chat header chip + reachable via the mode-cycle long-press, respect mic-mute under the chain branch, and suppress "NO DRAGON" home pill when chain is happy.

**PR title:** `feat(k144): vmode=4 mode-cycle + chip + privacy polish (refs #317)`

**Files modified:**
- `main/config.h` — `VOICE_MODE_COUNT` 4→5
- `main/chat_header.c` + `chat_header.h` — grow arrays, drop clamp, `TH_MODE_ONBOARD`
- `main/ui_chat.c` — drop `if (m > 3)` clamp, grow `names[]` toast array
- `main/ui_home.c` — grow mode-cycle arrays, mode-pill rendering
- `main/chat_suggestions.c` — grow per-mode suggestions array
- `main/ui_theme.h` — add `TH_MODE_ONBOARD` color token
- `main/voice.c` — mic-mute respect in chain branch
- `main/ui_home.c` — suppress degraded-status pill when `vmode==4 && chain_active`

### Task 2.1 — Bump `VOICE_MODE_COUNT` and grow `[4]` arrays

**Files:**
- Modify: `main/config.h:53`
- Modify: `main/chat_header.c:25-28`
- Modify: `main/chat_header.c:215`
- Modify: `main/ui_chat.c:166`
- Modify: `main/ui_chat.c:217-227`
- Modify: `main/ui_home.c:153-156, 1545`
- Modify: `main/chat_suggestions.c:52`

- [ ] **Step 1: Bump config**

In `main/config.h`, find the `VOICE_MODE_COUNT` define (around line 53) and change `4` to `5`.

- [ ] **Step 2: Add `TH_MODE_ONBOARD` color**

In `main/ui_theme.h`, find the `TH_MODE_*` palette defines.  Add a new entry that visually distinguishes from existing ones (e.g., violet `0x8E5BFF` — between amber-Hybrid and rose-Claw).  Match the documentation/exact hex against existing palette tokens for consistency.

- [ ] **Step 3: Grow `s_mode_short` and `s_mode_tint` arrays in `chat_header.c`**

Replace the `[4]` arrays with `[5]` versions:

```c
static const char *s_mode_short[5] = { "Local", "Hybrid", "Cloud", "Claw", "Onboard" };
static const uint32_t s_mode_tint[5] = {
    TH_MODE_LOCAL, TH_MODE_HYBRID, TH_MODE_CLOUD, TH_MODE_CLAW, TH_MODE_ONBOARD,
};
```

In `chat_header_set_mode` (around line 215), remove the clamp:

```c
- if (m > 3) m = 0;
+ if (m >= VOICE_MODE_COUNT) m = 0;
```

- [ ] **Step 4: Grow `paint_header_and_view_for_mode` in `ui_chat.c:166`**

Same pattern — drop `if (m > 3) m = 0;`, replace with `if (m >= VOICE_MODE_COUNT) m = 0;`.

- [ ] **Step 5: Grow toast names array**

In `ui_chat.c:217` (the `on_mode_lp` toast):

```c
- static const char *names[4] = { "Local", "Hybrid", "Cloud", "Claw" };
+ static const char *names[5] = { "Local", "Hybrid", "Cloud", "Claw", "Onboard" };
```

Same edit at `ui_home.c:1545` (home mode chip cycle).

- [ ] **Step 6: Grow chat suggestions array**

In `chat_suggestions.c:52`, find the `[4]` per-mode suggestion array.  Add a 5th entry — Onboard mode can reuse Local's suggestions for now.

- [ ] **Step 7: Grow `mode_names` in ui_home.c**

`ui_home.c:153, 156` — same pattern.

- [ ] **Step 8: Build + flash + verify**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
TOKEN="..."
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/mode?m=4"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=chat"
# Expect chat header chip shows "● Onboard" (with new violet tint), not "● Local"
curl -s -H "Authorization: Bearer $TOKEN" -o /tmp/chat-header.bmp http://192.168.1.90:8080/screenshot
# Visually inspect /tmp/chat-header.bmp
```

- [ ] **Step 9: Commit**

```bash
git add main/config.h main/chat_header.c main/chat_header.h main/ui_chat.c main/ui_home.c \
        main/chat_suggestions.c main/ui_theme.h
git commit -m "fix(k144): expose vmode=4 in chat header + mode-cycle (refs #317)"
```

### Task 2.2 — Mic-mute respect under chain branch

**Files:**
- Modify: `main/voice.c:3055-3065`

- [ ] **Step 1: Add mute check inside the chain short-circuit**

```c
    if (tab5_settings_get_voice_mode() == VMODE_LOCAL_ONBOARD) {
        if (tab5_settings_get_mic_mute()) {
            if (tab5_ui_try_lock(100)) {
                ui_home_show_toast("Mic muted — onboard mode also uses a mic");
                tab5_ui_unlock();
            }
            return ESP_ERR_INVALID_STATE;
        }
        if (s_chain_active) {
            return voice_m5_chain_stop();
        }
        return voice_m5_chain_start();
    }
```

- [ ] **Step 2: Verify**

```bash
TOKEN="..."
# Set mic_mute, switch to vmode=4, tap mic — should refuse with toast
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/audio -d '{"mic_mute":true}'
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/mode?m=4"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=chat"
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/touch -d '{"x":94,"y":1114,"action":"tap"}'
# Expect toast "Mic muted — onboard mode also uses a mic"
```

- [ ] **Step 3: Commit**

```bash
git add main/voice.c
git commit -m "fix(k144): respect mic_mute under vmode=4 chain branch (refs #317)"
```

### Task 2.3 — Suppress degraded-status pill in healthy onboard mode

**Files:**
- Modify: `main/ui_home.c:958-981` (or wherever `voice_get_degraded_reason` is consumed for the pill)

- [ ] **Step 1: Gate the pill**

Find the home-screen status block that renders Dragon connectivity.  Add a guard:

```c
    if (tab5_settings_get_voice_mode() == VMODE_LOCAL_ONBOARD &&
        voice_m5_failover_state() == 2 /* M5_FAIL_READY */) {
        /* Onboard mode + K144 healthy — Dragon connectivity is irrelevant.
         * Show a calm "ONBOARD" word instead of nagging about WS state. */
        lv_label_set_text(s_status_label, "ONBOARD");
        return;
    }

    /* ... existing degraded-reason rendering ... */
```

(Adapt to actual element/style of the home-screen status.)

- [ ] **Step 2: Verify**

```bash
# In vmode=4 with K144 warm:  unplug Dragon (or block port 3502).  Home pill
# should show "ONBOARD", not "NO DRAGON" / "Reconnecting…".
TOKEN="..."
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/mode?m=4"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=home"
curl -s -H "Authorization: Bearer $TOKEN" -o /tmp/home.bmp http://192.168.1.90:8080/screenshot
```

- [ ] **Step 3: Commit + push + PR**

```bash
git add main/ui_home.c
git commit -m "fix(k144): suppress degraded-WS pill when vmode=4 + K144 healthy (refs #317)"
git push
gh pr create --title "feat(k144): vmode=4 mode-cycle + chip + privacy polish" \
  --body "Wave 2 — closes audit items #5, #6, #9, #10."
```

### Wave 2 acceptance

- [ ] Chat-header chip displays "● Onboard" (with new violet tint) under vmode=4
- [ ] Long-press on chat chip cycles through Local→Hybrid→Cloud→Claw→Onboard→Local
- [ ] Same cycle works on home-screen mode pill
- [ ] Mic-mute prevents chain start with clear toast
- [ ] Home pill says "ONBOARD" (not "NO DRAGON") when in vmode=4 with K144 ready

---

# Wave 3 — TTS Eigen workaround + chain navigation lifecycle

**Depends on:** Wave 1 mutex (the per-utterance TTS synth call shares UART with chain_run, mutex required).

**Goal:**  Replace chain's `tts.setup` with per-utterance one-shot `voice_m5_llm_tts` synth on LLM `finish=true` (works around the documented K144 SummerTTS Eigen crash).  Stop the chain when user navigates away from chat overlay.  Stream LLM tokens into the in-progress bubble instead of commit-only-on-finish.

**Two PRs:**

### PR 3a — TTS Eigen workaround

**PR title:** `feat(k144): per-utterance TTS via voice_m5_llm_tts (refs #317)`

**Files modified:**
- `main/voice_m5_llm.c` — strip `tts.setup` from `voice_m5_llm_chain_setup`; remove the `is_tts` branch from chain_run dispatch (no chain TTS unit anymore)
- `main/voice.c` — in `chain_text_callback`, on `from_llm && finish`, enqueue a worker job that calls `voice_m5_llm_tts` + plays through `tab5_audio_play_raw`

#### Task 3a.1 — Strip `tts.setup` from chain

- [ ] **Step 1: Remove the 4th setup stage**

In `voice_m5_llm_chain_setup`, delete the `tts.setup` block (around lines 884-895):

```c
    /* DELETE this block:
       cJSON *d = cJSON_CreateObject();
       cJSON_AddStringToObject(d, "model", M5_TTS_MODEL);
       cJSON_AddStringToObject(d, "response_format", "tts.base64.wav");
       cJSON *inp = cJSON_CreateArray();
       cJSON_AddItemToArray(inp, cJSON_CreateString(h->llm_id));
       cJSON_AddItemToObject(d, "input", inp);
       cJSON_AddBoolToObject(d, "enoutput", true);
       cJSON_AddBoolToObject(d, "enkws", false);
       err = chain_setup_unit("tts", "tts.setup", d, h->tts_id, sizeof(h->tts_id), M5_SETUP_TIMEOUT_MS);
       if (err != ESP_OK) goto fail;
    */
```

The `tts_id` field of the handle stays (zero-initialised) so the existing teardown path's `chain_exit_unit(handle->tts_id)` is a safe no-op.

- [ ] **Step 2: Update the chain-ready log**

Adjust the log in `voice_m5_llm_chain_setup` to reflect 3 units (audio/asr/llm) plus mention TTS is per-utterance.

- [ ] **Step 3: Build, flash, verify chain still spins up**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
sudo adb shell "systemctl restart llm-llm llm-asr llm-tts llm-audio"; sleep 5
# m5chain 30 — should bring up audio/asr/llm only; no tts subscription created on K144
sudo adb shell "journalctl -u llm-tts -n 5 --since '1 minute ago'"
# Expect:  no new tts.setup call from us during chain
```

- [ ] **Step 4: Commit**

```bash
git add main/voice_m5_llm.c
git commit -m "fix(k144): strip tts.setup from chain to dodge SummerTTS Eigen crash (refs #317)"
```

#### Task 3a.2 — Per-utterance synth on LLM finish

- [ ] **Step 1: Add a worker-job helper in voice.c**

Below `voice_m5_failover_text_job`, add a TTS-per-utterance variant:

```c
static void voice_m5_chain_tts_job(void *arg) {
   char *text = (char *)arg;
   if (text == NULL) return;
   /* PSRAM scratch — 8 sec @ 16 kHz mono = 128 KB samples. */
   const size_t cap = 16 * 8 * 1024;
   int16_t *pcm16 = heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (pcm16 == NULL) { free(text); return; }

   size_t got = 0;
   esp_err_t te = voice_m5_llm_tts(text, pcm16, cap, &got, 30);
   if (te == ESP_OK && got > 0) {
      /* Upsample 1:3 → 48 kHz mono for tab5_audio_play_raw. */
      const size_t cap48 = got * 3;
      int16_t *pcm48 = heap_caps_malloc(cap48 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (pcm48 != NULL) {
         for (size_t i = 0; i < got; i++) {
            const int16_t cur = pcm16[i];
            const int16_t nxt = (i + 1 < got) ? pcm16[i + 1] : cur;
            for (int j = 0; j < 3; j++) {
               pcm48[i * 3 + j] = (int16_t)(cur + (int32_t)(nxt - cur) * j / 3);
            }
         }
         tab5_audio_play_raw(pcm48, cap48);
         heap_caps_free(pcm48);
      }
   } else {
      ESP_LOGW(TAG, "chain TTS failed (%s) for '%s'", esp_err_to_name(te), text);
   }
   heap_caps_free(pcm16);
   free(text);
}
```

- [ ] **Step 2: Hook into `chain_text_callback`**

After the `if (finish && *len > 0) { ... ui_chat_add_message ... }` block, when `from_llm`, enqueue:

```c
   if (finish && from_llm) {
      char *copy = strdup(buf);  /* freed by voice_m5_chain_tts_job */
      if (copy != NULL) {
         if (tab5_worker_enqueue(voice_m5_chain_tts_job, copy, "chain_tts") != ESP_OK) {
            free(copy);
         }
      }
   }
```

(The `*len = 0; buf[0] = '\0';` reset already runs after the bubble push;  duplicate the strdup before resetting.  Reorder if needed.)

- [ ] **Step 3: Build, flash, live verify**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
sudo adb shell "systemctl restart llm-llm llm-asr llm-tts llm-audio"; sleep 5
TOKEN="..."
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/mode?m=4"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=chat"
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/touch -d '{"x":94,"y":1114,"action":"tap"}'
# Speak at the K144.  Expect ASR transcribe + LLM reply + AUDIO PLAYS through Tab5 speaker.
```

- [ ] **Step 4: Commit + push + PR**

```bash
git add main/voice.c
git commit -m "feat(k144): per-utterance TTS via voice_m5_llm_tts on LLM finish (refs #317)"
git push
gh pr create --title "feat(k144): TTS Eigen workaround — per-utterance one-shot synth" \
  --body "Wave 3a — closes audit item #16. Depends on Wave 1 UART mutex."
```

### PR 3b — Chain navigation lifecycle + LLM streaming bubble

**PR title:** `feat(k144): tear down chain on chat hide; stream LLM into bubble (refs #317)`

**Files modified:**
- `main/ui_chat.c:618-647` — `ui_chat_hide()` calls `voice_m5_chain_stop()` when active
- `main/voice.c` — `chain_text_callback` for `from_llm` deltas mutates the in-progress bubble instead of waiting for finish

#### Task 3b.1 — Stop chain on chat hide

- [ ] **Step 1: Hook into `ui_chat_hide`**

Top of `ui_chat_hide` (`main/ui_chat.c:618`), add:

```c
    extern bool voice_m5_chain_is_active(void);
    extern esp_err_t voice_stop_listening(void);
    if (voice_m5_chain_is_active()) {
        voice_stop_listening();   /* dispatches to voice_m5_chain_stop in vmode=4 */
    }
```

- [ ] **Step 2: Verify**

```bash
TOKEN="..."
# Drive: vmode=4 → chat → tap mic → wait 2s → navigate home.  Expect chain stops.
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=chat"
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/touch -d '{"x":94,"y":1114,"action":"tap"}'
sleep 3
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=home"
sleep 3
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.1.90:8080/voice
# Expect state_name = READY (not LISTENING)
```

- [ ] **Step 3: Commit**

```bash
git add main/ui_chat.c
git commit -m "fix(k144): stop chain on chat hide / navigate-away (refs #317)"
```

#### Task 3b.2 — LLM streaming-into-bubble

- [ ] **Step 1: Refactor `chain_text_callback` to stream LLM**

This needs care because `ui_chat_add_message` creates a NEW bubble each call.  Likely there's an existing `chat_msg_view_begin_streaming` / `_refresh` / `_end_streaming` path used by Dragon's `poll_voice` (`ui_chat.c:407-460`).  Audit it first:

```bash
grep -n "stream" main/chat_msg_view.{c,h} main/chat_msg_store.{c,h} 2>/dev/null
```

Use the same lifecycle:  on first LLM delta of a turn, begin a streaming bubble; on each subsequent delta, refresh; on `finish=true`, end-stream + commit.

```c
   if (from_llm) {
      if (!s_chain_llm_streaming) {
         /* first delta of this turn — open a streaming bubble */
         if (tab5_ui_try_lock(150)) {
            chat_msg_view_begin_streaming("tinker");  /* exact API per audit */
            tab5_ui_unlock();
         }
         s_chain_llm_streaming = true;
      }
      if (tab5_ui_try_lock(150)) {
         chat_msg_view_refresh_streaming(buf);
         tab5_ui_unlock();
      }
      if (finish) {
         if (tab5_ui_try_lock(150)) {
            chat_msg_view_end_streaming();
            tab5_ui_unlock();
         }
         s_chain_llm_streaming = false;
         /* ... existing strdup-and-enqueue voice_m5_chain_tts_job ... */
         *len = 0;
         buf[0] = '\0';
      }
      return;
   }

   /* ASR keeps the existing replace-on-delta + commit-on-finish behaviour */
```

(Adapt the `chat_msg_view_*` API names to what actually exists in your `chat_msg_view.{c,h}` — read those headers first.)

- [ ] **Step 2: Verify**

Live test:  in vmode=4 chain, a multi-token LLM reply should appear character-by-character in a single TINKER bubble (matching Dragon mode's streaming UX).

- [ ] **Step 3: Commit + push + PR**

```bash
git add main/voice.c
git commit -m "feat(k144): stream LLM tokens into chat bubble (refs #317)"
git push
gh pr create --title "feat(k144): chain navigation lifecycle + LLM streaming bubble" \
  --body "Wave 3b — closes audit items #2, #8."
```

### Wave 3 acceptance

- [ ] Chain TTS audio plays through Tab5's speaker (PR 3a) — proves the Eigen workaround works
- [ ] Navigating from chat to home stops the chain (PR 3b)
- [ ] LLM reply text streams into a single TINKER bubble (PR 3b), matching Dragon's UX
- [ ] No K144-side `llm-tts` Eigen crashes during chain (verify with `journalctl -u llm-tts`)

---

# Wave 4 — split into 4a (shipped) + 4b (deferred)

**Wave 4a (shipped commit `bc1012f`):**  stop_flag plumbing through
`chain_setup_unit` + chain-aware tap in `on_ball_tap`.  Closes audit #13.

**Wave 4b (deferred to follow-up PR):**  full architecture extract of
`main/voice_onboard.{c,h}`.  ~600 lines moved out of voice.c (chain +
failover state + lifecycle).  Defers because:
  - voice.c is at 4,243 lines; mixing extract with hardening risks
    breaking working code under one squash-merge
  - The extract is decoupled from waves 1-3 fixes; can land any time
  - Worth its own focused review as a refactor PR

**Spec for the deferred extract** (kept here so the next session has it):

**Goal:** Extract chain + failover state and lifecycle into a dedicated module.  voice.c shrinks back below 3.7K lines; chain feature has clear ownership for the next iteration.

**PR title:** `refactor(k144): extract voice_onboard module (refs #317)`

**Files created:**
- `main/voice_onboard.h` — public API
- `main/voice_onboard.c` — moved chain + failover state and functions

**Files modified:**
- `main/voice.c` — chain & failover sections deleted; hooks reduced to 2-line short-circuits calling into `voice_onboard_*`
- `main/voice.h` — declarations of `voice_m5_failover_state`, `voice_m5_failover_start_warmup`, `voice_m5_chain_is_active` move to `voice_onboard.h` (kept as forwarding declarations for callers if needed)
- `main/main.c` — `voice_m5_failover_start_warmup` call → `voice_onboard_start_warmup`
- `main/CMakeLists.txt` — register new file

### Task 4.1 — Create voice_onboard.h

**Files:**
- Create: `main/voice_onboard.h`

- [ ] **Step 1: Define the API surface**

```c
/**
 * @file voice_onboard.h
 * @brief Tab5-side glue for the K144 LLM Module (vmode=4 / Onboard).
 *
 * Owns:
 *   - Boot warm-up (probe + one-shot hi-infer to map NPU model)
 *   - Per-text-turn failover (Dragon-down or vmode=4 always-route)
 *   - Autonomous voice-assistant chain (mic → ASR → LLM → per-utterance TTS)
 *   - Chain integration with chat overlay (mic-tap toggle)
 *
 * Composition (DIP):
 *   - voice_m5_llm  ── K144 control plane (UART, StackFlow JSON)
 *   - voice_onboard ── this file: lifecycle + UI binding
 *   - voice         ── routes to us when tab5_settings_get_voice_mode() == 4
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Boot-time: schedule the warm-up job.  Idempotent. */
esp_err_t voice_onboard_start_warmup(void);

/** Failover gate (0=UNKNOWN, 1=PROBING, 2=READY, 3=UNAVAILABLE). */
int voice_onboard_failover_state(void);

/** Schedule a text turn through the K144 (one-shot infer + chat bubble).
 *  Used by voice_send_text in vmode=4 and as Dragon-down failover in
 *  vmode=0.  Returns ESP_OK if the job was scheduled. */
esp_err_t voice_onboard_send_text(const char *text);

/** Chain start (called by voice_start_listening when vmode=4 + chain not
 *  active).  Spawns the drain task; returns immediately. */
esp_err_t voice_onboard_chain_start(void);

/** Chain stop (called by voice_stop_listening when chain is active).
 *  Sets stop_flag; the drain task tears down on its next loop. */
esp_err_t voice_onboard_chain_stop(void);

/** True while the chain drain task is alive. */
bool voice_onboard_chain_active(void);

#ifdef __cplusplus
}
#endif
```

### Task 4.2 — Create voice_onboard.c with moved bodies

**Files:**
- Create: `main/voice_onboard.c`

- [ ] **Step 1: Move all `s_m5_*` and `s_chain_*` static state from voice.c**

Cut from `voice.c:299-335` (failover state) and `voice.c:3441-3700` (warmup, failover_text_job, failover_schedule, chain callbacks, drain task, chain_start, chain_stop).  Paste into `voice_onboard.c` with an `#include "voice_m5_llm.h"`, `#include "task_worker.h"`, etc.

Rename external entry points to the new prefix:
- `voice_m5_failover_start_warmup` → `voice_onboard_start_warmup`
- `voice_m5_failover_state` → `voice_onboard_failover_state`
- `voice_failover_schedule` → static `onboard_failover_schedule` (local helper) + new `voice_onboard_send_text` exposed
- `voice_m5_chain_start/stop/is_active` → `voice_onboard_chain_*`

### Task 4.3 — Update voice.c hooks

- [ ] **Step 1: Reduce voice_send_text vmode=4 short-circuit**

```c
    if (tab5_settings_get_voice_mode() == VMODE_LOCAL_ONBOARD) {
        return voice_onboard_send_text(text);
    }
```

- [ ] **Step 2: Reduce voice_start_listening vmode=4 short-circuit**

```c
    if (tab5_settings_get_voice_mode() == VMODE_LOCAL_ONBOARD) {
        if (voice_onboard_chain_active()) return voice_onboard_chain_stop();
        return voice_onboard_chain_start();
    }
```

- [ ] **Step 3: Reduce voice_stop_listening short-circuit**

```c
    if (voice_onboard_chain_active()) {
        return voice_onboard_chain_stop();
    }
```

### Task 4.4 — Update main.c boot wiring + CMakeLists

- [ ] **Step 1: Update main.c**

```c
- voice_m5_failover_start_warmup();
+ voice_onboard_start_warmup();
```

- [ ] **Step 2: Add voice_onboard.c to CMakeLists.txt**

```cmake
idf_component_register(SRCS
    ...
    "voice.c"
    "voice_onboard.c"   # NEW
    ...
)
```

### Task 4.5 — Add stop_flag plumbing to chain_setup_unit (audit #13)

**Files:**
- Modify: `main/voice_m5_llm.h` — add stop_flag param to chain_setup
- Modify: `main/voice_m5_llm.c` — thread stop_flag through chain_setup_unit
- Modify: `main/voice_onboard.c` — pass `&s_chain_stop_flag` into setup

- [ ] **Step 1: Modify signature**

```c
esp_err_t voice_m5_llm_chain_setup(voice_m5_chain_handle_t **out_handle,
                                   volatile bool *stop_flag);
```

- [ ] **Step 2: Pass stop_flag to chain_setup_unit's deadline check**

In the inner refill loop in `chain_setup_unit`, add:

```c
   while (esp_timer_get_time() < deadline_us &&
          !(stop_flag != NULL && *stop_flag)) {
      ...
   }
```

Bail with `ESP_ERR_INVALID_STATE` if stop_flag fires mid-setup.

- [ ] **Step 3: voice_onboard_chain_drain_task passes the flag**

```c
   esp_err_t e = voice_m5_llm_chain_setup(&h, &s_chain_stop_flag);
```

- [ ] **Step 4: Build + flash + verify**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
TOKEN="..."
# In vmode=4, tap mic to start, then RAPIDLY tap mic to stop within 2 sec
# (during NPU cold-start).  Expect chain bails fast, not 15+ sec wait.
```

- [ ] **Step 5: Commit**

### Task 4.6 — Push + PR

- [ ] **Step 1: Final build verification**

```bash
idf.py build 2>&1 | grep -E "error:|tinkertab.bin" | head
wc -l main/voice.c main/voice_onboard.c
# voice.c should drop to ~3700 lines; voice_onboard.c around 600
```

- [ ] **Step 2: Run e2e harness for regression check**

```bash
cd ~/projects/TinkerTab
export TAB5_TOKEN=...
python3 tests/e2e/runner.py story_smoke
# Expect 14/14 pass
```

- [ ] **Step 3: Commit + push + PR**

```bash
git add main/voice_onboard.h main/voice_onboard.c main/voice.c main/voice.h \
        main/main.c main/CMakeLists.txt main/voice_m5_llm.c main/voice_m5_llm.h
git commit -m "refactor(k144): extract voice_onboard module (refs #317)"
git push
gh pr create --title "refactor(k144): extract voice_onboard module" \
  --body "Wave 4 — closes audit items #7, #13, #14, P2-Arch-2.2."
```

### Wave 4 acceptance

- [ ] voice.c LOC drops to <3700
- [ ] voice_onboard.c contains ~600 lines, single-cohesion
- [ ] Chain stop during NPU cold-start bails within ~200ms (not 15+ sec)
- [ ] e2e smoke story still passes 14/14
- [ ] All Wave 1-3 functionality unchanged

---

# Wave 5 — Observability + e2e tests

**Goal:** Add a `/m5` debug endpoint exposing chain + failover state, observability events for the chain lifecycle, and an e2e story that exercises vmode=4 text path.

**PR title:** `feat(k144): /m5 debug endpoint + obs events + story_onboard (refs #317)`

**Files modified:**
- `main/debug_server.c` — register `/m5` endpoint returning JSON snapshot
- `main/debug_obs.{c,h}` — register new event kinds (no code change required if generic kind+detail; just document)
- `main/voice_onboard.c` — emit `tab5_debug_obs_event()` calls at key transitions
- `tests/e2e/scenarios/story_onboard.py` — new scenario

### Task 5.1 — `/m5` debug endpoint

**Files:**
- Modify: `main/debug_server.c`

- [ ] **Step 1: Add handler**

```c
static esp_err_t m5_status_handler(httpd_req_t *req)
{
    if (!debug_check_auth(req)) return ESP_FAIL;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "failover_state", voice_onboard_failover_state());
    cJSON_AddBoolToObject(o, "chain_active", voice_onboard_chain_active());
    cJSON_AddNumberToObject(o, "uart_baud", voice_m5_llm_get_baud());
    /* Optional: probe round-trip latency, last_chain_started_ms, etc. */

    char *json = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(o);
    return ESP_OK;
}
```

Register near other voice URI handlers:

```c
    httpd_uri_t m5_uri = { .uri = "/m5", .method = HTTP_GET, .handler = m5_status_handler };
    httpd_register_uri_handler(server, &m5_uri);
```

- [ ] **Step 2: Verify**

```bash
TOKEN="..."
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.1.90:8080/m5 | python3 -m json.tool
# Expect:  {"failover_state": 2, "chain_active": false, "uart_baud": 115200}
```

### Task 5.2 — Obs events on chain lifecycle

- [ ] **Step 1: Emit events**

In `voice_onboard.c`, at key transitions:

```c
   tab5_debug_obs_event("m5.warmup", "start");
   ...
   tab5_debug_obs_event("m5.warmup", ie == ESP_OK ? "ready" : "unavailable");
   ...
   tab5_debug_obs_event("m5.chain", "start");
   ...
   tab5_debug_obs_event("m5.chain", "stop");
```

Match the 32-char kind / 48-char detail limits documented in CLAUDE.md "Observability events".

- [ ] **Step 2: Verify**

```bash
curl -s -H "Authorization: Bearer $TOKEN" "http://192.168.1.90:8080/events?since=0" | grep m5
```

### Task 5.3 — story_onboard e2e

**Files:**
- Create: `tests/e2e/scenarios/story_onboard.py`

- [ ] **Step 1: Scenario skeleton**

```python
"""Onboard mode (vmode=4 / K144) lifecycle smoke test.

Doesn't drive K144 mic (harness can't speak).  Exercises:
  - mode switch to 4
  - text path via /chat (routes through voice_onboard_send_text)
  - chain start via mic-tap (assert state transitions only)
  - chain stop via second mic-tap

Skips if voice_onboard_failover_state() != 2 (READY).
"""

def run(tab5):
    # Pre-check K144 is warm
    m5 = tab5.get("/m5").json()
    if m5.get("failover_state") != 2:
        return tab5.skip(f"K144 not ready (failover_state={m5.get('failover_state')})")

    # Switch to vmode=4
    tab5.mode(4)
    tab5.await_screen("home", 5)

    # Send text turn — should produce a TINKER bubble
    tab5.navigate("chat")
    tab5.chat("hello onboard")
    # No await_llm_done (event is for Dragon path); poll /m5 for chain_active=false then check chat
    tab5.await_event("m5.chain", timeout_s=60, detail_match="stop")  # may not fire for text-only

    # Chain lifecycle: tap mic, expect transitions
    tab5.tap(94, 1114)
    tab5.await_voice_state("PROCESSING", 10)  # chain setup
    tab5.await_voice_state("LISTENING", 30)   # chain ready
    tab5.tap(94, 1114)
    tab5.await_voice_state("READY", 5)        # chain stop
```

- [ ] **Step 2: Add to runner story registry**

Edit `tests/e2e/runner.py` to register `story_onboard`.

- [ ] **Step 3: Verify**

```bash
cd ~/projects/TinkerTab
python3 tests/e2e/runner.py story_onboard
```

### Task 5.4 — Push + PR

```bash
git add main/debug_server.c main/voice_onboard.c tests/e2e/scenarios/story_onboard.py \
        tests/e2e/runner.py
git commit -m "feat(k144): /m5 debug endpoint + obs events + story_onboard (refs #317)"
git push
gh pr create --title "feat(k144): observability + e2e for vmode=4" \
  --body "Wave 5 — closes audit items #17, #18."
```

### Wave 5 acceptance

- [ ] `GET /m5` returns valid JSON with chain_active, failover_state, baud
- [ ] `m5.warmup`, `m5.chain` obs events visible in `/events?since=0`
- [ ] `story_onboard` passes (or skips cleanly if K144 not ready)

---

## Self-review

**Spec coverage:** Each audit P0/P1 from the audit doc has a task or is explicitly listed in the P2-backlog section of the audit.  ✓

**Placeholder scan:** I left "Adapt to actual element/style of the home-screen status" in Wave 2 Task 2.3 because the exact LVGL widget there is one of: a label inside a status pill, the home-pill itself, or a child of `s_home_status_label`.  The implementer should `grep -n "voice_get_degraded_reason" main/ui_home.c` first to find the exact site.  Same for Wave 3b `chat_msg_view_*` API — implementer reads the header first.  These are **not** placeholders for missing logic; they're "look up the project-specific helper name" instructions, which is appropriate given the writing-plans skill's "assume the engineer doesn't know our codebase" stance.

**Type consistency:** All function names cross-checked between waves.  `voice_m5_chain_is_active` is renamed to `voice_onboard_chain_active` in Wave 4 — earlier waves call the old name; the rename is part of Wave 4's diff and the call sites (`ui_chat_hide` in Wave 3b) get updated then.  ✓
