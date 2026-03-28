/*
 * OUI-Spy C6 — HTTP + WebSocket server
 *
 * Serves the single-page web UI and provides a WebSocket endpoint
 * for real-time push updates (device lists, RSSI, drone data).
 *
 * REST endpoints:
 *   GET  /             — HTML UI
 *   GET  /api/state    — Full JSON state dump
 *   POST /api/mode     — Change mode {mode: 0-3}
 *   POST /api/fox/target — Set fox target {mac:"AA:BB:CC:DD:EE:FF"}
 *   POST /api/settings — Update prefs {brightness, sound, led}
 *   GET  /api/devices  — Flock device list (JSON array)
 *   GET  /api/drones   — Sky Spy drone list (JSON array)
 *   POST /api/fox/registry/update — Update registry metadata
 *   GET  /api/export/csv — Export Flock detections as CSV
 *   WS   /ws           — WebSocket for live push updates
 *
 * SPDX-License-Identifier: MIT
 */
#include "web_server.h"
#include "app_common.h"
#include "fox_hunter.h"
#include "nvs_store.h"
#include "storage_ext.h"
#include "display.h"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "websrv";
static httpd_handle_t s_http_server = NULL;
static httpd_handle_t s_https_server = NULL;
#define GPS_READY_TIMEOUT_MS 8000

/* Embedded index.html */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const uint8_t servercert_pem_end[]   asm("_binary_servercert_pem_end");
extern const uint8_t prvtkey_pem_start[]    asm("_binary_prvtkey_pem_start");
extern const uint8_t prvtkey_pem_end[]      asm("_binary_prvtkey_pem_end");

/* ── Serve main page ── */
static void set_security_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Permissions-Policy", "geolocation=(self)");
}

static esp_err_t get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    set_security_headers(req);
    size_t len = index_html_end - index_html_start;
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

/* ── JSON builders ── */
static char *build_state_json(void)
{
    uint32_t now_ms = uptime_ms();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "mode", g_app.current_mode);
    cJSON_AddNumberToObject(root, "uptime", g_app.uptime_sec);
    cJSON_AddNumberToObject(root, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "clients", g_app.wifi_clients);
    cJSON_AddNumberToObject(root, "brightness", g_app.lcd_brightness);
    cJSON_AddBoolToObject(root, "sound", g_app.sound_enabled);
    cJSON_AddBoolToObject(root, "led", g_app.led_enabled);
    cJSON_AddBoolToObject(root, "apBroadcast", g_app.ap_broadcast_enabled);
    cJSON_AddBoolToObject(root, "singleApName", g_app.single_ap_name_enabled);
    cJSON_AddNumberToObject(root, "displaySleepSec", g_app.display_sleep_timeout_sec);
    cJSON_AddNumberToObject(root, "menuLedColor", g_app.menu_led_color);
    cJSON_AddNumberToObject(root, "soundProfileFlock", g_app.sound_profile_flock);
    cJSON_AddNumberToObject(root, "soundProfileFox", g_app.sound_profile_fox);
    cJSON_AddNumberToObject(root, "soundProfileSky", g_app.sound_profile_sky);
    cJSON_AddNumberToObject(root, "shortcutModeBtn", g_app.shortcut_mode_btn);
    cJSON_AddNumberToObject(root, "shortcutActionBtn", g_app.shortcut_action_btn);
    cJSON_AddNumberToObject(root, "shortcutBackBtn", g_app.shortcut_back_btn);
    cJSON_AddBoolToObject(root, "useMicrosdLogs", g_app.use_microsd_logs);
    cJSON_AddBoolToObject(root, "gpsTagging", g_app.gps_tagging_enabled);
    bool gps_ready_fresh = g_app.gps_client_ready &&
                           (now_ms > g_app.gps_client_ready_ms) &&
                           ((now_ms - g_app.gps_client_ready_ms) <= GPS_READY_TIMEOUT_MS);
    bool gps_active = g_app.gps_tagging_enabled && gps_ready_fresh && (g_app.wifi_clients > 0);
    cJSON_AddBoolToObject(root, "gpsClientReady", gps_ready_fresh);
    cJSON_AddBoolToObject(root, "gpsTagActive", gps_active);
    storage_status_t microsd_status = storage_ext_get_status();
    cJSON_AddBoolToObject(root, "microsdAvailable", microsd_status == STORAGE_STATUS_AVAILABLE);
    cJSON_AddStringToObject(root, "microsdStatus", storage_ext_status_str(microsd_status));
    cJSON_AddNumberToObject(root, "logCapacityKb", storage_ext_log_capacity_kb());
    cJSON_AddNumberToObject(root, "deviceCount", g_app.device_count);
    cJSON_AddNumberToObject(root, "droneCount", g_app.drone_count);

    /* Fox hunter state */
    cJSON *fox = cJSON_AddObjectToObject(root, "fox");
    char mac_str[18] = "none";
    if (g_app.fox_target_set) mac_to_str(g_app.fox_target_mac, mac_str, sizeof(mac_str));
    cJSON_AddStringToObject(fox, "target", mac_str);
    cJSON_AddBoolToObject(fox, "hasTarget", g_app.fox_target_set);
    cJSON_AddNumberToObject(fox, "rssi", g_app.fox_rssi);
    cJSON_AddNumberToObject(fox, "bestRssi", g_app.fox_rssi_best);
    cJSON_AddBoolToObject(fox, "found", g_app.fox_target_found);
    cJSON_AddNumberToObject(fox, "ledMode", g_app.fox_led_mode);
    cJSON_AddNumberToObject(fox, "registryCount", g_app.fox_registry_count);
    uint32_t last_seen_sec = (g_app.fox_last_seen > 0 && now_ms > g_app.fox_last_seen)
                             ? (now_ms - g_app.fox_last_seen) / 1000U
                             : 0;
    cJSON_AddNumberToObject(fox, "lastSeenSec", last_seen_sec);
    const char *prox = "very far";
    if (g_app.fox_rssi >= -45) prox = "very close";
    else if (g_app.fox_rssi >= -60) prox = "close";
    else if (g_app.fox_rssi >= -72) prox = "near";
    else if (g_app.fox_rssi >= -84) prox = "far";
    cJSON_AddStringToObject(fox, "proximity", prox);

    /* WiFi AP client MACs */
    cJSON *wc_arr = cJSON_AddArrayToObject(root, "wifiClientMacs");
    uint8_t tracked = (g_app.wifi_clients < WIFI_MAX_AP_CLIENTS)
                      ? g_app.wifi_clients : WIFI_MAX_AP_CLIENTS;
    for (uint8_t i = 0; i < tracked; i++) {
        char mc[18];
        mac_to_str(g_app.wifi_client_macs[i], mc, sizeof(mc));
        cJSON_AddItemToArray(wc_arr, cJSON_CreateString(mc));
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

static char *build_devices_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < g_app.device_count; i++) {
            ble_device_t *d = &g_app.devices[i];
            cJSON *item = cJSON_CreateObject();
            char mac_str[18];
            mac_to_str(d->mac, mac_str, sizeof(mac_str));
            cJSON_AddStringToObject(item, "mac", mac_str);
            cJSON_AddStringToObject(item, "name", d->name);
            cJSON_AddNumberToObject(item, "rssi", d->rssi);
            cJSON_AddNumberToObject(item, "bestRssi", d->rssi_best);
            cJSON_AddNumberToObject(item, "hits", d->hit_count);
            cJSON_AddNumberToObject(item, "flags", d->detect_flags);
            cJSON_AddBoolToObject(item, "flock", d->is_flock);
            cJSON_AddBoolToObject(item, "raven", d->is_raven);
            cJSON_AddNumberToObject(item, "ravenFw", d->raven_fw);
            cJSON_AddNumberToObject(item, "firstSeen", d->first_seen / 1000);
            cJSON_AddNumberToObject(item, "lastSeen", d->last_seen / 1000);
            cJSON_AddItemToArray(arr, item);
        }
        xSemaphoreGive(g_app.device_mutex);
    }
    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return str;
}

static char *build_drones_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (xSemaphoreTake(g_app.drone_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < g_app.drone_count; i++) {
            drone_info_t *d = &g_app.drones[i];
            cJSON *item = cJSON_CreateObject();
            char mac_str[18];
            mac_to_str(d->mac, mac_str, sizeof(mac_str));
            cJSON_AddStringToObject(item, "mac", mac_str);
            cJSON_AddNumberToObject(item, "rssi", d->rssi);
            cJSON_AddStringToObject(item, "id", d->basic_id);
            cJSON_AddNumberToObject(item, "idType", d->id_type);
            cJSON_AddNumberToObject(item, "uaType", d->ua_type);
            cJSON_AddNumberToObject(item, "lat", d->lat);
            cJSON_AddNumberToObject(item, "lon", d->lon);
            cJSON_AddNumberToObject(item, "alt", d->altitude);
            cJSON_AddNumberToObject(item, "height", d->height);
            cJSON_AddNumberToObject(item, "speed", d->speed);
            cJSON_AddNumberToObject(item, "dir", d->direction);
            cJSON_AddNumberToObject(item, "pilotLat", d->pilot_lat);
            cJSON_AddNumberToObject(item, "pilotLon", d->pilot_lon);
            cJSON_AddStringToObject(item, "opId", d->operator_id);
            const char *proto_names[] = {"wifi", "ble", "dji"};
            cJSON_AddStringToObject(item, "proto", proto_names[d->protocol % 3]);
            cJSON_AddBoolToObject(item, "hasLoc", d->has_location);
            cJSON_AddBoolToObject(item, "hasPilot", d->has_pilot);
            cJSON_AddItemToArray(arr, item);
        }
        xSemaphoreGive(g_app.drone_mutex);
    }
    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return str;
}

/* ── API handlers ── */
static esp_err_t get_state(httpd_req_t *req)
{
    char *json = build_state_json();
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t get_devices(httpd_req_t *req)
{
    char *json = build_devices_json();
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t get_drones(httpd_req_t *req)
{
    char *json = build_drones_json();
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t post_mode(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (mode && cJSON_IsNumber(mode)) {
        int m = mode->valueint;
        if (m >= 0 && m < MODE_COUNT) {
            g_app.requested_mode = (app_mode_t)m;
            g_app.mode_change_pending = true;
            ESP_LOGI(TAG, "Mode change requested: %d", m);
        }
    }
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t post_fox_target(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (mac_item && cJSON_IsString(mac_item)) {
        uint8_t mac[6];
        if (mac_from_str(mac_item->valuestring, mac) == 0) {
            fox_hunter_set_target(mac);
        }
    }

    /* Also accept index into flock device list */
    cJSON *idx_item = cJSON_GetObjectItem(root, "index");
    if (idx_item && cJSON_IsNumber(idx_item)) {
        fox_hunter_set_target_from_flock(idx_item->valueint);
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t post_fox_ledmode(httpd_req_t *req)
{
    bool applied = false;
    int content_len = req->content_len;
    if (content_len > 0 && content_len < 64) {
        char buf[64];
        int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *mode = cJSON_GetObjectItem(root, "mode");
                if (mode && cJSON_IsNumber(mode)) {
                    g_app.fox_led_mode = (mode->valueint != 0) ? 1 : 0;
                    applied = true;
                }
                cJSON_Delete(root);
            }
        }
    }

    if (!applied) {
        /* Backward compatibility: no body means toggle. */
        g_app.fox_led_mode = (g_app.fox_led_mode + 1) % 2;
    }

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"ledMode\":%d}", g_app.fox_led_mode);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

/* ── Fox registry endpoints ── */
static esp_err_t get_fox_registry(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_app.fox_registry_count; i++) {
        fox_reg_entry_t *e = &g_app.fox_registry[i];
        cJSON *item = cJSON_CreateObject();
        char mac_str[18];
        mac_to_str(e->mac, mac_str, sizeof(mac_str));
        cJSON_AddStringToObject(item, "mac", mac_str);
        cJSON_AddStringToObject(item, "label", e->label);
        cJSON_AddStringToObject(item, "originalName", e->original_name);
        cJSON_AddStringToObject(item, "nickname", e->nickname);
        cJSON_AddStringToObject(item, "notes", e->notes);
        cJSON_AddStringToObject(item, "section", e->section);
        cJSON_AddItemToArray(arr, item);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t post_fox_registry(httpd_req_t *req)
{
    char buf[384];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (!mac_item || !cJSON_IsString(mac_item)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing mac");
    }

    uint8_t mac[6];
    if (mac_from_str(mac_item->valuestring, mac) != 0) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mac");
    }

    const char *label = "";
    cJSON *label_item = cJSON_GetObjectItem(root, "label");
    if (label_item && cJSON_IsString(label_item)) label = label_item->valuestring;

    const char *original_name = "";
    cJSON *name_item = cJSON_GetObjectItem(root, "originalName");
    if (!name_item) name_item = cJSON_GetObjectItem(root, "name");
    if (name_item && cJSON_IsString(name_item)) original_name = name_item->valuestring;

    const char *section = "auto";
    cJSON *section_item = cJSON_GetObjectItem(root, "section");
    if (section_item && cJSON_IsString(section_item) && section_item->valuestring[0]) {
        section = section_item->valuestring;
    }

    const char *nickname = NULL;
    cJSON *nickname_item = cJSON_GetObjectItem(root, "nickname");
    if (nickname_item && cJSON_IsString(nickname_item)) nickname = nickname_item->valuestring;

    const char *notes = NULL;
    cJSON *notes_item = cJSON_GetObjectItem(root, "notes");
    if (notes_item && cJSON_IsString(notes_item)) notes = notes_item->valuestring;

    int idx = fox_hunter_registry_add(mac, label, original_name, section);
    if (idx >= 0 && (nickname || notes)) {
        fox_hunter_registry_update(idx, nickname, notes, NULL, NULL, NULL);
    }
    cJSON_Delete(root);

    if (idx < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Registry full");

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"index\":%d}", idx);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t post_fox_registry_update(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    int idx = -1;
    cJSON *idx_item = cJSON_GetObjectItem(root, "index");
    if (idx_item && cJSON_IsNumber(idx_item)) {
        idx = idx_item->valueint;
    } else {
        cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
        if (mac_item && cJSON_IsString(mac_item)) {
            uint8_t mac[6];
            if (mac_from_str(mac_item->valuestring, mac) == 0) {
                for (int i = 0; i < g_app.fox_registry_count; i++) {
                    if (mac_equal(g_app.fox_registry[i].mac, mac)) {
                        idx = i;
                        break;
                    }
                }
            }
        }
    }

    if (idx < 0 || idx >= g_app.fox_registry_count) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing/invalid index or mac");
    }

    const char *nickname = NULL;
    const char *notes = NULL;
    const char *section = NULL;
    const char *label = NULL;
    const char *original_name = NULL;

    cJSON *nickname_item = cJSON_GetObjectItem(root, "nickname");
    if (nickname_item && cJSON_IsString(nickname_item)) nickname = nickname_item->valuestring;

    cJSON *notes_item = cJSON_GetObjectItem(root, "notes");
    if (notes_item && cJSON_IsString(notes_item)) notes = notes_item->valuestring;

    cJSON *section_item = cJSON_GetObjectItem(root, "section");
    if (section_item && cJSON_IsString(section_item)) section = section_item->valuestring;

    cJSON *label_item = cJSON_GetObjectItem(root, "label");
    if (label_item && cJSON_IsString(label_item)) label = label_item->valuestring;

    cJSON *name_item = cJSON_GetObjectItem(root, "originalName");
    if (name_item && cJSON_IsString(name_item)) original_name = name_item->valuestring;

    int rc = fox_hunter_registry_update(idx, nickname, notes, section, label, original_name);
    cJSON_Delete(root);

    if (rc < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid index");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t delete_fox_registry(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    cJSON *idx_item = cJSON_GetObjectItem(root, "index");
    if (!idx_item || !cJSON_IsNumber(idx_item)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing index");
    }

    int result = fox_hunter_registry_remove(idx_item->valueint);
    cJSON_Delete(root);

    if (result < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid index");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t post_gps_status(httpd_req_t *req)
{
    char buf[96];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    cJSON *ready = cJSON_GetObjectItem(root, "ready");
    if (ready) {
        g_app.gps_client_ready = cJSON_IsTrue(ready);
        g_app.gps_client_ready_ms = uptime_ms();
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t post_settings(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    cJSON *br = cJSON_GetObjectItem(root, "brightness");
    if (br && cJSON_IsNumber(br)) {
        g_app.lcd_brightness = (uint8_t)br->valueint;
        display_set_brightness(g_app.lcd_brightness);
    }
    cJSON *snd = cJSON_GetObjectItem(root, "sound");
    if (snd) g_app.sound_enabled = cJSON_IsTrue(snd);
    cJSON *led = cJSON_GetObjectItem(root, "led");
    if (led) g_app.led_enabled = cJSON_IsTrue(led);

    bool old_ap_broadcast = g_app.ap_broadcast_enabled;
    bool old_single_ap_name = g_app.single_ap_name_enabled;
    cJSON *ap_bcast = cJSON_GetObjectItem(root, "apBroadcast");
    if (ap_bcast) g_app.ap_broadcast_enabled = cJSON_IsTrue(ap_bcast);
    cJSON *single_ap = cJSON_GetObjectItem(root, "singleApName");
    if (single_ap) g_app.single_ap_name_enabled = cJSON_IsTrue(single_ap);

    cJSON *sleep_sec = cJSON_GetObjectItem(root, "displaySleepSec");
    if (sleep_sec && cJSON_IsNumber(sleep_sec)) {
        int val = sleep_sec->valueint;
        if (val < 0) val = 0;
        if (val > 3600) val = 3600;
        g_app.display_sleep_timeout_sec = (uint16_t)val;
    }

    cJSON *menu_led = cJSON_GetObjectItem(root, "menuLedColor");
    if (menu_led && cJSON_IsNumber(menu_led)) {
        int val = menu_led->valueint;
        if (val < 0) val = 0;
        if (val >= MENU_LED_COUNT) val = MENU_LED_COUNT - 1;
        g_app.menu_led_color = (uint8_t)val;
    }

    cJSON *snd_flock = cJSON_GetObjectItem(root, "soundProfileFlock");
    if (snd_flock && cJSON_IsNumber(snd_flock)) {
        int val = snd_flock->valueint;
        if (val < 0) val = 0;
        if (val >= SOUND_PROFILE_COUNT) val = SOUND_PROFILE_COUNT - 1;
        g_app.sound_profile_flock = (uint8_t)val;
    }

    cJSON *snd_fox = cJSON_GetObjectItem(root, "soundProfileFox");
    if (snd_fox && cJSON_IsNumber(snd_fox)) {
        int val = snd_fox->valueint;
        if (val < 0) val = 0;
        if (val >= SOUND_PROFILE_COUNT) val = SOUND_PROFILE_COUNT - 1;
        g_app.sound_profile_fox = (uint8_t)val;
    }

    cJSON *snd_sky = cJSON_GetObjectItem(root, "soundProfileSky");
    if (snd_sky && cJSON_IsNumber(snd_sky)) {
        int val = snd_sky->valueint;
        if (val < 0) val = 0;
        if (val >= SOUND_PROFILE_COUNT) val = SOUND_PROFILE_COUNT - 1;
        g_app.sound_profile_sky = (uint8_t)val;
    }

    cJSON *sc_mode = cJSON_GetObjectItem(root, "shortcutModeBtn");
    if (sc_mode && cJSON_IsNumber(sc_mode)) {
        int val = sc_mode->valueint;
        if (val < 0) val = 0;
        if (val >= SHORTCUT_COUNT) val = SHORTCUT_COUNT - 1;
        g_app.shortcut_mode_btn = (uint8_t)val;
    }

    cJSON *sc_action = cJSON_GetObjectItem(root, "shortcutActionBtn");
    if (sc_action && cJSON_IsNumber(sc_action)) {
        int val = sc_action->valueint;
        if (val < 0) val = 0;
        if (val >= SHORTCUT_COUNT) val = SHORTCUT_COUNT - 1;
        g_app.shortcut_action_btn = (uint8_t)val;
    }

    cJSON *sc_back = cJSON_GetObjectItem(root, "shortcutBackBtn");
    if (sc_back && cJSON_IsNumber(sc_back)) {
        int val = sc_back->valueint;
        if (val < 0) val = 0;
        if (val >= SHORTCUT_COUNT) val = SHORTCUT_COUNT - 1;
        g_app.shortcut_back_btn = (uint8_t)val;
    }

    cJSON *sd_logs = cJSON_GetObjectItem(root, "useMicrosdLogs");
    if (sd_logs) g_app.use_microsd_logs = cJSON_IsTrue(sd_logs);

    cJSON *gps_tagging = cJSON_GetObjectItem(root, "gpsTagging");
    if (gps_tagging) g_app.gps_tagging_enabled = cJSON_IsTrue(gps_tagging);

    if (old_ap_broadcast != g_app.ap_broadcast_enabled ||
        old_single_ap_name != g_app.single_ap_name_enabled) {
        const char *ssid = NULL;
        const char *pass = NULL;
        uint8_t channel = 6;
        app_mode_ap_credentials(g_app.current_mode, &ssid, &pass, &channel);
        wifi_manager_stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        wifi_manager_start_ap(ssid, pass, channel);
    }

    nvs_store_save_prefs();
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t get_export_csv(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=flock_devices.csv");
    httpd_resp_sendstr_chunk(req,
        "MAC,Name,RSSI,BestRSSI,Hits,Flags,Flock,Raven,RavenFW,FirstSeen,LastSeen\r\n");

    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (int i = 0; i < g_app.device_count; i++) {
            ble_device_t *d = &g_app.devices[i];
            char mac_str[18];
            mac_to_str(d->mac, mac_str, sizeof(mac_str));
            char line[256];
            snprintf(line, sizeof(line),
                     "%s,%s,%d,%d,%u,0x%02X,%d,%d,%d,%lu,%lu\r\n",
                     mac_str, d->name, d->rssi, d->rssi_best,
                     d->hit_count, d->detect_flags,
                     d->is_flock, d->is_raven, d->raven_fw,
                     (unsigned long)(d->first_seen / 1000),
                     (unsigned long)(d->last_seen / 1000));
            httpd_resp_sendstr_chunk(req, line);
        }
        xSemaphoreGive(g_app.device_mutex);
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ── WebSocket handler ── */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS handshake fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    /* Receive frame (handle ping, commands) */
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    uint8_t buf[128];
    frame.payload = buf;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
    if (ret != ESP_OK) return ret;
    buf[frame.len] = '\0';

    /* Echo state back on any client message */
    char *json = build_state_json();
    httpd_ws_frame_t resp = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    ret = httpd_ws_send_frame(req, &resp);
    free(json);
    return ret;
}

/* ── Broadcast to all WS clients ── */
static void broadcast_on_server(httpd_handle_t server, const char *json)
{
    if (!server || !json) return;

    size_t clients = 8;
    int fds[8];
    if (httpd_get_client_list(server, &clients, fds) == ESP_OK) {
        for (size_t i = 0; i < clients; i++) {
            if (httpd_ws_get_fd_info(server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t pkt = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)json,
                    .len = strlen(json),
                };
                httpd_ws_send_frame_async(server, fds[i], &pkt);
            }
        }
    }
}

void web_server_broadcast(const char *json)
{
    if (!json) return;
    broadcast_on_server(s_http_server, json);
    broadcast_on_server(s_https_server, json);
}

/* ── Push task: sends state updates over WS every 500ms ── */
static void ws_push_task(void *arg)
{
    while (s_http_server || s_https_server) {
        char *json = build_state_json();
        if (json) {
            web_server_broadcast(json);
            free(json);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelete(NULL);
}

/* ── Fox nearby candidates ── */
static char *build_fox_nearby_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < g_app.fox_nearby_count; i++) {
            ble_device_t *d = &g_app.fox_nearby[i];
            cJSON *item = cJSON_CreateObject();
            char mac_str[18];
            mac_to_str(d->mac, mac_str, sizeof(mac_str));
            cJSON_AddStringToObject(item, "mac", mac_str);
            cJSON_AddStringToObject(item, "name", d->name);
            cJSON_AddNumberToObject(item, "rssi", d->rssi);
            cJSON_AddNumberToObject(item, "bestRssi", d->rssi_best);
            cJSON_AddNumberToObject(item, "hits", d->hit_count);
            cJSON_AddNumberToObject(item, "firstSeen", d->first_seen / 1000);
            cJSON_AddNumberToObject(item, "lastSeen", d->last_seen / 1000);
            cJSON_AddItemToArray(arr, item);
        }
        xSemaphoreGive(g_app.device_mutex);
    }
    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return str;
}

static esp_err_t get_fox_nearby(httpd_req_t *req)
{
    char *json = build_fox_nearby_json();
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t post_target_gps(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    double lat = cJSON_GetObjectItem(root, "lat") ? cJSON_GetObjectItem(root, "lat")->valuedouble : 0;
    double lon = cJSON_GetObjectItem(root, "lon") ? cJSON_GetObjectItem(root, "lon")->valuedouble : 0;
    uint8_t mac[6] = {0};

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (mac_item && cJSON_IsString(mac_item)) {
        mac_from_str(mac_item->valuestring, mac);
    }

    if (type && lat != 0 && lon != 0) {
        char log_msg[128];
        if (strcmp(type, "fox") == 0) {
            g_app.fox_target_lat = lat;
            g_app.fox_target_lon = lon;
            snprintf(log_msg, sizeof(log_msg), "gps_captured mac=%02X:%02X:%02X:%02X:%02X:%02X lat=%.6f lon=%.6f",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], lat, lon);
            storage_ext_append_log("fox", log_msg);
        } else if (strcmp(type, "sky") == 0) {
            g_app.sky_tracked_lat = lat;
            g_app.sky_tracked_lon = lon;
            snprintf(log_msg, sizeof(log_msg), "gps_captured mac=%02X:%02X:%02X:%02X:%02X:%02X lat=%.6f lon=%.6f",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], lat, lon);
            storage_ext_append_log("sky", log_msg);
        }
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t post_sky_track(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    double lat = cJSON_GetObjectItem(root, "lat") ? cJSON_GetObjectItem(root, "lat")->valuedouble : 0;
    double lon = cJSON_GetObjectItem(root, "lon") ? cJSON_GetObjectItem(root, "lon")->valuedouble : 0;
    uint8_t mac[6] = {0};

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    cJSON *idx_item = cJSON_GetObjectItem(root, "index");
    if (mac_item && cJSON_IsString(mac_item)) {
        mac_from_str(mac_item->valuestring, mac);
    }
    if (idx_item && cJSON_IsNumber(idx_item)) {
        if (idx_item->valueint >= 0 && idx_item->valueint < g_app.drone_count) {
            memcpy(mac, g_app.drones[idx_item->valueint].mac, 6);
            g_app.sky_tracked_drone_idx = idx_item->valueint;
        }
    }

    // Log GPS if provided
    if (lat != 0 && lon != 0) {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "gps_captured mac=%02X:%02X:%02X:%02X:%02X:%02X lat=%.6f lon=%.6f",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], lat, lon);
        storage_ext_append_log("sky", log_msg);
        g_app.sky_tracked_lat = lat;
        g_app.sky_tracked_lon = lon;
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── Register routes and start server ── */
static void web_register_routes(httpd_handle_t srv)
{
    /* Static page */
    httpd_uri_t uri_index = { .uri="/", .method=HTTP_GET, .handler=get_index };
    httpd_register_uri_handler(srv, &uri_index);

    /* API endpoints */
    httpd_uri_t uri_state = { .uri="/api/state", .method=HTTP_GET, .handler=get_state };
    httpd_register_uri_handler(srv, &uri_state);

    httpd_uri_t uri_devices = { .uri="/api/devices", .method=HTTP_GET, .handler=get_devices };
    httpd_register_uri_handler(srv, &uri_devices);

    httpd_uri_t uri_drones = { .uri="/api/drones", .method=HTTP_GET, .handler=get_drones };
    httpd_register_uri_handler(srv, &uri_drones);

    httpd_uri_t uri_mode = { .uri="/api/mode", .method=HTTP_POST, .handler=post_mode };
    httpd_register_uri_handler(srv, &uri_mode);

    httpd_uri_t uri_gps_status = { .uri="/api/gps/status", .method=HTTP_POST, .handler=post_gps_status };
    httpd_register_uri_handler(srv, &uri_gps_status);

    httpd_uri_t uri_target_gps = { .uri="/api/target/gps", .method=HTTP_POST, .handler=post_target_gps };
    httpd_register_uri_handler(srv, &uri_target_gps);

    httpd_uri_t uri_sky_track = { .uri="/api/sky/track", .method=HTTP_POST, .handler=post_sky_track };
    httpd_register_uri_handler(srv, &uri_sky_track);

    httpd_uri_t uri_fox = { .uri="/api/fox/target", .method=HTTP_POST, .handler=post_fox_target };
    httpd_register_uri_handler(srv, &uri_fox);

    httpd_uri_t uri_fox_led = { .uri="/api/fox/ledmode", .method=HTTP_POST, .handler=post_fox_ledmode };
    httpd_register_uri_handler(srv, &uri_fox_led);

    httpd_uri_t uri_fox_reg_get = { .uri="/api/fox/registry", .method=HTTP_GET, .handler=get_fox_registry };
    httpd_register_uri_handler(srv, &uri_fox_reg_get);

    httpd_uri_t uri_fox_reg_post = { .uri="/api/fox/registry", .method=HTTP_POST, .handler=post_fox_registry };
    httpd_register_uri_handler(srv, &uri_fox_reg_post);

    httpd_uri_t uri_fox_reg_del = { .uri="/api/fox/registry", .method=HTTP_DELETE, .handler=delete_fox_registry };
    httpd_register_uri_handler(srv, &uri_fox_reg_del);

    httpd_uri_t uri_fox_reg_update = { .uri="/api/fox/registry/update", .method=HTTP_POST, .handler=post_fox_registry_update };
    httpd_register_uri_handler(srv, &uri_fox_reg_update);

    httpd_uri_t uri_settings = { .uri="/api/settings", .method=HTTP_POST, .handler=post_settings };
    httpd_register_uri_handler(srv, &uri_settings);

    httpd_uri_t uri_csv = { .uri="/api/export/csv", .method=HTTP_GET, .handler=get_export_csv };
    httpd_register_uri_handler(srv, &uri_csv);

    httpd_uri_t uri_fox_nearby = { .uri="/api/fox/nearby", .method=HTTP_GET, .handler=get_fox_nearby };
    httpd_register_uri_handler(srv, &uri_fox_nearby);

    /* WebSocket */
    httpd_uri_t uri_ws = {
        .uri="/ws", .method=HTTP_GET, .handler=ws_handler,
        .is_websocket=true, .handle_ws_control_frames=true,
    };
    httpd_register_uri_handler(srv, &uri_ws);
}

void web_server_start(void)
{
    bool started = false;

    httpd_ssl_config_t ssl_config = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_config.httpd.max_uri_handlers = 20;
    ssl_config.httpd.stack_size = 8192;
    ssl_config.httpd.lru_purge_enable = true;
    ssl_config.httpd.server_port = 443;
    ssl_config.httpd.ctrl_port = 32769;
    ssl_config.servercert = servercert_pem_start;
    ssl_config.servercert_len = servercert_pem_end - servercert_pem_start;
    ssl_config.prvtkey_pem = prvtkey_pem_start;
    ssl_config.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;

    if (httpd_ssl_start(&s_https_server, &ssl_config) == ESP_OK) {
        web_register_routes(s_https_server);
        started = true;
        ESP_LOGI(TAG, "HTTPS web server started on port %d", ssl_config.httpd.server_port);
    } else {
        ESP_LOGW(TAG, "HTTPS server failed to start; trying HTTP");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size       = 8192;
    config.lru_purge_enable = true;
    config.server_port      = 80;
    config.ctrl_port        = 32768;

    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        web_register_routes(s_http_server);
        started = true;
        ESP_LOGI(TAG, "HTTP web server started on port %d", config.server_port);
    } else {
        ESP_LOGW(TAG, "HTTP server failed to start");
    }

    if (!started) {
        ESP_LOGE(TAG, "Failed to start HTTP and HTTPS servers");
        return;
    }

    g_app.http_server = s_https_server ? s_https_server : s_http_server;

    /* Start push task */
    if (xTaskCreate(ws_push_task, "ws_push", TASK_STACK_WS, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WebSocket push task");
    }
}

void web_server_stop(void)
{
    if (s_https_server) {
        httpd_ssl_stop(s_https_server);
        s_https_server = NULL;
    }
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    g_app.http_server = NULL;
}
