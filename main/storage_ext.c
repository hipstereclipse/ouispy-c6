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
static bool s_sd_detected = false;  /* Card detected but may not be ready */
static sdmmc_card_t *s_card = NULL;

static void ensure_log_dir(void)
{
    struct stat st = {0};
    if (stat("/sdcard/ouispy_logs", &st) != 0) {
        mkdir("/sdcard/ouispy_logs", 0777);
    }
}

static bool card_is_inserted(void)
{
    /* Try to detect the card without mounting */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 1000;  /* Low frequency for detection */

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SPI2_HOST;
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.gpio_cd = SDSPI_SLOT_NO_CD;
    slot_cfg.gpio_wp = SDSPI_SLOT_NO_WP;

    sdmmc_card_t card = {0};
    esp_err_t err = sdmmc_card_init(&host, &card);
    (void)slot_cfg;  /* Silence unused variable warning */
    if (err == ESP_OK) {
        return true;
    }
    return false;
}

void storage_ext_init(void)
{
    /* Check if card is physically inserted */
    s_sd_detected = card_is_inserted();

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
        s_sd_detected = true;
        ensure_log_dir();
        ESP_LOGI(TAG, "microSD mounted");
        return;
    }

    s_sd_ready = false;
    s_card = NULL;
    if (s_sd_detected) {
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
    if (s_sd_ready) {
        return STORAGE_STATUS_AVAILABLE;
    }
    if (s_sd_detected) {
        return STORAGE_STATUS_NEEDS_FORMAT;
    }
    return STORAGE_STATUS_NOT_FOUND;
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
    if (!s_sd_detected) {
        ESP_LOGE(TAG, "No card detected to format");
        return ESP_ERR_INVALID_STATE;
    }

    /* Unmount if currently mounted */
    if (s_sd_ready) {
        esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        s_sd_ready = false;
        s_card = NULL;
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
        s_sd_detected = true;
        ensure_log_dir();
        ESP_LOGI(TAG, "microSD formatted and mounted successfully");
        return ESP_OK;
    }

    s_sd_ready = false;
    s_card = NULL;
    ESP_LOGE(TAG, "microSD format failed: %s", esp_err_to_name(err));
    return err;
}

