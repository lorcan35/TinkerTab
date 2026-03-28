/**
 * TinkerTab -- UDP JPEG Stream Receiver
 *
 * Receives JPEG frames from Dragon over UDP for low-latency display.
 * Supports two modes:
 *   1. Chunked: large JPEGs split across multiple UDP datagrams
 *      [frame_num:4B BE][chunk_idx:2B BE][total_chunks:2B BE][jpeg_data...]
 *   2. Simple: entire JPEG in one datagram (relies on IP fragmentation)
 *      [frame_num:4B BE][jpeg_data...]
 *
 * Uses the ESP32-P4 hardware JPEG decoder and writes directly to the
 * DPI framebuffer. esp_cache_msync() is called after every decode so
 * the DPI DMA sees the updated pixels in PSRAM.
 */

#include "udp_stream.h"
#include "config.h"
#include "display.h"

#include <string.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

static const char *TAG = "udp_stream";

/* ---------------------------------------------------------------------- */
/* Configuration                                                          */
/* ---------------------------------------------------------------------- */

#define UDP_LISTEN_PORT      5000
#define UDP_RX_BUF_SIZE      65535   /* Max UDP payload after IP reassembly */
#define JPEG_REASSEMBLY_SIZE TAB5_JPEG_BUF_SIZE  /* 100 KB */
#define MAX_CHUNKS           128     /* Max chunks per frame */
#define STREAM_TIMEOUT_US    3000000 /* 3 seconds without a frame = disconnect */
#define HEADER_CHUNKED_SIZE  8       /* 4 + 2 + 2 */
#define HEADER_SIMPLE_SIZE   4       /* 4 bytes frame_num only */

/* ---------------------------------------------------------------------- */
/* State                                                                  */
/* ---------------------------------------------------------------------- */

static volatile bool s_running = false;
static volatile bool s_stop_flag = false;
static TaskHandle_t s_task_handle = NULL;
static udp_stream_disconnect_cb_t s_disconnect_cb = NULL;

/* FPS tracking */
static float s_fps = 0.0f;
static uint32_t s_frame_count = 0;
static uint32_t s_drop_count = 0;
static int64_t s_last_fps_time = 0;

/* Chunk reassembly state */
typedef struct {
    uint32_t frame_num;
    uint16_t total_chunks;
    uint16_t received_mask[(MAX_CHUNKS + 15) / 16]; /* bitfield */
    uint16_t received_count;
    uint8_t *buf;           /* JPEG reassembly buffer */
    uint32_t chunk_offsets[MAX_CHUNKS]; /* byte offset for each chunk */
    uint32_t chunk_sizes[MAX_CHUNKS];
    uint32_t total_size;    /* accumulated JPEG bytes */
} reassembly_t;

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static inline uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline uint16_t read_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline void reassembly_reset(reassembly_t *r, uint32_t frame_num, uint16_t total)
{
    r->frame_num = frame_num;
    r->total_chunks = total;
    r->received_count = 0;
    r->total_size = 0;
    memset(r->received_mask, 0, sizeof(r->received_mask));
    memset(r->chunk_offsets, 0, sizeof(r->chunk_offsets));
    memset(r->chunk_sizes, 0, sizeof(r->chunk_sizes));
}

static inline bool reassembly_has_chunk(const reassembly_t *r, uint16_t idx)
{
    return (r->received_mask[idx / 16] >> (idx % 16)) & 1;
}

static inline void reassembly_mark_chunk(reassembly_t *r, uint16_t idx)
{
    r->received_mask[idx / 16] |= (1 << (idx % 16));
    r->received_count++;
}

static inline bool reassembly_complete(const reassembly_t *r)
{
    return r->received_count >= r->total_chunks;
}

/* Build a contiguous JPEG from received chunks (in order). */
static uint32_t reassembly_build(const reassembly_t *r, uint8_t *out, uint32_t out_cap)
{
    uint32_t pos = 0;
    for (uint16_t i = 0; i < r->total_chunks && pos < out_cap; i++) {
        uint32_t sz = r->chunk_sizes[i];
        if (pos + sz > out_cap) break;
        /* Chunks are already stored sequentially in reassembly buf at their offsets */
        memcpy(out + pos, r->buf + r->chunk_offsets[i], sz);
        pos += sz;
    }
    return pos;
}

/* ---------------------------------------------------------------------- */
/* Decode + display                                                       */
/* ---------------------------------------------------------------------- */

static void decode_and_display(const uint8_t *jpeg_data, uint32_t jpeg_size)
{
    if (!tab5_display_is_jpeg_enabled()) {
        s_drop_count++;
        return;
    }

    esp_err_t ret = tab5_display_draw_jpeg(jpeg_data, jpeg_size);
    if (ret == ESP_OK) {
        s_frame_count++;
        int64_t now = esp_timer_get_time();
        int64_t elapsed = now - s_last_fps_time;
        if (elapsed >= 1000000) {
            s_fps = (float)s_frame_count * 1000000.0f / (float)elapsed;
            if (s_drop_count > 0) {
                ESP_LOGI(TAG, "%.1f FPS (dropped %lu)", s_fps, (unsigned long)s_drop_count);
                s_drop_count = 0;
            }
            s_frame_count = 0;
            s_last_fps_time = now;
        }
    } else {
        ESP_LOGW(TAG, "HW decode failed (size=%lu): %s",
                 (unsigned long)jpeg_size, esp_err_to_name(ret));
    }
}

/* ---------------------------------------------------------------------- */
/* Main task                                                              */
/* ---------------------------------------------------------------------- */

static void udp_stream_task(void *arg)
{
    ESP_LOGI(TAG, "UDP stream task started, port %d", UDP_LISTEN_PORT);

    /* Allocate RX buffer in PSRAM */
    uint8_t *rx_buf = (uint8_t *)heap_caps_malloc(
        UDP_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer (%d bytes)", UDP_RX_BUF_SIZE);
        goto exit_task;
    }

    /* Allocate JPEG reassembly buffer in PSRAM (DMA-aligned for HW decoder) */
    uint8_t *jpeg_buf = (uint8_t *)heap_caps_aligned_alloc(
        64, JPEG_REASSEMBLY_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG reassembly buffer");
        heap_caps_free(rx_buf);
        goto exit_task;
    }

    /* Second JPEG buffer for double-buffering: decode into one while
       the previous frame is still being DMA'd to the display. */
    uint8_t *jpeg_buf_b = (uint8_t *)heap_caps_aligned_alloc(
        64, JPEG_REASSEMBLY_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_buf_b) {
        ESP_LOGW(TAG, "Double-buffer alloc failed, falling back to single buffer");
        /* Non-fatal: we just won't double-buffer */
    }

    /* Reassembly state */
    reassembly_t reasm = {0};
    reasm.buf = jpeg_buf;
    uint8_t *decode_buf = jpeg_buf_b ? jpeg_buf_b : jpeg_buf;
    bool use_double_buf = (jpeg_buf_b != NULL);

    /* Create UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
        heap_caps_free(rx_buf);
        heap_caps_free(jpeg_buf);
        if (jpeg_buf_b) heap_caps_free(jpeg_buf_b);
        goto exit_task;
    }

    /* Bind to port */
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_LISTEN_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno %d", errno);
        close(sock);
        heap_caps_free(rx_buf);
        heap_caps_free(jpeg_buf);
        if (jpeg_buf_b) heap_caps_free(jpeg_buf_b);
        goto exit_task;
    }

    /* Set receive timeout for stop-flag polling */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 }; /* 200ms */
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Increase socket receive buffer for bursty traffic */
    int rcvbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    ESP_LOGI(TAG, "Listening on UDP :%d", UDP_LISTEN_PORT);

    s_last_fps_time = esp_timer_get_time();
    s_frame_count = 0;
    s_drop_count = 0;
    int64_t last_frame_time = esp_timer_get_time();

    while (!s_stop_flag) {
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        int len = recvfrom(sock, rx_buf, UDP_RX_BUF_SIZE, 0,
                           (struct sockaddr *)&src_addr, &src_len);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout — check for stream disconnect */
                int64_t now = esp_timer_get_time();
                if (last_frame_time > 0 && (now - last_frame_time) > STREAM_TIMEOUT_US) {
                    ESP_LOGW(TAG, "Stream timeout (no data for %.1fs)",
                             (float)(now - last_frame_time) / 1000000.0f);
                    break;
                }
                continue;
            }
            ESP_LOGE(TAG, "recvfrom error: errno %d", errno);
            break;
        }

        if (len < HEADER_SIMPLE_SIZE) {
            continue; /* Runt packet */
        }

        last_frame_time = esp_timer_get_time();

        uint32_t frame_num = read_u32_be(rx_buf);

        /* Detect mode: if total_chunks == 1 and chunk_idx == 0 with valid
           header, treat as chunked. Otherwise, if the packet looks like a
           complete JPEG (starts with FFD8 after 4-byte header), use simple mode.

           Heuristic: check if bytes 4-5 are FFD8 (simple mode) or parse
           chunk_idx/total_chunks (chunked mode). In chunked mode, chunk_idx
           and total_chunks are both > 0, and total_chunks >= 1. */

        bool is_simple = false;
        if (len >= HEADER_SIMPLE_SIZE + 2) {
            /* Check for JPEG SOI right after 4-byte frame_num */
            if (rx_buf[4] == 0xFF && rx_buf[5] == 0xD8) {
                is_simple = true;
            }
        }

        if (is_simple) {
            /* Simple mode: entire JPEG in one datagram */
            const uint8_t *jpeg_data = rx_buf + HEADER_SIMPLE_SIZE;
            uint32_t jpeg_size = len - HEADER_SIMPLE_SIZE;

            if (use_double_buf) {
                memcpy(decode_buf, jpeg_data, jpeg_size);
                decode_and_display(decode_buf, jpeg_size);
            } else {
                decode_and_display(jpeg_data, jpeg_size);
            }

        } else if (len >= HEADER_CHUNKED_SIZE) {
            /* Chunked mode */
            uint16_t chunk_idx = read_u16_be(rx_buf + 4);
            uint16_t total_chunks = read_u16_be(rx_buf + 6);

            if (total_chunks == 0 || total_chunks > MAX_CHUNKS || chunk_idx >= total_chunks) {
                continue; /* Invalid header */
            }

            const uint8_t *chunk_data = rx_buf + HEADER_CHUNKED_SIZE;
            uint32_t chunk_size = len - HEADER_CHUNKED_SIZE;

            /* New frame? Reset reassembly. Drop incomplete previous frame. */
            if (frame_num != reasm.frame_num) {
                if (reasm.received_count > 0 && !reassembly_complete(&reasm)) {
                    s_drop_count++;
                    ESP_LOGD(TAG, "Dropped incomplete frame %lu (%d/%d chunks)",
                             (unsigned long)reasm.frame_num,
                             reasm.received_count, reasm.total_chunks);
                }
                reassembly_reset(&reasm, frame_num, total_chunks);
            }

            /* Store chunk if not already received */
            if (!reassembly_has_chunk(&reasm, chunk_idx)) {
                /* Calculate offset: store chunks sequentially by index.
                   For simplicity, we store at a running offset and record it. */
                if (reasm.total_size + chunk_size <= JPEG_REASSEMBLY_SIZE) {
                    reasm.chunk_offsets[chunk_idx] = reasm.total_size;
                    reasm.chunk_sizes[chunk_idx] = chunk_size;
                    memcpy(reasm.buf + reasm.total_size, chunk_data, chunk_size);
                    reasm.total_size += chunk_size;
                    reassembly_mark_chunk(&reasm, chunk_idx);
                } else {
                    ESP_LOGW(TAG, "JPEG reassembly buffer overflow at chunk %d", chunk_idx);
                }
            }

            /* All chunks received? Decode! */
            if (reassembly_complete(&reasm)) {
                if (use_double_buf) {
                    /* Build contiguous JPEG into decode buffer */
                    uint32_t jpeg_size = reassembly_build(&reasm, decode_buf, JPEG_REASSEMBLY_SIZE);
                    decode_and_display(decode_buf, jpeg_size);
                } else {
                    /* Build in-place (reasm.buf is also decode_buf) */
                    /* Need a temp approach: build into the same buffer */
                    uint32_t jpeg_size = reassembly_build(&reasm, reasm.buf, JPEG_REASSEMBLY_SIZE);
                    decode_and_display(reasm.buf, jpeg_size);
                }
                /* Reset for next frame */
                reassembly_reset(&reasm, 0, 0);
            }
        }
    }

    /* Cleanup */
    close(sock);
    heap_caps_free(rx_buf);
    heap_caps_free(jpeg_buf);
    if (jpeg_buf_b) heap_caps_free(jpeg_buf_b);

exit_task:
    s_fps = 0.0f;
    s_running = false;
    s_task_handle = NULL;

    if (!s_stop_flag && s_disconnect_cb) {
        s_disconnect_cb();
    }

    ESP_LOGI(TAG, "UDP stream task exiting");
    vTaskSuspend(NULL);  /* P4 TLSP workaround (#20) */
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

void udp_stream_start(void)
{
    if (s_running) return;
    s_stop_flag = false;
    s_running = true;

    xTaskCreatePinnedToCore(
        udp_stream_task,
        "udp_stream",
        8192,
        NULL,
        configMAX_PRIORITIES - 2,   /* Same priority as MJPEG — high on Core 1 */
        &s_task_handle,
        1                           /* Core 1 — away from LVGL on Core 0 */
    );
}

void udp_stream_stop(void)
{
    if (!s_running) return;
    s_stop_flag = true;
}

bool udp_stream_is_active(void)
{
    return s_running;
}

float udp_stream_get_fps(void)
{
    return s_fps;
}

void udp_stream_set_disconnect_cb(udp_stream_disconnect_cb_t cb)
{
    s_disconnect_cb = cb;
}
