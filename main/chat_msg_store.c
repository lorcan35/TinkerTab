/**
 * Chat Message Store — per-mode ring buffers for conversation history.
 *
 * Pure C, no LVGL dependency. Each voice mode (Local, Hybrid, Cloud, TinkerClaw)
 * has its own ring buffer of BSP_CHAT_MAX_MESSAGES entries. Messages are stored
 * as structs with text, media URLs, timestamps, and cached render heights.
 *
 * PSRAM allocation: The message arrays are allocated from PSRAM (SPIRAM) at
 * init time to avoid consuming ~350KB of internal SRAM. Each chat_msg_t is
 * ~908 bytes, and 4 modes * 100 messages = ~354KB.
 *
 * Thread safety: NOT thread-safe. All access must be from the LVGL thread
 * (Core 0) or protected by the LVGL lock. The push functions in ui_chat.c
 * use lv_async_call to ensure this.
 */

#include "chat_msg_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "chat_store";

/* Allocated from PSRAM at init */
static chat_msg_t *s_messages[CHAT_MODE_COUNT];
static int         s_count[CHAT_MODE_COUNT];
static int         s_write_idx[CHAT_MODE_COUNT];
static bool        s_inited = false;

void chat_store_init(void)
{
    if (s_inited) return;

    memset(s_count, 0, sizeof(s_count));
    memset(s_write_idx, 0, sizeof(s_write_idx));

    for (int i = 0; i < CHAT_MODE_COUNT; i++) {
        s_messages[i] = heap_caps_calloc(BSP_CHAT_MAX_MESSAGES, sizeof(chat_msg_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_messages[i]) {
            ESP_LOGE(TAG, "PSRAM alloc failed for mode %d (%d bytes)",
                     i, (int)(BSP_CHAT_MAX_MESSAGES * sizeof(chat_msg_t)));
            /* Fatal — can't recover. Leave NULL, callers will get NULL from get(). */
        }
    }

    s_inited = true;
    ESP_LOGI(TAG, "Init: %d modes x %d msgs x %d bytes = %d KB (PSRAM)",
             CHAT_MODE_COUNT, BSP_CHAT_MAX_MESSAGES, (int)sizeof(chat_msg_t),
             (int)(CHAT_MODE_COUNT * BSP_CHAT_MAX_MESSAGES * sizeof(chat_msg_t) / 1024));
}

int chat_store_add(uint8_t mode, const chat_msg_t *msg)
{
    if (mode >= CHAT_MODE_COUNT || !msg || !s_messages[mode]) return -1;

    int idx = s_write_idx[mode];
    s_messages[mode][idx] = *msg;
    s_messages[mode][idx].active = true;
    s_messages[mode][idx].height_px = 0;  /* uncached — measure on first render */

    s_write_idx[mode] = (idx + 1) % BSP_CHAT_MAX_MESSAGES;
    if (s_count[mode] < BSP_CHAT_MAX_MESSAGES) {
        s_count[mode]++;
    }
    /* If count == MAX, oldest message is overwritten (ring buffer) */

    return s_count[mode] - 1;  /* return logical index of new message */
}

int chat_store_count(uint8_t mode)
{
    if (mode >= CHAT_MODE_COUNT) return 0;
    return s_count[mode];
}

const chat_msg_t *chat_store_get(uint8_t mode, int index)
{
    if (mode >= CHAT_MODE_COUNT || index < 0 || index >= s_count[mode]) return NULL;
    if (!s_messages[mode]) return NULL;

    /* Ring buffer: oldest message is at (write_idx - count) wrapped */
    int start = (s_write_idx[mode] - s_count[mode] + BSP_CHAT_MAX_MESSAGES) % BSP_CHAT_MAX_MESSAGES;
    int real_idx = (start + index) % BSP_CHAT_MAX_MESSAGES;
    return &s_messages[mode][real_idx];
}

chat_msg_t *chat_store_get_mut(uint8_t mode, int index)
{
    return (chat_msg_t *)chat_store_get(mode, index);
}

void chat_store_clear(uint8_t mode)
{
    if (mode >= CHAT_MODE_COUNT) return;
    if (s_messages[mode])
        memset(s_messages[mode], 0, BSP_CHAT_MAX_MESSAGES * sizeof(chat_msg_t));
    s_count[mode] = 0;
    s_write_idx[mode] = 0;
}

void chat_store_clear_all(void)
{
    for (int i = 0; i < CHAT_MODE_COUNT; i++) {
        chat_store_clear(i);
    }
}

chat_msg_t *chat_store_last(uint8_t mode)
{
    if (mode >= CHAT_MODE_COUNT || s_count[mode] == 0) return NULL;
    if (!s_messages[mode]) return NULL;
    int idx = (s_write_idx[mode] - 1 + BSP_CHAT_MAX_MESSAGES) % BSP_CHAT_MAX_MESSAGES;
    return &s_messages[mode][idx];
}
