/*
 * ui_notification.h — Tab5-side surface for `channel_message` frames.
 *
 * Wave 7-E (mode-3 / TinkerClaw gateway integration).  Design spec:
 *   docs/PLAN-agent-mode-notification-surface.md
 *
 * v0 (W7-E.1, this slice) ships the toast path only:
 *   - parses `channel_message` WS frames into channel_message_t
 *   - routes everything to ui_home_show_toast_ex(text, UI_TOAST_INCOMING)
 *   - fires UI_CUE_INCOMING_LOW unless quiet-hours active
 *
 * Future slices:
 *   - W7-E.2: now-card claim for high-priority / reply-shaped messages
 *   - W7-E.3: dedupe + snooze rings
 *   - W7-E.4: voice reply flow (Tab5 → Dragon `channel_reply`)
 *   - W7-E.5: Settings → Channels per-channel toggles
 *   - W7-E.6: quiet-hours polish + final integration
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Short canonical channel names (matches NVS `ch_*_on` suffix).  Free-form
 * string so future gateway-added channels don't need a Tab5 firmware bump. */
#define CHANNEL_NAME_MAX 16
#define CHANNEL_SENDER_MAX 64
#define CHANNEL_PREVIEW_MAX 160
#define CHANNEL_MSG_ID_MAX 64
#define CHANNEL_THREAD_ID_MAX 64
#define CHANNEL_PRIORITY_MAX 16

typedef struct {
   char channel[CHANNEL_NAME_MAX];        /* "tg", "wa", "dc", ... */
   char message_id[CHANNEL_MSG_ID_MAX];   /* dedupe key; reserved W7-E.3 */
   char thread_id[CHANNEL_THREAD_ID_MAX]; /* reply target; reserved W7-E.4 */
   char sender[CHANNEL_SENDER_MAX];       /* sender display name */
   char preview[CHANNEL_PREVIEW_MAX];     /* short text for toast */
   char priority[CHANNEL_PRIORITY_MAX];   /* "low" / "normal" / "high" */
   bool sender_starred;
   bool needs_reply;
} channel_message_t;

/* Thread-safe.  Internally hops to the LVGL thread via tab5_lv_async_call
 * before rendering.  `msg` is copied — caller retains ownership of the
 * source struct. */
void ui_notification_show(const channel_message_t *msg);

/* W7-E.3: boot-time init.  Kicks the 60 s snooze-walk timer so deferred
 * messages refire on schedule.  Idempotent; safe to call more than once.
 * Must run on the LVGL thread (typically from main.c after ui_core_init).
 */
void ui_notification_init(void);

/* W7-E.3: enqueue the most recently shown now-card message into the
 * snooze ring with `fire_at = now + 15 min`.  Overflow (>8) drops the
 * oldest entry.  Wired to the SNOOZE button in ui_home.  No-op if no
 * now-card message has been shown yet this boot. */
void ui_notification_snooze_current(void);

#ifdef __cplusplus
}
#endif
