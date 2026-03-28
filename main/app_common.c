/*
 * OUI-Spy C6 — Common utilities implementation
 * SPDX-License-Identifier: MIT
 */
#include "app_common.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>

app_state_t g_app;

void app_state_init(void)
{
    memset(&g_app, 0, sizeof(g_app));
    g_app.current_mode    = MODE_SELECT;
    g_app.requested_mode  = MODE_SELECT;
    g_app.lcd_brightness  = 200;
    g_app.sound_enabled   = true;
    g_app.led_enabled     = true;
    g_app.ap_broadcast_enabled = true;
    g_app.display_sleep_timeout_sec = 60;
    g_app.menu_led_color = MENU_LED_GOLD;
    g_app.sound_profile_flock = SOUND_PROFILE_STANDARD;
    g_app.sound_profile_fox = SOUND_PROFILE_SONAR;
    g_app.sound_profile_sky = SOUND_PROFILE_CHIRP;
    g_app.shortcut_mode_btn = SHORTCUT_NEXT_MODE;
    g_app.shortcut_action_btn = SHORTCUT_FOX_LED_MODE;
    g_app.shortcut_back_btn = SHORTCUT_MODE_SELECT;
    g_app.use_microsd_logs = false;
    g_app.last_input_ms = uptime_ms();
    g_app.display_sleeping = false;
    g_app.device_mutex    = xSemaphoreCreateMutex();
    g_app.drone_mutex     = xSemaphoreCreateMutex();
}

uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void mac_to_str(const uint8_t mac[6], char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

int mac_from_str(const char *str, uint8_t mac[6])
{
    unsigned int tmp[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        if (tmp[i] > 0xFF) return -1;
        mac[i] = (uint8_t)tmp[i];
    }
    return 0;
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);   /* byte-swap: ESP32-C6 LE → ST7789 BE */
}
