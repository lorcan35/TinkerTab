/* main/voice_dictation_lvgl.h — LVGL-thread-marshalling wrapper around
 * voice_dictation_subscribe().  Lives in its own file because pulling
 * tab5_lv_async_call into voice_dictation.c would break the host test
 * build (host shims don't cover ui_core.h). */
#pragma once

#include "voice_dictation.h"

#ifdef __cplusplus
extern "C" {
#endif

int voice_dictation_subscribe_lvgl(dict_subscriber_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
