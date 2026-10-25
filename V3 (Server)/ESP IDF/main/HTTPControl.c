#include "HTTPControl.h"
#include "Audio.h"
#include "MusicStream.h"
#include "NTPClock.h"
#include "TFT_Display.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HTTPControl";

/* Set the IP to the Linux Gateway's reachable LAN address, not localhost. */
char g_timewise_gateway_url[128] = "HTTP_GATEWAY_URL";
/* Replace using the token from /etc/timewise-gateway.env. Do not commit a real token. */
char g_timewise_device_token[65] = "DEVICE_TOKEN";

#define HTTP_POLL_INTERVAL_MS       2000U
#define HTTP_STATE_INTERVAL_MS      6000U
#define HTTP_REQUEST_TIMEOUT_MS    3000
#define HTTP_TASK_STACK_SIZE       6144U
#define HTTP_RESPONSE_CAPACITY     8192U

static TaskHandle_t s_http_task = NULL;
static volatile bool s_running = false;
static volatile bool s_music_enabled = false;
static http_music_action_callback_t s_music_callback = NULL;
static void *s_music_callback_user_data = NULL;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} http_response_t;

static int clamp_int(int value, int lower, int upper)
{
    return value < lower ? lower : (value > upper ? upper : value);
}

static esp_err_t receive_event(esp_http_client_event_t *event)
{
    if (event == NULL || event->event_id != HTTP_EVENT_ON_DATA ||
        event->user_data == NULL || event->data == NULL || event->data_len <= 0)
        return ESP_OK;
    http_response_t *r = (http_response_t *)event->user_data;
    if (r->data == NULL || r->capacity == 0U) return ESP_OK;
    size_t count = (size_t)event->data_len;
    if (r->length >= r->capacity || count > r->capacity - r->length - 1U) {
        r->overflow = true;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(r->data + r->length, event->data, count);
    r->length += count;
    r->data[r->length] = '\0';
    return ESP_OK;
}

static esp_err_t gateway_request(const char *path, esp_http_client_method_t method,
                                 const char *request_body, char *response,
                                 size_t response_capacity)
{
    char url[192];
    int n = snprintf(url, sizeof(url), "%s%s", g_timewise_gateway_url, path);
    if (n <= 0 || (size_t)n >= sizeof(url)) return ESP_ERR_INVALID_SIZE;

    http_response_t result = { .data = response, .capacity = response_capacity,
                               .length = 0, .overflow = false };
    if (response != NULL && response_capacity != 0U) response[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = HTTP_REQUEST_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .event_handler = receive_event,
        .user_data = &result
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "X-Device-Token", g_timewise_device_token);
    if (request_body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));
    }
    esp_err_t error = esp_http_client_perform(client);
    int code = error == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (error == ESP_OK && result.overflow) error = ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK && code != 200) {
        ESP_LOGW(TAG, "%s: HTTP %d", path, code);
        error = ESP_FAIL;
    }
    return error;
}

static esp_err_t music_action(http_music_action_t action, int32_t value)
{
    if (s_music_callback == NULL) return ESP_ERR_NOT_SUPPORTED;
    return s_music_callback(action, value, s_music_callback_user_data);
}

static esp_err_t apply_command(const cJSON *command)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(command, "cmd");
    if (!cJSON_IsString(name) || name->valuestring == NULL) return ESP_ERR_INVALID_ARG;
    const char *cmd = name->valuestring;
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(command, "value");

    if (strcmp(cmd, "music_enable") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(command, "enabled");
        if (!cJSON_IsBool(enabled)) return ESP_ERR_INVALID_ARG;
        bool next = cJSON_IsTrue(enabled);
        esp_err_t error = music_action(HTTP_MUSIC_ACTION_ENABLE, next ? 1 : 0);
        if (error == ESP_OK) s_music_enabled = next;
        return error;
    }
    if (strcmp(cmd, "music_volume") == 0) {
        if (!cJSON_IsNumber(value)) return ESP_ERR_INVALID_ARG;
        audio_set_volume_percent((uint8_t)clamp_int(value->valueint, 0, 100));
        return ESP_OK;
    }
    if (strcmp(cmd, "music_stop") == 0) return music_action(HTTP_MUSIC_ACTION_STOP, 0);
    if (strcmp(cmd, "music_pause") == 0) return s_music_enabled ? music_action(HTTP_MUSIC_ACTION_PAUSE, 0) : ESP_ERR_INVALID_STATE;
    if (strcmp(cmd, "music_resume") == 0) return s_music_enabled ? music_action(HTTP_MUSIC_ACTION_RESUME, 0) : ESP_ERR_INVALID_STATE;
    if (strcmp(cmd, "music_next") == 0) return s_music_enabled ? music_action(HTTP_MUSIC_ACTION_NEXT, 0) : ESP_ERR_INVALID_STATE;
    if (strcmp(cmd, "music_previous") == 0) return s_music_enabled ? music_action(HTTP_MUSIC_ACTION_PREVIOUS, 0) : ESP_ERR_INVALID_STATE;
    if (strcmp(cmd, "music_play_random") == 0) return s_music_enabled ? music_action(HTTP_MUSIC_ACTION_PLAY_RANDOM, 0) : ESP_ERR_INVALID_STATE;

    /* While Music Mode is enabled, ignore clock/alarm/display commands. */
    if (s_music_enabled) return ESP_ERR_INVALID_STATE;
    if (strcmp(cmd, "volume") == 0) {
        if (!cJSON_IsNumber(value)) return ESP_ERR_INVALID_ARG;
        audio_set_volume_percent((uint8_t)clamp_int(value->valueint, 0, 100));
        return ESP_OK;
    }
    if (strcmp(cmd, "brightness") == 0) {
        if (!cJSON_IsNumber(value)) return ESP_ERR_INVALID_ARG;
        tft_display_brightness(g_display_is_day,
                               (uint8_t)clamp_int(value->valueint, 0, 100), g_ntp_hour);
        return ESP_OK;
    }
    if (strcmp(cmd, "alarm") == 0) {
        const cJSON *hour = cJSON_GetObjectItemCaseSensitive(command, "hour");
        const cJSON *minute = cJSON_GetObjectItemCaseSensitive(command, "minute");
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(command, "enabled");
        if (!cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) || !cJSON_IsBool(enabled))
            return ESP_ERR_INVALID_ARG;
        return ntpclock_set_alarm(hour->valueint, minute->valueint, cJSON_IsTrue(enabled));
    }
    if (strcmp(cmd, "snooze") == 0) {
        const cJSON *minutes = cJSON_GetObjectItemCaseSensitive(command, "minutes");
        uint32_t m = cJSON_IsNumber(minutes) ?
            (uint32_t)clamp_int(minutes->valueint, 1, 60) : g_alarm_snooze_minutes;
        return ntpclock_snooze_alarm(m);
    }
    if (strcmp(cmd, "stop") == 0) {
        ntpclock_stop_alarm();
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Unsupported command: %s", cmd);
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t fetch_commands(void)
{
    char *buffer = malloc(HTTP_RESPONSE_CAPACITY);
    if (buffer == NULL) return ESP_ERR_NO_MEM;
    esp_err_t error = gateway_request("/device/commands", HTTP_METHOD_GET,
                                      NULL, buffer, HTTP_RESPONSE_CAPACITY);
    if (error == ESP_OK) {
        cJSON *root = cJSON_Parse(buffer);
        if (root == NULL) error = ESP_ERR_INVALID_RESPONSE;
        else {
            const cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
            if (!cJSON_IsArray(commands)) error = ESP_ERR_INVALID_RESPONSE;
            else {
                const cJSON *entry = NULL;
                cJSON_ArrayForEach(entry, commands) {
                    esp_err_t applied = apply_command(entry);
                    if (applied != ESP_OK) {
                        ESP_LOGW(TAG, "Command failed: %s", esp_err_to_name(applied));
                    }
                }
            }
            cJSON_Delete(root);
        }
    }
    free(buffer);
    return error;
}

static esp_err_t post_state(void)
{
    int hour = 0, minute = 0;
    bool enabled = false;
    ntpclock_get_alarm(&hour, &minute, &enabled);
    music_stream_song_t song = {0};
    music_stream_get_current_song(&song);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "type", "state");
    cJSON_AddBoolToObject(root, "music_enabled", s_music_enabled);
    cJSON_AddStringToObject(root, "music_state", music_stream_state_to_string(music_stream_get_state()));
    cJSON_AddBoolToObject(root, "music_playing", music_stream_is_busy());
    cJSON_AddBoolToObject(root, "music_paused", music_stream_is_paused());
    cJSON_AddStringToObject(root, "song_id", song.id);
    cJSON_AddStringToObject(root, "song_title", song.title);
    cJSON_AddStringToObject(root, "artist", song.artist);
    cJSON_AddStringToObject(root, "album", song.album);
    cJSON_AddNumberToObject(root, "duration", song.duration_seconds);
    cJSON_AddNumberToObject(root, "position", music_stream_get_position_seconds());
    cJSON_AddNumberToObject(root, "music_volume", audio_get_volume_percent());
    cJSON_AddBoolToObject(root, "music_only_server", s_music_enabled);
    cJSON_AddNumberToObject(root, "volume", audio_get_volume_percent());
    cJSON_AddNumberToObject(root, "gain_q8", audio_get_gain_q8());
    cJSON_AddNumberToObject(root, "brightness", g_display_brightness_percent);
    cJSON_AddNumberToObject(root, "effective_brightness",
                             (!g_display_is_day && g_ntp_hour < 6U) ? 10U : g_display_brightness_percent);
    cJSON_AddBoolToObject(root, "is_day", g_display_is_day);
    cJSON_AddNumberToObject(root, "alarm_hour", hour);
    cJSON_AddNumberToObject(root, "alarm_minute", minute);
    cJSON_AddBoolToObject(root, "alarm_enabled", enabled);
    cJSON_AddBoolToObject(root, "alarm_ringing", ntpclock_alarm_is_ringing());
    cJSON_AddNumberToObject(root, "snooze_minutes", g_alarm_snooze_minutes);
    cJSON_AddBoolToObject(root, "snooze_pending", ntpclock_snooze_is_pending());
    cJSON_AddNumberToObject(root, "snooze_until", (double)ntpclock_get_snooze_until());

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return ESP_ERR_NO_MEM;
    esp_err_t error = gateway_request("/device/state", HTTP_METHOD_POST, body, NULL, 0);
    cJSON_free(body);
    return error;
}

static void http_control_task(void *unused)
{
    (void)unused;
    TickType_t last_report = 0;
    while (s_running) {
        esp_err_t error = fetch_commands();
        if (error != ESP_OK) ESP_LOGW(TAG, "Command poll: %s", esp_err_to_name(error));

        TickType_t now = xTaskGetTickCount();
        if (last_report == 0 || (TickType_t)(now - last_report) >= pdMS_TO_TICKS(HTTP_STATE_INTERVAL_MS)) {
            error = post_state();
            if (error != ESP_OK) ESP_LOGW(TAG, "State report: %s", esp_err_to_name(error));
            last_report = now;
        }
        vTaskDelay(pdMS_TO_TICKS(HTTP_POLL_INTERVAL_MS));
    }
    s_http_task = NULL;
    vTaskDelete(NULL);
}

void http_control_set_music_action_callback(http_music_action_callback_t callback, void *user_data)
{
    s_music_callback = callback;
    s_music_callback_user_data = user_data;
}

bool http_control_get_music_enabled(void) { return s_music_enabled; }

esp_err_t http_control_start(void)
{
    if (s_running) return ESP_OK;
    if (g_timewise_device_token[0] == '\0' ||
        strcmp(g_timewise_device_token, "REPLACE_WITH_DEVICE_TOKEN") == 0) {
        ESP_LOGE(TAG, "Configure the gateway device token before starting");
        return ESP_ERR_INVALID_STATE;
    }
    s_running = true;
    if (xTaskCreatePinnedToCore(http_control_task, "http_control",
                                HTTP_TASK_STACK_SIZE, NULL, 3, &s_http_task, 0) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void http_control_stop(void)
{
    /* Async stop; the task will exit after any in-progress HTTP operation. */
    s_running = false;
}

esp_err_t http_control_enter_music_mode(void) { return ESP_OK; }
esp_err_t http_control_exit_music_mode(void) { return ESP_OK; }
