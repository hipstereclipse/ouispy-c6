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
 *   GET  /api/export/csv — Export Flock detections as CSV
 *   WS   /ws           — WebSocket for live push updates
 *
 * SPDX-License-Identifier: MIT
 */
#include "web_server.h"
#include "app_common.h"
#include "fox_hunter.h"
#include "nvs_store.h"
#include "display.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "websrv";

/* Embedded index.html */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ── Serve main page ── */
static esp_err_t get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t len = index_html_end - index_html_start;
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

/* ── JSON builders ── */
static char *build_state_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "mode", g_app.current_mode);
    cJSON_AddNumberToObject(root, "uptime", g_app.uptime_sec);
    cJSON_AddNumberToObject(root, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "clients", g_app.wifi_clients);
    cJSON_AddNumberToObject(root, "brightness", g_app.lcd_brightness);
    cJSON_AddBoolToObject(root, "sound", g_app.sound_enabled);
    cJSON_AddBoolToObject(root, "led", g_app.led_enabled);
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
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t get_devices(httpd_req_t *req)
{
    char *json = build_devices_json();
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t get_drones(httpd_req_t *req)
{
    char *json = build_drones_json();
    httpd_resp_set_type(req, "application/json");
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
    g_app.fox_led_mode = (g_app.fox_led_mode + 1) % 2;
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"ledMode\":%d}", g_app.fox_led_mode);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t post_settings(httpd_req_t *req)
{
    char buf[128];
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
void web_server_broadcast(const char *json)
{
    if (!g_app.http_server || !json) return;

    size_t clients = 8;
    int fds[8];
    if (httpd_get_client_list(g_app.http_server, &clients, fds) == ESP_OK) {
        for (size_t i = 0; i < clients; i++) {
            if (httpd_ws_get_fd_info(g_app.http_server, fds[i])
                == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t pkt = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)json,
                    .len = strlen(json),
                };
                httpd_ws_send_frame_async(g_app.http_server, fds[i], &pkt);
            }
        }
    }
}

/* ── Push task: sends state updates over WS every 500ms ── */
static void ws_push_task(void *arg)
{
    while (g_app.http_server) {
        char *json = build_state_json();
        if (json) {
            web_server_broadcast(json);
            free(json);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelete(NULL);
}

/* ── Register routes and start server ── */
void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size       = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&g_app.http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    /* Static page */
    httpd_uri_t uri_index = { .uri="/", .method=HTTP_GET, .handler=get_index };
    httpd_register_uri_handler(g_app.http_server, &uri_index);

    /* API endpoints */
    httpd_uri_t uri_state = { .uri="/api/state", .method=HTTP_GET, .handler=get_state };
    httpd_register_uri_handler(g_app.http_server, &uri_state);

    httpd_uri_t uri_devices = { .uri="/api/devices", .method=HTTP_GET, .handler=get_devices };
    httpd_register_uri_handler(g_app.http_server, &uri_devices);

    httpd_uri_t uri_drones = { .uri="/api/drones", .method=HTTP_GET, .handler=get_drones };
    httpd_register_uri_handler(g_app.http_server, &uri_drones);

    httpd_uri_t uri_mode = { .uri="/api/mode", .method=HTTP_POST, .handler=post_mode };
    httpd_register_uri_handler(g_app.http_server, &uri_mode);

    httpd_uri_t uri_fox = { .uri="/api/fox/target", .method=HTTP_POST, .handler=post_fox_target };
    httpd_register_uri_handler(g_app.http_server, &uri_fox);

    httpd_uri_t uri_fox_led = { .uri="/api/fox/ledmode", .method=HTTP_POST, .handler=post_fox_ledmode };
    httpd_register_uri_handler(g_app.http_server, &uri_fox_led);

    httpd_uri_t uri_settings = { .uri="/api/settings", .method=HTTP_POST, .handler=post_settings };
    httpd_register_uri_handler(g_app.http_server, &uri_settings);

    httpd_uri_t uri_csv = { .uri="/api/export/csv", .method=HTTP_GET, .handler=get_export_csv };
    httpd_register_uri_handler(g_app.http_server, &uri_csv);

    /* WebSocket */
    httpd_uri_t uri_ws = {
        .uri="/ws", .method=HTTP_GET, .handler=ws_handler,
        .is_websocket=true, .handle_ws_control_frames=true,
    };
    httpd_register_uri_handler(g_app.http_server, &uri_ws);

    /* Start push task */
    if (xTaskCreate(ws_push_task, "ws_push", TASK_STACK_WS, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WebSocket push task");
    }

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
}

void web_server_stop(void)
{
    if (g_app.http_server) {
        httpd_stop(g_app.http_server);
        g_app.http_server = NULL;
    }
}
