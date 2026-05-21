/**
 * @file voice_usb_cdc.h
 * @brief USB Host CDC-ACM driver for the Tab5↔K144 control transport.
 *
 * Tab5 boots its USB-OTG-HS controller as a USB host (USB-A jack on the
 * bezel, 5 V gated by PI4IOE2 P3 — enabled at boot in
 * `bsp/tab5/io_expander.c`).  K144's top USB-C exposes a composite
 * gadget (ADB + CDC-ACM + UAC1, see `scripts/k144/usb-tinker.sh`).  We
 * open the CDC-ACM endpoint at interface 1 and replace the M5-Bus UART
 * as the StackFlow JSON channel.
 *
 * API surface mirrors `bsp/tab5/uart_port_c.h` (init / send / recv /
 * lock / unlock / flush / is_initialized) so the W3 transport
 * abstraction (`voice_xport.{c,h}`) can route either side blind.
 *
 * TT #620 W2.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** K144 composite gadget identifiers (see scripts/k144/usb-tinker.sh). */
#define VOICE_USB_CDC_K144_VID 0x32c9
#define VOICE_USB_CDC_K144_PID 0x2003

/** Interface index of the CDC-ACM control interface within the K144
 *  composite.  Layout (verified live 2026-05-20):
 *
 *    intf 0  Vendor Specific (FFS ADB)
 *    intf 1  CDC Communications  ← we want this
 *    intf 2  CDC Data
 *    intf 3+ UAC1 Audio (separate driver)
 *
 *  Pass to `cdc_acm_host_open(vid, pid, interface_idx=1, ...)`.
 */
#define VOICE_USB_CDC_K144_INTERFACE 1

/**
 * @brief Initialise USB host stack + CDC-ACM driver.
 *
 * Idempotent.  Installs the USB host library, spawns the lib event task,
 * installs the cdc_acm_host driver, and starts a connect-watcher task
 * that opens K144 when it enumerates (and reopens on reconnect).  Does
 * NOT block — caller can carry on; `voice_usb_cdc_is_connected()` flips
 * true once K144 is up.
 *
 * @return ESP_OK on success;
 *         ESP_ERR_NO_MEM / ESP_ERR_INVALID_STATE on stack failure.
 */
esp_err_t voice_usb_cdc_init(void);

/** @brief Tear down — close device, uninstall drivers.  Safe when not
 *         running.  Mirror of tab5_port_c_uart_deinit. */
void voice_usb_cdc_deinit(void);

/** @brief Whether the K144 CDC-ACM endpoint is currently open. */
bool voice_usb_cdc_is_connected(void);

/** @brief True after voice_usb_cdc_init has succeeded, regardless of
 *         whether the device is currently connected.  Mirror of
 *         tab5_port_c_uart_is_initialized. */
bool voice_usb_cdc_is_initialized(void);

/**
 * @brief Send bytes to K144 over CDC-ACM.  Blocking, with timeout.
 *
 * Mirror of `tab5_port_c_send`.  Returns the number of bytes sent (==
 * @p len on success).  -1 on error or device disconnected.
 *
 * @param buf  Bytes to send (must be in DMA-capable memory; the driver
 *             copies to its own DMA buffer internally).
 * @param len  Length in bytes.
 */
int voice_usb_cdc_send(const void *buf, size_t len);

/**
 * @brief Read up to @p len bytes from the internal RX stream buffer.
 *
 * Mirror of `tab5_port_c_recv`.  CDC RX bytes are pushed into a 16 KB
 * FreeRTOS stream buffer by the cdc_acm_host data callback.  This call
 * drains the buffer with a timeout.
 *
 * @return  Bytes read (0 .. len), or 0 on timeout / disconnect.
 */
int voice_usb_cdc_recv(void *buf, size_t len, uint32_t timeout_ms);

/** @brief Discard everything currently buffered (mirror flush). */
void voice_usb_cdc_flush(void);

/** @brief Take the per-device send/recv mutex.  Recursive — same task
 *         may take it multiple times.  Mirror of tab5_port_c_lock. */
esp_err_t voice_usb_cdc_lock(uint32_t timeout_ms);

/** @brief Release one level of the lock.  Mirror of tab5_port_c_unlock. */
void voice_usb_cdc_unlock(void);

/* `_set_baud` / `_get_baud` are intentionally NOT mirrored — USB has no
 * baud rate.  The transport abstraction layer (W3) returns ESP_OK for
 * set_baud and the negotiated USB HS rate for get_baud. */

#ifdef __cplusplus
}
#endif
