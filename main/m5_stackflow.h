/**
 * @file m5_stackflow.h
 * @brief M5Stack StackFlow JSON request/response marshalling for the K144
 *        LLM Module.  Pure transport-agnostic layer — operates on caller
 *        buffers, never touches the UART.  Pair with `bsp/tab5/uart_port_c.h`
 *        (or any other byte-stream transport) at the call site.
 *
 * Wire shape (newline-delimited JSON over UART or TCP — see
 * `docs/PLAN-m5-llm-module.md` "Wire interface" + Phase 0 results):
 *
 *   Request   { "request_id": "...", "work_id": "...", "action": "...",
 *               ["object": "..."], ["data": "<string>" | <object>] }
 *
 *   Response  { "request_id": "...", "work_id": "...",
 *               "object": "..." | "None",
 *               "error": { "code": 0, "message": "" },
 *               "data":  "..." | <object>,
 *               "created": <epoch-seconds> }
 *
 *   Streaming inference responses arrive as multiple frames where
 *     object == "llm.utf-8.stream"
 *     data   == { "delta": "...", "index": N, "finish": bool }
 *   on the same socket; the caller loops until `finish == true`.
 *
 * Authoritative protocol source: M5Module-LLM Arduino library
 *   https://github.com/m5stack/M5Module-LLM (v1.7.0+).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/*  Request building — caller-allocated buffer, snprintf-style return     */
/* ---------------------------------------------------------------------- */

/**
 * @brief Description of one StackFlow request frame.
 *
 * `request_id`, `work_id`, and `action` are required and must be
 * non-empty C strings.  `object` and the two `data_*` fields are optional;
 * when both data fields are non-NULL, `data_json` wins (and
 * `data_string` is ignored).  All pointers must remain valid for the
 * duration of the build call but are NOT retained afterwards.
 */
typedef struct {
   const char *request_id;
   const char *work_id;
   const char *action;
   const char *object;      /**< optional; e.g. "llm.utf-8.stream" */
   const char *data_string; /**< optional; emits "data": "<string>" */
   const cJSON *data_json;  /**< optional; emits "data": <object>; not consumed */
} m5_stackflow_request_t;

/**
 * @brief Serialise a request into the caller's buffer as newline-terminated
 *        JSON, ready to push down a transport.
 *
 * The output is a single line: `{...}\n`.  No length-prefix framing — the
 * StackFlow daemon parses one JSON object per line.
 *
 * @param req     Request description.  Must be non-NULL with all required
 *                fields populated.
 * @param buf     Destination buffer.
 * @param buf_cap Capacity of @p buf in bytes (including the trailing NUL).
 *
 * @return Number of bytes written (excluding NUL terminator) on success,
 *         or -1 on validation failure (NULL field, missing required field,
 *         or buffer too small).
 */
int m5_stackflow_build_request(const m5_stackflow_request_t *req, char *buf, size_t buf_cap);

/* ---------------------------------------------------------------------- */
/*  Response parsing — owns a cJSON tree, exposes typed view              */
/* ---------------------------------------------------------------------- */

/**
 * @brief Parsed view of one StackFlow response.
 *
 * `root` owns the underlying cJSON tree.  All other pointer fields are
 * borrowed from inside that tree and remain valid until
 * @ref m5_stackflow_response_free is called.  Missing fields are zero
 * (`error_code`, `created`) or NULL (string pointers, `data`).
 */
typedef struct {
   cJSON *root;               /**< owned */
   const char *request_id;    /**< borrowed; NULL if absent */
   const char *work_id;       /**< borrowed; NULL if absent */
   const char *object;        /**< borrowed; NULL if absent */
   int error_code;            /**< 0 == MODULE_LLM_OK */
   const char *error_message; /**< borrowed; "" if empty */
   const cJSON *data;         /**< borrowed; NULL if absent */
   int64_t created;           /**< epoch seconds; 0 if absent */
} m5_stackflow_response_t;

/**
 * @brief Parse a single StackFlow response frame.
 *
 * Accepts payloads with or without a trailing newline.  On success @p out
 * contains an owned cJSON tree (free with @ref m5_stackflow_response_free).
 * On failure @p out is zero-initialised and may be passed to free safely.
 *
 * @param json_text  Raw JSON bytes; need not be NUL-terminated.
 * @param len        Length of @p json_text.
 * @param out        Destination view.  Must be non-NULL.
 *
 * @return ESP_OK on success;
 *         ESP_ERR_INVALID_ARG on NULL inputs;
 *         ESP_ERR_INVALID_STATE if cJSON couldn't parse.
 */
esp_err_t m5_stackflow_parse_response(const char *json_text, size_t len, m5_stackflow_response_t *out);

/**
 * @brief Release the cJSON tree owned by @p resp.
 *
 * Safe on a zero-initialised or already-freed response.  Borrowed string
 * pointers become dangling — callers must copy before freeing.
 */
void m5_stackflow_response_free(m5_stackflow_response_t *resp);

/**
 * @brief True iff @p resp was parsed successfully and its `request_id`
 *        equals @p expected_request_id.
 */
bool m5_stackflow_response_matches(const m5_stackflow_response_t *resp, const char *expected_request_id);

/* ---------------------------------------------------------------------- */
/*  Streaming inference helpers (object == "llm.utf-8.stream", etc.)      */
/* ---------------------------------------------------------------------- */

/**
 * @brief One streamed inference chunk.  String pointers are borrowed from
 *        the parent response and become invalid after the response is freed.
 */
typedef struct {
   const char *delta; /**< borrowed; NULL if absent */
   int index;
   bool finish;
} m5_stackflow_stream_chunk_t;

/**
 * @brief Heuristic — true iff the response's `object` field ends in
 *        `.stream` (e.g. "llm.utf-8.stream", "asr.utf-8.stream").
 */
bool m5_stackflow_response_is_stream(const m5_stackflow_response_t *resp);

/**
 * @brief Extract a streaming chunk view from a stream-shaped response.
 *
 * @param resp Parsed response (presumably stream-shaped — call
 *             @ref m5_stackflow_response_is_stream first if uncertain).
 * @param out  Destination view.  Must be non-NULL.
 *
 * @return ESP_OK on success;
 *         ESP_ERR_INVALID_ARG on NULL inputs;
 *         ESP_ERR_INVALID_STATE if `data` is missing or not an object.
 */
esp_err_t m5_stackflow_extract_stream_chunk(const m5_stackflow_response_t *resp, m5_stackflow_stream_chunk_t *out);

#ifdef __cplusplus
}
#endif
