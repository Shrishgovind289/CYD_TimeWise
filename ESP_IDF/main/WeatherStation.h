#ifndef WEATHER_STATION_H
#define WEATHER_STATION_H

#include <stdbool.h>

#include "esp_err.h"

typedef struct
{
    float temperature_c;

    const char *condition;
    const char *location;

    /* LVGL filesystem paths selected by WeatherStation. */
    const char *background_path;
    const char *icon_path;

    /* Rendering decision already made by WeatherStation. */
    bool use_dark_text;

} weatherstation_update_t;

typedef void (*weatherstation_update_callback_t)(
    const weatherstation_update_t *update,
    void *user_data
);

/* Start the current-weather request/refresh task. */
esp_err_t weatherstation_start_task(
    const char *api_key,
    const char *location,
    weatherstation_update_callback_t callback,
    void *user_data
);

#endif /* WEATHER_STATION_H */