#include "MusicStream.h"
#include "HTTPControl.h"
#include "Audio.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "MusicStream";
#define STREAM_TASK_STACK 5120U
#define STREAM_READ_BYTES 512U
#define STREAM_DAC_CHUNK 256U
#define STREAM_TIMEOUT_MS 1500

volatile music_stream_state_t g_music_stream_state = MUSIC_STREAM_STOPPED;
volatile uint32_t g_music_stream_position_seconds = 0U;
static music_stream_song_t s_song;
static TaskHandle_t s_task = NULL;
static volatile bool s_busy = false;
static volatile bool s_stop = false;
static volatile bool s_paused = false;

static void str_copy(char *dest, size_t cap, const char *src)
{
    if (cap) snprintf(dest, cap, "%s", src ? src : "");
}
static void read_json_string(cJSON *obj, const char *key, char *dest, size_t cap)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    str_copy(dest, cap, cJSON_IsString(v) ? v->valuestring : "");
}
static void set_token(esp_http_client_handle_t client)
{
    esp_http_client_set_header(client, "X-Device-Token", g_timewise_device_token);
}
static esp_err_t prepare_url(char *url, size_t cap, const char *path)
{
    int n = snprintf(url, cap, "%s%s", g_timewise_gateway_url, path);
    return n < 0 || (size_t)n >= cap ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

typedef struct {
    char text[2048];
    size_t used;
    bool overflow;
} select_buffer_t;
static esp_err_t receive_select(esp_http_client_event_t *event)
{
    if (!event || !event->user_data || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
        return ESP_OK;
    select_buffer_t *buf = (select_buffer_t *)event->user_data;
    size_t n = (size_t)event->data_len;
    if (n > sizeof(buf->text) - 1U - buf->used) { buf->overflow = true; return ESP_ERR_INVALID_SIZE; }
    memcpy(buf->text + buf->used, event->data, n);
    buf->used += n;
    buf->text[buf->used] = '\0';
    return ESP_OK;
}

esp_err_t music_stream_select(const char *action, music_stream_song_t *song)
{
    if (!action || !song || s_busy) return ESP_ERR_INVALID_STATE;
    if (strcmp(action, "random") && strcmp(action, "next") && strcmp(action, "previous"))
        return ESP_ERR_INVALID_ARG;
    char url[200];
    esp_err_t err = prepare_url(url, sizeof(url), "/music/select");
    if (err != ESP_OK) return err;
    select_buffer_t response = {0};
    esp_http_client_config_t config = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 5000,
        .event_handler = receive_select, .user_data = &response,
        .buffer_size = 512, .buffer_size_tx = 512
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    set_token(client);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char body[64];
    snprintf(body, sizeof(body), "{\"action\":\"%s\"}", action);
    esp_http_client_set_post_field(client, body, strlen(body));
    err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    if (err != ESP_OK || response.overflow) return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    if (status != 200) {
        ESP_LOGE(TAG, "Gateway /music/select: HTTP %d", status);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(response.text);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    music_stream_song_t selected = {0};
    read_json_string(root, "id", selected.id, sizeof(selected.id));
    read_json_string(root, "title", selected.title, sizeof(selected.title));
    read_json_string(root, "artist", selected.artist, sizeof(selected.artist));
    read_json_string(root, "album", selected.album, sizeof(selected.album));
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
    if (cJSON_IsNumber(duration) && duration->valuedouble >= 0)
        selected.duration_seconds = (uint32_t)duration->valuedouble;
    cJSON_Delete(root);
    if (!selected.id[0]) return ESP_ERR_INVALID_RESPONSE;
    s_song = selected;
    *song = selected;
    g_music_stream_position_seconds = 0;
    g_music_stream_state = MUSIC_STREAM_STOPPED;
    ESP_LOGI(TAG, "Gateway selected: %s - %s", selected.artist, selected.title);
    return ESP_OK;
}

static esp_err_t encode_id(const char *id, char *dst, size_t capacity)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; id[i]; ++i) {
        unsigned char c = (unsigned char)id[i];
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (j + (safe ? 1 : 3) >= capacity) return ESP_ERR_INVALID_SIZE;
        if (safe) dst[j++] = (char)c;
        else { dst[j++] = '%'; dst[j++] = digits[c >> 4]; dst[j++] = digits[c & 15]; }
    }
    dst[j] = 0;
    return ESP_OK;
}
static void wait_if_paused(void)
{
    while (s_paused && !s_stop) vTaskDelay(pdMS_TO_TICKS(30));
}
static esp_err_t stream_pcm(void)
{
    esp_err_t err = ESP_OK;
    dac_continuous_handle_t dac = NULL;
    esp_http_client_handle_t client = NULL;
    bool audio_released = false;
    bool dac_enabled = false;
    uint64_t samples = 0;
    char encoded_id[3 * sizeof(s_song.id) + 1];
    char path[400];
    char url[600];
    uint8_t buffer[STREAM_READ_BYTES];
    if ((err = encode_id(s_song.id, encoded_id, sizeof(encoded_id))) != ESP_OK) goto cleanup;
    snprintf(path, sizeof(path), "/music/stream?id=%s", encoded_id);
    if ((err = prepare_url(url, sizeof(url), path)) != ESP_OK) goto cleanup;
    esp_http_client_config_t config = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = STREAM_TIMEOUT_MS,
        .buffer_size = 512, .buffer_size_tx = 512
    };
    client = esp_http_client_init(&config);
    if (!client) { err = ESP_ERR_NO_MEM; goto cleanup; }
    set_token(client);
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) goto cleanup;
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) { err = ESP_FAIL; goto cleanup; }
    int status = esp_http_client_get_status_code(client);
    if (status != 200) { ESP_LOGE(TAG, "Gateway PCM HTTP %d", status); err = ESP_FAIL; goto cleanup; }
    ESP_LOGI(TAG, "PCM HTTP 200, 24000 Hz, mono unsigned 8-bit");
    if (audio_is_playing()) { err = ESP_ERR_INVALID_STATE; goto cleanup; }
    if ((err = audio_deinit()) != ESP_OK) goto cleanup;
    audio_released = true;
    dac_continuous_config_t dac_config = {
        .chan_mask = DAC_CHANNEL_MASK_CH1,
        .desc_num = 6, .buf_size = 512,
        .freq_hz = MUSIC_STREAM_SAMPLE_RATE,
        .offset = 0, .clk_src = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL
    };
    if ((err = dac_continuous_new_channels(&dac_config, &dac)) != ESP_OK) goto cleanup;
    if ((err = dac_continuous_enable(dac)) != ESP_OK) goto cleanup;
    dac_enabled = true;
    gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_ENABLE_LEVEL);
    g_music_stream_state = MUSIC_STREAM_BUFFERING;
    while (!s_stop) {
        wait_if_paused();
        if (s_stop) break;
        int got = esp_http_client_read(client, (char *)buffer, sizeof(buffer));
        if (got == -ESP_ERR_HTTP_EAGAIN) continue;
        if (got < 0) { ESP_LOGE(TAG, "PCM read failed: %d", got); err = ESP_FAIL; break; }
        if (got == 0) { ESP_LOGI(TAG, "PCM stream completed"); break; }
        /* The Gateway emits DAC-ready unsigned 8-bit mono, one byte per sample.
           Volume is adjusted locally; no codec/MP3 decoder is used on ESP32. */
        for (int i = 0; i < got; ++i) {
            int32_t sample = ((int32_t)buffer[i] - 128) << 8;
            sample = audio_process_sample_s16(sample);
            int32_t v = (sample >> 8) + 128;
            buffer[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        size_t written = 0;
        while (written < (size_t)got && !s_stop) {
            wait_if_paused();
            if (s_stop) break;
            size_t remaining = (size_t)got - written;
            if (remaining > STREAM_DAC_CHUNK) remaining = STREAM_DAC_CHUNK;
            size_t loaded = 0;
            esp_err_t wr = dac_continuous_write(dac, buffer + written, remaining, &loaded, 250);
            if (loaded) {
                samples += loaded;
                written += loaded;
                g_music_stream_position_seconds = (uint32_t)(samples / MUSIC_STREAM_SAMPLE_RATE);
                if (!s_paused) g_music_stream_state = MUSIC_STREAM_PLAYING;
            }
            if (wr != ESP_OK && wr != ESP_ERR_TIMEOUT) { err = wr; break; }
            if (!loaded && wr == ESP_OK) { err = ESP_FAIL; break; }
            if (err != ESP_OK) break;
        }
        if (err != ESP_OK) break;
    }
cleanup:
    gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_DISABLE_LEVEL);
    if (dac) {
        if (dac_enabled) dac_continuous_disable(dac);
        dac_continuous_del_channels(dac);
    }
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    if (audio_released) {
        esp_err_t restore = audio_init();
        if (restore != ESP_OK) ESP_LOGE(TAG, "Alarm audio restore failed: %s", esp_err_to_name(restore));
    }
    return err;
}
static void task_entry(void *arg)
{
    (void)arg;
    esp_err_t err = stream_pcm();
    bool stopped = s_stop;
    g_music_stream_state = stopped || err == ESP_OK ? MUSIC_STREAM_STOPPED : MUSIC_STREAM_ERROR;
    if (err != ESP_OK && !stopped) ESP_LOGE(TAG, "PCM playback failed: %s", esp_err_to_name(err));
    s_paused = false;
    s_stop = false;
    s_busy = false;
    s_task = NULL;
    vTaskDelete(NULL);
}
esp_err_t music_stream_start(void)
{
    if (s_busy || !s_song.id[0]) return ESP_ERR_INVALID_STATE;
    s_stop = false;
    s_paused = false;
    s_busy = true;
    g_music_stream_position_seconds = 0;
    g_music_stream_state = MUSIC_STREAM_CONNECTING;
    if (xTaskCreatePinnedToCore(task_entry, "pcm_stream", STREAM_TASK_STACK,
                                NULL, 4, &s_task, 1) != pdPASS) {
        s_task = NULL;
        s_busy = false;
        g_music_stream_state = MUSIC_STREAM_ERROR;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void music_stream_stop(void) { s_stop = true; s_paused = false; }
esp_err_t music_stream_pause(void)
{
    if (!s_busy) return ESP_ERR_INVALID_STATE;
    s_paused = true;
    gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_DISABLE_LEVEL);
    g_music_stream_state = MUSIC_STREAM_PAUSED;
    return ESP_OK;
}
esp_err_t music_stream_resume(void)
{
    if (!s_busy) return ESP_ERR_INVALID_STATE;
    s_paused = false;
    gpio_set_level(AUDIO_PIN_ENABLE, AUDIO_AMP_ENABLE_LEVEL);
    g_music_stream_state = MUSIC_STREAM_PLAYING;
    return ESP_OK;
}
bool music_stream_is_busy(void) { return s_busy; }
bool music_stream_is_paused(void) { return s_paused; }
music_stream_state_t music_stream_get_state(void) { return g_music_stream_state; }
uint32_t music_stream_get_position_seconds(void) { return g_music_stream_position_seconds; }
void music_stream_get_current_song(music_stream_song_t *song)
{
    if (song) *song = s_song;
}
const char *music_stream_state_to_string(music_stream_state_t state)
{
    switch (state) {
        case MUSIC_STREAM_CONNECTING: return "connecting";
        case MUSIC_STREAM_BUFFERING: return "buffering";
        case MUSIC_STREAM_PLAYING: return "playing";
        case MUSIC_STREAM_PAUSED: return "paused";
        case MUSIC_STREAM_ERROR: return "error";
        default: return "stopped";
    }
}
