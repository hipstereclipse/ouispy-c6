/*
 * OUI-Spy C6 — NVS persistent storage
 * SPDX-License-Identifier: MIT
 */
#include "nvs_store.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "nvs_store";
#define NVS_NAMESPACE "ouispy"

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
            size_t len = count * sizeof(fox_reg_entry_t);
            if (nvs_get_blob(h, "fox_reg", g_app.fox_registry, &len) == ESP_OK) {
                g_app.fox_registry_count = count;
            }
        }
        nvs_close(h);
    }
}

void nvs_store_load_prefs(void)
{
    nvs_handle_t h;
    uint8_t tmp;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "brightness", &tmp) == ESP_OK) g_app.lcd_brightness = tmp;
        if (nvs_get_u8(h, "sound", &tmp) == ESP_OK) g_app.sound_enabled = (tmp != 0);
        if (nvs_get_u8(h, "led", &tmp) == ESP_OK) g_app.led_enabled = (tmp != 0);
        nvs_close(h);
    }
}
