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
static int64_t s_last_probe_us = 0;

#define SD_RUNTIME_PROBE_INTERVAL_US (2000000LL)

static void ensure_log_dir(void);

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

static esp_err_t mount_sd_card(bool format_if_mount_failed, bool log_result)
{
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
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
        if (log_result) {
            ESP_LOGI(TAG, "microSD mounted");
        }
        return ESP_OK;
    }

    s_sd_ready = false;
    s_card = NULL;
    s_sd_status = classify_mount_error(err);
    refresh_storage_dependent_limits();
    if (log_result) {
        if (s_sd_status == STORAGE_STATUS_NEEDS_FORMAT) {
            ESP_LOGW(TAG, "microSD card detected but mount failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "microSD card not found");
        }
    }
    return err;
}

static void mark_card_missing(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    }
    s_sd_ready = false;
    s_card = NULL;
    s_sd_status = STORAGE_STATUS_NOT_FOUND;
    refresh_storage_dependent_limits();
}

static void refresh_card_runtime_status(void)
{
    int64_t now_us = esp_timer_get_time();

    if (!s_sd_ready || !s_card) {
        if ((now_us - s_last_probe_us) >= SD_RUNTIME_PROBE_INTERVAL_US) {
            s_last_probe_us = now_us;
            mount_sd_card(false, false);
        }
        return;
    }

    /* CMD13 card-status check; if this fails the card was removed or bus is gone. */
    esp_err_t err = sdmmc_get_status(s_card);
    if (err == ESP_OK) {
        s_sd_status = STORAGE_STATUS_AVAILABLE;
        return;
    }

    ESP_LOGW(TAG, "microSD runtime status check failed: %s", esp_err_to_name(err));
    mark_card_missing();
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
    s_last_probe_us = 0;
    refresh_storage_dependent_limits();

    mount_sd_card(false, true);
}

bool storage_ext_is_available(void)
{
    refresh_card_runtime_status();
    return s_sd_ready;
}

storage_status_t storage_ext_get_status(void)
{
    refresh_card_runtime_status();
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

uint32_t storage_ext_free_kb(void)
{
    refresh_card_runtime_status();
    if (!s_sd_ready) {
        return 0;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes) != ESP_OK) {
        return 0;
    }

    return (uint32_t)((uint64_t)free_bytes / 1024ULL);
}

esp_err_t storage_ext_append_log(const char *kind, const char *message)
{
    if (!kind || !message) return ESP_ERR_INVALID_ARG;
    refresh_card_runtime_status();
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

    /* Format with automatic mount on success */
    esp_err_t err = mount_sd_card(true, false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "microSD formatted and mounted successfully");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "microSD format failed: %s", esp_err_to_name(err));
    return err;
}

