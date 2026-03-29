/*
 * OUI-Spy C6 — NVS persistent storage
 * SPDX-License-Identifier: MIT
 */
#include "nvs_store.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "nvs_store";
#define NVS_NAMESPACE "ouispy"

typedef struct {
    uint8_t  mac[6];
    char     label[FOX_REG_LABEL_LEN];
} fox_reg_entry_v1_t;

void nvs_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialized");
}

void nvs_store_save_mode(app_mode_t mode)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "mode", (uint8_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
}

app_mode_t nvs_store_load_mode(void)
{
    nvs_handle_t h;
    uint8_t val = MODE_SELECT;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "mode", &val);
        nvs_close(h);
    }
    return (val < MODE_COUNT) ? (app_mode_t)val : MODE_SELECT;
}

void nvs_store_save_fox_target(const uint8_t mac[6])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "fox_mac", mac, 6);
        nvs_commit(h);
        nvs_close(h);
    }
}

void nvs_store_clear_fox_target(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "fox_mac");
        nvs_commit(h);
        nvs_close(h);
    }
}

int nvs_store_load_fox_target(uint8_t mac[6])
{
    nvs_handle_t h;
    size_t len = 6;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_blob(h, "fox_mac", mac, &len);
        nvs_close(h);
        return (err == ESP_OK && len == 6) ? 0 : -1;
    }
    return -1;
}

void nvs_store_save_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "brightness", g_app.lcd_brightness);
        nvs_set_u8(h, "sound", g_app.sound_enabled ? 1 : 0);
        nvs_set_u8(h, "led", g_app.led_enabled ? 1 : 0);
        nvs_set_u8(h, "ap_bcast", g_app.ap_broadcast_enabled ? 1 : 0);
        nvs_set_u8(h, "ap_single", g_app.single_ap_name_enabled ? 1 : 0);
        nvs_set_u16(h, "sleep_sec", g_app.display_sleep_timeout_sec);
        nvs_set_u8(h, "menu_led", g_app.menu_led_color);
        nvs_set_u8(h, "snd_flock", g_app.sound_profile_flock);
        nvs_set_u8(h, "snd_fox", g_app.sound_profile_fox);
        nvs_set_u8(h, "snd_sky", g_app.sound_profile_sky);
        nvs_set_u8(h, "sc_mode", g_app.shortcut_mode_btn);
        nvs_set_u8(h, "sc_act", g_app.shortcut_action_btn);
        nvs_set_u8(h, "sc_back", g_app.shortcut_back_btn);
        nvs_set_u8(h, "sd_logs", g_app.use_microsd_logs ? 1 : 0);
        nvs_set_u8(h, "adv_logs", g_app.advanced_logging_enabled ? 1 : 0);
        nvs_set_u8(h, "gps_diag", g_app.gps_diagnostics_enabled ? 1 : 0);
        nvs_set_u8(h, "web_diag", g_app.web_diagnostics_enabled ? 1 : 0);
        nvs_set_u8(h, "ser_log", g_app.serial_log_verbosity);
        nvs_set_u8(h, "gps_tag", g_app.gps_tagging_enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void nvs_store_save_fox_registry(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "fox_reg_n", (uint8_t)g_app.fox_registry_count);
        nvs_set_blob(h, "fox_reg", g_app.fox_registry,
                     g_app.fox_registry_count * sizeof(fox_reg_entry_t));
        nvs_commit(h);
        nvs_close(h);
    }
}

void nvs_store_load_fox_registry(void)
{
    nvs_handle_t h;
    uint8_t count = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "fox_reg_n", &count) == ESP_OK && count <= FOX_REGISTRY_MAX) {
            size_t len = 0;
            if (nvs_get_blob(h, "fox_reg", NULL, &len) == ESP_OK) {
                if (len == count * sizeof(fox_reg_entry_t)) {
                    size_t out_len = len;
                    if (nvs_get_blob(h, "fox_reg", g_app.fox_registry, &out_len) == ESP_OK) {
                        g_app.fox_registry_count = count;
                    }
                } else if (len == count * sizeof(fox_reg_entry_v1_t)) {
                    fox_reg_entry_v1_t legacy[FOX_REGISTRY_MAX] = {0};
                    size_t out_len = len;
                    if (nvs_get_blob(h, "fox_reg", legacy, &out_len) == ESP_OK) {
                        memset(g_app.fox_registry, 0, sizeof(g_app.fox_registry));
                        for (int i = 0; i < count; i++) {
                            memcpy(g_app.fox_registry[i].mac, legacy[i].mac, 6);
                            strncpy(g_app.fox_registry[i].label, legacy[i].label,
                                    sizeof(g_app.fox_registry[i].label) - 1);
                            strncpy(g_app.fox_registry[i].section, "auto",
                                    sizeof(g_app.fox_registry[i].section) - 1);
                        }
                        g_app.fox_registry_count = count;
                        nvs_store_save_fox_registry();
                    }
                }
            }
        }
        nvs_close(h);
    }

    for (int i = 0; i < g_app.fox_registry_count; i++) {
        g_app.fox_registry[i].label[sizeof(g_app.fox_registry[i].label) - 1] = '\0';
        g_app.fox_registry[i].original_name[sizeof(g_app.fox_registry[i].original_name) - 1] = '\0';
        g_app.fox_registry[i].nickname[sizeof(g_app.fox_registry[i].nickname) - 1] = '\0';
        g_app.fox_registry[i].notes[sizeof(g_app.fox_registry[i].notes) - 1] = '\0';
        g_app.fox_registry[i].section[sizeof(g_app.fox_registry[i].section) - 1] = '\0';
        if (!g_app.fox_registry[i].section[0]) {
            strncpy(g_app.fox_registry[i].section, "auto",
                    sizeof(g_app.fox_registry[i].section) - 1);
        }
    }
}

void nvs_store_load_prefs(void)
{
    nvs_handle_t h;
    uint8_t tmp;
    uint16_t sleep_tmp;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "brightness", &tmp) == ESP_OK) g_app.lcd_brightness = tmp;
        if (nvs_get_u8(h, "sound", &tmp) == ESP_OK) g_app.sound_enabled = (tmp != 0);
        if (nvs_get_u8(h, "led", &tmp) == ESP_OK) g_app.led_enabled = (tmp != 0);
        if (nvs_get_u8(h, "ap_bcast", &tmp) == ESP_OK) g_app.ap_broadcast_enabled = (tmp != 0);
        if (nvs_get_u8(h, "ap_single", &tmp) == ESP_OK) g_app.single_ap_name_enabled = (tmp != 0);
        if (nvs_get_u16(h, "sleep_sec", &sleep_tmp) == ESP_OK) g_app.display_sleep_timeout_sec = sleep_tmp;
        if (nvs_get_u8(h, "menu_led", &tmp) == ESP_OK) g_app.menu_led_color = tmp;
        if (nvs_get_u8(h, "snd_flock", &tmp) == ESP_OK) g_app.sound_profile_flock = tmp;
        if (nvs_get_u8(h, "snd_fox", &tmp) == ESP_OK) g_app.sound_profile_fox = tmp;
        if (nvs_get_u8(h, "snd_sky", &tmp) == ESP_OK) g_app.sound_profile_sky = tmp;
        if (nvs_get_u8(h, "sc_mode", &tmp) == ESP_OK) g_app.shortcut_mode_btn = tmp;
        if (nvs_get_u8(h, "sc_act", &tmp) == ESP_OK) g_app.shortcut_action_btn = tmp;
        if (nvs_get_u8(h, "sc_back", &tmp) == ESP_OK) g_app.shortcut_back_btn = tmp;
        if (nvs_get_u8(h, "sd_logs", &tmp) == ESP_OK) g_app.use_microsd_logs = (tmp != 0);
        if (nvs_get_u8(h, "adv_logs", &tmp) == ESP_OK) g_app.advanced_logging_enabled = (tmp != 0);
        if (nvs_get_u8(h, "gps_diag", &tmp) == ESP_OK) g_app.gps_diagnostics_enabled = (tmp != 0);
        if (nvs_get_u8(h, "web_diag", &tmp) == ESP_OK) g_app.web_diagnostics_enabled = (tmp != 0);
        if (nvs_get_u8(h, "ser_log", &tmp) == ESP_OK) g_app.serial_log_verbosity = tmp;
        if (nvs_get_u8(h, "gps_tag", &tmp) == ESP_OK) g_app.gps_tagging_enabled = (tmp != 0);
        nvs_close(h);
    }

    if (g_app.menu_led_color >= MENU_LED_COUNT) g_app.menu_led_color = MENU_LED_TOPAZ;
    if (g_app.sound_profile_flock >= SOUND_PROFILE_COUNT) g_app.sound_profile_flock = SOUND_PROFILE_STANDARD;
    if (g_app.sound_profile_fox >= SOUND_PROFILE_COUNT) g_app.sound_profile_fox = SOUND_PROFILE_SONAR;
    if (g_app.sound_profile_sky >= SOUND_PROFILE_COUNT) g_app.sound_profile_sky = SOUND_PROFILE_CHIRP;
    if (g_app.shortcut_mode_btn >= SHORTCUT_COUNT) g_app.shortcut_mode_btn = SHORTCUT_NEXT_MODE;
    if (g_app.shortcut_action_btn >= SHORTCUT_COUNT) g_app.shortcut_action_btn = SHORTCUT_FOX_LED_MODE;
    if (g_app.shortcut_back_btn >= SHORTCUT_COUNT) g_app.shortcut_back_btn = SHORTCUT_MODE_SELECT;
    if (g_app.serial_log_verbosity >= SERIAL_LOG_COUNT) g_app.serial_log_verbosity = SERIAL_LOG_INFO;
    app_apply_runtime_logging_prefs();
}
