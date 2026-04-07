/*
 * OUI-Spy C6 — Optional microSD-backed storage
 * SPDX-License-Identifier: MIT
 */
#include "storage_ext.h"
#include "app_common.h"
#include "map_tile.h"

#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
static int64_t s_last_runtime_status_us = 0;
static int64_t s_last_space_refresh_us = 0;
static uint32_t s_total_kb_cached = 0;
static uint32_t s_free_kb_cached = 0;
static storage_log_entry_t s_log_ring[LOG_RING_SIZE];
static int s_log_ring_head = 0;
static int s_log_ring_count = 0;

#define SD_RUNTIME_PROBE_INTERVAL_US (2000000LL)
#define SD_RETRY_NEEDS_FORMAT_INTERVAL_US (30000000LL)
#define SD_RUNTIME_STATUS_CHECK_INTERVAL_US (1000000LL)
#define SD_SPACE_REFRESH_INTERVAL_US (1500000LL)
#define SD_SPI_MAX_FREQ_KHZ          4000
#define LOG_MIN_FREE_KB              128U
#define EVENT_LOG_MAX_BYTES          (512U * 1024U)
#define EVENT_LOG_KEEP_BYTES         (256U * 1024U)

static const char *EVENT_LOG_PATH = "/sdcard/ouispy_logs/events.log";
static const char *IDENTITY_LOG_PATH = "/sdcard/ouispy_logs/identity.log";
static const char *DIAGNOSTIC_LOG_PATH = "/sdcard/ouispy_logs/diagnostics.log";
static SemaphoreHandle_t s_storage_lock = NULL;

static void ensure_log_dir(void);
static void refresh_card_runtime_status(void);
static void prune_noncritical_logs_if_needed(void);

static void update_space_cache_locked(void)
{
    int64_t now_us = esp_timer_get_time();
    if (!s_sd_ready) {
        s_total_kb_cached = 0;
        s_free_kb_cached = 0;
        s_last_space_refresh_us = 0;
        return;
    }

    if (s_last_space_refresh_us != 0
        && (now_us - s_last_space_refresh_us) < SD_SPACE_REFRESH_INTERVAL_US) {
        return;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (!app_spi_bus_lock(pdMS_TO_TICKS(250))) {
        return;
    }
    esp_err_t info_err = esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes);
    app_spi_bus_unlock();
    if (info_err != ESP_OK) {
        return;
    }

    s_total_kb_cached = (uint32_t)(total_bytes / 1024ULL);
    s_free_kb_cached = (uint32_t)(free_bytes / 1024ULL);
    s_last_space_refresh_us = now_us;
}

static int64_t storage_probe_interval_us(void)
{
    return (s_sd_status == STORAGE_STATUS_NEEDS_FORMAT)
        ? SD_RETRY_NEEDS_FORMAT_INTERVAL_US
        : SD_RUNTIME_PROBE_INTERVAL_US;
}

static bool storage_ext_lock(TickType_t wait_ticks)
{
    if (!s_storage_lock) {
        s_storage_lock = xSemaphoreCreateRecursiveMutex();
        if (!s_storage_lock) {
            ESP_LOGE(TAG, "Failed to create storage lock");
            return false;
        }
    }

    return xSemaphoreTakeRecursive(s_storage_lock, wait_ticks) == pdTRUE;
}

static void storage_ext_unlock(void)
{
    if (s_storage_lock) {
        xSemaphoreGiveRecursive(s_storage_lock);
    }
}

static esp_vfs_fat_mount_config_t storage_mount_cfg(bool format_if_mount_failed)
{
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };

    return mount_cfg;
}

static void clear_log_ring(void)
{
    memset(s_log_ring, 0, sizeof(s_log_ring));
    s_log_ring_head = 0;
    s_log_ring_count = 0;
}

static esp_err_t truncate_log_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    fclose(f);
    return ESP_OK;
}

static esp_err_t delete_log_file(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) != 0) return ESP_OK;
    return remove(path) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t scramble_log_file(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return delete_log_file(path);
    }

    FILE *f = fopen(path, "r+b");
    if (!f) return ESP_FAIL;

    uint8_t buf[256];
    size_t remaining = (size_t)st.st_size;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        for (size_t i = 0; i < chunk; i += sizeof(uint32_t)) {
            uint32_t rnd = esp_random();
            size_t copy = (chunk - i) < sizeof(uint32_t) ? (chunk - i) : sizeof(uint32_t);
            memcpy(&buf[i], &rnd, copy);
        }
        if (fwrite(buf, 1, chunk, f) != chunk) {
            fclose(f);
            return ESP_FAIL;
        }
        remaining -= chunk;
    }

    fflush(f);
    fclose(f);
    return delete_log_file(path);
}

static esp_err_t apply_to_log_files(esp_err_t (*op)(const char *path))
{
    if (!op) return ESP_ERR_INVALID_ARG;

    refresh_card_runtime_status();
    if (!s_sd_ready) {
        clear_log_ring();
        return ESP_OK;
    }

    ensure_log_dir();
    esp_err_t result = ESP_OK;
    const char *paths[] = {EVENT_LOG_PATH, IDENTITY_LOG_PATH, DIAGNOSTIC_LOG_PATH};
    for (size_t i = 0; i < (sizeof(paths) / sizeof(paths[0])); i++) {
        esp_err_t err = op(paths[i]);
        if (err != ESP_OK) result = err;
    }
    clear_log_ring();
    return result;
}

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

static bool log_kind_matches(const char *kind, const char *name)
{
    return kind && name && strcmp(kind, name) == 0;
}

static bool log_kind_has_prefix(const char *kind, const char *prefix)
{
    return kind && prefix && strncmp(kind, prefix, strlen(prefix)) == 0;
}

static bool log_matches_gps_detail(const char *kind, const char *message)
{
    if (log_kind_matches(kind, "gps") || log_kind_has_prefix(kind, "gps_")) return true;
    if (!message) return false;
    return strstr(message, "gps_") != NULL
        || (strstr(message, "lat=") != NULL && strstr(message, "lon=") != NULL);
}

static bool log_matches_fox_source(const char *kind)
{
    return log_kind_matches(kind, "fox") || log_kind_has_prefix(kind, "fox_");
}

static bool log_matches_flock_source(const char *kind)
{
    return log_kind_matches(kind, "flock") || log_kind_has_prefix(kind, "flock_");
}

static bool log_matches_sky_source(const char *kind)
{
    return log_kind_matches(kind, "sky") || log_kind_has_prefix(kind, "sky_");
}

static bool storage_ext_log_allowed(storage_log_bucket_t bucket, const char *kind, const char *message)
{
    bool matched_applet = false;

    if (log_matches_gps_detail(kind, message) && !g_app.log_gps_enabled) {
        return false;
    }

    if (log_matches_fox_source(kind)) {
        matched_applet = true;
        if (!g_app.log_fox_enabled) return false;
    }
    if (log_matches_flock_source(kind)) {
        matched_applet = true;
        if (!g_app.log_flock_enabled) return false;
    }
    if (log_matches_sky_source(kind)) {
        matched_applet = true;
        if (!g_app.log_sky_enabled) return false;
    }

    if (bucket == STORAGE_LOG_BUCKET_IDENTITY) {
        bool fox_identity = log_matches_fox_source(kind) || log_message_contains_device_identity(message);
        if (fox_identity && !g_app.log_saved_fox_enabled) {
            return false;
        }
    }

    if (!matched_applet && !g_app.log_general_enabled) {
        return false;
    }

    return true;
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
    case ESP_ERR_INVALID_CRC:
    case ESP_FAIL:
        /* Generic probe/init failures are not reliable evidence of a bad filesystem. */
        ESP_LOGW(TAG, "mount error treated as NOT_FOUND: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return STORAGE_STATUS_NOT_FOUND;
    default:
        /* Card responded on SPI but filesystem is unreadable, unsupported, or corrupt */
        ESP_LOGW(TAG, "mount error classified as NEEDS_FORMAT: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return STORAGE_STATUS_NEEDS_FORMAT;
    }
}

static esp_err_t mount_sd_card(bool format_if_mount_failed, bool log_result)
{
    esp_vfs_fat_mount_config_t mount_cfg = storage_mount_cfg(format_if_mount_failed);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = SD_SPI_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SPI2_HOST;
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.gpio_cd = SDSPI_SLOT_NO_CD;
    slot_cfg.gpio_wp = SDSPI_SLOT_NO_WP;

    if (!app_spi_bus_lock(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &s_card);
    app_spi_bus_unlock();
    if (err == ESP_OK) {
        s_sd_ready = true;
        s_sd_status = STORAGE_STATUS_AVAILABLE;
        s_last_probe_us = esp_timer_get_time();
        s_last_runtime_status_us = s_last_probe_us;
        s_last_space_refresh_us = 0;
        update_space_cache_locked();
        refresh_storage_dependent_limits();
        ensure_log_dir();
        map_tile_invalidate_cache();
        if (log_result) {
            ESP_LOGI(TAG, "microSD mounted");
        }
        return ESP_OK;
    }

    s_sd_ready = false;
    s_card = NULL;
    s_sd_status = classify_mount_error(err);
    s_last_probe_us = esp_timer_get_time();
    s_last_runtime_status_us = 0;
    s_last_space_refresh_us = 0;
    s_total_kb_cached = 0;
    s_free_kb_cached = 0;
    refresh_storage_dependent_limits();
    if (log_result) {
        if (s_sd_status == STORAGE_STATUS_NEEDS_FORMAT) {
            ESP_LOGW(TAG, "microSD card detected but mount failed: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "Likely cause: filesystem is damaged, uses an unsupported format, or the card needs in-device formatting");
        } else {
            ESP_LOGW(TAG, "microSD card not ready: %s", esp_err_to_name(err));
        }
    }
    return err;
}

static void mark_card_missing(void)
{
    if (s_card) {
        if (app_spi_bus_lock(pdMS_TO_TICKS(1000))) {
            esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
            app_spi_bus_unlock();
        }
    }
    s_sd_ready = false;
    s_card = NULL;
    s_sd_status = STORAGE_STATUS_NOT_FOUND;
    s_total_kb_cached = 0;
    s_free_kb_cached = 0;
    s_last_runtime_status_us = 0;
    s_last_space_refresh_us = 0;
    refresh_storage_dependent_limits();
    map_tile_invalidate_cache();
}

static void refresh_card_runtime_status(void)
{
    int64_t now_us = esp_timer_get_time();
    int64_t probe_interval_us = storage_probe_interval_us();

    if (!s_sd_ready || !s_card) {
        if ((now_us - s_last_probe_us) >= probe_interval_us) {
            s_last_probe_us = now_us;
            mount_sd_card(false, false);
        }
        return;
    }

    /* CMD13 card-status check; if this fails the card was removed or bus is gone. */
    if ((now_us - s_last_runtime_status_us) < SD_RUNTIME_STATUS_CHECK_INTERVAL_US) {
        return;
    }

    s_last_runtime_status_us = now_us;
    if (!app_spi_bus_lock(pdMS_TO_TICKS(250))) {
        return;
    }
    esp_err_t err = sdmmc_get_status(s_card);
    app_spi_bus_unlock();
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
    if (!storage_ext_lock(portMAX_DELAY)) {
        return;
    }

    s_sd_ready = false;
    s_sd_status = STORAGE_STATUS_NOT_FOUND;
    s_card = NULL;
    s_last_probe_us = 0;
    s_last_runtime_status_us = 0;
    s_last_space_refresh_us = 0;
    s_total_kb_cached = 0;
    s_free_kb_cached = 0;
    memset(s_log_ring, 0, sizeof(s_log_ring));
    s_log_ring_head = 0;
    s_log_ring_count = 0;
    refresh_storage_dependent_limits();

    mount_sd_card(false, true);
    storage_ext_unlock();
}

void storage_ext_poll(void)
{
    if (!storage_ext_lock(pdMS_TO_TICKS(10))) {
        return;
    }
    refresh_card_runtime_status();
    if (s_sd_ready) {
        update_space_cache_locked();
    }
    storage_ext_unlock();
}

bool storage_ext_is_available(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return false;
    }
    refresh_card_runtime_status();
    bool available = s_sd_ready;
    storage_ext_unlock();
    return available;
}

storage_status_t storage_ext_get_status(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return STORAGE_STATUS_NOT_FOUND;
    }
    refresh_card_runtime_status();
    storage_status_t status = s_sd_status;
    storage_ext_unlock();
    return status;
}

storage_status_t storage_ext_get_status_cached(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return STORAGE_STATUS_NOT_FOUND;
    }
    storage_status_t status = s_sd_status;
    storage_ext_unlock();
    return status;
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
    if (!storage_ext_lock(portMAX_DELAY)) {
        return 768;
    }
    if (s_sd_ready) {
        storage_ext_unlock();
        return 8192;
    }
    storage_ext_unlock();
    return 768;
}

uint32_t storage_ext_total_kb(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return 0;
    }
    refresh_card_runtime_status();
    if (!s_sd_ready) {
        storage_ext_unlock();
        return 0;
    }

    update_space_cache_locked();
    uint32_t total_kb = s_total_kb_cached;
    storage_ext_unlock();
    return total_kb;
}

uint32_t storage_ext_used_kb(void)
{
    uint32_t total_kb = storage_ext_total_kb();
    uint32_t free_kb = storage_ext_free_kb();
    if (total_kb <= free_kb) {
        return 0;
    }
    return total_kb - free_kb;
}

uint32_t storage_ext_free_kb(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return 0;
    }
    refresh_card_runtime_status();
    if (!s_sd_ready) {
        storage_ext_unlock();
        return 0;
    }

    update_space_cache_locked();
    uint32_t free_kb = s_free_kb_cached;
    storage_ext_unlock();
    return free_kb;
}

uint32_t storage_ext_total_kb_cached(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return 0;
    }
    uint32_t total_kb = s_total_kb_cached;
    storage_ext_unlock();
    return total_kb;
}

uint32_t storage_ext_used_kb_cached(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return 0;
    }
    uint32_t used_kb = (s_total_kb_cached > s_free_kb_cached) ? (s_total_kb_cached - s_free_kb_cached) : 0;
    storage_ext_unlock();
    return used_kb;
}

uint32_t storage_ext_free_kb_cached(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return 0;
    }
    uint32_t free_kb = s_free_kb_cached;
    storage_ext_unlock();
    return free_kb;
}

bool storage_ext_logging_active(void)
{
    return g_app.use_microsd_logs && (storage_ext_get_status_cached() == STORAGE_STATUS_AVAILABLE);
}

bool storage_ext_logging_blocked(void)
{
    return g_app.use_microsd_logs && (storage_ext_get_status_cached() != STORAGE_STATUS_AVAILABLE);
}

static esp_err_t storage_ext_append_bucketed(storage_log_bucket_t bucket, const char *kind, const char *message)
{
    if (!kind || !message) return ESP_ERR_INVALID_ARG;
    if (!storage_ext_log_allowed(bucket, kind, message)) return ESP_OK;
    uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    log_ring_append(bucket, now_sec, kind, message);

    if (!storage_ext_lock(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }

    refresh_card_runtime_status();
    if (!g_app.use_microsd_logs || !s_sd_ready) {
        storage_ext_unlock();
        return ESP_OK;
    }

    ensure_log_dir();
    prune_noncritical_logs_if_needed();

    const char *path = EVENT_LOG_PATH;
    if (bucket == STORAGE_LOG_BUCKET_IDENTITY) {
        path = IDENTITY_LOG_PATH;
    } else if (bucket == STORAGE_LOG_BUCKET_DIAGNOSTICS) {
        path = DIAGNOSTIC_LOG_PATH;
    }

    if (!app_spi_bus_lock(pdMS_TO_TICKS(500))) {
        storage_ext_unlock();
        return ESP_ERR_TIMEOUT;
    }
    FILE *f = fopen(path, "a");
    if (!f) {
        app_spi_bus_unlock();
        storage_ext_unlock();
        return ESP_FAIL;
    }

    fprintf(f, "%lu,%s,%s\n", (unsigned long)now_sec, kind, message);
    fclose(f);
    app_spi_bus_unlock();
    storage_ext_unlock();
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
    if (!storage_ext_lock(portMAX_DELAY)) {
        return 0;
    }
    refresh_card_runtime_status();
    if (!s_sd_ready) {
        storage_ext_unlock();
        return 0;
    }

    /* Try event log first, then identity log */
    const char *paths[] = {EVENT_LOG_PATH, IDENTITY_LOG_PATH};
    int total = 0;

    for (int p = 0; p < 2 && total < max_lines; p++) {
        struct stat st = {0};
        if (stat(paths[p], &st) != 0 || st.st_size == 0) continue;

        if (!app_spi_bus_lock(pdMS_TO_TICKS(500))) {
            continue;
        }
        FILE *f = fopen(paths[p], "rb");
        if (!f) {
            app_spi_bus_unlock();
            continue;
        }

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
        app_spi_bus_unlock();
        total += ring_count;
    }

    storage_ext_unlock();
    return total;
}

esp_err_t storage_ext_format(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Starting microSD format (status=%d)...", (int)s_sd_status);

    refresh_card_runtime_status();

    /* If already mounted, format in-place then remount */
    if (s_sd_ready && s_card) {
        esp_vfs_fat_mount_config_t format_cfg = storage_mount_cfg(false);
        if (!app_spi_bus_lock(pdMS_TO_TICKS(1000))) {
            storage_ext_unlock();
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err = esp_vfs_fat_sdcard_format_cfg("/sdcard", s_card, &format_cfg);
        app_spi_bus_unlock();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "microSD formatted in-place successfully");
            ensure_log_dir();
            clear_log_ring();
            storage_ext_unlock();
            return ESP_OK;
        }
        ESP_LOGW(TAG, "In-place format failed (%s), trying unmount+remount", esp_err_to_name(err));
        /* Fall through to unmount-and-reformat path */
    }

    if (s_sd_status == STORAGE_STATUS_NOT_FOUND) {
        ESP_LOGW(TAG, "microSD format aborted: card not detected");
        storage_ext_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    /* Unmount if currently mounted */
    if (s_sd_ready) {
        if (!app_spi_bus_lock(pdMS_TO_TICKS(1000))) {
            storage_ext_unlock();
            return ESP_ERR_TIMEOUT;
        }
        esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        app_spi_bus_unlock();
        s_sd_ready = false;
        s_card = NULL;
        refresh_storage_dependent_limits();
    }

    /* Only an explicit format request on a detected-but-unreadable card may auto-format on mount. */
    esp_err_t err = mount_sd_card(true, false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "microSD formatted and mounted successfully");
        clear_log_ring();
        storage_ext_unlock();
        return ESP_OK;
    }

    ESP_LOGE(TAG, "microSD format failed: %s", esp_err_to_name(err));
    storage_ext_unlock();
    return err;
}

esp_err_t storage_ext_clear_logs(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = apply_to_log_files(truncate_log_file);
    storage_ext_unlock();
    return err;
}

esp_err_t storage_ext_delete_logs(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = apply_to_log_files(delete_log_file);
    storage_ext_unlock();
    return err;
}

esp_err_t storage_ext_scramble_logs(void)
{
    if (!storage_ext_lock(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = apply_to_log_files(scramble_log_file);
    storage_ext_unlock();
    return err;
}

