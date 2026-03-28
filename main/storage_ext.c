/*
 * OUI-Spy C6 — Optional microSD-backed storage
 * SPDX-License-Identifier: MIT
 */
#include "storage_ext.h"
#include "app_common.h"

#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "storage_ext";
static bool s_sd_ready = false;
static storage_status_t s_sd_status = STORAGE_STATUS_NOT_FOUND;
static sdmmc_card_t *s_card = NULL;

static void refresh_storage_dependent_limits(void)
{
    g_app.max_drones_allowed = s_sd_ready ? MAX_DRONES : MAX_DRONES_NO_SD;
}

static storage_status_t classify_mount_error(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return STORAGE_STATUS_AVAILABLE;
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_RESPONSE:
        return STORAGE_STATUS_NOT_FOUND;
    default:
        return STORAGE_STATUS_NEEDS_FORMAT;
    }
}

static void ensure_log_dir(void)
{
    struct stat st = {0};
    if (stat("/sdcard/ouispy_logs", &st) != 0) {
        mkdir("/sdcard/ouispy_logs", 0777);
    }
}

void storage_ext_init(void)
{
    s_sd_ready = false;
    s_sd_status = STORAGE_STATUS_NOT_FOUND;
    s_card = NULL;
    refresh_storage_dependent_limits();

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SPI2_HOST;
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.gpio_cd = SDSPI_SLOT_NO_CD;
    slot_cfg.gpio_wp = SDSPI_SLOT_NO_WP;

    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &s_card);
    if (err == ESP_OK) {
        s_sd_ready = true;
        s_sd_status = STORAGE_STATUS_AVAILABLE;
        refresh_storage_dependent_limits();
        ensure_log_dir();
        ESP_LOGI(TAG, "microSD mounted");
        return;
    }

    s_sd_ready = false;
    s_card = NULL;
    s_sd_status = classify_mount_error(err);
    refresh_storage_dependent_limits();
    if (s_sd_status == STORAGE_STATUS_NEEDS_FORMAT) {
        ESP_LOGW(TAG, "microSD card detected but mount failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "microSD card not found");
    }
}

bool storage_ext_is_available(void)
{
    return s_sd_ready;
}

storage_status_t storage_ext_get_status(void)
{
    return s_sd_status;
}

const char *storage_ext_status_str(storage_status_t status)
{
    switch (status) {
    case STORAGE_STATUS_NOT_FOUND:
        return "Not Found";
    case STORAGE_STATUS_NEEDS_FORMAT:
        return "Needs Format";
    case STORAGE_STATUS_AVAILABLE:
        return "Available";
    default:
        return "Unknown";
    }
}

bool storage_ext_status_is_present(storage_status_t status)
{
    return status == STORAGE_STATUS_AVAILABLE || status == STORAGE_STATUS_NEEDS_FORMAT;
}

uint32_t storage_ext_log_capacity_kb(void)
{
    if (s_sd_ready) {
        return 8192;
    }
    return 768;
}

esp_err_t storage_ext_append_log(const char *kind, const char *message)
{
    if (!kind || !message) return ESP_ERR_INVALID_ARG;
    if (!g_app.use_microsd_logs || !s_sd_ready) return ESP_ERR_INVALID_STATE;

    ensure_log_dir();
    FILE *f = fopen("/sdcard/ouispy_logs/events.log", "a");
    if (!f) return ESP_FAIL;

    uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    fprintf(f, "%lu,%s,%s\n", (unsigned long)now_sec, kind, message);
    fclose(f);
    return ESP_OK;
}

esp_err_t storage_ext_format(void)
{
    if (!storage_ext_status_is_present(s_sd_status)) {
        ESP_LOGE(TAG, "No card detected to format");
        return ESP_ERR_INVALID_STATE;
    }

    /* Unmount if currently mounted */
    if (s_sd_ready) {
        esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        s_sd_ready = false;
        s_card = NULL;
        refresh_storage_dependent_limits();
    }

    ESP_LOGI(TAG, "Starting microSD format...");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SPI2_HOST;
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.gpio_cd = SDSPI_SLOT_NO_CD;
    slot_cfg.gpio_wp = SDSPI_SLOT_NO_WP;

    /* Format with automatic mount on success */
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,  /* Format if FAT mount fails */
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &s_card);
    if (err == ESP_OK) {
        s_sd_ready = true;
        s_sd_status = STORAGE_STATUS_AVAILABLE;
        refresh_storage_dependent_limits();
        ensure_log_dir();
        ESP_LOGI(TAG, "microSD formatted and mounted successfully");
        return ESP_OK;
    }

    s_sd_ready = false;
    s_card = NULL;
    s_sd_status = classify_mount_error(err);
    refresh_storage_dependent_limits();
    ESP_LOGE(TAG, "microSD format failed: %s", esp_err_to_name(err));
    return err;
}

