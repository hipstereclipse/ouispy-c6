/*
 * OUI-Spy C6 — WiFi AP manager
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start_ap(const char *ssid, const char *password, uint8_t channel);
esp_err_t wifi_manager_stop(void);
uint8_t wifi_manager_client_count(void);
