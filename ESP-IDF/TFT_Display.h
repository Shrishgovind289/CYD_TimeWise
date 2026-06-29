#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include "esp_err.h"
#include "lvgl.h"
#include <sys/lock.h>

esp_err_t tft_display_init(lv_display_t **display_out);

_lock_t *tft_display_get_lvgl_lock(void);

void tft_display_lock(void);
void tft_display_unlock(void);

void lv_fs_stdio_init(void);

#endif // TFT_DISPLAY_H