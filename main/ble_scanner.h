/*
 * OUI-Spy C6 — BLE scanner using NimBLE
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef void (*ble_scan_cb_t)(const uint8_t *addr, int8_t rssi,
                              const uint8_t *adv_data, uint8_t adv_len,
                              const uint8_t *name, uint8_t name_len);

void ble_scanner_init(void);
void ble_scanner_start(ble_scan_cb_t cb, uint16_t interval_ms,
                       uint16_t window_ms, bool passive);
void ble_scanner_stop(void);
bool ble_scanner_is_running(void);
