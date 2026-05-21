/**
 * @file voice_usb_ffs.c
 * @brief Implementation — see voice_usb_ffs.h.
 *
 * One USB host client + watcher claims TWO independent vendor-class
 * interfaces on the same K144 device:
 *
 *   channel CONTROL  (subclass 0x44) → ffs.control on K144
 *   channel VIDEO    (subclass 0x43) → ffs.video on K144
 *
 * Each channel has its own:
 *   - interface number + bulk OUT/IN endpoint addresses (discovered at
 *     claim time; not hardcoded)
 *   - mutex (callers can hold control while video is in flight, etc.)
 *   - RX stream buffer + persistent bulk IN transfer
 *
 * The dual-CLIENT variant tried earlier blew Tab5's internal SRAM
 * (TT #621 W6 audit: free=1KB, largest=0KB after second client init).
 * One client with two interface claims keeps the per-device tracking
 * overhead flat.
 *
 * TT #621 (W5: control) + TT #621 W6 (video).
 */

#include "voice_usb_ffs.h"

#include <string.h>

#include "debug_obs.h" /* TT #627 Wave B.3 — xport.reswitch event */
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "io_expander.h"
#include "task_worker.h" /* TT #627 Wave B.3 — schedule voice_xport_init from watcher */
#include "usb/usb_host.h"

static const char *TAG = "voice_usb_ffs";

#define USB_LIB_TASK_STACK 4096
#define USB_LIB_TASK_PRIO 20
#define CLIENT_TASK_STACK 4096
#define CLIENT_TASK_PRIO 15
#define WATCHER_TASK_STACK 4096
#define WATCHER_TASK_PRIO 10

#define IN_XFER_SIZE (4 * 1024)
#define WATCHER_POLL_MS 1000
#define TX_TIMEOUT_MS 3000

#define RX_RING_CTRL_BYTES (16 * 1024)
#define RX_RING_VIDEO_BYTES (32 * 1024) /* yolo responses are bigger */

/* ── Per-channel state ─────────────────────────────────────────────── */

typedef struct {
   const char *name;
   uint8_t subclass;
   size_t rx_ring_bytes;
   size_t out_xfer_size; /* persistent OUT transfer capacity */

   volatile bool connected;
   SemaphoreHandle_t lock;  /* recursive — per-channel TX lock */
   StreamBufferHandle_t rx; /* RX from in_xfer_cb */
   usb_transfer_t *in_xfer;
   usb_transfer_t *out_xfer;   /* persistent — never freed mid-flight */
   SemaphoreHandle_t out_done; /* persistent binary sem; given by cb */
   uint8_t intf_num;
   uint8_t ep_out_addr;
   uint8_t ep_in_addr;
} channel_t;

static channel_t s_ch_ctrl = {
    .name = "ctrl",
    .subclass = VOICE_USB_FFS_K144_SUBCLASS_CONTROL,
    .rx_ring_bytes = RX_RING_CTRL_BYTES,
    /* 8 KB — ext_pcm pump frames are ~4.4 KB (base64 PCM + JSON env);
     * sys.* / asr.setup / llm.setup frames are <1 KB.  4 KB was too
     * tight and silently dropped every audio frame. */
    .out_xfer_size = 8 * 1024,
};
static channel_t s_ch_video = {
    .name = "video",
    .subclass = VOICE_USB_FFS_K144_SUBCLASS_VIDEO,
    .rx_ring_bytes = RX_RING_VIDEO_BYTES,
    .out_xfer_size = 48 * 1024, /* yolo base64 JPEG frames */
};

/* ── Shared (one-client) state ─────────────────────────────────────── */

static volatile bool s_initialized = false;
static volatile bool s_lib_installed = false;
static TaskHandle_t s_lib_task = NULL;
static TaskHandle_t s_client_task = NULL;
static TaskHandle_t s_watcher_task = NULL;

static usb_host_client_handle_t s_client = NULL;
static usb_device_handle_t s_dev = NULL;
static SemaphoreHandle_t s_disconnect_sem = NULL;

/* ── USB lib + client event loops ─────────────────────────────────── */

static void usb_lib_task(void *arg) {
   (void)arg;
   while (1) {
      uint32_t flags;
      esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
      if (err != ESP_OK) {
         ESP_LOGW(TAG, "lib_handle_events: %s", esp_err_to_name(err));
         vTaskDelay(pdMS_TO_TICKS(100));
      }
      if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
         (void)usb_host_device_free_all();
      }
   }
}

/* TT #627 Wave B.3 (R9) — track whether we ever had a successful USB
 * claim this session.  Set on first successful claim_channel(ctrl).
 * Read in claim path to decide whether the NEXT successful claim is a
 * RE-claim after a yank, and therefore needs voice_xport_init() to
 * re-evaluate (the boot-time pick may have timed out and fallen back
 * to UART before USB enumerated).  Without this, replug stays on the
 * slower UART for the rest of the session. */
static volatile bool s_had_prior_connect = false;
static volatile bool s_pending_reswitch = false;

static void on_client_event(const usb_host_client_event_msg_t *msg, void *arg) {
   (void)arg;
   switch (msg->event) {
      case USB_HOST_CLIENT_EVENT_NEW_DEV:
         ESP_LOGI(TAG, "client: NEW_DEV addr=%d", msg->new_dev.address);
         break;
      case USB_HOST_CLIENT_EVENT_DEV_GONE:
         ESP_LOGW(TAG, "client: DEV_GONE");
         if (s_ch_ctrl.connected) s_had_prior_connect = true;
         s_ch_ctrl.connected = false;
         s_ch_video.connected = false;
         if (s_disconnect_sem) xSemaphoreGive(s_disconnect_sem);
         break;
      default:
         break;
   }
}

/* TT #627 Wave B.3 (R9) — worker job that re-runs voice_xport_init so
 * the device flips back to usb_ffs after a yank/replug cycle.  Posted
 * from the watcher after a re-claim succeeds. */
static void xport_reswitch_job(void *arg) {
   (void)arg;
   extern esp_err_t voice_xport_init(uint32_t ready_wait_ms);
   tab5_debug_obs_event("xport.reswitch", "begin");
   esp_err_t err = voice_xport_init(2000);
   if (err == ESP_OK) {
      tab5_debug_obs_event("xport.reswitch", "usb_ffs");
      ESP_LOGI(TAG, "xport re-init after USB replug: OK");
   } else {
      tab5_debug_obs_event("xport.reswitch", esp_err_to_name(err));
      ESP_LOGW(TAG, "xport re-init after USB replug: %s", esp_err_to_name(err));
   }
   s_pending_reswitch = false;
}

static void client_task(void *arg) {
   (void)arg;
   while (1) {
      esp_err_t err = usb_host_client_handle_events(s_client, portMAX_DELAY);
      if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
         ESP_LOGW(TAG, "client_handle_events: %s", esp_err_to_name(err));
         vTaskDelay(pdMS_TO_TICKS(50));
      }
   }
}

/* ── Bulk IN: per-channel resubmit + push into stream buffer ──────── */

static void in_xfer_cb(usb_transfer_t *xfer) {
   channel_t *ch = (channel_t *)xfer->context;
   if (!ch) return;

   if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
      size_t pushed = xStreamBufferSend(ch->rx, xfer->data_buffer, xfer->actual_num_bytes, 0);
      if (pushed < (size_t)xfer->actual_num_bytes) {
         ESP_LOGW(TAG, "%s rx overrun: %u dropped", ch->name, (unsigned)(xfer->actual_num_bytes - pushed));
      }
   } else if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      if (xfer->status == USB_TRANSFER_STATUS_STALL) {
         usb_host_endpoint_clear(s_dev, ch->ep_in_addr);
      }
   }
   if (ch->connected && s_dev) {
      xfer->num_bytes = IN_XFER_SIZE;
      esp_err_t err = usb_host_transfer_submit(xfer);
      if (err != ESP_OK) {
         ESP_LOGW(TAG, "%s in resubmit: %s", ch->name, esp_err_to_name(err));
      }
   }
}

/* ── Walk descriptor, fill one channel's intf+EP addrs ─────────────── */

static esp_err_t discover_channel(const usb_config_desc_t *cfg, channel_t *ch) {
   const usb_intf_desc_t *match = NULL;
   for (int i = 0; i < cfg->bNumInterfaces && !match; i++) {
      int ioff = 0;
      const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, i, 0, &ioff);
      if (!intf) continue;
      if (intf->bInterfaceClass == USB_CLASS_VENDOR_SPEC && intf->bInterfaceSubClass == ch->subclass &&
          intf->bInterfaceProtocol == VOICE_USB_FFS_K144_PROTOCOL) {
         match = intf;
      }
   }
   if (!match) {
      ESP_LOGW(TAG, "%s: no vendor-class sub=0x%02x interface in config", ch->name, ch->subclass);
      return ESP_ERR_NOT_FOUND;
   }
   uint8_t ep_out = 0, ep_in = 0;
   for (int e = 0; e < match->bNumEndpoints; e++) {
      int eoff = 0;
      const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(match, e, cfg->wTotalLength, &eoff);
      if (!ep) continue;
      if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK) continue;
      if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK)
         ep_in = ep->bEndpointAddress;
      else
         ep_out = ep->bEndpointAddress;
   }
   if (!ep_out || !ep_in) {
      ESP_LOGW(TAG, "%s: missing bulk EPs (OUT=0x%02x IN=0x%02x)", ch->name, ep_out, ep_in);
      return ESP_ERR_NOT_FOUND;
   }
   ch->intf_num = match->bInterfaceNumber;
   ch->ep_out_addr = ep_out;
   ch->ep_in_addr = ep_in;
   ESP_LOGI(TAG, "%s: intf %u  OUT=0x%02x IN=0x%02x", ch->name, ch->intf_num, ch->ep_out_addr, ch->ep_in_addr);
   return ESP_OK;
}

/* Forward decl — used in claim_channel for the OUT cb. */
static void out_xfer_cb(usb_transfer_t *xfer);

static esp_err_t claim_channel(channel_t *ch) {
   esp_err_t err = usb_host_interface_claim(s_client, s_dev, ch->intf_num, 0);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s claim(%u): %s", ch->name, ch->intf_num, esp_err_to_name(err));
      return err;
   }
   /* Persistent IN transfer. */
   err = usb_host_transfer_alloc(IN_XFER_SIZE, 0, &ch->in_xfer);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s in xfer_alloc: %s", ch->name, esp_err_to_name(err));
      usb_host_interface_release(s_client, s_dev, ch->intf_num);
      return err;
   }
   ch->in_xfer->device_handle = s_dev;
   ch->in_xfer->bEndpointAddress = ch->ep_in_addr;
   ch->in_xfer->callback = in_xfer_cb;
   ch->in_xfer->context = ch;
   ch->in_xfer->num_bytes = IN_XFER_SIZE;

   /* Persistent OUT transfer + done sem.  Reused per send; never freed
    * mid-flight (which the USB host stack explicitly forbids and which
    * caused the burst panic in the first cut). */
   err = usb_host_transfer_alloc(ch->out_xfer_size, 0, &ch->out_xfer);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s out xfer_alloc(%u): %s", ch->name, (unsigned)ch->out_xfer_size, esp_err_to_name(err));
      usb_host_transfer_free(ch->in_xfer);
      ch->in_xfer = NULL;
      usb_host_interface_release(s_client, s_dev, ch->intf_num);
      return err;
   }
   ch->out_xfer->device_handle = s_dev;
   ch->out_xfer->bEndpointAddress = ch->ep_out_addr;
   ch->out_xfer->callback = out_xfer_cb;
   ch->out_xfer->context = ch;
   if (!ch->out_done) ch->out_done = xSemaphoreCreateBinary();

   err = usb_host_transfer_submit(ch->in_xfer);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s initial submit: %s", ch->name, esp_err_to_name(err));
      usb_host_transfer_free(ch->in_xfer);
      ch->in_xfer = NULL;
      usb_host_transfer_free(ch->out_xfer);
      ch->out_xfer = NULL;
      usb_host_interface_release(s_client, s_dev, ch->intf_num);
      return err;
   }
   ch->connected = true;
   ESP_LOGI(TAG, "%s connected — bulk IN armed (out_xfer cap=%u)", ch->name, (unsigned)ch->out_xfer_size);
   return ESP_OK;
}

static void release_channel(channel_t *ch) {
   if (s_dev) {
      if (ch->in_xfer) {
         (void)usb_host_endpoint_halt(s_dev, ch->ep_in_addr);
         (void)usb_host_endpoint_flush(s_dev, ch->ep_in_addr);
      }
      if (ch->out_xfer) {
         (void)usb_host_endpoint_halt(s_dev, ch->ep_out_addr);
         (void)usb_host_endpoint_flush(s_dev, ch->ep_out_addr);
      }
   }
   if (ch->in_xfer) {
      usb_host_transfer_free(ch->in_xfer);
      ch->in_xfer = NULL;
   }
   if (ch->out_xfer) {
      usb_host_transfer_free(ch->out_xfer);
      ch->out_xfer = NULL;
   }
   if (s_dev) usb_host_interface_release(s_client, s_dev, ch->intf_num);
   ch->connected = false;
}

/* ── Open device + claim both channels ─────────────────────────────── */

static esp_err_t open_k144(uint8_t addr) {
   esp_err_t err = usb_host_device_open(s_client, addr, &s_dev);
   if (err != ESP_OK) {
      ESP_LOGW(TAG, "device_open: %s", esp_err_to_name(err));
      return err;
   }
   const usb_config_desc_t *cfg = NULL;
   err = usb_host_get_active_config_descriptor(s_dev, &cfg);
   if (err != ESP_OK || !cfg) {
      ESP_LOGW(TAG, "get_active_config_desc: %s", esp_err_to_name(err));
      usb_host_device_close(s_client, s_dev);
      s_dev = NULL;
      return err;
   }

   /* Control is required.  Video is optional — older gadgets without
    * ffs.video should still get a working control channel. */
   if (discover_channel(cfg, &s_ch_ctrl) != ESP_OK) {
      ESP_LOGE(TAG,
               "control interface not present (sub=0x%02x) — K144 needs "
               "the latest usb-tinker-ffs gadget",
               s_ch_ctrl.subclass);
      usb_host_device_close(s_client, s_dev);
      s_dev = NULL;
      return ESP_ERR_NOT_FOUND;
   }
   bool have_video = (discover_channel(cfg, &s_ch_video) == ESP_OK);

   if (claim_channel(&s_ch_ctrl) != ESP_OK) {
      usb_host_device_close(s_client, s_dev);
      s_dev = NULL;
      return ESP_FAIL;
   }
   if (have_video) {
      if (claim_channel(&s_ch_video) != ESP_OK) {
         ESP_LOGW(TAG, "video claim failed; continuing with control only");
      }
   }

   /* TT #627 Wave B.3 (R9) — if this is a RE-claim after a yank, the
    * voice_xport layer is probably still on its UART fallback (it picked
    * at boot only).  Post a worker job to re-run voice_xport_init so we
    * flip back to usb_ffs.  Idempotent — guarded by s_pending_reswitch
    * so multiple claim cycles don't queue multiple jobs. */
   if (s_had_prior_connect && !s_pending_reswitch) {
      s_pending_reswitch = true;
      esp_err_t we = tab5_worker_enqueue(xport_reswitch_job, NULL, "xport_resw");
      if (we != ESP_OK) {
         ESP_LOGW(TAG, "xport_reswitch_job enqueue failed: %s", esp_err_to_name(we));
         s_pending_reswitch = false;
      }
   }
   return ESP_OK;
}

static void close_k144(void) {
   release_channel(&s_ch_video);
   release_channel(&s_ch_ctrl);
   if (s_dev) {
      usb_host_device_close(s_client, s_dev);
      s_dev = NULL;
   }
}

/* ── Watcher: poll for K144 ──────────────────────────────────────── */

static void watcher_task(void *arg) {
   (void)arg;
   while (1) {
      if (!s_ch_ctrl.connected) {
         uint8_t addrs[16];
         int num = 0;
         if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &num) == ESP_OK) {
            for (int i = 0; i < num; i++) {
               usb_device_handle_t tmp = NULL;
               if (usb_host_device_open(s_client, addrs[i], &tmp) != ESP_OK) continue;
               const usb_device_desc_t *dd = NULL;
               bool match = false;
               if (usb_host_get_device_descriptor(tmp, &dd) == ESP_OK && dd) {
                  match = (dd->idVendor == VOICE_USB_FFS_K144_VID && dd->idProduct == VOICE_USB_FFS_K144_PID);
               }
               usb_host_device_close(s_client, tmp);
               if (match) {
                  xSemaphoreTakeRecursive(s_ch_ctrl.lock, portMAX_DELAY);
                  xSemaphoreTakeRecursive(s_ch_video.lock, portMAX_DELAY);
                  esp_err_t er = open_k144(addrs[i]);
                  xSemaphoreGiveRecursive(s_ch_video.lock);
                  xSemaphoreGiveRecursive(s_ch_ctrl.lock);
                  if (er == ESP_OK) break;
               }
            }
         }
      } else if (s_disconnect_sem && xSemaphoreTake(s_disconnect_sem, pdMS_TO_TICKS(WATCHER_POLL_MS)) == pdTRUE) {
         xSemaphoreTakeRecursive(s_ch_ctrl.lock, portMAX_DELAY);
         xSemaphoreTakeRecursive(s_ch_video.lock, portMAX_DELAY);
         close_k144();
         xSemaphoreGiveRecursive(s_ch_video.lock);
         xSemaphoreGiveRecursive(s_ch_ctrl.lock);
         continue;
      }
      vTaskDelay(pdMS_TO_TICKS(WATCHER_POLL_MS));
   }
}

/* ── Per-channel send/recv helpers ─────────────────────────────────── */

/* OUT completion — give the per-channel done sem.  Persistent; we never
 * free the transfer or sem from here.  Safe under callback rules. */
static void out_xfer_cb(usb_transfer_t *xfer) {
   channel_t *ch = (channel_t *)xfer->context;
   if (ch && ch->out_done) xSemaphoreGive(ch->out_done);
}

static int channel_send(channel_t *ch, const void *buf, size_t len) {
   if (!ch->connected || !s_dev || !buf || len == 0) return -1;
   if (!ch->out_xfer || !ch->out_done) return -1;
   if (len > ch->out_xfer_size) {
      ESP_LOGW(TAG, "%s tx oversize %u > %u", ch->name, (unsigned)len, (unsigned)ch->out_xfer_size);
      return -1;
   }

   xSemaphoreTakeRecursive(ch->lock, portMAX_DELAY);

   /* Drop any stale completion the kernel might have given the done sem
    * after a prior timeout. */
   (void)xSemaphoreTake(ch->out_done, 0);

   memcpy(ch->out_xfer->data_buffer, buf, len);
   ch->out_xfer->num_bytes = len;

   int written = -1;
   esp_err_t err = usb_host_transfer_submit(ch->out_xfer);
   if (err != ESP_OK) {
      ESP_LOGW(TAG, "%s tx submit: %s", ch->name, esp_err_to_name(err));
      xSemaphoreGiveRecursive(ch->lock);
      return -1;
   }
   if (xSemaphoreTake(ch->out_done, pdMS_TO_TICKS(TX_TIMEOUT_MS)) == pdTRUE) {
      if (ch->out_xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
         written = ch->out_xfer->actual_num_bytes;
      } else {
         ESP_LOGW(TAG, "%s tx status=%d", ch->name, ch->out_xfer->status);
      }
   } else {
      /* Timed out waiting for the kernel.  Abort the in-flight transfer
       * via EP halt+flush so the next send doesn't race with it.  Do
       * NOT free the transfer — kernel still owns it briefly. */
      ESP_LOGW(TAG, "%s tx timeout — halt+flush ep_out 0x%02x", ch->name, ch->ep_out_addr);
      (void)usb_host_endpoint_halt(s_dev, ch->ep_out_addr);
      (void)usb_host_endpoint_flush(s_dev, ch->ep_out_addr);
      (void)usb_host_endpoint_clear(s_dev, ch->ep_out_addr);
      /* Drain the late completion so the next call starts clean. */
      (void)xSemaphoreTake(ch->out_done, pdMS_TO_TICKS(500));
   }
   xSemaphoreGiveRecursive(ch->lock);
   return written;
}

static int channel_recv(channel_t *ch, void *buf, size_t len, uint32_t timeout_ms) {
   if (!ch->rx) return 0;
   return (int)xStreamBufferReceive(ch->rx, buf, len, pdMS_TO_TICKS(timeout_ms));
}

/* ── Public API ────────────────────────────────────────────────────── */

esp_err_t voice_usb_ffs_init(void) {
   if (s_initialized) return ESP_OK;

   tab5_set_usb_5v_en(true);

   s_ch_ctrl.lock = xSemaphoreCreateRecursiveMutex();
   s_ch_video.lock = xSemaphoreCreateRecursiveMutex();
   s_ch_ctrl.rx = xStreamBufferCreate(s_ch_ctrl.rx_ring_bytes, 1);
   s_ch_video.rx = xStreamBufferCreate(s_ch_video.rx_ring_bytes, 1);
   s_disconnect_sem = xSemaphoreCreateBinary();
   if (!s_ch_ctrl.lock || !s_ch_video.lock || !s_ch_ctrl.rx || !s_ch_video.rx || !s_disconnect_sem) {
      ESP_LOGE(TAG, "alloc fail");
      return ESP_ERR_NO_MEM;
   }

   const usb_host_config_t host_cfg = {
       .skip_phy_setup = false,
       .intr_flags = ESP_INTR_FLAG_LEVEL1,
   };
   esp_err_t err = usb_host_install(&host_cfg);
   if (err == ESP_ERR_INVALID_STATE) {
      ESP_LOGI(TAG, "usb_host already installed (shared with voice_usb_cdc)");
   } else if (err != ESP_OK) {
      ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
      return err;
   } else {
      s_lib_installed = true;
      if (xTaskCreate(usb_lib_task, "usb_lib", USB_LIB_TASK_STACK, NULL, USB_LIB_TASK_PRIO, &s_lib_task) != pdPASS)
         return ESP_ERR_NO_MEM;
   }

   const usb_host_client_config_t client_cfg = {
       .is_synchronous = false,
       .max_num_event_msg = 8,
       .async = {.client_event_callback = on_client_event, .callback_arg = NULL},
   };
   err = usb_host_client_register(&client_cfg, &s_client);
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "client_register: %s", esp_err_to_name(err));
      return err;
   }

   if (xTaskCreate(client_task, "usb_ffs_cli", CLIENT_TASK_STACK, NULL, CLIENT_TASK_PRIO, &s_client_task) != pdPASS)
      return ESP_ERR_NO_MEM;
   if (xTaskCreate(watcher_task, "usb_ffs_watch", WATCHER_TASK_STACK, NULL, WATCHER_TASK_PRIO, &s_watcher_task) !=
       pdPASS)
      return ESP_ERR_NO_MEM;

   s_initialized = true;
   ESP_LOGI(TAG, "init done — single client, claiming ctrl+video by subclass");
   return ESP_OK;
}

void voice_usb_ffs_deinit(void) {
   if (!s_initialized) return;
   if (s_watcher_task) {
      vTaskDelete(s_watcher_task);
      s_watcher_task = NULL;
   }
   if (s_client_task) {
      vTaskDelete(s_client_task);
      s_client_task = NULL;
   }
   xSemaphoreTakeRecursive(s_ch_ctrl.lock, portMAX_DELAY);
   xSemaphoreTakeRecursive(s_ch_video.lock, portMAX_DELAY);
   close_k144();
   if (s_client) {
      usb_host_client_deregister(s_client);
      s_client = NULL;
   }
   if (s_lib_task) {
      vTaskDelete(s_lib_task);
      s_lib_task = NULL;
   }
   if (s_lib_installed) {
      usb_host_uninstall();
      s_lib_installed = false;
   }
   xSemaphoreGiveRecursive(s_ch_video.lock);
   xSemaphoreGiveRecursive(s_ch_ctrl.lock);
   vSemaphoreDelete(s_ch_ctrl.lock);
   s_ch_ctrl.lock = NULL;
   vSemaphoreDelete(s_ch_video.lock);
   s_ch_video.lock = NULL;
   vStreamBufferDelete(s_ch_ctrl.rx);
   s_ch_ctrl.rx = NULL;
   vStreamBufferDelete(s_ch_video.rx);
   s_ch_video.rx = NULL;
   vSemaphoreDelete(s_disconnect_sem);
   s_disconnect_sem = NULL;
   s_initialized = false;
}

bool voice_usb_ffs_is_initialized(void) { return s_initialized; }

/* Control channel public API */
bool voice_usb_ffs_is_connected(void) { return s_ch_ctrl.connected; }
int voice_usb_ffs_send(const void *b, size_t l) { return channel_send(&s_ch_ctrl, b, l); }
int voice_usb_ffs_recv(void *b, size_t l, uint32_t t) { return channel_recv(&s_ch_ctrl, b, l, t); }
void voice_usb_ffs_flush(void) {
   if (s_ch_ctrl.rx) xStreamBufferReset(s_ch_ctrl.rx);
}
esp_err_t voice_usb_ffs_lock(uint32_t t) {
   if (!s_ch_ctrl.lock) return ESP_ERR_INVALID_STATE;
   return (xSemaphoreTakeRecursive(s_ch_ctrl.lock, pdMS_TO_TICKS(t)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
void voice_usb_ffs_unlock(void) {
   if (s_ch_ctrl.lock) xSemaphoreGiveRecursive(s_ch_ctrl.lock);
}

/* Video channel public API */
bool voice_usb_ffs_video_is_connected(void) { return s_ch_video.connected; }
int voice_usb_ffs_video_send(const void *b, size_t l) { return channel_send(&s_ch_video, b, l); }
int voice_usb_ffs_video_recv(void *b, size_t l, uint32_t t) { return channel_recv(&s_ch_video, b, l, t); }
void voice_usb_ffs_video_flush(void) {
   if (s_ch_video.rx) xStreamBufferReset(s_ch_video.rx);
}
esp_err_t voice_usb_ffs_video_lock(uint32_t t) {
   if (!s_ch_video.lock) return ESP_ERR_INVALID_STATE;
   return (xSemaphoreTakeRecursive(s_ch_video.lock, pdMS_TO_TICKS(t)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
void voice_usb_ffs_video_unlock(void) {
   if (s_ch_video.lock) xSemaphoreGiveRecursive(s_ch_video.lock);
}

/* Zero-copy TX: hand the caller our persistent DMA-PSRAM buffer.  No
 * intermediate memcpy → halves the per-call PSRAM bandwidth churn for
 * yolo bursts, which fixes the Wi-Fi-flap under sustained load. */
void *voice_usb_ffs_video_tx_borrow(size_t *out_cap) {
   if (!s_ch_video.connected || !s_ch_video.out_xfer) return NULL;
   if (out_cap) *out_cap = s_ch_video.out_xfer_size;
   return s_ch_video.out_xfer->data_buffer;
}

int voice_usb_ffs_video_tx_commit(size_t len) {
   channel_t *ch = &s_ch_video;
   if (!ch->connected || !s_dev || !ch->out_xfer || !ch->out_done) return -1;
   if (len == 0 || len > ch->out_xfer_size) return -1;

   /* Caller already holds ch->lock (per the borrow contract). */
   (void)xSemaphoreTake(ch->out_done, 0); /* drain stale */

   ch->out_xfer->num_bytes = len;

   esp_err_t err = usb_host_transfer_submit(ch->out_xfer);
   if (err != ESP_OK) {
      ESP_LOGW(TAG, "%s tx_commit submit: %s", ch->name, esp_err_to_name(err));
      return -1;
   }
   if (xSemaphoreTake(ch->out_done, pdMS_TO_TICKS(TX_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGW(TAG, "%s tx_commit timeout — halt+flush ep_out 0x%02x", ch->name, ch->ep_out_addr);
      (void)usb_host_endpoint_halt(s_dev, ch->ep_out_addr);
      (void)usb_host_endpoint_flush(s_dev, ch->ep_out_addr);
      (void)usb_host_endpoint_clear(s_dev, ch->ep_out_addr);
      (void)xSemaphoreTake(ch->out_done, pdMS_TO_TICKS(500));
      return -1;
   }
   if (ch->out_xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      ESP_LOGW(TAG, "%s tx_commit status=%d", ch->name, ch->out_xfer->status);
      return -1;
   }
   return ch->out_xfer->actual_num_bytes;
}
