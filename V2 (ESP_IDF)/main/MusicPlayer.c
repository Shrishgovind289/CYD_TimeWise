#include "MusicPlayer.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_DAC_DMA_AUTO_16BIT_ALIGN)
#error "TimeWise MusicPlayer requires CONFIG_DAC_DMA_AUTO_16BIT_ALIGN=y on ESP32"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/dac_continuous.h"
#include "driver/gpio.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"

#include "mbedtls/md5.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#include "Audio.h"
#include "NavidromeClient.h"

static const char *TAG = "MusicPlayer";

/* -------------------------------------------------------------------------- */
/*                           Player configuration                             */
/* -------------------------------------------------------------------------- */

#define MUSIC_PLAYER_TASK_STACK_SIZE          6144U
#define MUSIC_PLAYER_TASK_PRIORITY            4U

#if defined(CONFIG_FREERTOS_UNICORE)
#define MUSIC_PLAYER_TASK_CORE                0
#else
#define MUSIC_PLAYER_TASK_CORE                1
#endif

#define MUSIC_HTTP_TIMEOUT_MS                 5000

#define MUSIC_STREAM_URL_SIZE                 1024U

/*
 * Small MP3 input blocks.
 *
 * The Espressif Simple Decoder internally keeps incomplete MP3 frame data.
 */
#define MUSIC_STREAM_INPUT_BUFFER_SIZE        512U
#define MUSIC_MP3_PROBE_MAX_BYTES              (64U * 1024U)

/*
 * Maximum decoded PCM size for a normal stereo MP3 frame:
 *
 * 1152 samples x 2 channels x 2 bytes = 4608 bytes.
 */
#define MUSIC_DECODER_OUTPUT_SIZE             4608U

/*
 * Yield only occasionally after successfully decoded audio frames.
 *
 * This prevents IDLE0 watchdog starvation without inserting frequent
 * interruptions into the audio stream.
 */
#define MUSIC_DECODED_FRAMES_BEFORE_YIELD     128U

/*
 * Exclusive music mode allows a larger DMA queue.
 * Four 512-byte descriptors provide a much larger audio cushion than the
 * previous 2 x 256-byte setup that produced severe breakup.
 */
#define MUSIC_DAC_DMA_DESCRIPTOR_COUNT        4U
#define MUSIC_DAC_DMA_BUFFER_SIZE             512U

/*
 * Do not give dac_continuous_write() an arbitrarily large block with an
 * infinite timeout. On ESP32 the driver expands each 8-bit sample to a 16-bit
 * DMA slot, so a full 1152-sample MP3 frame can exceed the instantaneous DMA
 * queue capacity.
 *
 * Small bounded writes keep the DMA moving and, critically, allow a Stop /
 * Music Enable=0 request to be observed even if the DAC stops accepting data.
 */
#define MUSIC_DAC_WRITE_CHUNK_SIZE            256U
#define MUSIC_DAC_WRITE_TIMEOUT_MS            250
#define MUSIC_DAC_MAX_CONSECUTIVE_TIMEOUTS    8U

#define MUSIC_DAC_CENTER_VALUE                128U

#define MUSIC_AMP_WAKE_DELAY_US               100000U

#define MUSIC_NAVIDROME_API_VERSION           "1.16.1"
#define MUSIC_NAVIDROME_CLIENT_NAME           "TimeWise"

#define MUSIC_NAVIDROME_SALT_LENGTH           16U
#define MUSIC_NAVIDROME_TOKEN_LENGTH          32U

/* -------------------------------------------------------------------------- */
/*                               Public state                                 */
/* -------------------------------------------------------------------------- */

volatile bool g_music_player_playing = false;
volatile bool g_music_player_stop_requested = false;
volatile bool g_music_player_exclusive_mode = false;
volatile bool g_music_player_paused = false;

volatile uint16_t g_music_player_max_bitrate_kbps = 128U;
volatile uint32_t g_music_player_position_seconds = 0U;
volatile music_player_state_t g_music_player_state = MUSIC_PLAYER_STATE_STOPPED;

navidrome_song_t g_music_player_song = {0};

/* -------------------------------------------------------------------------- */
/*                               Private state                                */
/* -------------------------------------------------------------------------- */

static TaskHandle_t s_music_player_task = NULL;

static dac_continuous_handle_t s_music_dac_handle = NULL;

static uint32_t s_music_dac_sample_rate = 0U;
static uint64_t s_music_output_samples = 0U;

/*
 * True while Audio.c has released its normal one-shot DAC.
 */
static bool s_alarm_audio_driver_released = false;

/* -------------------------------------------------------------------------- */
/*                              Heap diagnostics                              */
/* -------------------------------------------------------------------------- */

static void music_player_log_heap(const char *label)
{
    uint32_t free_8bit =
        heap_caps_get_free_size(MALLOC_CAP_8BIT);

    uint32_t largest_8bit =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    uint32_t free_internal =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    uint32_t largest_internal =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    ESP_LOGI(
        TAG,
        "%s: free8=%lu largest8=%lu freeInternal=%lu largestInternal=%lu",
        label != NULL ? label : "Heap",
        (unsigned long)free_8bit,
        (unsigned long)largest_8bit,
        (unsigned long)free_internal,
        (unsigned long)largest_internal
    );
}

/* -------------------------------------------------------------------------- */
/*                               PCM helpers                                  */
/* -------------------------------------------------------------------------- */

static int32_t music_player_clamp_s16(int32_t value)
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

static uint8_t music_player_sample_to_dac(int32_t sample)
{
    sample = music_player_clamp_s16(sample);

    int32_t value =
        (sample >> 8) +
        MUSIC_DAC_CENTER_VALUE;

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

static int16_t music_player_read_s16_le(const uint8_t *data)
{
    uint16_t value =
        (uint16_t)data[0] |
        ((uint16_t)data[1] << 8U);

    return (int16_t)value;
}

/* -------------------------------------------------------------------------- */
/*                            URL encoding                                    */
/* -------------------------------------------------------------------------- */

static bool music_player_url_character_is_safe(char character)
{
    if (isalnum((unsigned char)character))
    {
        return true;
    }

    switch (character)
    {
        case '-':
        case '_':
        case '.':
        case '~':
            return true;

        default:
            return false;
    }
}

static esp_err_t music_player_url_encode(const char *source,
                                         char *destination,
                                         size_t destination_size)
{
    static const char hex[] = "0123456789ABCDEF";

    if (source == NULL ||
        destination == NULL ||
        destination_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t destination_index = 0U;

    for (size_t source_index = 0U;
         source[source_index] != '\0';
         source_index++)
    {
        unsigned char character =
            (unsigned char)source[source_index];

        if (music_player_url_character_is_safe((char)character))
        {
            if (destination_index + 1U >= destination_size)
            {
                return ESP_ERR_INVALID_SIZE;
            }

            destination[destination_index++] =
                (char)character;
        }
        else
        {
            if (destination_index + 3U >= destination_size)
            {
                return ESP_ERR_INVALID_SIZE;
            }

            destination[destination_index++] = '%';

            destination[destination_index++] =
                hex[(character >> 4U) & 0x0FU];

            destination[destination_index++] =
                hex[character & 0x0FU];
        }
    }

    destination[destination_index] = '\0';

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                        Navidrome authentication                            */
/* -------------------------------------------------------------------------- */

static void music_player_generate_salt(
    char salt[MUSIC_NAVIDROME_SALT_LENGTH + 1U])
{
    uint32_t random_1 = esp_random();
    uint32_t random_2 = esp_random();

    snprintf(
        salt,
        MUSIC_NAVIDROME_SALT_LENGTH + 1U,
        "%08lx%08lx",
        (unsigned long)random_1,
        (unsigned long)random_2
    );
}

static esp_err_t music_player_generate_token(
    const char *password,
    const char *salt,
    char token[MUSIC_NAVIDROME_TOKEN_LENGTH + 1U])
{
    if (password == NULL ||
        salt == NULL ||
        token == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char authentication_text[
        NAVIDROME_PASSWORD_MAX_LENGTH +
        MUSIC_NAVIDROME_SALT_LENGTH +
        1U
    ];

    int written = snprintf(
        authentication_text,
        sizeof(authentication_text),
        "%s%s",
        password,
        salt
    );

    if (written < 0 ||
        (size_t)written >= sizeof(authentication_text))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[16];

    int md5_result = mbedtls_md5(
        (const unsigned char *)authentication_text,
        strlen(authentication_text),
        digest
    );

    if (md5_result != 0)
    {
        ESP_LOGE(
            TAG,
            "Could not generate Navidrome token: %d",
            md5_result
        );

        return ESP_FAIL;
    }

    for (size_t index = 0U;
         index < sizeof(digest);
         index++)
    {
        snprintf(
            &token[index * 2U],
            3U,
            "%02x",
            digest[index]
        );
    }

    token[MUSIC_NAVIDROME_TOKEN_LENGTH] = '\0';

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                         Build Navidrome stream URL                         */
/* -------------------------------------------------------------------------- */

static esp_err_t music_player_build_stream_url(
    const navidrome_song_t *song,
    char *url,
    size_t url_size)
{
    if (song == NULL ||
        url == NULL ||
        url_size == 0U ||
        song->id[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    char salt[MUSIC_NAVIDROME_SALT_LENGTH + 1U];
    char token[MUSIC_NAVIDROME_TOKEN_LENGTH + 1U];

    char encoded_username[
        NAVIDROME_USERNAME_MAX_LENGTH * 3U + 1U
    ];

    char encoded_song_id[
        NAVIDROME_SONG_ID_MAX_LENGTH * 3U + 1U
    ];

    music_player_generate_salt(salt);

    esp_err_t result = music_player_generate_token(
        g_navidrome_password,
        salt,
        token
    );

    if (result != ESP_OK)
    {
        return result;
    }

    result = music_player_url_encode(
        g_navidrome_username,
        encoded_username,
        sizeof(encoded_username)
    );

    if (result != ESP_OK)
    {
        return result;
    }

    result = music_player_url_encode(
        song->id,
        encoded_song_id,
        sizeof(encoded_song_id)
    );

    if (result != ESP_OK)
    {
        return result;
    }

    size_t base_length = strlen(g_navidrome_base_url);

    const char *separator =
        (
            base_length > 0U &&
            g_navidrome_base_url[base_length - 1U] == '/'
        )
        ? ""
        : "/";

    int written = snprintf(
        url,
        url_size,
        "%s%srest/stream.view"
        "?u=%s"
        "&t=%s"
        "&s=%s"
        "&v=%s"
        "&c=%s"
        "&id=%s"
        "&format=mp3"
        "&maxBitRate=%u",
        g_navidrome_base_url,
        separator,
        encoded_username,
        token,
        salt,
        MUSIC_NAVIDROME_API_VERSION,
        MUSIC_NAVIDROME_CLIENT_NAME,
        encoded_song_id,
        (unsigned int)g_music_player_max_bitrate_kbps
    );

    if (written < 0 ||
        (size_t)written >= url_size)
    {
        ESP_LOGE(
            TAG,
            "Navidrome stream URL is too long"
        );

        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                       Normal Audio.c ownership                             */
/* -------------------------------------------------------------------------- */

static esp_err_t music_player_release_alarm_audio(void)
{
    if (s_alarm_audio_driver_released)
    {
        return ESP_OK;
    }

    if (audio_is_playing())
    {
        ESP_LOGW(
            TAG,
            "Cannot release Audio driver while alarm audio is active"
        );

        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "Releasing normal Audio driver before MP3 decode"
    );

    esp_err_t result = audio_deinit();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not release Audio driver: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    s_alarm_audio_driver_released = true;

    music_player_log_heap(
        "Heap after releasing Audio driver"
    );

    return ESP_OK;
}

static void music_player_restore_alarm_audio(void)
{
    if (!s_alarm_audio_driver_released)
    {
        return;
    }

    ESP_LOGI(
        TAG,
        "Restoring normal Audio driver"
    );

    esp_err_t result = audio_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not restore Audio driver: %s",
            esp_err_to_name(result)
        );

        return;
    }

    s_alarm_audio_driver_released = false;

    ESP_LOGI(
        TAG,
        "Normal Audio driver restored"
    );
}

/* -------------------------------------------------------------------------- */
/*                           Continuous DAC                                   */
/* -------------------------------------------------------------------------- */

static void music_player_stop_dac(void)
{
    gpio_set_level(
        AUDIO_PIN_ENABLE,
        AUDIO_AMP_DISABLE_LEVEL
    );

    if (s_music_dac_handle != NULL)
    {
        uint8_t center_buffer[32];

        memset(
            center_buffer,
            MUSIC_DAC_CENTER_VALUE,
            sizeof(center_buffer)
        );

        size_t bytes_loaded = 0U;

        dac_continuous_write(
            s_music_dac_handle,
            center_buffer,
            sizeof(center_buffer),
            &bytes_loaded,
            100
        );

        dac_continuous_disable(
            s_music_dac_handle
        );

        dac_continuous_del_channels(
            s_music_dac_handle
        );

        s_music_dac_handle = NULL;
    }

    s_music_dac_sample_rate = 0U;
}

static esp_err_t music_player_start_dac(uint32_t sample_rate)
{
    if (sample_rate == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_music_dac_handle != NULL)
    {
        if (s_music_dac_sample_rate == sample_rate)
        {
            return ESP_OK;
        }

        ESP_LOGE(
            TAG,
            "Sample rate changed from %lu to %lu Hz",
            (unsigned long)s_music_dac_sample_rate,
            (unsigned long)sample_rate
        );

        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!s_alarm_audio_driver_released)
    {
        ESP_LOGE(
            TAG,
            "Audio driver still owns GPIO26"
        );

        return ESP_ERR_INVALID_STATE;
    }

    music_player_log_heap(
        "Heap before continuous DAC allocation"
    );

    dac_continuous_config_t dac_configuration =
    {
        .chan_mask = DAC_CHANNEL_MASK_CH1,
        .desc_num = MUSIC_DAC_DMA_DESCRIPTOR_COUNT,
        .buf_size = MUSIC_DAC_DMA_BUFFER_SIZE,
        .freq_hz = sample_rate,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL
    };

    esp_err_t result = dac_continuous_new_channels(
        &dac_configuration,
        &s_music_dac_handle
    );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Continuous DAC allocation failed: %s",
            esp_err_to_name(result)
        );

        s_music_dac_handle = NULL;

        return result;
    }

    result = dac_continuous_enable(
        s_music_dac_handle
    );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Continuous DAC enable failed: %s",
            esp_err_to_name(result)
        );

        dac_continuous_del_channels(
            s_music_dac_handle
        );

        s_music_dac_handle = NULL;

        return result;
    }

    s_music_dac_sample_rate = sample_rate;

    uint8_t center_buffer[32];

    memset(
        center_buffer,
        MUSIC_DAC_CENTER_VALUE,
        sizeof(center_buffer)
    );

    size_t bytes_loaded = 0U;

    result = dac_continuous_write(
        s_music_dac_handle,
        center_buffer,
        sizeof(center_buffer),
        &bytes_loaded,
        -1
    );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not initialize DAC center level: %s",
            esp_err_to_name(result)
        );

        music_player_stop_dac();

        return result;
    }

    gpio_set_level(
        AUDIO_PIN_ENABLE,
        AUDIO_AMP_ENABLE_LEVEL
    );

    esp_rom_delay_us(
        MUSIC_AMP_WAKE_DELAY_US
    );

    ESP_LOGI(
        TAG,
        "Music DAC started: GPIO%d, sample_rate=%lu Hz, DMA=%u x %u bytes",
        AUDIO_PIN_DAC,
        (unsigned long)sample_rate,
        MUSIC_DAC_DMA_DESCRIPTOR_COUNT,
        MUSIC_DAC_DMA_BUFFER_SIZE
    );

    music_player_log_heap(
        "Heap after continuous DAC startup"
    );

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                             Pause handling                                 */
/* -------------------------------------------------------------------------- */

static void music_player_wait_while_paused(void)
{
    bool amplifier_muted = false;

    while (g_music_player_paused &&
           !g_music_player_stop_requested)
    {
        if (!amplifier_muted)
        {
            gpio_set_level(
                AUDIO_PIN_ENABLE,
                AUDIO_AMP_DISABLE_LEVEL
            );

            amplifier_muted = true;

            ESP_LOGI(
                TAG,
                "Music paused at %lu s",
                (unsigned long)g_music_player_position_seconds
            );
        }

        vTaskDelay(
            pdMS_TO_TICKS(50)
        );
    }

    if (amplifier_muted &&
        !g_music_player_stop_requested &&
        s_music_dac_handle != NULL)
    {
        gpio_set_level(
            AUDIO_PIN_ENABLE,
            AUDIO_AMP_ENABLE_LEVEL
        );

        esp_rom_delay_us(
            MUSIC_AMP_WAKE_DELAY_US
        );

        ESP_LOGI(
            TAG,
            "Music resumed at %lu s",
            (unsigned long)g_music_player_position_seconds
        );
    }
}

/* -------------------------------------------------------------------------- */
/*                          PCM -> mono -> DAC                                */
/* -------------------------------------------------------------------------- */

static esp_err_t music_player_write_pcm(
    uint8_t *pcm,
    size_t pcm_size,
    const esp_audio_simple_dec_info_t *info)
{
    if (pcm == NULL ||
        info == NULL ||
        pcm_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (info->bits_per_sample != 16U)
    {
        ESP_LOGE(
            TAG,
            "Unsupported PCM bit depth: %u",
            info->bits_per_sample
        );

        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->channel != 1U &&
        info->channel != 2U)
    {
        ESP_LOGE(
            TAG,
            "Unsupported PCM channel count: %u",
            info->channel
        );

        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_music_dac_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t pcm_frame_size =
        (size_t)info->channel *
        sizeof(int16_t);

    size_t sample_count =
        pcm_size /
        pcm_frame_size;

    /*
     * Convert the decoder's signed 16-bit mono/stereo PCM in-place to one
     * unsigned 8-bit DAC sample per audio frame.
     */
    for (size_t sample_index = 0U;
         sample_index < sample_count;
         sample_index++)
    {
        size_t source_offset =
            sample_index *
            pcm_frame_size;

        int32_t left =
            music_player_read_s16_le(
                &pcm[source_offset]
            );

        int32_t mono =
            left;

        if (info->channel == 2U)
        {
            int32_t right =
                music_player_read_s16_le(
                    &pcm[source_offset + 2U]
                );

            mono =
                (left + right) /
                2;
        }

        mono =
            audio_process_sample_s16(
                mono
            );

        pcm[sample_index] =
            music_player_sample_to_dac(
                mono
            );
    }

    size_t total_written = 0U;
    uint32_t consecutive_timeouts = 0U;

    while (total_written < sample_count &&
           !g_music_player_stop_requested)
    {
        music_player_wait_while_paused();

        if (g_music_player_stop_requested)
        {
            break;
        }

        size_t bytes_remaining =
            sample_count -
            total_written;

        size_t bytes_to_write =
            bytes_remaining;

        if (bytes_to_write >
            MUSIC_DAC_WRITE_CHUNK_SIZE)
        {
            bytes_to_write =
                MUSIC_DAC_WRITE_CHUNK_SIZE;
        }

        size_t bytes_loaded = 0U;

        esp_err_t result =
            dac_continuous_write(
                s_music_dac_handle,
                &pcm[total_written],
                bytes_to_write,
                &bytes_loaded,
                MUSIC_DAC_WRITE_TIMEOUT_MS
            );

        /*
         * A timeout is recoverable if some data was accepted. Account for it
         * and continue. A repeated zero-byte timeout means the DAC pipeline is
         * stalled; fail cleanly instead of trapping Music Mode forever.
         */
        if (result == ESP_ERR_TIMEOUT)
        {
            if (bytes_loaded > 0U)
            {
                total_written +=
                    bytes_loaded;

                s_music_output_samples +=
                    bytes_loaded;

                consecutive_timeouts =
                    0U;
            }
            else
            {
                consecutive_timeouts++;

                if (consecutive_timeouts >=
                    MUSIC_DAC_MAX_CONSECUTIVE_TIMEOUTS)
                {
                    ESP_LOGE(
                        TAG,
                        "DAC output stalled: %u consecutive timeouts",
                        (unsigned int)consecutive_timeouts
                    );

                    return ESP_ERR_TIMEOUT;
                }

                vTaskDelay(1);
            }

            continue;
        }

        if (result != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "DAC write failed: %s",
                esp_err_to_name(result)
            );

            return result;
        }

        if (bytes_loaded == 0U)
        {
            ESP_LOGE(
                TAG,
                "DAC accepted zero bytes"
            );

            return ESP_FAIL;
        }

        if (s_music_output_samples == 0U)
        {
            ESP_LOGI(
                TAG,
                "PCM output started: first %u DAC samples loaded",
                (unsigned int)bytes_loaded
            );
        }

        consecutive_timeouts =
            0U;

        total_written +=
            bytes_loaded;

        s_music_output_samples +=
            bytes_loaded;

        if (s_music_dac_sample_rate > 0U)
        {
            g_music_player_position_seconds =
                (uint32_t)(
                    s_music_output_samples /
                    s_music_dac_sample_rate
                );
        }
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                         MP3 header probing                                 */
/* -------------------------------------------------------------------------- */

static bool music_player_parse_mp3_header(const uint8_t *header,
                                          uint32_t *sample_rate)
{
    if (header == NULL || sample_rate == NULL)
    {
        return false;
    }

    if (header[0] != 0xFFU || (header[1] & 0xE0U) != 0xE0U)
    {
        return false;
    }

    uint8_t version = (header[1] >> 3U) & 0x03U;
    uint8_t layer = (header[1] >> 1U) & 0x03U;
    uint8_t bitrate_index = (header[2] >> 4U) & 0x0FU;
    uint8_t sample_rate_index = (header[2] >> 2U) & 0x03U;

    /* Reserved MPEG version, not Layer III, invalid/free bitrate, bad rate. */
    if (version == 1U ||
        layer != 1U ||
        bitrate_index == 0U ||
        bitrate_index == 15U ||
        sample_rate_index == 3U)
    {
        return false;
    }

    static const uint32_t mpeg1_rates[3] =
    {
        44100U,
        48000U,
        32000U
    };

    uint32_t rate = mpeg1_rates[sample_rate_index];

    if (version == 2U)
    {
        rate /= 2U;
    }
    else if (version == 0U)
    {
        rate /= 4U;
    }

    if (rate == 0U)
    {
        return false;
    }

    *sample_rate = rate;
    return true;
}

static bool music_player_find_mp3_header(const uint8_t *data,
                                         size_t data_size,
                                         size_t *header_offset,
                                         uint32_t *sample_rate)
{
    if (data == NULL ||
        header_offset == NULL ||
        sample_rate == NULL ||
        data_size < 4U)
    {
        return false;
    }

    for (size_t index = 0U; index + 4U <= data_size; index++)
    {
        uint32_t detected_rate = 0U;

        if (music_player_parse_mp3_header(&data[index], &detected_rate))
        {
            *header_offset = index;
            *sample_rate = detected_rate;
            return true;
        }
    }

    return false;
}

static esp_err_t music_player_probe_mp3_stream(esp_http_client_handle_t http_client,
                                                uint8_t *stream_buffer,
                                                size_t stream_buffer_size,
                                                size_t *buffered_bytes,
                                                uint32_t *sample_rate)
{
    if (http_client == NULL ||
        stream_buffer == NULL ||
        stream_buffer_size < 4U ||
        buffered_bytes == NULL ||
        sample_rate == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t retained = 0U;
    size_t total_read = 0U;

    while (!g_music_player_stop_requested &&
           total_read < MUSIC_MP3_PROBE_MAX_BYTES)
    {
        size_t available = stream_buffer_size - retained;

        if (available == 0U)
        {
            return ESP_FAIL;
        }

        int read_length = esp_http_client_read(
            http_client,
            (char *)&stream_buffer[retained],
            available
        );

        if (read_length == -ESP_ERR_HTTP_EAGAIN)
        {
            vTaskDelay(1);
            continue;
        }

        if (read_length < 0)
        {
            ESP_LOGE(TAG, "MP3 probe read failed: %d", read_length);
            return ESP_FAIL;
        }

        if (read_length == 0)
        {
            ESP_LOGE(TAG, "Stream ended before an MP3 frame was found");
            return ESP_FAIL;
        }

        size_t total = retained + (size_t)read_length;
        total_read += (size_t)read_length;

        size_t header_offset = 0U;
        uint32_t detected_rate = 0U;

        if (music_player_find_mp3_header(stream_buffer,
                                         total,
                                         &header_offset,
                                         &detected_rate))
        {
            size_t audio_bytes = total - header_offset;

            if (header_offset > 0U)
            {
                memmove(stream_buffer,
                        &stream_buffer[header_offset],
                        audio_bytes);
            }

            *buffered_bytes = audio_bytes;
            *sample_rate = detected_rate;

            ESP_LOGI(TAG,
                     "MP3 header found: sample_rate=%lu Hz, retained=%lu bytes",
                     (unsigned long)detected_rate,
                     (unsigned long)audio_bytes);

            return ESP_OK;
        }

        /* Keep only the bytes that could be the start of a split 4-byte header. */
        retained = total < 3U ? total : 3U;

        if (retained > 0U)
        {
            memmove(stream_buffer,
                    &stream_buffer[total - retained],
                    retained);
        }
    }

    ESP_LOGE(TAG,
             "No MP3 frame found in the first %u bytes",
             MUSIC_MP3_PROBE_MAX_BYTES);

    return ESP_ERR_NOT_FOUND;
}

/* -------------------------------------------------------------------------- */
/*                       HTTP stream + MP3 decoder                            */
/* -------------------------------------------------------------------------- */

static esp_err_t music_player_stream_song(const navidrome_song_t *song)
{
    esp_err_t result = ESP_OK;
    esp_http_client_handle_t http_client = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    uint8_t *stream_buffer = NULL;
    uint8_t *decoder_output_buffer = NULL;

    bool decoder_registered = false;
    bool decoder_info_ready = false;
    bool first_decode_log_pending = true;

    uint32_t decoded_frame_counter = 0U;
    uint32_t detected_sample_rate = 0U;
    size_t initial_stream_bytes = 0U;

    esp_audio_simple_dec_info_t decoder_info = {0};
    char stream_url[MUSIC_STREAM_URL_SIZE];

    result = music_player_build_stream_url(song, stream_url, sizeof(stream_url));

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not build stream URL: %s", esp_err_to_name(result));
        goto cleanup;
    }

    ESP_LOGI(TAG, "Streaming: %s - %s", song->artist, song->title);
    ESP_LOGI(TAG,
             "Requested MP3 bitrate: %u kbps",
             (unsigned int)g_music_player_max_bitrate_kbps);

    music_player_log_heap("Heap before music buffers");

    stream_buffer = malloc(MUSIC_STREAM_INPUT_BUFFER_SIZE);
    decoder_output_buffer = malloc(MUSIC_DECODER_OUTPUT_SIZE);

    if (stream_buffer == NULL || decoder_output_buffer == NULL)
    {
        ESP_LOGE(TAG, "Could not allocate music buffers");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    music_player_log_heap("Heap after music buffers");

    /* Open HTTP before the MP3 decoder performs its large lazy allocation. */
    esp_http_client_config_t http_configuration =
    {
        .url = stream_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = MUSIC_HTTP_TIMEOUT_MS,
        .buffer_size = 512,
        .buffer_size_tx = 512
    };

    http_client = esp_http_client_init(&http_configuration);

    if (http_client == NULL)
    {
        ESP_LOGE(TAG, "Could not create HTTP client");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    result = esp_http_client_open(http_client, 0);

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not open music stream: %s", esp_err_to_name(result));
        goto cleanup;
    }

    int64_t content_length;

    do
    {
        content_length = esp_http_client_fetch_headers(http_client);
    }
    while (content_length == -ESP_ERR_HTTP_EAGAIN &&
           !g_music_player_stop_requested);

    if (content_length < 0)
    {
        ESP_LOGE(TAG, "Could not receive music HTTP headers");
        result = ESP_FAIL;
        goto cleanup;
    }

    int http_status = esp_http_client_get_status_code(http_client);
    ESP_LOGI(TAG, "Music HTTP status: %d", http_status);

    if (http_status != 200)
    {
        ESP_LOGE(TAG, "Navidrome stream returned HTTP %d", http_status);
        result = ESP_FAIL;
        goto cleanup;
    }

    g_music_player_state =
        MUSIC_PLAYER_STATE_BUFFERING;

    if (content_length > 0)
    {
        ESP_LOGI(TAG, "Stream length: %lld bytes", (long long)content_length);
    }
    else
    {
        ESP_LOGI(TAG, "Stream has no fixed Content-Length");
    }

    /* GPIO26 must be released before continuous DAC mode can be allocated. */
    result = music_player_release_alarm_audio();

    if (result != ESP_OK)
    {
        goto cleanup;
    }

    music_player_log_heap("Heap before MP3 header probe");

    /* Determine the exact MP3 rate without triggering the decoder's big allocation. */
    result = music_player_probe_mp3_stream(
        http_client,
        stream_buffer,
        MUSIC_STREAM_INPUT_BUFFER_SIZE,
        &initial_stream_bytes,
        &detected_sample_rate
    );

    if (result != ESP_OK)
    {
        goto cleanup;
    }

    /* Critical ordering: reserve DAC DMA while plenty of heap is still available. */
    result = music_player_start_dac(detected_sample_rate);

    if (result != ESP_OK)
    {
        goto cleanup;
    }

    music_player_log_heap("Heap after EARLY DAC reservation");

    /* Only now open the decoder. Its large internal allocation occurs on first decode. */
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    decoder_registered = true;

    esp_audio_simple_dec_cfg_t decoder_configuration =
    {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false
    };

    music_player_log_heap("Heap before decoder open after DAC reservation");

    esp_audio_err_t decoder_result = esp_audio_simple_dec_open(
        &decoder_configuration,
        &decoder
    );

    if (decoder_result != ESP_AUDIO_ERR_OK)
    {
        ESP_LOGE(TAG, "Could not open MP3 decoder: %d", decoder_result);
        result = decoder_result == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "MP3 decoder opened after DAC reservation");

    audio_reset_filter();

    ESP_LOGI(TAG,
             "Music processing: Gain_Q8=%ld, BPF=%s",
             (long)audio_get_gain_q8(),
             audio_get_bandpass_enabled() ? "ON" : "OFF");

    bool use_initial_block = initial_stream_bytes > 0U;

    while (!g_music_player_stop_requested)
    {
        music_player_wait_while_paused();

        if (g_music_player_stop_requested)
        {
            break;
        }

        int read_length = 0;

        if (use_initial_block)
        {
            read_length = (int)initial_stream_bytes;
            use_initial_block = false;
        }
        else
        {
            read_length = esp_http_client_read(
                http_client,
                (char *)stream_buffer,
                MUSIC_STREAM_INPUT_BUFFER_SIZE
            );

            if (read_length == -ESP_ERR_HTTP_EAGAIN)
            {
                vTaskDelay(1);
                continue;
            }

            if (read_length < 0)
            {
                ESP_LOGE(TAG, "Music stream read failed: %d", read_length);
                result = ESP_FAIL;
                goto cleanup;
            }

            if (read_length == 0)
            {
                ESP_LOGI(TAG, "End of music stream");
                break;
            }
        }

        esp_audio_simple_dec_raw_t raw =
        {
            .buffer = stream_buffer,
            .len = (uint32_t)read_length,
            .eos = false,
            .consumed = 0U
        };

        while (raw.len > 0U && !g_music_player_stop_requested)
        {
            if (first_decode_log_pending)
            {
                music_player_log_heap("Heap before first MP3 decode WITH DAC reserved");
                ESP_LOGI(TAG,
                         "First MP3 input block: %lu bytes",
                         (unsigned long)raw.len);
                first_decode_log_pending = false;
            }

            esp_audio_simple_dec_out_t decoded =
            {
                .buffer = decoder_output_buffer,
                .len = MUSIC_DECODER_OUTPUT_SIZE,
                .needed_size = 0U,
                .decoded_size = 0U
            };

            decoder_result = esp_audio_simple_dec_process(
                decoder,
                &raw,
                &decoded
            );

            if (decoder_result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH)
            {
                ESP_LOGE(TAG,
                         "PCM buffer too small: have=%u need=%lu",
                         MUSIC_DECODER_OUTPUT_SIZE,
                         (unsigned long)decoded.needed_size);
                result = ESP_ERR_NO_MEM;
                goto cleanup;
            }

            if (decoder_result != ESP_AUDIO_ERR_OK)
            {
                ESP_LOGE(TAG, "MP3 decode failed: %d", decoder_result);
                music_player_log_heap("Heap after MP3 decode failure");
                result = decoder_result == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
                goto cleanup;
            }

            if (decoded.decoded_size > 0U)
            {
                if (!decoder_info_ready)
                {
                    decoder_result = esp_audio_simple_dec_get_info(
                        decoder,
                        &decoder_info
                    );

                    if (decoder_result != ESP_AUDIO_ERR_OK)
                    {
                        ESP_LOGE(TAG, "Could not read MP3 information: %d", decoder_result);
                        result = ESP_FAIL;
                        goto cleanup;
                    }

                    ESP_LOGI(TAG,
                             "MP3 decoded: %lu Hz, %u-bit, %u channel(s), bitrate field=%lu",
                             (unsigned long)decoder_info.sample_rate,
                             decoder_info.bits_per_sample,
                             decoder_info.channel,
                             (unsigned long)decoder_info.bitrate);

                    music_player_log_heap("Heap after first successful MP3 decode");

                    if (decoder_info.bits_per_sample != 16U ||
                        (decoder_info.channel != 1U && decoder_info.channel != 2U))
                    {
                        result = ESP_ERR_NOT_SUPPORTED;
                        goto cleanup;
                    }

                    if (decoder_info.sample_rate != detected_sample_rate)
                    {
                        ESP_LOGE(TAG,
                                 "MP3 sample-rate mismatch: header=%lu decoder=%lu",
                                 (unsigned long)detected_sample_rate,
                                 (unsigned long)decoder_info.sample_rate);
                        result = ESP_ERR_INVALID_RESPONSE;
                        goto cleanup;
                    }

                    decoder_info_ready = true;
                }

                result = music_player_write_pcm(
                    decoded.buffer,
                    decoded.decoded_size,
                    &decoder_info
                );

                if (result != ESP_OK)
                {
                    goto cleanup;
                }

                if (!g_music_player_paused)
                {
                    g_music_player_state =
                        MUSIC_PLAYER_STATE_PLAYING;
                }

                decoded_frame_counter++;

                if (decoded_frame_counter >= MUSIC_DECODED_FRAMES_BEFORE_YIELD)
                {
                    decoded_frame_counter = 0U;
                    vTaskDelay(1);
                }
            }

            if (raw.consumed > raw.len)
            {
                ESP_LOGE(TAG, "Decoder reported invalid consumed size");
                result = ESP_FAIL;
                goto cleanup;
            }

            if (raw.consumed == 0U)
            {
                /* Simple Decoder has cached the block and needs more compressed data. */
                break;
            }

            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            raw.consumed = 0U;
        }
    }

cleanup:

    music_player_stop_dac();

    if (decoder != NULL)
    {
        esp_audio_simple_dec_close(decoder);
        decoder = NULL;
    }

    if (decoder_registered)
    {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        decoder_registered = false;
    }

    if (http_client != NULL)
    {
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        http_client = NULL;
    }

    if (stream_buffer != NULL)
    {
        free(stream_buffer);
        stream_buffer = NULL;
    }

    if (decoder_output_buffer != NULL)
    {
        free(decoder_output_buffer);
        decoder_output_buffer = NULL;
    }

    music_player_restore_alarm_audio();
    music_player_log_heap("Heap after MusicPlayer cleanup");

    return result;
}

/* -------------------------------------------------------------------------- */
/*                               Player task                                  */
/* -------------------------------------------------------------------------- */

static void music_player_task(void *argument)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Music task started"
    );

    ESP_LOGI(
        TAG,
        "Song: %s - %s",
        g_music_player_song.artist,
        g_music_player_song.title
    );

    music_player_log_heap(
        "Heap at MusicPlayer task start"
    );

    esp_err_t result =
        music_player_stream_song(
            &g_music_player_song
        );

    bool playback_failed = false;

    if (g_music_player_stop_requested)
    {
        ESP_LOGI(
            TAG,
            "Music stopped by request"
        );
    }
    else if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Music playback completed"
        );
    }
    else
    {
        playback_failed = true;

        ESP_LOGE(
            TAG,
            "Music playback failed: %s",
            esp_err_to_name(result)
        );
    }

    /*
     * On ESP-IDF, this value is already reported in bytes.
     */
    ESP_LOGI(
        TAG,
        "Music task minimum free stack: %u bytes",
        (unsigned int)uxTaskGetStackHighWaterMark(NULL)
    );

    g_music_player_playing = false;
    g_music_player_stop_requested = false;
    g_music_player_exclusive_mode = false;
    g_music_player_paused = false;

    g_music_player_state =
        playback_failed
        ? MUSIC_PLAYER_STATE_ERROR
        : MUSIC_PLAYER_STATE_STOPPED;

    s_music_player_task = NULL;

    ESP_LOGI(
        TAG,
        "Music task finished"
    );

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/*                                Public API                                  */
/* -------------------------------------------------------------------------- */

esp_err_t music_player_start(const navidrome_song_t *song)
{
    if (song == NULL ||
        song->id[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (g_music_player_playing ||
        s_music_player_task != NULL)
    {
        ESP_LOGW(
            TAG,
            "Music is already playing"
        );

        return ESP_ERR_INVALID_STATE;
    }

    if (audio_is_playing())
    {
        ESP_LOGW(
            TAG,
            "Cannot start music while alarm audio is playing"
        );

        return ESP_ERR_INVALID_STATE;
    }

    g_music_player_song = *song;

    g_music_player_stop_requested = false;
    g_music_player_playing = true;
    g_music_player_exclusive_mode = true;
    g_music_player_paused = false;
    g_music_player_position_seconds = 0U;
    g_music_player_state = MUSIC_PLAYER_STATE_CONNECTING;

    s_music_output_samples = 0U;

    BaseType_t task_result =
        xTaskCreatePinnedToCore(
            music_player_task,
            "music_player",
            MUSIC_PLAYER_TASK_STACK_SIZE,
            NULL,
            MUSIC_PLAYER_TASK_PRIORITY,
            &s_music_player_task,
            MUSIC_PLAYER_TASK_CORE
        );

    if (task_result != pdPASS)
    {
        g_music_player_playing = false;
        g_music_player_stop_requested = false;
        g_music_player_exclusive_mode = false;
        g_music_player_paused = false;
        g_music_player_state = MUSIC_PLAYER_STATE_ERROR;

        s_music_player_task = NULL;

        ESP_LOGE(
            TAG,
            "Could not create MusicPlayer task"
        );

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void music_player_stop(void)
{
    if (!g_music_player_playing)
    {
        return;
    }

    if (g_music_player_stop_requested)
    {
        return;
    }

    ESP_LOGI(
        TAG,
        "Music stop requested"
    );

    g_music_player_stop_requested = true;
    g_music_player_paused = false;
}

esp_err_t music_player_pause(void)
{
    if (!g_music_player_playing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (g_music_player_paused)
    {
        return ESP_OK;
    }

    g_music_player_paused = true;
    g_music_player_state = MUSIC_PLAYER_STATE_PAUSED;

    return ESP_OK;
}

esp_err_t music_player_resume(void)
{
    if (!g_music_player_playing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!g_music_player_paused)
    {
        return ESP_OK;
    }

    g_music_player_paused = false;
    g_music_player_state = MUSIC_PLAYER_STATE_PLAYING;

    return ESP_OK;
}

bool music_player_is_playing(void)
{
    return g_music_player_playing;
}

bool music_player_is_paused(void)
{
    return g_music_player_paused;
}

bool music_player_is_exclusive_mode(void)
{
    return g_music_player_exclusive_mode;
}

music_player_state_t music_player_get_state(void)
{
    return g_music_player_state;
}

const char *music_player_state_to_string(music_player_state_t state)
{
    switch (state)
    {
        case MUSIC_PLAYER_STATE_CONNECTING:
            return "connecting";

        case MUSIC_PLAYER_STATE_BUFFERING:
            return "buffering";

        case MUSIC_PLAYER_STATE_PLAYING:
            return "playing";

        case MUSIC_PLAYER_STATE_PAUSED:
            return "paused";

        case MUSIC_PLAYER_STATE_ERROR:
            return "error";

        case MUSIC_PLAYER_STATE_STOPPED:
        default:
            return "stopped";
    }
}

uint32_t music_player_get_position_seconds(void)
{
    return g_music_player_position_seconds;
}

void music_player_get_current_song(navidrome_song_t *song)
{
    if (song == NULL)
    {
        return;
    }

    *song = g_music_player_song;
}