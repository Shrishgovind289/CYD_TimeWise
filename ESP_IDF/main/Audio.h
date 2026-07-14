#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#include "esp_err.h"

#define AUDIO_PIN_ENABLE                 4
#define AUDIO_PIN_DAC                    26

#define ALARM_FILE_PATH                  "/sdcard/alarm.wav"

/*
 * Q8 gain values:
 *
 *   0    = mute
 *   256  = 1.0x
 *   512  = 2.0x
 *   1024 = 4.0x
 *   2048 = 8.0x
 */
#define AUDIO_GAIN_Q8_MIN                0
#define AUDIO_GAIN_Q8_UNITY              1024
#define AUDIO_GAIN_Q8_MAX                2048

/*
 * Runtime audio configuration.
 *
 * AUDIO_PLAYBACK_RATE_HZ = 0 uses the sample rate stored in the WAV header.
 * A value such as 24000 forces playback at 24 kHz.
 */
extern volatile int16_t AUDIO_GAIN_Q8;
extern volatile uint32_t AUDIO_PLAYBACK_RATE_HZ;

#define BPF_HP_ALPHA_Q15                 28000
#define BPF_LP_ALPHA_Q15                 16000

#define AUDIO_AMP_ENABLE_LEVEL           0
#define AUDIO_AMP_DISABLE_LEVEL          1

esp_err_t audio_init(void);
esp_err_t audio_deinit(void);

esp_err_t audio_amp_enable(int enable);

void audio_set_gain_q8(int32_t gain_q8);
int32_t audio_get_gain_q8(void);

void audio_set_playback_rate_hz(uint32_t playback_rate_hz);
uint32_t audio_get_playback_rate_hz(void);

void audio_set_bandpass_enabled(int enabled);
int audio_get_bandpass_enabled(void);

void audio_set_bandpass_alpha_q15(int32_t hp_alpha_q15, int32_t lp_alpha_q15);
void audio_get_bandpass_alpha_q15(int32_t *hp_alpha_q15, int32_t *lp_alpha_q15);
void audio_reset_filter(void);

esp_err_t audio_play_wav_file(const char *path);
void audio_stop(void);
int audio_is_playing(void);

#endif /* AUDIO_H */