/*
 * voice_video.c — see header.
 */
#include "voice_video.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_encode.h"

#include "camera.h"
#include "settings.h"
#include "voice.h"
#include "ui_video_pane.h"
#include "ui_core.h"          /* tab5_lv_async_call (#258) */
#include "lvgl.h"

static const char *TAG = "voice_video";

/* JPEG quality.  30 keeps frames in the 15-25 KB range so they fit
 * inside the 32 KB WS_CLIENT_BUFFER_SIZE in one WS binary send.
 * Quality 30 still looks fine for 1280x720 hand-held video — the
 * artefacts are masked by motion.  Bump in tandem with the WS buffer
 * if you need higher fidelity. */
#define VV_JPEG_QUALITY        30
#define VV_OUT_BUF_BYTES       (96 * 1024)    /* 96 KB safety ceiling */
#define VV_TASK_STACK          16384
#define VV_TASK_PRIO           4
#define VV_TASK_CORE           1

static jpeg_encoder_handle_t s_enc      = NULL;
static SemaphoreHandle_t     s_enc_mux  = NULL;
static TaskHandle_t          s_task     = NULL;

static volatile bool s_running    = false;
static volatile int  s_target_fps = VOICE_VIDEO_DEFAULT_FPS;

static voice_video_stats_t s_stats = {0};

/* Forward declarations for hooks we'll need from voice.c.  Kept as
 * extern so we don't drag voice.h's full surface into this header. */
extern esp_err_t voice_ws_send_binary_public(const void *data, size_t len);

/* RGB565 rotation helpers — same shapes as ui_camera.c (#260).  Local
 * copies so the streamer doesn't depend on the UI module. */
static void rot180(const uint16_t *src, uint16_t *dst, int sw, int sh)
{
    int n = sw * sh;
    for (int i = 0; i < n; i++) dst[n - 1 - i] = src[i];
}
static void rot90(const uint16_t *src, uint16_t *dst, int sw, int sh)
{
    for (int y = 0; y < sh; y++) {
        const uint16_t *row = src + y * sw;
        for (int x = 0; x < sw; x++) {
            dst[(sh - 1 - y) + x * sh] = row[x];
        }
    }
}
static void rot270(const uint16_t *src, uint16_t *dst, int sw, int sh)
{
    for (int y = 0; y < sh; y++) {
        const uint16_t *row = src + y * sw;
        for (int x = 0; x < sw; x++) {
            dst[y + (sw - 1 - x) * sh] = row[x];
        }
    }
}

esp_err_t voice_video_init(void)
{
    if (!s_enc_mux) {
        s_enc_mux = xSemaphoreCreateMutex();
        if (!s_enc_mux) return ESP_ERR_NO_MEM;
    }
    if (!s_enc) {
        jpeg_encode_engine_cfg_t cfg = {
            .intr_priority = 0,
            .timeout_ms    = 5000,
        };
        esp_err_t r = jpeg_new_encoder_engine(&cfg, &s_enc);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(r));
            s_enc = NULL;
            return r;
        }
    }
    ESP_LOGI(TAG, "init OK (jpeg engine ready)");
    return ESP_OK;
}

bool voice_video_is_streaming(void)
{
    return s_running;
}

void voice_video_get_stats(voice_video_stats_t *out)
{
    if (!out) return;
    *out = s_stats;
}

/* Encode one RGB565 frame to JPEG via the hardware engine.
 * out_buf is caller-owned.  Returns ESP_OK + sets *out_size. */
static esp_err_t encode_jpeg(const uint8_t *rgb565, int w, int h,
                             uint8_t *out_buf, size_t out_cap,
                             uint32_t *out_size)
{
    jpeg_encode_cfg_t cfg = {
        .height        = h,
        .width         = w,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = VV_JPEG_QUALITY,
    };
    if (xSemaphoreTake(s_enc_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t r = jpeg_encoder_process(s_enc, &cfg,
                                       (uint8_t *)rgb565,
                                       (size_t)w * h * 2,
                                       out_buf, out_cap, out_size);
    xSemaphoreGive(s_enc_mux);
    return r;
}

/* Build the on-wire frame: 4-byte magic + 4-byte length + JPEG bytes.
 * Caller-supplied wire_buf must be at least jpeg_len + 8 bytes. */
static size_t pack_wire_frame(uint8_t *wire_buf,
                              const uint8_t *jpeg, uint32_t jpeg_len)
{
    /* "VID0" big-endian magic. */
    wire_buf[0] = 'V'; wire_buf[1] = 'I'; wire_buf[2] = 'D'; wire_buf[3] = '0';
    wire_buf[4] = (uint8_t)((jpeg_len >> 24) & 0xff);
    wire_buf[5] = (uint8_t)((jpeg_len >> 16) & 0xff);
    wire_buf[6] = (uint8_t)((jpeg_len >> 8)  & 0xff);
    wire_buf[7] = (uint8_t)( jpeg_len        & 0xff);
    memcpy(wire_buf + 8, jpeg, jpeg_len);
    return jpeg_len + 8;
}

static void streaming_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "streaming task started fps=%d core=%d",
             s_target_fps, xPortGetCoreID());

    /* HW JPEG encoder requires DMA-aligned buffers (the engine writes
     * the bitstream via DMA).  Plain heap_caps_malloc fails with
     * "bit stream is not aligned" — must use jpeg_alloc_encoder_mem
     * with the OUTPUT direction for the destination buffer. */
    jpeg_encode_memory_alloc_cfg_t out_alloc = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t jpeg_cap = 0;
    uint8_t *jpeg_buf = jpeg_alloc_encoder_mem(VV_OUT_BUF_BYTES,
                                               &out_alloc, &jpeg_cap);
    /* Rotation scratch must also be DMA-aligned (the JPEG engine reads
     * from it via DMA when rotation != 0).  Wire buffer can be plain
     * PSRAM since it's only consumed by the WS-send path. */
    jpeg_encode_memory_alloc_cfg_t in_alloc = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    size_t rot_cap = 0;
    uint16_t *rot_buf  = (uint16_t *)jpeg_alloc_encoder_mem(
        1280 * 720 * 2, &in_alloc, &rot_cap);
    uint8_t  *wire_buf = heap_caps_malloc(VV_OUT_BUF_BYTES + 8, MALLOC_CAP_SPIRAM);
    if (!jpeg_buf || !rot_buf || !wire_buf) {
        ESP_LOGE(TAG, "streaming alloc failed (jpeg=%p rot=%p wire=%p)",
                 jpeg_buf, rot_buf, wire_buf);
        if (jpeg_buf) free(jpeg_buf);    /* jpeg_alloc_encoder_mem path */
        if (rot_buf)  free(rot_buf);
        heap_caps_free(wire_buf);
        s_running = false;
        s_task    = NULL;
        vTaskSuspend(NULL);
        return;
    }

    int last_fps = s_target_fps;
    TickType_t period = pdMS_TO_TICKS(1000 / last_fps);

    while (s_running) {
        if (s_target_fps != last_fps) {
            last_fps = s_target_fps;
            period = pdMS_TO_TICKS(1000 / last_fps);
            ESP_LOGI(TAG, "fps changed -> %d (period=%u ms)",
                     last_fps, (unsigned)(1000 / last_fps));
        }

        TickType_t t0 = xTaskGetTickCount();

        if (!tab5_camera_initialized()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        tab5_cam_frame_t frame;
        esp_err_t cer = tab5_camera_capture(&frame);
        if (cer != ESP_OK || frame.format != TAB5_CAM_FMT_RGB565) {
            s_stats.frames_dropped++;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Apply rotation per cam_rot.  Match the viewfinder so the
         * remote sees what the user sees. */
        const uint8_t *jpeg_src = frame.data;
        int sw = frame.width;
        int sh = frame.height;
        uint8_t rot = tab5_settings_get_cam_rotation() & 0x03;
        if (rot != 0) {
            const uint16_t *src = (const uint16_t *)frame.data;
            switch (rot) {
            case 1: rot90 (src, rot_buf, sw, sh); sw = frame.height; sh = frame.width; break;
            case 2: rot180(src, rot_buf, sw, sh); break;
            case 3: rot270(src, rot_buf, sw, sh); sw = frame.height; sh = frame.width; break;
            }
            jpeg_src = (const uint8_t *)rot_buf;
        }

        uint32_t jpeg_size = 0;
        esp_err_t er = encode_jpeg(jpeg_src, sw, sh,
                                   jpeg_buf, VV_OUT_BUF_BYTES, &jpeg_size);
        if (er != ESP_OK || jpeg_size == 0) {
            ESP_LOGW(TAG, "jpeg encode failed: %s sz=%" PRIu32,
                     esp_err_to_name(er), jpeg_size);
            s_stats.frames_dropped++;
            goto wait;
        }

        size_t wire_len = pack_wire_frame(wire_buf, jpeg_buf, jpeg_size);
        esp_err_t ser = voice_ws_send_binary_public(wire_buf, wire_len);
        if (ser == ESP_OK) {
            s_stats.frames_sent++;
            s_stats.bytes_sent     += wire_len;
            s_stats.last_jpeg_bytes = jpeg_size;
        } else {
            s_stats.frames_dropped++;
        }

wait:
        ;  /* C: empty stmt after label */
        TickType_t now = xTaskGetTickCount();
        TickType_t spent = now - t0;
        if (spent < period) {
            vTaskDelay(period - spent);
        } else {
            taskYIELD();
        }
    }

    free(jpeg_buf);                       /* jpeg_alloc_encoder_mem path */
    free(rot_buf);                        /* same */
    heap_caps_free(wire_buf);

    ESP_LOGI(TAG, "streaming task exiting cleanly");
    s_stats.active = false;
    s_task = NULL;
    /* P4 TLSP rule (#20): suspend, don't delete.  Worker stack lives
     * in PSRAM (#262 follow-up) so the leak per stream is bounded
     * PSRAM, not internal SRAM. */
    vTaskSuspend(NULL);
}

esp_err_t voice_video_start_streaming(int fps)
{
    if (s_running || s_task) {
        ESP_LOGW(TAG, "already streaming");
        return ESP_ERR_INVALID_STATE;
    }
    if (voice_video_init() != ESP_OK) return ESP_FAIL;

    if (fps < 1) fps = 1;
    if (fps > VOICE_VIDEO_MAX_FPS) fps = VOICE_VIDEO_MAX_FPS;
    s_target_fps    = fps;
    s_stats.active  = true;
    s_stats.fps     = fps;
    s_running       = true;

    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        streaming_task, "voice_video", VV_TASK_STACK,
        NULL, VV_TASK_PRIO, &s_task, VV_TASK_CORE,
        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task spawn failed");
        s_running      = false;
        s_stats.active = false;
        s_task         = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "streaming started fps=%d", fps);
    return ESP_OK;
}

esp_err_t voice_video_stop_streaming(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    /* Wait up to ~1.2 s for the task to drain its current frame and
     * exit on its own.  We don't vTaskDelete (P4 TLSP rule). */
    for (int i = 0; i < 60 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_stats.active = false;
    s_stats.fps    = 0;
    ESP_LOGI(TAG, "streaming stopped (frames_sent=%" PRIu32
                  " bytes=%" PRIu32 " dropped=%" PRIu32 ")",
             s_stats.frames_sent, s_stats.bytes_sent, s_stats.frames_dropped);
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────
 *  #268 Phase 3B: downlink — receive video frames from Dragon, decode
 *  via LVGL TJPGD, render via ui_video_pane.
 *
 *  Single PSRAM slot: voice.c hands us the wire bytes from inside the
 *  WS event handler (binary RX path).  We copy the JPEG payload into
 *  the slot, then hop to the LVGL thread to update the renderer's
 *  image source.  The renderer holds onto the bytes via lv_image_dsc_t
 *  with cf=LV_COLOR_FORMAT_RAW so TJPGD lazily decodes on draw.
 * ──────────────────────────────────────────────────────────────────── */

#define VV_RECV_SLOT_BYTES   (96 * 1024)   /* matches encoder ceiling */

static uint8_t        *s_recv_slot       = NULL;
static SemaphoreHandle_t s_recv_mux      = NULL;
static lv_image_dsc_t   s_recv_dsc       = {0};

bool voice_video_peek_downlink_magic(const void *data, size_t len)
{
    if (!data || len < 4) return false;
    const uint8_t *b = (const uint8_t *)data;
    return b[0] == 'V' && b[1] == 'I' && b[2] == 'D' && b[3] == '0';
}

/* JPEG SOF marker scan to extract w/h.  Same recipe as media_cache.c. */
static void jpeg_dims(const uint8_t *jpeg, size_t len, uint16_t *w, uint16_t *h)
{
    *w = *h = 0;
    for (size_t i = 0; i + 8 < len; i++) {
        if (jpeg[i] == 0xFF && (jpeg[i+1] == 0xC0 || jpeg[i+1] == 0xC2)) {
            *h = (jpeg[i+5] << 8) | jpeg[i+6];
            *w = (jpeg[i+7] << 8) | jpeg[i+8];
            return;
        }
    }
}

/* Runs on the LVGL thread (via tab5_lv_async_call). */
static void downlink_render_async(void *arg)
{
    (void)arg;
    /* The renderer is allowed to ignore the call (pane not open),
     * which is fine — voice_video_on_downlink_frame already
     * incremented frames_recv. */
    ui_video_pane_set_dsc(&s_recv_dsc);
}

esp_err_t voice_video_on_downlink_frame(const uint8_t *wire_bytes, size_t len)
{
    if (!wire_bytes || len < 8 + 2) return ESP_ERR_INVALID_ARG;
    if (!voice_video_peek_downlink_magic(wire_bytes, len)) return ESP_ERR_INVALID_ARG;

    /* Lazy-init the slot + mutex on first frame so a connection that
     * never receives video pays no PSRAM. */
    if (!s_recv_mux) {
        s_recv_mux = xSemaphoreCreateMutex();
        if (!s_recv_mux) return ESP_ERR_NO_MEM;
    }
    if (!s_recv_slot) {
        s_recv_slot = heap_caps_malloc(VV_RECV_SLOT_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_recv_slot) return ESP_ERR_NO_MEM;
    }

    /* Length sanity from the wire header. */
    uint32_t payload_len = ((uint32_t)wire_bytes[4] << 24)
                         | ((uint32_t)wire_bytes[5] << 16)
                         | ((uint32_t)wire_bytes[6] <<  8)
                         | ((uint32_t)wire_bytes[7]);
    if (payload_len + 8 != len) {
        ESP_LOGW(TAG, "downlink len mismatch hdr=%" PRIu32 " wire=%zu", payload_len, len);
        s_stats.frames_recv_dropped++;
        return ESP_ERR_INVALID_SIZE;
    }
    if (payload_len > VV_RECV_SLOT_BYTES) {
        ESP_LOGW(TAG, "downlink frame %" PRIu32 " B exceeds slot %d B — dropping",
                 payload_len, VV_RECV_SLOT_BYTES);
        s_stats.frames_recv_dropped++;
        return ESP_ERR_NO_MEM;
    }

    /* Copy JPEG payload into the slot under the mutex.  The renderer's
     * dsc still references the slot bytes; LVGL TJPGD only touches
     * them during draw on the LVGL thread, which we serialize via
     * tab5_lv_async_call below. */
    xSemaphoreTake(s_recv_mux, portMAX_DELAY);
    memcpy(s_recv_slot, wire_bytes + 8, payload_len);
    uint16_t jw = 0, jh = 0;
    jpeg_dims(s_recv_slot, payload_len, &jw, &jh);
    if (jw == 0 || jh == 0) {
        /* Sensible default — keeps LVGL happy even if SOF parse missed. */
        jw = 1280; jh = 720;
    }
    s_recv_dsc.header.w   = jw;
    s_recv_dsc.header.h   = jh;
    s_recv_dsc.header.cf  = LV_COLOR_FORMAT_RAW;
    s_recv_dsc.data_size  = payload_len;
    s_recv_dsc.data       = s_recv_slot;
    s_stats.frames_recv++;
    s_stats.bytes_recv          += len;
    s_stats.last_recv_jpeg_bytes = payload_len;
    xSemaphoreGive(s_recv_mux);

    tab5_lv_async_call(downlink_render_async, NULL);
    return ESP_OK;
}
