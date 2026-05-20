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
   if (s_rx == NULL || data == NULL || data_len == 0) return true;
   size_t pushed = xStreamBufferSend(s_rx, data, data_len, 0);
   if (pushed < data_len) {
      /* RX overrun — drop the rest.  Worst-case this is one corrupt
       * StackFlow frame which the JSON parser will reject; next frame
       * lands clean.  Logged at warn level once per overrun event. */
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

static void connect_watcher_task(void *arg) {
   (void)arg;
   ESP_LOGI(TAG, "connect_watcher_task running — polling for K144 (vid=0x%04X pid=0x%04X intf=%d)", VOICE_USB_CDC_K144_VID,
            VOICE_USB_CDC_K144_PID, VOICE_USB_CDC_K144_INTERFACE);

   const cdc_acm_host_device_config_t dev_cfg = {
       .connection_timeout_ms = 5000,
       .out_buffer_size = 4096,
       .in_buffer_size = 4096,
       .event_cb = handle_event,
       .data_cb = handle_rx,
       .user_arg = NULL,
   };

   while (1) {
      if (s_connected) {
         /* Wait for disconnect notification */
         if (s_disconnect_sem) {
            xSemaphoreTake(s_disconnect_sem, portMAX_DELAY);
         } else {
            vTaskDelay(pdMS_TO_TICKS(K144_CONNECT_RETRY_MS));
         }
         continue;
      }

      cdc_acm_dev_hdl_t hdl = NULL;
      esp_err_t err = cdc_acm_host_open(VOICE_USB_CDC_K144_VID, VOICE_USB_CDC_K144_PID, VOICE_USB_CDC_K144_INTERFACE,
                                        &dev_cfg, &hdl);
      if (err == ESP_OK) {
         xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
         s_dev = hdl;
         s_connected = true;
         xSemaphoreGiveRecursive(s_lock);
         ESP_LOGI(TAG, "K144 CDC-ACM opened");
         continue;
      }
      /* ESP_ERR_NOT_FOUND or timeout — wait and try again */
      vTaskDelay(pdMS_TO_TICKS(K144_CONNECT_RETRY_MS));
   }
}

esp_err_t voice_usb_cdc_init(void) {
   if (s_initialized) return ESP_OK;

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

   /* USB host library */
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

   /* CDC-ACM driver */
   const cdc_acm_host_driver_config_t drv_cfg = {
       .driver_task_stack_size = 4096,
       .driver_task_priority = 11,
       .xCoreID = tskNO_AFFINITY,
       .new_dev_cb = NULL,
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
