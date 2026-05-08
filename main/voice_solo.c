/* voice_solo — Phase 1 skeleton.
 *
 * The real STT → LLM → TTS chain wires in over the next several
 * commits as openrouter_client.{c,h} grows verbs.  This stub gives
 * voice_modes.c a real symbol to link against and proves the route
 * plumbing end-to-end before we touch HTTP.
 *
 * TT #370 — see docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md
 */

#include "voice_solo.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "voice.h"

static const char *TAG = "voice_solo";

static bool s_initialized = false;
static volatile bool s_busy = false;

esp_err_t voice_solo_init(void) {
   if (s_initialized) return ESP_OK;
   /* Follow-up commits: solo_session_init, solo_rag_init. */
   s_initialized = true;
   ESP_LOGI(TAG, "voice_solo_init OK (skeleton)");
   return ESP_OK;
}

esp_err_t voice_solo_send_text(const char *text) {
   if (!text || !*text) return ESP_ERR_INVALID_ARG;
   /* Wired into the LLM streaming verb in a follow-up commit. */
   ESP_LOGW(TAG, "send_text stub — text='%s' (len=%u)", text, (unsigned)strlen(text));
   return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voice_solo_send_audio(int16_t *pcm, size_t samples) {
   if (!pcm) return ESP_ERR_INVALID_ARG;
   /* Wired into the STT/LLM/TTS chain in a follow-up commit. */
   ESP_LOGW(TAG, "send_audio stub — samples=%u", (unsigned)samples);
   heap_caps_free(pcm);
   return ESP_ERR_NOT_SUPPORTED;
}

void voice_solo_cancel(void) {
   /* Wired into openrouter_cancel_inflight in a follow-up commit. */
   s_busy = false;
}

bool voice_solo_busy(void) {
   return s_busy;
}
