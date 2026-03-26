/**
 * TinkerTab — Audio Player Overlay
 *
 * Bottom-half overlay for WAV playback with play/pause and volume.
 * 720x1280 portrait, LVGL v9, dark theme with #3B82F6 accent.
 */
#pragma once
#include "lvgl.h"

/** Show audio player for given WAV file path. Overlays on current screen. */
lv_obj_t *ui_audio_create(const char *wav_path);

/** Stop playback and destroy the audio player overlay. */
void ui_audio_destroy(void);
