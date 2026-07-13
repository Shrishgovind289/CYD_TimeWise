#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define TFT_FORECAST_CARD_COUNT 5U

typedef struct
{
    const char *time_text;
    float temperature_c;
    const char *condition;
    const char *icon_path;

} tft_forecast_hour_t;

/* Initialize LCD hardware, LVGL, the SD-backed background decoder, and LVGL task. */
esp_err_t tft_display_init(void);

/* Create the TimeWise dashboard. */
esp_err_t tft_dashboard_create(void);

/* Thread-safe dashboard updates. */
void tft_dashboard_set_time(const char *time_text);

void tft_dashboard_set_weather(float temperature_c, const char *condition, const char *location, const char *background_path, const char *icon_path);

void tft_dashboard_set_wind(float wind_speed_kmh, int wind_direction_degrees);

void tft_dashboard_set_hourly_forecast(const tft_forecast_hour_t *forecast, size_t forecast_count);

#endif /* TFT_DISPLAY_H */