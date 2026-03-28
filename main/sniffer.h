/*
 * OUI-Spy C6 — WiFi promiscuous sniffer for drone detection
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

typedef void (*sniffer_drone_cb_t)(const uint8_t *src_mac, int8_t rssi,
                                   const uint8_t *payload, uint16_t len,
                                   uint8_t protocol);

void sniffer_init(void);
void sniffer_start(sniffer_drone_cb_t cb);
void sniffer_stop(void);
