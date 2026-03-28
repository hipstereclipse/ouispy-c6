/*
 * OUI-Spy C6 — Buzzer/speaker driver using LEDC PWM
 * Generates tones on PIN_BUZZER (GPIO18).
 * Supports both passive piezo buzzers (freq matters) and active buzzers
 * (any freq toggles the output).
 * SPDX-License-Identifier: MIT
 */
#include "buzzer.h"
#include "app_common.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

#define BUZZER_LEDC_TIMER   LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_1

static bool s_initialized = false;

void buzzer_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUZZER,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(PIN_BUZZER, 0);

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = BUZZER_LEDC_TIMER,
        .freq_hz         = 2000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BUZZER_LEDC_CHANNEL,
        .timer_sel  = BUZZER_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
    s_initialized = true;
}

void buzzer_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_initialized || !g_app.sound_enabled) return;
    if (freq_hz < 100) freq_hz = 100;
    if (freq_hz > 20000) freq_hz = 20000;

    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_TIMER, freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, 512); /* 50% duty */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);

    if (duration_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        buzzer_off();
    }
}

void buzzer_beep(uint32_t duration_ms)
{
    buzzer_tone(2000, duration_ms);
}

void buzzer_off(void)
{
    if (!s_initialized) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
}

/* Startup melodies — distinct per mode */
void buzzer_melody_boot(void)
{
    buzzer_tone(880, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    buzzer_tone(1100, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    buzzer_tone(1320, 150);
}

void buzzer_melody_flock(void)
{
    buzzer_tone(660, 80);
    vTaskDelay(pdMS_TO_TICKS(40));
    buzzer_tone(880, 80);
    vTaskDelay(pdMS_TO_TICKS(40));
    buzzer_tone(1100, 120);
}

void buzzer_melody_fox(void)
{
    buzzer_tone(440, 100);
    vTaskDelay(pdMS_TO_TICKS(80));
    buzzer_tone(660, 100);
    vTaskDelay(pdMS_TO_TICKS(80));
    buzzer_tone(880, 200);
}

void buzzer_melody_sky(void)
{
    buzzer_tone(1320, 80);
    vTaskDelay(pdMS_TO_TICKS(40));
    buzzer_tone(1100, 80);
    vTaskDelay(pdMS_TO_TICKS(40));
    buzzer_tone(880, 80);
    vTaskDelay(pdMS_TO_TICKS(40));
    buzzer_tone(1320, 200);
}
