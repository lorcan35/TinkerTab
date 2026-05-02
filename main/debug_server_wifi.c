/*
 * debug_server_wifi.c — /wifi/status + /wifi/kick handlers.
 *
 * Wave 23b follow-up (#332): eighth per-family extract.  Two
 * handlers + the local IP-read helper moved verbatim from
 * debug_server.c.  Only call-site changes are the standard
 * `check_auth` → `tab5_debug_check_auth` and `send_json_resp` →
 * `tab5_debug_send_json_resp` substitutions.
 *
 * The original `get_wifi_ip` static helper is duplicated here under
 * the same name (file-static, so no linker conflict) rather than
 * promoted to debug_server_internal.h — `info_handler` in
 * debug_server.c is its only other caller and the function is 13
 * lines of pure netif lookups; the duplication cost is lower than
 * the public-API surface it would otherwise demand.
 */

#include "debug_server_wifi.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug_server_internal.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi.h" /* tab5_wifi_connected, tab5_wifi_kick, tab5_wifi_hard_kick */

static const char *TAG = "debug_wifi";

/* Local IP-read helper — duplicated from debug_server.c by design (see
 * file header for rationale).  Keeps the family self-contained. */
static esp_err_t get_wifi_ip(char *buf, size_t len) {
   esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
   if (!netif) {
      snprintf(buf, len, "0.0.0.0");
      return ESP_FAIL;
   }
   esp_netif_ip_info_t ip;
   if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
      snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
      return ESP_OK;
   }
   snprintf(buf, len, "0.0.0.0");
   return ESP_FAIL;
}

/* POST /wifi/kick?mode=soft|hard|reboot — force the escalation tiers
 * from #146.  Used by the test harness to verify recovery paths without
 * waiting 2-3 min of actual flap.  mode=soft is safe; hard costs ~1-2 s
 * of downtime; reboot is self-explanatory. */
static esp_err_t wifi_kick_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   char query[64] = {0};
   char mode[16] = "soft";
   if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      httpd_query_key_value(query, "mode", mode, sizeof(mode));
   }

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "mode", mode);

   if (strcmp(mode, "hard") == 0) {
      esp_err_t r = tab5_wifi_hard_kick();
      cJSON_AddBoolToObject(root, "ok", r == ESP_OK);
      if (r != ESP_OK) cJSON_AddStringToObject(root, "error", esp_err_to_name(r));
   } else if (strcmp(mode, "reboot") == 0) {
      cJSON_AddBoolToObject(root, "ok", true);
      cJSON_AddStringToObject(root, "note", "rebooting in 100 ms");
      char *json = cJSON_PrintUnformatted(root);
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, json);
      free(json);
      cJSON_Delete(root);
      vTaskDelay(pdMS_TO_TICKS(100));
      esp_restart();
      return ESP_OK; /* unreachable */
   } else {
      tab5_wifi_kick();
      cJSON_AddBoolToObject(root, "ok", true);
   }
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_sendstr(req, json);
   free(json);
   return ESP_OK;
}

/* ── GET /wifi/status ──────────────────────────────────────────── */
static esp_err_t wifi_status_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;
   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "connected", tab5_wifi_connected());

   char ip[20];
   get_wifi_ip(ip, sizeof(ip));
   cJSON_AddStringToObject(root, "ip", ip);

   wifi_ap_record_t ap = {0};
   esp_err_t r = esp_wifi_sta_get_ap_info(&ap);
   if (r == ESP_OK) {
      char bssid[18];
      snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0], ap.bssid[1], ap.bssid[2],
               ap.bssid[3], ap.bssid[4], ap.bssid[5]);
      cJSON_AddStringToObject(root, "ssid", (const char *)ap.ssid);
      cJSON_AddStringToObject(root, "bssid", bssid);
      cJSON_AddNumberToObject(root, "channel", ap.primary);
      cJSON_AddNumberToObject(root, "rssi", ap.rssi);
      const char *auths[] = {"open",     "wep",           "wpa_psk",  "wpa2_psk", "wpa_wpa2_psk",    "wpa2_enterprise",
                             "wpa3_psk", "wpa2_wpa3_psk", "wapi_psk", "owe",      "wpa3_enterprise", "wpa3_ent_192"};
      int am = (int)ap.authmode;
      cJSON_AddStringToObject(root, "authmode",
                              (am >= 0 && am < (int)(sizeof(auths) / sizeof(auths[0]))) ? auths[am] : "?");
   } else {
      cJSON_AddStringToObject(root, "error", esp_err_to_name(r));
   }
   return tab5_debug_send_json_resp(req, root);
}

void debug_server_wifi_register(httpd_handle_t server) {
   const httpd_uri_t uri_wifi_kick = {.uri = "/wifi/kick", .method = HTTP_POST, .handler = wifi_kick_handler};
   const httpd_uri_t uri_wifi_status = {.uri = "/wifi/status", .method = HTTP_GET, .handler = wifi_status_handler};
   httpd_register_uri_handler(server, &uri_wifi_kick);
   httpd_register_uri_handler(server, &uri_wifi_status);
   ESP_LOGI(TAG, "WiFi endpoint family registered (2 URIs)");
}
