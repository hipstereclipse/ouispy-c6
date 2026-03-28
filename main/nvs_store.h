/*
 * OUI-Spy C6 — NVS persistent storage
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "app_common.h"

void nvs_store_init(void);
void nvs_store_save_mode(app_mode_t mode);
app_mode_t nvs_store_load_mode(void);
void nvs_store_save_fox_target(const uint8_t mac[6]);
int  nvs_store_load_fox_target(uint8_t mac[6]);
void nvs_store_save_fox_registry(void);
void nvs_store_load_fox_registry(void);
void nvs_store_save_prefs(void);
void nvs_store_load_prefs(void);
