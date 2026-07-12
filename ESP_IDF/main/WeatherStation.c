#include "WeatherStation.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "cJSON.h"

#define WEATHER_RESPONSE_BUFFER_SIZE       4096
#define WEATHER_NORMAL_UPDATE_MS           (30 * 60 * 1000)
#define WEATHER_ERROR_RETRY_MS              (60 * 1000)

/* -------------------------------------------------------------------------- */
/*                         Weather background paths                           */
/* -------------------------------------------------------------------------- */

#define BG_DEFAULT          "S:/BG/DEF.BIN"
#define BG_SUNNY            "S:/BG/SUNNY.BIN"
#define BG_NIGHT            "S:/BG/NIGHT.BIN"
#define BG_CLOUD            "S:/BG/CLOUD.BIN"
#define BG_RAIN             "S:/BG/RAIN.BIN"
#define BG_FREEZING_RAIN    "S:/BG/FRAIN.BIN"
#define BG_DRIZZLE          "S:/BG/DRIZZ.BIN"
#define BG_SNOW             "S:/BG/SNOW.BIN"
#define BG_DARK_SNOW        "S:/BG/DSNOW.BIN"
#define BG_THUNDER          "S:/BG/THUND.BIN"
#define BG_RAIN_THUNDER     "S:/BG/RTHUND.BIN"
#define BG_THUNDER_SNOW     "S:/BG/TSNOW.BIN"
#define BG_FOG              "S:/BG/FOG.BIN"
#define BG_MIST             "S:/BG/MIST.BIN"
#define BG_ICE              "S:/BG/ICE.BIN"
#define BG_DUST             "S:/BG/DUST.BIN"
#define BG_BLIZZARD         "S:/BG/BLIZZ.BIN"

/* -------------------------------------------------------------------------- */
/*                           Weather icon paths                               */
/* -------------------------------------------------------------------------- */

#define ICON_SUNNY          "S:/ICO/SUNNY.BIN"
#define ICON_NIGHT          "S:/ICO/NIGHT.BIN"
#define ICON_CLOUD          "S:/ICO/CLOUD.BIN"
#define ICON_MIST           "S:/ICO/MIST.BIN"
#define ICON_FOG            "S:/ICO/FOG.BIN"
#define ICON_SMOKE          "S:/ICO/SMOKE.BIN"
#define ICON_DUST           "S:/ICO/DUST.BIN"
#define ICON_LIGHT_RAIN     "S:/ICO/LRAIN.BIN"
#define ICON_FREEZE_RAIN    "S:/ICO/FRAIN.BIN"
#define ICON_HEAVY_RAIN     "S:/ICO/HRAIN.BIN"
#define ICON_THUNDER        "S:/ICO/THUNDP.BIN"
#define ICON_THUNDER_RAIN   "S:/ICO/TRAIN.BIN"
#define ICON_THUNDER_SNOW   "S:/ICO/HTSNOW.BIN"
#define ICON_LIGHT_SNOW     "S:/ICO/LSNOW.BIN"
#define ICON_SNOW           "S:/ICO/SNOW.BIN"
#define ICON_HEAVY_SNOW     "S:/ICO/HSNOW.BIN"
#define ICON_BLIZZARD       "S:/ICO/BLIZZ.BIN"
#define ICON_SLEET          "S:/ICO/SLEET.BIN"
#define ICON_ICE            "S:/ICO/ICE.BIN"

static const char *TAG = "WeatherStation";

static char s_response_buffer[WEATHER_RESPONSE_BUFFER_SIZE];
static size_t s_response_length = 0;
static bool s_response_overflow = false;

static const char *s_api_key = NULL;
static const char *s_location_query = NULL;

static weatherstation_update_callback_t s_update_callback = NULL;
static void *s_callback_user_data = NULL;
static TaskHandle_t s_update_task_handle = NULL;

static bool weatherstation_contains_text(
    const char *text,
    const char *search_text
)
{
    if (text == NULL || search_text == NULL)
    {
        return false;
    }

    size_t search_length = strlen(search_text);

    if (search_length == 0)
    {
        return true;
    }

    for (const char *position = text;
         *position != '\0';
         position++)
    {
        if (strncasecmp(
                position,
                search_text,
                search_length) == 0)
        {
            return true;
        }
    }

    return false;
}

static const char *weatherstation_select_background(
    const char *condition,
    bool is_day
)
{
    if (condition == NULL)
    {
        return BG_DEFAULT;
    }

    if (weatherstation_contains_text(condition, "blizzard"))
    {
        return BG_BLIZZARD;
    }

    if (weatherstation_contains_text(condition, "thunder") &&
        weatherstation_contains_text(condition, "snow"))
    {
        return BG_THUNDER_SNOW;
    }

    if (weatherstation_contains_text(condition, "thunder") &&
        weatherstation_contains_text(condition, "rain"))
    {
        return BG_RAIN_THUNDER;
    }

    if (weatherstation_contains_text(condition, "thunder"))
    {
        return BG_THUNDER;
    }

    if (weatherstation_contains_text(condition, "freezing rain") ||
        weatherstation_contains_text(condition, "freezing drizzle"))
    {
        return BG_FREEZING_RAIN;
    }

    if (weatherstation_contains_text(condition, "ice") ||
        weatherstation_contains_text(condition, "sleet") ||
        weatherstation_contains_text(condition, "pellets"))
    {
        return BG_ICE;
    }

    if (weatherstation_contains_text(condition, "snow"))
    {
        return is_day ? BG_SNOW : BG_DARK_SNOW;
    }

    if (weatherstation_contains_text(condition, "drizzle"))
    {
        return BG_DRIZZLE;
    }

    if (weatherstation_contains_text(condition, "rain"))
    {
        return BG_RAIN;
    }

    if (weatherstation_contains_text(condition, "fog"))
    {
        return BG_FOG;
    }

    if (weatherstation_contains_text(condition, "mist"))
    {
        return BG_MIST;
    }

    if (weatherstation_contains_text(condition, "dust") ||
        weatherstation_contains_text(condition, "sand") ||
        weatherstation_contains_text(condition, "smoke") ||
        weatherstation_contains_text(condition, "haze"))
    {
        return BG_DUST;
    }

    if (weatherstation_contains_text(condition, "cloud") ||
        weatherstation_contains_text(condition, "overcast"))
    {
        return BG_CLOUD;
    }

    if (weatherstation_contains_text(condition, "sunny") ||
        weatherstation_contains_text(condition, "clear"))
    {
        return is_day ? BG_SUNNY : BG_NIGHT;
    }

    return BG_DEFAULT;
}

static const char *weatherstation_select_icon(
    const char *condition,
    bool is_day
)
{
    if (condition == NULL)
    {
        return NULL;
    }

    if (weatherstation_contains_text(condition, "blizzard") ||
        weatherstation_contains_text(condition, "blowing snow"))
    {
        return ICON_BLIZZARD;
    }

    if (weatherstation_contains_text(condition, "thunder") &&
        weatherstation_contains_text(condition, "snow"))
    {
        return ICON_THUNDER_SNOW;
    }

    if (weatherstation_contains_text(condition, "thunder") &&
        weatherstation_contains_text(condition, "rain"))
    {
        return ICON_THUNDER_RAIN;
    }

    if (weatherstation_contains_text(condition, "thunder"))
    {
        return ICON_THUNDER;
    }

    if (weatherstation_contains_text(condition, "freezing rain") ||
        weatherstation_contains_text(condition, "freezing drizzle"))
    {
        return ICON_FREEZE_RAIN;
    }

    if (weatherstation_contains_text(condition, "ice pellets"))
    {
        return ICON_ICE;
    }

    if (weatherstation_contains_text(condition, "sleet"))
    {
        return ICON_SLEET;
    }

    if (weatherstation_contains_text(condition, "heavy snow") ||
        weatherstation_contains_text(condition, "heavy snow shower"))
    {
        return ICON_HEAVY_SNOW;
    }

    if (weatherstation_contains_text(condition, "moderate snow"))
    {
        return ICON_SNOW;
    }

    if (weatherstation_contains_text(condition, "snow"))
    {
        return ICON_LIGHT_SNOW;
    }

    if (weatherstation_contains_text(condition, "heavy rain") ||
        weatherstation_contains_text(condition, "moderate rain") ||
        weatherstation_contains_text(condition, "torrential"))
    {
        return ICON_HEAVY_RAIN;
    }

    if (weatherstation_contains_text(condition, "rain") ||
        weatherstation_contains_text(condition, "drizzle"))
    {
        return ICON_LIGHT_RAIN;
    }

    if (weatherstation_contains_text(condition, "smoke") ||
        weatherstation_contains_text(condition, "haze"))
    {
        return ICON_SMOKE;
    }

    if (weatherstation_contains_text(condition, "dust") ||
        weatherstation_contains_text(condition, "sand"))
    {
        return ICON_DUST;
    }

    if (weatherstation_contains_text(condition, "fog"))
    {
        return ICON_FOG;
    }

    if (weatherstation_contains_text(condition, "mist"))
    {
        return ICON_MIST;
    }

    if (weatherstation_contains_text(condition, "cloud") ||
        weatherstation_contains_text(condition, "overcast"))
    {
        return ICON_CLOUD;
    }

    if (weatherstation_contains_text(condition, "sunny") ||
        weatherstation_contains_text(condition, "clear"))
    {
        return is_day ? ICON_SUNNY : ICON_NIGHT;
    }

    return NULL;
}

static esp_err_t weatherstation_http_event_handler(
    esp_http_client_event_t *event
)
{
    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (event->event_id == HTTP_EVENT_ON_DATA &&
        event->data_len > 0)
    {
        size_t available =
            WEATHER_RESPONSE_BUFFER_SIZE -
            s_response_length -
            1;

        size_t requested =
            (size_t)event->data_len;

        size_t copy_length =
            requested < available
                ? requested
                : available;

        if (copy_length > 0)
        {
            memcpy(
                s_response_buffer + s_response_length,
                event->data,
                copy_length
            );

            s_response_length += copy_length;
            s_response_buffer[s_response_length] = '\0';
        }

        if (copy_length != requested)
        {
            s_response_overflow = true;
        }
    }

    return ESP_OK;
}

static bool weatherstation_parse_and_publish(
    const char *json_text
)
{
    if (json_text == NULL || json_text[0] == '\0')
    {
        ESP_LOGE(TAG, "Weather JSON is empty");
        return false;
    }

    cJSON *root = cJSON_Parse(json_text);

    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to parse weather JSON");
        return false;
    }

    bool success = false;

    cJSON *location =
        cJSON_GetObjectItemCaseSensitive(root, "location");

    cJSON *current =
        cJSON_GetObjectItemCaseSensitive(root, "current");

    if (!cJSON_IsObject(location) ||
        !cJSON_IsObject(current))
    {
        ESP_LOGE(TAG, "Weather JSON is missing location/current");
        goto cleanup;
    }

    cJSON *name =
        cJSON_GetObjectItemCaseSensitive(location, "name");

    cJSON *region =
        cJSON_GetObjectItemCaseSensitive(location, "region");

    cJSON *temperature =
        cJSON_GetObjectItemCaseSensitive(current, "temp_c");

    cJSON *condition_object =
        cJSON_GetObjectItemCaseSensitive(current, "condition");

    cJSON *is_day_value =
        cJSON_GetObjectItemCaseSensitive(current, "is_day");

    cJSON *condition_text = NULL;

    if (cJSON_IsObject(condition_object))
    {
        condition_text =
            cJSON_GetObjectItemCaseSensitive(
                condition_object,
                "text"
            );
    }

    if (!cJSON_IsString(name) ||
        !cJSON_IsNumber(temperature) ||
        !cJSON_IsString(condition_text) ||
        !cJSON_IsNumber(is_day_value))
    {
        ESP_LOGE(TAG, "Weather JSON is missing required values");
        goto cleanup;
    }

    char location_text[96];

    if (cJSON_IsString(region) &&
        region->valuestring[0] != '\0')
    {
        snprintf(
            location_text,
            sizeof(location_text),
            "%s, %s",
            name->valuestring,
            region->valuestring
        );
    }
    else
    {
        snprintf(
            location_text,
            sizeof(location_text),
            "%s",
            name->valuestring
        );
    }

    bool is_day =
        is_day_value->valueint != 0;

    const char *background_path =
        weatherstation_select_background(
            condition_text->valuestring,
            is_day
        );

    const char *icon_path =
        weatherstation_select_icon(
            condition_text->valuestring,
            is_day
        );

    weatherstation_update_t update =
    {
        .temperature_c =
            (float)temperature->valuedouble,

        .condition =
            condition_text->valuestring,

        .location =
            location_text,

        .background_path =
            background_path,

        .icon_path =
            icon_path,

        .use_dark_text =
            is_day
    };

    if (s_update_callback != NULL)
    {
        /*
         * The callback is synchronous. TFT_Display copies label strings
         * before this function deletes the cJSON tree.
         */
        s_update_callback(
            &update,
            s_callback_user_data
        );
    }

    ESP_LOGI(
        TAG,
        "Weather: %.0f C | %s",
        temperature->valuedouble,
        condition_text->valuestring
    );

    ESP_LOGI(TAG, "Location: %s", location_text);
    ESP_LOGI(TAG, "Background: %s", background_path);
    ESP_LOGI(
        TAG,
        "Icon: %s",
        icon_path != NULL ? icon_path : "none"
    );

    success = true;

cleanup:

    cJSON_Delete(root);
    return success;
}

static esp_err_t weatherstation_request_once(void)
{
    char request_url[384];

    int written =
        snprintf(
            request_url,
            sizeof(request_url),
            "http://api.weatherapi.com/"
            "v1/current.json"
            "?key=%s"
            "&q=%s"
            "&aqi=no",
            s_api_key,
            s_location_query
        );

    if (written < 0 ||
        written >= (int)sizeof(request_url))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    s_response_length = 0;
    s_response_overflow = false;
    memset(s_response_buffer, 0, sizeof(s_response_buffer));

    esp_http_client_config_t configuration =
    {
        .url = request_url,
        .event_handler = weatherstation_http_event_handler,
        .timeout_ms = 15000,
        .buffer_size = 2048
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&configuration);

    if (client == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Requesting current weather");

    esp_err_t result =
        esp_http_client_perform(client);

    if (result == ESP_OK)
    {
        int status_code =
            esp_http_client_get_status_code(client);

        ESP_LOGI(
            TAG,
            "Weather HTTP status=%d, response=%u bytes",
            status_code,
            (unsigned int)s_response_length
        );

        if (status_code != 200)
        {
            result = ESP_FAIL;
        }
        else if (s_response_overflow)
        {
            ESP_LOGE(TAG, "Weather response exceeded the buffer");
            result = ESP_ERR_NO_MEM;
        }
        else if (!weatherstation_parse_and_publish(
                     s_response_buffer))
        {
            result = ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Weather HTTP request failed: %s",
            esp_err_to_name(result)
        );
    }

    esp_http_client_cleanup(client);
    return result;
}

static void weatherstation_update_task(void *argument)
{
    (void)argument;

    while (true)
    {
        esp_err_t result =
            weatherstation_request_once();

        TickType_t delay_ticks =
            result == ESP_OK
                ? pdMS_TO_TICKS(WEATHER_NORMAL_UPDATE_MS)
                : pdMS_TO_TICKS(WEATHER_ERROR_RETRY_MS);

        if (result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Weather update failed; retrying in one minute"
            );
        }

        vTaskDelay(delay_ticks);
    }
}

esp_err_t weatherstation_start_task(
    const char *api_key,
    const char *location,
    weatherstation_update_callback_t callback,
    void *user_data
)
{
    if (api_key == NULL || location == NULL || callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_update_task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_api_key = api_key;
    s_location_query = location;
    s_update_callback = callback;
    s_callback_user_data = user_data;

    BaseType_t task_result =
        xTaskCreate(
            weatherstation_update_task,
            "weatherstation_update",
            8192,
            NULL,
            3,
            &s_update_task_handle
        );

    if (task_result != pdPASS)
    {
        s_update_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WeatherStation task started");
    return ESP_OK;
}