// Minimal audio output for the ZecTrix Note 4's ES8311 codec + NS4168 amp.
// Scope: play short notification tones when a prompt arrives. Not a full
// audio stack — no streaming, no mic capture, no codec. Just enough to
// beep.

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-time bring-up: power rail, I2C for ES8311 control, I2S for data,
// amplifier shutdown release, codec register init. Returns ESP_OK on
// success. If init fails (missing hardware, bad pins, etc.) the firmware
// still runs — speaker calls turn into no-ops.
esp_err_t speaker_init(void);

// Block for `duration_ms` playing a square wave at `freq_hz`. Amplitude
// is fixed at a safe level to avoid clipping the amp. Returns immediately
// if speaker_init failed.
void speaker_tone(uint16_t freq_hz, uint16_t duration_ms);

// Convenience: a 3-note ascending chime for "prompt arrived".
void speaker_ding(void);

// Convenience: a descending two-tone for "denied".
void speaker_deny(void);

// Convenience: a short up-chord for "approved".
void speaker_allow(void);

#ifdef __cplusplus
}
#endif
