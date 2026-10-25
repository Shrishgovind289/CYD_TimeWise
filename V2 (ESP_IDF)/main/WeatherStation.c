#include "WeatherStation.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "cJSON.h"

#define WEATHER_RESPONSE_INITIAL_SIZE       (4U * 1024U)
#define WEATHER_RESPONSE_MAX_SIZE           (16U * 1024U)
#define WEATHER_PRINT_JSON                  0
#define WEATHER_MOON_OFFSET_MINUTES         35

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
#define BG_ICE              "S:/BG/ICE.BIN"

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

static const char *TAG = "WeatherStation";

static char *s_response_buffer = NULL;
static size_t s_response_capacity = 0U;
static size_t s_response_length = 0U;
static bool s_response_overflow = false;

static double s_latitude = 0.0;
static double s_longitude = 0.0;
static char s_location_name[WEATHERSTATION_LOCATION_LENGTH] = "";

static weatherstation_update_callback_t s_update_callback = NULL;
static void *s_callback_user_data = NULL;
static bool s_initialized = false;
static bool s_request_in_progress = false;

/* -------------------------------------------------------------------------- */
/*                              Text helpers                                  */
/* -------------------------------------------------------------------------- */

static void weatherstation_copy_text(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0U)
    {
        return;
    }

    snprintf(destination, destination_size, "%s", source != NULL ? source : "");
}


typedef struct
{
    int year;
    int month;
    int day;
} weatherstation_date_t;

static bool weatherstation_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int weatherstation_days_in_month(int year, int month)
{
    static const int days_per_month[12] =
    {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12)
    {
        return 0;
    }

    if (month == 2 && weatherstation_is_leap_year(year))
    {
        return 29;
    }

    return days_per_month[month - 1];
}

static void weatherstation_shift_date(weatherstation_date_t *date, int day_delta)
{
    if (date == NULL)
    {
        return;
    }

    while (day_delta > 0)
    {
        date->day++;

        if (date->day > weatherstation_days_in_month(date->year, date->month))
        {
            date->day = 1;
            date->month++;

            if (date->month > 12)
            {
                date->month = 1;
                date->year++;
            }
        }

        day_delta--;
    }

    while (day_delta < 0)
    {
        date->day--;

        if (date->day < 1)
        {
            date->month--;

            if (date->month < 1)
            {
                date->month = 12;
                date->year--;
            }

            date->day = weatherstation_days_in_month(date->year, date->month);
        }

        day_delta++;
    }
}

static bool weatherstation_adjust_iso_minutes(const char *input,  int minute_offset,  char *output,  size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0U)
    {
        return false;
    }

    weatherstation_date_t date = {0};
    int hour = 0;
    int minute = 0;

    if (sscanf(input, "%d-%d-%dT%d:%d", &date.year, &date.month, &date.day, &hour, &minute) != 5)
    {
        return false;
    }

    int days_in_month = weatherstation_days_in_month(date.year, date.month);

    if (days_in_month == 0 || date.day < 1 || date.day > days_in_month || hour < 0 || hour > 23 || minute < 0 || minute > 59)
    {
        return false;
    }

    int total_minutes = hour * 60 + minute + minute_offset;

    while (total_minutes < 0)
    {
        total_minutes += 24 * 60;
        weatherstation_shift_date(&date, -1);
    }

    while (total_minutes >= 24 * 60)
    {
        total_minutes -= 24 * 60;
        weatherstation_shift_date(&date, 1);
    }

    hour = total_minutes / 60;
    minute = total_minutes % 60;

    int written = snprintf(output, output_size, "%04d-%02d-%02dT%02d:%02d", date.year, date.month, date.day, hour, minute);

    return written > 0 && written < (int)output_size;
}

static const char *weatherstation_json_string_at(cJSON *array, int index)
{
    if (!cJSON_IsArray(array) || index < 0)
    {
        return NULL;
    }

    cJSON *item = cJSON_GetArrayItem(array, index);

    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : NULL;
}

static int weatherstation_find_daily_index(cJSON *dates, const char *current_time)
{
    if (!cJSON_IsArray(dates) || current_time == NULL || strlen(current_time) < 10U)
    {
        return -1;
    }

    int count = cJSON_GetArraySize(dates);

    for (int index = 0; index < count; index++)
    {
        const char *date_text = weatherstation_json_string_at(dates, index);

        if (date_text != NULL && strncmp(date_text, current_time, 10U) == 0)
        {
            return index;
        }
    }

    return -1;
}

/*static bool weatherstation_format_hour_text(const char *api_time_text, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0U)
    {
        return false;
    }

    weatherstation_copy_text(output, output_size, "--");

    if (api_time_text == NULL)
    {
        return false;
    }

    const char *time_part = strrchr(api_time_text, 'T');

    if (time_part == NULL)
    {
        time_part = strrchr(api_time_text, ' ');
    }

    if (time_part == NULL)
    {
        return false;
    }

    int hour_24 = 0;
    int minute = 0;

    if (sscanf(time_part + 1, "%d:%d", &hour_24, &minute) != 2)
    {
        return false;
    }

    (void)minute;

    bool is_pm = hour_24 >= 12;
    int hour_12 = hour_24 % 12;

    if (hour_12 == 0)
    {
        hour_12 = 12;
    }

    snprintf(output, output_size, "%02d %s", hour_12, is_pm ? "PM" : "AM");
    return true;
}*/

static const char *weatherstation_wind_cardinal(int direction_degrees)
{
    static const char *directions[16] =
    {
        "N", "NNE", "NE", "ENE",
        "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW",
        "W", "WNW", "NW", "NNW"
    };

    while (direction_degrees < 0)
    {
        direction_degrees += 360;
    }

    direction_degrees %= 360;

    int index = (int)(((float)direction_degrees + 11.25f) / 22.5f) % 16;
    return directions[index];
}

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

/* -------------------------------------------------------------------------- */
/*                         HTTP response buffer                               */
/* -------------------------------------------------------------------------- */

static bool weatherstation_response_reserve(size_t required_capacity)
{
    if (required_capacity > WEATHER_RESPONSE_MAX_SIZE)
    {
        return false;
    }

    if (required_capacity <= s_response_capacity)
    {
        return true;
    }

    size_t new_capacity = s_response_capacity > 0U ? s_response_capacity : WEATHER_RESPONSE_INITIAL_SIZE;

    while (new_capacity < required_capacity)
    {
        new_capacity *= 2U;

        if (new_capacity > WEATHER_RESPONSE_MAX_SIZE)
        {
            new_capacity = WEATHER_RESPONSE_MAX_SIZE;
        }

        if (new_capacity < required_capacity && new_capacity == WEATHER_RESPONSE_MAX_SIZE)
        {
            return false;
        }
    }

    char *new_buffer = realloc(s_response_buffer, new_capacity);

    if (new_buffer == NULL)
    {
        return false;
    }

    s_response_buffer = new_buffer;
    s_response_capacity = new_capacity;
    return true;
}

static bool weatherstation_response_reset(void)
{
    s_response_length = 0U;
    s_response_overflow = false;

    if (s_response_buffer != NULL && s_response_capacity > 0U)
    {
        s_response_buffer[0] = '\0';
    }

    return true;
}

static void weatherstation_response_release(void)
{
    free(s_response_buffer);
    s_response_buffer = NULL;
    s_response_capacity = 0U;
    s_response_length = 0U;
    s_response_overflow = false;
}

static esp_err_t weatherstation_http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
    {
        return ESP_OK;
    }

    size_t requested = (size_t)event->data_len;
    size_t required_capacity = s_response_length + requested + 1U;

    if (!weatherstation_response_reserve(required_capacity))
    {
        s_response_overflow = true;
        return ESP_ERR_NO_MEM;
    }

    memcpy(s_response_buffer + s_response_length, event->data, requested);
    s_response_length += requested;
    s_response_buffer[s_response_length] = '\0';
    return ESP_OK;
}

#if WEATHER_PRINT_JSON
static void weatherstation_print_json(const char *json_text, size_t json_length)
{
    if (json_text == NULL || json_length == 0U)
    {
        ESP_LOGW(TAG, "JSON response is empty");
        return;
    }

    printf("\n========== OPEN-METEO JSON START ==========\n");

    const size_t chunk_size = 512U;

    for (size_t offset = 0U; offset < json_length; offset += chunk_size)
    {
        size_t remaining = json_length - offset;
        size_t bytes_to_print = remaining < chunk_size ? remaining : chunk_size;
        fwrite(json_text + offset, 1U, bytes_to_print, stdout);
    }

    printf("\n=========== OPEN-METEO JSON END ===========\n\n");
    fflush(stdout);
}
#endif

/* -------------------------------------------------------------------------- */
/*                              JSON parsing                                  */
/* -------------------------------------------------------------------------- */

/*static size_t weatherstation_smallest_array_size(cJSON *array_1, cJSON *array_2, cJSON *array_3, cJSON *array_4)
{
    int size_1 = cJSON_GetArraySize(array_1);
    int size_2 = cJSON_GetArraySize(array_2);
    int size_3 = cJSON_GetArraySize(array_3);
    int size_4 = cJSON_GetArraySize(array_4);

    int smallest = size_1;

    if (size_2 < smallest)
    {
        smallest = size_2;
    }

    if (size_3 < smallest)
    {
        smallest = size_3;
    }

    if (size_4 < smallest)
    {
        smallest = size_4;
    }

    return smallest > 0 ? (size_t)smallest : 0U;
}*/

/*static size_t weatherstation_parse_hourly_forecast(cJSON *root, const char *current_time, weatherstation_update_t *update)
{
    if (root == NULL || current_time == NULL || update == NULL)
    {
        return 0U;
    }

    cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");

    if (!cJSON_IsObject(hourly))
    {
        ESP_LOGW(TAG, "Open-Meteo JSON has no hourly object");
        return 0U;
    }

    cJSON *times = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    cJSON *temperatures = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    cJSON *is_day_values = cJSON_GetObjectItemCaseSensitive(hourly, "is_day");
    cJSON *weather_codes = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");

    if (!cJSON_IsArray(times) || !cJSON_IsArray(temperatures) ||
        !cJSON_IsArray(is_day_values) || !cJSON_IsArray(weather_codes))
    {
        ESP_LOGW(TAG, "Open-Meteo hourly arrays are incomplete");
        return 0U;
    }

    size_t available_count = weatherstation_smallest_array_size(times, temperatures, is_day_values, weather_codes);
    size_t forecast_count = 0U;

    for (size_t index = 0U; index < available_count; index++)
    {
        if (forecast_count >= WEATHERSTATION_FORECAST_HOURS)
        {
            break;
        }

        cJSON *time_value = cJSON_GetArrayItem(times, (int)index);
        cJSON *temperature_value = cJSON_GetArrayItem(temperatures, (int)index);
        cJSON *is_day_value = cJSON_GetArrayItem(is_day_values, (int)index);
        cJSON *weather_code_value = cJSON_GetArrayItem(weather_codes, (int)index);

        if (!cJSON_IsString(time_value) || time_value->valuestring == NULL ||
            !cJSON_IsNumber(temperature_value) || !cJSON_IsNumber(is_day_value) ||
            !cJSON_IsNumber(weather_code_value))
        {
            continue;
        }

        //Open-Meteo returns local ISO-8601 strings in the same timezone andfixed format. Lexicographical order therefore matches time order.
        if (strcmp(time_value->valuestring, current_time) <= 0)
        {
            continue;
        }

        weatherstation_hourly_forecast_t *slot = &update->hourly[forecast_count];
        memset(slot, 0, sizeof(*slot));

        slot->temperature_c = (float)temperature_value->valuedouble;
        slot->weather_code = weather_code_value->valueint;
        slot->is_day = is_day_value->valueint != 0;

        weatherstation_format_hour_text(time_value->valuestring, slot->time_text, sizeof(slot->time_text));
        weatherstation_copy_text(slot->condition, sizeof(slot->condition), weatherstation_condition_from_code(slot->weather_code));

        slot->icon_path = weatherstation_select_icon(slot->weather_code, slot->is_day);
        slot->valid = true;
        forecast_count++;
    }

    return forecast_count;
}*/

static void weatherstation_parse_astro(cJSON *root, weatherstation_update_t *update)
{
    if (root == NULL || update == NULL || update->current_time[0] == '\0')
    {
        return;
    }

    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");

    if (!cJSON_IsObject(daily))
    {
        ESP_LOGW(TAG, "Open-Meteo JSON has no daily object");
        return;
    }

    cJSON *date_values = cJSON_GetObjectItemCaseSensitive(daily, "time");
    cJSON *sunrise_values = cJSON_GetObjectItemCaseSensitive(daily, "sunrise");
    cJSON *sunset_values = cJSON_GetObjectItemCaseSensitive(daily, "sunset");

    if (!cJSON_IsArray(date_values) || !cJSON_IsArray(sunrise_values) || !cJSON_IsArray(sunset_values))
    {
        ESP_LOGW(TAG, "Open-Meteo daily astronomy arrays are incomplete");
        return;
    }

    int today_index = weatherstation_find_daily_index(date_values, update->current_time);

    if (today_index < 0)
    {
        ESP_LOGW(TAG, "Could not match current date to Open-Meteo daily astronomy data");
        return;
    }

    const char *today_sunrise = weatherstation_json_string_at(sunrise_values, today_index);
    const char *today_sunset = weatherstation_json_string_at(sunset_values, today_index);

    if (today_sunrise == NULL || today_sunset == NULL)
    {
        ESP_LOGW(TAG, "Today's sunrise or sunset is unavailable");
        return;
    }

    weatherstation_copy_text(update->sunrise_time, sizeof(update->sunrise_time), today_sunrise);
    weatherstation_copy_text(update->sunset_time, sizeof(update->sunset_time), today_sunset);

    char today_moonrise[WEATHERSTATION_ISO_TIME_LENGTH] = "";
    char today_moonset[WEATHERSTATION_ISO_TIME_LENGTH] = "";

    if (!weatherstation_adjust_iso_minutes(today_sunset,
                                           WEATHER_MOON_OFFSET_MINUTES,
                                           today_moonrise,
                                           sizeof(today_moonrise)) ||
        !weatherstation_adjust_iso_minutes(today_sunrise,
                                           -WEATHER_MOON_OFFSET_MINUTES,
                                           today_moonset,
                                           sizeof(today_moonset)))
    {
        ESP_LOGW(TAG, "Could not derive assumed Moon times");
        return;
    }

    /*
     * Before today's assumed moonset, continue the interval that started after
     * yesterday's sunset. Otherwise cache tonight's upcoming interval.
     */
    if (strcmp(update->current_time, today_moonset) <= 0)
    {
        const char *previous_sunset = weatherstation_json_string_at(sunset_values, today_index - 1);

        if (previous_sunset != NULL &&
            weatherstation_adjust_iso_minutes(previous_sunset,
                                              WEATHER_MOON_OFFSET_MINUTES,
                                              update->moonrise_time,
                                              sizeof(update->moonrise_time)))
        {
            weatherstation_copy_text(update->moonset_time,
                                     sizeof(update->moonset_time),
                                     today_moonset);
        }
    }
    else
    {
        const char *next_sunrise = weatherstation_json_string_at(sunrise_values, today_index + 1);

        if (next_sunrise != NULL &&
            weatherstation_adjust_iso_minutes(next_sunrise,
                                              -WEATHER_MOON_OFFSET_MINUTES,
                                              update->moonset_time,
                                              sizeof(update->moonset_time)))
        {
            weatherstation_copy_text(update->moonrise_time,
                                     sizeof(update->moonrise_time),
                                     today_moonrise);
        }
    }

    if (update->moonrise_time[0] == '\0' || update->moonset_time[0] == '\0')
    {
        ESP_LOGW(TAG, "Could not create the assumed Moon interval");
    }
}

static bool weatherstation_parse_and_publish(const char *json_text, size_t json_length)
{
    if (json_text == NULL || json_length == 0U)
    {
        ESP_LOGE(TAG, "Open-Meteo JSON is empty");
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(json_text, json_length);

    if (root == NULL)
    {
        const char *error_pointer = cJSON_GetErrorPtr();

        ESP_LOGE(TAG,
                 "Failed to parse Open-Meteo JSON: bytes=%u, free_heap=%u, largest_block=%u",
                 (unsigned int)json_length,
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        if (error_pointer != NULL)
        {
            ESP_LOGE(TAG, "JSON error near: %.80s", error_pointer);
        }

        return false;
    }

    weatherstation_update_t update = {0};
    bool parsed = false;

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");

    if (!cJSON_IsObject(current))
    {
        ESP_LOGE(TAG, "Open-Meteo JSON is missing current");
        goto cleanup;
    }

    cJSON *current_time = cJSON_GetObjectItemCaseSensitive(current, "time");
    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    cJSON *is_day_value = cJSON_GetObjectItemCaseSensitive(current, "is_day");
    cJSON *weather_code_value = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    cJSON *wind_speed_value = cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m");
    cJSON *wind_direction_value = cJSON_GetObjectItemCaseSensitive(current, "wind_direction_10m");

    if (!cJSON_IsString(current_time) || current_time->valuestring == NULL || !cJSON_IsNumber(temperature) || !cJSON_IsNumber(is_day_value) || !cJSON_IsNumber(weather_code_value) || !cJSON_IsNumber(wind_speed_value) || !cJSON_IsNumber(wind_direction_value))
    {
        ESP_LOGE(TAG, "Open-Meteo current object is missing required values");
        goto cleanup;
    }

    weatherstation_copy_text(update.location, sizeof(update.location), s_location_name);
    weatherstation_copy_text(update.current_time, sizeof(update.current_time), current_time->valuestring);

    update.temperature_c = (float)temperature->valuedouble;
    update.weather_code = weather_code_value->valueint;
    update.is_day = is_day_value->valueint != 0;
    update.wind_speed_kmh = (float)wind_speed_value->valuedouble;
    update.wind_direction_degrees = wind_direction_value->valueint;

    weatherstation_copy_text(update.condition, sizeof(update.condition), weatherstation_condition_from_code(update.weather_code));
    weatherstation_copy_text(update.wind_direction, sizeof(update.wind_direction), weatherstation_wind_cardinal(update.wind_direction_degrees));

    update.background_path = weatherstation_select_background(update.weather_code, update.is_day);
    update.icon_path = weatherstation_select_icon(update.weather_code, update.is_day);

    weatherstation_parse_astro(root, &update);
    //update.hourly_count = weatherstation_parse_hourly_forecast(root, update.current_time, &update);
    parsed = true;

cleanup:
    cJSON_Delete(root);

    if (!parsed)
    {
        return false;
    }

    ESP_LOGI(TAG, "Current: %.1f C | %s | code=%d | is_day=%d",
             (double)update.temperature_c,
             update.condition,
             update.weather_code,
             update.is_day ? 1 : 0);

    ESP_LOGI(TAG, "Wind: %.1f km/h | %s | %d deg",
             (double)update.wind_speed_kmh,
             update.wind_direction,
             update.wind_direction_degrees);

    ESP_LOGI(TAG, "Astro: sunrise=%s | sunset=%s | assumed moonrise=%s | assumed moonset=%s",
             update.sunrise_time[0] != '\0' ? update.sunrise_time : "unavailable",
             update.sunset_time[0] != '\0' ? update.sunset_time : "unavailable",
             update.moonrise_time[0] != '\0' ? update.moonrise_time : "unavailable",
             update.moonset_time[0] != '\0' ? update.moonset_time : "unavailable");

    //ESP_LOGI(TAG, "Hourly forecast cards: %u", (unsigned int)update.hourly_count);

    /*for (size_t index = 0U; index < update.hourly_count; index++)
    {
        const weatherstation_hourly_forecast_t *slot = &update.hourly[index];

        ESP_LOGI(TAG, "Forecast %u: %s | %.1f C | code=%d | %s",
                 (unsigned int)(index + 1U),
                 slot->time_text,
                 (double)slot->temperature_c,
                 slot->weather_code,
                 slot->condition);
    }*/

    if (s_update_callback != NULL)
    {
        s_update_callback(&update, s_callback_user_data);
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/*                             Request task                                   */
/* -------------------------------------------------------------------------- */

static esp_err_t weatherstation_perform_request(void)
{
    char request_url[640];

    int written = snprintf(
        request_url,
        sizeof(request_url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.6f"
        "&longitude=%.6f"
        "&current=temperature_2m,is_day,weather_code,wind_speed_10m,wind_direction_10m"
        //"&hourly=temperature_2m,is_day,weather_code"
        "&daily=sunrise,sunset"
        "&past_days=1"
        "&forecast_days=2"
        "&timezone=auto",
        s_latitude,
        s_longitude
    );

    if (written < 0 || written >= (int)sizeof(request_url))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!weatherstation_response_reset())
    {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t configuration =
    {
        .url = request_url,
        .event_handler = weatherstation_http_event_handler,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    esp_http_client_handle_t client = esp_http_client_init(&configuration);

    if (client == NULL)
    {
        weatherstation_response_release();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Requesting Open-Meteo weather");

    esp_err_t result = esp_http_client_perform(client);
    int status_code = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Open-Meteo HTTP request failed: %s", esp_err_to_name(result));
    }
    else
    {
        ESP_LOGI(TAG, "Open-Meteo HTTP status=%d, response=%u bytes",
                 status_code,
                 (unsigned int)s_response_length);
    }

    esp_http_client_cleanup(client);
    client = NULL;

    if (result == ESP_OK)
    {
        if (status_code != 200)
        {
            ESP_LOGE(TAG, "Open-Meteo returned HTTP status %d", status_code);
            result = ESP_FAIL;
        }
        else if (s_response_overflow)
        {
            ESP_LOGE(TAG, "Open-Meteo response exceeded the buffer");
            result = ESP_ERR_NO_MEM;
        }
        else
        {
#if WEATHER_PRINT_JSON
            weatherstation_print_json(s_response_buffer, s_response_length);
#endif

            if (!weatherstation_parse_and_publish(s_response_buffer, s_response_length))
            {
                result = ESP_FAIL;
            }
        }
    }

    weatherstation_response_release();
    return result;
}

esp_err_t weatherstation_init(double latitude,
                              double longitude,
                              const char *location_name,
                              weatherstation_update_callback_t callback,
                              void *user_data)
{
    if (latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0 ||
        location_name == NULL ||
        callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_request_in_progress)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_latitude = latitude;
    s_longitude = longitude;

    weatherstation_copy_text(
        s_location_name,
        sizeof(s_location_name),
        location_name
    );

    s_update_callback = callback;
    s_callback_user_data = user_data;
    s_initialized = true;

    ESP_LOGI(
        TAG,
        "Open-Meteo configured for %.6f, %.6f",
        s_latitude,
        s_longitude
    );

    return ESP_OK;
}

esp_err_t weatherstation_request_once(void)
{
    if (!s_initialized ||
        s_update_callback == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_request_in_progress)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_request_in_progress = true;

    esp_err_t result =
        weatherstation_perform_request();

    s_request_in_progress = false;

    return result;
}