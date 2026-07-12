#ifndef SDCARD_H
#define SDCARD_H

#include <stddef.h>

#include "esp_err.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"

/*
 * LCDWIKI ESP32-32E onboard MicroSD pins
 *
 * SD_CS   -> GPIO5
 * SD_MOSI -> GPIO23
 * SD_SCK  -> GPIO18
 * SD_MISO -> GPIO19
 */
#define SDCARD_MOUNT_POINT     "/sdcard"

#define SDCARD_PIN_MISO        19
#define SDCARD_PIN_MOSI        23
#define SDCARD_PIN_SCLK        18
#define SDCARD_PIN_CS          5

/*
 * LCD uses SPI2_HOST in your TFT_Display code.
 * SD card should use SPI3_HOST to avoid bus conflict.
 */
#define SDCARD_SPI_HOST        SPI3_HOST

#define SDCARD_MAX_FILES       5
#define SDCARD_FREQ_KHZ        4000

esp_err_t sdcard_init(void);
esp_err_t sdcard_deinit(void);

int sdcard_is_mounted(void);

sdmmc_card_t *sdcard_get_card(void);
const char *sdcard_get_mount_point(void);

void sdcard_print_info(void);

esp_err_t sdcard_make_path(const char *path, char *out_path, size_t out_size);
esp_err_t sdcard_check_file(const char *path);
esp_err_t sdcard_list_dir(const char *path, int max_depth);

esp_err_t sdcard_mkdir(const char *path);
esp_err_t sdcard_write_text_file(const char *path, const char *text);
esp_err_t sdcard_read_text_file(const char *path, char *buffer, size_t buffer_size);

#endif