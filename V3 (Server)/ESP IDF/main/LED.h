
#ifndef LED_H
#define LED_H

#include <stdint.h>
#include "esp_err.h"

// LED GPIO configuration
#define LED_RED_PIN       22
#define LED_GREEN_PIN     16
#define LED_BLUE_PIN      17

// PWM configuration
#define LED_PWM_FREQ      5000
#define LED_PWM_MAX       1024

// Initialize LED PWM
esp_err_t led_init(void);

// Independent PWM controls (0 - 1024)
esp_err_t led_set_red(uint16_t pwm);
esp_err_t led_set_green(uint16_t pwm);
esp_err_t led_set_blue(uint16_t pwm);

#endif
