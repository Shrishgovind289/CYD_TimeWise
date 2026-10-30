
#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "WeatherStation.h"

#include "esp_err.h"

/* -------------------------------------------------------------------------- */
/*                         Global Display Settings                            */
/* -------------------------------------------------------------------------- */

extern volatile uint8_t g_display_brightness_percent;
extern volatile bool g_display_is_day;
extern volatile bool g_tft_display_paused;

/* -------------------------------------------------------------------------- */
/*                         Display Initialization                             */
/* -------------------------------------------------------------------------- */

esp_err_t tft_display_init(void);

/* -------------------------------------------------------------------------- */
/*                         Backlight Brightness                               */
/* -------------------------------------------------------------------------- */

/*
 * Apply the requested WebSocket brightness (0-100%).
 * Between midnight and 6 AM, if is_day is false,
 * the effective brightness is fixed at 10%.
 */
void tft_display_brightness(
    bool is_day,
    uint8_t websocket_brightness,
    uint8_t hour
);

/* -------------------------------------------------------------------------- */
/*                         Display Pause / Resume                             */
/* -------------------------------------------------------------------------- */

void tft_display_set_paused(bool paused);
bool tft_display_is_paused(void);

/* -------------------------------------------------------------------------- */
/*                         Dashboard Initialization                           */
/* -------------------------------------------------------------------------- */

esp_err_t tft_dashboard_create(void);

/* -------------------------------------------------------------------------- */
/*                         Clock and Date                                     */
/* -------------------------------------------------------------------------- */

void tft_dashboard_set_time(const char *time_text);
void tft_dashboard_set_date(const char *date_text);

/* -------------------------------------------------------------------------- */
/*                         Weather Display                                    */
/* -------------------------------------------------------------------------- */

/*
 * Update current weather, location, background and weather icon.
 *
 * Background images are loaded from the SD card.
 * Dashboard text color is selected automatically from the
 * background filename using the Image_Use.xlsx mapping.
 *
 * This includes:
 * - Time, date and location
 * - Current temperature and weather condition
 * - Five-hour forecast times and temperatures
 *
 * No separate function call is needed to change text color.
 */
void tft_dashboard_set_weather(
    float temperature_c,
    const char *condition,
    const char *location,
    const char *background_path,
    const char *icon_path
);

/* Update five weather-forecast cards, from the server's hourly data. */
void tft_dashboard_set_forecast(const weatherstation_forecast_t *forecast, size_t count);

/* Release only forecast icon pixels; preserve forecast labels and all primary UI. */
void tft_dashboard_release_forecast_icons(void);

/* -------------------------------------------------------------------------- */
/*                         Sun / Moon                                         */
/* -------------------------------------------------------------------------- */

void tft_dashboard_set_astro(
    const char *current_time,
    const char *sunrise_time,
    const char *sunset_time,
    const char *moonrise_time,
    const char *moonset_time
);

/* -------------------------------------------------------------------------- */
/*                         Music Display                                      */
/* -------------------------------------------------------------------------- */

void tft_dashboard_show_music(
    const char *title,
    const char *artist,
    uint32_t duration_seconds
);

void tft_dashboard_hide_music(void);

#endif /* TFT_DISPLAY_H */
