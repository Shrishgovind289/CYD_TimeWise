#include "SDCard.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SDCard";

static sdmmc_card_t *sd_card = NULL;
static int sdcard_mounted = 0;
static int spi_bus_was_initialized_here = 0;

static esp_err_t sdcard_enable_pullups(void)
{
    gpio_set_pull_mode(SDCARD_PIN_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SDCARD_PIN_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SDCARD_PIN_SCLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SDCARD_PIN_CS, GPIO_PULLUP_ONLY);

    return ESP_OK;
}

static bool sdcard_path_is_absolute(const char *path)
{
    if (path == NULL)
    {
        return false;
    }

    size_t mount_length = strlen(SDCARD_MOUNT_POINT);

    if (strcmp(path, SDCARD_MOUNT_POINT) == 0)
    {
        return true;
    }

    return strncmp(path, SDCARD_MOUNT_POINT, mount_length) == 0 &&
           path[mount_length] == '/';
}

static esp_err_t sdcard_validate_path(const char *path)
{
    if (path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!sdcard_path_is_absolute(path))
    {
        ESP_LOGE(TAG, "SD path must be absolute and begin with %s: %s",
                 SDCARD_MOUNT_POINT,
                 path);

        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t sdcard_init(void)
{
    esp_err_t ret;

    if (sdcard_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card");
    ESP_LOGI(TAG, "Mount point: %s", SDCARD_MOUNT_POINT);
    ESP_LOGI(TAG, "Pins: CS=%d MOSI=%d SCLK=%d MISO=%d",
             SDCARD_PIN_CS,
             SDCARD_PIN_MOSI,
             SDCARD_PIN_SCLK,
             SDCARD_PIN_MISO);

    ret = sdcard_enable_pullups();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable SD card pullups");
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDCARD_SPI_HOST;
    host.max_freq_khz = SDCARD_FREQ_KHZ;

    spi_bus_config_t bus_config = {
        .mosi_io_num = SDCARD_PIN_MOSI,
        .miso_io_num = SDCARD_PIN_MISO,
        .sclk_io_num = SDCARD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ret = spi_bus_initialize(host.slot, &bus_config, SDSPI_DEFAULT_DMA);

    if (ret == ESP_OK) {
        spi_bus_was_initialized_here = 1;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus already initialized, continuing");
        spi_bus_was_initialized_here = 0;
    } else {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = 0,
        .max_files = SDCARD_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SDCARD_PIN_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(
        SDCARD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &sd_card
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));

        if (spi_bus_was_initialized_here) {
            spi_bus_free(host.slot);
            spi_bus_was_initialized_here = 0;
        }

        sd_card = NULL;
        sdcard_mounted = 0;

        return ret;
    }

    sdcard_mounted = 1;

    ESP_LOGI(TAG, "SD card mounted successfully");

    return ESP_OK;
}

esp_err_t sdcard_deinit(void)
{
    if (!sdcard_mounted) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Unmounting SD card");

    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, sd_card);

    sd_card = NULL;
    sdcard_mounted = 0;

    if (spi_bus_was_initialized_here) {
        spi_bus_free(SDCARD_SPI_HOST);
        spi_bus_was_initialized_here = 0;
    }

    return ESP_OK;
}

int sdcard_is_mounted(void)
{
    return sdcard_mounted;
}

esp_err_t sdcard_check_file(const char *path)
{
    struct stat file_stat;

    if (!sdcard_mounted)
    {
        ESP_LOGE(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = sdcard_validate_path(path);

    if (result != ESP_OK)
    {
        return result;
    }

    if (stat(path, &file_stat) != 0)
    {
        ESP_LOGE(TAG, "File not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "File found: %s, size=%ld bytes", path, (long)file_stat.st_size);

    return ESP_OK;
}

static esp_err_t sdcard_list_dir_internal(const char *path, int max_depth, int current_depth)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(path);

    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory %s, errno=%d", path, errno);
        return ESP_FAIL;
    }

    while ((entry = readdir(dir)) != NULL) {
        char child_path[512];
        struct stat child_stat;
        int written;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        written = snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);

        if (written < 0 || written >= (int)sizeof(child_path)) {
            ESP_LOGW(TAG, "Skipping path because it is too long");
            continue;
        }

        if (stat(child_path, &child_stat) != 0) {
            ESP_LOGW(TAG, "Could not stat path: %s", child_path);
            continue;
        }

        for (int i = 0; i < current_depth; i++) {
            printf("  ");
        }

        if (S_ISDIR(child_stat.st_mode)) {
            printf("[DIR]  %s\n", child_path);

            if (current_depth < max_depth) {
                sdcard_list_dir_internal(child_path, max_depth, current_depth + 1);
            }
        } else {
            printf("[FILE] %s (%ld bytes)\n", child_path, (long)child_stat.st_size);
        }
    }

    closedir(dir);

    return ESP_OK;
}

esp_err_t sdcard_list_dir(const char *path, int max_depth)
{
    if (!sdcard_mounted)
    {
        ESP_LOGE(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (path == NULL)
    {
        path = SDCARD_MOUNT_POINT;
    }

    esp_err_t result = sdcard_validate_path(path);

    if (result != ESP_OK)
    {
        return result;
    }

    if (max_depth < 0)
    {
        max_depth = 0;
    }

    ESP_LOGI(TAG, "Listing directory: %s", path);

    return sdcard_list_dir_internal(path, max_depth, 0);
}

esp_err_t sdcard_write_text_file(const char *path, const char *text)
{
    if (!sdcard_mounted)
    {
        ESP_LOGE(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (text == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = sdcard_validate_path(path);

    if (result != ESP_OK)
    {
        return result;
    }

    FILE *file = fopen(path, "w");

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
        return ESP_FAIL;
    }

    int write_result = fprintf(file, "%s", text);

    fclose(file);

    if (write_result < 0)
    {
        ESP_LOGE(TAG, "Failed to write file: %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wrote file: %s", path);

    return ESP_OK;
}

esp_err_t sdcard_read_text_file(const char *path, char *buffer, size_t buffer_size)
{
    if (!sdcard_mounted)
    {
        ESP_LOGE(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (buffer == NULL || buffer_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = sdcard_validate_path(path);

    if (result != ESP_OK)
    {
        return result;
    }

    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", path);
        return ESP_FAIL;
    }

    size_t bytes_read = fread(buffer, 1U, buffer_size - 1U, file);
    buffer[bytes_read] = '\0';

    fclose(file);

    ESP_LOGI(TAG, "Read %u bytes from %s", (unsigned int)bytes_read, path);

    return ESP_OK;
}