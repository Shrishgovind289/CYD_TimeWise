#ifndef WEATHER_STATION_H
#define WEATHER_STATION_H

#include "lvgl.h"
#include <sys/lock.h>

void weatherstation_start_task(
    const char *api_key,
    const char *location,
    lv_obj_t *weather_label,
    lv_obj_t *location_label,
    _lock_t *lvgl_lock
);

#endif // WEATHER_STATION_H