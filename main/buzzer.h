/*
 * OUI-Spy C6 — Buzzer/speaker driver via LEDC PWM
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void buzzer_init(void);
void buzzer_tone(uint32_t freq_hz, uint32_t duration_ms);
void buzzer_beep(uint32_t duration_ms);
void buzzer_off(void);
void buzzer_melody_boot(void);
void buzzer_melody_flock(void);
void buzzer_melody_fox(void);
void buzzer_melody_sky(void);
