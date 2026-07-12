#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include <stdbool.h>

#include "esp_err.h"

/* Initialize LCD hardware, LVGL, the SD-backed background decoder, and LVGL task. */
esp_err_t tft_display_init(void);

/* Create the TimeWise dashboard. */
esp_err_t tft_dashboard_create(void);

/* Thread-safe dashboard updates. */
void tft_dashboard_set_time(const char *time_text);

void tft_dashboard_set_weather(
    float temperature_c,
    const char *condition,
    const char *location,
    const char *background_path,
    const char *icon_path,
    bool use_dark_text
);

#endif /* TFT_DISPLAY_H */