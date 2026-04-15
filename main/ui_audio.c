/*
 * ui_audio.c — TinkerTab audio player overlay
 * 720x1280 portrait, LVGL v9, dark theme
 *
 * Bottom-half overlay for WAV file playback with play/pause and volume control.
 * Reads 16-bit PCM WAV from SD card in chunks via a FreeRTOS background task.
 */

#include "ui_audio.h"
#include "ui_core.h"
#include "audio.h"
#include "sdcard.h"
#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ui_audio";

/* ── Palette ─────────────────────────────────────────────────── */
#define COL_OVERLAY_BG  0x000000
#define COL_PANEL_BG    0x1E293B
#define COL_ACCENT      0x3B82F6
#define COL_WHITE       0xFFFFFF
#define COL_GRAY        0x888888
#define COL_RED         0xEF4444

/* ── Layout constants ────────────────────────────────────────── */
#define SCREEN_W        720
#define SCREEN_H        1280
#define OVERLAY_H       640
#define OVERLAY_Y       (SCREEN_H - OVERLAY_H)
#define PANEL_RADIUS    16
#define PLAY_BTN_SIZE   100
#define SLIDER_W        (SCREEN_W - 120)
#define CLOSE_BTN_SIZE  48

/* ── Audio buffer ────────────────────────────────────────────── */
#define AUDIO_CHUNK_SAMPLES  8192
#define PLAYBACK_STACK_SIZE  4096

/* ── WAV header (standard 44-byte RIFF/PCM) ──────────────────── */
typedef struct __attribute__((packed)) {
    char     riff_tag[4];       /* "RIFF" */
    uint32_t riff_size;
    char     wave_tag[4];       /* "WAVE" */
    char     fmt_tag[4];        /* "fmt " */
    uint32_t fmt_size;
    uint16_t audio_format;      /* 1 = PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data_tag[4];       /* "data" */
    uint32_t data_size;
} wav_header_t;

/* ── Module state ────────────────────────────────────────────── */
static lv_obj_t    *overlay       = NULL;
static lv_obj_t    *panel         = NULL;
static lv_obj_t    *lbl_filename  = NULL;
static lv_obj_t    *btn_play      = NULL;
static lv_obj_t    *lbl_play_icon = NULL;
static lv_obj_t    *slider_vol    = NULL;
static lv_obj_t    *lbl_status    = NULL;

static char         wav_filepath[256] = {0};
static bool         is_playing    = false;
static bool         playback_paused = false;
static bool         stop_requested = false;
static TaskHandle_t playback_task = NULL;

/* Forward declarations */
static void playback_task_fn(void *arg);

/* ── Event callbacks ─────────────────────────────────────────── */

static void cb_close(lv_event_t *e)
{
    (void)e;
    ui_audio_destroy();
}

static void cb_play_pause(lv_event_t *e)
{
    (void)e;

    if (!is_playing) {
        /* Start playback */
        is_playing = true;
        playback_paused = false;
        stop_requested = false;

        tab5_ui_lock();
        lv_label_set_text(lbl_play_icon, LV_SYMBOL_PAUSE);
        lv_obj_set_style_bg_color(btn_play, lv_color_hex(COL_ACCENT),
                                  LV_PART_MAIN);
        if (lbl_status) {
            lv_label_set_text(lbl_status, "Playing...");
        }
        tab5_ui_unlock();

        /* Launch playback task */
        if (playback_task == NULL) {
            xTaskCreate(playback_task_fn, "wav_play", PLAYBACK_STACK_SIZE,
                        NULL, 5, &playback_task);
        }
    } else if (!playback_paused) {
        /* Pause */
        playback_paused = true;

        tab5_ui_lock();
        lv_label_set_text(lbl_play_icon, LV_SYMBOL_PLAY);
        lv_obj_set_style_bg_color(btn_play, lv_color_hex(COL_PANEL_BG),
                                  LV_PART_MAIN);
        if (lbl_status) {
            lv_label_set_text(lbl_status, "Paused");
        }
        tab5_ui_unlock();
    } else {
        /* Resume */
        playback_paused = false;

        tab5_ui_lock();
        lv_label_set_text(lbl_play_icon, LV_SYMBOL_PAUSE);
        lv_obj_set_style_bg_color(btn_play, lv_color_hex(COL_ACCENT),
                                  LV_PART_MAIN);
        if (lbl_status) {
            lv_label_set_text(lbl_status, "Playing...");
        }
        tab5_ui_unlock();
    }
}

static void cb_volume_changed(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(sl);
    tab5_audio_set_volume((uint8_t)val);
}

/* ── WAV playback task (runs in background, does not block UI) ── */

static void playback_task_fn(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Playback task started: %s", wav_filepath);

    FILE *fp = fopen(wav_filepath, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open WAV: %s", wav_filepath);
        goto done;
    }

    /* Read and validate WAV header */
    wav_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        ESP_LOGE(TAG, "Failed to read WAV header");
        fclose(fp);
        goto done;
    }

    if (memcmp(hdr.riff_tag, "RIFF", 4) != 0 ||
        memcmp(hdr.wave_tag, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV file (bad RIFF/WAVE tags)");
        fclose(fp);
        goto done;
    }

    /* Seek to data chunk — handle non-standard headers by scanning */
    if (memcmp(hdr.fmt_tag, "fmt ", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV: missing fmt chunk");
        fclose(fp);
        goto done;
    }

    /* Skip past fmt chunk to find data chunk if header is extended */
    uint32_t fmt_end = 12 + 8 + hdr.fmt_size;  /* offset after fmt chunk */
    fseek(fp, fmt_end, SEEK_SET);

    /* Scan for "data" sub-chunk */
    char chunk_id[4];
    uint32_t chunk_size;
    bool found_data = false;
    while (fread(chunk_id, 1, 4, fp) == 4 && fread(&chunk_size, 1, 4, fp) == 4) {
        if (memcmp(chunk_id, "data", 4) == 0) {
            found_data = true;
            hdr.data_size = chunk_size;
            break;
        }
        /* Skip unknown chunk */
        fseek(fp, chunk_size, SEEK_CUR);
    }

    if (!found_data) {
        ESP_LOGE(TAG, "WAV file missing data chunk");
        fclose(fp);
        goto done;
    }

    if (hdr.audio_format != 1 || hdr.bits_per_sample != 16) {
        ESP_LOGE(TAG, "Only 16-bit PCM WAV supported (got fmt=%d bps=%d)",
                 hdr.audio_format, hdr.bits_per_sample);
        fclose(fp);
        goto done;
    }

    ESP_LOGI(TAG, "WAV: %lu Hz, %d ch, %lu bytes data",
             (unsigned long)hdr.sample_rate, hdr.num_channels,
             (unsigned long)hdr.data_size);

    /* Allocate audio buffer in PSRAM */
    int16_t *audio_buf = heap_caps_malloc(AUDIO_CHUNK_SAMPLES * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        fclose(fp);
        goto done;
    }

    /* Enable speaker */
    tab5_audio_speaker_enable(true);

    /* Read and play in chunks */
    uint32_t bytes_remaining = hdr.data_size;
    while (bytes_remaining > 0 && !stop_requested) {
        /* Handle pause */
        while (playback_paused && !stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (stop_requested) break;

        size_t chunk_bytes = AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
        if (chunk_bytes > bytes_remaining) {
            chunk_bytes = bytes_remaining;
        }

        size_t read_bytes = fread(audio_buf, 1, chunk_bytes, fp);
        if (read_bytes == 0) break;

        size_t samples = read_bytes / sizeof(int16_t);
        tab5_audio_play_raw(audio_buf, samples);

        bytes_remaining -= read_bytes;
    }

    /* Disable speaker */
    tab5_audio_speaker_enable(false);

    heap_caps_free(audio_buf);
    fclose(fp);

done:
    /* Update UI to stopped state */
    is_playing = false;
    playback_paused = false;

    tab5_ui_lock();
    if (lbl_play_icon) {
        lv_label_set_text(lbl_play_icon, LV_SYMBOL_PLAY);
    }
    if (btn_play) {
        lv_obj_set_style_bg_color(btn_play, lv_color_hex(COL_PANEL_BG),
                                  LV_PART_MAIN);
    }
    if (lbl_status) {
        lv_label_set_text(lbl_status, stop_requested ? "Stopped" : "Finished");
    }
    tab5_ui_unlock();

    playback_task = NULL;
    ESP_LOGI(TAG, "Playback task ended");
    vTaskSuspend(NULL);
}

/* ================================================================
 * ui_audio_create
 * ================================================================ */
lv_obj_t *ui_audio_create(const char *wav_path)
{
    if (!wav_path) return NULL;

    /* Clean up any existing overlay */
    if (overlay) {
        ui_audio_destroy();
    }

    strncpy(wav_filepath, wav_path, sizeof(wav_filepath) - 1);
    wav_filepath[sizeof(wav_filepath) - 1] = '\0';

    is_playing = false;
    playback_paused = false;
    stop_requested = false;

    /* Get the current active screen to overlay on */
    lv_obj_t *parent = lv_screen_active();

    /* ── Full-screen semi-transparent backdrop ───────────────── */
    overlay = lv_obj_create(parent);
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(COL_OVERLAY_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Bottom panel ────────────────────────────────────────── */
    panel = lv_obj_create(overlay);
    lv_obj_set_size(panel, SCREEN_W, OVERLAY_H);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL_BG), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    /* Rounded top corners only */
    lv_obj_set_style_radius(panel, PANEL_RADIUS, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Close button (top-right of panel) ───────────────────── */
    lv_obj_t *btn_close = lv_button_create(panel);
    lv_obj_set_size(btn_close, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_radius(btn_close, CLOSE_BTN_SIZE / 2, 0);
    lv_obj_set_style_shadow_width(btn_close, 0, 0);

    lv_obj_t *lbl_x = lv_label_create(btn_close);
    lv_label_set_text(lbl_x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(lbl_x, lv_color_hex(COL_WHITE), 0);
    lv_obj_center(lbl_x);

    lv_obj_add_event_cb(btn_close, cb_close, LV_EVENT_CLICKED, NULL);

    /* ── Audio icon ──────────────────────────────────────────── */
    lv_obj_t *lbl_audio_icon = lv_label_create(panel);
    lv_label_set_text(lbl_audio_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(lbl_audio_icon, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(lbl_audio_icon, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_audio_icon, LV_ALIGN_TOP_MID, 0, 40);

    /* ── Filename label ──────────────────────────────────────── */
    const char *basename = strrchr(wav_path, '/');
    basename = basename ? basename + 1 : wav_path;

    lbl_filename = lv_label_create(panel);
    lv_label_set_text(lbl_filename, basename);
    lv_label_set_long_mode(lbl_filename, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_filename, SCREEN_W - 120);
    lv_obj_set_style_text_color(lbl_filename, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_filename, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(lbl_filename, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_filename, LV_ALIGN_TOP_MID, 0, 110);

    /* ── Status label ────────────────────────────────────────── */
    lbl_status = lv_label_create(panel);
    lv_label_set_text(lbl_status, "Ready");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 145);

    /* ── Play/Pause button ───────────────────────────────────── */
    btn_play = lv_button_create(panel);
    lv_obj_set_size(btn_play, PLAY_BTN_SIZE, PLAY_BTN_SIZE);
    lv_obj_align(btn_play, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_set_style_bg_color(btn_play, lv_color_hex(COL_PANEL_BG), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_play, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_border_width(btn_play, 3, 0);
    lv_obj_set_style_radius(btn_play, PLAY_BTN_SIZE / 2, 0);
    lv_obj_set_style_shadow_width(btn_play, 0, 0);

    lbl_play_icon = lv_label_create(btn_play);
    lv_label_set_text(lbl_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(lbl_play_icon, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_play_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(lbl_play_icon);

    lv_obj_add_event_cb(btn_play, cb_play_pause, LV_EVENT_CLICKED, NULL);

    /* ── Volume section ──────────────────────────────────────── */
    lv_obj_t *lbl_vol_icon = lv_label_create(panel);
    lv_label_set_text(lbl_vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_vol_icon, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_vol_icon, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_vol_icon, LV_ALIGN_TOP_MID, 0, 340);

    /* Volume slider */
    slider_vol = lv_slider_create(panel);
    lv_obj_set_size(slider_vol, SLIDER_W, 12);
    lv_obj_align(slider_vol, LV_ALIGN_TOP_MID, 0, 380);
    lv_slider_set_range(slider_vol, 0, 100);
    lv_slider_set_value(slider_vol, tab5_audio_get_volume(), LV_ANIM_OFF);

    /* Slider track style */
    lv_obj_set_style_bg_color(slider_vol, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider_vol, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider_vol, 6, LV_PART_MAIN);

    /* Slider indicator (filled part) */
    lv_obj_set_style_bg_color(slider_vol, lv_color_hex(COL_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider_vol, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider_vol, 6, LV_PART_INDICATOR);

    /* Slider knob */
    lv_obj_set_style_bg_color(slider_vol, lv_color_hex(COL_WHITE), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider_vol, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_vol, 8, LV_PART_KNOB);

    lv_obj_add_event_cb(slider_vol, cb_volume_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Volume percentage label */
    char vol_text[16];
    snprintf(vol_text, sizeof(vol_text), "%d%%", tab5_audio_get_volume());
    lv_obj_t *lbl_vol_pct = lv_label_create(panel);
    lv_label_set_text(lbl_vol_pct, vol_text);
    lv_obj_set_style_text_color(lbl_vol_pct, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_vol_pct, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_vol_pct, LV_ALIGN_TOP_MID, 0, 410);

    ESP_LOGI(TAG, "Audio player overlay created for: %s", basename);

    return overlay;
}

/* ================================================================
 * ui_audio_destroy
 * ================================================================ */
void ui_audio_destroy(void)
{
    /* Signal playback task to stop */
    stop_requested = true;

    /* Wait for playback task to finish (up to 2 seconds) */
    int wait_count = 0;
    while (playback_task != NULL && wait_count < 40) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_count++;
    }

    /* Force-delete task if it didn't exit cleanly */
    if (playback_task != NULL) {
        ESP_LOGW(TAG, "Force-deleting playback task");
        vTaskDelete(playback_task);
        playback_task = NULL;
        tab5_audio_speaker_enable(false);
    }

    is_playing = false;
    playback_paused = false;

    if (overlay) {
        lv_obj_delete(overlay);
        overlay       = NULL;
        panel         = NULL;
        lbl_filename  = NULL;
        btn_play      = NULL;
        lbl_play_icon = NULL;
        slider_vol    = NULL;
        lbl_status    = NULL;
        ESP_LOGI(TAG, "Audio player overlay destroyed");
    }
}
