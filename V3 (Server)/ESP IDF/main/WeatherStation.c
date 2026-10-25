#include "WeatherStation.h"
#include "NTPClock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"

#define WEATHER_SERVER_URL "HTTP_GATEWAY_IP:8766/weather"  // This should be defined elsewhere, e.g., in a config header
#define FORECAST_SERVER_URL "HTTP_GATEWAY_IP:8766/forecast"  // This should be defined elsewhere, e.g., in a config header
#define WEATHER_RESPONSE_MAX_SIZE 3072U
#define WEATHER_MOON_OFFSET_MINUTES 35

static const char *TAG = "WeatherStation";
static char s_location_name[WEATHERSTATION_LOCATION_LENGTH] = "";
static weatherstation_update_callback_t s_update_callback = NULL;
static weatherstation_forecast_callback_t s_forecast_callback = NULL;

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
#define BG_FOG              "S:/BG/FOG.BIN"

/* -------------------------------------------------------------------------- */
/*                           Weather icon paths                               */
/* -------------------------------------------------------------------------- */

#define ICON_SUNNY          "S:/ICO/SUNNY.BIN"
#define ICON_NIGHT          "S:/ICO/NIGHT.BIN"
#define ICON_CLOUD          "S:/ICO/CLOUD.BIN"
#define ICON_FOG            "S:/ICO/FOG.BIN"
#define ICON_LIGHT_RAIN     "S:/ICO/LRAIN.BIN"
#define ICON_FREEZE_RAIN    "S:/ICO/FRAIN.BIN"
#define ICON_HEAVY_RAIN     "S:/ICO/HRAIN.BIN"
#define ICON_THUNDER        "S:/ICO/THUNDP.BIN"
#define ICON_THUNDER_RAIN   "S:/ICO/TRAIN.BIN"
#define ICON_LIGHT_SNOW     "S:/ICO/LSNOW.BIN"
#define ICON_SNOW           "S:/ICO/SNOW.BIN"
#define ICON_HEAVY_SNOW     "S:/ICO/HSNOW.BIN"
#define ICON_ICE            "S:/ICO/ICE.BIN"

/* -------------------------------------------------------------------------- */
/*                         WMO weather-code mapping                           */
/* -------------------------------------------------------------------------- */

static const char *weatherstation_condition_from_code(int weather_code)
{
    switch (weather_code)
    {
        case 0:
            return "Clear";

        case 1:
            return "Mainly clear";

        case 2:
            return "Partly cloudy";

        case 3:
            return "Overcast";

        case 45:
            return "Fog";

        case 48:
            return "Rime fog";

        case 51:
            return "Light drizzle";

        case 53:
            return "Drizzle";

        case 55:
            return "Heavy drizzle";

        case 56:
            return "Light freezing drizzle";

        case 57:
            return "Heavy freezing drizzle";

        case 61:
            return "Light rain";

        case 63:
            return "Rain";

        case 65:
            return "Heavy rain";

        case 66:
            return "Light freezing rain";

        case 67:
            return "Heavy freezing rain";

        case 71:
            return "Light snow";

        case 73:
            return "Snow";

        case 75:
            return "Heavy snow";

        case 77:
            return "Snow grains";

        case 80:
            return "Light rain showers";

        case 81:
            return "Rain showers";

        case 82:
            return "Heavy rain showers";

        case 85:
            return "Light snow showers";

        case 86:
            return "Heavy snow showers";

        case 95:
            return "Thunderstorm";

        case 96:
            return "Thunderstorm with hail";

        case 99:
            return "Heavy thunderstorm with hail";

        default:
            return "Unknown";
    }
}

static const char *weatherstation_select_background(int weather_code, bool is_day)
{
    switch (weather_code)
    {
        case 0:
        case 1:
            return is_day ? BG_SUNNY : BG_NIGHT;

        case 2:
        case 3:
            return BG_CLOUD;

        case 45:
        case 48:
            return BG_FOG;

        case 51:
        case 53:
        case 55:
            return BG_DRIZZLE;

        case 56:
        case 57:
        case 66:
        case 67:
            return BG_FREEZING_RAIN;

        case 61:
        case 63:
        case 65:
        case 80:
        case 81:
        case 82:
            return BG_RAIN;

        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86:
            return is_day ? BG_SNOW : BG_DARK_SNOW;

        case 95:
            return BG_THUNDER;

        case 96:
        case 99:
            return BG_RAIN_THUNDER;

        default:
            return BG_DEFAULT;
    }
}

static const char *weatherstation_select_icon(int weather_code, bool is_day)
{
    switch (weather_code)
    {
        case 0:
        case 1:
            return is_day ? ICON_SUNNY : ICON_NIGHT;

        case 2:
        case 3:
            return ICON_CLOUD;

        case 45:
        case 48:
            return ICON_FOG;

        case 51:
        case 53:
        case 55:
        case 61:
        case 80:
            return ICON_LIGHT_RAIN;

        case 56:
        case 57:
        case 66:
        case 67:
            return ICON_FREEZE_RAIN;

        case 63:
        case 65:
        case 81:
        case 82:
            return ICON_HEAVY_RAIN;

        case 71:
        case 85:
            return ICON_LIGHT_SNOW;

        case 73:
            return ICON_SNOW;

        case 75:
        case 77:
        case 86:
            return ICON_HEAVY_SNOW;

        case 95:
            return ICON_THUNDER;

        case 96:
        case 99:
            return ICON_THUNDER_RAIN;

        default:
            return NULL;
    }
}


/* A single weather request is active at a time (main.c enforces this). */
typedef struct
{
    char data[WEATHER_RESPONSE_MAX_SIZE];
    size_t length;
    bool overflow;
} weather_response_t;

static void copy_text(char *dst, size_t capacity, const char *src)
{
    if (dst != NULL && capacity != 0U)
    {
        snprintf(dst, capacity, "%s", src != NULL ? src : "");
    }
}

static esp_err_t on_http_event(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
    {
        return ESP_OK;
    }
    weather_response_t *response = (weather_response_t *)event->user_data;
    const size_t bytes = (size_t)event->data_len;
    if (bytes >= sizeof(response->data) ||
        response->length > sizeof(response->data) - 1U - bytes)
    {
        response->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->data + response->length, event->data, bytes);
    response->length += bytes;
    response->data[response->length] = '\0';
    return ESP_OK;
}

/* Parse local ISO time, then shift minutes across calendar boundaries.
   mktime/localtime use the TZ set by NTPClock, handling DST transitions. */
static bool shift_iso_minutes(const char *input, int delta,
                              char *out, size_t out_capacity)
{
    if (input == NULL || out == NULL || out_capacity < 17U)
    {
        return false;
    }
    struct tm local = {0};
    int year, month, day, hour, minute;
    if (sscanf(input, "%d-%d-%dT%d:%d", &year, &month, &day,
               &hour, &minute) != 5 || year < 2024 ||
        month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59)
    {
        return false;
    }
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute + delta;
    local.tm_isdst = -1;
    time_t adjusted = mktime(&local);
    if (adjusted == (time_t)-1)
    {
        return false;
    }
    struct tm normalized;
    return localtime_r(&adjusted, &normalized) != NULL &&
           strftime(out, out_capacity, "%Y-%m-%dT%H:%M", &normalized) != 0;
}

static const char *json_string(const cJSON *object, const char *key)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(field) ? field->valuestring : NULL;
}

/* The server supplies local hourly ISO strings. Keep time formatting local to ESP32. */
static void parse_forecast(const cJSON *root, const struct tm *now,
                           weatherstation_update_t *update)
{
    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    if (!cJSON_IsArray(hourly))
    {
        ESP_LOGW(TAG, "Weather response contains no hourly forecast array");
        return;
    }

    char now_hour[14] = {0};
    if (strftime(now_hour, sizeof(now_hour), "%Y-%m-%dT%H", now) == 0)
        return;

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, hourly)
    {
        if (update->hourly_count >= WEATHERSTATION_FORECAST_HOURS) break;
        const char *iso = json_string(entry, "time");
        const cJSON *temp = cJSON_GetObjectItemCaseSensitive(entry, "temperature_2m");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(entry, "weather_code");
        const cJSON *day = cJSON_GetObjectItemCaseSensitive(entry, "is_day");
        if (iso == NULL || strlen(iso) < 16U ||
            !cJSON_IsNumber(temp) || !cJSON_IsNumber(code) ||
            !(cJSON_IsBool(day) || cJSON_IsNumber(day)) ||
            strncmp(iso, now_hour, sizeof(now_hour) - 1U) <= 0)
        {
            continue;
        }

        int hour = -1;
        if (sscanf(iso + 11, "%2d", &hour) != 1 || hour < 0 || hour > 23)
            continue;

        weatherstation_forecast_t *slot = &update->hourly[update->hourly_count];
        slot->temperature_c = (float)temp->valuedouble;
        slot->weather_code = code->valueint;
        slot->is_day = cJSON_IsBool(day) ? cJSON_IsTrue(day) : day->valueint != 0;
        slot->icon_path = weatherstation_select_icon(slot->weather_code, slot->is_day);
        snprintf(slot->time_text, sizeof(slot->time_text), "%d %s",
                 (hour % 12) ? (hour % 12) : 12, hour >= 12 ? "PM" : "AM");
        slot->valid = true;
        update->hourly_count++;
    }
    ESP_LOGI(TAG, "Received %u future hourly forecasts", (unsigned)update->hourly_count);
}

static bool parse_weather(const char *data, size_t length)
{
    struct tm now;
    if (!ntpclock_get_local_time(&now))
    {
        ESP_LOGW(TAG, "No valid NTP local time; cannot select astronomy date");
        return false;
    }

    char now_iso[WEATHERSTATION_ISO_TIME_LENGTH];
    char today[11];
    if (strftime(now_iso, sizeof(now_iso), "%Y-%m-%dT%H:%M", &now) == 0 ||
        strftime(today, sizeof(today), "%Y-%m-%d", &now) == 0)
    {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(data, length);
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Cannot parse server weather JSON");
        return false;
    }

    bool ok = false;
    weatherstation_update_t update = {0};
    do
    {
        const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "weather_code");
        const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
        if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(code) ||
            !cJSON_IsArray(daily))
        {
            ESP_LOGE(TAG, "Missing temperature, weather_code or daily array");
            break;
        }

        const cJSON *today_entry = NULL;
        const cJSON *yesterday_entry = NULL;
        const cJSON *tomorrow_entry = NULL;
        char yesterday[11], tomorrow[11];
        struct tm shifted = now;
        shifted.tm_mday -= 1;
        shifted.tm_isdst = -1;
        if (mktime(&shifted) == (time_t)-1 ||
            strftime(yesterday, sizeof(yesterday), "%Y-%m-%d", &shifted) == 0)
            break;
        shifted = now;
        shifted.tm_mday += 1;
        shifted.tm_isdst = -1;
        if (mktime(&shifted) == (time_t)-1 ||
            strftime(tomorrow, sizeof(tomorrow), "%Y-%m-%d", &shifted) == 0)
            break;

        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, daily)
        {
            const char *date = json_string(entry, "date");
            if (date == NULL) continue;
            if (strcmp(date, today) == 0) today_entry = entry;
            if (strcmp(date, yesterday) == 0) yesterday_entry = entry;
            if (strcmp(date, tomorrow) == 0) tomorrow_entry = entry;
        }
        if (today_entry == NULL)
        {
            ESP_LOGW(TAG, "Server weather cache missing NTP date %s", today);
            break;
        }
        const char *sunrise = json_string(today_entry, "sunrise");
        const char *sunset = json_string(today_entry, "sunset");
        if (sunrise == NULL || sunset == NULL || strlen(sunrise) < 16U ||
            strlen(sunset) < 16U)
        {
            ESP_LOGE(TAG, "Missing sunrise/sunset for %s", today);
            break;
        }
        copy_text(update.current_time, sizeof(update.current_time), now_iso);
        copy_text(update.sunrise_time, sizeof(update.sunrise_time), sunrise);
        copy_text(update.sunset_time, sizeof(update.sunset_time), sunset);
        copy_text(update.location, sizeof(update.location), s_location_name);
        update.temperature_c = (float)temperature->valuedouble;
        update.weather_code = code->valueint;
        update.is_day = strcmp(now_iso, sunrise) >= 0 &&
                        strcmp(now_iso, sunset) < 0;
        copy_text(update.condition, sizeof(update.condition),
                  weatherstation_condition_from_code(update.weather_code));
        update.background_path = weatherstation_select_background(
            update.weather_code, update.is_day);
        update.icon_path = weatherstation_select_icon(
            update.weather_code, update.is_day);

        char early_moonset[WEATHERSTATION_ISO_TIME_LENGTH];
        if (!shift_iso_minutes(sunrise, -WEATHER_MOON_OFFSET_MINUTES,
                               early_moonset, sizeof(early_moonset)))
        {
            ESP_LOGE(TAG, "Could not calculate assumed moonset");
            break;
        }
        if (strcmp(now_iso, early_moonset) <= 0)
        {
            const char *previous_sunset = yesterday_entry == NULL ? NULL :
                json_string(yesterday_entry, "sunset");
            if (previous_sunset == NULL ||
                !shift_iso_minutes(previous_sunset, WEATHER_MOON_OFFSET_MINUTES,
                                   update.moonrise_time, sizeof(update.moonrise_time)))
            {
                ESP_LOGW(TAG, "Missing previous day's sunset for moon arc");
                break;
            }
            copy_text(update.moonset_time, sizeof(update.moonset_time), early_moonset);
        }
        else
        {
            const char *next_sunrise = tomorrow_entry == NULL ? NULL :
                json_string(tomorrow_entry, "sunrise");
            if (next_sunrise == NULL ||
                !shift_iso_minutes(sunset, WEATHER_MOON_OFFSET_MINUTES,
                                   update.moonrise_time, sizeof(update.moonrise_time)) ||
                !shift_iso_minutes(next_sunrise, -WEATHER_MOON_OFFSET_MINUTES,
                                   update.moonset_time, sizeof(update.moonset_time)))
            {
                ESP_LOGW(TAG, "Missing next day's sunrise for moon arc");
                break;
            }
        }
        ok = true;
    } while (false);

    cJSON_Delete(root);
    if (!ok) return false;

    ESP_LOGI(TAG, "Weather: %.1f C, %s, code=%d, is_day=%d",
             (double)update.temperature_c, update.condition,
             update.weather_code, update.is_day ? 1 : 0);
    ESP_LOGI(TAG, "Astro NTP=%s sunrise=%s sunset=%s assumed moonrise=%s moonset=%s",
             update.current_time, update.sunrise_time, update.sunset_time,
             update.moonrise_time, update.moonset_time);
    if (s_update_callback != NULL) s_update_callback(&update);
    return true;
}

esp_err_t weatherstation_init(double latitude, double longitude,
                              const char *location_name,
                              weatherstation_update_callback_t callback)
{
    if (latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0 ||
        location_name == NULL || callback == NULL)
        return ESP_ERR_INVALID_ARG;

    /* Coordinates are configured on the Inspiron; validate here for
       compatibility with the existing main.c interface only. */
    copy_text(s_location_name, sizeof(s_location_name), location_name);
    s_update_callback = callback;
    ESP_LOGI(TAG, "Using Inspiron weather cache: %s", WEATHER_SERVER_URL);
    return ESP_OK;
}

static esp_err_t weatherstation_fetch(const char *url, weather_response_t *response)
{
    memset(response, 0, sizeof(*response));
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = on_http_event,
        .user_data = response,
        .timeout_ms = 5000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Requesting %s from Inspiron", url);
    esp_err_t result = esp_http_client_perform(client);
    const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (result != ESP_OK)
        ESP_LOGE(TAG, "Local weather request failed: %s", esp_err_to_name(result));
    else if (status != 200)
    {
        ESP_LOGW(TAG, "Weather HTTP status %d", status);
        result = ESP_FAIL;
    }
    else if (response->overflow || response->length == 0U)
    {
        ESP_LOGE(TAG, "Weather response empty or exceeds %u bytes",
                 (unsigned int)WEATHER_RESPONSE_MAX_SIZE);
        result = ESP_ERR_INVALID_SIZE;
    }
    return result;
}

esp_err_t weatherstation_request(void)
{
    weather_response_t response = {0};
    esp_err_t result = weatherstation_fetch(WEATHER_SERVER_URL, &response);
    if (result == ESP_OK && !parse_weather(response.data, response.length))
        result = ESP_ERR_INVALID_RESPONSE;
    return result;
}

void weatherstation_set_forecast_callback(weatherstation_forecast_callback_t callback)
{
    s_forecast_callback = callback;
}

esp_err_t weatherstation_request_forecast(void)
{
    weather_response_t response = {0};
    esp_err_t result = weatherstation_fetch(FORECAST_SERVER_URL, &response);
    if (result != ESP_OK) return result;
    struct tm now = {0};
    if (!ntpclock_get_local_time(&now)) return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_ParseWithLength(response.data, response.length);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    weatherstation_update_t update = {0};
    parse_forecast(root, &now, &update);
    cJSON_Delete(root);
    if (update.hourly_count == 0U) return ESP_ERR_NOT_FOUND;
    if (s_forecast_callback) s_forecast_callback(update.hourly, update.hourly_count);
    return ESP_OK;
}
