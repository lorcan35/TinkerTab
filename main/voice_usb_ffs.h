/**
 * @file voice_usb_ffs.h
 * @brief USB host raw-bulk driver for K144's ffs.control and ffs.video.
 *
 * One USB host client claims BOTH interfaces (Tinker Control + Tinker
 * Video) from the same K144 device.  Two stream buffers, two locks, two
 * sets of send/recv APIs — but only one client + one watcher task.  The
 * dual-client variant blew Tab5's internal SRAM (TT #621 W6 audit).
 *
 * Interfaces are discovered at runtime by unique subclass:
 *   - 0x44  Tinker Control  (StackFlow JSON control plane)
 *   - 0x43  Tinker Video    (yolo / image inference plane)
 * ADB uses 0x42 (same class/proto), which we ignore.
 *
 * Endpoint addresses are kernel-assigned per gadget composition; the
 * driver walks the descriptor at claim time and stashes the actual
 * addresses.  No hardcoded EPs.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_USB_FFS_K144_VID 0x32c9
#define VOICE_USB_FFS_K144_PID 0x2003

#define VOICE_USB_FFS_K144_PROTOCOL 0x01
#define VOICE_USB_FFS_K144_SUBCLASS_CONTROL 0x44
#define VOICE_USB_FFS_K144_SUBCLASS_VIDEO 0x43

/* Back-compat alias for callers (e.g. voice_xport.c) that already use
 * the bare _SUBCLASS / _INTERFACE names from the original single-channel
 * driver.  Maps to the control channel. */
#define VOICE_USB_FFS_K144_SUBCLASS VOICE_USB_FFS_K144_SUBCLASS_CONTROL

esp_err_t voice_usb_ffs_init(void);
void voice_usb_ffs_deinit(void);
bool voice_usb_ffs_is_initialized(void);

/** ── Control channel (ffs.control, subclass 0x44) ──
 *  Used by voice_xport / voice_onboard / voice_m5_llm. */
bool voice_usb_ffs_is_connected(void);
int voice_usb_ffs_send(const void *buf, size_t len);
int voice_usb_ffs_recv(void *buf, size_t len, uint32_t timeout_ms);
void voice_usb_ffs_flush(void);
esp_err_t voice_usb_ffs_lock(uint32_t timeout_ms);
void voice_usb_ffs_unlock(void);

/** ── Video channel (ffs.video, subclass 0x43) ──
 *  Used by voice_yolo.  Independent lock so it doesn't contend with the
 *  control channel's always-on voice traffic. */
bool voice_usb_ffs_video_is_connected(void);
int voice_usb_ffs_video_send(const void *buf, size_t len);
int voice_usb_ffs_video_recv(void *buf, size_t len, uint32_t timeout_ms);
void voice_usb_ffs_video_flush(void);
esp_err_t voice_usb_ffs_video_lock(uint32_t timeout_ms);
void voice_usb_ffs_video_unlock(void);

/** Zero-copy TX path for the video channel.  Caller takes the lock,
 *  composes its payload directly into the returned buffer (up to
 *  @p *out_cap bytes), then calls _tx_commit() with the actual length.
 *  Eliminates the memcpy through PSRAM that was burning bandwidth and
 *  flapping Wi-Fi during YOLO bursts.
 *
 *  Usage:
 *      uint8_t *buf; size_t cap;
 *      voice_usb_ffs_video_lock(2000);
 *      buf = voice_usb_ffs_video_tx_borrow(&cap);
 *      // ... write up to cap bytes into buf ...
 *      int sent = voice_usb_ffs_video_tx_commit(actual_len);
 *      voice_usb_ffs_video_unlock();
 *
 *  Returns NULL if the channel isn't connected. */
void *voice_usb_ffs_video_tx_borrow(size_t *out_cap);
int voice_usb_ffs_video_tx_commit(size_t len);

#ifdef __cplusplus
}
#endif
