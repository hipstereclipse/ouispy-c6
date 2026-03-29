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
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>

static const char *TAG = "websrv";
static httpd_handle_t s_http_server = NULL;
static httpd_handle_t s_https_server = NULL;
#define GPS_READY_TIMEOUT_MS 20000
#define LOG_RECENT_LIMIT 32
#define WEB_MAX_URI_HANDLERS 32
static const char *MAP_ROOT_PATH = "/sdcard/map";

/* Embedded index.html */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const uint8_t servercert_pem_end[]   asm("_binary_servercert_pem_end");
extern const uint8_t prvtkey_pem_start[]    asm("_binary_prvtkey_pem_start");
extern const uint8_t prvtkey_pem_end[]      asm("_binary_prvtkey_pem_end");

static const char *logging_status_label(void)
{
    if (storage_ext_logging_active()) return "SD + RAM";
    if (storage_ext_logging_blocked()) return "RAM only (SD waiting)";
    return "RAM only";
}

static const char *log_bucket_name(storage_log_bucket_t bucket)
{
    switch (bucket) {
    case STORAGE_LOG_BUCKET_IDENTITY:
        return "identity";
    case STORAGE_LOG_BUCKET_DIAGNOSTICS:
        return "diagnostics";
    case STORAGE_LOG_BUCKET_EVENTS:
    default:
        return "events";
    }
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float rssi_weight_from_dbm(int rssi)
{
    float norm = ((float)rssi + 95.0f) / 50.0f;  /* -95..-45 -> 0..1 */
    norm = clampf(norm, 0.0f, 1.0f);
    return 0.35f + (norm * 1.65f);
}

static float assumed_radius_from_rssi(int rssi, bool drone_mode)
{
    float norm = ((float)rssi + 95.0f) / 50.0f;
    norm = clampf(norm, 0.0f, 1.0f);
    if (drone_mode) {
        return 55.0f + ((1.0f - norm) * 245.0f);  /* 55m..300m */
    }
    return 6.0f + ((1.0f - norm) * 64.0f);       /* 6m..70m */
}

static float approx_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * (M_PI / 180.0);
    double dlon = (lon2 - lon1) * (M_PI / 180.0);
    double mean_lat = ((lat1 + lat2) * 0.5) * (M_PI / 180.0);
    double x = dlon * cos(mean_lat);
    double y = dlat;
    return (float)(sqrt((x * x) + (y * y)) * 6371000.0);
}

static void update_fox_fused_pin(double sample_lat, double sample_lon, int rssi)
{
    float w = rssi_weight_from_dbm(rssi);
    if (g_app.fox_target_gps_samples == 0 || g_app.fox_target_weight_sum <= 0.0f) {
        g_app.fox_target_lat = sample_lat;
        g_app.fox_target_lon = sample_lon;
        g_app.fox_target_weight_sum = w;
        g_app.fox_target_gps_samples = 1;
        g_app.fox_target_radius_m = clampf(assumed_radius_from_rssi(rssi, false), 6.0f, 90.0f);
        return;
    }

    double prev_lat = g_app.fox_target_lat;
    double prev_lon = g_app.fox_target_lon;
    float prev_w = g_app.fox_target_weight_sum;
    float next_w = prev_w + w;

    g_app.fox_target_lat = ((prev_lat * prev_w) + (sample_lat * w)) / next_w;
    g_app.fox_target_lon = ((prev_lon * prev_w) + (sample_lon * w)) / next_w;
    g_app.fox_target_weight_sum = next_w;
    if (g_app.fox_target_gps_samples < 65535U) g_app.fox_target_gps_samples++;

    float d_m = approx_distance_m(prev_lat, prev_lon, sample_lat, sample_lon);
    float instant_radius = assumed_radius_from_rssi(rssi, false) + (d_m * 0.42f);
    g_app.fox_target_radius_m = clampf((g_app.fox_target_radius_m * 0.78f) + (instant_radius * 0.22f), 6.0f, 110.0f);
}

static void update_sky_fused_pin(double sample_lat, double sample_lon, int rssi)
{
    if (g_app.sky_tracked_gps_samples == 0) {
        g_app.sky_tracked_lat = sample_lat;
        g_app.sky_tracked_lon = sample_lon;
        g_app.sky_tracked_gps_samples = 1;
        g_app.sky_tracked_radius_m = clampf(assumed_radius_from_rssi(rssi, true), 40.0f, 320.0f);
        return;
    }

    double prev_lat = g_app.sky_tracked_lat;
    double prev_lon = g_app.sky_tracked_lon;
    float w = rssi_weight_from_dbm(rssi);
    float alpha = clampf(0.62f + (w * 0.16f), 0.62f, 0.90f);  /* prioritize newest known point */

    g_app.sky_tracked_lat = (prev_lat * (1.0 - alpha)) + (sample_lat * alpha);
    g_app.sky_tracked_lon = (prev_lon * (1.0 - alpha)) + (sample_lon * alpha);
    if (g_app.sky_tracked_gps_samples < 65535U) g_app.sky_tracked_gps_samples++;

    float d_m = approx_distance_m(prev_lat, prev_lon, sample_lat, sample_lon);
    float instant_radius = assumed_radius_from_rssi(rssi, true) + (d_m * 0.90f);
    g_app.sky_tracked_radius_m = clampf((g_app.sky_tracked_radius_m * 0.72f) + (instant_radius * 0.28f), 40.0f, 500.0f);
}

static char *build_logs_json(void)
{
    storage_log_entry_t *recent = calloc(LOG_RECENT_LIMIT, sizeof(storage_log_entry_t));
    if (!recent) {
        return NULL;
    }
    int recent_count = storage_ext_get_recent_entries(recent, LOG_RECENT_LIMIT);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "loggingEnabled", g_app.use_microsd_logs);
    cJSON_AddBoolToObject(root, "loggingActive", storage_ext_logging_active());
    cJSON_AddBoolToObject(root, "loggingBlocked", storage_ext_logging_blocked());
    cJSON_AddBoolToObject(root, "advancedLoggingEnabled", g_app.advanced_logging_enabled);
    cJSON_AddBoolToObject(root, "logGeneralEnabled", g_app.log_general_enabled);
    cJSON_AddBoolToObject(root, "logFlockEnabled", g_app.log_flock_enabled);
    cJSON_AddBoolToObject(root, "logFoxEnabled", g_app.log_fox_enabled);
    cJSON_AddBoolToObject(root, "logSkyEnabled", g_app.log_sky_enabled);
    cJSON_AddBoolToObject(root, "logGpsEnabled", g_app.log_gps_enabled);
    cJSON_AddBoolToObject(root, "logSavedFoxEnabled", g_app.log_saved_fox_enabled);
    cJSON_AddBoolToObject(root, "gpsDiagnosticsEnabled", g_app.gps_diagnostics_enabled);
    cJSON_AddBoolToObject(root, "webDiagnosticsEnabled", g_app.web_diagnostics_enabled);
    cJSON_AddNumberToObject(root, "serialLogVerbosity", g_app.serial_log_verbosity);
    cJSON_AddStringToObject(root, "status", logging_status_label());
    cJSON_AddStringToObject(root, "microsdStatus", storage_ext_status_str(storage_ext_get_status()));
    cJSON_AddNumberToObject(root, "microsdTotalKb", storage_ext_total_kb());
    cJSON_AddNumberToObject(root, "microsdUsedKb", storage_ext_used_kb());
    cJSON_AddNumberToObject(root, "microsdFreeKb", storage_ext_free_kb());

    cJSON *all_arr = cJSON_AddArrayToObject(root, "all");
    cJSON *events_arr = cJSON_AddArrayToObject(root, "events");
    cJSON *identity_arr = cJSON_AddArrayToObject(root, "identity");
    cJSON *diag_arr = cJSON_AddArrayToObject(root, "diagnostics");
    for (int i = 0; i < recent_count; i++) {
        const char *source = log_bucket_name(recent[i].bucket);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "ts", recent[i].ts);
        cJSON_AddStringToObject(item, "source", source);
        cJSON_AddStringToObject(item, "kind", recent[i].kind);
        cJSON_AddStringToObject(item, "message", recent[i].message);
        cJSON_AddItemToArray(all_arr, item);
        switch (recent[i].bucket) {
        case STORAGE_LOG_BUCKET_IDENTITY:
            cJSON_AddItemToArray(identity_arr, cJSON_Duplicate(item, 1));
            break;
        case STORAGE_LOG_BUCKET_DIAGNOSTICS:
            cJSON_AddItemToArray(diag_arr, cJSON_Duplicate(item, 1));
            break;
        case STORAGE_LOG_BUCKET_EVENTS:
        default:
            cJSON_AddItemToArray(events_arr, cJSON_Duplicate(item, 1));
            break;
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(recent);
    return json;
}

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

static bool parse_nonnegative_query_int(const char *query, const char *key, int *out_value)
{
    if (!query || !key || !out_value) return false;

    char value[16] = {0};
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 30) {
        return false;
    }

    *out_value = (int)parsed;
    return true;
}

static bool map_root_available(void)
{
    struct stat st = {0};
    if (storage_ext_get_status() != STORAGE_STATUS_AVAILABLE) return false;
    return stat(MAP_ROOT_PATH, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *map_tile_content_type(const char *ext)
{
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, "png") == 0) return "image/png";
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, "webp") == 0) return "image/webp";
    return "application/octet-stream";
}

static esp_err_t get_map_status(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"available\":%s}", map_root_available() ? "true" : "false");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t get_map_tile(httpd_req_t *req)
{
    char query[96] = {0};
    int z = 0, x = 0, y = 0;

    if (httpd_req_get_url_query_len(req) <= 0
            || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK
            || !parse_nonnegative_query_int(query, "z", &z)
            || !parse_nonnegative_query_int(query, "x", &x)
            || !parse_nonnegative_query_int(query, "y", &y)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad tile query");
    }

    if (!map_root_available()) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Map folder not available");
    }

    const char *exts[] = {"png", "jpg", "jpeg", "webp"};
    char path[128];
    const char *matched_ext = NULL;
    struct stat st = {0};
    for (size_t i = 0; i < (sizeof(exts) / sizeof(exts[0])); i++) {
        snprintf(path, sizeof(path), "%s/%d/%d/%d.%s", MAP_ROOT_PATH, z, x, y, exts[i]);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            matched_ext = exts[i];
            break;
        }
    }

    if (!matched_ext) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Tile not found");
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open tile");
    }

    set_security_headers(req);
    httpd_resp_set_type(req, map_tile_content_type(matched_ext));
    char buf[1024];
    size_t read_len = 0;
    esp_err_t result = ESP_OK;
    while ((read_len = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_len) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    fclose(f);

    if (result == ESP_OK) {
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    return ESP_FAIL;
}

static map_pin_kind_t map_pin_kind_from_json(const cJSON *kind_item)
{
    if (!kind_item) return MAP_PIN_KIND_FLOCK;

    if (cJSON_IsNumber(kind_item)) {
        int kind = kind_item->valueint;
        if (kind == (int)MAP_PIN_KIND_FOX) return MAP_PIN_KIND_FOX;
        if (kind == (int)MAP_PIN_KIND_DRONE) return MAP_PIN_KIND_DRONE;
        if (kind == (int)MAP_PIN_KIND_SELF) return MAP_PIN_KIND_SELF;
        return MAP_PIN_KIND_FLOCK;
    }

    if (cJSON_IsString(kind_item) && kind_item->valuestring) {
        if (strcmp(kind_item->valuestring, "fox") == 0) return MAP_PIN_KIND_FOX;
        if (strcmp(kind_item->valuestring, "drone") == 0) return MAP_PIN_KIND_DRONE;
        if (strcmp(kind_item->valuestring, "self") == 0) return MAP_PIN_KIND_SELF;
    }

    return MAP_PIN_KIND_FLOCK;
}

static esp_err_t post_map_pins(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 8192) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    }

    char *buf = calloc(1, (size_t)req->content_len + 1U);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    int len = httpd_req_recv(req, buf, req->content_len);
    if (len <= 0) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
    }

    cJSON *pins = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "pins");
    if (!pins || !cJSON_IsArray(pins)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing pins array");
    }

    map_pin_t next_pins[MAX_SHARED_MAP_PINS];
    memset(next_pins, 0, sizeof(next_pins));
    int next_count = 0;

    cJSON *pin_item = NULL;
    cJSON_ArrayForEach(pin_item, pins) {
        if (!cJSON_IsObject(pin_item) || next_count >= MAX_SHARED_MAP_PINS) continue;

        cJSON *lat_item = cJSON_GetObjectItem(pin_item, "lat");
        cJSON *lon_item = cJSON_GetObjectItem(pin_item, "lon");
        if (!cJSON_IsNumber(lat_item) || !cJSON_IsNumber(lon_item)) continue;

        double lat = lat_item->valuedouble;
        double lon = lon_item->valuedouble;
        if (!isfinite(lat) || !isfinite(lon)) continue;

        map_pin_t *pin = &next_pins[next_count++];
        memset(pin, 0, sizeof(*pin));
        pin->lat = lat;
        pin->lon = lon;
        pin->kind = map_pin_kind_from_json(cJSON_GetObjectItem(pin_item, "kind"));
        pin->rssi = -85;

        cJSON *mac_item = cJSON_GetObjectItem(pin_item, "mac");
        if (cJSON_IsString(mac_item) && mac_item->valuestring) {
            mac_from_str(mac_item->valuestring, pin->mac);
        }

        cJSON *label_item = cJSON_GetObjectItem(pin_item, "label");
        if (cJSON_IsString(label_item) && label_item->valuestring) {
            strncpy(pin->label, label_item->valuestring, sizeof(pin->label) - 1);
        }

        cJSON *radius_item = cJSON_GetObjectItem(pin_item, "radiusM");
        if (cJSON_IsNumber(radius_item) && isfinite(radius_item->valuedouble)) {
            pin->radius_m = (float)radius_item->valuedouble;
        }

        cJSON *rssi_item = cJSON_GetObjectItem(pin_item, "rssi");
        if (cJSON_IsNumber(rssi_item)) {
            pin->rssi = (int8_t)rssi_item->valueint;
        }
    }

    memset(g_app.shared_map_pins, 0, sizeof(g_app.shared_map_pins));
    if (next_count > 0) {
        memcpy(g_app.shared_map_pins, next_pins, (size_t)next_count * sizeof(next_pins[0]));
    }
    g_app.shared_map_pin_count = (uint8_t)next_count;
    g_app.ui_refresh_token++;

    cJSON_Delete(root);

    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"count\":%d}", next_count);
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    return httpd_resp_sendstr(req, resp);
}

/* ── JSON helper: escape a C string for safe JSON embedding ── */
static void json_escape_str(const char *src, char *dst, size_t dstsz)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dstsz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            if (j < dstsz) dst[j++] = (char)c;
        } else if (c < 0x20) {
            /* skip control chars */
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

/* ── JSON builders ── */
/*
 * Build the full state JSON with a single snprintf + one malloc.
 * Replaces the previous cJSON implementation which performed ~60 small
 * allocations per call, fragmenting the heap every 500 ms.
 */
#define STATE_JSON_BUF 2496
static char *build_state_json(void)
{
    uint32_t now_ms = uptime_ms();
    bool gps_ready_fresh = g_app.gps_client_ready &&
                           (now_ms > g_app.gps_client_ready_ms) &&
                           ((now_ms - g_app.gps_client_ready_ms) <= GPS_READY_TIMEOUT_MS);
    bool gps_active = g_app.gps_tagging_enabled && gps_ready_fresh && (g_app.wifi_clients > 0);
    storage_status_t microsd_status = storage_ext_get_status();
    uint32_t microsd_total_kb = storage_ext_total_kb();
    uint32_t microsd_used_kb = storage_ext_used_kb();
    uint32_t microsd_free_kb = storage_ext_free_kb();

    char fox_mac[18] = "none";
    if (g_app.fox_target_set) mac_to_str(g_app.fox_target_mac, fox_mac, sizeof(fox_mac));

    uint32_t last_seen_sec = (g_app.fox_last_seen > 0 && now_ms > g_app.fox_last_seen)
                             ? (now_ms - g_app.fox_last_seen) / 1000U : 0;
    const char *prox = "very far";
    if (g_app.fox_rssi >= -45) prox = "very close";
    else if (g_app.fox_rssi >= -60) prox = "close";
    else if (g_app.fox_rssi >= -72) prox = "near";
    else if (g_app.fox_rssi >= -84) prox = "far";

    char *buf = (char *)malloc(STATE_JSON_BUF);
    if (!buf) return NULL;

    int n = snprintf(buf, STATE_JSON_BUF,
        "{\"mode\":%d,\"uptime\":%lu,\"heap\":%lu,\"clients\":%u,"
        "\"brightness\":%u,\"sound\":%s,\"led\":%s,"
        "\"apBroadcast\":%s,\"singleApName\":%s,"
        "\"displaySleepSec\":%u,\"menuLedColor\":%u,"
        "\"soundProfileFlock\":%u,\"soundProfileFox\":%u,\"soundProfileSky\":%u,"
        "\"shortcutModeBtn\":%u,\"shortcutActionBtn\":%u,\"shortcutBackBtn\":%u,"
        "\"useMicrosdLogs\":%s,\"advancedLoggingEnabled\":%s,\"gpsDiagnosticsEnabled\":%s,\"webDiagnosticsEnabled\":%s,\"serialLogVerbosity\":%u,\"loggingActive\":%s,\"loggingBlocked\":%s,"
        "\"logGeneralEnabled\":%s,\"logFlockEnabled\":%s,\"logFoxEnabled\":%s,\"logSkyEnabled\":%s,\"logGpsEnabled\":%s,\"logSavedFoxEnabled\":%s,"
        "\"loggingStatus\":\"%s\",\"gpsTagging\":%s,"
        "\"gpsClientReady\":%s,\"gpsTagActive\":%s,"
        "\"microsdAvailable\":%s,\"microsdStatus\":\"%s\","
        "\"microsdTotalKb\":%lu,\"microsdUsedKb\":%lu,\"microsdFreeKb\":%lu,"
        "\"logCapacityKb\":%lu,\"deviceCount\":%d,\"droneCount\":%d,"
        "\"foxRegistryCapacity\":%d,"
        "\"fox\":{\"target\":\"%s\",\"hasTarget\":%s,\"rssi\":%d,"
        "\"bestRssi\":%d,\"found\":%s,\"ledMode\":%u,"
        "\"registryCount\":%d,\"lastSeenSec\":%lu,"
        "\"estLat\":%.8f,\"estLon\":%.8f,\"estRadiusM\":%.1f,"
        "\"gpsSamples\":%u,\"proximity\":\"%s\"},"
        "\"sky\":{\"trackedIdx\":%d,\"estLat\":%.8f,\"estLon\":%.8f,"
        "\"estRadiusM\":%.1f,\"gpsSamples\":%u},"
        "\"wifiClientMacs\":[",
        (int)g_app.current_mode,
        (unsigned long)g_app.uptime_sec,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned)g_app.wifi_clients,
        (unsigned)g_app.lcd_brightness,
        g_app.sound_enabled ? "true" : "false",
        g_app.led_enabled ? "true" : "false",
        g_app.ap_broadcast_enabled ? "true" : "false",
        g_app.single_ap_name_enabled ? "true" : "false",
        (unsigned)g_app.display_sleep_timeout_sec,
        (unsigned)g_app.menu_led_color,
        (unsigned)g_app.sound_profile_flock,
        (unsigned)g_app.sound_profile_fox,
        (unsigned)g_app.sound_profile_sky,
        (unsigned)g_app.shortcut_mode_btn,
        (unsigned)g_app.shortcut_action_btn,
        (unsigned)g_app.shortcut_back_btn,
        g_app.use_microsd_logs ? "true" : "false",
        g_app.advanced_logging_enabled ? "true" : "false",
        g_app.gps_diagnostics_enabled ? "true" : "false",
        g_app.web_diagnostics_enabled ? "true" : "false",
        (unsigned)g_app.serial_log_verbosity,
        storage_ext_logging_active() ? "true" : "false",
        storage_ext_logging_blocked() ? "true" : "false",
        g_app.log_general_enabled ? "true" : "false",
        g_app.log_flock_enabled ? "true" : "false",
        g_app.log_fox_enabled ? "true" : "false",
        g_app.log_sky_enabled ? "true" : "false",
        g_app.log_gps_enabled ? "true" : "false",
        g_app.log_saved_fox_enabled ? "true" : "false",
        logging_status_label(),
        g_app.gps_tagging_enabled ? "true" : "false",
        gps_ready_fresh ? "true" : "false",
        gps_active ? "true" : "false",
        (microsd_status == STORAGE_STATUS_AVAILABLE) ? "true" : "false",
        storage_ext_status_str(microsd_status),
        (unsigned long)microsd_total_kb,
        (unsigned long)microsd_used_kb,
        (unsigned long)microsd_free_kb,
        (unsigned long)storage_ext_log_capacity_kb(),
        g_app.device_count,
        g_app.drone_count,
        fox_hunter_registry_capacity(),
        fox_mac,
        g_app.fox_target_set ? "true" : "false",
        (int)g_app.fox_rssi,
        (int)g_app.fox_rssi_best,
        g_app.fox_target_found ? "true" : "false",
        (unsigned)g_app.fox_led_mode,
        g_app.fox_registry_count,
        (unsigned long)last_seen_sec,
        g_app.fox_target_lat, g_app.fox_target_lon,
        (double)g_app.fox_target_radius_m,
        (unsigned)g_app.fox_target_gps_samples,
        prox,
        g_app.sky_tracked_drone_idx,
        g_app.sky_tracked_lat, g_app.sky_tracked_lon,
        (double)g_app.sky_tracked_radius_m,
        (unsigned)g_app.sky_tracked_gps_samples
    );

    /* Append wifi client MACs */
    uint8_t tracked = (g_app.wifi_clients < WIFI_MAX_AP_CLIENTS)
                      ? g_app.wifi_clients : WIFI_MAX_AP_CLIENTS;
    for (uint8_t i = 0; i < tracked && n < STATE_JSON_BUF - 24; i++) {
        char mc[18];
        mac_to_str(g_app.wifi_client_macs[i], mc, sizeof(mc));
        n += snprintf(buf + n, STATE_JSON_BUF - n, "%s\"%s\"", (i > 0) ? "," : "", mc);
    }
    if (n < STATE_JSON_BUF - 3) {
        buf[n++] = ']'; buf[n++] = '}'; buf[n] = '\0';
    }
    return buf;
}

/* build_devices_json / build_drones_json removed — streamed directly in handlers */

/* ── API handlers ── */
static esp_err_t get_state(httpd_req_t *req)
{
    char *json = build_state_json();
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

/* Stream device list directly — no intermediate malloc/cJSON tree */
static esp_err_t get_devices(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    httpd_resp_sendstr_chunk(req, "[");

    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        char buf[256];
        for (int i = 0; i < g_app.device_count; i++) {
            ble_device_t *d = &g_app.devices[i];
            char ms[18];
            mac_to_str(d->mac, ms, sizeof(ms));
            char sn[DEVICE_NAME_LEN * 2];
            json_escape_str(d->name, sn, sizeof(sn));
            snprintf(buf, sizeof(buf),
                "%s{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"bestRssi\":%d,"
                "\"hits\":%u,\"flags\":%u,\"flock\":%s,\"raven\":%s,"
                "\"ravenFw\":%u,\"firstSeen\":%lu,\"lastSeen\":%lu}",
                i ? "," : "",
                ms, sn, (int)d->rssi, (int)d->rssi_best,
                (unsigned)d->hit_count, (unsigned)d->detect_flags,
                d->is_flock ? "true" : "false",
                d->is_raven ? "true" : "false",
                (unsigned)d->raven_fw,
                (unsigned long)(d->first_seen / 1000),
                (unsigned long)(d->last_seen / 1000));
            httpd_resp_sendstr_chunk(req, buf);
        }
        xSemaphoreGive(g_app.device_mutex);
    }

    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Stream drone list directly */
static esp_err_t get_drones(httpd_req_t *req)
{
    static const char *proto_names[] = {"wifi", "ble", "dji"};
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    httpd_resp_sendstr_chunk(req, "[");

    if (xSemaphoreTake(g_app.drone_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        char buf[384];
        for (int i = 0; i < g_app.drone_count; i++) {
            drone_info_t *d = &g_app.drones[i];
            char ms[18];
            mac_to_str(d->mac, ms, sizeof(ms));
            char sid[DRONE_ID_LEN * 2], sop[DRONE_ID_LEN * 2];
            json_escape_str(d->basic_id, sid, sizeof(sid));
            json_escape_str(d->operator_id, sop, sizeof(sop));
            snprintf(buf, sizeof(buf),
                "%s{\"mac\":\"%s\",\"rssi\":%d,\"id\":\"%s\",\"idType\":%u,"
                "\"uaType\":%u,\"lat\":%.8f,\"lon\":%.8f,\"alt\":%.1f,"
                "\"height\":%.1f,\"speed\":%.1f,\"dir\":%.1f,"
                "\"pilotLat\":%.8f,\"pilotLon\":%.8f,\"opId\":\"%s\","
                "\"proto\":\"%s\",\"hasLoc\":%s,\"hasPilot\":%s}",
                i ? "," : "",
                ms, (int)d->rssi, sid,
                (unsigned)d->id_type, (unsigned)d->ua_type,
                d->lat, d->lon, (double)d->altitude,
                (double)d->height, (double)d->speed, (double)d->direction,
                d->pilot_lat, d->pilot_lon, sop,
                proto_names[d->protocol % 3],
                d->has_location ? "true" : "false",
                d->has_pilot ? "true" : "false");
            httpd_resp_sendstr_chunk(req, buf);
        }
        xSemaphoreGive(g_app.drone_mutex);
    }

    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t get_logs(httpd_req_t *req)
{
    char *json = build_logs_json();
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    esp_err_t ret = httpd_resp_send(req, json, (ssize_t)strlen(json));
    free(json);
    return ret;
}

static esp_err_t post_logs_manage(httpd_req_t *req)
{
    char buf[96];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (action && strcmp(action, "clear") == 0) {
        err = storage_ext_clear_logs();
    } else if (action && strcmp(action, "delete") == 0) {
        err = storage_ext_delete_logs();
    } else if (action && strcmp(action, "scramble") == 0) {
        err = storage_ext_scramble_logs();
    }

    cJSON_Delete(root);

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "log action failed");
    }

    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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

    cJSON *clear_item = cJSON_GetObjectItem(root, "clear");
    if (clear_item && cJSON_IsTrue(clear_item)) {
        fox_hunter_clear_target();
        cJSON_Delete(root);
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (mac_item && cJSON_IsString(mac_item)) {
        uint8_t mac[6];
        if (mac_from_str(mac_item->valuestring, mac) == 0) {
            fox_hunter_set_target(mac);
        } else if (mac_item->valuestring[0] == '\0') {
            fox_hunter_clear_target();
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
typedef struct {
    bool        online;
    uint32_t    age_sec;
    int8_t      rssi;
    uint16_t    hits;
    const char *source;
} fox_registry_live_info_t;

static void fox_registry_live_info_for_mac(const uint8_t mac[6], fox_registry_live_info_t *info)
{
    if (!info) return;

    uint32_t now_ms = uptime_ms();
    info->online = false;
    info->age_sec = 0;
    info->rssi = -127;
    info->hits = 0;
    info->source = "";

    if (g_app.fox_target_set && mac_equal(mac, g_app.fox_target_mac) && g_app.fox_last_seen > 0) {
        uint32_t age_ms = (now_ms > g_app.fox_last_seen) ? (now_ms - g_app.fox_last_seen) : 0;
        info->online = g_app.fox_target_found;
        info->age_sec = age_ms / 1000U;
        info->rssi = g_app.fox_rssi;
        info->source = "fox";
    }

    for (uint8_t i = 0; i < g_app.wifi_clients && i < WIFI_MAX_AP_CLIENTS; i++) {
        if (mac_equal(mac, g_app.wifi_client_macs[i])) {
            info->online = true;
            info->age_sec = 0;
            info->hits = 0;
            info->source = "wifi";
            return;
        }
    }

    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < g_app.device_count; i++) {
            ble_device_t *d = &g_app.devices[i];
            if (!mac_equal(mac, d->mac)) continue;
            uint32_t age_ms = (now_ms > d->last_seen) ? (now_ms - d->last_seen) : 0;
            if (!info->source[0] || (age_ms / 1000U) <= info->age_sec) {
                info->online = (age_ms <= 8000U);
                info->age_sec = age_ms / 1000U;
                info->rssi = d->rssi;
                info->hits = d->hit_count;
                info->source = d->is_flock ? "flock" : "ble";
            }
        }

        for (int i = 0; i < g_app.fox_nearby_count; i++) {
            ble_device_t *d = &g_app.fox_nearby[i];
            if (!mac_equal(mac, d->mac)) continue;
            uint32_t age_ms = (now_ms > d->last_seen) ? (now_ms - d->last_seen) : 0;
            if (!info->source[0] || (age_ms / 1000U) <= info->age_sec) {
                info->online = (age_ms <= 8000U);
                info->age_sec = age_ms / 1000U;
                info->rssi = d->rssi;
                info->hits = d->hit_count;
                info->source = "ble";
            }
        }
        xSemaphoreGive(g_app.device_mutex);
    }

    if (xSemaphoreTake(g_app.drone_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < g_app.drone_count; i++) {
            drone_info_t *d = &g_app.drones[i];
            if (!mac_equal(mac, d->mac)) continue;
            uint32_t age_ms = (now_ms > d->last_seen) ? (now_ms - d->last_seen) : 0;
            if (!info->source[0] || (age_ms / 1000U) <= info->age_sec) {
                info->online = (age_ms <= 8000U);
                info->age_sec = age_ms / 1000U;
                info->rssi = d->rssi;
                info->hits = 0;
                info->source = "drone";
            }
        }
        xSemaphoreGive(g_app.drone_mutex);
    }
}

static esp_err_t get_fox_registry(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    httpd_resp_sendstr_chunk(req, "[");

    char buf[720];
    for (int i = 0; i < g_app.fox_registry_count; i++) {
        fox_reg_entry_t *e = &g_app.fox_registry[i];
        char ms[18];
        mac_to_str(e->mac, ms, sizeof(ms));
        char sl[FOX_REG_LABEL_LEN * 2], so[DEVICE_NAME_LEN * 2];
        char snk[FOX_REG_NICK_LEN * 2], snt[FOX_REG_NOTES_LEN * 2];
        char ss[FOX_REG_SECTION_LEN * 2];
        json_escape_str(e->label, sl, sizeof(sl));
        json_escape_str(e->original_name, so, sizeof(so));
        json_escape_str(e->nickname, snk, sizeof(snk));
        json_escape_str(e->notes, snt, sizeof(snt));
        json_escape_str(e->section, ss, sizeof(ss));

        fox_registry_live_info_t li;
        fox_registry_live_info_for_mac(e->mac, &li);
        int age = li.source[0] ? (int)li.age_sec : -1;
        bool has_pin = (e->pinned_lat != 0.0 || e->pinned_lon != 0.0);

        int n = snprintf(buf, sizeof(buf),
            "%s{\"mac\":\"%s\",\"label\":\"%s\",\"originalName\":\"%s\","
            "\"nickname\":\"%s\",\"notes\":\"%s\",\"section\":\"%s\","
            "\"online\":%s,\"lastSeenAgeSec\":%d,"
            "\"liveRssi\":%d,\"liveHits\":%u,\"lastSeenSource\":\"%s\"",
            i ? "," : "",
            ms, sl, so, snk, snt, ss,
            li.online ? "true" : "false", age,
            (int)li.rssi, (unsigned)li.hits, li.source);
        if (has_pin && n > 0 && n < (int)sizeof(buf) - 1) {
            n += snprintf(buf + n, sizeof(buf) - n,
                ",\"pinnedLat\":%.6f,\"pinnedLon\":%.6f,\"pinnedRadiusM\":%.1f",
                e->pinned_lat, e->pinned_lon, (double)e->pinned_radius_m);
        }
        if (n > 0 && n < (int)sizeof(buf) - 1) {
            buf[n] = '}'; buf[n + 1] = '\0';
        }
        httpd_resp_sendstr_chunk(req, buf);
    }

    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_send_chunk(req, NULL, 0);
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

    /* Update pinned GPS location if provided */
    cJSON *lat_item = cJSON_GetObjectItem(root, "lat");
    cJSON *lon_item = cJSON_GetObjectItem(root, "lon");
    if (lat_item && lon_item && cJSON_IsNumber(lat_item) && cJSON_IsNumber(lon_item)) {
        double lat = lat_item->valuedouble;
        double lon = lon_item->valuedouble;
        float radius_m = 30.0f;
        cJSON *rad_item = cJSON_GetObjectItem(root, "radiusM");
        if (rad_item && cJSON_IsNumber(rad_item)) radius_m = (float)rad_item->valuedouble;
        fox_hunter_registry_set_gps(idx, lat, lon, radius_m);
    }
    /* Allow clearing pinned GPS */
    cJSON *clear_pin = cJSON_GetObjectItem(root, "clearPin");
    if (clear_pin && cJSON_IsTrue(clear_pin)) {
        fox_hunter_registry_set_gps(idx, 0.0, 0.0, 0.0f);
    }

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
        bool is_ready = cJSON_IsTrue(ready);
        bool changed = (g_app.gps_client_ready != is_ready);
        g_app.gps_client_ready = is_ready;
        g_app.gps_client_ready_ms = is_ready ? uptime_ms() : 0;
        if (changed && g_app.gps_diagnostics_enabled) {
            char log_msg[96];
            snprintf(log_msg, sizeof(log_msg), "gps_client_ready=%s clients=%u secure_window_ms=%u",
                     is_ready ? "true" : "false",
                     (unsigned)g_app.wifi_clients,
                     (unsigned)GPS_READY_TIMEOUT_MS);
            storage_ext_append_diagnostic("gps", log_msg);
        }
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

    cJSON *adv_logs = cJSON_GetObjectItem(root, "advancedLoggingEnabled");
    if (adv_logs) g_app.advanced_logging_enabled = cJSON_IsTrue(adv_logs);

    cJSON *log_general = cJSON_GetObjectItem(root, "logGeneralEnabled");
    if (log_general) g_app.log_general_enabled = cJSON_IsTrue(log_general);

    cJSON *log_flock = cJSON_GetObjectItem(root, "logFlockEnabled");
    if (log_flock) g_app.log_flock_enabled = cJSON_IsTrue(log_flock);

    cJSON *log_fox = cJSON_GetObjectItem(root, "logFoxEnabled");
    if (log_fox) g_app.log_fox_enabled = cJSON_IsTrue(log_fox);

    cJSON *log_sky = cJSON_GetObjectItem(root, "logSkyEnabled");
    if (log_sky) g_app.log_sky_enabled = cJSON_IsTrue(log_sky);

    cJSON *log_gps = cJSON_GetObjectItem(root, "logGpsEnabled");
    if (log_gps) g_app.log_gps_enabled = cJSON_IsTrue(log_gps);

    cJSON *log_saved_fox = cJSON_GetObjectItem(root, "logSavedFoxEnabled");
    if (log_saved_fox) g_app.log_saved_fox_enabled = cJSON_IsTrue(log_saved_fox);

    cJSON *gps_diag = cJSON_GetObjectItem(root, "gpsDiagnosticsEnabled");
    if (gps_diag) g_app.gps_diagnostics_enabled = cJSON_IsTrue(gps_diag);

    cJSON *web_diag = cJSON_GetObjectItem(root, "webDiagnosticsEnabled");
    if (web_diag) g_app.web_diagnostics_enabled = cJSON_IsTrue(web_diag);

    cJSON *serial_log = cJSON_GetObjectItem(root, "serialLogVerbosity");
    if (serial_log && cJSON_IsNumber(serial_log)) {
        int val = serial_log->valueint;
        if (val < 0) val = 0;
        if (val >= SERIAL_LOG_COUNT) val = SERIAL_LOG_COUNT - 1;
        g_app.serial_log_verbosity = (uint8_t)val;
    }

    cJSON *gps_tagging = cJSON_GetObjectItem(root, "gpsTagging");
    if (gps_tagging) {
        g_app.gps_tagging_enabled = cJSON_IsTrue(gps_tagging);
        if (!g_app.gps_tagging_enabled) {
            g_app.gps_client_ready = false;
            g_app.gps_client_ready_ms = 0;
        }
    }

    app_apply_runtime_logging_prefs();

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
    if (g_app.advanced_logging_enabled) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                 "settings_updated sd=%s adv=%s src(gen=%s flock=%s fox=%s sky=%s gps=%s foxid=%s) gpsdiag=%s webdiag=%s serial=%u gps=%s",
                 g_app.use_microsd_logs ? "on" : "off",
                 g_app.advanced_logging_enabled ? "on" : "off",
                 g_app.log_general_enabled ? "on" : "off",
                 g_app.log_flock_enabled ? "on" : "off",
                 g_app.log_fox_enabled ? "on" : "off",
                 g_app.log_sky_enabled ? "on" : "off",
                 g_app.log_gps_enabled ? "on" : "off",
                 g_app.log_saved_fox_enabled ? "on" : "off",
                 g_app.gps_diagnostics_enabled ? "on" : "off",
                 g_app.web_diagnostics_enabled ? "on" : "off",
                 (unsigned)g_app.serial_log_verbosity,
                 g_app.gps_tagging_enabled ? "on" : "off");
        storage_ext_append_diagnostic("system", log_msg);
    }
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
        if (g_app.web_diagnostics_enabled) {
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "ws_handshake fd=%d", httpd_req_to_sockfd(req));
            storage_ext_append_diagnostic("web", log_msg);
        }
        return ESP_OK;
    }

    /* Receive frame (handle ping, commands) */
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    uint8_t *payload = NULL;
    if (frame.len > 0) {
        payload = (uint8_t *)malloc(frame.len + 1);
        if (!payload) {
            return ESP_ERR_NO_MEM;
        }
        frame.payload = payload;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret != ESP_OK) {
            free(payload);
            return ret;
        }
        payload[frame.len] = '\0';
    }

    /* Echo state back on any client message */
    char *json = build_state_json();
    if (!json) {
        if (payload) free(payload);
        return ESP_ERR_NO_MEM;
    }
    httpd_ws_frame_t resp = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    ret = httpd_ws_send_frame(req, &resp);
    if (ret != ESP_OK && g_app.web_diagnostics_enabled) {
        char log_msg[96];
        snprintf(log_msg, sizeof(log_msg), "ws_send_failed fd=%d err=%s",
                 httpd_req_to_sockfd(req), esp_err_to_name(ret));
        storage_ext_append_diagnostic("web", log_msg);
    }
    if (payload) free(payload);
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
                esp_err_t send_err = httpd_ws_send_data(server, fds[i], &pkt);
                if (send_err != ESP_OK) {
                    ESP_LOGW(TAG, "WS send failed fd=%d err=%s", fds[i], esp_err_to_name(send_err));
                    if (g_app.web_diagnostics_enabled) {
                        char log_msg[96];
                        snprintf(log_msg, sizeof(log_msg), "ws_broadcast_failed fd=%d err=%s",
                                 fds[i], esp_err_to_name(send_err));
                        storage_ext_append_diagnostic("web", log_msg);
                    }
                }
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

/* ── Push task: sends state updates over WS every 1 s ── */
static void ws_push_task(void *arg)
{
    while (s_http_server || s_https_server) {
        if (esp_get_free_heap_size() > 8192) {
            char *json = build_state_json();
            if (json) {
                web_server_broadcast(json);
                free(json);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

/* ── Fox nearby candidates (streamed) ── */
static esp_err_t get_fox_nearby(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_security_headers(req);
    httpd_resp_sendstr_chunk(req, "[");

    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        char buf[192];
        for (int i = 0; i < g_app.fox_nearby_count; i++) {
            ble_device_t *d = &g_app.fox_nearby[i];
            char ms[18];
            mac_to_str(d->mac, ms, sizeof(ms));
            char sn[DEVICE_NAME_LEN * 2];
            json_escape_str(d->name, sn, sizeof(sn));
            snprintf(buf, sizeof(buf),
                "%s{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"bestRssi\":%d,"
                "\"hits\":%u,\"firstSeen\":%lu,\"lastSeen\":%lu}",
                i ? "," : "",
                ms, sn, (int)d->rssi, (int)d->rssi_best,
                (unsigned)d->hit_count,
                (unsigned long)(d->first_seen / 1000),
                (unsigned long)(d->last_seen / 1000));
            httpd_resp_sendstr_chunk(req, buf);
        }
        xSemaphoreGive(g_app.device_mutex);
    }

    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_send_chunk(req, NULL, 0);
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
    int rssi = cJSON_GetObjectItem(root, "rssi") ? cJSON_GetObjectItem(root, "rssi")->valueint : -85;
    uint8_t mac[6] = {0};

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (mac_item && cJSON_IsString(mac_item)) {
        mac_from_str(mac_item->valuestring, mac);
    }

    if (type && lat != 0 && lon != 0) {
        char log_msg[128];
        if (strcmp(type, "fox") == 0) {
            update_fox_fused_pin(lat, lon, rssi);
            snprintf(log_msg, sizeof(log_msg), "gps_fused mac=%02X:%02X:%02X:%02X:%02X:%02X lat=%.6f lon=%.6f r=%dm n=%u rssi=%d",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     g_app.fox_target_lat, g_app.fox_target_lon,
                     (int)g_app.fox_target_radius_m, (unsigned)g_app.fox_target_gps_samples, rssi);
            storage_ext_append_log("fox", log_msg);
        } else if (strcmp(type, "sky") == 0) {
            update_sky_fused_pin(lat, lon, rssi);
            snprintf(log_msg, sizeof(log_msg), "gps_fused mac=%02X:%02X:%02X:%02X:%02X:%02X lat=%.6f lon=%.6f r=%dm n=%u rssi=%d",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     g_app.sky_tracked_lat, g_app.sky_tracked_lon,
                     (int)g_app.sky_tracked_radius_m, (unsigned)g_app.sky_tracked_gps_samples, rssi);
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
    int rssi = cJSON_GetObjectItem(root, "rssi") ? cJSON_GetObjectItem(root, "rssi")->valueint : -85;
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
        update_sky_fused_pin(lat, lon, rssi);
        snprintf(log_msg, sizeof(log_msg), "gps_fused mac=%02X:%02X:%02X:%02X:%02X:%02X lat=%.6f lon=%.6f r=%dm n=%u rssi=%d",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 g_app.sky_tracked_lat, g_app.sky_tracked_lon,
                 (int)g_app.sky_tracked_radius_m, (unsigned)g_app.sky_tracked_gps_samples, rssi);
        storage_ext_append_log("sky", log_msg);
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── Register routes and start server ── */
static void register_route(httpd_handle_t srv, const httpd_uri_t *uri)
{
    if (!srv || !uri) return;

    esp_err_t err = httpd_register_uri_handler(srv, uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register route %s [%d]: %s",
                 uri->uri ? uri->uri : "<null>", (int)uri->method, esp_err_to_name(err));
    }
}

static void web_register_routes(httpd_handle_t srv)
{
    /* Static page */
    httpd_uri_t uri_index = { .uri="/", .method=HTTP_GET, .handler=get_index };
    register_route(srv, &uri_index);

    /* API endpoints */
    httpd_uri_t uri_state = { .uri="/api/state", .method=HTTP_GET, .handler=get_state };
    register_route(srv, &uri_state);

    httpd_uri_t uri_devices = { .uri="/api/devices", .method=HTTP_GET, .handler=get_devices };
    register_route(srv, &uri_devices);

    httpd_uri_t uri_drones = { .uri="/api/drones", .method=HTTP_GET, .handler=get_drones };
    register_route(srv, &uri_drones);

    httpd_uri_t uri_logs = { .uri="/api/logs", .method=HTTP_GET, .handler=get_logs };
    register_route(srv, &uri_logs);

    httpd_uri_t uri_logs_manage = { .uri="/api/logs/manage", .method=HTTP_POST, .handler=post_logs_manage };
    register_route(srv, &uri_logs_manage);

    httpd_uri_t uri_map_status = { .uri="/api/map/status", .method=HTTP_GET, .handler=get_map_status };
    register_route(srv, &uri_map_status);

    httpd_uri_t uri_map_tile = { .uri="/api/map/tile", .method=HTTP_GET, .handler=get_map_tile };
    register_route(srv, &uri_map_tile);

    httpd_uri_t uri_map_pins = { .uri="/api/map/pins", .method=HTTP_POST, .handler=post_map_pins };
    register_route(srv, &uri_map_pins);

    httpd_uri_t uri_mode = { .uri="/api/mode", .method=HTTP_POST, .handler=post_mode };
    register_route(srv, &uri_mode);

    httpd_uri_t uri_gps_status = { .uri="/api/gps/status", .method=HTTP_POST, .handler=post_gps_status };
    register_route(srv, &uri_gps_status);

    httpd_uri_t uri_target_gps = { .uri="/api/target/gps", .method=HTTP_POST, .handler=post_target_gps };
    register_route(srv, &uri_target_gps);

    httpd_uri_t uri_sky_track = { .uri="/api/sky/track", .method=HTTP_POST, .handler=post_sky_track };
    register_route(srv, &uri_sky_track);

    httpd_uri_t uri_fox = { .uri="/api/fox/target", .method=HTTP_POST, .handler=post_fox_target };
    register_route(srv, &uri_fox);

    httpd_uri_t uri_fox_led = { .uri="/api/fox/ledmode", .method=HTTP_POST, .handler=post_fox_ledmode };
    register_route(srv, &uri_fox_led);

    httpd_uri_t uri_fox_reg_get = { .uri="/api/fox/registry", .method=HTTP_GET, .handler=get_fox_registry };
    register_route(srv, &uri_fox_reg_get);

    httpd_uri_t uri_fox_reg_post = { .uri="/api/fox/registry", .method=HTTP_POST, .handler=post_fox_registry };
    register_route(srv, &uri_fox_reg_post);

    httpd_uri_t uri_fox_reg_del = { .uri="/api/fox/registry", .method=HTTP_DELETE, .handler=delete_fox_registry };
    register_route(srv, &uri_fox_reg_del);

    httpd_uri_t uri_fox_reg_update = { .uri="/api/fox/registry/update", .method=HTTP_POST, .handler=post_fox_registry_update };
    register_route(srv, &uri_fox_reg_update);

    httpd_uri_t uri_settings = { .uri="/api/settings", .method=HTTP_POST, .handler=post_settings };
    register_route(srv, &uri_settings);

    httpd_uri_t uri_csv = { .uri="/api/export/csv", .method=HTTP_GET, .handler=get_export_csv };
    register_route(srv, &uri_csv);

    httpd_uri_t uri_fox_nearby = { .uri="/api/fox/nearby", .method=HTTP_GET, .handler=get_fox_nearby };
    register_route(srv, &uri_fox_nearby);

    /* WebSocket */
    httpd_uri_t uri_ws = {
        .uri="/ws", .method=HTTP_GET, .handler=ws_handler,
        .is_websocket=true, .handle_ws_control_frames=true,
    };
    register_route(srv, &uri_ws);
}

void web_server_start(void)
{
    bool started = false;

    httpd_ssl_config_t ssl_config = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_config.httpd.max_uri_handlers = WEB_MAX_URI_HANDLERS;
    ssl_config.httpd.stack_size = 6144;
    ssl_config.httpd.max_open_sockets = 2;
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
    config.max_uri_handlers = WEB_MAX_URI_HANDLERS;
    config.stack_size       = 6144;
    config.max_open_sockets = 3;
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
