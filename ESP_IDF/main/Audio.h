#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include "esp_err.h"

#define AUDIO_PIN_ENABLE   4             // Low = amplifier enabled
#define AUDIO_PIN_DAC      26            // DAC output pin

#define ALARM_FILE_PATH       "/sdcard/alarm.wav"

//Volume: Min = 0 / db = 0, Max = 2048 / db = 90
#define AUDIO_GAIN_Q8_MIN             0
#define AUDIO_GAIN_Q8_MAX             2048
extern volatile int16_t AUDIO_GAIN_Q8;

#define PLAYBACK_TIMING_RATE_HZ 24000UL //Value will change as per audio file. Your manual timing correction.

#define BPF_HP_ALPHA_Q15    28000     // high-pass around low hundreds of Hz
#define BPF_LP_ALPHA_Q15    16000     // low-pass around few kHz

#define AUDIO_AMP_ENABLE_LEVEL        0
#define AUDIO_AMP_DISABLE_LEVEL       1

/*
 * Q15 band-pass filter defaults.
 * Q15 value = alpha * 32768
 *
 * HP alpha 29838 = 0.9106
 * LP alpha 18975 = 0.5792
 */
#define AUDIO_BPF_HP_ALPHA_Q15        28000
#define AUDIO_BPF_LP_ALPHA_Q15        16000
#define AUDIO_Q15_ONE                 32768

#define AUDIO_DEFAULT_VOLUME_PERCENT  60

esp_err_t audio_init(void);
esp_err_t audio_deinit(void);

esp_err_t audio_amp_enable(int enable);

void audio_set_volume_percent(uint8_t volume_percent);
uint8_t audio_get_volume_percent(void);

void audio_set_bandpass_enabled(int enabled);
int audio_get_bandpass_enabled(void);

void audio_set_bandpass_alpha_q15(int32_t hp_alpha_q15, int32_t lp_alpha_q15);
void audio_get_bandpass_alpha_q15(int32_t *hp_alpha_q15, int32_t *lp_alpha_q15);
void audio_reset_filter(void);

esp_err_t audio_play_wav_file(const char *path);
void audio_stop(void);
int audio_is_playing(void);

#endif