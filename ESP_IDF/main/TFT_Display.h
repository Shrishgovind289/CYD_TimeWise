#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include "esp_err.h"

/* Initialize LCD hardware, LVGL, the SD-backed background decoder, and LVGL task. */
esp_err_t tft_display_init(void);

/* Create the TimeWise dashboard. */
esp_err_t tft_dashboard_create(void);

/* Thread-safe dashboard updates. */
void tft_dashboard_set_time(const char *time_text);

void tft_dashboard_set_date(const char *date_text);

void tft_dashboard_set_weather(float temperature_c, const char *condition, const char *location, const char *background_path, const char *icon_path);

void tft_dashboard_set_wind(float wind_speed_kmh, int wind_direction_degrees);

/* Update the Sun and Moon paths from local ISO-8601 timestamps. */
void tft_dashboard_set_astro(const char *current_time, const char *sunrise_time, const char *sunset_time, const char *moonrise_time, const char *moonset_time);

#endif /* TFT_DISPLAY_H */