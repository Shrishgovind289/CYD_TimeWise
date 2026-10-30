#ifndef WEATHER_STATION_H
#define WEATHER_STATION_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define WEATHERSTATION_ISO_TIME_LENGTH         24U
#define WEATHERSTATION_CONDITION_LENGTH        64U
#define WEATHERSTATION_LOCATION_LENGTH         96U
#define WEATHERSTATION_FORECAST_HOURS            4U
#define WEATHERSTATION_FORECAST_TIME_LENGTH      12U

typedef struct
{
    bool valid;
    float temperature_c;
    int weather_code;
    bool is_day;
    char time_text[WEATHERSTATION_FORECAST_TIME_LENGTH];
    const char *icon_path;
} weatherstation_forecast_t;

typedef struct
{
    float temperature_c;
    int weather_code;
    bool is_day;

    char condition[WEATHERSTATION_CONDITION_LENGTH];
    char location[WEATHERSTATION_LOCATION_LENGTH];

    /*
     * Local ISO-8601 values used by the Sun/Moon arc calculation.
     * Moonrise is assumed to be sunset + 35 minutes.
     * Moonset is assumed to be the following sunrise - 35 minutes.
     */
    char current_time[WEATHERSTATION_ISO_TIME_LENGTH];
    char sunrise_time[WEATHERSTATION_ISO_TIME_LENGTH];
    char sunset_time[WEATHERSTATION_ISO_TIME_LENGTH];
    char moonrise_time[WEATHERSTATION_ISO_TIME_LENGTH];
    char moonset_time[WEATHERSTATION_ISO_TIME_LENGTH];

    /* Static LVGL filesystem paths selected by WeatherStation. */
    const char *background_path;
    const char *icon_path;

    size_t hourly_count;
    weatherstation_forecast_t hourly[WEATHERSTATION_FORECAST_HOURS];
} weatherstation_update_t;

typedef void (*weatherstation_update_callback_t)(const weatherstation_update_t *update);
typedef void (*weatherstation_forecast_callback_t)(const weatherstation_forecast_t *forecast, size_t count);

/*
 * Store location label and callback; coordinates are configured on Inspiron.
 * This function does not create a task and does not make an HTTP request.
 */
esp_err_t weatherstation_init(double latitude, double longitude, const char *location_name, weatherstation_update_callback_t callback);

esp_err_t weatherstation_request(bool is_forecast);
void weatherstation_set_forecast_callback(weatherstation_forecast_callback_t callback);

#endif /* WEATHER_STATION_H */