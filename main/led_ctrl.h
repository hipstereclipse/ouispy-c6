/*
 * OUI-Spy C6 — WS2812 RGB LED control
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void led_ctrl_init(void);
void led_ctrl_set(uint8_t r, uint8_t g, uint8_t b);
void led_ctrl_set_forced(uint8_t r, uint8_t g, uint8_t b);
void led_ctrl_off(void);
void led_ctrl_pulse(uint8_t r, uint8_t g, uint8_t b, int period_ms);

/* Smooth sine-wave breathing — starts a background task.
 * r,g,b = peak color; period_ms = full breath cycle time (good range: 2000-4000). */
void led_ctrl_breathe(uint8_t r, uint8_t g, uint8_t b, int period_ms);

/* Stop any running breathe task */
void led_ctrl_breathe_stop(void);
