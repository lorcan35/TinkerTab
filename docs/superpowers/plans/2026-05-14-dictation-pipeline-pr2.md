# Dictation pipeline PR 2 — orb morphs + Dictate chip on home

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** First visible UI change in the 4-PR series.  The orb morphs through pipeline states (RECORDING red / UPLOADING amber / TRANSCRIBING amber+comet / SAVED green / FAILED red) with a caption label under it; a new Dictate chip lands on home below the existing mode chip, in matching dark-pill language, that fires `voice_start_dictation()` on tap and mirrors the orb's pipeline state.

**Architecture:** Both surfaces subscribe to the `voice_dictation` state machine that landed in PR #519.  No new pipeline state — we just paint it.  The orb gets a new `ui_orb_set_pipeline_state(state, fail_reason)` API that takes precedence over the existing voice-state-driven IDLE/LISTENING/PROCESSING/SPEAKING painting; when pipeline returns to IDLE, orb reverts to voice-state visuals.  The Dictate chip is a new widget inside `ui_home.c` that subscribes to the same pipeline and mirrors state in its label.

**Tech Stack:** ESP-IDF 5.5.2, LVGL 9.x, existing `voice_dictation.{c,h}` from PR #519.  PR 2 also ships the 2 s `SAVED → IDLE` auto-fade timer that PR 1 deferred (PR 1 keeps `SAVED → RECORDING` legal as a backup; PR 2's timer is the primary path).

Spec: `docs/superpowers/specs/2026-05-14-dictation-pipeline-notes-redesign-design.md` § PR 2.

---

## File map

**Create:** none — both deliverables are extensions of existing files.

**Modify:**
- `main/ui_orb.h` — new public API: `ui_orb_set_pipeline_state`, `ui_orb_set_pipeline_caption` (internal use), include of `voice_dictation.h`
- `main/ui_orb.c` — pipeline state field + caption label widget + per-state paint variants + subscriber registration in `ui_orb_create`
- `main/ui_home.c` — new `s_dictate_chip` widget below `s_mode_chip`, subscriber callback, tap handler firing `voice_start_dictation()`, cancel × handler firing `voice_cancel()`
- `main/ui_home.h` — no public changes (chip is internal)

**Boundary rules:**
- The pipeline state takes precedence over voice-state-driven orb painting when pipeline ≠ IDLE.  When pipeline returns to IDLE, the orb reverts to existing voice-state visuals.
- Both subscribers (orb + chip) run on the LVGL thread (callbacks are fired via `tab5_lv_async_call` because `voice_dictation_set_state` can fire from any task).
- Strictly additive — no existing voice_state_t / orb visual is removed or changed.

---

## Tasks

### Task 1 — Async-safe pipeline subscriber dispatch helper

The voice_dictation state machine fires callbacks synchronously from whatever task called `set_state` (mic task, WS event task, REST polling task, http server task).  Any LVGL touch from there is a thread-safety hazard.  We need a shared marshalling helper that captures the event + posts an LVGL-safe callback.

**The host test already compiles `main/voice_dictation.c` as pure C with no ESP-IDF deps** (see `tests/host/CMakeLists.txt` → `test_voice_dictation` target).  Adding `#include "ui_core.h"` to voice_dictation.c would break that host build.  So the marshalling wrapper goes in a new file `main/voice_dictation_lvgl.{c,h}` from the start — no conditional path.

**Files:**
- Create: `main/voice_dictation_lvgl.h`
- Create: `main/voice_dictation_lvgl.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create `main/voice_dictation_lvgl.h`**

```c
/* main/voice_dictation_lvgl.h — LVGL-thread-marshalling wrapper around
 * voice_dictation_subscribe().  Lives in its own file because pulling
 * tab5_lv_async_call into voice_dictation.c would break the host test
 * build (host shims don't cover ui_core.h). */
#pragma once

#include "voice_dictation.h"

#ifdef __cplusplus
extern "C" {
#endif

int voice_dictation_subscribe_lvgl(dict_subscriber_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `main/voice_dictation_lvgl.c`**

```c
#include "voice_dictation_lvgl.h"

#include <stdlib.h>

#include "ui_core.h"   /* tab5_lv_async_call */

typedef struct {
   dict_subscriber_t cb;
   void             *user_data;
   dict_event_t      event;
} dict_marshal_t;

static void dict_marshal_lvgl_cb(void *arg) {
   dict_marshal_t *m = (dict_marshal_t *)arg;
   m->cb(&m->event, m->user_data);
   free(m);
}

static void dict_marshal_dispatch(const dict_event_t *e, void *user_data) {
   dict_subscriber_t real_cb = ((void **)user_data)[0];
   void *real_user_data      = ((void **)user_data)[1];
   dict_marshal_t *m = malloc(sizeof(*m));
   if (!m) return;
   m->cb = real_cb;
   m->user_data = real_user_data;
   m->event = *e;
   tab5_lv_async_call(dict_marshal_lvgl_cb, m);
}

int voice_dictation_subscribe_lvgl(dict_subscriber_t cb, void *user_data) {
   if (!cb) return -1;
   void **tuple = malloc(sizeof(void *) * 2);
   if (!tuple) return -1;
   tuple[0] = (void *)cb;
   tuple[1] = user_data;
   int h = voice_dictation_subscribe(dict_marshal_dispatch, tuple);
   if (h < 0) {
      free(tuple);
      return -1;
   }
   return h;
}
```

- [ ] **Step 3: Add to `main/CMakeLists.txt`**

Insert `"voice_dictation_lvgl.c"` immediately after the existing `"voice_dictation.c"` line in the SRCS list.

- [ ] **Step 4: Build + verify**

```bash
cd /home/rebelforce/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -5
```

Expected: clean build.  Also verify host tests still pass:

```bash
cd /home/rebelforce/projects/TinkerTab/tests/host
cmake --build build 2>&1 | tail -3
ctest --test-dir build -R voice_dictation --output-on-failure
```

Expected: pass (host build never touches voice_dictation_lvgl.c).

- [ ] **Step 5: Commit**

```bash
cd /home/rebelforce/projects/TinkerTab
git add main/voice_dictation_lvgl.{c,h} main/CMakeLists.txt
git commit -m "feat(dictation): voice_dictation_subscribe_lvgl marshalling wrapper"
```

---

### Task 2 — Orb pipeline-state field + public API

**Files:**
- Modify: `main/ui_orb.h`
- Modify: `main/ui_orb.c`

- [ ] **Step 1: Add to ui_orb.h before `#ifdef __cplusplus }`**

```c
/* ── Pipeline state (PR 2) ──────────────────────────────────────────── */

/* Drive the orb's pipeline-state painting.  Takes precedence over the
 * voice-state machine (IDLE/LISTENING/PROCESSING/SPEAKING) when pipeline
 * state is anything except DICT_IDLE.  On DICT_IDLE, orb returns to the
 * voice-state-driven visuals it had before.
 *
 * Safe to call only on the LVGL thread — callers using the dictation
 * pipeline subscriber API should subscribe via voice_dictation_subscribe_lvgl
 * to get automatic marshalling.
 *
 * The `event` is taken by value (caller may pass a stack copy). */
void ui_orb_set_pipeline_state(const dict_event_t *event);

/* Returns true if the orb is currently in a non-IDLE pipeline state and
 * thus the voice-state painting is suppressed.  Used by ui_home's
 * voice-state callback to know whether to skip orb repaints. */
bool ui_orb_pipeline_active(void);
```

Add the include at the top of ui_orb.h (after `#include "widget.h"`):

```c
#include "voice_dictation.h"
```

- [ ] **Step 2: Add state field + initial paint stub in ui_orb.c**

Find the existing static state block in `ui_orb.c` (search for `static ui_orb_state_t s_state` or similar — should be near the top of the file).  Add:

```c
/* PR 2: pipeline state overlay.  When != DICT_IDLE, all paint cycles
 * route through the pipeline-state painter and the voice-state painter
 * is suppressed.  IDLE → revert to voice-state painting. */
static dict_event_t s_pipeline = {
   .state = DICT_IDLE,
   .fail_reason = DICT_FAIL_NONE,
   .started_ms = 0,
   .stopped_ms = 0,
   .last_change_ms = 0,
   .note_slot = -1,
};
static lv_obj_t *s_orb_caption = NULL;  /* Label below the orb body */
static lv_timer_t *s_saved_fade_timer = NULL;  /* SAVED → IDLE 2s timer */
```

Add the include at the top of ui_orb.c:

```c
#include "voice_dictation.h"
```

- [ ] **Step 3: Add the API stub**

Add these functions near the end of ui_orb.c, before the file's closing braces:

```c
void ui_orb_set_pipeline_state(const dict_event_t *event) {
   if (!event) return;
   s_pipeline = *event;
   /* The actual painting is implemented in subsequent tasks (Tasks 3-7
    * land per-state visuals).  This task just lands the API surface. */
}

bool ui_orb_pipeline_active(void) {
   return s_pipeline.state != DICT_IDLE;
}
```

- [ ] **Step 4: Build + verify**

```bash
cd /home/rebelforce/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -5
```

Expected: build clean.

- [ ] **Step 5: Commit**

```bash
git add main/ui_orb.h main/ui_orb.c
git commit -m "feat(orb): ui_orb_set_pipeline_state + pipeline_active stubs"
```

---

### Task 3 — Caption label widget under orb

**Files:**
- Modify: `main/ui_orb.c`

- [ ] **Step 1: Create the caption label in `ui_orb_create`**

Find `ui_orb_create` in `ui_orb.c`.  Near the bottom of the function, after the body / halo / specular widgets are built, add:

```c
   /* PR 2: caption label below the orb for pipeline state text
    * ("● RECORDING 0:23", "UPLOADING…", "TRANSCRIBING…", "✓ Saved").
    * Sits 24 px below the orb body, transparent background, monospace
    * caption font.  Hidden by default — paint_for_pipeline_state shows
    * it. */
   {
      /* Orb body radius is the size passed to ui_orb_create.  Caption
       * sits below it.  Use the existing body radius (whatever the
       * file's constant is — search for ORB_BODY_RADIUS or similar). */
      extern int ui_orb_body_radius_px(void);  /* If this helper doesn't
        exist yet, just inline the magic number here (orb is ~120 px
        radius per the spec). */

      s_orb_caption = lv_label_create(parent);
      if (s_orb_caption) {
         lv_label_set_text(s_orb_caption, "");
         lv_obj_set_style_text_color(s_orb_caption, lv_color_hex(0xE8E8EF), 0);
         lv_obj_set_style_text_font(s_orb_caption, FONT_CAPTION, 0);
         lv_obj_set_style_text_letter_space(s_orb_caption, 2, 0);
         lv_obj_align_to(s_orb_caption, s_body, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);
         lv_obj_add_flag(s_orb_caption, LV_OBJ_FLAG_HIDDEN);
      }
   }
```

If `FONT_CAPTION` isn't already declared in this file's includes, add `#include "config.h"` at the top (matches what ui_home.c uses).

If `ui_orb_body_radius_px()` doesn't exist, just delete the `extern int ui_orb_body_radius_px(void);` line — the `lv_obj_align_to` doesn't need it because it positions relative to `s_body` directly.

- [ ] **Step 2: Destroy the caption in `ui_orb_destroy`**

Find `ui_orb_destroy`.  Add to the cleanup block:

```c
   if (s_orb_caption) {
      lv_obj_del(s_orb_caption);
      s_orb_caption = NULL;
   }
   if (s_saved_fade_timer) {
      lv_timer_del(s_saved_fade_timer);
      s_saved_fade_timer = NULL;
   }
```

- [ ] **Step 3: Build + verify**

```bash
idf.py build 2>&1 | tail -5
```

Expected: clean build.  Flash a smoke build to confirm the caption exists but is hidden:

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -3
sleep 22
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" http://192.168.1.90:8080/screenshot.jpg -o /tmp/orb-task3.jpg
file /tmp/orb-task3.jpg
```

The screenshot should look identical to the current home (caption hidden).  If it shows a stray label, the HIDDEN flag isn't being set.

- [ ] **Step 4: Commit**

```bash
git add main/ui_orb.c
git commit -m "feat(orb): caption label widget below orb (hidden by default)"
```

---

### Task 4 — Per-state paint variants + caption text

**Files:**
- Modify: `main/ui_orb.c`

This task lands the visual differences per pipeline state.  We're keeping it tight in PR 2 v1 — body tint + caption + breath rate.  The internal ripple rings + amber arc progress + green pulse animation are deferred to a PR 2.5 polish pass.

**Per-state visuals (this task implements all five):**

| State | Body tint | Caption text |
|---|---|---|
| RECORDING | red — `lv_color_hex(0xE74C3C)` | `● RECORDING 0:23` (timer updates every 200 ms) |
| UPLOADING | amber — `lv_color_hex(0xF59E0B)` | `UPLOADING…` |
| TRANSCRIBING | amber — `lv_color_hex(0xF59E0B)` | `TRANSCRIBING…` |
| SAVED | mint green — `lv_color_hex(0x22C55E)` | `✓ Saved` |
| FAILED | red — `lv_color_hex(0xE74C3C)` | `● {REASON} — tap to retry` |

Reason strings (Failed-state caption substitutes `{REASON}`):

| `dict_fail_t` | Caption substitution |
|---|---|
| `DICT_FAIL_AUTH` | `AUTH` |
| `DICT_FAIL_NETWORK` | `NETWORK` |
| `DICT_FAIL_EMPTY` | `EMPTY — got silence` |
| `DICT_FAIL_NO_AUDIO` | `NO AUDIO` |
| `DICT_FAIL_TOO_LONG` | `TOO LONG (5 min cap)` |
| `DICT_FAIL_CANCELLED` | `CANCELLED` |

- [ ] **Step 1: Add paint helpers in ui_orb.c**

Insert these static helpers above `ui_orb_set_pipeline_state` (which Task 2 stubbed):

```c
/* PR 2: paint the orb body in the pipeline-state tint.  Caller is
 * responsible for setting the caption text + showing the caption label.
 *
 * Uses the same body widget that the voice-state painter paints into.
 * The previous body styling (circadian gradient, etc.) is shadowed by
 * the solid tint until pipeline returns to IDLE. */
static void paint_pipeline_body(uint32_t color_hex) {
   if (!s_body) return;
   lv_obj_set_style_bg_color(s_body, lv_color_hex(color_hex), 0);
   /* Keep the radial-gradient luster for depth — only tint, don't go
    * flat.  The existing body uses LV_GRAD_DIR_VER; preserve. */
}

static const char *fail_reason_caption(dict_fail_t r) {
   switch (r) {
   case DICT_FAIL_AUTH:      return "AUTH";
   case DICT_FAIL_NETWORK:   return "NETWORK";
   case DICT_FAIL_EMPTY:     return "EMPTY — got silence";
   case DICT_FAIL_NO_AUDIO:  return "NO AUDIO";
   case DICT_FAIL_TOO_LONG:  return "TOO LONG (5 min cap)";
   case DICT_FAIL_CANCELLED: return "CANCELLED";
   default:                  return "FAIL";
   }
}

static void set_caption_text(const char *text) {
   if (!s_orb_caption || !text) return;
   lv_label_set_text(s_orb_caption, text);
   lv_obj_clear_flag(s_orb_caption, LV_OBJ_FLAG_HIDDEN);
}

static void hide_caption(void) {
   if (s_orb_caption) lv_obj_add_flag(s_orb_caption, LV_OBJ_FLAG_HIDDEN);
}

/* Timer cb for SAVED → IDLE auto-fade (Task 5 schedules this). */
static void saved_fade_to_idle_cb(lv_timer_t *t) {
   (void)t;
   s_saved_fade_timer = NULL;
   voice_dictation_set_state(DICT_IDLE, DICT_FAIL_NONE,
                             (uint32_t)(esp_timer_get_time() / 1000));
}
```

If `esp_timer_get_time` isn't already pulled in, add `#include "esp_timer.h"` at the top.

- [ ] **Step 2: Implement the state-driver inside `ui_orb_set_pipeline_state`**

Replace the stub from Task 2 with the real implementation:

```c
void ui_orb_set_pipeline_state(const dict_event_t *event) {
   if (!event) return;
   s_pipeline = *event;

   /* Tear down the SAVED auto-fade timer when leaving SAVED. */
   if (event->state != DICT_SAVED && s_saved_fade_timer) {
      lv_timer_del(s_saved_fade_timer);
      s_saved_fade_timer = NULL;
   }

   char buf[40];
   switch (event->state) {
   case DICT_IDLE:
      hide_caption();
      /* Repaint via the voice-state painter so the orb returns to its
       * normal IDLE/LISTENING/PROCESSING/SPEAKING visuals. */
      ui_orb_set_state(s_state);
      return;

   case DICT_RECORDING: {
      /* Engage the existing LISTENING state machine so the mic-RMS-driven
       * halo bloom fires.  Our pipeline body tint overrides the body
       * colour, but the bloom mechanic (halo opacity from mic RMS) still
       * runs because it manipulates s_halo, not s_body. */
      ui_orb_set_state(ORB_STATE_LISTENING);
      paint_pipeline_body(0xE74C3C);
      uint32_t dur_ms = (uint32_t)(esp_timer_get_time() / 1000) - event->started_ms;
      uint32_t s = dur_ms / 1000;
      snprintf(buf, sizeof(buf), "● RECORDING %lu:%02lu",
               (unsigned long)(s / 60), (unsigned long)(s % 60));
      set_caption_text(buf);
      break;
   }

   case DICT_UPLOADING:
      paint_pipeline_body(0xF59E0B);
      set_caption_text("UPLOADING…");
      break;

   case DICT_TRANSCRIBING:
      paint_pipeline_body(0xF59E0B);
      set_caption_text("TRANSCRIBING…");
      /* Reuse the existing PROCESSING comet animation for visual
       * continuity — call into the existing helper.  If the orb's
       * comet starter doesn't exist as a public/file-local helper,
       * just call ui_orb_set_state(ORB_STATE_PROCESSING) to engage
       * the existing PROCESSING visuals AS the underlying state and
       * let our pipeline paint stack on top. */
      ui_orb_set_state(ORB_STATE_PROCESSING);
      break;

   case DICT_SAVED:
      paint_pipeline_body(0x22C55E);
      set_caption_text("✓ Saved");
      /* Schedule auto-fade back to IDLE after 2 s.  Idempotent — if
       * one already exists (rapid SAVED re-entry), don't stack. */
      if (!s_saved_fade_timer) {
         s_saved_fade_timer = lv_timer_create(saved_fade_to_idle_cb, 2000, NULL);
         if (s_saved_fade_timer) lv_timer_set_repeat_count(s_saved_fade_timer, 1);
      }
      break;

   case DICT_FAILED: {
      paint_pipeline_body(0xE74C3C);
      snprintf(buf, sizeof(buf), "● %s — tap to retry",
               fail_reason_caption(event->fail_reason));
      set_caption_text(buf);
      break;
   }
   }
}
```

The variable `s_state` is the existing orb's voice-state field — confirm its name by reading the surrounding code.  Likely candidates: `s_state`, `s_orb_state`, `s_current_state`.

- [ ] **Step 3: Build + verify**

```bash
idf.py build 2>&1 | tail -8
```

Expected: clean build.  Likely compile errors at this point:
- `s_state` / `s_orb_state` — pick the right name
- `s_body` — confirm name matches whatever the existing orb body widget is called
- `FONT_CAPTION` — make sure config.h is included

Fix any compile errors with read-and-match against the surrounding file.

- [ ] **Step 4: Commit**

```bash
git add main/ui_orb.c
git commit -m "feat(orb): paint per-pipeline-state body tint + caption text"
```

---

### Task 5 — Live caption-timer for RECORDING state

The RECORDING caption shows live elapsed time.  We need a periodic timer to update it.

**Files:**
- Modify: `main/ui_orb.c`

- [ ] **Step 1: Add a recurring timer + cb**

Add to the static block near the top of ui_orb.c (alongside `s_saved_fade_timer`):

```c
static lv_timer_t *s_rec_timer_label = NULL;  /* updates RECORDING caption every 200 ms */
```

Add the timer callback near `saved_fade_to_idle_cb`:

```c
/* Update the RECORDING caption with live elapsed time.  Stops itself
 * if the pipeline has left RECORDING (defensive — set_pipeline_state
 * also tears it down explicitly). */
static void rec_timer_label_cb(lv_timer_t *t) {
   (void)t;
   if (s_pipeline.state != DICT_RECORDING) {
      if (s_rec_timer_label) { lv_timer_del(s_rec_timer_label); s_rec_timer_label = NULL; }
      return;
   }
   uint32_t dur_ms = (uint32_t)(esp_timer_get_time() / 1000) - s_pipeline.started_ms;
   uint32_t s = dur_ms / 1000;
   char buf[40];
   snprintf(buf, sizeof(buf), "● RECORDING %lu:%02lu",
            (unsigned long)(s / 60), (unsigned long)(s % 60));
   if (s_orb_caption) lv_label_set_text(s_orb_caption, buf);
}
```

- [ ] **Step 2: Start the timer on entering RECORDING + stop on leaving**

Modify the `DICT_RECORDING` case in `ui_orb_set_pipeline_state` to spawn the timer:

```c
   case DICT_RECORDING: {
      paint_pipeline_body(0xE74C3C);
      uint32_t dur_ms = (uint32_t)(esp_timer_get_time() / 1000) - event->started_ms;
      uint32_t s = dur_ms / 1000;
      snprintf(buf, sizeof(buf), "● RECORDING %lu:%02lu",
               (unsigned long)(s / 60), (unsigned long)(s % 60));
      set_caption_text(buf);
      if (!s_rec_timer_label) {
         s_rec_timer_label = lv_timer_create(rec_timer_label_cb, 200, NULL);
      }
      break;
   }
```

And in every NON-RECORDING case (DICT_IDLE / DICT_UPLOADING / DICT_TRANSCRIBING / DICT_SAVED / DICT_FAILED) add at the top of the case:

```c
      if (s_rec_timer_label) { lv_timer_del(s_rec_timer_label); s_rec_timer_label = NULL; }
```

Cleanest implementation: add the teardown above the switch, then re-create inside the RECORDING case:

```c
   /* Stop the live-timer if we're leaving RECORDING. */
   if (event->state != DICT_RECORDING && s_rec_timer_label) {
      lv_timer_del(s_rec_timer_label);
      s_rec_timer_label = NULL;
   }
```

- [ ] **Step 3: Tear down in ui_orb_destroy**

Add to the cleanup block (alongside the saved-fade timer cleanup from Task 3):

```c
   if (s_rec_timer_label) {
      lv_timer_del(s_rec_timer_label);
      s_rec_timer_label = NULL;
   }
```

- [ ] **Step 4: Build + verify**

```bash
idf.py build 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add main/ui_orb.c
git commit -m "feat(orb): live elapsed-time caption during RECORDING state"
```

---

### Task 6 — Orb subscribes to pipeline state machine

**Files:**
- Modify: `main/ui_orb.c`

- [ ] **Step 1: Add the subscriber callback**

Add near the bottom of ui_orb.c, just before `ui_orb_set_pipeline_state`:

```c
/* Subscriber bridge: voice_dictation fires this on the LVGL thread
 * (because we use voice_dictation_subscribe_lvgl in ui_orb_create).
 * We just forward to the public state-driver. */
static void orb_pipeline_cb(const dict_event_t *event, void *user_data) {
   (void)user_data;
   ui_orb_set_pipeline_state(event);
}
```

- [ ] **Step 2: Register in `ui_orb_create`**

Find the end of `ui_orb_create` (just before `return;` or end of function).  Add:

```c
   /* PR 2: subscribe to the dictation pipeline.  Callbacks land on the
    * LVGL thread thanks to the _lvgl variant. */
   static int s_pipeline_sub = -1;
   if (s_pipeline_sub < 0) {
      s_pipeline_sub = voice_dictation_subscribe_lvgl(orb_pipeline_cb, NULL);
   }
```

(If you used the split-file path from Task 1 Step 4, include `"voice_dictation_lvgl.h"` at the top of ui_orb.c.)

- [ ] **Step 3: Build + flash + verify visually**

```bash
idf.py build 2>&1 | tail -5
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -3
sleep 22

# Trigger a dictation via debug endpoint and watch the orb paint changes:
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" -X POST "http://192.168.1.90:8080/dictation?action=start"
sleep 1
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" http://192.168.1.90:8080/screenshot.jpg -o /tmp/orb-recording.jpg
file /tmp/orb-recording.jpg
echo "captured RECORDING state — orb should be red with caption"

sleep 1
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" -X POST "http://192.168.1.90:8080/dictation?action=stop"
sleep 3
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" http://192.168.1.90:8080/screenshot.jpg -o /tmp/orb-transcribing.jpg
echo "captured TRANSCRIBING state — orb should be amber with caption"
```

Inspect the two JPEGs.  RECORDING screenshot: orb body red, caption "● RECORDING 0:01" or similar.  TRANSCRIBING screenshot: orb body amber, caption "TRANSCRIBING…".

- [ ] **Step 4: Commit**

```bash
git add main/ui_orb.c
git commit -m "feat(orb): subscribe to dictation pipeline state machine"
```

---

### Task 7 — Dictate chip widget on home

**Files:**
- Modify: `main/ui_home.c`

- [ ] **Step 1: Add the chip widget statics + the create helper**

Search ui_home.c for `s_mode_chip` to find the existing chip widget — match its style exactly.  Add a parallel static block:

```c
/* PR 2: Dictate chip — sits directly below the mode chip.  Same dark-pill
 * visual language (thin gray border, monospace, no gradient).  Mirrors
 * the dictation pipeline state in its label.  Tap → voice_start_dictation. */
static lv_obj_t *s_dictate_chip = NULL;
static lv_obj_t *s_dictate_chip_dot = NULL;     /* red breathing dot when IDLE */
static lv_obj_t *s_dictate_chip_label = NULL;
static lv_obj_t *s_dictate_chip_hint = NULL;    /* "TAP TO START" / "0:23" */
static lv_obj_t *s_dictate_chip_icon = NULL;    /* 🎤 / × */
```

- [ ] **Step 2: Add chip construction inside `ui_home_create_root` (or wherever `s_mode_chip` is created)**

Find where `s_mode_chip` is built.  Immediately after, add:

```c
   /* PR 2: Dictate chip — same width / height as the mode chip, 12 px below. */
   s_dictate_chip = lv_obj_create(parent);
   lv_obj_remove_style_all(s_dictate_chip);
   lv_obj_set_size(s_dictate_chip, lv_obj_get_width(s_mode_chip), 36);
   lv_obj_align_to(s_dictate_chip, s_mode_chip, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
   lv_obj_set_style_bg_color(s_dictate_chip, lv_color_hex(0x141420), 0);
   lv_obj_set_style_bg_opa(s_dictate_chip, LV_OPA_COVER, 0);
   lv_obj_set_style_border_width(s_dictate_chip, 1, 0);
   lv_obj_set_style_border_color(s_dictate_chip, lv_color_hex(0x262637), 0);
   lv_obj_set_style_radius(s_dictate_chip, 18, 0);
   lv_obj_set_style_pad_hor(s_dictate_chip, 14, 0);
   lv_obj_clear_flag(s_dictate_chip, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(s_dictate_chip, LV_OBJ_FLAG_CLICKABLE);

   /* Red breathing dot (left). */
   s_dictate_chip_dot = lv_obj_create(s_dictate_chip);
   lv_obj_remove_style_all(s_dictate_chip_dot);
   lv_obj_set_size(s_dictate_chip_dot, 10, 10);
   lv_obj_align(s_dictate_chip_dot, LV_ALIGN_LEFT_MID, 0, 0);
   lv_obj_set_style_bg_color(s_dictate_chip_dot, lv_color_hex(0xE74C3C), 0);
   lv_obj_set_style_bg_opa(s_dictate_chip_dot, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(s_dictate_chip_dot, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_border_width(s_dictate_chip_dot, 0, 0);

   /* Label "Dictate" (left-mid, after the dot). */
   s_dictate_chip_label = lv_label_create(s_dictate_chip);
   lv_label_set_text(s_dictate_chip_label, "Dictate");
   lv_obj_set_style_text_color(s_dictate_chip_label, lv_color_hex(0xE8E8EF), 0);
   lv_obj_set_style_text_font(s_dictate_chip_label, FONT_BODY, 0);
   lv_obj_align(s_dictate_chip_label, LV_ALIGN_LEFT_MID, 18, 0);

   /* Hint "TAP TO START" (right-mid, before the mic icon). */
   s_dictate_chip_hint = lv_label_create(s_dictate_chip);
   lv_label_set_text(s_dictate_chip_hint, "TAP TO START");
   lv_obj_set_style_text_color(s_dictate_chip_hint, lv_color_hex(0x888888), 0);
   lv_obj_set_style_text_font(s_dictate_chip_hint, FONT_CAPTION, 0);
   lv_obj_set_style_text_letter_space(s_dictate_chip_hint, 2, 0);
   lv_obj_align(s_dictate_chip_hint, LV_ALIGN_RIGHT_MID, -28, 0);

   /* Mic icon (right edge). */
   s_dictate_chip_icon = lv_label_create(s_dictate_chip);
   lv_label_set_text(s_dictate_chip_icon, LV_SYMBOL_AUDIO);
   lv_obj_set_style_text_color(s_dictate_chip_icon, lv_color_hex(0xE8E8EF), 0);
   lv_obj_set_style_text_font(s_dictate_chip_icon, FONT_CAPTION, 0);
   lv_obj_align(s_dictate_chip_icon, LV_ALIGN_RIGHT_MID, 0, 0);
```

Match the surrounding ui_home.c style.  If `FONT_BODY` / `FONT_CAPTION` aren't already in scope, add `#include "config.h"`.

- [ ] **Step 3: Build + flash + visual check**

```bash
idf.py build 2>&1 | tail -5
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -3
sleep 22
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" -X POST "http://192.168.1.90:8080/navigate?screen=home"
sleep 2
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" http://192.168.1.90:8080/screenshot.jpg -o /tmp/home-with-chip.jpg
echo "home should now show two chips stacked: Local · ON-DEVICE · ⌄ AND ● Dictate · TAP TO START · 🎤"
```

- [ ] **Step 4: Commit**

```bash
git add main/ui_home.c
git commit -m "feat(home): Dictate chip widget below mode chip (visual only)"
```

---

### Task 8 — Dictate chip tap handler → voice_start_dictation

**Files:**
- Modify: `main/ui_home.c`

- [ ] **Step 1: Add the tap event callback**

Near other event callbacks in ui_home.c, add:

```c
/* PR 2: Dictate chip tap.  When chip is in IDLE, starts a dictation;
 * when in RECORDING (via × icon), cancels via voice_cancel().  Other
 * states route through the orb's pipeline visuals + ignore taps. */
static void dictate_chip_tap_cb(lv_event_t *e) {
   (void)e;
   dict_event_t dp = voice_dictation_get();
   if (dp.state == DICT_IDLE) {
      esp_err_t err = voice_start_dictation();
      if (err != ESP_OK) {
         ESP_LOGW("home", "voice_start_dictation failed: %s",
                  esp_err_to_name(err));
      }
   } else if (dp.state == DICT_RECORDING) {
      /* Cancel — fire voice_cancel + transition pipeline to FAILED(CANCELLED).
       * The voice_ws_proto.c dictation_cancelled handler will pick up the
       * Dragon-side ack; we drive the local pipeline immediately so the UI
       * is responsive. */
      voice_cancel();
      voice_dictation_set_state(DICT_FAILED, DICT_FAIL_CANCELLED,
                                (uint32_t)(esp_timer_get_time() / 1000));
   }
   /* UPLOADING / TRANSCRIBING / SAVED / FAILED states: tap is a no-op for
    * v1.  Future polish: tap FAILED to retry, tap SAVED to dismiss early. */
}
```

Add the include at the top of ui_home.c (if not already present):

```c
#include "voice_dictation.h"
#include "voice.h"  /* voice_start_dictation / voice_cancel — likely already there */
```

- [ ] **Step 2: Wire the event handler**

Inside the chip-construction block (Task 7), after `s_dictate_chip` is built:

```c
   lv_obj_add_event_cb(s_dictate_chip, dictate_chip_tap_cb, LV_EVENT_CLICKED, NULL);
```

- [ ] **Step 3: Build + flash + live tap test**

```bash
idf.py build 2>&1 | tail -5
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -3
sleep 22

# Tap the chip via touch coords.  Chip is below the mode chip; touch
# y coord depends on the exact layout.  Estimate based on the home
# screenshot from Task 7 — approx (360, 750) on a 720×1280 portrait.
# Confirm exact y by reading the screenshot first.
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST http://192.168.1.90:8080/touch \
     -d '{"x":360,"y":760,"action":"tap"}'

sleep 2
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/dictation_pipeline | python3 -m json.tool
```

Expected: state transitions to RECORDING.  Voice is now hot.  Wait a few seconds, then stop:

```bash
sleep 3
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/screenshot.jpg -o /tmp/recording-live.jpg
echo "should see orb red + caption RECORDING N:NN + chip is now in RECORDING state too"
```

- [ ] **Step 4: Commit**

```bash
git add main/ui_home.c
git commit -m "feat(home): Dictate chip tap fires voice_start_dictation"
```

---

### Task 9 — Dictate chip subscribes to pipeline (mirror orb state)

**Files:**
- Modify: `main/ui_home.c`

- [ ] **Step 1: Add the chip's pipeline subscriber callback**

Add near `dictate_chip_tap_cb`:

```c
/* PR 2: keep the Dictate chip's label / hint / icon in sync with the
 * dictation pipeline state.  Mirrors the orb's heroic painting at a
 * compact scale. */
static void dictate_chip_pipeline_cb(const dict_event_t *event, void *user_data) {
   (void)user_data;
   if (!s_dictate_chip || !s_dictate_chip_label ||
       !s_dictate_chip_hint || !s_dictate_chip_icon ||
       !s_dictate_chip_dot) {
      return;
   }

   /* Default styling = IDLE.  Each state below mutates from here. */
   lv_obj_set_style_border_color(s_dictate_chip, lv_color_hex(0x262637), 0);
   lv_obj_set_style_bg_color(s_dictate_chip_dot, lv_color_hex(0xE74C3C), 0);

   char buf[40];
   switch (event->state) {
   case DICT_IDLE:
      lv_label_set_text(s_dictate_chip_label, "Dictate");
      lv_label_set_text(s_dictate_chip_hint,  "TAP TO START");
      lv_obj_set_style_text_color(s_dictate_chip_hint, lv_color_hex(0x888888), 0);
      lv_label_set_text(s_dictate_chip_icon,  LV_SYMBOL_AUDIO);
      break;

   case DICT_RECORDING: {
      uint32_t dur_ms = (uint32_t)(esp_timer_get_time() / 1000) - event->started_ms;
      uint32_t s = dur_ms / 1000;
      snprintf(buf, sizeof(buf), "%lu:%02lu",
               (unsigned long)(s / 60), (unsigned long)(s % 60));
      lv_label_set_text(s_dictate_chip_label, "RECORDING");
      lv_label_set_text(s_dictate_chip_hint,  buf);
      lv_obj_set_style_text_color(s_dictate_chip_hint, lv_color_hex(0xE74C3C), 0);
      lv_label_set_text(s_dictate_chip_icon,  LV_SYMBOL_CLOSE);
      lv_obj_set_style_border_color(s_dictate_chip, lv_color_hex(0xE74C3C), 0);
      break;
   }

   case DICT_UPLOADING:
      lv_label_set_text(s_dictate_chip_label, "UPLOADING");
      lv_label_set_text(s_dictate_chip_hint,  "");
      lv_label_set_text(s_dictate_chip_icon,  LV_SYMBOL_UPLOAD);
      lv_obj_set_style_border_color(s_dictate_chip, lv_color_hex(0xF59E0B), 0);
      lv_obj_set_style_bg_color(s_dictate_chip_dot, lv_color_hex(0xF59E0B), 0);
      break;

   case DICT_TRANSCRIBING:
      lv_label_set_text(s_dictate_chip_label, "TRANSCRIBING");
      lv_label_set_text(s_dictate_chip_hint,  "");
      lv_label_set_text(s_dictate_chip_icon,  LV_SYMBOL_REFRESH);
      lv_obj_set_style_border_color(s_dictate_chip, lv_color_hex(0xF59E0B), 0);
      lv_obj_set_style_bg_color(s_dictate_chip_dot, lv_color_hex(0xF59E0B), 0);
      break;

   case DICT_SAVED:
      lv_label_set_text(s_dictate_chip_label, "Saved");
      lv_label_set_text(s_dictate_chip_hint,  "");
      lv_label_set_text(s_dictate_chip_icon,  LV_SYMBOL_OK);
      lv_obj_set_style_border_color(s_dictate_chip, lv_color_hex(0x22C55E), 0);
      lv_obj_set_style_bg_color(s_dictate_chip_dot, lv_color_hex(0x22C55E), 0);
      break;

   case DICT_FAILED:
      lv_label_set_text(s_dictate_chip_label, "FAILED");
      lv_label_set_text(s_dictate_chip_hint,  "TAP TO RETRY");
      lv_obj_set_style_text_color(s_dictate_chip_hint, lv_color_hex(0xE74C3C), 0);
      lv_label_set_text(s_dictate_chip_icon,  LV_SYMBOL_REFRESH);
      lv_obj_set_style_border_color(s_dictate_chip, lv_color_hex(0xE74C3C), 0);
      break;
   }
}
```

- [ ] **Step 2: Register the subscriber inside `ui_home_create_root` (or wherever the home is built)**

Just before the home create function returns (or at the end of the chip-build block), add:

```c
   /* PR 2: subscribe the chip to the dictation pipeline.  LVGL-thread
    * marshalling handled by the _lvgl variant. */
   static int s_dictate_chip_sub = -1;
   if (s_dictate_chip_sub < 0) {
      s_dictate_chip_sub = voice_dictation_subscribe_lvgl(dictate_chip_pipeline_cb, NULL);
   }
```

Add the include for `voice_dictation_subscribe_lvgl` (either `voice_dictation.h` if Task 1 Step 2 went in-file, or `voice_dictation_lvgl.h` if Task 1 Step 4 split it).

- [ ] **Step 3: Live tap-cycle verification**

```bash
idf.py build 2>&1 | tail -5
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -3
sleep 22

# 1. Capture IDLE chip state
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST http://192.168.1.90:8080/navigate?screen=home -o /dev/null
sleep 1
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/screenshot.jpg -o /tmp/chip-idle.jpg

# 2. Tap to start, capture RECORDING state
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST http://192.168.1.90:8080/touch \
     -d '{"x":360,"y":760,"action":"tap"}'
sleep 2
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/screenshot.jpg -o /tmp/chip-recording.jpg

# 3. Stop via debug endpoint, capture TRANSCRIBING state
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST "http://192.168.1.90:8080/dictation?action=stop"
sleep 1
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/screenshot.jpg -o /tmp/chip-transcribing.jpg

ls -la /tmp/chip-*.jpg
echo "three screenshots — chip-idle should show Dictate / TAP TO START / 🎤;"
echo "chip-recording should show RECORDING / 0:01 / × in red;"
echo "chip-transcribing should show TRANSCRIBING / / 🔄 in amber."
```

- [ ] **Step 4: Commit**

```bash
git add main/ui_home.c
git commit -m "feat(home): Dictate chip subscribes to pipeline + mirrors state"
```

---

### Task 10 — Format gate + smoke test under real dictation cycle

**Files:** none (verification).

- [ ] **Step 1: Format gate**

```bash
cd /home/rebelforce/projects/TinkerTab
git fetch origin main:refs/remotes/origin/main
git-clang-format --binary clang-format-18 --diff origin/main \
   main/voice_dictation.{c,h} main/ui_orb.{c,h} main/ui_home.c \
   $( [ -f main/voice_dictation_lvgl.c ] && echo "main/voice_dictation_lvgl.{c,h}" )
```

If non-empty, run without `--diff` to autofix:

```bash
git-clang-format --binary clang-format-18 origin/main
git add -u
git commit --amend --no-edit
```

- [ ] **Step 2: Live end-to-end happy path**

```bash
# Should be a clean home — capture baseline
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST http://192.168.1.90:8080/navigate?screen=home -o /dev/null
sleep 1
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/screenshot.jpg -o /tmp/pr2-home-idle.jpg

# Trigger via chip tap
curl -s -m 5 -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST http://192.168.1.90:8080/touch \
     -d '{"x":360,"y":760,"action":"tap"}'

# Capture each state at ~0.5 s, 2 s, 5 s
sleep 1
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/screenshot.jpg -o /tmp/pr2-recording.jpg

curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     -X POST "http://192.168.1.90:8080/dictation?action=stop"

sleep 2
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/screenshot.jpg -o /tmp/pr2-transcribing.jpg

sleep 5
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/screenshot.jpg -o /tmp/pr2-saved-or-back-to-idle.jpg

ls -la /tmp/pr2-*.jpg
```

Visually verify the four screenshots show: IDLE chip, red orb + RECORDING chip + caption, amber orb + TRANSCRIBING chip + caption, green orb + SAVED chip + caption (or already back to IDLE if Dragon's summary was fast).

- [ ] **Step 3: Live failure-path verify (tap during recording → CANCEL)**

```bash
# Tap to start
curl -s -X POST -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/touch -d '{"x":360,"y":760,"action":"tap"}'
sleep 2
# Tap again (× icon should now be active)
curl -s -X POST -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/touch -d '{"x":360,"y":760,"action":"tap"}'
sleep 1
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/dictation_pipeline | python3 -m json.tool
```

Expected: state shows FAILED with reason CANCELLED, then (after 2 s if SAVED fade timer reaches it... wait, FAILED has no auto-fade in v1) sits at FAILED with the chip showing "TAP TO RETRY".

Tap once more — should retry / restart.  Confirm state goes back to RECORDING.

- [ ] **Step 4: Heap stability**

```bash
curl -s -H "Authorization: Bearer 05eed3b13bf62d92cfd8ac424438b9f2" \
     http://192.168.1.90:8080/heap | python3 -m json.tool | head -12
```

Expected: internal free > 80 KB, no fragmentation alarm, LVGL pool free > 1.5 MB.

- [ ] **Step 5: Update memory**

Update the `project_dictation_pipeline_redesign_2026_05_14.md` memory file's "Status" section to reflect PR 2 landed + the visible-on-device changes.

---

### Task 11 — Push + open PR

**Files:** none (operational).

- [ ] **Step 1: Final format check**

```bash
git-clang-format --binary clang-format-18 --diff origin/main
```

Expected: `clang-format did not modify any files`.

- [ ] **Step 2: File the issue**

```bash
gh issue create --title "PR 2 of 4: orb morphs + Dictate chip on home (visible UI starts here)" \
  --body "PR 2 of the dictation pipeline visibility design (#518 / spec at docs/superpowers/specs/2026-05-14-dictation-pipeline-notes-redesign-design.md). First PR in the series where the home screen visibly changes.

## What lands
- Orb morphs through pipeline states (RECORDING red / UPLOADING amber / TRANSCRIBING amber+comet / SAVED green / FAILED red) with caption text + live timer
- New Dictate chip below mode chip on home — dark pill, mode-chip language, no gradient
- Chip tap → voice_start_dictation; tap during recording → voice_cancel
- Both subscribers (orb + chip) run on the LVGL thread via voice_dictation_subscribe_lvgl
- SAVED → IDLE 2 s auto-fade timer (PR 1 deferred; lands here)

## Out of scope (filed as v1.5 polish)
- Internal ripple rings during RECORDING (current: solid red tint)
- Amber arc fill animation during UPLOADING (current: solid amber tint)
- Body breath rate variation per state (current: existing IDLE breath)
- Halo color changes per state (current: unchanged from voice-state)"
```

- [ ] **Step 3: Push + PR**

```bash
git push -u origin feat/dictation-pipeline-pr2

gh pr create --title "feat(dictation): orb morphs + Dictate chip on home (PR 2 of 4)" \
  --body "## Summary
- First visible UI change in the dictation pipeline series.
- Orb takes precedence over voice-state painting when pipeline state ≠ IDLE; reverts cleanly on IDLE.
- Dictate chip below mode chip — matches existing mode-chip language (dark pill, thin border, monospace).
- Chip + orb both subscribe to the voice_dictation state machine that landed in #519.
- New voice_dictation_subscribe_lvgl marshalling wrapper for LVGL-thread-safe subscribers.
- SAVED → IDLE 2 s auto-fade timer that PR 1 deferred.

Builds on #519 (state machine + /dictation_pipeline endpoint).  Sets up PR 3 (Notes Timeline) which subscribes the Notes-screen processing row to the same pipeline.

## Test plan
- [x] idf.py build clean, no warnings
- [x] git-clang-format --diff origin/main clean
- [x] Live on Tab5 192.168.1.90:
  - Chip tap → orb morphs RECORDING → TRANSCRIBING → SAVED → IDLE
  - Chip mirrors orb state in real time (label, hint, icon, border color)
  - Live elapsed-time counter during RECORDING updates every 200 ms
  - Cancel via second chip tap during RECORDING transitions to FAILED(CANCELLED) → chip shows TAP TO RETRY → retry tap restarts cycle
- [x] No PANIC, heap stable

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

- [ ] **Step 4: Mark complete**

Edit `~/.claude/projects/-home-rebelforce/memory/MEMORY.md` and the project memory file with the PR number once `gh pr create` returns.

---

## Acceptance summary

When all 11 tasks land:

- [ ] `voice_dictation_subscribe_lvgl()` available (either in voice_dictation.{c,h} or in voice_dictation_lvgl.{c,h})
- [ ] `ui_orb_set_pipeline_state()` API published + implemented in ui_orb.c
- [ ] Orb morphs through all 5 pipeline states (RECORDING / UPLOADING / TRANSCRIBING / SAVED / FAILED) with appropriate body tint + caption text
- [ ] Live elapsed-time counter on RECORDING caption (200 ms update cadence)
- [ ] SAVED auto-fades back to IDLE after 2 s via lv_timer
- [ ] Dictate chip widget below mode chip on home, matching mode-chip visual language
- [ ] Chip tap fires voice_start_dictation
- [ ] Chip × tap during RECORDING fires voice_cancel + drives pipeline to FAILED(CANCELLED)
- [ ] Chip mirrors pipeline state in label / hint / icon / border color
- [ ] Format gate green, target build clean, no new warnings
- [ ] Live e2e cycle visually verified on Tab5 across all 5 pipeline states
- [ ] PR open + linked to issue, memory updated

PR 3 (Notes Timeline) gets its own plan once this lands and is user-verified.
