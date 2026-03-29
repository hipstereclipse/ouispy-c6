/*
 * OUI-Spy C6 — Optional microSD-backed storage
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* microSD card status */
typedef enum {
    STORAGE_STATUS_NOT_FOUND = 0,    /* No card detected */
    STORAGE_STATUS_NEEDS_FORMAT = 1, /* Card detected but not formatted */
    STORAGE_STATUS_AVAILABLE = 2,    /* Mounted and ready to use */
} storage_status_t;

typedef enum {
    STORAGE_LOG_BUCKET_EVENTS = 0,
    STORAGE_LOG_BUCKET_IDENTITY,
    STORAGE_LOG_BUCKET_DIAGNOSTICS,
} storage_log_bucket_t;

typedef struct {
    uint32_t ts;
    storage_log_bucket_t bucket;
    char kind[24];
    char message[224];
} storage_log_entry_t;

void storage_ext_init(void);
bool storage_ext_is_available(void);
storage_status_t storage_ext_get_status(void);
const char *storage_ext_status_str(storage_status_t status);
bool storage_ext_status_is_present(storage_status_t status);
uint32_t storage_ext_log_capacity_kb(void);
uint32_t storage_ext_total_kb(void);
uint32_t storage_ext_used_kb(void);
uint32_t storage_ext_free_kb(void);
bool storage_ext_logging_active(void);
bool storage_ext_logging_blocked(void);
esp_err_t storage_ext_append_log(const char *kind, const char *message);
esp_err_t storage_ext_append_identity(const char *kind, const char *message);
esp_err_t storage_ext_append_diagnostic(const char *kind, const char *message);
esp_err_t storage_ext_format(void);
esp_err_t storage_ext_clear_logs(void);
esp_err_t storage_ext_delete_logs(void);
esp_err_t storage_ext_scramble_logs(void);
int  storage_ext_read_recent_lines(char lines[][64], int max_lines);
int  storage_ext_get_recent_entries(storage_log_entry_t *entries, int max_entries);
