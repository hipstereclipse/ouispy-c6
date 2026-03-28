/*
 * OUI-Spy C6 — WS2812 RGB LED control
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

void led_ctrl_init(void);
void led_ctrl_set(uint8_t r, uint8_t g, uint8_t b);
void led_ctrl_set_forced(uint8_t r, uint8_t g, uint8_t b);
void led_ctrl_off(void);
void led_ctrl_pulse(uint8_t r, uint8_t g, uint8_t b, int period_ms);
