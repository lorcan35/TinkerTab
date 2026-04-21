/**
 * TinkerTab — Media Cache (HTTP image download + PSRAM LRU cache)
 *
 * Pre-allocates 5 PSRAM slots at boot for decoded JPEG images.
 * Downloads JPEG from Dragon, stores raw bytes, lets LVGL TJPGD decode on draw.
 * Thread-safe via FreeRTOS mutex.
 */

#include "media_cache.h"
#include "settings.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "media_cache";

/* Download buffer size for JPEG (512KB each) */
#define DL_BUF_SIZE     (512 * 1024)
#define CHUNK_SIZE      4096

/* FNV-1a 32-bit hash */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 0x811c9dc5;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193;
    }
    return h;
}

typedef struct {
    uint32_t  hash;         /* FNV-1a of relative URL (0 = empty) */
    uint8_t  *data;         /* PSRAM slot for JPEG bytes */
    uint32_t  data_len;     /* actual JPEG byte count */
    uint32_t  tick;         /* last-access tick for LRU */
    uint16_t  width;        /* parsed JPEG width */
    uint16_t  height;       /* parsed JPEG height */
} cache_slot_t;

static cache_slot_t s_slots[MEDIA_CACHE_SLOTS];
static uint8_t     *s_dl_buf = NULL;   /* shared download buffer */
static SemaphoreHandle_t s_mutex = NULL;
static bool s_inited = false;

esp_err_t media_cache_init(void)
{
    if (s_inited) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate PSRAM slots */
    for (int i = 0; i < MEDIA_CACHE_SLOTS; i++) {
        s_slots[i].data = heap_caps_malloc(MEDIA_CACHE_SLOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_slots[i].data) {
            ESP_LOGE(TAG, "Failed to alloc slot %d (%d bytes)", i, MEDIA_CACHE_SLOT_BYTES);
            return ESP_ERR_NO_MEM;
        }
        s_slots[i].hash = 0;
        s_slots[i].data_len = 0;
        s_slots[i].tick = 0;
    }

    /* Shared download buffer */
    s_dl_buf = heap_caps_malloc(DL_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dl_buf) {
        ESP_LOGE(TAG, "Failed to alloc download buffer (%d bytes)", DL_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "Initialized: %d slots x %dKB + %dKB download buf",
             MEDIA_CACHE_SLOTS, MEDIA_CACHE_SLOT_BYTES / 1024, DL_BUF_SIZE / 1024);
    return ESP_OK;
}

/**
 * Find slot by URL hash. Returns index or -1.
 */
static int find_slot(uint32_t hash)
{
    for (int i = 0; i < MEDIA_CACHE_SLOTS; i++) {
        if (s_slots[i].hash == hash && s_slots[i].data_len > 0) return i;
    }
    return -1;
}

/**
 * Find LRU slot (lowest tick, or first empty).
 */
static int find_lru_slot(void)
{
    int best = 0;
    uint32_t best_tick = UINT32_MAX;
    for (int i = 0; i < MEDIA_CACHE_SLOTS; i++) {
        if (s_slots[i].hash == 0) return i;   /* empty = best */
        if (s_slots[i].tick < best_tick) {
            best_tick = s_slots[i].tick;
            best = i;
        }
    }
    return best;
}

esp_err_t media_cache_fetch(const char *relative_url, lv_image_dsc_t *out_dsc)
{
    if (!s_inited || !relative_url || !out_dsc) return ESP_ERR_INVALID_ARG;

    uint32_t hash = fnv1a(relative_url);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Check cache hit */
    int idx = find_slot(hash);
    if (idx >= 0) {
        s_slots[idx].tick = xTaskGetTickCount();
        out_dsc->header.w = s_slots[idx].width;
        out_dsc->header.h = s_slots[idx].height;
        out_dsc->header.cf = LV_COLOR_FORMAT_RAW;
        out_dsc->data_size = s_slots[idx].data_len;
        out_dsc->data = s_slots[idx].data;
        xSemaphoreGive(s_mutex);
        ESP_LOGD(TAG, "Cache hit: %s (slot %d)", relative_url, idx);
        return ESP_OK;
    }

    /* Cache miss — download from Dragon.
     *
     * Wave 7 B5 fix: widget_media events from /debug/widget_media arrive
     * with an ABSOLUTE URL ("http://192.168.1.91:3502/api/media/foo.jpg"),
     * while chat media events from the rich-media pipeline arrive RELATIVE
     * ("/api/media/foo.jpg"). Detect the scheme prefix and use the URL
     * verbatim when absolute; otherwise prepend the configured Dragon host.
     * Previously the prepend was unconditional, so absolute URLs became
     * "http://host:porthttp://host:port/..." → esp_http_client_open OOM
     * (error 257 == ESP_ERR_NO_MEM) and the decode path silently failed. */
    char full_url[384];
    if (strncasecmp(relative_url, "http://", 7) == 0
        || strncasecmp(relative_url, "https://", 8) == 0) {
        snprintf(full_url, sizeof(full_url), "%s", relative_url);
    } else {
        char host[64];
        tab5_settings_get_dragon_host(host, sizeof(host));
        uint16_t port = tab5_settings_get_dragon_port();
        snprintf(full_url, sizeof(full_url), "http://%s:%u%s",
                 host, port, relative_url);
    }

    ESP_LOGI(TAG, "Downloading: %s", full_url);

    esp_http_client_config_t cfg = {
        .url = full_url,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        xSemaphoreGive(s_mutex);
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) content_len = 0;

    /* Read data in chunks */
    uint32_t total = 0;
    int read_len;
    while (total < (uint32_t)DL_BUF_SIZE) {
        uint32_t remaining = DL_BUF_SIZE - total;
        uint32_t to_read = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
        read_len = esp_http_client_read(client, (char *)(s_dl_buf + total), to_read);
        if (read_len <= 0) break;
        total += read_len;
        /* Yield to let voice WS task run (2ms) */
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200 || total == 0) {
        ESP_LOGE(TAG, "Download failed: status=%d, bytes=%lu", status, (unsigned long)total);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %lu bytes (status %d)", (unsigned long)total, status);

    /* Check fits in slot */
    if (total > MEDIA_CACHE_SLOT_BYTES) {
        ESP_LOGW(TAG, "Image too large (%lu > %d), truncating",
                 (unsigned long)total, MEDIA_CACHE_SLOT_BYTES);
        total = MEDIA_CACHE_SLOT_BYTES;
    }

    /* Evict LRU and store */
    idx = find_lru_slot();
    if (s_slots[idx].hash != 0) {
        ESP_LOGD(TAG, "Evicting slot %d (hash=0x%08lx)", idx, (unsigned long)s_slots[idx].hash);
    }
    memcpy(s_slots[idx].data, s_dl_buf, total);
    s_slots[idx].hash = hash;
    s_slots[idx].data_len = total;
    s_slots[idx].tick = xTaskGetTickCount();

    /* Parse JPEG SOF marker to extract width/height.
     * SOF0 (0xFFC0) or SOF2 (0xFFC2) marker contains: length(2) + precision(1) + height(2) + width(2) */
    uint16_t jpg_w = 0, jpg_h = 0;
    for (uint32_t i = 0; i + 8 < total; i++) {
        if (s_slots[idx].data[i] == 0xFF &&
            (s_slots[idx].data[i + 1] == 0xC0 || s_slots[idx].data[i + 1] == 0xC2)) {
            jpg_h = (s_slots[idx].data[i + 5] << 8) | s_slots[idx].data[i + 6];
            jpg_w = (s_slots[idx].data[i + 7] << 8) | s_slots[idx].data[i + 8];
            ESP_LOGI(TAG, "JPEG dimensions: %ux%u", jpg_w, jpg_h);
            break;
        }
    }
    if (jpg_w == 0 || jpg_h == 0) {
        ESP_LOGW(TAG, "Could not parse JPEG dimensions, using defaults");
        jpg_w = MEDIA_CACHE_MAX_W;
        jpg_h = MEDIA_CACHE_MAX_H;
    }
    s_slots[idx].width = jpg_w;
    s_slots[idx].height = jpg_h;

    /* Fill output descriptor — TJPGD decodes at draw time */
    out_dsc->header.w = jpg_w;
    out_dsc->header.h = jpg_h;
    out_dsc->header.cf = LV_COLOR_FORMAT_RAW;
    out_dsc->data_size = total;
    out_dsc->data = s_slots[idx].data;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void media_cache_clear(void)
{
    if (!s_inited) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MEDIA_CACHE_SLOTS; i++) {
        s_slots[i].hash = 0;
        s_slots[i].data_len = 0;
        s_slots[i].tick = 0;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Cache cleared");
}

/* Wave 11 stability P1: heap_watchdog calls this each tick for a one-line
 * telemetry entry. Running count of occupied slots × slot byte cost gives
 * the current PSRAM footprint of the cache so a 5-slot × 567 KB ceiling
 * (~2.9 MB) is observable alongside the main PSRAM graph. */
void media_cache_stats(int *used_slots, unsigned *resident_kb)
{
    int used = 0;
    if (s_inited && s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (int i = 0; i < MEDIA_CACHE_SLOTS; i++) {
            if (s_slots[i].data_len > 0) used++;
        }
        xSemaphoreGive(s_mutex);
    }
    if (used_slots) *used_slots = used;
    if (resident_kb) *resident_kb = (unsigned)((used * MEDIA_CACHE_SLOT_BYTES) / 1024);
}
