/*
 * OUI-Spy C6 — Common utilities implementation
 * SPDX-License-Identifier: MIT
 */
#include "app_common.h"
#include "storage_ext.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

app_state_t g_app;

static esp_log_level_t serial_log_level_from_pref(uint8_t pref)
{
    switch (pref) {
    case SERIAL_LOG_ERROR:
        return ESP_LOG_ERROR;
    case SERIAL_LOG_WARN:
        return ESP_LOG_WARN;
    case SERIAL_LOG_DEBUG:
        return ESP_LOG_DEBUG;
    case SERIAL_LOG_VERBOSE:
        return ESP_LOG_VERBOSE;
    case SERIAL_LOG_INFO:
    default:
        return ESP_LOG_INFO;
    }
}

void app_mode_ap_credentials(app_mode_t mode, const char **ssid, const char **pass, uint8_t *channel)
{
    static const struct {
        const char *ssid;
        const char *pass;
        uint8_t channel;
    } mode_cfg[MODE_COUNT] = {
        [MODE_SELECT]     = {"ouispy-c6",   "ouispy123",   6},
        [MODE_FLOCK_YOU]  = {"flockyou-c6", "flockyou123", 6},
        [MODE_FOX_HUNTER] = {"foxhunt-c6",  "foxhunt123",  6},
        [MODE_SKY_SPY]    = {"skyspy-c6",   "skyspy1234",  6},
        [MODE_SETTINGS]   = {"ouispy-c6",   "ouispy123",   6},
    };

    if (g_app.single_ap_name_enabled) {
        if (ssid) *ssid = "UniSpy-C6";
        if (pass) *pass = "ouispy123";
        if (channel) *channel = 6;
        return;
    }

    if (mode < 0 || mode >= MODE_COUNT) {
        mode = MODE_SELECT;
    }

    if (ssid) *ssid = mode_cfg[mode].ssid;
    if (pass) *pass = mode_cfg[mode].pass;
    if (channel) *channel = mode_cfg[mode].channel;
}

void app_state_init(void)
{
    memset(&g_app, 0, sizeof(g_app));
    g_app.current_mode    = MODE_SELECT;
    g_app.requested_mode  = MODE_SELECT;
    g_app.lcd_brightness  = 200;
    g_app.sound_enabled   = true;
    g_app.led_enabled     = true;
    g_app.ap_broadcast_enabled = true;
    g_app.single_ap_name_enabled = false;
    g_app.display_sleep_timeout_sec = 60;
    g_app.menu_led_color = MENU_LED_TOPAZ;
    g_app.sound_profile_flock = SOUND_PROFILE_STANDARD;
    g_app.sound_profile_fox = SOUND_PROFILE_SONAR;
    g_app.sound_profile_sky = SOUND_PROFILE_CHIRP;
    g_app.shortcut_mode_btn = SHORTCUT_NEXT_MODE;
    g_app.shortcut_action_btn = SHORTCUT_FOX_LED_MODE;
    g_app.shortcut_back_btn = SHORTCUT_MODE_SELECT;
    g_app.use_microsd_logs = false;
    g_app.advanced_logging_enabled = false;
    g_app.log_general_enabled = true;
    g_app.log_flock_enabled = true;
    g_app.log_fox_enabled = true;
    g_app.log_sky_enabled = true;
    g_app.log_gps_enabled = true;
    g_app.log_saved_fox_enabled = true;
    g_app.gps_diagnostics_enabled = false;
    g_app.web_diagnostics_enabled = false;
    g_app.advanced_serial_logging_enabled = false;
    g_app.serial_log_verbosity = SERIAL_LOG_INFO;
    g_app.gps_tagging_enabled = false;
    g_app.gps_client_ready = false;
    g_app.gps_client_ready_ms = 0;
    g_app.sky_tracked_drone_idx = -1;
    g_app.fox_target_radius_m = 60.0f;
    g_app.sky_tracked_radius_m = 120.0f;
    g_app.fox_target_gps_samples = 0;
    g_app.sky_tracked_gps_samples = 0;
    g_app.fox_target_weight_sum = 0.0f;
    g_app.shared_map_pin_count = 0;
    g_app.local_map_open = false;
    g_app.local_map_zoom_idx = 2;
    g_app.last_input_ms = uptime_ms();
    g_app.display_sleeping = false;
    g_app.device_mutex    = xSemaphoreCreateMutex();
    g_app.drone_mutex     = xSemaphoreCreateMutex();
    g_app.map_mutex       = xSemaphoreCreateMutex();
    
    /* Set drone capacity based on microSD availability */
    g_app.max_drones_allowed = storage_ext_is_available() ? MAX_DRONES : MAX_DRONES_NO_SD;
    app_apply_runtime_logging_prefs();
}

void app_apply_runtime_logging_prefs(void)
{
    esp_log_level_set("*", serial_log_level_from_pref(g_app.serial_log_verbosity));
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
