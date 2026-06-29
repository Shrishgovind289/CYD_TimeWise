#ifndef NTP_CLOCK_H
#define NTP_CLOCK_H

#include "esp_err.h"
#include "lvgl.h"
#include <sys/lock.h>

esp_err_t ntpclock_init(const char *timezone, int retry_count);

void ntpclock_start_task(lv_obj_t *time_label, lv_obj_t *date_label, _lock_t *lvgl_lock);

#endif // NTP_CLOCK_H