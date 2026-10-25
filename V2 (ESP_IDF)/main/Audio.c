#include "Audio.h"
#include "SDCard.h"

#include <stdio.h>
#include <string.h>

#include "driver/dac_oneshot.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "AudioPlayer";

#define AUDIO_READ_BUFFER_SIZE           1024U
#define AUDIO_DAC_CHANNEL                DAC_CHAN_1
#define AUDIO_DAC_CENTER_VALUE           128U
#define AUDIO_TIMING_RESET_THRESHOLD_US  5000LL

typedef struct
{
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint32_t data_size;
    long data_offset;
} wav_info_t;

typedef struct
{
    int32_t hp_prev_x;
    int32_t hp_prev_y;
    int32_t lp_prev_y;
} audio_filter_state_t;

/*
 * Public runtime configuration.
 *
 * Start at unity gain to avoid clipping. Increase AUDIO_GAIN_Q8 only after
 * confirming that the WAV file plays cleanly.
 */
volatile int16_t AUDIO_GAIN_Q8 = AUDIO_GAIN_Q8_DEFAULT;
volatile uint8_t g_audio_volume_percent = 50U;
volatile uint32_t AUDIO_PLAYBACK_RATE_HZ = 16000U;

static dac_oneshot_handle_t s_dac_handle = NULL;

static int s_audio_initialized = 0;
static volatile int s_audio_playing = 0;
static volatile int s_stop_requested = 0;

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
    return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int32_t clamp_s16(int32_t value)
{
    if (value > 32767)
    {
        return 32767;
    }

    if (value < -32768)
    {
        return -32768;
    }

    return value;
}

static uint8_t sample_s16_to_dac_u8(int32_t sample)
{
    int32_t value;

    sample = clamp_s16(sample);
    value = (sample >> 8) + AUDIO_DAC_CENTER_VALUE;

    if (value < 0)
    {
        value = 0;
    }

    if (value > 255)
    {
        value = 255;
    }

    return (uint8_t)value;
}

static int32_t apply_gain_s16(int32_t sample)
{
    int32_t gain_q8 = AUDIO_GAIN_Q8;

    if (gain_q8 < AUDIO_GAIN_Q8_MIN)
    {
        gain_q8 = AUDIO_GAIN_Q8_MIN;
    }

    if (gain_q8 > AUDIO_GAIN_Q8_MAX)
    {
        gain_q8 = AUDIO_GAIN_Q8_MAX;
    }

    /*
     * Q8 gain:
     *
     * output = sample * gain / 256
     *
     * The previous code used >> 15 here. That made the signal extremely low
     * because the value is Q8, not Q15.
     */
    sample = (int32_t)(((int64_t)sample * gain_q8) >> 8);

    return clamp_s16(sample);
}

static int32_t apply_bandpass_s16(int32_t x)
{
    int32_t hp;
    int32_t lp;

    if (!s_bandpass_enabled)
    {
        return x;
    }

    hp = (int32_t)(((int64_t)s_hp_alpha_q15 * (s_filter.hp_prev_y + x - s_filter.hp_prev_x)) >> 15);

    s_filter.hp_prev_x = x;
    s_filter.hp_prev_y = hp;

    lp = s_filter.lp_prev_y +
         (int32_t)(((int64_t)s_lp_alpha_q15 * (hp - s_filter.lp_prev_y)) >> 15);

    s_filter.lp_prev_y = lp;

    return clamp_s16(lp);
}

static void audio_output_center(void)
{
    if (s_dac_handle != NULL)
    {
        dac_oneshot_output_voltage(s_dac_handle, AUDIO_DAC_CENTER_VALUE);
    }
}

static esp_err_t audio_parse_wav(FILE *file, wav_info_t *info)
{
    uint8_t header[12];
    int fmt_found = 0;
    int data_found = 0;

    if (file == NULL || info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    if (fread(header, 1, sizeof(header), file) != sizeof(header))
    {
        ESP_LOGE(TAG, "Failed to read WAV header");
        return ESP_FAIL;
    }

    if (memcmp(&header[0], "RIFF", 4) != 0 || memcmp(&header[8], "WAVE", 4) != 0)
    {
        ESP_LOGE(TAG, "Not a valid RIFF/WAVE file");
        return ESP_ERR_INVALID_ARG;
    }

    while (!feof(file))
    {
        uint8_t chunk_header[8];
        uint32_t chunk_size;
        long chunk_data_start;
        long skip_size;

        if (fread(chunk_header, 1, sizeof(chunk_header), file) != sizeof(chunk_header))
        {
            break;
        }

        chunk_size = read_le32(&chunk_header[4]);
        chunk_data_start = ftell(file);

        if (chunk_data_start < 0)
        {
            ESP_LOGE(TAG, "Could not read WAV chunk position");
            return ESP_FAIL;
        }

        if (memcmp(&chunk_header[0], "fmt ", 4) == 0)
        {
            uint8_t fmt[16];

            if (chunk_size < sizeof(fmt))
            {
                ESP_LOGE(TAG, "Invalid WAV fmt chunk");
                return ESP_ERR_INVALID_ARG;
            }

            if (fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt))
            {
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
        }
        else if (memcmp(&chunk_header[0], "data", 4) == 0)
        {
            info->data_offset = ftell(file);

            if (info->data_offset < 0)
            {
                ESP_LOGE(TAG, "Could not read WAV data position");
                return ESP_FAIL;
            }

            info->data_size = chunk_size;
            data_found = 1;
        }

        skip_size = (long)chunk_size;

        if ((skip_size & 1L) != 0L)
        {
            skip_size++;
        }

        if (fseek(file, chunk_data_start + skip_size, SEEK_SET) != 0)
        {
            ESP_LOGE(TAG, "Failed to skip WAV chunk");
            return ESP_FAIL;
        }

        if (fmt_found && data_found)
        {
            break;
        }
    }

    if (!fmt_found)
    {
        ESP_LOGE(TAG, "WAV fmt chunk not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (!data_found)
    {
        ESP_LOGE(TAG, "WAV data chunk not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (info->audio_format != 1U)
    {
        ESP_LOGE(TAG, "Unsupported WAV format: %u. Only PCM is supported", info->audio_format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->num_channels != 1U && info->num_channels != 2U)
    {
        ESP_LOGE(TAG, "Unsupported channel count: %u", info->num_channels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->bits_per_sample != 8U && info->bits_per_sample != 16U)
    {
        ESP_LOGE(TAG, "Unsupported bits per sample: %u", info->bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->sample_rate == 0U)
    {
        ESP_LOGE(TAG, "Invalid WAV sample rate");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG,
             "WAV: PCM, channels=%u, sample_rate=%lu Hz, bits=%u, data=%lu bytes",
             info->num_channels,
             (unsigned long)info->sample_rate,
             info->bits_per_sample,
             (unsigned long)info->data_size);

    return ESP_OK;
}

static int32_t audio_frame_to_mono_s16(const uint8_t *frame,
                                       uint16_t num_channels,
                                       uint16_t bits_per_sample)
{
    int32_t sum = 0;

    if (bits_per_sample == 8U)
    {
        for (uint16_t channel = 0; channel < num_channels; channel++)
        {
            sum += ((int32_t)frame[channel] - 128) << 8;
        }
    }
    else
    {
        for (uint16_t channel = 0; channel < num_channels; channel++)
        {
            uint16_t index = channel * 2U;
            int16_t sample = (int16_t)((uint16_t)frame[index] |
                                       ((uint16_t)frame[index + 1U] << 8));

            sum += sample;
        }
    }

    sum /= (int32_t)num_channels;

    return clamp_s16(sum);
}

esp_err_t audio_init(void)
{
    if (s_audio_initialized)
    {
        return ESP_OK;
    }

    gpio_config_t gpio_configuration =
    {
        .pin_bit_mask = 1ULL << AUDIO_PIN_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t result = gpio_config(&gpio_configuration);

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio enable GPIO config failed: %s", esp_err_to_name(result));
        return result;
    }

    gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_DISABLE_LEVEL);

    dac_oneshot_config_t dac_configuration =
    {
        .chan_id = AUDIO_DAC_CHANNEL
    };

    result = dac_oneshot_new_channel(&dac_configuration, &s_dac_handle);

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "DAC init failed: %s", esp_err_to_name(result));
        s_dac_handle = NULL;
        return result;
    }

    audio_output_center();
    audio_reset_filter();

    s_audio_initialized = 1;

    ESP_LOGI(TAG,
             "Audio initialized: DAC GPIO%d, AMP_ENABLE GPIO%d",
             AUDIO_PIN_DAC,
             AUDIO_PIN_ENABLE);

    return ESP_OK;
}

esp_err_t audio_deinit(void)
{
    if (!s_audio_initialized)
    {
        return ESP_OK;
    }

    audio_stop();
    audio_amp_enable(0);
    audio_output_center();

    if (s_dac_handle != NULL)
    {
        dac_oneshot_del_channel(s_dac_handle);
        s_dac_handle = NULL;
    }

    s_audio_initialized = 0;

    return ESP_OK;
}

esp_err_t audio_amp_enable(int enable)
{
    if (!s_audio_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (enable)
    {
        gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_ENABLE_LEVEL);
        esp_rom_delay_us(100000U);
    }
    else
    {
        gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_DISABLE_LEVEL);
    }

    return ESP_OK;
}

void audio_set_gain_q8(int32_t gain_q8)
{
    int32_t gain_range;
    int32_t volume_percent;

    if (gain_q8 < AUDIO_GAIN_Q8_MIN)
    {
        gain_q8 = AUDIO_GAIN_Q8_MIN;
    }

    if (gain_q8 > AUDIO_GAIN_Q8_MAX)
    {
        gain_q8 = AUDIO_GAIN_Q8_MAX;
    }

    AUDIO_GAIN_Q8 = (int16_t)gain_q8;

    gain_range = AUDIO_GAIN_Q8_MAX - AUDIO_GAIN_Q8_MIN;

    if (gain_range <= 0)
    {
        g_audio_volume_percent = 0U;
        return;
    }

    volume_percent =
        ((gain_q8 - AUDIO_GAIN_Q8_MIN) * 100 + gain_range / 2) /
        gain_range;

    if (volume_percent < 0)
    {
        volume_percent = 0;
    }
    else if (volume_percent > 100)
    {
        volume_percent = 100;
    }

    g_audio_volume_percent = (uint8_t)volume_percent;
}

int32_t audio_get_gain_q8(void)
{
    return AUDIO_GAIN_Q8;
}

void audio_set_volume_percent(uint8_t volume_percent)
{
    int32_t gain_range;
    int32_t gain_q8;

    if (volume_percent > 100U)
    {
        volume_percent = 100U;
    }

    gain_range = AUDIO_GAIN_Q8_MAX - AUDIO_GAIN_Q8_MIN;
    gain_q8 = AUDIO_GAIN_Q8_MIN +
              ((gain_range * (int32_t)volume_percent + 50) / 100);

    g_audio_volume_percent = volume_percent;
    AUDIO_GAIN_Q8 = (int16_t)gain_q8;
}

uint8_t audio_get_volume_percent(void)
{
    return g_audio_volume_percent;
}

void audio_set_playback_rate_hz(uint32_t playback_rate_hz)
{
    AUDIO_PLAYBACK_RATE_HZ = playback_rate_hz;
}

uint32_t audio_get_playback_rate_hz(void)
{
    return AUDIO_PLAYBACK_RATE_HZ;
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
    if (hp_alpha_q15 < 0)
    {
        hp_alpha_q15 = 0;
    }

    if (hp_alpha_q15 > 32767)
    {
        hp_alpha_q15 = 32767;
    }

    if (lp_alpha_q15 < 0)
    {
        lp_alpha_q15 = 0;
    }

    if (lp_alpha_q15 > 32767)
    {
        lp_alpha_q15 = 32767;
    }

    s_hp_alpha_q15 = hp_alpha_q15;
    s_lp_alpha_q15 = lp_alpha_q15;

    audio_reset_filter();
}

void audio_get_bandpass_alpha_q15(int32_t *hp_alpha_q15, int32_t *lp_alpha_q15)
{
    if (hp_alpha_q15 != NULL)
    {
        *hp_alpha_q15 = s_hp_alpha_q15;
    }

    if (lp_alpha_q15 != NULL)
    {
        *lp_alpha_q15 = s_lp_alpha_q15;
    }
}

void audio_reset_filter(void)
{
    memset(&s_filter, 0, sizeof(s_filter));
}

int32_t audio_process_sample_s16(int32_t sample)
{
    sample = apply_bandpass_s16(sample);
    sample = apply_gain_s16(sample);

    return sample;
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
    FILE *file = NULL;
    wav_info_t wav;
    esp_err_t result = ESP_OK;

    uint32_t bytes_per_sample;
    uint32_t frame_size;
    uint32_t data_remaining;
    uint32_t max_aligned_read;
    uint32_t playback_rate_hz;
    uint32_t frame_counter = 0;

    int64_t period_q16;
    int64_t next_time_q16;

    if (path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_audio_playing)
    {
        ESP_LOGW(TAG, "Audio is already playing");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_audio_initialized)
    {
        result = audio_init();

        if (result != ESP_OK)
        {
            return result;
        }
    }

    if (!sdcard_is_mounted())
    {
        ESP_LOGE(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    file = fopen(path, "rb");

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    result = audio_parse_wav(file, &wav);

    if (result != ESP_OK)
    {
        fclose(file);
        return result;
    }

    bytes_per_sample = wav.bits_per_sample / 8U;
    frame_size = (uint32_t)wav.num_channels * bytes_per_sample;

    if (frame_size == 0U || frame_size > AUDIO_READ_BUFFER_SIZE)
    {
        ESP_LOGE(TAG, "Invalid WAV frame size");
        fclose(file);
        return ESP_ERR_INVALID_ARG;
    }

    max_aligned_read = AUDIO_READ_BUFFER_SIZE - (AUDIO_READ_BUFFER_SIZE % frame_size);

    if (max_aligned_read == 0U)
    {
        ESP_LOGE(TAG, "Audio read buffer is too small");
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    if (fseek(file, wav.data_offset, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Failed to seek to WAV data");
        fclose(file);
        return ESP_FAIL;
    }

    playback_rate_hz = AUDIO_PLAYBACK_RATE_HZ;

    if (playback_rate_hz == 0U)
    {
        playback_rate_hz = wav.sample_rate;
    }

    if (playback_rate_hz == 0U)
    {
        fclose(file);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Playing: %s", path);
    ESP_LOGI(TAG,
             "Header rate=%lu Hz, playback rate=%lu Hz, Gain_Q8=%ld, BPF=%s",
             (unsigned long)wav.sample_rate,
             (unsigned long)playback_rate_hz,
             (long)AUDIO_GAIN_Q8,
             s_bandpass_enabled ? "ON" : "OFF");

    s_stop_requested = 0;
    s_audio_playing = 1;

    audio_reset_filter();
    audio_output_center();

    result = audio_amp_enable(1);

    if (result != ESP_OK)
    {
        s_audio_playing = 0;
        fclose(file);
        return result;
    }

    data_remaining = wav.data_size;

    period_q16 = ((int64_t)1000000 << 16) / (int64_t)playback_rate_hz;
    next_time_q16 = esp_timer_get_time() << 16;

    while (data_remaining > 0U && !s_stop_requested)
    {
        uint32_t to_read = data_remaining;

        if (to_read > max_aligned_read)
        {
            to_read = max_aligned_read;
        }

        to_read -= to_read % frame_size;

        if (to_read == 0U)
        {
            break;
        }

        size_t bytes_read = fread(s_read_buffer, 1, to_read, file);

        if (bytes_read == 0U)
        {
            if (ferror(file))
            {
                ESP_LOGE(TAG, "WAV read failed");
                result = ESP_FAIL;
            }

            break;
        }

        size_t usable_bytes = bytes_read - (bytes_read % frame_size);
        data_remaining -= (uint32_t)usable_bytes;

        for (size_t offset = 0;
             offset + frame_size <= usable_bytes && !s_stop_requested;
             offset += frame_size)
        {
            int32_t sample = audio_frame_to_mono_s16(&s_read_buffer[offset],
                                                     wav.num_channels,
                                                     wav.bits_per_sample);

            sample = audio_process_sample_s16(sample);

            uint8_t dac_value = sample_s16_to_dac_u8(sample);
            dac_oneshot_output_voltage(s_dac_handle, dac_value);

            next_time_q16 += period_q16;

            int64_t target_us = next_time_q16 >> 16;
            int64_t delay_us = target_us - esp_timer_get_time();

            if (delay_us > 0)
            {
                esp_rom_delay_us((uint32_t)delay_us);
            }
            else if (delay_us < -AUDIO_TIMING_RESET_THRESHOLD_US)
            {
                next_time_q16 = esp_timer_get_time() << 16;
            }

            frame_counter++;
        }
    }

    audio_output_center();
    audio_amp_enable(0);

    fclose(file);

    s_audio_playing = 0;
    s_stop_requested = 0;

    ESP_LOGI(TAG, "Playback finished, frames=%lu", (unsigned long)frame_counter);

    return result;
}