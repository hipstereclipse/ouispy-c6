/*
 * OUI-Spy C6 — Optional microSD-backed storage
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

void storage_ext_init(void);
bool storage_ext_is_available(void);
uint32_t storage_ext_log_capacity_kb(void);
esp_err_t storage_ext_append_log(const char *kind, const char *message);
