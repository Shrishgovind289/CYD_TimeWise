#ifndef NTP_CLOCK_H
#define NTP_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

typedef void (*ntpclock_update_callback_t)(const char *time_text, const char *date_text, void *user_data);

esp_err_t ntpclock_init(const char *timezone, int retry_count);

/*
 * Optional clock callback task.
 * TimeWise main.c does not use this because main.c owns the one-second scheduler.
 */
esp_err_t ntpclock_start_task(ntpclock_update_callback_t update_callback, void *user_data);

/* Public runtime alarm configuration. */
extern volatile int g_alarm_hour;
extern volatile int g_alarm_minute;
extern volatile bool g_alarm_enabled;
extern volatile uint32_t g_alarm_snooze_minutes;

/* Daily alarm configuration. hour_24 must be 0-23 and minute must be 0-59. */
esp_err_t ntpclock_set_alarm(int hour_24, int minute, bool enabled);
void ntpclock_get_alarm(int *hour_24, int *minute, bool *enabled);
void ntpclock_enable_alarm(bool enabled);

bool ntpclock_alarm_is_ringing(void);

/* Starts the two-play alarm task immediately. */
esp_err_t ntpclock_trigger_alarm_now(void);

/* Stops the current alarm and cancels any pending snooze. */
void ntpclock_stop_alarm(void);

/* Stops the current alarm and schedules it to ring again later. */
esp_err_t ntpclock_snooze_alarm(uint32_t snooze_minutes);

bool ntpclock_snooze_is_pending(void);
bool ntpclock_snooze_is_due(time_t current_time);
time_t ntpclock_get_snooze_until(void);
void ntpclock_clear_snooze(void);

#endif /* NTP_CLOCK_H */