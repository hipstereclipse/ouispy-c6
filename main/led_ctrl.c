/*
 * OUI-Spy C6 — WS2812 RGB LED control (RMT, no DMA on C6)
 * SPDX-License-Identifier: MIT
 */
#include "led_ctrl.h"
#include "app_common.h"
#include "led_strip.h"
#include "esp_timer.h"
#include <math.h>

static const char *TAG = "led";
static led_strip_handle_t s_led = NULL;

/* Breathing task state */
static TaskHandle_t s_breathe_task = NULL;
static volatile bool s_breathe_running = false;
static uint8_t s_breathe_r, s_breathe_g, s_breathe_b;
static int s_breathe_period_ms;

static void led_ctrl_write(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

void led_ctrl_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num         = PIN_RGB_LED,
        .max_leds               = 1,
        .led_model              = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,  /* No DMA on ESP32-C6 RMT */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt_cfg, &s_led));
    led_strip_clear(s_led);
    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", PIN_RGB_LED);
}

void led_ctrl_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led || !g_app.led_enabled) return;
    led_ctrl_write(r, g, b);
}

void led_ctrl_set_forced(uint8_t r, uint8_t g, uint8_t b)
{
    led_ctrl_write(r, g, b);
}

void led_ctrl_off(void)
{
    if (!s_led) return;
    led_strip_clear(s_led);
}

void led_ctrl_pulse(uint8_t r, uint8_t g, uint8_t b, int period_ms)
{
    /* Simple on/off pulse — call repeatedly from a task */
    static bool on = false;
    on = !on;
    if (on) {
        led_ctrl_set(r, g, b);
    } else {
        led_ctrl_off();
    }
}

/* ── Smooth sine-wave breathing task ── */
static void breathe_task(void *arg)
{
    const int step_ms = 30;

    while (s_breathe_running) {
        if (!g_app.led_enabled) {
            led_ctrl_off();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        float phase = (float)(now % (uint32_t)s_breathe_period_ms)
                    / (float)s_breathe_period_ms;
        /* Sine curve: smooth 0→1→0 */
        float brightness = (1.0f - cosf(phase * 2.0f * 3.14159265f)) * 0.5f;

        /* Gamma for perceptual smoothness */
        brightness = brightness * brightness;

        /* Floor so LED never fully extinguishes */
        float floor = 0.04f;
        brightness = floor + brightness * (1.0f - floor);

        uint8_t r = (uint8_t)(s_breathe_r * brightness);
        uint8_t g = (uint8_t)(s_breathe_g * brightness);
        uint8_t b = (uint8_t)(s_breathe_b * brightness);

        led_ctrl_write(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    led_ctrl_off();
    s_breathe_task = NULL;
    vTaskDelete(NULL);
}

void led_ctrl_breathe(uint8_t r, uint8_t g, uint8_t b, int period_ms)
{
    led_ctrl_breathe_stop();

    s_breathe_r = r;
    s_breathe_g = g;
    s_breathe_b = b;
    s_breathe_period_ms = period_ms > 200 ? period_ms : 200;
    s_breathe_running = true;

    if (xTaskCreate(breathe_task, "led_breathe", 2048, NULL, 2,
                    &s_breathe_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create breathe task");
        s_breathe_running = false;
        s_breathe_task = NULL;
    }
}

void led_ctrl_breathe_stop(void)
{
    s_breathe_running = false;
    if (s_breathe_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
        s_breathe_task = NULL;
    }
}
