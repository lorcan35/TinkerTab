/**
 * @file voice_yolo.h
 * @brief Tab5↔K144 YOLO11n object detection over the USB ffs.control
 *        bridge.
 *
 * K144's `llm_yolo` daemon (StackFlow `yolo` work_id) accepts:
 *   {request_id, work_id="yolo", action="setup",
 *    object="yolo.setup",
 *    data={model="yolo11n", input="yolo.jpeg.base64",
 *          response_format="yolo.box.stream", enoutput=true}}
 * → returns assigned work_id "yolo.NNNN".
 *
 * Then per frame:
 *   {request_id, work_id="yolo.NNNN", action="inference",
 *    object="yolo.jpeg.base64",
 *    data="<base64(JPEG)>"}
 * → streams one or more
 *   {object="yolo.box.stream",
 *    data={delta={bbox:[x,y,w,h], class:"...", confidence:"..."},
 *          index, finish}}
 * lines, terminated by an empty `delta` with `finish=true`.
 *
 * Live-verified on the bench 2026-05-21 — 320×320 dog.jpg → bicycle, dog,
 * truck detections in ~330 ms round-trip over USB HS.
 *
 * TT #621 W6.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** YOLO11n native input resolution.  Frames must be downscaled to this
 *  size before calling _infer; the daemon rejects mismatched sizes. */
#define VOICE_YOLO_INPUT_W 320
#define VOICE_YOLO_INPUT_H 320

/** Single detection box returned by yolo.inference. */
typedef struct {
   float x, y, w, h; /**< pixel coords in the 320×320 input */
   float confidence; /**< 0..1 */
   char klass[24];   /**< COCO class name, NUL-terminated */
} voice_yolo_box_t;

/**
 * @brief One-shot setup of the K144 YOLO11n work_id.
 *
 * Sends a `yolo.setup` request via the active voice_xport and parses the
 * assigned `work_id`.  Idempotent — re-calling returns the same handle.
 * Blocks up to a few seconds (NPU model load on first call).
 *
 * @return ESP_OK on success, error code otherwise.  ESP_ERR_INVALID_STATE
 *         if transport isn't ready.
 */
esp_err_t voice_yolo_init(void);

/** @brief Whether voice_yolo_init has succeeded. */
bool voice_yolo_is_ready(void);

/**
 * @brief Clear the cached yolo work_id so the next voice_yolo_infer call
 *        re-runs voice_yolo_init against a fresh handle.
 *
 * TT #629 Wave C.2 (R12b): K144's sys.reset reboots the StackFlow daemon
 * and invalidates every cached work_id on Tab5.  Hook this from the
 * m5.reset:recovered edge in voice_onboard.c so stale "yolo.NNNN"
 * handles don't survive a K144 daemon restart.
 */
void voice_yolo_invalidate(void);

/**
 * @brief Run YOLO11n on a 320×320 JPEG and collect detections.
 *
 * Caller owns @p jpeg; this function base64-encodes internally + sends
 * a `yolo.inference` JSON over voice_xport, drains the streamed
 * `yolo.box.stream` responses, and fills @p out_boxes up to
 * @p max_boxes.
 *
 * @param jpeg       JPEG-encoded bytes (downscaled to 320×320 by caller).
 * @param jpeg_len   Length in bytes.
 * @param out_boxes  Caller-allocated array of `max_boxes` entries.
 * @param max_boxes  Capacity of out_boxes (typical 16).
 * @param out_count  Receives number of boxes filled (<= max_boxes).
 * @param timeout_ms Total budget; typical 1500 ms covers NPU + USB.
 *
 * @return ESP_OK + populated out_boxes / out_count on success.
 *         ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE on parse failure.
 */
esp_err_t voice_yolo_infer(const void *jpeg, size_t jpeg_len, voice_yolo_box_t *out_boxes, size_t max_boxes,
                           size_t *out_count, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
