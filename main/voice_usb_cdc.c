/**
 * @file voice_usb_cdc.c
 * @brief Implementation — see voice_usb_cdc.h for API contract.
 *
 * Architecture:
 *
 *    app  ─► voice_usb_cdc_send  ─► cdc_acm_host_data_tx_blocking
 *    app  ─► voice_usb_cdc_recv  ◄─── stream_buffer ◄─── handle_rx (cb)
 *
 *    usb_lib_task        — drains usb_host_lib_handle_events forever
 *    connect_watcher     — polls cdc_acm_host_open until K144 enumerates;
 *                          reopens on disconnect events.
 *
 * Threading:
 *  - All cdc_acm_host_* TX / config calls go through s_lock (recursive
 *    mutex) so concurrent callers don't race.
 *  - RX runs from the cdc_acm_host data callback into the stream buffer;
 *    drain happens lock-free against the same buffer.
 *
 * TT #620 W2.
 */

#include "voice_usb_cdc.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "io_expander.h" /* tab5_set_usb_5v_en — TT #620 force-assert USB-A 5V */
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

static const char *TAG = "voice_usb_cdc";

#define USB_LIB_TASK_STACK 4096
#define USB_LIB_TASK_PRIO 20
#define WATCHER_TASK_STACK 4096
#define WATCHER_TASK_PRIO 10

/* Big enough to hold the largest StackFlow frame K144 emits (LLM TTS
 * binary chunks can be a few KB).  Stream buffer in PSRAM so we don't
 * burn internal SRAM. */
#define RX_RING_BYTES (16 * 1024)
#define RX_TRIGGER 1

#define K144_CONNECT_RETRY_MS 1000
#define K144_TX_TIMEOUT_MS 1500

static volatile bool s_initialized = false;
static volatile bool s_connected = false;
static SemaphoreHandle_t s_lock = NULL;
static StreamBufferHandle_t s_rx = NULL;
static cdc_acm_dev_hdl_t s_dev = NULL;
static TaskHandle_t s_lib_task = NULL;
static TaskHandle_t s_watcher_task = NULL;
static SemaphoreHandle_t s_disconnect_sem = NULL;

/* RX callback — called from cdc_acm_host's RX task.  Push into the
 * stream buffer with zero copy beyond what FreeRTOS requires.  Return
 * true so cdc_acm_host treats the data as consumed. */
static bool handle_rx(const uint8_t *data, size_t data_len, void *arg) {
   (void)arg;
   /* ALWAYS log entry — catches ZLPs (data_len=0) which would otherwise
    * be invisible.  If this never fires at all, the cdc_acm_host bulk-IN
    * transfer never completes (or never gets submitted). */
   ESP_LOGI(TAG, "rx callback fired: data=%p len=%u", data, (unsigned)data_len);
   if (s_rx == NULL || data == NULL || data_len == 0) return true;
   size_t pushed = xStreamBufferSend(s_rx, data, data_len, 0);
   ESP_LOGI(TAG, "rx %u bytes (pushed %u): %.*s", (unsigned)data_len, (unsigned)pushed,
            (int)(data_len > 80 ? 80 : data_len), (const char *)data);
   if (pushed < data_len) {
      ESP_LOGW(TAG, "rx ring overrun: %u/%u dropped", (unsigned)(data_len - pushed), (unsigned)data_len);
   }
   return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx) {
   (void)user_ctx;
   switch (event->type) {
      case CDC_ACM_HOST_ERROR:
         ESP_LOGE(TAG, "cdc-acm error: %d", event->data.error);
         break;
      case CDC_ACM_HOST_DEVICE_DISCONNECTED:
         ESP_LOGW(TAG, "K144 USB disconnect");
         s_connected = false;
         /* close + watcher will reopen on next enum */
         if (event->data.cdc_hdl) {
            (void)cdc_acm_host_close(event->data.cdc_hdl);
         }
         s_dev = NULL;
         if (s_disconnect_sem) xSemaphoreGive(s_disconnect_sem);
         break;
      case CDC_ACM_HOST_SERIAL_STATE:
         ESP_LOGI(TAG, "serial state 0x%04X", event->data.serial_state.val);
         break;
      case CDC_ACM_HOST_NETWORK_CONNECTION:
      default:
         ESP_LOGD(TAG, "cdc-acm event type=%d", event->type);
         break;
   }
}

static void usb_lib_task(void *arg) {
   (void)arg;
   ESP_LOGI(TAG, "usb_lib_task running");
   while (1) {
      uint32_t flags;
      esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
      if (err != ESP_OK) {
         ESP_LOGW(TAG, "usb_host_lib_handle_events: %s", esp_err_to_name(err));
         vTaskDelay(pdMS_TO_TICKS(100));
         continue;
      }
      if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
         (void)usb_host_device_free_all();
      }
   }
}

/* Track whether the K144 has been seen on the bus since the last
 * successful open.  The new-device callback flips s_k144_seen true; if
 * the watcher then keeps failing to open it, we know the host has
 * stale state and we kick a usb_host_device_free_all() to flush.
 *
 * Without this, a Tab5 reflash while K144 USB-C stays plugged in
 * leaves the host with a stale device entry that cdc_acm_host_open
 * can't bind to — user used to have to manually unplug+replug the
 * cable to recover.  TT #620 W4 stability fix. */
static volatile bool s_k144_seen_on_bus = false;

static void log_any_new_device(usb_device_handle_t usb_dev) {
   const usb_device_desc_t *desc = NULL;
   if (usb_host_get_device_descriptor(usb_dev, &desc) == ESP_OK && desc) {
      ESP_LOGI(TAG, "*** USB device enumerated: vid=0x%04X pid=0x%04X bcdDevice=0x%04X class=0x%02X", desc->idVendor,
               desc->idProduct, desc->bcdDevice, desc->bDeviceClass);
      if (desc->idVendor == VOICE_USB_CDC_K144_VID && desc->idProduct == VOICE_USB_CDC_K144_PID) {
         s_k144_seen_on_bus = true;
      }
   } else {
      ESP_LOGI(TAG, "*** USB device enumerated (descriptor read failed)");
   }
}

static void connect_watcher_task(void *arg) {
   (void)arg;
   ESP_LOGI(TAG, "connect_watcher_task running — polling for K144 (vid=0x%04X pid=0x%04X intf=%d)",
            VOICE_USB_CDC_K144_VID, VOICE_USB_CDC_K144_PID, VOICE_USB_CDC_K144_INTERFACE);

   /* connection_timeout_ms = 200 — fast retries.
    * in_buffer_size = 4096 — restored after the 512 experiment didn't
    * help.  USB bulk transfers complete on short packets regardless of
    * buffer size, so 4 KB is fine. */
   const cdc_acm_host_device_config_t dev_cfg = {
       .connection_timeout_ms = 200,
       .out_buffer_size = 4096,
       .in_buffer_size = 4096,
       .event_cb = handle_event,
       .data_cb = handle_rx,
       .user_arg = NULL,
   };

   int iters = 0;
   int stale_open_fails = 0;
   while (1) {
      if (s_connected) {
         if (s_disconnect_sem) {
            xSemaphoreTake(s_disconnect_sem, portMAX_DELAY);
         } else {
            vTaskDelay(pdMS_TO_TICKS(K144_CONNECT_RETRY_MS));
         }
         /* On disconnect, the device descriptor is no longer valid;
          * reset the "seen" flag so the recovery path doesn't false-fire
          * on the next iteration. */
         s_k144_seen_on_bus = false;
         stale_open_fails = 0;
         continue;
      }

      /* TT #621 — try interface 1 first (composite ADB+CDC+UAC1 layout
       * where ADB is intf 0, CDC Comm is intf 1).  Fallback to intf 0
       * for CDC-only gadget layouts.  cdc_acm_host_open returns
       * NOT_FOUND fast for wrong interface so the fallback is cheap. */
      cdc_acm_dev_hdl_t hdl = NULL;
      esp_err_t err = cdc_acm_host_open(VOICE_USB_CDC_K144_VID, VOICE_USB_CDC_K144_PID,
                                        /*interface_idx=*/VOICE_USB_CDC_K144_INTERFACE, &dev_cfg, &hdl);
      if (err != ESP_OK) {
         err = cdc_acm_host_open(VOICE_USB_CDC_K144_VID, VOICE_USB_CDC_K144_PID,
                                 /*interface_idx=*/0, &dev_cfg, &hdl);
      }
      if (err == ESP_OK) {
         /* TT #621 — SET_INTERFACE on alt 0 caused first BULK IN URB
          * to STATUS_ERROR and chasing the halt-clear didn't end up
          * delivering data either.  Skip the explicit SET_INTERFACE.
          * Tab5's interface_claim already implicitly activates alt 0;
          * Linux f_acm should be happy with that.  See vendor patch
          * for endpoint_clear-on-error if STATUS_ERROR shows up
          * spontaneously. */

         /* TT #621: force DTR transition 0→1 instead of static 1.
          * Linux f_acm's port_open flag is set on the rising edge of
          * DTR, not on its level.  If a previous host session left
          * DTR=1 in the gadget's state, setting DTR=1 again is a
          * no-op and port_open stays unset → /dev/ttyGSn doesn't get
          * OUT data. */
         (void)cdc_acm_host_set_control_line_state(hdl, /*dtr=*/false, /*rts=*/false);
         vTaskDelay(pdMS_TO_TICKS(50));
         esp_err_t cls = cdc_acm_host_set_control_line_state(hdl, /*dtr=*/true, /*rts=*/true);
         if (cls != ESP_OK) {
            ESP_LOGW(TAG, "set_control_line_state(DTR=1,RTS=1): %s — continuing anyway", esp_err_to_name(cls));
         } else {
            ESP_LOGI(TAG, "DTR transitioned 0→1 (forced edge for f_acm port_open)");
         }
         /* TT #621 — send SET_LINE_CODING with conventional values.
          * Linux ttyACM driver does this on open; some f_acm gadgets
          * require it to fully transition into "active" state and
          * start servicing bulk IN reads.  Use 9600 8N1 (default values
          * any tty driver accepts) instead of the experimental 1.5 Mbps
          * we tried earlier. */
         const cdc_acm_line_coding_t coding = {
             .dwDTERate = 9600,
             .bCharFormat = 0, /* 1 stop bit */
             .bParityType = 0, /* none */
             .bDataBits = 8,
         };
         esp_err_t lc = cdc_acm_host_line_coding_set(hdl, &coding);
         if (lc != ESP_OK) {
            ESP_LOGW(TAG, "line_coding_set 9600 8N1: %s — continuing", esp_err_to_name(lc));
         } else {
            ESP_LOGI(TAG, "line coding set to 9600 8N1");
         }

         xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
         s_dev = hdl;
         s_connected = true;
         xSemaphoreGiveRecursive(s_lock);
         stale_open_fails = 0;
         ESP_LOGI(TAG, "K144 CDC-ACM opened (DTR=1 RTS=1, line coding set)");
         continue;
      }

      /* Auto-recovery: if K144 enumerated on the bus (new_dev_cb fired)
       * but cdc_acm_host_open keeps failing, the host has stale device
       * state — typically left over from a Tab5 reflash while K144's
       * USB-C stayed plugged in.  After 3 consecutive failures with
       * device seen on bus, flush all USB host devices to force fresh
       * enumeration on the next host event loop iteration.  K144's UDC
       * watchdog (ax_usb_tinker_event.sh) re-binds the gadget within
       * ~1 s so the device re-enumerates promptly. */
      if (s_k144_seen_on_bus) {
         stale_open_fails++;
         if (stale_open_fails == 3) {
            ESP_LOGW(TAG, "K144 enumerated but open keeps failing — flushing USB host state");
            (void)usb_host_device_free_all();
            s_k144_seen_on_bus = false;
            stale_open_fails = 0;
         }
      }

      /* Every 10 s log what we're seeing — distinguishes "polling but
       * bus is empty" from "polling and rejected wrong device". */
      if ((iters++ % 10) == 0) {
         ESP_LOGI(TAG, "watcher: cdc_acm_host_open → %s (k144_seen=%d, stale_fails=%d)", esp_err_to_name(err),
                  (int)s_k144_seen_on_bus, stale_open_fails);
      }
      vTaskDelay(pdMS_TO_TICKS(K144_CONNECT_RETRY_MS));
   }
}

esp_err_t voice_usb_cdc_init(void) {
   if (s_initialized) return ESP_OK;

   /* TT #620 #621 debug — global LOG_MAX is INFO so DEBUG calls inside
    * USB host driver ISR contexts get compiled out (one such path blew
    * the interrupt watchdog).  Our own LOGI lines on handle_rx remain
    * — those run in cdc_acm_client_task context (safe). */

   /* Force-assert Tab5's USB-A 5V rail before the host stack comes up.
    * io_expander.c sets PI4IOE2 P3 = high in tab5_io_expander_init, but
    * something later in the boot may clobber the OUT_SET register; do
    * one belt-and-braces toggle here so the VBUS to any attached
    * device is definitely live by the time the host PHY enables.  Log
    * the readback so the diagnostic surface tells us whether the bit
    * actually latched. */
   tab5_set_usb_5v_en(true);
   bool usb5v = tab5_get_usb_5v_en();
   ESP_LOGI(TAG, "USB-A 5V state (PI4IOE2 P3) = %s", usb5v ? "HIGH (ok)" : "LOW (BAD — VBUS dead)");

   if (s_lock == NULL) {
      s_lock = xSemaphoreCreateRecursiveMutex();
      if (!s_lock) return ESP_ERR_NO_MEM;
   }
   if (s_disconnect_sem == NULL) {
      s_disconnect_sem = xSemaphoreCreateBinary();
      if (!s_disconnect_sem) return ESP_ERR_NO_MEM;
   }
   if (s_rx == NULL) {
      /* Stream buffer header lives in internal RAM but the payload ring
       * is small — keep it on the regular heap.  16 KB is fine. */
      s_rx = xStreamBufferCreate(RX_RING_BYTES, RX_TRIGGER);
      if (!s_rx) return ESP_ERR_NO_MEM;
   }

   /* USB host library — default peripheral_map (BIT0 = HS controller).
    *
    * ESP32-P4 has two USB-OTG controllers:
    *   [0] HS — dedicated USB_DP_HS / USB_DM_HS chip pads → Tab5 USB-A jack
    *   [1] FS — GPIO 26/27 — shared with USB Serial/JTAG (Tab5 USB-C console)
    *
    * M5Tab5-UserDemo (M5Stack reference firmware) uses HS for its USB
    * keyboard/mouse host code (default peripheral_map).  We match.
    *
    * USB Serial/JTAG console (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y) lives
    * on the FS USB pads through Tab5's USB-C — different block from
    * USB-OTG-HS, so they don't conflict.
    */
   const usb_host_config_t host_cfg = {
       .skip_phy_setup = false,
       .intr_flags = ESP_INTR_FLAG_LEVEL1,
   };
   esp_err_t err = usb_host_install(&host_cfg);
   if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
      return err;
   }

   BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", USB_LIB_TASK_STACK, NULL, USB_LIB_TASK_PRIO,
                                           &s_lib_task, tskNO_AFFINITY);
   if (ok != pdPASS) {
      ESP_LOGE(TAG, "usb_lib task create failed");
      return ESP_ERR_NO_MEM;
   }

   /* CDC-ACM driver — register a generic new-device callback so we log
    * EVERY USB device that enumerates on Tab5's host bus, regardless of
    * VID/PID.  Diagnostic for the W2 bring-up: tells us whether the
    * host PHY is actually seeing anything on the wire. */
   const cdc_acm_host_driver_config_t drv_cfg = {
       .driver_task_stack_size = 4096,
       .driver_task_priority = 11,
       .xCoreID = tskNO_AFFINITY,
       .new_dev_cb = log_any_new_device,
   };
   err = cdc_acm_host_install(&drv_cfg);
   if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "cdc_acm_host_install: %s", esp_err_to_name(err));
      return err;
   }

   ok = xTaskCreatePinnedToCore(connect_watcher_task, "cdc_watcher", WATCHER_TASK_STACK, NULL, WATCHER_TASK_PRIO,
                                &s_watcher_task, tskNO_AFFINITY);
   if (ok != pdPASS) {
      ESP_LOGE(TAG, "cdc_watcher task create failed");
      return ESP_ERR_NO_MEM;
   }

   s_initialized = true;
   ESP_LOGI(TAG, "voice_usb_cdc init OK — watcher polling for K144");
   return ESP_OK;
}

void voice_usb_cdc_deinit(void) {
   if (!s_initialized) return;
   if (s_watcher_task) {
      vTaskDelete(s_watcher_task);
      s_watcher_task = NULL;
   }
   if (s_dev) {
      (void)cdc_acm_host_close(s_dev);
      s_dev = NULL;
   }
   s_connected = false;
   (void)cdc_acm_host_uninstall();
   if (s_lib_task) {
      vTaskDelete(s_lib_task);
      s_lib_task = NULL;
   }
   (void)usb_host_uninstall();
   s_initialized = false;
}

bool voice_usb_cdc_is_connected(void) { return s_connected; }
bool voice_usb_cdc_is_initialized(void) { return s_initialized; }

int voice_usb_cdc_send(const void *buf, size_t len) {
   if (!s_initialized || buf == NULL || len == 0) return -1;
   if (xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(K144_TX_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGW(TAG, "send: lock busy");
      return -1;
   }
   if (!s_connected || s_dev == NULL) {
      xSemaphoreGiveRecursive(s_lock);
      return -1;
   }
   esp_err_t err = cdc_acm_host_data_tx_blocking(s_dev, (const uint8_t *)buf, len, K144_TX_TIMEOUT_MS);
   xSemaphoreGiveRecursive(s_lock);
   if (err != ESP_OK) {
      ESP_LOGW(TAG, "tx_blocking: %s", esp_err_to_name(err));
      return -1;
   }
   /* TT #620 W4 diagnostic: log TX with first 80 bytes preview. */
   ESP_LOGI(TAG, "tx %u bytes: %.*s", (unsigned)len, (int)(len > 80 ? 80 : len), (const char *)buf);
   return (int)len;
}

int voice_usb_cdc_recv(void *buf, size_t len, uint32_t timeout_ms) {
   if (!s_initialized || buf == NULL || len == 0) return -1;
   if (s_rx == NULL) return -1;
   size_t got = xStreamBufferReceive(s_rx, buf, len, pdMS_TO_TICKS(timeout_ms));
   return (int)got;
}

void voice_usb_cdc_flush(void) {
   if (s_rx) xStreamBufferReset(s_rx);
}

esp_err_t voice_usb_cdc_lock(uint32_t timeout_ms) {
   if (!s_initialized || s_lock == NULL) return ESP_ERR_INVALID_STATE;
   const TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
   return (xSemaphoreTakeRecursive(s_lock, ticks) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void voice_usb_cdc_unlock(void) {
   if (s_lock) xSemaphoreGiveRecursive(s_lock);
}
