/*
 * debug_server_ota.c — OTA firmware update HTTP endpoint family.
 *
 * Wave 23b follow-up (#332): mechanical extract of the 2 OTA endpoints
 * from debug_server.c.  Handlers + the local ota_apply_args_t /
 * ota_apply_task helpers moved verbatim; the only call-site change is
 * `check_auth(req)` → `tab5_debug_check_auth(req)` so the moved file
 * goes through the shared internal-header wrapper.
 *
 * The Wave 10 #77 "schedule, don't apply in-process" pattern is
 * preserved — see ota_apply_task() comment for why a fresh boot is
 * required for the DMA heap.  Behavior is identical pre/post extract.
 */

#include "debug_server_ota.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"                /* TAB5_FIRMWARE_VER */
#include "debug_server_internal.h" /* tab5_debug_check_auth */
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota.h" /* tab5_ota_check / _schedule / _info_t */

static esp_err_t ota_check_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   tab5_ota_info_t info;
   esp_err_t err = tab5_ota_check(&info);

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "current", TAB5_FIRMWARE_VER);
   cJSON_AddStringToObject(root, "partition", tab5_ota_current_partition());
   if (err == ESP_OK) {
      cJSON_AddBoolToObject(root, "update_available", info.available);
      if (info.available) {
         cJSON_AddStringToObject(root, "new_version", info.version);
         cJSON_AddStringToObject(root, "url", info.url);
      }
   } else {
      cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
   }

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

typedef struct {
   char *url;
   char sha256[65];
} ota_apply_args_t;

static void ota_apply_task(void *arg) {
   ota_apply_args_t *args = (ota_apply_args_t *)arg;
   ESP_LOGI("ota", "Scheduling OTA for next boot: %s (sha256=%s)", args->url, args->sha256[0] ? args->sha256 : "none");
   /* Wave 10 #77 extended-use fix: schedule instead of apply in-process.
    * tab5_ota_schedule stores url+sha to NVS, reboots, and the fresh
    * boot applies with a pristine DMA heap (see main.c near WiFi init).
    * Prevents the "esp_dma_capable_malloc: Not enough heap memory"
    * failure mode that hits after 30+ min of normal use. */
   esp_err_t err = tab5_ota_schedule(args->url, args->sha256[0] ? args->sha256 : NULL);
   /* If we get here, schedule failed (success reboots) */
   ESP_LOGE("ota", "OTA schedule failed: %s", esp_err_to_name(err));
   free(args->url);
   free(args);
   vTaskSuspend(NULL) /* wave 13 C4: P4 TLSP crash on delete — suspend instead */;
}

static esp_err_t ota_apply_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   /* Check for update first */
   tab5_ota_info_t info;
   esp_err_t err = tab5_ota_check(&info);
   if (err != ESP_OK || !info.available) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"no update available\"}");
      return ESP_OK;
   }

   /* Spawn OTA task with SHA256 from version.json (SEC07) */
   ota_apply_args_t *args = malloc(sizeof(ota_apply_args_t));
   if (args) {
      args->url = strdup(info.url);
      strncpy(args->sha256, info.sha256, sizeof(args->sha256) - 1);
      args->sha256[sizeof(args->sha256) - 1] = '\0';
      if (args->url) {
         xTaskCreate(ota_apply_task, "ota_apply", 8192, args, 5, NULL);
      } else {
         free(args);
      }
   }

   httpd_resp_set_type(req, "application/json");
   char resp[512];
   snprintf(resp, sizeof(resp), "{\"status\":\"updating\",\"version\":\"%s\",\"url\":\"%s\",\"sha256_verify\":%s}",
            info.version, info.url, info.sha256[0] ? "true" : "false");
   httpd_resp_sendstr(req, resp);
   return ESP_OK;
}

void debug_server_ota_register(httpd_handle_t server) {
   const httpd_uri_t uri_ota_check = {.uri = "/ota/check", .method = HTTP_GET, .handler = ota_check_handler};
   const httpd_uri_t uri_ota_apply = {.uri = "/ota/apply", .method = HTTP_POST, .handler = ota_apply_handler};

   httpd_register_uri_handler(server, &uri_ota_check);
   httpd_register_uri_handler(server, &uri_ota_apply);
   ESP_LOGI("debug_ota", "OTA endpoint family registered (2 URIs)");
}
