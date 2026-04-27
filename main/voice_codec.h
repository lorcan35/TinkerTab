/*
 * voice_codec.h — OPUS encode/decode for the Tab5 ↔ Dragon voice WS
 * (#262 / #263).  Wraps the espressif/esp_audio_codec OPUS engine for
 * the specific shape of our pipeline: 16 kHz mono, 16-bit PCM, 20 ms
 * frames (320 samples = 640 bytes per chunk).
 *
 * Codec is negotiated via WS handshake:
 *   - Tab5 advertises capabilities.audio_codec = ["pcm","opus"] in
 *     `register`.
 *   - Dragon picks one and replies with config_update.audio_codec.
 *   - Tab5 calls voice_codec_set_uplink/downlink per the chosen codec
 *     before sending mic / decoding TTS.
 *
 * Default uplink + downlink = PCM (current behavior).  When the WS
 * connection drops the codec resets to PCM so a stale OPUS state
 * doesn't outlive the reconnect.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Compile-time gate for the OPUS uplink (mic) path.  The
 * espressif/esp_audio_codec OPUS encoder crashes reproducibly inside
 * silk_NSQ_c / silk_NSQ_del_dec_c on this build (PSRAM-backed PCM
 * input + complexity 0 or 5 both reproduce; suspect a SIMD alignment
 * or working-buffer issue specific to the ESP32-P4 port).  Until that
 * is root-caused we keep the encoder turned off — Tab5 only advertises
 * PCM uplink in capabilities, the encode helper short-circuits to PCM
 * even if voice_codec_set_uplink(OPUS) is called.
 *
 * Decoder is unaffected (different SILK / CELT code paths) so the
 * downlink path is left enabled — Phase 2B will exercise it. */
#define VOICE_CODEC_OPUS_UPLINK_ENABLED 0

typedef enum {
    VOICE_CODEC_PCM  = 0,   /* raw int16_t LE @ 16 kHz mono — default */
    VOICE_CODEC_OPUS = 1,
} voice_codec_t;

/* Initialize the codec module.  Idempotent.  Allocates encoder +
 * decoder lazily on first use; this function just makes the module
 * usable.  Returns ESP_OK on success. */
esp_err_t voice_codec_init(void);

/* Tear down all encoder/decoder state.  Called on voice_disconnect or
 * deinit.  Safe to call multiple times. */
void voice_codec_deinit(void);

/* Set the uplink codec (mic → Dragon).  Called by the WS RX handler
 * after Dragon's config_update arrives. */
void voice_codec_set_uplink(voice_codec_t c);

/* Set the downlink codec (Dragon → Tab5 TTS audio). */
void voice_codec_set_downlink(voice_codec_t c);

/* Read current uplink/downlink codec. */
voice_codec_t voice_codec_get_uplink(void);
voice_codec_t voice_codec_get_downlink(void);

/* Encode one 20 ms PCM chunk (16 kHz mono int16_t).
 *
 * @param pcm_samples  Pointer to PCM samples.
 * @param n_samples    Number of int16_t samples.  Must be exactly 320
 *                     (= 20 ms @ 16 kHz) when codec=OPUS.
 * @param out          Output buffer (caller-owned).
 * @param out_cap      Capacity of out in bytes.
 * @param[out] out_len Bytes written.
 *
 * For PCM uplink this is just a memcpy.  For OPUS it produces a
 * variable-length packet (~60 bytes typical at 24 kbps).
 *
 * Returns ESP_OK on success. */
esp_err_t voice_codec_encode_uplink(const int16_t *pcm_samples,
                                    size_t n_samples,
                                    uint8_t *out, size_t out_cap,
                                    size_t *out_len);

/* Decode one downlink frame back to int16_t mono 16 kHz PCM.
 *
 * For PCM downlink this is just a memcpy / type-pun.  For OPUS this
 * decodes the packet into PCM samples (typically 320 per frame for a
 * 20 ms packet, but variable based on the encoder's choice).
 *
 * @param data         Encoded packet bytes (or PCM bytes if codec=PCM).
 * @param len          Length of data in bytes.
 * @param out_pcm      Output PCM buffer.
 * @param out_cap      Capacity of out_pcm in samples (int16_t units).
 * @param[out] out_n   Number of int16_t samples written.
 *
 * Returns ESP_OK on success. */
esp_err_t voice_codec_decode_downlink(const uint8_t *data, size_t len,
                                      int16_t *out_pcm, size_t out_cap,
                                      size_t *out_n);

/* Convenience: name → enum + enum → name. */
voice_codec_t voice_codec_from_name(const char *name);
const char *voice_codec_to_name(voice_codec_t c);
