/**
 * @file voice_xport.h
 * @brief Transport abstraction for Tab5↔K144 traffic.
 *
 * Multiplexes between the legacy M5-Bus UART (`tab5_port_c_*`) and the
 * new USB CDC-ACM (`voice_usb_cdc_*`) based on the NVS `xport` key.
 * Callers (voice_m5_llm.c, voice_ext_pcm_stream.c) use this layer so
 * the rest of the firmware doesn't care which wire the StackFlow JSON
 * is travelling on.
 *
 * Default at boot is whatever the user has saved in NVS; if no key is
 * set, we default to UART (the historical transport, known-good).
 *
 * TT #620 W3.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   VOICE_XPORT_UART = 0,    /**< M5-Bus UART via Port C (1.5 Mbps, fragile) */
   VOICE_XPORT_USB_CDC = 1, /**< USB-A → K144 USB-C, CDC-ACM (USB HS) — BLOCKED by TT #621 */
   VOICE_XPORT_USB_FFS = 2, /**< USB-A → K144 USB-C, vendor-class bulk pair (TT #621 escape) */
} voice_xport_kind_t;

/**
 * @brief Initialise the active transport.  Reads NVS `xport` key, sets
 *        the active backend, and waits briefly (≤ @p ready_wait_ms) for
 *        the underlying transport to become ready (USB CDC: device
 *        enumeration).  Falls back to UART if USB selected but not
 *        ready within the window.
 *
 * Idempotent — re-call after NVS change to apply.
 *
 * @return ESP_OK on either backend becoming ready;
 *         ESP_ERR_TIMEOUT on USB selected and not enumerated yet (UART
 *         fallback in place).
 */
esp_err_t voice_xport_init(uint32_t ready_wait_ms);

/** @brief Current active transport. */
voice_xport_kind_t voice_xport_kind(void);

/** @brief Human-readable name of the active transport. */
const char *voice_xport_name(void);

/** @brief Whether the active transport is ready for traffic. */
bool voice_xport_is_ready(void);

/** Send / recv / lock — dispatch to the active backend.  Surface mirrors
 *  `tab5_port_c_*` (which mirrors `voice_usb_cdc_*`). */
int voice_xport_send(const void *buf, size_t len);
int voice_xport_recv(void *buf, size_t len, uint32_t timeout_ms);
void voice_xport_flush(void);
esp_err_t voice_xport_lock(uint32_t timeout_ms);
void voice_xport_unlock(void);

#ifdef __cplusplus
}
#endif
