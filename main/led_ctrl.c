/*
 * OUI-Spy C6 — WS2812 RGB LED control (RMT, no DMA on C6)
 * SPDX-License-Identifier: MIT
 */
#include "led_ctrl.h"
#include "app_common.h"
#include "led_strip.h"

static const char *TAG = "led";
static led_strip_handle_t s_led = NULL;

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
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
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
