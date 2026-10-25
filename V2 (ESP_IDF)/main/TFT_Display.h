#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Public runtime display configuration. */
extern volatile uint8_t g_display_brightness_percent;
extern volatile bool g_display_is_day;
extern volatile bool g_tft_display_paused;

/* Initialize LCD hardware, LVGL, the SD-backed background decoder, and LVGL task. */
esp_err_t tft_display_init(void);

void tft_display_set_brightness_percent(uint8_t brightness_percent);
uint8_t tft_display_get_brightness_percent(void);
uint8_t tft_display_get_effective_brightness_percent(void);

void tft_display_set_day_mode(bool is_day);
bool tft_display_is_day(void);

/* Pause/resume LVGL processing while the music player owns the system. */
void tft_display_set_paused(bool paused);
bool tft_display_is_paused(void);

/* Create the TimeWise dashboard. */
esp_err_t tft_dashboard_create(void);

/* Thread-safe dashboard updates. */
void tft_dashboard_set_time(const char *time_text);
void tft_dashboard_set_date(const char *date_text);

void tft_dashboard_set_weather(float temperature_c,
                               const char *condition,
                               const char *location,
                               const char *background_path,
                               const char *icon_path);

void tft_dashboard_set_wind(float wind_speed_kmh, int wind_direction_degrees);

/* Full-screen static Now Playing view. */
void tft_dashboard_show_music(const char *title, const char *artist, uint32_t duration_seconds);
void tft_dashboard_hide_music(void);

/* Update the Sun and Moon paths from local ISO-8601 timestamps. */
void tft_dashboard_set_astro(const char *current_time,
                             const char *sunrise_time,
                             const char *sunset_time,
                             const char *moonrise_time,
                             const char *moonset_time);

#endif /* TFT_DISPLAY_H */