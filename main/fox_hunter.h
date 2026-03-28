/*
 * OUI-Spy C6 — Fox Hunter: BLE proximity tracker with RSSI-mapped buzzer
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void fox_hunter_start(void);
void fox_hunter_stop(void);
void fox_hunter_set_target(const uint8_t mac[6]);
void fox_hunter_set_target_from_flock(int device_index);
bool fox_hunter_has_target(void);
int  fox_hunter_registry_add(const uint8_t mac[6], const char *label);
int  fox_hunter_registry_remove(int index);
void fox_hunter_registry_select(int index);
