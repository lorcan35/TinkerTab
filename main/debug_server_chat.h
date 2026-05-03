/*
 * debug_server_chat.h — chat surface debug HTTP family.
 *
 * Wave 23b follow-up (#332): sixteenth per-family extract.  Owns the
 * 5 chat-overlay debug endpoints:
 *
 *   POST /chat              — send text to Dragon via voice WS
 *   GET  /chat/messages     — last N messages from chat_msg_store
 *   POST /chat/llm_done     — push assistant reply (md_strip + push_message)
 *   POST /chat/partial      — set the live STT-partial caption
 *   POST /chat/audio_clip   — inject an audio_clip row into the chat overlay
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers /chat /chat/messages /chat/llm_done /chat/partial
 * /chat/audio_clip against the live HTTPD server.  Called from
 * debug_server.c init after the server is started. */
void debug_server_chat_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
