/* solo_session_store — SD-backed solo-mode session persistence.
 *
 * Per-session JSON at /sdcard/sessions/<sid>.json.  Active sid lives
 * in NVS ("solo_sid") so it survives reboots.  Single live session
 * at a time; rotate() forces a fresh sid (used by "New Chat" UX).
 *
 * Concurrency: solo_session_append is called sequentially from the
 * tab5_worker job (same one that ran the LLM round-trip); no
 * cross-task races.  The NVS sid getter/setter inherits the
 * settings.c mutex via the standard nvs_open/commit pattern.
 *
 * TT #370 — see solo_session_store.h.
 */

#include "solo_session_store.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#define SESSIONS_DIR  "/sdcard/sessions"
#define NVS_NAMESPACE "settings"
#define NVS_KEY       "solo_sid"

static const char *TAG = "solo_sess";
static char s_sid[33] = {0};

esp_err_t solo_session_init(void) {
   struct stat st;
   if (stat(SESSIONS_DIR, &st) != 0) {
      if (mkdir(SESSIONS_DIR, 0775) != 0) {
         ESP_LOGE(TAG, "mkdir %s failed", SESSIONS_DIR);
         return ESP_FAIL;
      }
   }
   return ESP_OK;
}

static void gen_sid(char *out, size_t cap) {
   uint32_t a = esp_random();
   uint32_t b = esp_random();
   snprintf(out, cap, "%08lx%08lx", (unsigned long)a, (unsigned long)b);
}

static esp_err_t nvs_get_sid(char *out, size_t cap) {
   nvs_handle_t h;
   esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
   if (err != ESP_OK) return err;
   size_t len = cap;
   err = nvs_get_str(h, NVS_KEY, out, &len);
   nvs_close(h);
   return err;
}

static esp_err_t nvs_set_sid(const char *sid) {
   nvs_handle_t h;
   esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
   if (err != ESP_OK) return err;
   err = nvs_set_str(h, NVS_KEY, sid);
   if (err == ESP_OK) nvs_commit(h);
   nvs_close(h);
   return err;
}

static void session_path(char *buf, size_t cap, const char *sid) {
   snprintf(buf, cap, "%s/%s.json", SESSIONS_DIR, sid);
}

esp_err_t solo_session_open(char *out_id, size_t cap) {
   if (s_sid[0] == '\0') {
      char buf[33] = {0};
      if (nvs_get_sid(buf, sizeof buf) != ESP_OK || buf[0] == '\0') {
         gen_sid(buf, sizeof buf);
         nvs_set_sid(buf);
      }
      memcpy(s_sid, buf, sizeof s_sid - 1);
      s_sid[sizeof s_sid - 1] = '\0';
   }
   if (out_id && cap > 0) {
      strncpy(out_id, s_sid, cap - 1);
      out_id[cap - 1] = '\0';
   }
   /* Touch the file (creates if absent). */
   char path[96];
   session_path(path, sizeof path, s_sid);
   struct stat st;
   if (stat(path, &st) == 0) return ESP_OK;
   FILE *f = fopen(path, "w");
   if (!f) return ESP_FAIL;
   fprintf(f, "{\"id\":\"%s\",\"created\":%lld,\"turns\":[]}", s_sid, (long long)time(NULL));
   fclose(f);
   return ESP_OK;
}

static cJSON *load_session(const char *path) {
   FILE *f = fopen(path, "r");
   if (!f) return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n <= 0) {
      fclose(f);
      return NULL;
   }
   char *buf = heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!buf) {
      fclose(f);
      return NULL;
   }
   size_t got = fread(buf, 1, (size_t)n, f);
   buf[got] = '\0';
   fclose(f);
   cJSON *root = cJSON_Parse(buf);
   heap_caps_free(buf);
   return root;
}

static esp_err_t save_session(const char *path, cJSON *root) {
   char *s = cJSON_PrintUnformatted(root);
   if (!s) return ESP_ERR_NO_MEM;
   FILE *f = fopen(path, "w");
   if (!f) {
      free(s);
      return ESP_FAIL;
   }
   fwrite(s, 1, strlen(s), f);
   fclose(f);
   free(s);
   return ESP_OK;
}

esp_err_t solo_session_append(const char *role, const char *content) {
   if (!role || !content) return ESP_ERR_INVALID_ARG;
   if (s_sid[0] == '\0') {
      esp_err_t e = solo_session_open(NULL, 0);
      if (e != ESP_OK) return e;
   }
   char path[96];
   session_path(path, sizeof path, s_sid);
   cJSON *root = load_session(path);
   if (!root) return ESP_FAIL;
   cJSON *turns = cJSON_GetObjectItem(root, "turns");
   if (!cJSON_IsArray(turns)) {
      cJSON_Delete(root);
      return ESP_FAIL;
   }
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "role", role);
   cJSON_AddStringToObject(t, "content", content);
   cJSON_AddNumberToObject(t, "ts", (double)time(NULL));
   cJSON_AddItemToArray(turns, t);
   while (cJSON_GetArraySize(turns) > SOLO_SESSION_MAX_TURNS) {
      cJSON_DeleteItemFromArray(turns, 0);
   }
   esp_err_t err = save_session(path, root);
   cJSON_Delete(root);
   return err;
}

esp_err_t solo_session_load_recent(int n, char *out_json, size_t cap) {
   if (!out_json || cap < 3) return ESP_ERR_INVALID_ARG;
   /* Default empty array — caller can use this as messages prefix. */
   strncpy(out_json, "[]", cap);
   out_json[cap - 1] = '\0';
   if (n <= 0) return ESP_OK;

   if (s_sid[0] == '\0') {
      esp_err_t e = solo_session_open(NULL, 0);
      if (e != ESP_OK) return e;
   }
   char path[96];
   session_path(path, sizeof path, s_sid);
   cJSON *root = load_session(path);
   if (!root) return ESP_FAIL;
   cJSON *turns = cJSON_GetObjectItem(root, "turns");
   if (!cJSON_IsArray(turns)) {
      cJSON_Delete(root);
      return ESP_FAIL;
   }
   int total = cJSON_GetArraySize(turns);
   int start = total > n ? total - n : 0;
   cJSON *out = cJSON_CreateArray();
   for (int i = start; i < total; i++) {
      cJSON *t = cJSON_GetArrayItem(turns, i);
      if (!t) continue;
      cJSON *role = cJSON_GetObjectItem(t, "role");
      cJSON *content = cJSON_GetObjectItem(t, "content");
      if (!cJSON_IsString(role) || !cJSON_IsString(content)) continue;
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "role", role->valuestring);
      cJSON_AddStringToObject(m, "content", content->valuestring);
      cJSON_AddItemToArray(out, m);
   }
   char *s = cJSON_PrintUnformatted(out);
   if (s) {
      strncpy(out_json, s, cap - 1);
      out_json[cap - 1] = '\0';
      free(s);
   }
   cJSON_Delete(out);
   cJSON_Delete(root);
   return ESP_OK;
}

esp_err_t solo_session_rotate(void) {
   s_sid[0] = '\0';
   nvs_handle_t h;
   if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
      nvs_erase_key(h, NVS_KEY);
      nvs_commit(h);
      nvs_close(h);
   }
   return solo_session_open(NULL, 0);
}
