/**
 * Chat Message Store — session-scoped ring buffer.
 *
 * Single active session at a time (per chat v4·C spec §2.3). Switching
 * session wipes the buffer. The message array lives in PSRAM so the
 * ~90 KB footprint doesn't eat limited internal SRAM.
 *
 * Thread safety: NOT thread-safe by itself. Callers push via
 * lv_async_call so mutations happen on the LVGL thread (see ui_chat.c).
 */
#include "chat_msg_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "chat_store";

static chat_session_t s_active   = { .valid = false };
static chat_msg_t    *s_msgs     = NULL;   /* PSRAM-backed */
static int            s_count    = 0;
static int            s_write_idx = 0;
static bool           s_inited   = false;

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void chat_store_init(void)
{
    if (s_inited) return;

    s_msgs = heap_caps_calloc(BSP_CHAT_MAX_MESSAGES, sizeof(chat_msg_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_msgs) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%d bytes)",
                 (int)(BSP_CHAT_MAX_MESSAGES * sizeof(chat_msg_t)));
        return;
    }
    memset(&s_active, 0, sizeof(s_active));
    s_count = 0;
    s_write_idx = 0;
    s_inited = true;
    ESP_LOGI(TAG, "Init: %d msgs x %d bytes = %d KB (PSRAM, session-scoped)",
             BSP_CHAT_MAX_MESSAGES, (int)sizeof(chat_msg_t),
             (int)(BSP_CHAT_MAX_MESSAGES * sizeof(chat_msg_t) / 1024));
}

bool chat_store_set_session(const chat_session_t *s)
{
    if (!s_inited || !s_msgs) return false;
    if (!s) {
        memset(&s_active, 0, sizeof(s_active));
        memset(s_msgs, 0, BSP_CHAT_MAX_MESSAGES * sizeof(chat_msg_t));
        s_count = 0;
        s_write_idx = 0;
        return true;
    }
    s_active = *s;
    s_active.valid = true;
    memset(s_msgs, 0, BSP_CHAT_MAX_MESSAGES * sizeof(chat_msg_t));
    s_count = 0;
    s_write_idx = 0;
    ESP_LOGI(TAG, "Session switch -> id='%.16s' mode=%u model='%s'",
             s_active.session_id, s_active.voice_mode, s_active.llm_model);
    return true;
}

const chat_session_t *chat_store_active_session(void)
{
    return s_active.valid ? &s_active : NULL;
}

void chat_store_update_session_mode(uint8_t voice_mode, const char *llm_model)
{
    if (!s_active.valid) return;
    if (voice_mode <= 3) s_active.voice_mode = voice_mode;
    if (llm_model) copy_str(s_active.llm_model, sizeof(s_active.llm_model), llm_model);
}

int chat_store_add(const chat_msg_t *msg)
{
    if (!s_inited || !s_msgs || !msg) return -1;

    int real;
    if (s_count < BSP_CHAT_MAX_MESSAGES) {
        real = s_write_idx;
        s_write_idx = (s_write_idx + 1) % BSP_CHAT_MAX_MESSAGES;
        s_count++;
    } else {
        /* Ring is full — overwrite oldest (which is at s_write_idx). */
        real = s_write_idx;
        s_write_idx = (s_write_idx + 1) % BSP_CHAT_MAX_MESSAGES;
    }
    s_msgs[real] = *msg;
    s_msgs[real].active = true;
    if (s_msgs[real].height_px == 0) s_msgs[real].height_px = -1;
    return s_count - 1;
}

int chat_store_count(void) { return s_count; }

static int logical_to_real(int index)
{
    if (index < 0 || index >= s_count) return -1;
    int start = (s_write_idx - s_count + BSP_CHAT_MAX_MESSAGES) % BSP_CHAT_MAX_MESSAGES;
    return (start + index) % BSP_CHAT_MAX_MESSAGES;
}

const chat_msg_t *chat_store_get(int index)
{
    if (!s_inited || !s_msgs) return NULL;
    int real = logical_to_real(index);
    return real < 0 ? NULL : &s_msgs[real];
}

chat_msg_t *chat_store_get_mut(int index)
{
    return (chat_msg_t *)chat_store_get(index);
}

chat_msg_t *chat_store_last(void)
{
    return chat_store_get_mut(s_count - 1);
}

bool chat_store_set_height(int index, int16_t h)
{
    chat_msg_t *m = chat_store_get_mut(index);
    if (!m) return false;
    m->height_px = h;
    return true;
}

bool chat_store_update_last_text(const char *text)
{
    chat_msg_t *m = chat_store_last();
    if (!m) return false;
    copy_str(m->text, sizeof(m->text), text);
    m->height_px = -1;   /* invalidate cached height */
    return true;
}

int chat_store_attach_receipt_to_last_ai(uint32_t mils,
                                         uint16_t prompt_tok,
                                         uint16_t completion_tok,
                                         const char *model_short)
{
    /* Scan newest -> oldest looking for the first assistant-role bubble. */
    for (int i = s_count - 1; i >= 0; i--) {
        chat_msg_t *m = chat_store_get_mut(i);
        if (!m || !m->active) continue;
        if (m->is_user) continue;   /* skip user bubbles */
        if (m->type == MSG_SYSTEM) continue;  /* skip system messages */
        m->receipt_mils = mils;
        m->receipt_ptok = prompt_tok;
        m->receipt_ctok = completion_tok;
        copy_str(m->receipt_model_short, sizeof(m->receipt_model_short),
                 model_short ? model_short : "");
        /* Bubble gets a new subtitle line, so invalidate cached height. */
        m->height_px = -1;
        return i;
    }
    return -1;
}

bool chat_store_pop_last(void)
{
    if (s_count == 0) return false;
    int real = (s_write_idx - 1 + BSP_CHAT_MAX_MESSAGES) % BSP_CHAT_MAX_MESSAGES;
    memset(&s_msgs[real], 0, sizeof(chat_msg_t));
    s_write_idx = real;
    s_count--;
    return true;
}

void chat_store_clear(void)
{
    if (!s_inited || !s_msgs) return;
    memset(s_msgs, 0, BSP_CHAT_MAX_MESSAGES * sizeof(chat_msg_t));
    s_count = 0;
    s_write_idx = 0;
}
