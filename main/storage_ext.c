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
#include <string.h>
#include <sys/stat.h>

#define LOG_RING_SIZE                96
#define LOG_KIND_MAX_CHARS           23
#define LOG_MESSAGE_MAX_CHARS        223

static const char *TAG = "storage_ext";
static bool s_sd_ready = false;
static storage_status_t s_sd_status = STORAGE_STATUS_NOT_FOUND;
static sdmmc_card_t *s_card = NULL;
static int64_t s_last_probe_us = 0;
static storage_log_entry_t s_log_ring[LOG_RING_SIZE];
static int s_log_ring_head = 0;
static int s_log_ring_count = 0;

#define SD_RUNTIME_PROBE_INTERVAL_US (2000000LL)
#define LOG_MIN_FREE_KB              128U
#define EVENT_LOG_MAX_BYTES          (512U * 1024U)
#define EVENT_LOG_KEEP_BYTES         (256U * 1024U)

static const char *EVENT_LOG_PATH = "/sdcard/ouispy_logs/events.log";
static const char *IDENTITY_LOG_PATH = "/sdcard/ouispy_logs/identity.log";
static const char *DIAGNOSTIC_LOG_PATH = "/sdcard/ouispy_logs/diagnostics.log";

static void ensure_log_dir(void);
static void refresh_card_runtime_status(void);
static void prune_noncritical_logs_if_needed(void);

static bool log_message_contains_device_identity(const char *message)
{
    if (!message || !message[0]) return false;
    if (strstr(message, "mac=") != NULL) return true;

    int colon_count = 0;
    for (const char *cur = message; *cur; cur++) {
        if (*cur == ':') colon_count++;
    }
    return colon_count >= 5;
}

static bool log_kind_is_diagnostic(const char *kind)
{
    if (!kind || !kind[0]) return false;
    return strncmp(kind, "diag_", 5) == 0
        || strcmp(kind, "web") == 0
        || strcmp(kind, "gps") == 0
        || strcmp(kind, "system") == 0
        || strcmp(kind, "http") == 0;
}

static storage_log_bucket_t log_bucket_for_entry(const char *kind, const char *message)
{
    if (log_kind_is_diagnostic(kind)) {
        return STORAGE_LOG_BUCKET_DIAGNOSTICS;
    }
    if ((kind && strcmp(kind, "identity") == 0) || log_message_contains_device_identity(message)) {
        return STORAGE_LOG_BUCKET_IDENTITY;
    }
    return STORAGE_LOG_BUCKET_EVENTS;
}

static void log_ring_append(storage_log_bucket_t bucket, uint32_t ts, const char *kind, const char *message)
{
    storage_log_entry_t *entry = &s_log_ring[s_log_ring_head];
    memset(entry, 0, sizeof(*entry));
    entry->ts = ts;
    entry->bucket = bucket;
    snprintf(entry->kind, sizeof(entry->kind), "%.*s", LOG_KIND_MAX_CHARS, kind ? kind : "event");
    snprintf(entry->message, sizeof(entry->message), "%.*s", LOG_MESSAGE_MAX_CHARS, message ? message : "");

    s_log_ring_head = (s_log_ring_head + 1) % LOG_RING_SIZE;
    if (s_log_ring_count < LOG_RING_SIZE) {
        s_log_ring_count++;
    }
}

static void trim_log_keep_recent(const char *path, size_t keep_bytes)
{
    struct stat st = {0};
    if (!path || stat(path, &st) != 0 || (size_t)st.st_size <= keep_bytes) return;

    FILE *src = fopen(path, "rb");
    if (!src) return;

    FILE *dst = fopen("/sdcard/ouispy_logs/events.tmp", "wb");
    if (!dst) {
        fclose(src);
        return;
    }

    long start = (long)((size_t)st.st_size - keep_bytes);
    if (start > 0) {
        fseek(src, start, SEEK_SET);
        int ch;
        while ((ch = fgetc(src)) != EOF) {
            if (ch == '\n') break;
        }
    }

    char buf[256];
    size_t read_len;
    while ((read_len = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, read_len, dst);
    }

    fclose(src);
    fclose(dst);
    remove(path);
    rename("/sdcard/ouispy_logs/events.tmp", path);
}

static void prune_noncritical_logs_if_needed(void)
{
    refresh_card_runtime_status();
    if (!s_sd_ready) return;

    uint32_t free_kb = storage_ext_free_kb();
    struct stat st = {0};
    size_t event_log_size = (stat(EVENT_LOG_PATH, &st) == 0) ? (size_t)st.st_size : 0U;

    if (free_kb < LOG_MIN_FREE_KB || event_log_size > EVENT_LOG_MAX_BYTES) {
        trim_log_keep_recent(EVENT_LOG_PATH, EVENT_LOG_KEEP_BYTES);
    }
}

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
    memset(s_log_ring, 0, sizeof(s_log_ring));
    s_log_ring_head = 0;
    s_log_ring_count = 0;
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

bool storage_ext_logging_active(void)
{
    return g_app.use_microsd_logs && (storage_ext_get_status() == STORAGE_STATUS_AVAILABLE);
}

bool storage_ext_logging_blocked(void)
{
    return g_app.use_microsd_logs && (storage_ext_get_status() != STORAGE_STATUS_AVAILABLE);
}

static esp_err_t storage_ext_append_bucketed(storage_log_bucket_t bucket, const char *kind, const char *message)
{
    if (!kind || !message) return ESP_ERR_INVALID_ARG;
    uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    log_ring_append(bucket, now_sec, kind, message);

    refresh_card_runtime_status();
    if (!g_app.use_microsd_logs || !s_sd_ready) return ESP_OK;

    ensure_log_dir();
    prune_noncritical_logs_if_needed();

    const char *path = EVENT_LOG_PATH;
    if (bucket == STORAGE_LOG_BUCKET_IDENTITY) {
        path = IDENTITY_LOG_PATH;
    } else if (bucket == STORAGE_LOG_BUCKET_DIAGNOSTICS) {
        path = DIAGNOSTIC_LOG_PATH;
    }
    FILE *f = fopen(path, "a");
    if (!f) return ESP_FAIL;

    fprintf(f, "%lu,%s,%s\n", (unsigned long)now_sec, kind, message);
    fclose(f);
    return ESP_OK;
}

esp_err_t storage_ext_append_log(const char *kind, const char *message)
{
    return storage_ext_append_bucketed(log_bucket_for_entry(kind, message), kind, message);
}

esp_err_t storage_ext_append_identity(const char *kind, const char *message)
{
    return storage_ext_append_bucketed(STORAGE_LOG_BUCKET_IDENTITY,
                                       kind ? kind : "identity",
                                       message ? message : "");
}

esp_err_t storage_ext_append_diagnostic(const char *kind, const char *message)
{
    if (!g_app.advanced_logging_enabled) return ESP_OK;
    return storage_ext_append_log(kind ? kind : "diag_event", message ? message : "");
}

int storage_ext_get_recent_entries(storage_log_entry_t *entries, int max_entries)
{
    if (!entries || max_entries <= 0) return 0;

    int count = (s_log_ring_count < max_entries) ? s_log_ring_count : max_entries;
    for (int i = 0; i < count; i++) {
        int src_idx = (s_log_ring_head - 1 - i + LOG_RING_SIZE) % LOG_RING_SIZE;
        entries[i] = s_log_ring[src_idx];
    }
    return count;
}

int storage_ext_read_recent_lines(char lines[][64], int max_lines)
{
    if (!lines || max_lines <= 0) return 0;
    refresh_card_runtime_status();
    if (!s_sd_ready) return 0;

    /* Try event log first, then identity log */
    const char *paths[] = {EVENT_LOG_PATH, IDENTITY_LOG_PATH};
    int total = 0;

    for (int p = 0; p < 2 && total < max_lines; p++) {
        struct stat st = {0};
        if (stat(paths[p], &st) != 0 || st.st_size == 0) continue;

        FILE *f = fopen(paths[p], "rb");
        if (!f) continue;

        /* Read the tail of the file (last ~4KB) */
        long tail_size = 4096;
        if (st.st_size > tail_size) {
            fseek(f, -tail_size, SEEK_END);
            /* Skip partial first line */
            int ch;
            while ((ch = fgetc(f)) != EOF) {
                if (ch == '\n') break;
            }
        }

        /* Read lines into a circular buffer */
        char buf[64];
        int ring_start = total;
        int ring_count = 0;
        int ring_cap = max_lines - total;

        while (fgets(buf, sizeof(buf), f)) {
            /* Strip trailing newline */
            int len = (int)strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                buf[--len] = '\0';
            if (len == 0) continue;

            if (ring_count < ring_cap) {
                strncpy(lines[ring_start + ring_count], buf, 63);
                lines[ring_start + ring_count][63] = '\0';
                ring_count++;
            } else {
                /* Shift up: discard oldest, append newest */
                for (int i = 0; i < ring_cap - 1; i++) {
                    memcpy(lines[ring_start + i], lines[ring_start + i + 1], 64);
                }
                strncpy(lines[ring_start + ring_cap - 1], buf, 63);
                lines[ring_start + ring_cap - 1][63] = '\0';
            }
        }
        fclose(f);
        total += ring_count;
    }

    return total;
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

