/**
 * @file voice_xport.c
 * @brief Implementation — see voice_xport.h for API contract.
 *
 * TT #620 W3 + TT #621 (USB_FFS variant).
 */

#include "voice_xport.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"      /* tab5_settings_get_xport */
#include "uart_port_c.h"   /* tab5_port_c_* (legacy UART backend) */
#include "voice_usb_cdc.h" /* CDC-ACM backend (TT #620, broken bulk OUT) */
#include "voice_usb_ffs.h" /* vendor-class bulk backend (TT #621 escape hatch) */

static const char *TAG = "voice_xport";

static voice_xport_kind_t s_kind = VOICE_XPORT_UART;

const char *voice_xport_name(void) {
   switch (s_kind) {
      case VOICE_XPORT_USB_FFS:
         return "usb_ffs";
      case VOICE_XPORT_USB_CDC:
         return "usb_cdc";
      case VOICE_XPORT_UART:
      default:
         return "uart";
   }
}

voice_xport_kind_t voice_xport_kind(void) { return s_kind; }

esp_err_t voice_xport_init(uint32_t ready_wait_ms) {
   uint8_t want = tab5_settings_get_xport();
   voice_xport_kind_t want_kind;
   switch (want) {
      case 2:
         want_kind = VOICE_XPORT_USB_FFS;
         break;
      case 1:
         want_kind = VOICE_XPORT_USB_CDC;
         break;
      default:
         want_kind = VOICE_XPORT_UART;
         break;
   }

   if (want_kind == VOICE_XPORT_USB_FFS) {
      uint32_t waited = 0;
      while (waited < ready_wait_ms && !voice_usb_ffs_is_connected()) {
         vTaskDelay(pdMS_TO_TICKS(100));
         waited += 100;
      }
      if (voice_usb_ffs_is_connected()) {
         s_kind = VOICE_XPORT_USB_FFS;
         ESP_LOGI(TAG, "active transport: usb_ffs (K144 in %lu ms)", (unsigned long)waited);
         return ESP_OK;
      }
      ESP_LOGW(TAG, "xport=usb_ffs requested but K144 not enumerated in %lu ms — falling back to uart",
               (unsigned long)ready_wait_ms);
   } else if (want_kind == VOICE_XPORT_USB_CDC) {
      uint32_t waited = 0;
      while (waited < ready_wait_ms && !voice_usb_cdc_is_connected()) {
         vTaskDelay(pdMS_TO_TICKS(100));
         waited += 100;
      }
      if (voice_usb_cdc_is_connected()) {
         s_kind = VOICE_XPORT_USB_CDC;
         ESP_LOGI(TAG, "active transport: usb_cdc (K144 in %lu ms)", (unsigned long)waited);
         return ESP_OK;
      }
      ESP_LOGW(TAG, "xport=usb_cdc requested but K144 not enumerated in %lu ms — falling back to uart",
               (unsigned long)ready_wait_ms);
   }

   s_kind = VOICE_XPORT_UART;
   ESP_LOGI(TAG, "active transport: uart (M5-Bus Port C)");
   return ESP_OK;
}

bool voice_xport_is_ready(void) {
   switch (s_kind) {
      case VOICE_XPORT_USB_FFS:
         return voice_usb_ffs_is_connected();
      case VOICE_XPORT_USB_CDC:
         return voice_usb_cdc_is_connected();
      case VOICE_XPORT_UART:
      default:
         return tab5_port_c_uart_is_initialized();
   }
}

int voice_xport_send(const void *buf, size_t len) {
   switch (s_kind) {
      case VOICE_XPORT_USB_FFS:
         return voice_usb_ffs_send(buf, len);
      case VOICE_XPORT_USB_CDC:
         return voice_usb_cdc_send(buf, len);
      default:
         return tab5_port_c_send(buf, len);
   }
}

int voice_xport_recv(void *buf, size_t len, uint32_t timeout_ms) {
   switch (s_kind) {
      case VOICE_XPORT_USB_FFS:
         return voice_usb_ffs_recv(buf, len, timeout_ms);
      case VOICE_XPORT_USB_CDC:
         return voice_usb_cdc_recv(buf, len, timeout_ms);
      default:
         return tab5_port_c_recv(buf, len, timeout_ms);
   }
}

void voice_xport_flush(void) {
   switch (s_kind) {
      case VOICE_XPORT_USB_FFS:
         voice_usb_ffs_flush();
         return;
      case VOICE_XPORT_USB_CDC:
         voice_usb_cdc_flush();
         return;
      default:
         tab5_port_c_flush();
         return;
   }
}

esp_err_t voice_xport_lock(uint32_t timeout_ms) {
   switch (s_kind) {
      case VOICE_XPORT_USB_FFS:
         return voice_usb_ffs_lock(timeout_ms);
      case VOICE_XPORT_USB_CDC:
         return voice_usb_cdc_lock(timeout_ms);
      default:
         return tab5_port_c_lock(timeout_ms);
   }
}

void voice_xport_unlock(void) {
   switch (s_kind) {
      case VOICE_XPORT_USB_FFS:
         voice_usb_ffs_unlock();
         return;
      case VOICE_XPORT_USB_CDC:
         voice_usb_cdc_unlock();
         return;
      default:
         tab5_port_c_unlock();
         return;
   }
}
