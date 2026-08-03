#include "WebSocketControl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "Audio.h"
#include "NTPClock.h"
#include "TFT_Display.h"

static const char *TAG = "WebSocketControl";

#define WEBSOCKET_SERVER_PORT            80
#define WEBSOCKET_MAX_MESSAGE_LENGTH     512U
#define WEBSOCKET_SERVER_STACK_SIZE      6144U

#define WEBSITE_FILE_BUFFER_SIZE         512U

#define WEBSITE_DRAW_PATH             "/sdcard/WEB/DRAW.png"
#define WEBSITE_LOGO_PATH             "/sdcard/WEB/Logo.png"

static httpd_handle_t s_http_server = NULL;

/* -------------------------------------------------------------------------- */
/*                              Website HTML                                  */
/* -------------------------------------------------------------------------- */

static const char s_control_page[] =
    "<!DOCTYPE html>"
    "<html lang='en'>"

    "<head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='theme-color' content='#0b1220'>"

        "<title>TimeWise Control</title>"

        "<link rel='icon' type='image/png' href='/DRAW.png?v=5'>"
        "<link rel='shortcut icon' type='image/png' href='/DRAW.png?v=5'>"

        "<style>"
            "*{box-sizing:border-box}"

            "body{"
                "margin:0;"
                "min-height:100vh;"
                "font-family:Arial,sans-serif;"
                "background:#0b1220;"
                "color:#eef4ff;"
            "}"

            ".page{"
                "width:min(560px,100%);"
                "margin:auto;"
                "padding:22px;"
            "}"

            ".header{"
                "display:flex;"
                "justify-content:space-between;"
                "align-items:center;"
                "gap:16px;"
                "margin-bottom:20px;"
                "min-height:68px;"
            "}"

            ".brand{"
                "display:flex;"
                "align-items:center;"
                "min-width:0;"
                "overflow:hidden;"
            "}"

            ".header-logo{"
                "display:block;"
                "width:230px;"
                "height:64px;"
                "max-width:60vw;"
                "object-fit:contain;"
                "object-position:left center;"
            "}"

            ".brand-fallback{"
                "display:none;"
                "font-size:28px;"
                "font-weight:700;"
                "color:#ffffff;"
            "}"

            ".connection{"
                "flex-shrink:0;"
                "font-size:13px;"
                "padding:7px 11px;"
                "border-radius:20px;"
                "background:#5d2630;"
                "color:#ffd7dc;"
            "}"

            ".connection.connected{"
                "background:#174d38;"
                "color:#caffdf;"
            "}"

            ".card{"
                "background:#172235;"
                "border:1px solid #263751;"
                "border-radius:18px;"
                "padding:18px;"
                "margin-bottom:15px;"
                "box-shadow:0 8px 25px rgba(0,0,0,.22);"
            "}"

            ".title{"
                "font-size:19px;"
                "font-weight:bold;"
                "margin-bottom:16px;"
            "}"

            ".row{"
                "display:flex;"
                "justify-content:space-between;"
                "align-items:center;"
                "gap:12px;"
                "margin-bottom:10px;"
            "}"

            ".value{"
                "color:#77dff5;"
                "font-weight:bold;"
            "}"

            "input[type=range]{"
                "width:100%;"
                "accent-color:#38e6ff;"
            "}"

            "input[type=time],select{"
                "width:100%;"
                "padding:13px;"
                "font-size:18px;"
                "border-radius:11px;"
                "border:1px solid #385070;"
                "background:#0d1728;"
                "color:#ffffff;"
            "}"

            "input[type=checkbox]{"
                "width:24px;"
                "height:24px;"
                "accent-color:#38e6ff;"
            "}"

            ".button-row{"
                "display:grid;"
                "grid-template-columns:1fr 1fr;"
                "gap:11px;"
                "margin-top:14px;"
            "}"

            "button{"
                "border:0;"
                "border-radius:12px;"
                "padding:14px;"
                "font-size:17px;"
                "font-weight:bold;"
                "cursor:pointer;"
            "}"

            "button:disabled{"
                "opacity:.45;"
                "cursor:not-allowed;"
            "}"

            ".snooze{"
                "background:#e4b64e;"
                "color:#171717;"
            "}"

            ".stop{"
                "background:#e65a6b;"
                "color:#ffffff;"
            "}"

            ".status{"
                "display:flex;"
                "justify-content:space-between;"
                "padding-top:12px;"
                "font-size:14px;"
                "color:#b9c8dd;"
            "}"

            ".note{"
                "font-size:12px;"
                "line-height:1.45;"
                "color:#8fa2bd;"
                "margin-top:9px;"
            "}"

            "@media(max-width:430px){"
                ".page{padding:16px;}"
                ".header{gap:10px;}"
                ".header-logo{"
                    "width:175px;"
                    "height:52px;"
                    "max-width:55vw;"
                "}"
                ".connection{"
                    "font-size:12px;"
                    "padding:6px 9px;"
                "}"
            "}"
        "</style>"
    "</head>"

    "<body>"
        "<div class='page'>"

            "<div class='header'>"
                "<div class='brand'>"
                    "<img "
                        "id='headerLogo' "
                        "class='header-logo' "
                        "src='/Logo.png' "
                        "alt='TimeWise' "
                        "draggable='false' "
                        "onerror=\""
                            "this.style.display='none';"
                            "document.getElementById('brandFallback').style.display='block';"
                        "\""
                    ">"
                    "<span id='brandFallback' class='brand-fallback'>TimeWise</span>"
                "</div>"

                "<div id='connection' class='connection'>"
                    "Disconnected"
                "</div>"
            "</div>"

            "<div class='card'>"
                "<div class='title'>Volume</div>"

                "<div class='row'>"
                    "<span>Alarm volume</span>"
                    "<span id='volumeValue' class='value'>50%</span>"
                "</div>"

                "<input "
                    "id='volume' "
                    "type='range' "
                    "min='0' "
                    "max='100' "
                    "value='50'"
                ">"
            "</div>"

            "<div class='card'>"
                "<div class='title'>Brightness</div>"

                "<div class='row'>"
                    "<span>Selected brightness</span>"
                    "<span id='brightnessValue' class='value'>100%</span>"
                "</div>"

                "<input "
                    "id='brightness' "
                    "type='range' "
                    "min='0' "
                    "max='100' "
                    "value='100'"
                ">"

                "<div class='status'>"
                    "<span>Effective brightness</span>"
                    "<span id='effectiveBrightness'>100%</span>"
                "</div>"

                "<div class='status'>"
                    "<span>Display mode</span>"
                    "<span id='dayMode'>Day</span>"
                "</div>"

                "<div class='note'>"
                    "Night mode applies 60% of the selected brightness."
                "</div>"
            "</div>"

            "<div class='card'>"
                "<div class='title'>Alarm</div>"

                "<input "
                    "id='alarmTime' "
                    "type='time' "
                    "value='07:45'"
                ">"

                "<div class='row' style='margin-top:16px'>"
                    "<span>Alarm enabled</span>"
                    "<input "
                        "id='alarmEnabled' "
                        "type='checkbox' "
                        "checked"
                    ">"
                "</div>"

                "<div class='status'>"
                    "<span>Alarm status</span>"
                    "<span id='alarmStatus'>Idle</span>"
                "</div>"

                "<div class='status'>"
                    "<span>Snooze status</span>"
                    "<span id='snoozeStatus'>Not scheduled</span>"
                "</div>"
            "</div>"

            "<div class='card'>"
                "<div class='title'>Alarm Control</div>"

                "<select id='snoozeMinutes'>"
                    "<option value='5'>Snooze 5 minutes</option>"
                    "<option value='10'>Snooze 10 minutes</option>"
                    "<option value='15'>Snooze 15 minutes</option>"
                    "<option value='30'>Snooze 30 minutes</option>"
                "</select>"

                "<div class='button-row'>"
                    "<button "
                        "id='snoozeButton' "
                        "class='snooze'"
                    ">"
                        "Snooze"
                    "</button>"

                    "<button "
                        "id='stopButton' "
                        "class='stop'"
                    ">"
                        "Stop"
                    "</button>"
                "</div>"
            "</div>"

        "</div>"

        "<script>"
            "const el=id=>document.getElementById(id);"

            "const connection=el('connection');"
            "const volume=el('volume');"
            "const volumeValue=el('volumeValue');"
            "const brightness=el('brightness');"
            "const brightnessValue=el('brightnessValue');"
            "const effectiveBrightness=el('effectiveBrightness');"
            "const dayMode=el('dayMode');"
            "const alarmTime=el('alarmTime');"
            "const alarmEnabled=el('alarmEnabled');"
            "const alarmStatus=el('alarmStatus');"
            "const snoozeStatus=el('snoozeStatus');"
            "const snoozeMinutes=el('snoozeMinutes');"
            "const snoozeButton=el('snoozeButton');"
            "const stopButton=el('stopButton');"

            "let socket=null;"
            "let volumeTimer=null;"
            "let brightnessTimer=null;"

            "function send(command){"
                "if(socket&&socket.readyState===WebSocket.OPEN){"
                    "socket.send(JSON.stringify(command));"
                "}"
            "}"

            "function setConnected(connected){"
                "connection.textContent="
                    "connected?'Connected':'Disconnected';"

                "connection.classList.toggle("
                    "'connected',"
                    "connected"
                ");"
            "}"

            "function formatSnooze(until){"
                "if(!until){"
                    "return 'Not scheduled';"
                "}"

                "return new Date(until*1000).toLocaleTimeString("
                    "[],"
                    "{hour:'2-digit',minute:'2-digit'}"
                ");"
            "}"

            "function applyState(state){"
                "if(state.type!=='state'){"
                    "return;"
                "}"

                "volume.value=state.volume;"
                "volumeValue.textContent=state.volume+'%';"

                "brightness.value=state.brightness;"
                "brightnessValue.textContent="
                    "state.brightness+'%';"

                "effectiveBrightness.textContent="
                    "state.effective_brightness+'%';"

                "dayMode.textContent="
                    "state.is_day?'Day':'Night';"

                "alarmTime.value="
                    "String(state.alarm_hour).padStart(2,'0')+"
                    "':'+"
                    "String(state.alarm_minute).padStart(2,'0');"

                "alarmEnabled.checked=state.alarm_enabled;"

                "snoozeMinutes.value="
                    "String(state.snooze_minutes);"

                "alarmStatus.textContent="
                    "state.alarm_ringing?'Ringing':'Idle';"

                "snoozeStatus.textContent="
                    "state.snooze_pending?"
                    "'Until '+formatSnooze(state.snooze_until):"
                    "'Not scheduled';"

                "snoozeButton.disabled="
                    "!state.alarm_ringing;"

                "stopButton.disabled="
                    "!state.alarm_ringing&&"
                    "!state.snooze_pending;"
            "}"

            "function connect(){"
                "const protocol="
                    "location.protocol==='https:'?"
                    "'wss':'ws';"

                "socket=new WebSocket("
                    "protocol+'://'+location.host+'/ws'"
                ");"

                "socket.onopen=()=>{"
                    "setConnected(true);"
                    "send({cmd:'get_state'});"
                "};"

                "socket.onmessage=event=>{"
                    "try{"
                        "const message=JSON.parse(event.data);"

                        "if(message.type==='error'){"
                            "console.error(message.message);"
                            "return;"
                        "}"

                        "applyState(message);"
                    "}"
                    "catch(error){"
                        "console.error(error);"
                    "}"
                "};"

                "socket.onerror=()=>{"
                    "setConnected(false);"
                "};"

                "socket.onclose=()=>{"
                    "setConnected(false);"
                    "setTimeout(connect,1500);"
                "};"
            "}"

            "volume.addEventListener('input',()=>{"
                "volumeValue.textContent=volume.value+'%';"

                "clearTimeout(volumeTimer);"

                "volumeTimer=setTimeout(()=>{"
                    "send({"
                        "cmd:'volume',"
                        "value:Number(volume.value)"
                    "});"
                "},100);"
            "});"

            "brightness.addEventListener('input',()=>{"
                "brightnessValue.textContent="
                    "brightness.value+'%';"

                "clearTimeout(brightnessTimer);"

                "brightnessTimer=setTimeout(()=>{"
                    "send({"
                        "cmd:'brightness',"
                        "value:Number(brightness.value)"
                    "});"
                "},100);"
            "});"

            "function sendAlarm(){"
                "const parts=alarmTime.value.split(':');"

                "if(parts.length!==2){"
                    "return;"
                "}"

                "send({"
                    "cmd:'alarm',"
                    "hour:Number(parts[0]),"
                    "minute:Number(parts[1]),"
                    "enabled:alarmEnabled.checked"
                "});"
            "}"

            "alarmTime.addEventListener("
                "'change',"
                "sendAlarm"
            ");"

            "alarmEnabled.addEventListener("
                "'change',"
                "sendAlarm"
            ");"

            "snoozeButton.addEventListener('click',()=>{"
                "send({"
                    "cmd:'snooze',"
                    "minutes:Number(snoozeMinutes.value)"
                "});"
            "});"

            "stopButton.addEventListener('click',()=>{"
                "send({cmd:'stop'});"
            "});"

            "setInterval(()=>{"
                "send({cmd:'get_state'});"
            "},1000);"

            "connect();"
        "</script>"
    "</body>"

    "</html>";

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
        ESP_LOGE(
            TAG,
            "Website file not found: %s",
            file_path
        );

        return httpd_resp_send_err(
            request,
            HTTPD_404_NOT_FOUND,
            "Website image not found on SD card"
        );
    }

    httpd_resp_set_type(
        request,
        content_type
    );

    /*
     * Disable caching while developing.
     *
     * This is especially useful for DRAWs because browsers often keep
     * an old DRAW cached even after the file has been replaced.
     */
    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-cache, no-store, must-revalidate"
    );

    httpd_resp_set_hdr(
        request,
        "Pragma",
        "no-cache"
    );

    httpd_resp_set_hdr(
        request,
        "Expires",
        "0"
    );

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
                ESP_LOGE(
                    TAG,
                    "Failed to send website file: %s",
                    file_path
                );

                break;
            }
        }

        if (bytes_read < sizeof(file_buffer))
        {
            if (ferror(file))
            {
                ESP_LOGE(
                    TAG,
                    "Failed while reading website file: %s",
                    file_path
                );

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

    /*
     * An empty chunk marks the end of a chunked HTTP response.
     */
    result = httpd_resp_send_chunk(
        request,
        NULL,
        0U
    );

    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Website file served: %s",
            file_path
        );
    }

    return result;
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

    cJSON_AddStringToObject(
        root,
        "type",
        "error"
    );

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

static esp_err_t websocket_send_state(httpd_req_t *request)
{
    int alarm_hour;
    int alarm_minute;
    bool alarm_enabled;

    ntpclock_get_alarm(
        &alarm_hour,
        &alarm_minute,
        &alarm_enabled
    );

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

    cJSON_AddNumberToObject(
        root,
        "volume",
        audio_get_volume_percent()
    );

    cJSON_AddNumberToObject(
        root,
        "gain_q8",
        audio_get_gain_q8()
    );

    cJSON_AddNumberToObject(
        root,
        "brightness",
        tft_display_get_brightness_percent()
    );

    cJSON_AddNumberToObject(
        root,
        "effective_brightness",
        tft_display_get_effective_brightness_percent()
    );

    cJSON_AddBoolToObject(
        root,
        "is_day",
        tft_display_is_day()
    );

    cJSON_AddNumberToObject(
        root,
        "alarm_hour",
        alarm_hour
    );

    cJSON_AddNumberToObject(
        root,
        "alarm_minute",
        alarm_minute
    );

    cJSON_AddBoolToObject(
        root,
        "alarm_enabled",
        alarm_enabled
    );

    cJSON_AddBoolToObject(
        root,
        "alarm_ringing",
        ntpclock_alarm_is_ringing()
    );

    cJSON_AddNumberToObject(
        root,
        "snooze_minutes",
        g_alarm_snooze_minutes
    );

    cJSON_AddBoolToObject(
        root,
        "snooze_pending",
        ntpclock_snooze_is_pending()
    );

    cJSON_AddNumberToObject(
        root,
        "snooze_until",
        (double)ntpclock_get_snooze_until()
    );

    esp_err_t result = websocket_send_json(
        request,
        root
    );

    cJSON_Delete(root);

    return result;
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
        return websocket_send_state(request);
    }

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

        ESP_LOGI(
            TAG,
            "Volume changed to %d%%",
            volume_percent
        );

        return websocket_send_state(request);
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

        ESP_LOGI(
            TAG,
            "Brightness changed to %d%%",
            brightness_percent
        );

        return websocket_send_state(request);
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

        int alarm_hour =
            hour_item->valueint;

        int alarm_minute =
            minute_item->valueint;

        bool alarm_enabled =
            cJSON_IsTrue(enabled_item);

        esp_err_t result = ntpclock_set_alarm(
            alarm_hour,
            alarm_minute,
            alarm_enabled
        );

        if (result != ESP_OK)
        {
            return websocket_send_error(
                request,
                "Alarm time is invalid"
            );
        }

        return websocket_send_state(request);
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

        return websocket_send_state(request);
    }

    if (strcmp(command, "stop") == 0)
    {
        ntpclock_stop_alarm();

        return websocket_send_state(request);
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
    /*
     * The first HTTP GET is the WebSocket upgrade handshake.
     */
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
        ESP_LOGE(
            TAG,
            "Could not read WebSocket frame length: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE)
    {
        ESP_LOGI(
            TAG,
            "WebSocket client disconnected"
        );

        return ESP_OK;
    }

    if (frame.len == 0U)
    {
        return websocket_send_error(
            request,
            "Empty message"
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

    cJSON *root =
        cJSON_Parse((const char *)payload);

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

    cJSON_Delete(root);

    return result;
}

static esp_err_t control_page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(
        request,
        "text/html"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        request,
        s_control_page,
        HTTPD_RESP_USE_STRLEN
    );
}

/* -------------------------------------------------------------------------- */
/*                              Public API                                    */
/* -------------------------------------------------------------------------- */

esp_err_t websocket_control_start(void)
{
    if (s_http_server != NULL)
    {
        return ESP_OK;
    }

    httpd_config_t configuration =
        HTTPD_DEFAULT_CONFIG();

    configuration.server_port =
        WEBSOCKET_SERVER_PORT;

    configuration.max_uri_handlers =
        8;

    configuration.stack_size =
        WEBSOCKET_SERVER_STACK_SIZE;

    configuration.lru_purge_enable =
        true;

    configuration.recv_wait_timeout =
        10;

    configuration.send_wait_timeout =
        10;

    esp_err_t result = httpd_start(
        &s_http_server,
        &configuration
    );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "HTTP server start failed: %s",
            esp_err_to_name(result)
        );

        s_http_server = NULL;

        return result;
    }

    static const httpd_uri_t control_page_uri =
    {
        .uri = "/",
        .method = HTTP_GET,
        .handler = control_page_handler,
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
        &control_page_uri
    );

    if (result != ESP_OK)
    {
        websocket_control_stop();

        return result;
    }

    result = httpd_register_uri_handler(
        s_http_server,
        &website_DRAW_uri
    );

    if (result != ESP_OK)
    {
        websocket_control_stop();

        return result;
    }

    result = httpd_register_uri_handler(
        s_http_server,
        &website_Logo_uri
    );

    if (result != ESP_OK)
    {
        websocket_control_stop();

        return result;
    }

    result = httpd_register_uri_handler(
        s_http_server,
        &websocket_uri
    );

    if (result != ESP_OK)
    {
        websocket_control_stop();

        return result;
    }

    ESP_LOGI(
        TAG,
        "TimeWise control server started on port %d",
        WEBSOCKET_SERVER_PORT
    );

    ESP_LOGI(
        TAG,
        "DRAW path: %s",
        WEBSITE_DRAW_PATH
    );

    ESP_LOGI(
        TAG,
        "Header logo path: %s",
        WEBSITE_LOGO_PATH
    );

    return ESP_OK;
}

void websocket_control_stop(void)
{
    if (s_http_server == NULL)
    {
        return;
    }

    httpd_stop(s_http_server);

    s_http_server = NULL;

    ESP_LOGI(
        TAG,
        "TimeWise control server stopped"
    );
}