#ifndef WEATHER_STATION_H
#define WEATHER_STATION_H

#include <stdbool.h>

#include "esp_err.h"

#define WEATHERSTATION_ISO_TIME_LENGTH         24U
#define WEATHERSTATION_CONDITION_LENGTH        64U
#define WEATHERSTATION_LOCATION_LENGTH         96U
#define WEATHERSTATION_WIND_DIRECTION_LENGTH   4U

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
} weatherstation_update_t;

typedef void (*weatherstation_update_callback_t)(const weatherstation_update_t *update, void *user_data);

/*
 * Store the Open-Meteo location and callback.
 *
 * This function does not create a task and does not make an HTTP request.
 */
esp_err_t weatherstation_init(double latitude,
                              double longitude,
                              const char *location_name,
                              weatherstation_update_callback_t callback,
                              void *user_data);

/*
 * Perform one blocking Open-Meteo request.
 *
 * main.c should call this from a worker task so its one-second scheduler
 * remains responsive.
 */
esp_err_t weatherstation_request_once(void);

#endif /* WEATHER_STATION_H */