/*
 * OUI-Spy C6 — Common utilities implementation
 * SPDX-License-Identifier: MIT
 */
#include "app_common.h"
#include "storage_ext.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

app_state_t g_app;

static const uint8_t FX_INTENSITY_STEPS[] = {48, 80, 112, 144, 176, 208, 240, 255};

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

void app_palette_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t rr = 255;
    uint8_t gg = 170;
    uint8_t bb = 20;

    switch (idx) {
    case MENU_LED_SAPPHIRE:     rr = 0;   gg = 82;  bb = 255; break;
    case MENU_LED_AMBER:        rr = 255; gg = 140; bb = 0;   break;
    case MENU_LED_ALEXANDRITE:  rr = 0;   gg = 140; bb = 120; break;
    case MENU_LED_RUBY:         rr = 255; gg = 0;   bb = 60;  break;
    case MENU_LED_PERIDOT:      rr = 140; gg = 255; bb = 0;   break;
    case MENU_LED_MOONSTONE:    rr = 140; gg = 210; bb = 255; break;
    case MENU_LED_DIAMOND:      rr = 255; gg = 255; bb = 255; break;
    case MENU_LED_AMETHYST:     rr = 170; gg = 70;  bb = 255; break;
    case MENU_LED_EMERALD:      rr = 0;   gg = 200; bb = 90;  break;
    case MENU_LED_CITRINE:      rr = 255; gg = 224; bb = 64;  break;
    case MENU_LED_CARNELIAN:    rr = 255; gg = 96;  bb = 32;  break;
    case MENU_LED_ROSE_QUARTZ:  rr = 255; gg = 120; bb = 180; break;
    case MENU_LED_TANZANITE:    rr = 92;  gg = 66;  bb = 220; break;
    case MENU_LED_LABRADORITE:  rr = 24;  gg = 164; bb = 200; break;
    case MENU_LED_CHALCOPYRITE: rr = 255; gg = 196; bb = 0;   break;
    case MENU_LED_MALACHITE:    rr = 0;   gg = 180; bb = 120; break;
    case MENU_LED_TOPAZ:
    default:
        break;
    }

    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
}

uint16_t app_palette_rgb565(uint8_t idx)
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    app_palette_rgb(idx, &r, &g, &b);
    return rgb565(r, g, b);
}

uint8_t app_mode_default_led_color(app_mode_t mode)
{
    switch (mode) {
    case MODE_FLOCK_YOU:
        return MENU_LED_RUBY;
    case MODE_FOX_HUNTER:
        return MENU_LED_AMBER;
    case MODE_SKY_SPY:
        return MENU_LED_EMERALD;
    case MODE_SETTINGS:
    case MODE_SELECT:
    default:
        return MENU_LED_TOPAZ;
    }
}

uint8_t app_mode_default_display_style(app_mode_t mode)
{
    switch (mode) {
    case MODE_FLOCK_YOU:
        return BORDER_STYLE_RADIATION;
    case MODE_FOX_HUNTER:
        return BORDER_STYLE_VIPER;
    case MODE_SKY_SPY:
        return BORDER_STYLE_SONAR;
    case MODE_SETTINGS:
    case MODE_SELECT:
    default:
        return BORDER_STYLE_PULSE;
    }
}

uint8_t app_mode_default_display_color(app_mode_t mode)
{
    switch (mode) {
    case MODE_FLOCK_YOU:
        return MENU_LED_RUBY;
    case MODE_FOX_HUNTER:
        return MENU_LED_CARNELIAN;
    case MODE_SKY_SPY:
        return MENU_LED_LABRADORITE;
    case MODE_SETTINGS:
    case MODE_SELECT:
    default:
        return MENU_LED_TOPAZ;
    }
}

static uint8_t app_mode_display_style_pref(app_mode_t mode)
{
    switch (mode) {
    case MODE_FLOCK_YOU:
        return g_app.detect_flock_high;
    case MODE_FOX_HUNTER:
        return g_app.detect_fox_high;
    case MODE_SKY_SPY:
        return g_app.detect_sky_high;
    case MODE_SETTINGS:
    case MODE_SELECT:
    default:
        return g_app.border_style;
    }
}

uint8_t app_mode_led_color(app_mode_t mode)
{
    switch (mode) {
    case MODE_FLOCK_YOU:
        return g_app.detect_flock_low;
    case MODE_FOX_HUNTER:
        return g_app.detect_fox_low;
    case MODE_SKY_SPY:
        return g_app.detect_sky_low;
    case MODE_SETTINGS:
    case MODE_SELECT:
    default:
        return g_app.menu_led_color;
    }
}

uint8_t app_mode_display_style(app_mode_t mode)
{
    uint8_t style = app_mode_display_style_pref(mode);

    if (style == BORDER_STYLE_DEFAULT || style >= BORDER_STYLE_COUNT) {
        return app_mode_default_display_style(mode);
    }

    return style;
}

uint8_t app_mode_display_behavior(app_mode_t mode)
{
    switch (mode) {
    case MODE_FLOCK_YOU:
        return g_app.detect_flock_behavior;
    case MODE_FOX_HUNTER:
        return g_app.detect_fox_behavior;
    case MODE_SKY_SPY:
        return g_app.detect_sky_behavior;
    case MODE_SETTINGS:
    case MODE_SELECT:
    default:
        return FX_BEHAVIOR_STANDARD;
    }
}

uint8_t app_mode_display_intensity(app_mode_t mode)
{
    switch (mode) {
    case MODE_FLOCK_YOU:
        return g_app.detect_flock_intensity;
    case MODE_FOX_HUNTER:
        return g_app.detect_fox_intensity;
    case MODE_SKY_SPY:
        return g_app.detect_sky_intensity;
    case MODE_SETTINGS:
    case MODE_SELECT:
    default:
        return g_app.menu_fx_intensity;
    }
}

uint8_t app_mode_display_color(app_mode_t mode)
{
    if (app_mode_display_style_pref(mode) == BORDER_STYLE_DEFAULT) {
        return app_mode_default_display_color(mode);
    }

    switch (mode) {
    case MODE_FLOCK_YOU:
        return g_app.detect_flock_custom;
    case MODE_FOX_HUNTER:
        return g_app.detect_fox_custom;
    case MODE_SKY_SPY:
        return g_app.detect_sky_custom;
    case MODE_SETTINGS:
    case MODE_SELECT:
    default:
        return g_app.menu_led_color;
    }
}

void app_apply_mode_visual_prefs(app_mode_t mode)
{
    g_app.active_border_style = app_mode_display_style(mode);
}

bool app_spi_bus_lock(TickType_t wait_ticks)
{
    if (!g_app.spi_bus_mutex) {
        g_app.spi_bus_mutex = xSemaphoreCreateRecursiveMutex();
        if (!g_app.spi_bus_mutex) {
            ESP_LOGE("app", "Failed to create SPI bus mutex");
            return false;
        }
    }

    return xSemaphoreTakeRecursive(g_app.spi_bus_mutex, wait_ticks) == pdTRUE;
}

void app_spi_bus_unlock(void)
{
    if (g_app.spi_bus_mutex) {
        xSemaphoreGiveRecursive(g_app.spi_bus_mutex);
    }
}

uint8_t app_fx_sanitize_intensity(uint8_t value, uint8_t fallback)
{
    uint8_t best = fallback;
    int best_diff = INT32_MAX;

    if (fallback == 0) {
        fallback = FX_INTENSITY_STEPS[sizeof(FX_INTENSITY_STEPS) / sizeof(FX_INTENSITY_STEPS[0]) - 1];
    }

    if (value == 0) return fallback;

    for (size_t i = 0; i < (sizeof(FX_INTENSITY_STEPS) / sizeof(FX_INTENSITY_STEPS[0])); i++) {
        int diff = abs((int)value - (int)FX_INTENSITY_STEPS[i]);
        if (diff < best_diff) {
            best = FX_INTENSITY_STEPS[i];
            best_diff = diff;
        }
    }

    return best;
}

uint8_t app_fx_cycle_intensity(uint8_t current)
{
    uint8_t normalized = app_fx_sanitize_intensity(current, FX_INTENSITY_STEPS[0]);

    for (size_t i = 0; i < (sizeof(FX_INTENSITY_STEPS) / sizeof(FX_INTENSITY_STEPS[0])); i++) {
        if (FX_INTENSITY_STEPS[i] == normalized) {
            return FX_INTENSITY_STEPS[(i + 1) % (sizeof(FX_INTENSITY_STEPS) / sizeof(FX_INTENSITY_STEPS[0]))];
        }
    }

    return FX_INTENSITY_STEPS[0];
}

uint8_t app_fx_percent(uint8_t value)
{
    uint8_t normalized = app_fx_sanitize_intensity(value, FX_INTENSITY_STEPS[sizeof(FX_INTENSITY_STEPS) / sizeof(FX_INTENSITY_STEPS[0]) - 1]);
    return (uint8_t)(((uint16_t)normalized * 100U + 127U) / 255U);
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
    g_app.menu_led_color = app_mode_default_display_color(MODE_SELECT);
    g_app.border_style = BORDER_STYLE_DEFAULT;
    g_app.active_border_style = app_mode_display_style(MODE_SELECT);
    g_app.menu_fx_intensity = 176;
    g_app.detect_behavior = 0;
    g_app.detect_flock_low = app_mode_default_led_color(MODE_FLOCK_YOU);
    g_app.detect_flock_high = BORDER_STYLE_DEFAULT;
    g_app.detect_flock_custom = app_mode_default_display_color(MODE_FLOCK_YOU);
    g_app.detect_flock_behavior = FX_BEHAVIOR_STANDARD;
    g_app.detect_flock_intensity = 224;
    g_app.detect_fox_low = app_mode_default_led_color(MODE_FOX_HUNTER);
    g_app.detect_fox_high = BORDER_STYLE_DEFAULT;
    g_app.detect_fox_custom = app_mode_default_display_color(MODE_FOX_HUNTER);
    g_app.detect_fox_behavior = FX_BEHAVIOR_STANDARD;
    g_app.detect_fox_intensity = 255;
    g_app.detect_sky_low = app_mode_default_led_color(MODE_SKY_SPY);
    g_app.detect_sky_high = BORDER_STYLE_DEFAULT;
    g_app.detect_sky_custom = app_mode_default_display_color(MODE_SKY_SPY);
    g_app.detect_sky_behavior = FX_BEHAVIOR_STANDARD;
    g_app.detect_sky_intensity = 224;
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
    g_app.local_map_zoom_idx = 0;
    g_app.last_input_ms = uptime_ms();
    g_app.display_sleeping = false;
    g_app.spi_bus_mutex   = xSemaphoreCreateRecursiveMutex();
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

float app_detection_behavior_strength(app_mode_t mode, float detected_strength)
{
    uint8_t behavior = app_mode_display_behavior(mode);

    if (detected_strength < 0.0f) detected_strength = 0.0f;
    if (detected_strength > 1.0f) detected_strength = 1.0f;

    switch (behavior) {
    case FX_BEHAVIOR_BREATHE:
        return powf(detected_strength, 1.35f);
    case FX_BEHAVIOR_TRACKER:
        return 1.0f - powf(1.0f - detected_strength, 1.75f);
    case FX_BEHAVIOR_STING:
        return 1.0f - powf(1.0f - detected_strength, 2.10f);
    case FX_BEHAVIOR_STANDARD:
    default:
        if (mode == MODE_FOX_HUNTER) {
            float sharpness = g_app.fox_led_mode ? 2.00f : 1.75f;
            return 1.0f - powf(1.0f - detected_strength, sharpness);
        }
        return detected_strength;
    }
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
