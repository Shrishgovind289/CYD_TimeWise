#ifndef NTP_CLOCK_H
#define NTP_CLOCK_H

#include "esp_err.h"

typedef void (*ntpclock_update_callback_t)(
    const char *time_text,
    void *user_data
);

/* Configure SNTP and wait briefly for the first valid time. */
esp_err_t ntpclock_init(
    const char *timezone,
    int retry_count
);

/* Start the task that formats and publishes the current time. */
esp_err_t ntpclock_start_task(
    ntpclock_update_callback_t callback,
    void *user_data
);

#endif /* NTP_CLOCK_H */