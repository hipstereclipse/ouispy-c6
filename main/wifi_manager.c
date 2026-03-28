/*
 * OUI-Spy C6 — WiFi AP manager
 * Handles netif, event loop, and SoftAP configuration.
 * SPDX-License-Identifier: MIT
 */
#include "wifi_manager.h"
#include "app_common.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "wifi_mgr";
static bool s_initialized = false;
static esp_netif_t *s_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "Client connected, AID=%d", e->aid);
            /* Store MAC if we have space */
            if (g_app.wifi_clients < WIFI_MAX_AP_CLIENTS) {
                memcpy(g_app.wifi_client_macs[g_app.wifi_clients], e->mac, 6);
            }
            g_app.wifi_clients++;
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = data;
            /* Remove MAC from tracked list */
            uint8_t tracked = (g_app.wifi_clients < WIFI_MAX_AP_CLIENTS)
                              ? g_app.wifi_clients : WIFI_MAX_AP_CLIENTS;
            for (uint8_t i = 0; i < tracked; i++) {
                if (memcmp(g_app.wifi_client_macs[i], e->mac, 6) == 0) {
                    for (uint8_t j = i; j < tracked - 1; j++) {
                        memcpy(g_app.wifi_client_macs[j],
                               g_app.wifi_client_macs[j + 1], 6);
                    }
                    memset(g_app.wifi_client_macs[tracked - 1], 0, 6);
                    break;
                }
            }
            if (g_app.wifi_clients > 0) g_app.wifi_clients--;
            ESP_LOGI(TAG, "Client disconnected");
            break;
        }
        default:
            break;
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    s_netif = esp_netif_create_default_wifi_ap();
    if (!s_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event handler registration failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi subsystem initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_start_ap(const char *ssid, const char *password, uint8_t channel)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "wifi_manager_start_ap called before wifi_manager_init");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = channel,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_WPA_WPA2_PSK,
            .ssid_hidden    = 0,
            .pmf_cfg        = { .capable = true, .required = false },
            .pairwise_cipher = WIFI_CIPHER_TYPE_CCMP,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, password, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);

    if (strlen(password) < 8) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap_cfg.ap.ssid_hidden = g_app.ap_broadcast_enabled ? 0 : 1;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    g_app.wifi_clients = 0;
    memset(g_app.wifi_client_macs, 0, sizeof(g_app.wifi_client_macs));
    ESP_LOGI(TAG, "AP started: SSID=%s CH=%d hidden=%d", ssid, channel, ap_cfg.ap.ssid_hidden);
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
        return err;
    }

    g_app.wifi_clients = 0;
    return ESP_OK;
}

uint8_t wifi_manager_client_count(void)
{
    return g_app.wifi_clients;
}
