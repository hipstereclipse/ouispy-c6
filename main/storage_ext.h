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

void storage_ext_init(void);
bool storage_ext_is_available(void);
storage_status_t storage_ext_get_status(void);
const char *storage_ext_status_str(storage_status_t status);
uint32_t storage_ext_log_capacity_kb(void);
esp_err_t storage_ext_append_log(const char *kind, const char *message);
esp_err_t storage_ext_format(void);
