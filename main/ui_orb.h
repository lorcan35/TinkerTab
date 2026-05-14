/**
 * @file ui_orb.h
 * @brief Home-screen orb — public API.
 *
 * Owns: the lit-sphere body, halo, specular highlight, comet, and the
 * 4-state machine (IDLE / LISTENING / PROCESSING / SPEAKING).  Hardware-
 * aware behaviors layered on top: BMI270-driven specular drift, ES7210
 * mic-RMS-driven bloom, SC202CS-driven presence-wake dim.
 *
 * Strict design rule: exactly ONE active motion per state.  Presence-wake
 * dim is a global multiplier on top, not counted toward the budget.
 *
 * See docs/superpowers/specs/2026-05-14-orb-redesign-design.md for the
 * canonical spec.  Extract reason: ui_home.c had grown ~3,300 LOC with
 * ~600 LOC of orb state + anim + paint code accreted across TT
 * #501–#511.  No bounded module = no bounded reasoning.
 *
 * Thread model: all public APIs are LVGL-thread-only EXCEPT
 * ui_orb_set_presence which is safe from tab5_worker (it marshals via
 * tab5_lv_async_call internally).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "voice_dictation.h" /* dict_event_t, dict_state_t (PR 2) */
#include "widget.h"          /* widget_tone_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── State machine ──────────────────────────────────────────────────── */

typedef enum {
   ORB_STATE_IDLE = 0,   /* Default: circadian + tilt-spec + (presence dim). */
   ORB_STATE_LISTENING,  /* Mic hot: voice-bloom + listening-lean + tilt-spec. */
   ORB_STATE_PROCESSING, /* LLM / tool active: skill-comet only (everything else freezes). */
   ORB_STATE_SPEAKING,   /* TTS playing: steady warm halo, no motion. */
} ui_orb_state_t;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/** Build the orb hierarchy as children of `parent`, centered at (cx, cy).
 *  Idempotent: safe to call once per home screen.  Pulls circadian
 *  palette at creation; subsequent transitions repaint on demand.  */
void ui_orb_create(lv_obj_t *parent, int cx, int cy);

/** Destroy all orb LVGL objects + stop all timers (tilt poll, comet
 *  anim, bloom poll).  Safe to call when not created.  */
void ui_orb_destroy(void);

/* ── State driver ───────────────────────────────────────────────────── */

/** Drive the orb's state machine.  Called from voice_set_state() callback
 *  inside ui_home.c (which is the bridge from the voice state names to
 *  the orb's smaller state set).  Calls inside the same state are no-ops. */
void ui_orb_set_state(ui_orb_state_t s);

/** Read current orb state — used by ui_home for status-strip text + by
 *  /screen debug endpoint snapshots.  */
ui_orb_state_t ui_orb_get_state(void);

/* ── Hardware-aware hooks ───────────────────────────────────────────── */

/** Presence-wake dim hook.  `near=true` → full brightness + normal motion.
 *  `near=false` → orb dims to 35% and motion timers slow ×0.4.  Safe to
 *  call from non-LVGL tasks (marshals via tab5_lv_async_call internally). */
void ui_orb_set_presence(bool near);

/** Hour-of-day override for circadian palette debug.  -1 = clear override
 *  (returns to real RTC-driven palette).  Honors the existing debug
 *  endpoint POST /orb/force_hour?h=N from #503.  Thread-safe (marshals
 *  the repaint via tab5_lv_async_call). */
void ui_orb_force_hour(int hour);

/** Read the EFFECTIVE hour the orb is committed to right now —
 *  override value if set, else the real-time hour.  Never returns -1
 *  in normal operation.  Decoupled from the last-painted hour so
 *  callers polling right after a force_hour POST see the committed
 *  state. */
int ui_orb_get_effective_hour(void);

/** Repaint if the effective hour has rolled over since the last paint.
 *  Called from ui_home's 2 s refresh tick to drift the orb through the
 *  day. */
void ui_orb_repaint_if_hour_changed(void);

/** Voice-mode aware orb paint.  Called by ui_home on mode cycle.
 *  Currently ignores `mode` (all modes paint with the circadian palette)
 *  but the hook stays for future mode-tinted variants. */
void ui_orb_paint_for_mode(uint8_t mode);

/** Widget-tone orb override — claimed when a live widget takes the slot.
 *  Bypasses the circadian palette for the tone's color pair. */
void ui_orb_paint_for_tone(widget_tone_t tone);

/** Tool ripple — kept as a shim during the redesign rollout so
 *  voice_ws_proto.c links cleanly without immediate refactor.  Will be
 *  superseded by the PROCESSING state's comet in step 6; current behavior
 *  forwards to ui_orb_set_state(ORB_STATE_PROCESSING). */
void ui_orb_ripple_for_tool(const char *tool_name);

/* ── Touch routing ──────────────────────────────────────────────────── */

/** Returns the LVGL obj that should receive click + long-press handlers
 *  (i.e. the orb body, since that's the visible tap surface).  ui_home
 *  wires its existing orb_click_cb / orb_long_press_cb to this. */
lv_obj_t *ui_orb_get_root(void);

/** Returns the orb body obj — exposed for ui_home compatibility with code
 *  that needs to attach children (e.g. status pip).  Discouraged for new
 *  code; prefer adding behaviors inside ui_orb itself. */
lv_obj_t *ui_orb_get_body(void);

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

#ifdef __cplusplus
}
#endif
