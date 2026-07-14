#ifndef NTP_CLOCK_H
#define NTP_CLOCK_H

#include <stdbool.h>

#include "esp_err.h"

typedef void (*ntpclock_update_callback_t)(const char *time_text, const char *date_text, void *user_data);

esp_err_t ntpclock_init(const char *timezone, int retry_count);

esp_err_t ntpclock_start_task(ntpclock_update_callback_t update_callback, void *user_data);

/* Daily alarm configuration. hour_24 must be 0-23 and minute must be 0-59. */
esp_err_t ntpclock_set_alarm(int hour_24, int minute, bool enabled);
void ntpclock_enable_alarm(bool enabled);

extern volatile int g_alarm_hour;
extern volatile int g_alarm_minute;
extern volatile bool g_alarm_enabled;
bool ntpclock_alarm_is_ringing(void);

/* Stops the current alarm. It does not disable future daily alarms. */
void ntpclock_stop_alarm(void);

/* Useful while testing the speaker and alarm WAV file. */
esp_err_t ntpclock_trigger_alarm_now(void);

#endif