// components/bsp/include/bsp_audio.h
// ES8311 audio codec: I2C control port (shared bus from bsp_i2c), I2S full-duplex data port.
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize codec and I2S. Calls bsp_i2c_init() internally (idempotent), no need to call it first. Safe to call repeatedly after success;
// on failure the codec interface and I2S channel created by this call are released, and the call can be retried after the fault is fixed.
esp_err_t bsp_audio_init(void);

// Set the sample format. Repeating the same format is cheap (the open codec is reused).
//
// WARNING, a pitfall that must be avoided: esp_codec_dev_open() returns OK immediately if the codec is ALREADY open and
//   does NOT reconfigure the sample rate. Without a close first, playing 8kHz after 16kHz would be clocked out at 16k -
//   pitch and speed both double. So this function closes then opens when the format changes.
// Control-port and data-port errors are also checked (not relying on codec-dev to propagate errors); on failure it stops
// I2S, forces sleep and releases the codec object, and the next call rebuilds it, never reusing a half-open state.
// The caller must serialize format/sleep/wake operations and stop all PCM reads and writes before switching.
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch);

// Before entering low power, force the full ES8311 suspend register sequence and read back the key registers
// (REG0E compares only bits 6:0, other registers compare all bits),
// retry once on failure and explicitly stop I2S TX/RX. This path does not depend on whether codec-dev was ever opened,
// so it can pause correctly even if nothing has played since boot. All PCM reads/writes must be stopped before calling; idempotent;
// if audio is not initialized there is nothing to do and it returns success. Repeated calls after a failure keep the failed result.
// Sleep releases the codec object but keeps the shared I2C bus and I2S channel; it does not rely on the codec
// internal enabled flag to recover; wake recreates the codec and restores volume/format.
esp_err_t bsp_audio_sleep(void);

// Deep sleep only: make sure I2S TX/RX are stopped, then set MCLK/BCLK/WS/DOUT/DIN
// to floating inputs with no pull-up/pull-down. I2S cannot be resumed in this run afterwards; the caller must immediately
// enter deep sleep or reboot; not for light sleep.
esp_err_t bsp_audio_prepare_deep_sleep(void);

// Restore the ES8311 and the pre-sleep sample format after returning from light sleep. Idempotent; deep sleep
// wake restarts the app, which must re-initialize via bsp_audio_init() in the normal boot flow.
esp_err_t bsp_audio_wake(void);

// Playback / recording. bytes is a byte count (for 16-bit mono = samples x 2).
esp_err_t bsp_audio_write(const void *pcm, size_t bytes);
esp_err_t bsp_audio_read(void *pcm, size_t bytes);

// TV mode: bounded I2S write, output mute, and end-of-stream silence.
#include <stdbool.h>
esp_err_t bsp_audio_write_timeout(const void *pcm, size_t bytes, size_t *written, uint32_t timeout_ms);
void bsp_audio_set_mute(bool mute);
esp_err_t bsp_audio_stream_close(void);

// Output volume 0..100 (%).
void bsp_audio_set_volume(uint8_t percent);

#ifdef __cplusplus
}
#endif
