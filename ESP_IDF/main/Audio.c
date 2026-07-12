#include "Audio.h"
#include "SDCard.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/dac_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

static const char *TAG = "AudioPlayer";

#define AUDIO_READ_BUFFER_SIZE 1024

#define AUDIO_DAC_CHANNEL DAC_CHAN_1

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint32_t data_size;
    long data_offset;
} wav_info_t;

typedef struct {
    int32_t hp_prev_x;
    int32_t hp_prev_y;
    int32_t lp_prev_y;
} audio_filter_state_t;

static dac_oneshot_handle_t s_dac_handle = NULL;

static int s_audio_initialized = 0;
static volatile int s_audio_playing = 0;
static volatile int s_stop_requested = 0;

/*
 * This is your runtime gain variable.
 *
 * You asked for:
 *
 * int AUDIO_GAIN_Q8 = 1024;
 *
 * I am using volatile int32_t instead of plain int because:
 *
 * 1. int32_t gives fixed 32-bit size.
 * 2. volatile tells the compiler this value can change while audio is playing.
 *
 * Example:
 *
 * AUDIO_GAIN_Q8 = 512;
 * AUDIO_GAIN_Q8 = 1024;
 */
volatile int16_t AUDIO_GAIN_Q8 = 1024;

static int s_bandpass_enabled = 1;

static int32_t s_hp_alpha_q15 = BPF_HP_ALPHA_Q15;
static int32_t s_lp_alpha_q15 = BPF_LP_ALPHA_Q15;

static audio_filter_state_t s_filter = {0};

static uint8_t s_read_buffer[AUDIO_READ_BUFFER_SIZE];

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static int32_t clamp_s16(int32_t value)
{
    if (value > 32767) {
        return 32767;
    }

    if (value < -32768) {
        return -32768;
    }

    return value;
}

static uint8_t sample_s16_to_dac_u8(int32_t sample)
{
    int32_t value;

    sample = clamp_s16(sample);

    /*
     * Convert signed 16-bit audio:
     *
     * -32768 to +32767
     *
     * into unsigned 8-bit DAC:
     *
     * 0 to 255
     */
    value = (sample >> 8) + 128;

    if (value < 0) {
        value = 0;
    }

    if (value > 255) {
        value = 255;
    }

    return (uint8_t)value;
}

static int32_t apply_gain_s16(int32_t sample)
{
    int32_t gain_q8;

    /*
     * Copy the volatile global into a local variable.
     * This avoids reading the variable multiple times while doing math.
     */
    gain_q8 = AUDIO_GAIN_Q8;

    /*
     * Safety clamp.
     * Even if another task writes a bad value, playback stays safe.
     */
    if (gain_q8 < AUDIO_GAIN_Q8_MIN) {
        gain_q8 = AUDIO_GAIN_Q8_MIN;
    }

    if (gain_q8 > AUDIO_GAIN_Q8_MAX) {
        gain_q8 = AUDIO_GAIN_Q8_MAX;
    }

    /*
     * Q8 gain formula:
     *
     * output = sample * gain_q8 / 256
     *
     * Since 256 = 2^8, divide using >> 8.
     */
    sample = (int32_t)(((int64_t)sample * gain_q8) >> 15);

    return clamp_s16(sample);
}

static int32_t apply_bandpass_s16(int32_t x)
{
    int32_t hp;
    int32_t lp;

    if (!s_bandpass_enabled) {
        return x;
    }

    /*
     * First-order high-pass filter:
     *
     * hp[n] = alpha * (hp[n-1] + x[n] - x[n-1])
     *
     * This removes low rumble / DC offset.
     */
    hp = (int32_t)(((int64_t)s_hp_alpha_q15 *
                    (s_filter.hp_prev_y + x - s_filter.hp_prev_x)) >> 15);

    s_filter.hp_prev_x = x;
    s_filter.hp_prev_y = hp;

    /*
     * First-order low-pass filter:
     *
     * lp[n] = lp[n-1] + alpha * (hp[n] - lp[n-1])
     *
     * This softens high-frequency harshness.
     */
    lp = s_filter.lp_prev_y +
         (int32_t)(((int64_t)s_lp_alpha_q15 *
                    (hp - s_filter.lp_prev_y)) >> 15);

    s_filter.lp_prev_y = lp;

    return clamp_s16(lp);
}

static void audio_output_center(void)
{
    if (s_dac_handle != NULL) {
        dac_oneshot_output_voltage(s_dac_handle, 128);
    }
}

static esp_err_t audio_parse_wav(FILE *file, wav_info_t *info)
{
    uint8_t header[12];
    int fmt_found;
    int data_found;

    if (file == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(wav_info_t));

    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to read WAV header");
        return ESP_FAIL;
    }

    if (memcmp(&header[0], "RIFF", 4) != 0 ||
        memcmp(&header[8], "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a valid RIFF/WAVE file");
        return ESP_ERR_INVALID_ARG;
    }

    fmt_found = 0;
    data_found = 0;

    while (!feof(file)) {
        uint8_t chunk_header[8];
        uint32_t chunk_size;
        long chunk_data_start;
        long skip_size;

        if (fread(chunk_header, 1, sizeof(chunk_header), file) != sizeof(chunk_header)) {
            break;
        }

        chunk_size = read_le32(&chunk_header[4]);
        chunk_data_start = ftell(file);

        if (memcmp(&chunk_header[0], "fmt ", 4) == 0) {
            uint8_t fmt[16];

            if (chunk_size < 16) {
                ESP_LOGE(TAG, "Invalid WAV fmt chunk");
                return ESP_ERR_INVALID_ARG;
            }

            if (fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) {
                ESP_LOGE(TAG, "Failed to read WAV fmt chunk");
                return ESP_FAIL;
            }

            info->audio_format = read_le16(&fmt[0]);
            info->num_channels = read_le16(&fmt[2]);
            info->sample_rate = read_le32(&fmt[4]);
            info->byte_rate = read_le32(&fmt[8]);
            info->block_align = read_le16(&fmt[12]);
            info->bits_per_sample = read_le16(&fmt[14]);

            fmt_found = 1;
        } else if (memcmp(&chunk_header[0], "data", 4) == 0) {
            info->data_offset = ftell(file);
            info->data_size = chunk_size;
            data_found = 1;
        }

        skip_size = (long)chunk_size;

        if (skip_size & 1) {
            skip_size++;
        }

        if (fseek(file, chunk_data_start + skip_size, SEEK_SET) != 0) {
            ESP_LOGE(TAG, "Failed to skip WAV chunk");
            return ESP_FAIL;
        }

        if (fmt_found && data_found) {
            break;
        }
    }

    if (!fmt_found) {
        ESP_LOGE(TAG, "WAV fmt chunk not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (!data_found) {
        ESP_LOGE(TAG, "WAV data chunk not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (info->audio_format != 1) {
        ESP_LOGE(TAG, "Unsupported WAV format: %u. Only PCM is supported", info->audio_format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->num_channels != 1 && info->num_channels != 2) {
        ESP_LOGE(TAG, "Unsupported channel count: %u", info->num_channels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->bits_per_sample != 8 && info->bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported bits per sample: %u", info->bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->sample_rate == 0) {
        ESP_LOGE(TAG, "Invalid sample rate");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(
        TAG,
        "WAV: PCM, channels=%u, sample_rate=%lu Hz, bits=%u, data=%lu bytes",
        info->num_channels,
        (unsigned long)info->sample_rate,
        info->bits_per_sample,
        (unsigned long)info->data_size
    );

    return ESP_OK;
}

static int32_t audio_frame_to_mono_s16(const uint8_t *frame,
                                       uint16_t num_channels,
                                       uint16_t bits_per_sample)
{
    int32_t sum;
    uint16_t ch;

    sum = 0;

    if (bits_per_sample == 8) {
        /*
         * 8-bit PCM WAV is unsigned:
         *
         * 0   = minimum
         * 128 = center
         * 255 = maximum
         */
        for (ch = 0; ch < num_channels; ch++) {
            int32_t s;

            s = ((int32_t)frame[ch] - 128) << 8;
            sum += s;
        }
    } else {
        /*
         * 16-bit PCM WAV is signed little-endian.
         */
        for (ch = 0; ch < num_channels; ch++) {
            uint16_t index;
            int16_t s;

            index = ch * 2;
            s = (int16_t)((uint16_t)frame[index] |
                          ((uint16_t)frame[index + 1] << 8));

            sum += (int32_t)s;
        }
    }

    sum = sum / (int32_t)num_channels;

    return clamp_s16(sum);
}

esp_err_t audio_init(void)
{
    esp_err_t ret;
    gpio_config_t gpio_conf;
    dac_oneshot_config_t dac_conf = {0};

    if (s_audio_initialized) {
        return ESP_OK;
    }

    gpio_conf.pin_bit_mask = (1ULL << AUDIO_PIN_ENABLE);
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.intr_type = GPIO_INTR_DISABLE;

    ret = gpio_config(&gpio_conf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio enable GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_DISABLE_LEVEL);

    dac_conf.chan_id = AUDIO_DAC_CHANNEL;

    ret = dac_oneshot_new_channel(&dac_conf, &s_dac_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DAC init failed: %s", esp_err_to_name(ret));
        s_dac_handle = NULL;
        return ret;
    }

    audio_output_center();
    audio_reset_filter();

    s_audio_initialized = 1;

    ESP_LOGI(
        TAG,
        "Audio initialized: DAC GPIO%d, AMP_ENABLE GPIO%d",
        AUDIO_PIN_DAC,
        AUDIO_PIN_ENABLE
    );

    return ESP_OK;
}

esp_err_t audio_deinit(void)
{
    if (!s_audio_initialized) {
        return ESP_OK;
    }

    audio_stop();
    audio_amp_enable(0);
    audio_output_center();

    if (s_dac_handle != NULL) {
        dac_oneshot_del_channel(s_dac_handle);
        s_dac_handle = NULL;
    }

    s_audio_initialized = 0;

    return ESP_OK;
}

esp_err_t audio_amp_enable(int enable)
{
    if (!s_audio_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (enable) {
        gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_ENABLE_LEVEL);

        /*
         * FM8002E wake-up is not instant.
         * This delay helps reduce pop/click noise.
         */
        esp_rom_delay_us(100000);
    } else {
        gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_DISABLE_LEVEL);
    }

    return ESP_OK;
}

void audio_set_gain_q8(int32_t gain_q8)
{
    if (gain_q8 < AUDIO_GAIN_Q8_MIN) {
        gain_q8 = AUDIO_GAIN_Q8_MIN;
    }

    if (gain_q8 > AUDIO_GAIN_Q8_MAX) {
        gain_q8 = AUDIO_GAIN_Q8_MAX;
    }

    AUDIO_GAIN_Q8 = gain_q8;
}

int32_t audio_get_gain_q8(void)
{
    return AUDIO_GAIN_Q8;
}

void audio_set_bandpass_enabled(int enabled)
{
    s_bandpass_enabled = enabled ? 1 : 0;
    audio_reset_filter();
}

int audio_get_bandpass_enabled(void)
{
    return s_bandpass_enabled;
}

void audio_set_bandpass_alpha_q15(int32_t hp_alpha_q15, int32_t lp_alpha_q15)
{
    if (hp_alpha_q15 < 0) {
        hp_alpha_q15 = 0;
    }

    if (hp_alpha_q15 > 32767) {
        hp_alpha_q15 = 32767;
    }

    if (lp_alpha_q15 < 0) {
        lp_alpha_q15 = 0;
    }

    if (lp_alpha_q15 > 32767) {
        lp_alpha_q15 = 32767;
    }

    s_hp_alpha_q15 = hp_alpha_q15;
    s_lp_alpha_q15 = lp_alpha_q15;

    audio_reset_filter();
}

void audio_get_bandpass_alpha_q15(int32_t *hp_alpha_q15, int32_t *lp_alpha_q15)
{
    if (hp_alpha_q15 != NULL) {
        *hp_alpha_q15 = s_hp_alpha_q15;
    }

    if (lp_alpha_q15 != NULL) {
        *lp_alpha_q15 = s_lp_alpha_q15;
    }
}

void audio_reset_filter(void)
{
    memset(&s_filter, 0, sizeof(s_filter));
}

void audio_stop(void)
{
    s_stop_requested = 1;
}

int audio_is_playing(void)
{
    return s_audio_playing;
}

esp_err_t audio_play_wav_file(const char *path)
{
    char full_path[256];
    FILE *file;
    wav_info_t wav;
    esp_err_t ret;

    uint32_t bytes_per_sample;
    uint32_t frame_size;
    uint32_t data_remaining;
    uint32_t max_aligned_read;

    int64_t period_q16;
    int64_t next_time_q16;

    uint32_t frame_counter;

    if (!s_audio_initialized) {
        ret = audio_init();

        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (!sdcard_is_mounted()) {
        ESP_LOGE(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    ret = sdcard_make_path(path, full_path, sizeof(full_path));

    if (ret != ESP_OK) {
        return ret;
    }

    file = fopen(full_path, "rb");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", full_path);
        return ESP_ERR_NOT_FOUND;
    }

    ret = audio_parse_wav(file, &wav);

    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    bytes_per_sample = wav.bits_per_sample / 8;
    frame_size = (uint32_t)wav.num_channels * bytes_per_sample;

    if (frame_size == 0 || frame_size > AUDIO_READ_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Invalid WAV frame size");
        fclose(file);
        return ESP_ERR_INVALID_ARG;
    }

    max_aligned_read = AUDIO_READ_BUFFER_SIZE - (AUDIO_READ_BUFFER_SIZE % frame_size);

    if (max_aligned_read == 0) {
        ESP_LOGE(TAG, "Audio read buffer is too small");
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    if (fseek(file, wav.data_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to WAV data");
        fclose(file);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Playing: %s", full_path);
    ESP_LOGI(
        TAG,
        "Gain_Q8=%ld, BPF=%s, HP_Q15=%ld, LP_Q15=%ld",
        (long)AUDIO_GAIN_Q8,
        s_bandpass_enabled ? "ON" : "OFF",
        (long)s_hp_alpha_q15,
        (long)s_lp_alpha_q15
    );

    s_stop_requested = 0;
    s_audio_playing = 1;

    audio_reset_filter();
    audio_output_center();
    audio_amp_enable(1);

    data_remaining = wav.data_size;

    /*
     * Sample timing:
     *
     * period = 1 second / sample_rate
     *
     * Stored in Q16 microseconds to reduce timing drift.
     */
    period_q16 = ((int64_t)1000000 << 16) / (int64_t)wav.sample_rate;
    next_time_q16 = esp_timer_get_time() << 16;

    frame_counter = 0;

    while (data_remaining > 0 && !s_stop_requested) {
        uint32_t to_read;
        size_t bytes_read;
        size_t usable_bytes;
        size_t offset;

        to_read = data_remaining;

        if (to_read > max_aligned_read) {
            to_read = max_aligned_read;
        }

        to_read = to_read - (to_read % frame_size);

        if (to_read == 0) {
            break;
        }

        bytes_read = fread(s_read_buffer, 1, to_read, file);

        if (bytes_read == 0) {
            break;
        }

        usable_bytes = bytes_read - (bytes_read % frame_size);
        data_remaining -= (uint32_t)usable_bytes;

        offset = 0;

        while (offset + frame_size <= usable_bytes && !s_stop_requested) {
            int32_t sample;
            uint8_t dac_value;
            int64_t target_us;
            int64_t delay_us;

            sample = audio_frame_to_mono_s16(
                &s_read_buffer[offset],
                wav.num_channels,
                wav.bits_per_sample
            );

            sample = apply_bandpass_s16(sample);
            sample = apply_gain_s16(sample);

            dac_value = sample_s16_to_dac_u8(sample);

            dac_oneshot_output_voltage(s_dac_handle, dac_value);

            next_time_q16 += period_q16;
            target_us = next_time_q16 >> 16;
            delay_us = target_us - esp_timer_get_time();

            if (delay_us > 0) {
                esp_rom_delay_us((uint32_t)delay_us);
            } else if (delay_us < -5000) {
                /*
                 * If we fall too far behind, reset timing.
                 */
                next_time_q16 = esp_timer_get_time() << 16;
            }

            offset += frame_size;
            frame_counter++;
        }
    }

    audio_output_center();
    audio_amp_enable(0);

    fclose(file);

    s_audio_playing = 0;
    s_stop_requested = 0;

    ESP_LOGI(TAG, "Playback finished, frames=%lu", (unsigned long)frame_counter);

    return ESP_OK;
}