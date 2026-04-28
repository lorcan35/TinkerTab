/**
 * @file m5_stackflow.c
 * @brief Implementation — see m5_stackflow.h for API contract.
 *
 * Design notes:
 *   - Pure marshalling.  No transport, no allocation policy beyond cJSON
 *     (which TinkerTab already routes to PSRAM via cJSON_InitHooks in
 *     main.c — see comment near line 199).
 *   - All response field accessors are NULL-safe — missing fields read as
 *     NULL (strings) / 0 (ints), never as undefined.
 *   - The streaming helper detects `*.stream` shape via a suffix check
 *     so future units (asr / kws / yolo) flow through the same path
 *     without core changes (open/closed).
 */

#include "m5_stackflow.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "m5_stackflow";

/* ---------------------------------------------------------------------- */
/*  Local helpers                                                         */
/* ---------------------------------------------------------------------- */

static bool str_nonempty(const char *s) { return s != NULL && s[0] != '\0'; }

static const char *json_string_or_null(const cJSON *parent, const char *key) {
   const cJSON *node = cJSON_GetObjectItemCaseSensitive(parent, key);
   return cJSON_IsString(node) ? node->valuestring : NULL;
}

static int json_int_or_default(const cJSON *parent, const char *key, int dflt) {
   const cJSON *node = cJSON_GetObjectItemCaseSensitive(parent, key);
   return cJSON_IsNumber(node) ? (int)node->valuedouble : dflt;
}

static int64_t json_i64_or_default(const cJSON *parent, const char *key, int64_t dflt) {
   const cJSON *node = cJSON_GetObjectItemCaseSensitive(parent, key);
   return cJSON_IsNumber(node) ? (int64_t)node->valuedouble : dflt;
}

static bool ends_with(const char *s, const char *suffix) {
   if (s == NULL || suffix == NULL) return false;
   size_t s_len = strlen(s);
   size_t suf_len = strlen(suffix);
   if (suf_len > s_len) return false;
   return strcmp(s + (s_len - suf_len), suffix) == 0;
}

/* ---------------------------------------------------------------------- */
/*  Request building                                                      */
/* ---------------------------------------------------------------------- */

int m5_stackflow_build_request(const m5_stackflow_request_t *req, char *buf, size_t buf_cap) {
   if (req == NULL || buf == NULL || buf_cap < 8) return -1;
   if (!str_nonempty(req->request_id) || !str_nonempty(req->work_id) || !str_nonempty(req->action)) {
      return -1;
   }

   cJSON *root = cJSON_CreateObject();
   if (root == NULL) return -1;

   bool ok = true;
   ok &= cJSON_AddStringToObject(root, "request_id", req->request_id) != NULL;
   ok &= cJSON_AddStringToObject(root, "work_id", req->work_id) != NULL;
   ok &= cJSON_AddStringToObject(root, "action", req->action) != NULL;
   if (str_nonempty(req->object)) {
      ok &= cJSON_AddStringToObject(root, "object", req->object) != NULL;
   }
   if (req->data_json != NULL) {
      cJSON *dup = cJSON_Duplicate(req->data_json, /*recurse=*/true);
      if (dup == NULL) {
         ok = false;
      } else {
         cJSON_AddItemToObject(root, "data", dup);
      }
   } else if (req->data_string != NULL) {
      ok &= cJSON_AddStringToObject(root, "data", req->data_string) != NULL;
   }

   if (!ok) {
      cJSON_Delete(root);
      return -1;
   }

   /* PrintPreallocated keeps the allocation off the heap and lets us
    * reserve one byte for the newline + NUL terminator. */
   if (!cJSON_PrintPreallocated(root, buf, (int)(buf_cap - 2), /*fmt=*/0)) {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_Delete(root);

   size_t n = strlen(buf);
   if (n + 2 > buf_cap) return -1; /* defensive — PrintPreallocated already gated */
   buf[n] = '\n';
   buf[n + 1] = '\0';
   return (int)(n + 1);
}

/* ---------------------------------------------------------------------- */
/*  Response parsing                                                      */
/* ---------------------------------------------------------------------- */

esp_err_t m5_stackflow_parse_response(const char *json_text, size_t len, m5_stackflow_response_t *out) {
   if (json_text == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
   memset(out, 0, sizeof(*out));

   /* Strip a single trailing newline (M5 always sends one); cJSON
    * tolerates leading whitespace but not necessarily a trailing CR. */
   while (len > 0 && (json_text[len - 1] == '\n' || json_text[len - 1] == '\r')) {
      len--;
   }
   if (len == 0) return ESP_ERR_INVALID_STATE;

   cJSON *root = cJSON_ParseWithLength(json_text, len);
   if (root == NULL) {
      ESP_LOGW(TAG, "cJSON_ParseWithLength failed");
      return ESP_ERR_INVALID_STATE;
   }

   out->root = root;
   out->request_id = json_string_or_null(root, "request_id");
   out->work_id = json_string_or_null(root, "work_id");
   out->object = json_string_or_null(root, "object");
   out->created = json_i64_or_default(root, "created", 0);

   const cJSON *err_node = cJSON_GetObjectItemCaseSensitive(root, "error");
   if (cJSON_IsObject(err_node)) {
      out->error_code = json_int_or_default(err_node, "code", 0);
      const char *msg = json_string_or_null(err_node, "message");
      out->error_message = msg != NULL ? msg : "";
   } else {
      out->error_message = "";
   }

   /* `data` may be a string ("None"), an object (stream chunk), or
    * absent.  We expose the borrowed cJSON pointer either way; callers
    * pick the typed extractor that fits. */
   const cJSON *data_node = cJSON_GetObjectItemCaseSensitive(root, "data");
   if (data_node != NULL && !cJSON_IsNull(data_node)) {
      out->data = data_node;
   }

   return ESP_OK;
}

void m5_stackflow_response_free(m5_stackflow_response_t *resp) {
   if (resp == NULL) return;
   if (resp->root != NULL) {
      cJSON_Delete(resp->root);
   }
   memset(resp, 0, sizeof(*resp));
}

bool m5_stackflow_response_matches(const m5_stackflow_response_t *resp, const char *expected_request_id) {
   if (resp == NULL || expected_request_id == NULL || resp->request_id == NULL) {
      return false;
   }
   return strcmp(resp->request_id, expected_request_id) == 0;
}

/* ---------------------------------------------------------------------- */
/*  Streaming chunk extraction                                            */
/* ---------------------------------------------------------------------- */

bool m5_stackflow_response_is_stream(const m5_stackflow_response_t *resp) {
   if (resp == NULL || resp->object == NULL) return false;
   return ends_with(resp->object, ".stream");
}

esp_err_t m5_stackflow_extract_stream_chunk(const m5_stackflow_response_t *resp, m5_stackflow_stream_chunk_t *out) {
   if (resp == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
   memset(out, 0, sizeof(*out));
   if (resp->data == NULL || !cJSON_IsObject(resp->data)) {
      return ESP_ERR_INVALID_STATE;
   }
   out->delta = json_string_or_null(resp->data, "delta");
   out->index = json_int_or_default(resp->data, "index", 0);
   const cJSON *fin = cJSON_GetObjectItemCaseSensitive(resp->data, "finish");
   out->finish = cJSON_IsTrue(fin);
   return ESP_OK;
}
