#ifndef WEATHER_STATION_H
#define WEATHER_STATION_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define WEATHERSTATION_FORECAST_HOURS          5U
#define WEATHERSTATION_TIME_TEXT_LENGTH        12U
#define WEATHERSTATION_ISO_TIME_LENGTH         24U
#define WEATHERSTATION_CONDITION_LENGTH        64U
#define WEATHERSTATION_LOCATION_LENGTH         96U
#define WEATHERSTATION_WIND_DIRECTION_LENGTH   4U

typedef struct
{
    bool valid;
    char time_text[WEATHERSTATION_TIME_TEXT_LENGTH];
    float temperature_c;
    int weather_code;
    bool is_day;
    char condition[WEATHERSTATION_CONDITION_LENGTH];
    const char *icon_path;
} weatherstation_hourly_forecast_t;

typedef struct
{
    float temperature_c;
    int weather_code;
    bool is_day;

    char condition[WEATHERSTATION_CONDITION_LENGTH];
    char location[WEATHERSTATION_LOCATION_LENGTH];

    float wind_speed_kmh;
    int wind_direction_degrees;
    char wind_direction[WEATHERSTATION_WIND_DIRECTION_LENGTH];

    /*
     * Local ISO-8601 values returned by Open-Meteo.
     * These are ready for the astro-body position calculation.
     */
    char current_time[WEATHERSTATION_ISO_TIME_LENGTH];
    char sunrise_time[WEATHERSTATION_ISO_TIME_LENGTH];
    char sunset_time[WEATHERSTATION_ISO_TIME_LENGTH];

    /* Static LVGL filesystem paths selected by WeatherStation. */
    const char *background_path;
    const char *icon_path;

    weatherstation_hourly_forecast_t hourly[WEATHERSTATION_FORECAST_HOURS];
    size_t hourly_count;
} weatherstation_update_t;

typedef void (*weatherstation_update_callback_t)(const weatherstation_update_t *update, void *user_data);

/*
 * Start the Open-Meteo current-weather and five-hour forecast task.
 * No API key is required.
 */
esp_err_t weatherstation_start_task(double latitude, double longitude, const char *location_name, weatherstation_update_callback_t callback, void *user_data);

#endif /* WEATHER_STATION_H */