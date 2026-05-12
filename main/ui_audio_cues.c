/*
 * ui_audio_cues.c — see header.
 *
 * Wave 8 of the cross-stack cohesion audit (2026-05-11).
 */
#include "ui_audio_cues.h"

#include <math.h>
#include <stdlib.h>

#include "audio.h" /* tab5_audio_speaker_enable, tab5_audio_play_raw */
#include "debug_obs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "task_worker.h"

static const char *TAG = "ui_audio_cues";

/* Per-cue PCM + sample count.  48 kHz mono int16. */
typedef struct {
   int16_t *pcm;
   size_t samples;
   const char *obs_tag;
} cue_entry_t;

static cue_entry_t s_cues[UI_CUE_COUNT];

#define SAMPLE_RATE 48000

/* Generate a sine wave with linear attack + release envelope.  Volume
 * capped at `amp_q15` (0..32767).  Buffer must already be allocated
 * to `samples` int16s. */
static void gen_sine_envelope(int16_t *buf, size_t samples, float freq_hz, int amp_q15) {
   const size_t attack = SAMPLE_RATE / 100;  /* 10 ms */
   const size_t release = SAMPLE_RATE / 100; /* 10 ms */
   if (samples == 0) return;
   for (size_t i = 0; i < samples; i++) {
      float t = (float)i / (float)SAMPLE_RATE;
      float env = 1.0f;
      if (i < attack) {
         env = (float)i / (float)attack;
      } else if (samples > release && i > samples - release) {
         env = (float)(samples - i) / (float)release;
      }
      float sample = sinf(2.0f * (float)M_PI * freq_hz * t) * env;
      int v = (int)(sample * (float)amp_q15);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      buf[i] = (int16_t)v;
   }
}

static esp_err_t init_cue(ui_cue_t id, size_t samples, float freq_hz, int amp_q15, const char *obs_tag) {
   int16_t *pcm = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!pcm) {
      ESP_LOGW(TAG, "PSRAM alloc failed for cue %d", (int)id);
      return ESP_ERR_NO_MEM;
   }
   gen_sine_envelope(pcm, samples, freq_hz, amp_q15);
   s_cues[id].pcm = pcm;
   s_cues[id].samples = samples;
   s_cues[id].obs_tag = obs_tag;
   ESP_LOGI(TAG, "Cue %d ready: %u samples @ %.0fHz amp=%d", (int)id, (unsigned)samples, (double)freq_hz, amp_q15);
   return ESP_OK;
}

/* W7-E.0: two-tone bell.  Pre-computes two sine-envelope segments
 * back-to-back in one PSRAM buffer.  Each segment carries its own
 * attack/release so the transition between the two pitches doesn't
 * click.  Used for the high-priority "incoming message" cue —
 * recognizable as bell-shaped, distinct from the single-tone cues. */
static esp_err_t init_two_tone_cue(ui_cue_t id, size_t samples_each, float freq_a_hz, float freq_b_hz, int amp_q15,
                                   const char *obs_tag) {
   size_t total = samples_each * 2;
   int16_t *pcm = heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!pcm) {
      ESP_LOGW(TAG, "PSRAM alloc failed for cue %d", (int)id);
      return ESP_ERR_NO_MEM;
   }
   gen_sine_envelope(pcm, samples_each, freq_a_hz, amp_q15);
   gen_sine_envelope(pcm + samples_each, samples_each, freq_b_hz, amp_q15);
   s_cues[id].pcm = pcm;
   s_cues[id].samples = total;
   s_cues[id].obs_tag = obs_tag;
   ESP_LOGI(TAG, "Cue %d ready: %u samples @ %.0f→%.0fHz amp=%d", (int)id, (unsigned)total, (double)freq_a_hz,
            (double)freq_b_hz, amp_q15);
   return ESP_OK;
}

esp_err_t ui_audio_cues_init(void) {
   esp_err_t worst = ESP_OK;

   /* Mode switch: 80 ms 880 Hz (A5) — bright + confirmatory.
    * Volume ~30% of full-scale int16 so it's not startling. */
   if (!s_cues[UI_CUE_MODE_SWITCH].pcm) {
      esp_err_t r = init_cue(UI_CUE_MODE_SWITCH, SAMPLE_RATE / 1000 * 80, 880.0f, 32767 * 30 / 100, "mode_switch");
      if (r != ESP_OK) worst = r;
   }

   /* Cancel: 60 ms 220 Hz (A3) — sub-octave of mode_switch, low +
    * dismissive.  40% amplitude since low frequencies sound quieter
    * at the same digital level. */
   if (!s_cues[UI_CUE_CANCEL].pcm) {
      esp_err_t r = init_cue(UI_CUE_CANCEL, SAMPLE_RATE / 1000 * 60, 220.0f, 32767 * 40 / 100, "cancel");
      if (r != ESP_OK) worst = r;
   }

   /* Error: 120 ms 200 Hz — longer + lower than cancel so they
    * don't get confused.  Same 40% amplitude. */
   if (!s_cues[UI_CUE_ERROR].pcm) {
      esp_err_t r = init_cue(UI_CUE_ERROR, SAMPLE_RATE / 1000 * 120, 200.0f, 32767 * 40 / 100, "error");
      if (r != ESP_OK) worst = r;
   }

   /* W7-E.0: low-priority "incoming" toast ping — 30 ms 1200 Hz @ 30%
    * amplitude.  Short + soft so a Telegram message doesn't startle
    * the room; recognizable as "message arrived" by being brighter
    * and shorter than the existing mode_switch cue. */
   if (!s_cues[UI_CUE_INCOMING_LOW].pcm) {
      esp_err_t r = init_cue(UI_CUE_INCOMING_LOW, SAMPLE_RATE / 1000 * 30, 1200.0f, 32767 * 30 / 100, "incoming_low");
      if (r != ESP_OK) worst = r;
   }

   /* W7-E.0: high-priority "incoming" two-tone bell — 40 ms 880 Hz
    * then 40 ms 1320 Hz, each at 35% amplitude.  Bell-shaped
    * cadence (low→high) reads as "attention needed"; distinct from
    * both mode_switch (single tone) and error (long low growl). */
   if (!s_cues[UI_CUE_INCOMING_HIGH].pcm) {
      esp_err_t r = init_two_tone_cue(UI_CUE_INCOMING_HIGH, SAMPLE_RATE / 1000 * 40, 880.0f, 1320.0f, 32767 * 35 / 100,
                                      "incoming_high");
      if (r != ESP_OK) worst = r;
   }

   return worst;
}

typedef struct {
   ui_cue_t id;
} cue_job_t;

static void cue_play_job(void *arg) {
   cue_job_t *j = (cue_job_t *)arg;
   if (!j) return;
   if (j->id >= UI_CUE_COUNT || !s_cues[j->id].pcm) {
      free(j);
      return;
   }
   const cue_entry_t *c = &s_cues[j->id];
   tab5_audio_speaker_enable(true);
   esp_err_t r = tab5_audio_play_raw(c->pcm, c->samples);
   /* Disable amp after playback to avoid hiss/leakage between cues. */
   tab5_audio_speaker_enable(false);

   char detail[24];
   snprintf(detail, sizeof detail, "%s err=%d", c->obs_tag ? c->obs_tag : "?", (int)r);
   tab5_debug_obs_event("ui.cue", detail);
   free(j);
}

void ui_audio_cue_play(ui_cue_t id) {
   if (id >= UI_CUE_COUNT) {
      ESP_LOGW(TAG, "Cue id out of range: %d", (int)id);
      return;
   }
   if (!s_cues[id].pcm) {
      ESP_LOGD(TAG, "Cue %d not initialised — skip", (int)id);
      return;
   }
   cue_job_t *j = malloc(sizeof(*j));
   if (!j) {
      ESP_LOGW(TAG, "cue_job_t alloc failed — dropping cue %d", (int)id);
      return;
   }
   j->id = id;
   esp_err_t r = tab5_worker_enqueue(cue_play_job, j, "cue");
   if (r != ESP_OK) {
      ESP_LOGW(TAG, "Worker queue full — dropping cue %d", (int)id);
      free(j);
   }
}
