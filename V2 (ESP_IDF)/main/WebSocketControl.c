#include "WebSocketControl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "Audio.h"
#include "MusicPlayer.h"
#include "NTPClock.h"
#include "TFT_Display.h"

static const char *TAG = "WebSocketControl";

#define WEBSOCKET_SERVER_PORT                 80
#define WEBSOCKET_MAX_MESSAGE_LENGTH          512U
#define WEBSOCKET_MUSIC_MAX_MESSAGE_LENGTH    160U
#define WEBSOCKET_MUSIC_STATE_BUFFER_SIZE     768U

/*
 * Normal mode serves the complete TimeWise page.
 * Music Enable=1 keeps only a lightweight music-control server alive so
 * normal TimeWise services do not consume RAM while MusicPlayer is enabled.
 */
#define WEBSOCKET_SERVER_STACK_SIZE           6144U
#define WEBSOCKET_MUSIC_SERVER_STACK_SIZE     3584U

#define WEBSITE_FILE_BUFFER_SIZE              512U

#define WEBSITE_INDEX_PATH                    "/sdcard/WEB/index.htm"
#define WEBSITE_DRAW_PATH                     "/sdcard/WEB/DRAW.png"
#define WEBSITE_LOGO_PATH                     "/sdcard/WEB/Logo.png"

static httpd_handle_t s_http_server = NULL;
static bool s_music_only_server = false;

/* Master operating-mode switch: false = normal TimeWise, true = music only. */
static volatile bool s_music_enabled = false;

static websocket_music_action_callback_t s_music_action_callback = NULL;
static void *s_music_action_user_data = NULL;

/* -------------------------------------------------------------------------- */
/*                              General helpers                               */
/* -------------------------------------------------------------------------- */

static int websocket_clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum)
    {
        return minimum;
    }

    if (value > maximum)
    {
        return maximum;
    }

    return value;
}

static esp_err_t websocket_invoke_music_action(
    websocket_music_action_t action,
    int32_t value)
{
    if (s_music_action_callback == NULL)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return s_music_action_callback(
        action,
        value,
        s_music_action_user_data
    );
}

/* -------------------------------------------------------------------------- */
/*                          SD website file serving                           */
/* -------------------------------------------------------------------------- */

static esp_err_t website_send_file(httpd_req_t *request,
                                   const char *file_path,
                                   const char *content_type)
{
    if (request == NULL ||
        file_path == NULL ||
        content_type == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(file_path, "rb");

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Website file not found: %s", file_path);

        return httpd_resp_send_err(
            request,
            HTTPD_404_NOT_FOUND,
            "Website file not found on SD card"
        );
    }

    httpd_resp_set_type(request, content_type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(request, "Pragma", "no-cache");
    httpd_resp_set_hdr(request, "Expires", "0");

    char file_buffer[WEBSITE_FILE_BUFFER_SIZE];
    esp_err_t result = ESP_OK;

    while (true)
    {
        size_t bytes_read = fread(
            file_buffer,
            1U,
            sizeof(file_buffer),
            file
        );

        if (bytes_read > 0U)
        {
            result = httpd_resp_send_chunk(
                request,
                file_buffer,
                bytes_read
            );

            if (result != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to send website file: %s", file_path);
                break;
            }
        }

        if (bytes_read < sizeof(file_buffer))
        {
            if (ferror(file))
            {
                result = ESP_FAIL;
            }

            break;
        }
    }

    fclose(file);

    if (result != ESP_OK)
    {
        return result;
    }

    return httpd_resp_send_chunk(
        request,
        NULL,
        0U
    );
}


static void website_check_file(const char *file_path)
{
    if (file_path == NULL)
    {
        return;
    }

    FILE *file = fopen(file_path, "rb");

    if (file == NULL)
    {
        ESP_LOGE(TAG, "WEB FILE NOT FOUND: %s", file_path);
        return;
    }

    long file_size = -1;

    if (fseek(file, 0, SEEK_END) == 0)
    {
        file_size = ftell(file);
    }

    fclose(file);

    if (file_size >= 0)
    {
        ESP_LOGI(TAG, "WEB FILE FOUND: %s (%ld bytes)", file_path, file_size);
    }
    else
    {
        ESP_LOGI(TAG, "WEB FILE FOUND: %s", file_path);
    }
}

static void website_check_files(void)
{
    ESP_LOGI(TAG, "Checking website files on SD card...");

    website_check_file(WEBSITE_INDEX_PATH);
    website_check_file(WEBSITE_DRAW_PATH);
    website_check_file(WEBSITE_LOGO_PATH);
}

static esp_err_t website_index_handler(httpd_req_t *request)
{
    return website_send_file(
        request,
        WEBSITE_INDEX_PATH,
        "text/html; charset=utf-8"
    );
}

static esp_err_t website_DRAW_handler(httpd_req_t *request)
{
    return website_send_file(
        request,
        WEBSITE_DRAW_PATH,
        "image/png"
    );
}

static esp_err_t website_Logo_handler(httpd_req_t *request)
{
    return website_send_file(
        request,
        WEBSITE_LOGO_PATH,
        "image/png"
    );
}

/* -------------------------------------------------------------------------- */
/*                              WebSocket JSON                                */
/* -------------------------------------------------------------------------- */

static esp_err_t websocket_send_json(httpd_req_t *request, cJSON *root)
{
    if (request == NULL || root == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char *json_text = cJSON_PrintUnformatted(root);

    if (json_text == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    httpd_ws_frame_t frame = {0};

    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)json_text;
    frame.len = strlen(json_text);

    esp_err_t result = httpd_ws_send_frame(
        request,
        &frame
    );

    cJSON_free(json_text);

    return result;
}

static esp_err_t websocket_send_error(httpd_req_t *request,
                                      const char *message)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(
        root,
        "message",
        message != NULL ? message : "Unknown error"
    );

    esp_err_t result = websocket_send_json(
        request,
        root
    );

    cJSON_Delete(root);

    return result;
}

static esp_err_t websocket_send_text(httpd_req_t *request, const char *text)
{
    if (request == NULL || text == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_ws_frame_t frame = {0};

    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)text;
    frame.len = strlen(text);

    return httpd_ws_send_frame(
        request,
        &frame
    );
}

static bool websocket_buffer_append(char *buffer,
                                    size_t buffer_size,
                                    size_t *length,
                                    const char *text)
{
    if (buffer == NULL ||
        length == NULL ||
        text == NULL ||
        *length >= buffer_size)
    {
        return false;
    }

    size_t remaining = buffer_size - *length;
    int written = snprintf(
        &buffer[*length],
        remaining,
        "%s",
        text
    );

    if (written < 0 ||
        (size_t)written >= remaining)
    {
        return false;
    }

    *length += (size_t)written;
    return true;
}

static bool websocket_buffer_append_json_string(char *buffer,
                                                size_t buffer_size,
                                                size_t *length,
                                                const char *text)
{
    if (buffer == NULL ||
        length == NULL ||
        text == NULL)
    {
        return false;
    }

    for (size_t index = 0U; text[index] != '\0'; index++)
    {
        const char *escaped = NULL;
        char character_text[2] = {text[index], '\0'};

        switch (text[index])
        {
            case '"':
                escaped = "\\\"";
                break;

            case '\\':
                escaped = "\\\\";
                break;

            case '\n':
                escaped = "\\n";
                break;

            case '\r':
                escaped = "\\r";
                break;

            case '\t':
                escaped = "\\t";
                break;

            default:
                if ((unsigned char)text[index] < 0x20U)
                {
                    character_text[0] = ' ';
                }

                escaped = character_text;
                break;
        }

        if (!websocket_buffer_append(
                buffer,
                buffer_size,
                length,
                escaped))
        {
            return false;
        }
    }

    return true;
}

static esp_err_t websocket_send_music_state_compact(httpd_req_t *request)
{
    navidrome_song_t song = {0};

    music_player_get_current_song(
        &song
    );

    char state_text[WEBSOCKET_MUSIC_STATE_BUFFER_SIZE];
    size_t length = 0U;

    int written = snprintf(
        state_text,
        sizeof(state_text),
        "{\"type\":\"state\","
        "\"music_enabled\":%s,"
        "\"music_state\":\"%s\","
        "\"music_playing\":%s,"
        "\"music_paused\":%s,"
        "\"song_title\":\"",
        s_music_enabled ? "true" : "false",
        music_player_state_to_string(music_player_get_state()),
        music_player_is_playing() ? "true" : "false",
        music_player_is_paused() ? "true" : "false"
    );

    if (written < 0 ||
        (size_t)written >= sizeof(state_text))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    length = (size_t)written;

    if (!websocket_buffer_append_json_string(
            state_text,
            sizeof(state_text),
            &length,
            song.title) ||
        !websocket_buffer_append(
            state_text,
            sizeof(state_text),
            &length,
            "\",\"artist\":\"") ||
        !websocket_buffer_append_json_string(
            state_text,
            sizeof(state_text),
            &length,
            song.artist) ||
        !websocket_buffer_append(
            state_text,
            sizeof(state_text),
            &length,
            "\",\"album\":\"") ||
        !websocket_buffer_append_json_string(
            state_text,
            sizeof(state_text),
            &length,
            song.album))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t remaining = sizeof(state_text) - length;

    written = snprintf(
        &state_text[length],
        remaining,
        "\",\"duration\":%lu,"
        "\"position\":%lu,"
        "\"music_volume\":%u,"
        "\"music_only_server\":true}",
        (unsigned long)song.duration_seconds,
        (unsigned long)music_player_get_position_seconds(),
        (unsigned int)audio_get_volume_percent()
    );

    if (written < 0 ||
        (size_t)written >= remaining)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return websocket_send_text(
        request,
        state_text
    );
}

static esp_err_t websocket_send_music_error_compact(httpd_req_t *request,
                                                    const char *message)
{
    char error_text[192];

    int written = snprintf(
        error_text,
        sizeof(error_text),
        "{\"type\":\"error\",\"message\":\"%s\"}",
        message != NULL ? message : "Music command failed"
    );

    if (written < 0 ||
        (size_t)written >= sizeof(error_text))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return websocket_send_text(
        request,
        error_text
    );
}

static void websocket_add_music_state(cJSON *root)
{
    navidrome_song_t song = {0};

    music_player_get_current_song(
        &song
    );

    cJSON_AddBoolToObject(
        root,
        "music_enabled",
        s_music_enabled
    );

    cJSON_AddStringToObject(
        root,
        "music_state",
        music_player_state_to_string(
            music_player_get_state()
        )
    );

    cJSON_AddBoolToObject(
        root,
        "music_playing",
        music_player_is_playing()
    );

    cJSON_AddBoolToObject(
        root,
        "music_paused",
        music_player_is_paused()
    );

    cJSON_AddStringToObject(
        root,
        "song_id",
        song.id
    );

    cJSON_AddStringToObject(
        root,
        "song_title",
        song.title
    );

    cJSON_AddStringToObject(
        root,
        "artist",
        song.artist
    );

    cJSON_AddStringToObject(
        root,
        "album",
        song.album
    );

    cJSON_AddNumberToObject(
        root,
        "duration",
        song.duration_seconds
    );

    cJSON_AddNumberToObject(
        root,
        "position",
        music_player_get_position_seconds()
    );

    cJSON_AddNumberToObject(
        root,
        "music_volume",
        audio_get_volume_percent()
    );

    cJSON_AddBoolToObject(
        root,
        "music_only_server",
        s_music_only_server
    );
}

static esp_err_t websocket_send_state(httpd_req_t *request)
{
    if (s_music_only_server)
    {
        return websocket_send_music_state_compact(request);
    }

    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(
        root,
        "type",
        "state"
    );

    websocket_add_music_state(
        root
    );

    /*
     * During playback only report the small music state. This keeps JSON
     * allocations and background work low while the MP3 decoder owns RAM.
     */
    if (!s_music_only_server)
    {
        int alarm_hour = 0;
        int alarm_minute = 0;
        bool alarm_enabled = false;

        ntpclock_get_alarm(
            &alarm_hour,
            &alarm_minute,
            &alarm_enabled
        );

        cJSON_AddNumberToObject(root, "volume", audio_get_volume_percent());
        cJSON_AddNumberToObject(root, "gain_q8", audio_get_gain_q8());
        cJSON_AddNumberToObject(root, "brightness", tft_display_get_brightness_percent());
        cJSON_AddNumberToObject(root, "effective_brightness", tft_display_get_effective_brightness_percent());
        cJSON_AddBoolToObject(root, "is_day", tft_display_is_day());

        cJSON_AddNumberToObject(root, "alarm_hour", alarm_hour);
        cJSON_AddNumberToObject(root, "alarm_minute", alarm_minute);
        cJSON_AddBoolToObject(root, "alarm_enabled", alarm_enabled);
        cJSON_AddBoolToObject(root, "alarm_ringing", ntpclock_alarm_is_ringing());

        cJSON_AddNumberToObject(root, "snooze_minutes", g_alarm_snooze_minutes);
        cJSON_AddBoolToObject(root, "snooze_pending", ntpclock_snooze_is_pending());
        cJSON_AddNumberToObject(
            root,
            "snooze_until",
            (double)ntpclock_get_snooze_until()
        );
    }

    esp_err_t result = websocket_send_json(
        request,
        root
    );

    cJSON_Delete(root);

    return result;
}

static esp_err_t websocket_music_action_command(
    httpd_req_t *request,
    websocket_music_action_t action)
{
    if (!s_music_enabled &&
        action != WEBSOCKET_MUSIC_ACTION_ENABLE &&
        action != WEBSOCKET_MUSIC_ACTION_STOP)
    {
        return websocket_send_error(
            request,
            "Music is disabled"
        );
    }

    esp_err_t result = websocket_invoke_music_action(
        action,
        0
    );

    if (result != ESP_OK)
    {
        return websocket_send_error(
            request,
            "Music command could not be completed"
        );
    }

    return websocket_send_state(
        request
    );
}

static esp_err_t websocket_process_music_text_command(httpd_req_t *request,
                                                        const char *payload)
{
    if (request == NULL || payload == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *command_start = strstr(
        payload,
        "\"cmd\":\""
    );

    if (command_start == NULL)
    {
        return websocket_send_music_error_compact(
            request,
            "Missing command"
        );
    }

    command_start += strlen("\"cmd\":\"");

    const char *command_end = strchr(
        command_start,
        '"'
    );

    if (command_end == NULL)
    {
        return websocket_send_music_error_compact(
            request,
            "Invalid command"
        );
    }

    size_t command_length = (size_t)(command_end - command_start);

    char command[32];

    if (command_length == 0U ||
        command_length >= sizeof(command))
    {
        return websocket_send_music_error_compact(
            request,
            "Invalid command"
        );
    }

    memcpy(
        command,
        command_start,
        command_length
    );

    command[command_length] = '\0';

    if (strcmp(command, "get_state") == 0)
    {
        return websocket_send_music_state_compact(
            request
        );
    }

    if (strcmp(command, "music_enable") == 0)
    {
        bool enabled = strstr(payload, "\"enabled\":true") != NULL;
        bool disabled = strstr(payload, "\"enabled\":false") != NULL;

        if (!enabled && !disabled)
        {
            return websocket_send_music_error_compact(
                request,
                "Music enabled value is missing"
            );
        }

        bool previous_enabled =
            s_music_enabled;

        /*
         * Music Enable is the master operating-mode flag. Update it before
         * invoking the application callback so main.c can observe the new
         * mode immediately. Roll it back if the callback rejects the change.
         */
        s_music_enabled =
            enabled;

        esp_err_t result = websocket_invoke_music_action(
            WEBSOCKET_MUSIC_ACTION_ENABLE,
            enabled ? 1 : 0
        );

        if (result != ESP_OK)
        {
            s_music_enabled =
                previous_enabled;

            return websocket_send_music_error_compact(
                request,
                "Could not change music enabled state"
            );
        }

        ESP_LOGI(
            TAG,
            "Music Enable changed to %d",
            enabled ? 1 : 0
        );

        return websocket_send_music_state_compact(
            request
        );
    }

    if (strcmp(command, "music_volume") == 0)
    {
        const char *value_start = strstr(
            payload,
            "\"value\":"
        );

        if (value_start == NULL)
        {
            return websocket_send_music_error_compact(
                request,
                "Music volume value is missing"
            );
        }

        value_start += strlen("\"value\":");

        int volume_percent = websocket_clamp_int(
            atoi(value_start),
            0,
            100
        );

        audio_set_volume_percent(
            (uint8_t)volume_percent
        );

        return websocket_send_music_state_compact(
            request
        );
    }

    websocket_music_action_t action;

    if (strcmp(command, "music_play_random") == 0)
    {
        action = WEBSOCKET_MUSIC_ACTION_PLAY_RANDOM;
    }
    else if (strcmp(command, "music_pause") == 0)
    {
        action = WEBSOCKET_MUSIC_ACTION_PAUSE;
    }
    else if (strcmp(command, "music_resume") == 0)
    {
        action = WEBSOCKET_MUSIC_ACTION_RESUME;
    }
    else if (strcmp(command, "music_next") == 0)
    {
        action = WEBSOCKET_MUSIC_ACTION_NEXT;
    }
    else if (strcmp(command, "music_previous") == 0)
    {
        action = WEBSOCKET_MUSIC_ACTION_PREVIOUS;
    }
    else if (strcmp(command, "music_stop") == 0)
    {
        action = WEBSOCKET_MUSIC_ACTION_STOP;
    }
    else
    {
        return websocket_send_music_error_compact(
            request,
            "Only music commands are available while Music Mode is enabled"
        );
    }

    if (!s_music_enabled &&
        action != WEBSOCKET_MUSIC_ACTION_STOP)
    {
        return websocket_send_music_error_compact(
            request,
            "Music is disabled"
        );
    }

    esp_err_t result = websocket_invoke_music_action(
        action,
        0
    );

    if (result != ESP_OK)
    {
        return websocket_send_music_error_compact(
            request,
            "Music command could not be completed"
        );
    }

    return websocket_send_music_state_compact(
        request
    );
}

static esp_err_t websocket_process_command(httpd_req_t *request,
                                           const cJSON *root)
{
    const cJSON *command_item =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "cmd"
        );

    if (!cJSON_IsString(command_item) ||
        command_item->valuestring == NULL)
    {
        return websocket_send_error(
            request,
            "Missing command"
        );
    }

    const char *command =
        command_item->valuestring;

    if (strcmp(command, "get_state") == 0)
    {
        return websocket_send_state(
            request
        );
    }

    /* ---------------------------------------------------------------------- */
    /* Music commands are valid in both normal and music-only server modes.   */
    /* ---------------------------------------------------------------------- */

    if (strcmp(command, "music_enable") == 0)
    {
        const cJSON *enabled_item =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "enabled"
            );

        if (!cJSON_IsBool(enabled_item))
        {
            return websocket_send_error(
                request,
                "Music enabled value is missing"
            );
        }

        bool enabled =
            cJSON_IsTrue(
                enabled_item
            );

        bool previous_enabled =
            s_music_enabled;

        /*
         * Treat Music Enable as the master mode switch, not as a playback
         * permission bit. main.c will enter/leave the corresponding operating
         * mode on its next scheduler pass.
         */
        s_music_enabled =
            enabled;

        esp_err_t result =
            websocket_invoke_music_action(
                WEBSOCKET_MUSIC_ACTION_ENABLE,
                enabled ? 1 : 0
            );

        if (result != ESP_OK)
        {
            s_music_enabled =
                previous_enabled;

            return websocket_send_error(
                request,
                "Could not change music enabled state"
            );
        }

        ESP_LOGI(
            TAG,
            "Music Enable changed to %d",
            enabled ? 1 : 0
        );

        return websocket_send_state(
            request
        );
    }

    if (strcmp(command, "music_play_random") == 0)
    {
        return websocket_music_action_command(
            request,
            WEBSOCKET_MUSIC_ACTION_PLAY_RANDOM
        );
    }

    if (strcmp(command, "music_pause") == 0)
    {
        return websocket_music_action_command(
            request,
            WEBSOCKET_MUSIC_ACTION_PAUSE
        );
    }

    if (strcmp(command, "music_resume") == 0)
    {
        return websocket_music_action_command(
            request,
            WEBSOCKET_MUSIC_ACTION_RESUME
        );
    }

    if (strcmp(command, "music_next") == 0)
    {
        return websocket_music_action_command(
            request,
            WEBSOCKET_MUSIC_ACTION_NEXT
        );
    }

    if (strcmp(command, "music_previous") == 0)
    {
        return websocket_music_action_command(
            request,
            WEBSOCKET_MUSIC_ACTION_PREVIOUS
        );
    }

    if (strcmp(command, "music_stop") == 0)
    {
        return websocket_music_action_command(
            request,
            WEBSOCKET_MUSIC_ACTION_STOP
        );
    }

    if (strcmp(command, "music_volume") == 0)
    {
        const cJSON *value_item =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "value"
            );

        if (!cJSON_IsNumber(value_item))
        {
            return websocket_send_error(
                request,
                "Music volume value is missing"
            );
        }

        int volume_percent =
            websocket_clamp_int(
                value_item->valueint,
                0,
                100
            );

        audio_set_volume_percent(
            (uint8_t)volume_percent
        );

        return websocket_send_state(
            request
        );
    }

    /*
     * In music-only mode do not execute alarm/display commands. The browser
     * remains connected only for music control until playback finishes.
     */
    if (s_music_only_server)
    {
        return websocket_send_error(
            request,
            "Only music commands are available while Music Mode is enabled"
        );
    }

    /* ---------------------------------------------------------------------- */
    /* Normal TimeWise controls                                               */
    /* ---------------------------------------------------------------------- */

    if (strcmp(command, "volume") == 0)
    {
        const cJSON *value_item =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "value"
            );

        if (!cJSON_IsNumber(value_item))
        {
            return websocket_send_error(
                request,
                "Volume value is missing"
            );
        }

        int volume_percent =
            websocket_clamp_int(
                value_item->valueint,
                0,
                100
            );

        audio_set_volume_percent(
            (uint8_t)volume_percent
        );

        return websocket_send_state(
            request
        );
    }

    if (strcmp(command, "brightness") == 0)
    {
        const cJSON *value_item =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "value"
            );

        if (!cJSON_IsNumber(value_item))
        {
            return websocket_send_error(
                request,
                "Brightness value is missing"
            );
        }

        int brightness_percent =
            websocket_clamp_int(
                value_item->valueint,
                0,
                100
            );

        tft_display_set_brightness_percent(
            (uint8_t)brightness_percent
        );

        return websocket_send_state(
            request
        );
    }

    if (strcmp(command, "alarm") == 0)
    {
        const cJSON *hour_item =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "hour"
            );

        const cJSON *minute_item =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "minute"
            );

        const cJSON *enabled_item =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "enabled"
            );

        if (!cJSON_IsNumber(hour_item) ||
            !cJSON_IsNumber(minute_item) ||
            !cJSON_IsBool(enabled_item))
        {
            return websocket_send_error(
                request,
                "Alarm hour, minute, or enabled value is missing"
            );
        }

        esp_err_t result = ntpclock_set_alarm(
            hour_item->valueint,
            minute_item->valueint,
            cJSON_IsTrue(enabled_item)
        );

        if (result != ESP_OK)
        {
            return websocket_send_error(
                request,
                "Alarm time is invalid"
            );
        }

        return websocket_send_state(
            request
        );
    }

    if (strcmp(command, "snooze") == 0)
    {
        const cJSON *minutes_item =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "minutes"
            );

        uint32_t snooze_minutes =
            g_alarm_snooze_minutes;

        if (cJSON_IsNumber(minutes_item))
        {
            snooze_minutes =
                (uint32_t)websocket_clamp_int(
                    minutes_item->valueint,
                    1,
                    60
                );
        }

        esp_err_t result =
            ntpclock_snooze_alarm(
                snooze_minutes
            );

        if (result != ESP_OK)
        {
            return websocket_send_error(
                request,
                "The alarm is not currently ringing"
            );
        }

        return websocket_send_state(
            request
        );
    }

    if (strcmp(command, "stop") == 0)
    {
        ntpclock_stop_alarm();

        return websocket_send_state(
            request
        );
    }

    return websocket_send_error(
        request,
        "Unknown command"
    );
}

/* -------------------------------------------------------------------------- */
/*                            HTTP request handlers                           */
/* -------------------------------------------------------------------------- */

static esp_err_t websocket_handler(httpd_req_t *request)
{
    if (request->method == HTTP_GET)
    {
        ESP_LOGI(
            TAG,
            "WebSocket client connected, socket=%d",
            httpd_req_to_sockfd(request)
        );

        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};

    esp_err_t result = httpd_ws_recv_frame(
        request,
        &frame,
        0
    );

    if (result != ESP_OK)
    {
        return result;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE)
    {
        return ESP_OK;
    }

    if (frame.len == 0U)
    {
        if (s_music_only_server)
        {
            return websocket_send_music_error_compact(
                request,
                "Empty message"
            );
        }

        return websocket_send_error(
            request,
            "Empty message"
        );
    }

    if (s_music_only_server)
    {
        if (frame.len > WEBSOCKET_MUSIC_MAX_MESSAGE_LENGTH)
        {
            return websocket_send_music_error_compact(
                request,
                "Music message is too large"
            );
        }

        uint8_t payload[WEBSOCKET_MUSIC_MAX_MESSAGE_LENGTH + 1U];

        frame.payload = payload;

        result = httpd_ws_recv_frame(
            request,
            &frame,
            frame.len
        );

        if (result != ESP_OK)
        {
            return result;
        }

        payload[frame.len] = '\0';

        if (frame.type != HTTPD_WS_TYPE_TEXT)
        {
            return websocket_send_music_error_compact(
                request,
                "Only text messages are supported"
            );
        }

        return websocket_process_music_text_command(
            request,
            (const char *)payload
        );
    }

    if (frame.len > WEBSOCKET_MAX_MESSAGE_LENGTH)
    {
        return websocket_send_error(
            request,
            "Message is too large"
        );
    }

    uint8_t *payload = calloc(
        frame.len + 1U,
        sizeof(uint8_t)
    );

    if (payload == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    frame.payload = payload;

    result = httpd_ws_recv_frame(
        request,
        &frame,
        frame.len
    );

    if (result != ESP_OK)
    {
        free(payload);
        return result;
    }

    payload[frame.len] = '\0';

    if (frame.type != HTTPD_WS_TYPE_TEXT)
    {
        free(payload);

        return websocket_send_error(
            request,
            "Only text messages are supported"
        );
    }

    cJSON *root = cJSON_Parse(
        (const char *)payload
    );

    free(payload);

    if (root == NULL)
    {
        return websocket_send_error(
            request,
            "Invalid JSON"
        );
    }

    result = websocket_process_command(
        request,
        root
    );

    cJSON_Delete(
        root
    );

    return result;
}

/* -------------------------------------------------------------------------- */
/*                              Server start                                  */
/* -------------------------------------------------------------------------- */

static esp_err_t websocket_control_start_internal(bool music_only)
{
    if (s_http_server != NULL)
    {
        if (s_music_only_server == music_only)
        {
            return ESP_OK;
        }

        websocket_control_stop();
    }

    httpd_config_t configuration =
        HTTPD_DEFAULT_CONFIG();

    configuration.server_port =
        WEBSOCKET_SERVER_PORT;

    configuration.max_uri_handlers =
        music_only
        ? 3
        : 8;

    if (music_only)
    {
        configuration.max_open_sockets = 2;
        configuration.backlog_conn = 1;
        configuration.max_resp_headers = 4;
    }

    configuration.stack_size =
        music_only
        ? WEBSOCKET_MUSIC_SERVER_STACK_SIZE
        : WEBSOCKET_SERVER_STACK_SIZE;

    configuration.lru_purge_enable =
        true;

    configuration.recv_wait_timeout =
        10;

    configuration.send_wait_timeout =
        10;

    if (!music_only)
    {
        website_check_files();
    }

    esp_err_t result = httpd_start(
        &s_http_server,
        &configuration
    );

    if (result != ESP_OK)
    {
        s_http_server = NULL;
        return result;
    }

    s_music_only_server =
        music_only;

    static const httpd_uri_t websocket_uri =
    {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = websocket_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };

    result = httpd_register_uri_handler(
        s_http_server,
        &websocket_uri
    );

    if (result != ESP_OK)
    {
        websocket_control_stop();
        return result;
    }

    if (music_only)
    {
        /*
         * Keep the SD-card control page reachable if the user refreshes while
         * music is playing. Images are intentionally not registered in this
         * lightweight mode; the page falls back to the text TimeWise logo.
         */
        static const httpd_uri_t music_control_page_uri =
        {
            .uri = "/",
            .method = HTTP_GET,
            .handler = website_index_handler,
            .user_ctx = NULL
        };

        result = httpd_register_uri_handler(
            s_http_server,
            &music_control_page_uri
        );

        if (result == ESP_OK)
        {
            static const httpd_uri_t music_index_file_uri =
            {
                .uri = "/index.htm",
                .method = HTTP_GET,
                .handler = website_index_handler,
                .user_ctx = NULL
            };

            result = httpd_register_uri_handler(
                s_http_server,
                &music_index_file_uri
            );
        }

        if (result != ESP_OK)
        {
            websocket_control_stop();
            return result;
        }
    }
    else
    {
        static const httpd_uri_t control_page_uri =
        {
            .uri = "/",
            .method = HTTP_GET,
            .handler = website_index_handler,
            .user_ctx = NULL
        };

        static const httpd_uri_t website_DRAW_uri =
        {
            .uri = "/DRAW.png",
            .method = HTTP_GET,
            .handler = website_DRAW_handler,
            .user_ctx = NULL
        };

        static const httpd_uri_t website_Logo_uri =
        {
            .uri = "/Logo.png",
            .method = HTTP_GET,
            .handler = website_Logo_handler,
            .user_ctx = NULL
        };

        result = httpd_register_uri_handler(
            s_http_server,
            &control_page_uri
        );

        if (result == ESP_OK)
        {
            static const httpd_uri_t index_file_uri =
            {
                .uri = "/index.htm",
                .method = HTTP_GET,
                .handler = website_index_handler,
                .user_ctx = NULL
            };

            result = httpd_register_uri_handler(
                s_http_server,
                &index_file_uri
            );
        }

        if (result == ESP_OK)
        {
            result = httpd_register_uri_handler(
                s_http_server,
                &website_DRAW_uri
            );
        }

        if (result == ESP_OK)
        {
            result = httpd_register_uri_handler(
                s_http_server,
                &website_Logo_uri
            );
        }

        if (result != ESP_OK)
        {
            websocket_control_stop();
            return result;
        }
    }

    ESP_LOGI(
        TAG,
        "%s WebSocket server started on port %d, stack=%u",
        music_only ? "Music-only" : "Full",
        WEBSOCKET_SERVER_PORT,
        music_only
            ? WEBSOCKET_MUSIC_SERVER_STACK_SIZE
            : WEBSOCKET_SERVER_STACK_SIZE
    );

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                              Public API                                    */
/* -------------------------------------------------------------------------- */

void websocket_control_set_music_action_callback(
    websocket_music_action_callback_t callback,
    void *user_data)
{
    s_music_action_callback =
        callback;

    s_music_action_user_data =
        user_data;
}

bool websocket_control_get_music_enabled(void)
{
    return s_music_enabled;
}

esp_err_t websocket_control_start(void)
{
    return websocket_control_start_internal(
        false
    );
}

esp_err_t websocket_control_enter_music_mode(void)
{
    return websocket_control_start_internal(
        true
    );
}

esp_err_t websocket_control_exit_music_mode(void)
{
    return websocket_control_start_internal(
        false
    );
}

void websocket_control_stop(void)
{
    if (s_http_server == NULL)
    {
        return;
    }

    httpd_stop(
        s_http_server
    );

    s_http_server = NULL;
    s_music_only_server = false;

    ESP_LOGI(
        TAG,
        "TimeWise control server stopped"
    );
}