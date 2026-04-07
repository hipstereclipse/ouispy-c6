/*
 * OUI-Spy C6 — LED status indicator
 *
 * Non-touch board (HAS_RGB_LED=1): WS2812 addressable RGB LED on PIN_RGB_LED.
 * Touch board     (HAS_RGB_LED=0): Animated detection border drawn around the
 *   LCD perimeter.  Multiple animation styles (pulse, flames, radiation, …)
 *   encode state information that the RGB LED would normally convey.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "app_common.h"

void led_ctrl_init(void);
void led_ctrl_set(uint8_t r, uint8_t g, uint8_t b);
void led_ctrl_set_forced(uint8_t r, uint8_t g, uint8_t b);
void led_ctrl_off(void);
void led_ctrl_pulse(uint8_t r, uint8_t g, uint8_t b, int period_ms);
void led_ctrl_set_effect_intensity(uint8_t intensity);

/* Smooth sine-wave breathing — starts a background task.
 * r,g,b = peak color; period_ms = full breath cycle time (good range: 2000-4000). */
void led_ctrl_breathe(uint8_t r, uint8_t g, uint8_t b, int period_ms);

/* Same as led_ctrl_breathe(), but ignores g_app.led_enabled so settings can preview visuals live. */
void led_ctrl_breathe_forced(uint8_t r, uint8_t g, uint8_t b, int period_ms);

/* Stop any running breathe task */
void led_ctrl_breathe_stop(void);

#if !HAS_RGB_LED
/*
 * Animated detection border — drawn around the screen perimeter.
 * The border is 3 px wide on each edge.  It occupies the outermost
 * pixels that the safe-area constants already reserve for padding.
 *
 * led_ctrl_border_clear() erases the border back to black so
 * content redraws don't leave fragments.
 */
#define LED_BORDER_THICK  3
void led_ctrl_border_clear(void);
#endif
