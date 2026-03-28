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
static sdmmc_card_t *s_card = NULL;

static void ensure_log_dir(void)
{
    struct stat st = {0};
    if (stat("/sdcard/ouispy_logs", &st) != 0) {
        mkdir("/sdcard/ouispy_logs", 0777);
    }
}

void storage_ext_init(void)
{
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
        ensure_log_dir();
        ESP_LOGI(TAG, "microSD mounted");
        return;
    }

    s_sd_ready = false;
    s_card = NULL;
    ESP_LOGW(TAG, "microSD mount unavailable: %s", esp_err_to_name(err));
}

bool storage_ext_is_available(void)
{
    return s_sd_ready;
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
