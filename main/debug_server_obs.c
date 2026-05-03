/*
 * debug_server_obs.c — observability debug HTTP family.
 *
 * Wave 23b follow-up (#332): twelfth per-family extract.  Owns the
 * 6 observability handlers + the heap-trace standalone buffer they
 * share:
 *
 *   GET  /log                — process snapshot (heap, tasks, uptime)
 *   GET  /crashlog           — last reset reason + coredump summary
 *   GET  /coredump           — raw coredump partition stream (chunked)
 *   POST /heap_trace_start   — begin standalone heap-trace leak capture
 *   GET  /heap_trace_dump    — stop + serial dump + JSON summary
 *   GET  /heap                — full per-pool heap state + reset reason
 *
 * Same convention as the prior 11 per-family extracts:
 *   check_auth(req)            → tab5_debug_check_auth(req)
 *   send_json_resp(req, root)  → tab5_debug_send_json_resp(req, root)
 */
#include "debug_server_obs.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "debug_server_internal.h"
#include "esp_core_dump.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "debug_obs_http";

#define check_auth(req) tab5_debug_check_auth(req)
#define send_json_resp(req, root) tab5_debug_send_json_resp(req, root)

/* ── Process snapshot (/log) ──────────────────────────────────────────── */

static esp_err_t log_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   cJSON *root = cJSON_CreateObject();
   if (!root) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
      return ESP_FAIL;
   }

   cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
   cJSON_AddNumberToObject(root, "heap_min", (double)esp_get_minimum_free_heap_size());
   cJSON_AddNumberToObject(root, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
   cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));

   UBaseType_t task_count = uxTaskGetNumberOfTasks();
   cJSON_AddNumberToObject(root, "tasks", (double)task_count);

   /* Task list requires configUSE_TRACE_FACILITY — just report count */

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);

   if (!json) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "print");
      return ESP_FAIL;
   }

   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* ── Crash summary (/crashlog) ────────────────────────────────────────── */

static esp_err_t crashlog_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   cJSON *root = cJSON_CreateObject();
   if (!root) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
      return ESP_FAIL;
   }

   /* Reset reason */
   const char *reset_reasons[] = {"UNKNOWN",  "POWERON", "EXT",        "SW",        "PANIC", "INT_WDT",
                                  "TASK_WDT", "WDT",     "DEEPSLEEP",  "BROWNOUT",  "SDIO",  "USB",
                                  "JTAG",     "EFUSE",   "PWR_GLITCH", "CPU_LOCKUP"};
   esp_reset_reason_t reason = esp_reset_reason();
   cJSON_AddStringToObject(
       root, "reset_reason",
       reason < sizeof(reset_reasons) / sizeof(reset_reasons[0]) ? reset_reasons[reason] : "UNKNOWN");
   cJSON_AddBoolToObject(
       root, "was_crash",
       reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT);

   /* Check if a core dump exists in flash */
   esp_core_dump_summary_t summary;
   esp_err_t cd_ret = esp_core_dump_get_summary(&summary);
   if (cd_ret == ESP_OK) {
      cJSON_AddBoolToObject(root, "coredump_present", true);
      cJSON_AddNumberToObject(root, "exc_pc", (double)summary.exc_pc);
      cJSON_AddStringToObject(root, "exc_task", summary.exc_task);

      /* RISC-V: no on-device backtrace, provide stack dump size */
      cJSON_AddNumberToObject(root, "stackdump_size", (double)summary.exc_bt_info.dump_size);
      cJSON_AddStringToObject(root, "hint", "Use 'espcoredump.py info_corefile' on host for full backtrace");
   } else {
      cJSON_AddBoolToObject(root, "coredump_present", false);
      cJSON_AddStringToObject(root, "note",
                              cd_ret == ESP_ERR_NOT_FOUND ? "No core dump in flash" : "Core dump read error");
   }

   cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
   cJSON_AddNumberToObject(root, "heap_min", (double)esp_get_minimum_free_heap_size());

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!json) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "print");
      return ESP_FAIL;
   }

   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* ── Coredump binary download (/coredump) ─────────────────────────────── */

/* Wave 15 W15-C05: stream the raw coredump partition bytes so we can
 * decode them offline with `espcoredump.py info_corefile -t elf`.
 * Partition reads are chunked into 4 KiB so the httpd task stack stays
 * tiny regardless of the dump size (~3 MB on this board).  Erase is
 * NOT performed here — the operator explicitly calls /crashlog?erase=1
 * or runs the esptool after confirming the dump was captured.  This
 * avoids the pathological loop where a corrupted dump keeps crashing
 * the decoder and gets wiped on every fetch attempt. */
static esp_err_t coredump_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   size_t addr = 0, size = 0;
   esp_err_t err = esp_core_dump_image_get(&addr, &size);
   if (err != ESP_OK) {
      if (err == ESP_ERR_NOT_FOUND) {
         httpd_resp_set_status(req, "404 Not Found");
         httpd_resp_send(req, "{\"error\":\"no coredump\"}", HTTPD_RESP_USE_STRLEN);
         return ESP_OK;
      }
      ESP_LOGE(TAG, "coredump_image_get failed: %s", esp_err_to_name(err));
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read error");
      return ESP_FAIL;
   }
   if (size == 0) {
      httpd_resp_set_status(req, "404 Not Found");
      httpd_resp_send(req, "{\"error\":\"empty coredump\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
   }

   /* Find the coredump partition so we can read via partition-relative
    * offsets.  `esp_core_dump_image_get` returns absolute flash addrs
    * but `esp_partition_read` wants offset-from-partition-start. */
   const esp_partition_t *part =
       esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
   if (!part) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no coredump partition");
      return ESP_FAIL;
   }
   if (addr < part->address || addr + size > part->address + part->size) {
      ESP_LOGE(TAG, "coredump range 0x%zx+%zu outside partition 0x%lx+%lu", addr, size, (unsigned long)part->address,
               (unsigned long)part->size);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "range mismatch");
      return ESP_FAIL;
   }
   const size_t rel = addr - part->address;

   httpd_resp_set_type(req, "application/octet-stream");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"tab5_coredump.elf\"");
   /* Advertise size via X-Content-Length so the client can track
    * progress without conflicting with the chunked Transfer-Encoding
    * that httpd_resp_send_chunk() emits. */
   char len[32];
   snprintf(len, sizeof(len), "%zu", size);
   httpd_resp_set_hdr(req, "X-Content-Length", len);

   /* Stream in 4 KiB chunks. */
   const size_t CHUNK = 4096;
   uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DEFAULT);
   if (!buf) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
      return ESP_FAIL;
   }
   size_t sent = 0;
   while (sent < size) {
      size_t to_read = size - sent;
      if (to_read > CHUNK) to_read = CHUNK;
      err = esp_partition_read(part, rel + sent, buf, to_read);
      if (err != ESP_OK) {
         ESP_LOGE(TAG, "partition_read@%zu: %s", rel + sent, esp_err_to_name(err));
         break;
      }
      if (httpd_resp_send_chunk(req, (const char *)buf, to_read) != ESP_OK) {
         /* Client dropped — bail silently. */
         break;
      }
      sent += to_read;
   }
   heap_caps_free(buf);
   /* Terminate chunked stream. */
   httpd_resp_send_chunk(req, NULL, 0);
   ESP_LOGI(TAG, "/coredump: sent %zu/%zu bytes", sent, size);
   return ESP_OK;
}

/* ── Heap trace (Wave 15 leak hunt) ───────────────────────────────────── */
/* Standalone heap tracing — ESP-IDF tracks every heap_caps_malloc /
 * heap_caps_free inside a fixed-size ring buffer.  HEAP_TRACE_LEAKS
 * mode keeps only allocations that haven't been freed yet.  The
 * matching serial dump reveals the caller's PC and the bytes held.
 * Flow:
 *   1. POST /heap_trace_start  → reset + start tracking
 *   2. ... run activity for N minutes ...
 *   3. GET  /heap_trace_dump   → stop + print to serial + JSON summary
 * Operator captures serial output for the full stack / caller PCs;
 * the JSON summary returns high-level stats so a scripted probe can
 * tell when tracking filled up.
 *
 * Requires CONFIG_HEAP_TRACING_STANDALONE=y. */

#define TAB5_HEAP_TRACE_NUM_RECORDS 300
static heap_trace_record_t *s_heap_trace_buf = NULL;
static bool s_heap_trace_active = false;

static esp_err_t heap_trace_start_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   if (!s_heap_trace_buf) {
      s_heap_trace_buf = heap_caps_calloc(TAB5_HEAP_TRACE_NUM_RECORDS, sizeof(heap_trace_record_t), MALLOC_CAP_SPIRAM);
      if (!s_heap_trace_buf) {
         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom: trace buffer (PSRAM)");
         return ESP_FAIL;
      }
      esp_err_t err = heap_trace_init_standalone(s_heap_trace_buf, TAB5_HEAP_TRACE_NUM_RECORDS);
      if (err != ESP_OK) {
         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "heap_trace_init_standalone failed");
         heap_caps_free(s_heap_trace_buf);
         s_heap_trace_buf = NULL;
         return ESP_FAIL;
      }
   }
   if (s_heap_trace_active) {
      heap_trace_stop();
   }
   heap_trace_resume(); /* wipes unfreed-set, ready for fresh record */
   esp_err_t err = heap_trace_start(HEAP_TRACE_LEAKS);
   if (err != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "heap_trace_start failed");
      return ESP_FAIL;
   }
   s_heap_trace_active = true;
   ESP_LOGI(TAG, "Heap trace STARTED (capacity=%d records, PSRAM)", TAB5_HEAP_TRACE_NUM_RECORDS);

   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_sendstr(req, "{\"ok\":true,\"records\":300,\"mode\":\"leaks\"}");
   return ESP_OK;
}

static esp_err_t heap_trace_dump_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   if (!s_heap_trace_active || !s_heap_trace_buf) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"heap_trace not running — POST /heap_trace_start first\"}");
      return ESP_OK;
   }

   /* Stop collection, then emit the outstanding-allocations report to
    * serial.  Full info (caller PCs + size) goes to UART via ESP_LOG;
    * we also compute a quick summary for the HTTP response. */
   heap_trace_stop();

   size_t count = heap_trace_get_count();
   /* Walk outstanding records and split by whether the alloc lives in
    * internal SRAM (by address range) vs PSRAM.  The record struct
    * itself doesn't carry caps, so classify via the actual pointer. */
   size_t internal_bytes = 0, psram_bytes = 0;
   heap_trace_record_t rec;
   for (size_t i = 0; i < count; i++) {
      if (heap_trace_get(i, &rec) != ESP_OK) break;
      /* ESP32-P4 PSRAM is mapped in the 0x48000000–0x49000000
       * and 0x4ff00000–... windows; internal SRAM lives below.
       * Simplest reliable split: ask the heap subsystem. */
      uint32_t caps = heap_caps_get_allocated_size(rec.address) > 0
                          ? (uintptr_t)rec.address >= 0x48000000 && (uintptr_t)rec.address < 0x4c000000
                                ? MALLOC_CAP_SPIRAM
                                : MALLOC_CAP_INTERNAL
                          : 0;
      if (caps == MALLOC_CAP_SPIRAM) {
         psram_bytes += rec.size;
      } else {
         internal_bytes += rec.size;
      }
   }

   /* Dump the full record list to UART for offline analysis.
    * Operator tails `idf.py monitor` or captures via serial script. */
   ESP_LOGW(TAG, "── Heap trace dump (%zu records, internal=%zu B, psram=%zu B) ──", count, internal_bytes,
            psram_bytes);
   heap_trace_dump();

   char body[256];
   snprintf(body, sizeof(body),
            "{\"ok\":true,\"records\":%zu,\"internal_bytes\":%zu,"
            "\"psram_bytes\":%zu,\"hint\":\"full dump in serial log\"}",
            count, internal_bytes, psram_bytes);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_sendstr(req, body);

   s_heap_trace_active = false;
   return ESP_OK;
}

/* ── Full per-pool heap state (/heap) ─────────────────────────────────── */
/* Wave 12 observability: /heap endpoint
 *
 * Exposes the per-pool heap state + last reboot reason in one call so
 * dashboards and post-mortem scripts can track device health over time.
 * This pairs with the wave 11 coredump path — if last_reboot_reason is
 * something like "abort" or a heap_wd string, the coredump partition
 * at 0x620000 has a forensic dump available via
 *     esptool read_flash 0x620000 0x40000 cd.bin
 *     espcoredump.py info_corefile -t elf ... cd.bin
 *
 * No auth-gate — the debug server overall requires auth per
 * reference_tab5_debug_access.md, but /heap is READ-ONLY and useful
 * to have behind the same bearer token.
 */
static esp_err_t heap_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   extern void media_cache_stats(int *used_slots, unsigned *resident_kb);
   int mc_used = 0;
   unsigned mc_kb = 0;
   media_cache_stats(&mc_used, &mc_kb);

   size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
   size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
   size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
   size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
   size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
   size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

   lv_mem_monitor_t lvgl;
   lv_mem_monitor(&lvgl);

   const char *reset_str = "unknown";
   switch (esp_reset_reason()) {
      case ESP_RST_POWERON:
         reset_str = "poweron";
         break;
      case ESP_RST_EXT:
         reset_str = "external_pin";
         break;
      case ESP_RST_SW:
         reset_str = "esp_restart";
         break;
      case ESP_RST_PANIC:
         reset_str = "panic_abort";
         break; /* wave 11 coredump path */
      case ESP_RST_INT_WDT:
         reset_str = "int_wdt";
         break;
      case ESP_RST_TASK_WDT:
         reset_str = "task_wdt";
         break;
      case ESP_RST_WDT:
         reset_str = "other_wdt";
         break;
      case ESP_RST_DEEPSLEEP:
         reset_str = "deepsleep_wake";
         break;
      case ESP_RST_BROWNOUT:
         reset_str = "brownout";
         break;
      case ESP_RST_SDIO:
         reset_str = "sdio";
         break;
      default:
         reset_str = "unknown";
         break;
   }

   char buf[640];
   int n = snprintf(buf, sizeof(buf),
                    "{\"uptime_ms\":%llu,\"reset_reason\":\"%s\","
                    "\"psram\":{\"free_kb\":%u,\"largest_kb\":%u},"
                    "\"internal\":{\"free_kb\":%u,\"largest_kb\":%u,\"frag_pct\":%d},"
                    "\"dma\":{\"free_kb\":%u,\"largest_kb\":%u},"
                    "\"lvgl\":{\"used_kb\":%u,\"free_kb\":%u,\"frag_pct\":%u},"
                    "\"media_cache\":{\"slots_used\":%d,\"resident_kb\":%u},"
                    "\"coredump_available\":%s}",
                    (unsigned long long)(esp_timer_get_time() / 1000), reset_str, (unsigned)(psram_free / 1024),
                    (unsigned)(psram_largest / 1024), (unsigned)(int_free / 1024), (unsigned)(int_largest / 1024),
                    int_free ? (int)(100 - (int_largest * 100 / int_free)) : 0, (unsigned)(dma_free / 1024),
                    (unsigned)(dma_largest / 1024), (unsigned)((lvgl.total_size - lvgl.free_size) / 1024),
                    (unsigned)(lvgl.free_size / 1024), (unsigned)lvgl.frag_pct, mc_used, mc_kb,
                    (esp_core_dump_image_check() == ESP_OK) ? "true" : "false");
   (void)n;
   httpd_resp_set_type(req, "application/json");
   httpd_resp_sendstr(req, buf);
   return ESP_OK;
}

void debug_server_obs_register(httpd_handle_t server) {
   if (!server) return;

   static const httpd_uri_t uri_log = {.uri = "/log", .method = HTTP_GET, .handler = log_handler};
   static const httpd_uri_t uri_crashlog = {.uri = "/crashlog", .method = HTTP_GET, .handler = crashlog_handler};
   static const httpd_uri_t uri_coredump = {.uri = "/coredump", .method = HTTP_GET, .handler = coredump_handler};
   static const httpd_uri_t uri_heap_trace_start = {
       .uri = "/heap_trace_start", .method = HTTP_POST, .handler = heap_trace_start_handler};
   static const httpd_uri_t uri_heap_trace_dump = {
       .uri = "/heap_trace_dump", .method = HTTP_GET, .handler = heap_trace_dump_handler};
   static const httpd_uri_t uri_heap = {.uri = "/heap", .method = HTTP_GET, .handler = heap_handler};

   httpd_register_uri_handler(server, &uri_log);
   httpd_register_uri_handler(server, &uri_crashlog);
   httpd_register_uri_handler(server, &uri_coredump);
   httpd_register_uri_handler(server, &uri_heap_trace_start);
   httpd_register_uri_handler(server, &uri_heap_trace_dump);
   httpd_register_uri_handler(server, &uri_heap);
}
