/*
 * voice_codec.c — see header.
 *
 * The 20 ms / 320-sample frame size matches both:
 *   - voice.c's existing mic chunk size (VOICE_CHUNK_SAMPLES)
 *   - the typical OPUS frame the gateway is expected to send back
 * so encode/decode is a 1:1 chunk mapping with no buffering needed.
 */
#include "voice_codec.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"

static const char *TAG = "voice_codec";

/* Wire format constants — see voice.c: 16 kHz mono, 16-bit PCM. */
#define VC_SAMPLE_RATE  16000
#define VC_CHANNELS     1
#define VC_BITS         16
#define VC_FRAME_MS     20
#define VC_FRAME_SAMPS  ((VC_SAMPLE_RATE / 1000) * VC_FRAME_MS)  /* 320 */

/* OPUS bitrate target.  24 kbps @ 16 kHz mono with VOIP application
 * mode is comfortably above the cliff for intelligibility while still
 * giving ~10x bandwidth reduction over raw PCM (256 kbps). */
#define VC_OPUS_BITRATE 24000

static voice_codec_t s_uplink   = VOICE_CODEC_PCM;
static voice_codec_t s_downlink = VOICE_CODEC_PCM;

static void *s_enc = NULL;   /* opaque OPUS encoder handle */
static void *s_dec = NULL;   /* opaque OPUS decoder handle */
static int   s_enc_in_size  = 0;   /* bytes per input frame (== 640) */
static int   s_enc_out_size = 0;   /* recommended output buffer size */

static bool  s_inited = false;

esp_err_t voice_codec_init(void)
{
    if (s_inited) return ESP_OK;
    s_inited = true;
    ESP_LOGI(TAG, "init (default uplink=PCM downlink=PCM)");
    return ESP_OK;
}

static esp_err_t ensure_encoder(void)
{
    if (s_enc) return ESP_OK;
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate      = VC_SAMPLE_RATE;
    cfg.channel          = VC_CHANNELS;
    cfg.bits_per_sample  = VC_BITS;
    cfg.bitrate          = VC_OPUS_BITRATE;
    cfg.frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    /* complexity=0 minimises encoder CPU + memory footprint.  The
     * #263 baseline tested complexity=5 + crashed; that was actually
     * a stack-overflow misdiagnosed as "encoder bug" — TT #264 Wave
     * 19 bumped MIC_TASK_STACK_SIZE 16→32 KB and the encoder now
     * runs cleanly.  Higher complexity values would work too once
     * the stack is sized for SILK NSQ's working storage; 0 stays the
     * default because Tab5's bandwidth target is met cleanly + the
     * CPU saving is genuine on the slower P4 cores. */
    cfg.complexity       = 0;
    cfg.enable_fec       = false;
    cfg.enable_dtx       = false;
    cfg.enable_vbr       = true;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_enc) != ESP_AUDIO_ERR_OK || !s_enc) {
        ESP_LOGE(TAG, "esp_opus_enc_open failed");
        s_enc = NULL;
        return ESP_FAIL;
    }
    if (esp_opus_enc_get_frame_size(s_enc, &s_enc_in_size, &s_enc_out_size) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_opus_enc_get_frame_size failed");
        esp_opus_enc_close(s_enc);
        s_enc = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OPUS encoder ready (in=%d B/frame, out_recommend=%d B, bitrate=%d bps)",
             s_enc_in_size, s_enc_out_size, VC_OPUS_BITRATE);
    return ESP_OK;
}

static esp_err_t ensure_decoder(void)
{
    if (s_dec) return ESP_OK;
    esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    cfg.sample_rate    = VC_SAMPLE_RATE;
    cfg.channel        = VC_CHANNELS;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    cfg.self_delimited = false;
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &s_dec) != ESP_AUDIO_ERR_OK || !s_dec) {
        ESP_LOGE(TAG, "esp_opus_dec_open failed");
        s_dec = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OPUS decoder ready");
    return ESP_OK;
}

void voice_codec_deinit(void)
{
    if (s_enc) { esp_opus_enc_close(s_enc); s_enc = NULL; }
    if (s_dec) { esp_opus_dec_close(s_dec); s_dec = NULL; }
    s_uplink   = VOICE_CODEC_PCM;
    s_downlink = VOICE_CODEC_PCM;
    s_inited   = false;
    ESP_LOGI(TAG, "deinit");
}

void voice_codec_set_uplink(voice_codec_t c)
{
    if (c == s_uplink) return;
#if !VOICE_CODEC_OPUS_UPLINK_ENABLED
    if (c == VOICE_CODEC_OPUS) {
        ESP_LOGW(TAG, "uplink OPUS request ignored — encoder disabled at compile-time "
                      "(VOICE_CODEC_OPUS_UPLINK_ENABLED=0); staying PCM");
        return;
    }
#endif
    ESP_LOGI(TAG, "uplink: %s -> %s", voice_codec_to_name(s_uplink), voice_codec_to_name(c));
    s_uplink = c;
    if (c == VOICE_CODEC_OPUS) {
        if (ensure_encoder() != ESP_OK) {
            ESP_LOGW(TAG, "OPUS encoder init failed; falling back to PCM uplink");
            s_uplink = VOICE_CODEC_PCM;
        }
    }
}

void voice_codec_set_downlink(voice_codec_t c)
{
    if (c == s_downlink) return;
    ESP_LOGI(TAG, "downlink: %s -> %s", voice_codec_to_name(s_downlink), voice_codec_to_name(c));
    s_downlink = c;
    if (c == VOICE_CODEC_OPUS) {
        if (ensure_decoder() != ESP_OK) {
            ESP_LOGW(TAG, "OPUS decoder init failed; falling back to PCM downlink");
            s_downlink = VOICE_CODEC_PCM;
        }
    }
}

voice_codec_t voice_codec_get_uplink(void)   { return s_uplink; }
voice_codec_t voice_codec_get_downlink(void) { return s_downlink; }

esp_err_t voice_codec_encode_uplink(const int16_t *pcm_samples,
                                    size_t n_samples,
                                    uint8_t *out, size_t out_cap,
                                    size_t *out_len)
{
    if (!pcm_samples || !out || !out_len) return ESP_ERR_INVALID_ARG;

    if (s_uplink == VOICE_CODEC_PCM) {
        size_t bytes = n_samples * sizeof(int16_t);
        if (bytes > out_cap) return ESP_ERR_INVALID_SIZE;
        memcpy(out, pcm_samples, bytes);
        *out_len = bytes;
        return ESP_OK;
    }

    /* OPUS: expect exactly one 20 ms frame (320 samples). */
    if (!s_enc) {
        ESP_LOGW(TAG, "encode_uplink called with no encoder — falling back to PCM");
        size_t bytes = n_samples * sizeof(int16_t);
        if (bytes > out_cap) return ESP_ERR_INVALID_SIZE;
        memcpy(out, pcm_samples, bytes);
        *out_len = bytes;
        return ESP_OK;
    }
    if ((int)(n_samples * sizeof(int16_t)) != s_enc_in_size) {
        ESP_LOGW(TAG, "encode_uplink: bad frame size %u (want %d B)",
                 (unsigned)(n_samples * sizeof(int16_t)), s_enc_in_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((int)out_cap < s_enc_out_size) {
        ESP_LOGW(TAG, "encode_uplink: out_cap %u < recommended %d",
                 (unsigned)out_cap, s_enc_out_size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_audio_enc_in_frame_t  in  = {
        .buffer = (uint8_t *)pcm_samples,
        .len    = (uint32_t)(n_samples * sizeof(int16_t)),
    };
    esp_audio_enc_out_frame_t outf = {
        .buffer = out,
        .len    = (uint32_t)out_cap,
    };
    esp_audio_err_t er = esp_opus_enc_process(s_enc, &in, &outf);
    if (er != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "esp_opus_enc_process failed: %d", (int)er);
        return ESP_FAIL;
    }
    *out_len = outf.encoded_bytes;
    return ESP_OK;
}

esp_err_t voice_codec_decode_downlink(const uint8_t *data, size_t len,
                                      int16_t *out_pcm, size_t out_cap,
                                      size_t *out_n)
{
    if (!data || !out_pcm || !out_n) return ESP_ERR_INVALID_ARG;

    if (s_downlink == VOICE_CODEC_PCM) {
        size_t samples = len / sizeof(int16_t);
        if (samples > out_cap) samples = out_cap;
        memcpy(out_pcm, data, samples * sizeof(int16_t));
        *out_n = samples;
        return ESP_OK;
    }

    if (!s_dec) {
        ESP_LOGW(TAG, "decode_downlink called with no decoder — treating as PCM");
        size_t samples = len / sizeof(int16_t);
        if (samples > out_cap) samples = out_cap;
        memcpy(out_pcm, data, samples * sizeof(int16_t));
        *out_n = samples;
        return ESP_OK;
    }

    esp_audio_dec_in_raw_t raw = {
        .buffer        = (uint8_t *)data,
        .len           = (uint32_t)len,
        .consumed      = 0,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t outf = {
        .buffer       = (uint8_t *)out_pcm,
        .len          = (uint32_t)(out_cap * sizeof(int16_t)),
        .needed_size  = 0,
        .decoded_size = 0,
    };
    esp_audio_dec_info_t info = {0};
    esp_audio_err_t er = esp_opus_dec_decode(s_dec, &raw, &outf, &info);
    if (er != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "esp_opus_dec_decode failed: %d", (int)er);
        return ESP_FAIL;
    }
    *out_n = outf.decoded_size / sizeof(int16_t);
    return ESP_OK;
}

voice_codec_t voice_codec_from_name(const char *name)
{
    if (!name) return VOICE_CODEC_PCM;
    if (strcasecmp(name, "opus") == 0) return VOICE_CODEC_OPUS;
    return VOICE_CODEC_PCM;
}

const char *voice_codec_to_name(voice_codec_t c)
{
    switch (c) {
    case VOICE_CODEC_OPUS: return "opus";
    case VOICE_CODEC_PCM:
    default:               return "pcm";
    }
}

/* #272 Phase 3E: call-audio framing helpers. */

bool voice_codec_peek_call_audio_magic(const void *data, size_t len)
{
    if (!data || len < 4) return false;
    const uint8_t *b = (const uint8_t *)data;
    return b[0] == 'A' && b[1] == 'U' && b[2] == 'D' && b[3] == '0';
}

size_t voice_codec_pack_call_audio(uint8_t *out, size_t out_cap,
                                   const void *body, size_t body_len)
{
    if (!out || !body) return 0;
    if (out_cap < body_len + VOICE_CALL_AUDIO_HEADER_LEN) return 0;
    out[0] = 'A'; out[1] = 'U'; out[2] = 'D'; out[3] = '0';
    out[4] = (uint8_t)((body_len >> 24) & 0xff);
    out[5] = (uint8_t)((body_len >> 16) & 0xff);
    out[6] = (uint8_t)((body_len >>  8) & 0xff);
    out[7] = (uint8_t)( body_len        & 0xff);
    memcpy(out + VOICE_CALL_AUDIO_HEADER_LEN, body, body_len);
    return body_len + VOICE_CALL_AUDIO_HEADER_LEN;
}

bool voice_codec_unpack_call_audio(const uint8_t *wire, size_t wire_len,
                                   const uint8_t **out_body, size_t *out_body_len)
{
    if (!wire || !out_body || !out_body_len) return false;
    if (wire_len < VOICE_CALL_AUDIO_HEADER_LEN) return false;
    if (!voice_codec_peek_call_audio_magic(wire, wire_len)) return false;
    uint32_t body_len = ((uint32_t)wire[4] << 24)
                      | ((uint32_t)wire[5] << 16)
                      | ((uint32_t)wire[6] <<  8)
                      | ((uint32_t)wire[7]);
    if (body_len + VOICE_CALL_AUDIO_HEADER_LEN != wire_len) return false;
    *out_body     = wire + VOICE_CALL_AUDIO_HEADER_LEN;
    *out_body_len = body_len;
    return true;
}
